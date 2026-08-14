#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Comparison/ClassifyUtil.h"
#include "Comparison/ComparisonResult.h"
#include "Comparison/ConcurrentSink.h"
#include "Filesystem/FileIndex.h"
#include "Hashing/HashCache.h"
#include "Threading/ThreadPool.h"

namespace bv {

// Content verification (Phase 3), shared by the serial comparator and the
// concurrent comparer.
//
// A candidate (same relative path + size on both sides) is hashed across a
// ThreadPool and its outcome is folded into a thread-safe ConcurrentSink:
// identical files only bump a counter, mismatches / read errors / change
// detection append a FileResult. The sink decouples the classification from
// the writer, so the serial path and the concurrent path (which hashes while
// the enumerators are still running) share exactly one implementation.
//
// `offlineSource` selects how the source digest is obtained:
//   - true:  the source device is absent; digests come from `index` (a
//            snapshot captured in Content mode), which must be non-null. Only
//            the destination is read.
//   - false: the source is live under `sourceRoot` and is read too; `index` is
//            ignored.
// `cache` (optional) is the persistent hash cache; a hit skips the read.
// `cacheHits` accumulates hits (relaxed atomics are fine). `cancel` (optional)
// stops the walk between batches when *cancel becomes true.
//
// SubmitHashCandidates() submits one task per candidate; each task captures its
// candidate by value, so the input vector can be reused/destroyed as soon as
// the call returns. It performs NO synchronization: the caller is responsible
// for bounding outstanding work and for ensuring no task can still touch
// `sink` before it is read (a final pool.waitAll()).
//
// `hashDone` (optional) is a completion counter: each task increments it after
// it hashes its candidate (even if the task throws, so progress can still reach
// 100%). It lets a concurrent producer report accurate "completed" progress
// without waiting for batches to drain. Pass nullptr when progress is tracked
// elsewhere (e.g. RunHashPhase counts its own batches).
constexpr size_t kHashBatchSize = 256;      // candidates per submission batch
constexpr size_t kHashMaxOutstanding = 1024; // cap on submitted-but-not-finished tasks

void SubmitHashCandidates(const std::vector<ContentCandidate>& candidates, ThreadPool& pool,
                          bool offlineSource, FileIndex* index,
                          const std::wstring& sourceRoot, const std::wstring& destRoot,
                          ConcurrentSink& sink, const std::atomic_bool* cancel,
                          hashing::HashCache* cache, std::atomic<size_t>& cacheHits,
                          std::atomic<uint64_t>* hashDone = nullptr);

// Legacy whole-phase entry point (serial comparator): hashes `candidates` in
// bounded batches of kHashBatchSize and folds the outcomes into `out`.
// `onProgress` (optional) receives (hashedCount, totalCandidates).
void RunHashPhase(const std::vector<ContentCandidate>& candidates, ThreadPool& pool,
                  bool offlineSource, FileIndex* index, const std::wstring& sourceRoot,
                  const std::wstring& destRoot, ResultSet& out, const std::atomic_bool* cancel,
                  const std::function<void(uint64_t done, uint64_t total)>& onProgress,
                  hashing::HashCache* cache, std::atomic<size_t>& cacheHits);

} // namespace bv