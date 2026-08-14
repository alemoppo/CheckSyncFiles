#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Filesystem/FileEntry.h"

namespace bv {

// Two-sided match table used to pair up entries discovered by two concurrent
// filesystem workers (same relative path on both roots).
//
// Both workers insert every entry they find under the shard selected by the
// canonical key (folded path or raw path, matching FileIndex::key). The shard
// operation is atomic with respect to the other worker: an insert either stores
// the entry, or finds the peer inserted by the other worker and removes it so
// the caller can classify the pair immediately. No I/O, hashing or callbacks
// are ever performed under the shard lock.
//
// Memory bound: a fast source enumerator (e.g. MFT) must not run away from a
// slow one (e.g. a USB device). `pending_` counts the entries CURRENTLY STORED
// in the table (unmatched pairs): it grows by one on every stored insert and
// shrinks by one on every cross-side match-and-remove, so it is always exactly
// the number of entries the shards hold. When the source worker (side 0) is at
// or above the high-water mark it parks on cv_ until pending drops below it,
// the destination finishes, or cancellation is requested. The destination
// worker is never parked, so it always keeps inserting and matching (shrinking
// pending) and, when it finishes, releases the source via setSideDone; the
// source can therefore never stay blocked forever. Cancellation also reaches a
// parked source through the destination: the destination observes the flag on
// its next entry and calls setSideDone, whose notify_all wakes the waiter, and
// the waiter's predicate sees cancel_. See insert() for the deadlock rationale
// and the exact notify discipline (never notify while holding a shard lock;
// always notify under cvMutex_ so a wakeup can never be lost).
class MatchTable {
public:
    enum class Outcome : uint8_t {
        Inserted, // no peer yet: entry stored (pending count grew)
        Matched,  // peer from the other side found and removed; `peer` filled
        Replaced, // same-side entry already present: last-wins replace
    };

    // `shardBits` selects the number of shards (1 << shardBits); `highWater`
    // overrides the backpressure threshold for tests (default ~1M entries).
    explicit MatchTable(unsigned int shardBits = 6, uint64_t highWater = 1ull << 20)
        : shardCount_(size_t{1} << shardBits),
          mask_((uint64_t{1} << shardBits) - 1),
          shards_(new Shard[shardCount_]),
          highWater_(highWater) {}

    // `side` is 0 (source) or 1 (destination). On a cross-side match the peer is
    // removed and returned (by value) through `peer`; the just-inserted entry
    // is then also consumed by the caller, so a pair never stays in the table.
    //
    // Backpressure is applied to the SOURCE side only (side 0). Gating both
    // sides would dead-lock: pending can only decrease through a match, and a
    // match needs an insert, so once both workers are parked above the high-water
    // mark nobody can ever shrink the table again (reachable with two large,
    // mostly-disjoint trees, or an MFT side flooding far ahead of a slow device).
    // The destination worker is never parked, so it always keeps inserting and
    // matching (shrinking pending) and, when it finishes, releases the source via
    // setSideDone; the source can therefore never stay blocked forever. The
    // trade-off is that a destination that runs far ahead of the source is not
    // flow-controlled (no deadlock is worth more than bounded memory here).
    //
    // The common path (pending below the high-water mark) does not take a global
    // lock: the flag is polled atomically first, and the single cvMutex_/cv_ is
    // only touched when throttling actually engages. So no global lock is held
    // per entry, matching the "no global serialization point" requirement.
    //
    // No worker ever waits while holding a shard lock: the throttle park happens
    // BEFORE the shard lock is acquired, and a woken worker only takes the shard
    // after the park returns. Conversely, a cross-side match releases the shard
    // lock BEFORE waking a waiter, so a woken worker can immediately acquire the
    // shard it needs without contending with the notifier.
    //
    // Overshoot: pending_ is a throttle for the source, not a hard cap on the
    // table. A single source worker inserts at most one entry per insert() call
    // and only when it observed pending_ < highWater_, so its own contribution
    // can push pending_ to at most highWater_ (the check-to-insert window is
    // not under a lock; the destination may add entries in that window, which is
    // the documented one-sided trade-off -- it never deadlocks because the
    // destination is never parked). With a single source worker there is no
    // multi-producer reservation overshoot.
    Outcome insert(const std::wstring& key, int side, FileEntry&& e, FileEntry& peer) {
        if (side == 0 && pending_.load(std::memory_order_relaxed) >= highWater_) {
            // Park WITHOUT holding any shard lock. The predicate re-checks the
            // throttle under cvMutex_, so a notify that lands just before we
            // block cannot be lost: either the predicate re-evaluates to true
            // and we proceed, or the notify wakes us directly.
            std::unique_lock<std::mutex> lk(cvMutex_);
            throttleWaiters_.fetch_add(1, std::memory_order_relaxed);
            cv_.wait(lk, [&] {
                return pending_.load(std::memory_order_relaxed) < highWater_ ||
                       done_[1].load(std::memory_order_acquire) ||
                       (cancel_ && cancel_->load(std::memory_order_relaxed));
            });
            throttleWaiters_.fetch_sub(1, std::memory_order_relaxed);
        }

        Shard& s = shards_[Hash(key) & mask_];
        std::unique_lock<std::mutex> slk(s.mutex);
        auto it = s.map.find(key);
        if (it == s.map.end()) {
            s.map.emplace(key, Slot{side, std::move(e)});
            pending_.fetch_add(1, std::memory_order_relaxed);
            return Outcome::Inserted;
        }
        if (it->second.side == side) {
            // Two entries folded to the same key on the same device (case-
            // tolerant enumerator edge case): keep FileIndex's last-wins rule.
            it->second.entry = std::move(e);
            return Outcome::Replaced;
        }
        // Cross-side match: consume the stored peer. The just-inserted entry is
        // never stored, so pending_ goes from old_count to old_count - 1 (it is
        // never temporarily old_count + 1).
        peer = std::move(it->second.entry);
        s.map.erase(it);
        pending_.fetch_sub(1, std::memory_order_relaxed);
        // Release the shard lock BEFORE waking a waiter: the waiter must be able
        // to take the shard it needs immediately, and the notify must never
        // happen under a shard lock.
        slk.unlock();
        {
            // Notify under cvMutex_: the waiter holds cvMutex_ across the window
            // between its predicate evaluation and entering the kernel wait, so
            // this notification can never be lost (see insert() park above).
            std::lock_guard<std::mutex> ck(cvMutex_);
            cv_.notify_one();
        }
        return Outcome::Matched;
    }

