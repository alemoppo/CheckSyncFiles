#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Profiling/HashProfile.h"

namespace bv {
namespace profiling {

// Per-directory scan-time tracking (top-N slowest directories).
//
// Two profilers are kept conceptually separate because they measure different
// phases with different perimeters:
//   - directory LISTING (enumeration): one value per directory, reported once
//     when the directory's listing completes -> exact bounded top-N;
//   - content HASHING: one value per FILE, attributed to the parent directory
//     and summed -> Space-Saving heavy hitters (see DirHashTop).
// List and hash times are never summed into a "total": they are costs of
// different phases. `walkSeconds` (MFT) and `listSeconds` (Win32) are likewise
// never mixed: the MFT walk-step resolves $I30 indexes rather than listing
// through FindFirst/FindNext, so its perimeter differs by construction.

// Entries shown per timing column.
constexpr size_t kDirTopN = 15;
// Space-Saving counters per run side backing the hash top-N display. Kept
// comfortably larger than kDirTopN: every directory whose true hash-time sum
// exceeds totalFed/kDirHashCounters is guaranteed present in top().
constexpr size_t kDirHashCounters = 64;

struct DirEntry {
    std::wstring dir;
    double seconds = 0.0;
};

// Exact bounded top-N for values reported at most once per key (one directory
// listing or MFT walk-step completion per directory). Min-heap: O(log N) per
// add, O(N) memory. Single-threaded use only (enumeration walks are
// sequential per side); no locking.
class DirTopN {
public:
    explicit DirTopN(size_t n = kDirTopN) : n_(n) {}
    void add(std::wstring dir, double seconds);
    // Descending by seconds. Empty when nothing was recorded.
    std::vector<DirEntry> sorted() const;
    bool empty() const { return heap_.empty(); }

private:
    size_t n_;
    std::vector<DirEntry> heap_; // min-heap by seconds
};

// Per-run-side directory-listing columns. `list` holds Win32
// FindFirst/FindNext syscall time per directory (`listSeconds`); `walk` holds
// MFT $I30 resolve + walk-step time per directory (`walkSeconds`).
struct DirListSink {
    DirTopN list;
    DirTopN walk;
};

// Parent directory of an absolute path ("C:\a\b.txt" -> "C:\a"). Used to
// attribute per-file hash times to the containing directory.
std::wstring DirParent(const std::wstring& absPath);

// Space-Saving heavy hitters over per-file hash times keyed by parent
// directory, split by run side (Side::Source = tree A, Side::Dest = tree B;
// the mapping is exact per hashed FILE, see HashOneSide).
//
// Why not an exact map: a directory can reach the top-N purely through the
// SUM of many individually small files, so per-file admission against a
// threshold would silently drop it; but an exact per-directory sum needs one
// entry per directory (O(directories) memory). Space-Saving keeps O(counters)
// memory with a hard guarantee: any directory whose true sum exceeds
// totalFed/counters is always present, with est - err <= true <= est.
// Ordering among near-tied directories is approximate; that is the documented
// price of the memory bound.
//
// Thread-safe: one mutex held only for a map operation per hashed file.
// Contention is negligible next to file I/O. No lock-free machinery.
class DirHashTop {
public:
    explicit DirHashTop(size_t displayN = kDirTopN, size_t counters = kDirHashCounters);
    void add(Side fileSide, const std::wstring& dir, double seconds);
    // Descending by estimated sum, at most displayN entries.
    std::vector<DirEntry> top(int runSide) const;
    bool empty(int runSide) const;
    // Live counters held (<= the configured counter budget). Test hook for
    // the O(counters) memory bound.
    size_t tracked(int runSide) const;

private:
    struct Counter {
        double est = 0.0;
        double err = 0.0;
    };
    struct SideState {
        std::unordered_map<std::wstring, Counter> map;
        double total = 0.0;
    };
    size_t displayN_;
    size_t counters_;
    mutable std::mutex mtx_;
    SideState sides_[2];
};

// Live per-run timing state, owned by ScanController (one instance per run).
// The per-side list sinks are fed single-threaded by the enumerators; `hash`
// is fed concurrently by the hash pool workers.
struct RunDirTiming {
    DirListSink a; // source tree (live enumeration or snapshot capture)
    DirListSink b; // destination tree
    DirHashTop hash;
};

// Plain-data copy of one run's directory timings for ScanReport. Copyable by
// design: the live profilers above hold a mutex and must never cross the
// report boundary.
struct DirTimingReport {
    std::vector<DirEntry> listA;
    std::vector<DirEntry> walkA;
    std::vector<DirEntry> listB;
    std::vector<DirEntry> walkB;
    std::vector<DirEntry> hashA;
    std::vector<DirEntry> hashB;
};

// Copies the current tops out of the live state (descending order).
void SnapshotRunTiming(const RunDirTiming& timing, DirTimingReport& out);

} // namespace profiling
} // namespace bv
