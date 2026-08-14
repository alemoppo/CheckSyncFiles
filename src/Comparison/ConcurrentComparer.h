#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Comparison/ClassifyUtil.h"
#include "Comparison/ComparisonResult.h"
#include "Comparison/ConcurrentSink.h"
#include "Comparison/HashPhase.h"
#include "Comparison/MatchTable.h"
#include "Comparison/ScanMode.h"
#include "Filesystem/FileEnumerator.h"
#include "Filesystem/FileIndex.h"
#include "Hashing/HashCache.h"
#include "Threading/ThreadPool.h"

namespace bv {

// Compares two trees concurrently: worker A enumerates the source, worker B the
// destination; the two streams are paired through a sharded MatchTable and each
// matched pair is classified immediately (atomic stats, thread-safe problems).
// There is no global lock: only the per-shard locks (match-and-remove) and a
// small lock on the problems vector are ever taken.
//
// Source policies:
//   - Live: A enumerates the source device; in Content mode the source files
//     are hashed after enumeration.
//   - FromIndex: A feeds an already-loaded FileIndex (snapshot / --compare),
//     and source digests come from the index, so the source device is never
//     touched (offline verification).
//
// Memory bound: the match table throttles a side that runs ahead of the other; a
// device can never push its entries arbitrarily far past what the other device
// has produced. Content verification runs after BOTH workers finish, on the
// caller's pool.
//
// Outcome gating: Missing/Extra are reported only when both sides succeeded and
// no cancellation happened. If a side fails (e.g. an MFT scan goes incomplete
// after emitting entries), matched pairs already classified are retained but
// nothing is reported as missing/extra -- a partial enumeration must never
// fabricate missing/extra verdicts.
class ConcurrentComparer {
public:
    enum class SourceKind : uint8_t { Live, FromIndex };
    enum class WorkerStatus : uint8_t { Success, Failed, Cancelled };

    struct Result {
        ResultSet results;
        WorkerStatus sourceStatus = WorkerStatus::Failed;
        WorkerStatus destinationStatus = WorkerStatus::Failed;
        // Human-readable notices for the user: e.g. "MFT non utilizzabile su 'C:';
        // ripiego su Win32". Empty when nothing noteworthy happened.
        std::vector<std::wstring> notes;
    };

    using ProgressCallback =
        std::function<void(uint64_t files, uint64_t dirs, const std::wstring& currentPath)>;
    using HashProgressCallback = std::function<void(uint64_t done, uint64_t total)>;
    using EnumeratorFactory = std::function<std::unique_ptr<IFileEnumerator>()>;

    // One entry in a side's enumeration plan: a named back-end (e.g. "MFT",
    // "Win32") plus the factory that builds it. The name is used only for the
    // user-facing fallback notice.
    struct EnumeratorStep {
        std::wstring name;
        EnumeratorFactory factory;
    };

    // `acceptMft` allows the MFT scanner on each local NTFS root (with automatic
    // Win32 fallback only when the MFT fails before emitting any entry).
    // `fromIndex` is required when sourceKind == FromIndex and ignored otherwise.
    ConcurrentComparer(bool caseSensitive, ScanMode mode, bool acceptMft,
                       std::wstring sourceRoot, std::wstring destRoot, SourceKind sourceKind,
                       FileIndex* fromIndex, const std::atomic_bool* cancel = nullptr)
        : caseSensitive_(caseSensitive), mode_(mode), acceptMft_(acceptMft),
          sourceRoot_(std::move(sourceRoot)), destRoot_(std::move(destRoot)),
          sourceKind_(sourceKind), fromIndex_(fromIndex), cancel_(cancel) {}

    // Real-world entry: builds the per-side enumerator plan from `acceptMft`.
    Result run(ThreadPool& hashPool, const ProgressCallback& onProgress = {},
               const HashProgressCallback& onHashProgress = {},
               hashing::HashCache* cache = nullptr);

    // Test entry: enumerators come from the supplied factories (in order of
    // preference). A side moves to the next factory only when the current one
    // failed before emitting any entry -- this models the MFT -> Win32 fallback
    // and lets tests drive both discovery orders deterministically.
    Result runWithFactories(EnumeratorFactory sourceFactory, EnumeratorFactory destFactory,
                            ThreadPool& hashPool, const ProgressCallback& onProgress = {},
                            const HashProgressCallback& onHashProgress = {},
                            hashing::HashCache* cache = nullptr);

    // Same, with a whole fallback chain per side (a vector of factories tried in
    // order). Used by tests to model the MFT -> Win32 fallback explicitly.
    Result runWithFactories(std::vector<EnumeratorFactory> sourceFactories,
                            std::vector<EnumeratorFactory> destFactories,
                            ThreadPool& hashPool, const ProgressCallback& onProgress = {},
                            const HashProgressCallback& onHashProgress = {},
                            hashing::HashCache* cache = nullptr);

    size_t cacheHits() const { return cacheHits_.load(std::memory_order_relaxed); }

private:
    struct WorkerState {
        uint64_t lastFiles = 0; // per-worker cumulative progress baselines
        uint64_t lastDirs = 0;
    };

    // Outcome of a single enumerator run, used to decide the fallback
    // explicitly instead of inferring it from an entry count.
    enum class EnumAttemptResult : uint8_t {
        Finished,          // enumerator returned success
        Cancelled,         // cancellation requested before/during the walk
        FailedWithEntries, // failed AFTER emitting entries: too late to fall back
        FailedEmpty,       // failed BEFORE emitting any entry: safe to fall back
    };

    Result runImpl(std::vector<EnumeratorStep> sourceSteps, std::vector<EnumeratorStep> destSteps,
                   ThreadPool& hashPool, const ProgressCallback& onProgress,
                   const HashProgressCallback& onHashProgress, hashing::HashCache* cache);
    WorkerStatus runEnumWorker(int side, const std::vector<EnumeratorStep>& steps,
                               MatchTable& table, ConcurrentSink& sink,
                               std::vector<ContentCandidate>& candidates, WorkerState& state,
                               std::vector<std::wstring>& notes);
    WorkerStatus runIndexWorker(MatchTable& table, ConcurrentSink& sink,
                                std::vector<ContentCandidate>& candidates);
    // Runs one enumerator to completion and reports WHY it stopped; emits
    // progress and classifies entries through the table.
    EnumAttemptResult runOneStep(int side, const std::wstring& root, IFileEnumerator& enumerator,
                                 MatchTable& table, ConcurrentSink& sink,
                                 std::vector<ContentCandidate>& candidates, WorkerState& state);
    void onEntry(int side, FileEntry e, MatchTable& table, ConcurrentSink& sink,
                 std::vector<ContentCandidate>& candidates);
    void onError(const ScanError& err, ConcurrentSink& sink);
    void finalizeMissingExtra(MatchTable& table, ResultSet& out);
    void sortProblems(ResultSet& out);

    bool caseSensitive_;
    ScanMode mode_;
    bool acceptMft_;
    std::wstring sourceRoot_;
    std::wstring destRoot_;
    SourceKind sourceKind_;
    FileIndex* fromIndex_;
    const std::atomic_bool* cancel_;

    std::atomic<size_t> cacheHits_{0};
    std::atomic<uint64_t> totalFiles_{0};
    std::atomic<uint64_t> totalDirs_{0};
    ProgressCallback onProgress_;
    HashProgressCallback onHashProgress_;
};

} // namespace bv