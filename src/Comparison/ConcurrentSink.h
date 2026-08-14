#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include "Comparison/ComparisonResult.h"

namespace bv {

// Thread-safe destination for comparison results produced by the concurrent
// comparer. Every stats counter is an atomic, incremented from the two
// enumeration workers and from the hash drain; non-identical entries are
// appended to `problems` under a mutex. The hot path stays lock-free for the
// counters and only contends on the mutex for the (usually small) set of
// problems.
//
// Call take() exactly once after all workers and hash tasks have finished: it
// snapshots the stats and moves the problems out. Sorting into a deterministic
// order is left to the caller so the whole report can be sorted exactly once at
// the very end (post-join phases may still append problems).
class ConcurrentSink {
public:
    struct AtomicStats {
        std::atomic<uint64_t> sourceFiles{0};
        std::atomic<uint64_t> sourceDirs{0};
        std::atomic<uint64_t> destFiles{0};
        std::atomic<uint64_t> destDirs{0};

        std::atomic<uint64_t> identicalFiles{0};
        std::atomic<uint64_t> identicalDirs{0};
        std::atomic<uint64_t> missingFiles{0};
        std::atomic<uint64_t> missingDirs{0};
        std::atomic<uint64_t> extraFiles{0};
        std::atomic<uint64_t> extraDirs{0};
        std::atomic<uint64_t> sizeMismatch{0};
        std::atomic<uint64_t> contentMismatch{0};

        std::atomic<uint64_t> readErrors{0};
        std::atomic<uint64_t> accessDenied{0};
        std::atomic<uint64_t> changedDuringScan{0};

        std::atomic<uint64_t> bytesSource{0};
        std::atomic<uint64_t> bytesDest{0};
    };

    explicit ConcurrentSink() = default;

    AtomicStats& stats() { return stats_; }

    void addProblem(FileResult&& r) {
        std::lock_guard<std::mutex> lock(mutex_);
        problems_.push_back(std::move(r));
    }

    // Moves everything into a plain ResultSet (order preserved). Not safe
    // against concurrent appends; call after joining the comparison workers.
    ResultSet take() {
        ResultSet out;
        out.stats.sourceFiles = stats_.sourceFiles.load(std::memory_order_relaxed);
        out.stats.sourceDirs = stats_.sourceDirs.load(std::memory_order_relaxed);
        out.stats.destFiles = stats_.destFiles.load(std::memory_order_relaxed);
        out.stats.destDirs = stats_.destDirs.load(std::memory_order_relaxed);
        out.stats.identicalFiles = stats_.identicalFiles.load(std::memory_order_relaxed);
        out.stats.identicalDirs = stats_.identicalDirs.load(std::memory_order_relaxed);
        out.stats.missingFiles = stats_.missingFiles.load(std::memory_order_relaxed);
        out.stats.missingDirs = stats_.missingDirs.load(std::memory_order_relaxed);
        out.stats.extraFiles = stats_.extraFiles.load(std::memory_order_relaxed);
        out.stats.extraDirs = stats_.extraDirs.load(std::memory_order_relaxed);
        out.stats.sizeMismatch = stats_.sizeMismatch.load(std::memory_order_relaxed);
        out.stats.contentMismatch = stats_.contentMismatch.load(std::memory_order_relaxed);
        out.stats.readErrors = stats_.readErrors.load(std::memory_order_relaxed);
        out.stats.accessDenied = stats_.accessDenied.load(std::memory_order_relaxed);
        out.stats.changedDuringScan = stats_.changedDuringScan.load(std::memory_order_relaxed);
        out.stats.bytesSource = stats_.bytesSource.load(std::memory_order_relaxed);
        out.stats.bytesDest = stats_.bytesDest.load(std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            out.problems.swap(problems_);
        }
        return out;
    }

private:
    AtomicStats stats_;
    std::mutex mutex_;
    std::vector<FileResult> problems_;
};

} // namespace bv