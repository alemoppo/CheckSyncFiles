#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Comparison/ComparisonResult.h"
#include "Comparison/ScanMode.h"
#include "Filesystem/FileEnumerator.h"
#include "Filesystem/FileIndex.h"
#include "Hashing/HashCache.h"
#include "Threading/ThreadPool.h"

namespace bv {

// Compares a pre-built source index against a destination tree that is
// streamed (enumerated on the fly, not indexed), keeping memory bounded.
//
// Matching is by relative path (with the index case policy). As destination
// entries are matched they are erased from the source index, so what remains
// afterwards is exactly the set of missing entries.
//
// In Content mode, files that match on path AND size are not classified yet;
// they are collected in `pendingHashes_` and verified afterwards by
// runHashing(). This is the one phase whose working set is proportional to the
// number of candidate files (unavoidable: every candidate must be checksummed).
//
// Two source policies are supported:
//   - live source: hash both source and destination files (constructor with a
//     source root);
//   - offline / pre-hashed source: the source device is absent and its digests
//     come from the index (snapshot captured in Content mode); only the
//     destination is read (constructor without a source root).
class FileComparator {
public:
    // `sourceRoot` is the absolute path of the source tree, needed to build the
    // source side of a content-hash pair.
    FileComparator(FileIndex& source, ScanMode mode, const std::wstring& sourceRoot)
        : source_(source), mode_(mode), sourceRoot_(sourceRoot) {}

    // Offline comparison: source digests are already stored in `source`
    // (loaded from a snapshot). The source files are never read.
    FileComparator(FileIndex& source, ScanMode mode) : source_(source), mode_(mode) {}

    // Enumerates `destRoot` and classifies every entry into `out`.
    // Errors encountered while enumerating destination are also recorded.
    // Returns false if the destination root itself could not be accessed.
    // `onProgress` (optional) is forwarded to the enumerator; `cancel`
    // (optional) stops the comparison when *cancel becomes true.
    bool run(const std::wstring& destRoot,
             IFileEnumerator& destEnumerator,
             ResultSet& out,
             const IFileEnumerator::ProgressCallback& onProgress = {},
             const std::atomic_bool* cancel = nullptr);

    // Content verification. Must be called after a successful run() when the
    // mode is Content: hashes every collected pair across `pool` (batches, so
    // peak live memory stays bounded) and folds the outcome into `out`.
    // `cache` (optional) is a persistent hash cache keyed by
    // path+size+mtime; hits skip re-reading the file. `onProgress` (optional)
    // receives (hashedCount, totalPairs). `cancel` (optional) stops between
    // batches when *cancel becomes true.
    void runHashing(ThreadPool& pool,
                    ResultSet& out,
                    const std::atomic_bool* cancel = nullptr,
                    const std::function<void(uint64_t done, uint64_t total)>& onProgress = {},
                    hashing::HashCache* cache = nullptr);

    // Number of hash-cache hits of the last runHashing() invocation.
    size_t cacheHits() const { return cacheHits_.load(std::memory_order_relaxed); }

private:
    struct HashPair {
        std::wstring relativePath;
        uint64_t sizeSource = 0;
        uint64_t sizeDest = 0;
        uint64_t srcMtime = 0; // last-write FILETIME at enumeration time
        uint64_t dstMtime = 0;
    };

    void classifyMatched(FileEntry& src, FileEntry& dst, ResultSet& out);
    void recordMissing(ResultSet& out);

    FileIndex& source_;
    ScanMode mode_;
    std::wstring sourceRoot_; // empty == source digests come from the index
    std::wstring destRoot_;
    std::vector<HashPair> pendingHashes_;
    std::atomic<size_t> cacheHits_{0};
};

} // namespace bv
