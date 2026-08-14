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
// slow one (e.g. a USB device). A global pending counter throttles the source
// worker (side 0) while it exceeds a high-water mark and the destination is
// still running; the destination worker is never parked, so the table can always
// be shrunk by its matches or fully released when it finishes. Cancellation also
// wakes any waiter. See insert() for the deadlock rationale.
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
    Outcome insert(const std::wstring& key, int side, FileEntry&& e, FileEntry& peer) {
        if (side == 0 && pending_.load(std::memory_order_relaxed) >= highWater_) {
            std::unique_lock<std::mutex> lk(cvMutex_);
            cv_.wait(lk, [&] {
                return pending_.load(std::memory_order_relaxed) < highWater_ ||
                       done_[1].load(std::memory_order_acquire) ||
                       (cancel_ && cancel_->load(std::memory_order_relaxed));
            });
        }

        Shard& s = shards_[Hash(key) & mask_];
        std::lock_guard<std::mutex> slk(s.mutex);
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
        peer = std::move(it->second.entry);
        s.map.erase(it);
        pending_.fetch_sub(1, std::memory_order_relaxed);
        cv_.notify_one();
        return Outcome::Matched;
    }

    // Marks `side` as fully enumerated so throttle waiters on the other side
    // are released (no future peer can arrive from a finished side).
    void setSideDone(int side, const std::atomic_bool* cancel = nullptr) {
        if (cancel) cancel_ = cancel;
        done_[side].store(true, std::memory_order_release);
        cv_.notify_all();
    }

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
    std::atomic<uint64_t> pending_{0};
    std::atomic<bool> done_[2]{false, false};
    const std::atomic_bool* cancel_ = nullptr;
    std::mutex cvMutex_;
    std::condition_variable cv_;
};

} // namespace bv