    // Marks `side` as fully enumerated so throttle waiters on the other side
    // are released (no future peer can arrive from a finished side). Also the
    // path through which cancellation reaches a parked source: the destination
    // observes the cancel flag on its next entry, stops, and calls this, waking
    // the waiter so its predicate can see cancel_. Only the per-side completion
    // state is touched; the cancellation pointer is configured once via
    // setCancel() before any worker starts, so it is never written concurrently
    // with worker reads. The notify is taken under cvMutex_ (never lost).
    void setSideDone(int side) {
        done_[side].store(true, std::memory_order_release);
        std::lock_guard<std::mutex> ck(cvMutex_);
        cv_.notify_all();
    }

    // Establishes the cancellation pointer exactly once, before the workers
    // start. After this point the workers only ever READ cancel_ (in insert(),
    // while throttling) and never write it, so there is no concurrent
    // read/write of this object.
    void setCancel(const std::atomic_bool* cancel) { cancel_ = cancel; }

    // Snapshot of every entry still in the table (unmatched). Not thread-safe
    // against concurrent inserts; call after both workers have finished.
    std::vector<std::pair<int, FileEntry>> remaining() const {
        std::vector<std::pair<int, FileEntry>> out;
        for (size_t i = 0; i < shardCount_; ++i) {
            const auto& map = shards_[i].map;
            out.reserve(out.size() + map.size());
            for (const auto& kv : map) out.emplace_back(kv.second.side, kv.second.entry);
        }
        return out;
    }

    uint64_t pendingCount() const { return pending_.load(std::memory_order_relaxed); }

    // Number of workers currently parked in the throttle wait. Only the source
    // worker ever throttles, so this is 0 or 1. Diagnostic/test hook: a test can
    // spin on this (the project's latch idiom) to observe a worker enter the
    // park deterministically.
    uint64_t throttleWaiters() const { return throttleWaiters_.load(std::memory_order_relaxed); }

private:
    struct Slot {
        int side; // 0 = source, 1 = destination
        FileEntry entry;
    };
    struct Shard {
        std::mutex mutex;
        std::unordered_map<std::wstring, Slot> map;
    };

    static uint64_t Hash(const std::wstring& key) {
        uint64_t h = 1469598103934665603ull; // FNV-1a 64 offset basis
        for (const wchar_t c : key) {
            h ^= static_cast<uint64_t>(static_cast<uint16_t>(c));
            h *= 1099511628211ull;
        }
        return h;
    }

    size_t shardCount_;
    uint64_t mask_;
    std::unique_ptr<Shard[]> shards_;
    uint64_t highWater_ = 1ull << 20; // ~1M unmatched entries: throttle writers
    std::atomic<uint64_t> pending_{0};       // entries currently stored (unmatched)
    std::atomic<uint64_t> throttleWaiters_{0}; // workers parked in the throttle wait
    std::atomic<bool> done_[2]{false, false};
    const std::atomic_bool* cancel_ = nullptr;
    std::mutex cvMutex_;
    std::condition_variable cv_;
};

} // namespace bv