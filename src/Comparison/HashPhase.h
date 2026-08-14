#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Comparison/ClassifyUtil.h"
#include "Comparison/ComparisonResult.h"
#include "Filesystem/FileIndex.h"
#include "Hashing/HashCache.h"
#include "Threading/ThreadPool.h"

namespace bv {

// Content verification (Phase 3), shared by the serial comparator and the
// concurrent comparer.
//
// Hashes every candidate across `pool` in batches (so peak live memory stays
// bounded) and folds the outcome into `out`. Runs after both sides have been
// enumerated, on a single thread that only submits and drains batches.
//
// `offlineSource` selects how the source digest is obtained:
//   - true:  the source device is absent; digests come from `index` (a
//            snapshot captured in Content mode), which must be non-null. Only
//            the destination is read.
//   - false: the source is live under `sourceRoot` and is read too; `index` is
//            ignored.
// `cache` (optional) is the persistent hash cache; a hit skips the read.
// `cacheHits` accumulates hits (relaxed atomics are fine). `onProgress`
// (optional) receives (hashedCount, totalCandidates). `cancel` (optional)
// stops between batches when *cancel becomes true.
void RunHashPhase(const std::vector<ContentCandidate>& candidates, ThreadPool& pool,
                  bool offlineSource, FileIndex* index, const std::wstring& sourceRoot,
                  const std::wstring& destRoot, ResultSet& out, const std::atomic_bool* cancel,
                  const std::function<void(uint64_t done, uint64_t total)>& onProgress,
                  hashing::HashCache* cache, std::atomic<size_t>& cacheHits);

} // namespace bv