#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Comparison/ComparisonResult.h"
#include "Comparison/ScanMode.h"
#include "Export/ExportUtil.h"
#include "Filesystem/FileEnumerator.h"
#include "Profiling/HashProfile.h"

namespace bv {

namespace hashing {
class HashCache;
}

class FileIndex;
class ThreadPool;

// Hashes every file of `index` (all under `root`) into the index, in bounded
// batches over `pool`. A slot whose job did not produce a digest (cancellation
// landed mid-batch, or the file could not be hashed) is left WITHOUT an entry,
// never with an all-zero digest. Declared here (it is an internal helper) so
// the snapshot-capture cancellation behaviour can be tested directly.
void HashSourceIndex(FileIndex& index, const std::wstring& root, ThreadPool& pool,
                     const std::atomic_bool* cancel, hashing::HashCache* cache,
                     std::atomic<size_t>& cacheHits,
                     const std::function<void(uint64_t done, uint64_t total)>& onProgress,
                     std::function<void()> onBatchSubmitted = {},
                     profiling::HashProfiler* prof = nullptr);

// High level phases of a run, reported through ScanProgress.
enum class ScanPhase : uint8_t {
    EnumerateSource,     // building the source index
    CompareDestination,  // enumerating + comparing the destination
    Hashing,             // content verification (Phase 3)
    Done,
};

// Which filesystem enumeration back-end to use.
enum class EnumeratorBackend : uint8_t {
    Auto,  // MFT when both roots are local NTFS, else Win32
    Win32, // regular FindFirstFile/FindNextFile walk
    Mft,   // NTFS Master File Table scan (fallback to Win32 on failure)
};

struct ScanProgress {
    ScanPhase phase = ScanPhase::EnumerateSource;
    uint64_t files = 0;   // files visited in the current phase
    uint64_t dirs = 0;    // directories visited in the current phase
    uint64_t bytes = 0;   // cumulative size of the files visited in this phase
    std::wstring currentPath; // directory currently being processed
    // Number of hash workers actually running during ScanPhase::Hashing
    // (live, updated while the phase is in progress); 0 in the other phases.
    unsigned int threads = 0;
};

struct ScanOptions {
    std::wstring source;
    std::wstring destination;
    ScanMode mode = ScanMode::Presence;
    bool caseSensitive = false;
    // Worker threads for the content-hash phase. 0 (default) = "auto": the
    // controller resolves it from the IO class of the two roots. Only used when
    // `mode` is Content; otherwise no hash pool is created.
    unsigned int hashThreads = 0;
    // Enumeration back-end (default Auto uses MFT with automatic Win32 fallback
    // when both roots are local NTFS volumes).
    EnumeratorBackend backend = EnumeratorBackend::Auto;
    // Phase 5 ----------------------------------------------------------------
    // Capture the enumerated source index to this snapshot file. In Content
    // mode the source files are also hashed first so the snapshot embeds their
    // digests (later offline verification). Empty = no snapshot.
    std::wstring snapshotOut;
    // Load the source index from this snapshot instead of enumerating the
    // source device (which may not even be connected). Mutually exclusive with
    // `snapshotOut`.
    std::wstring compareFrom;
    // Write the resulting problems (non-identical entries) to this CSV/JSON
    // file after the run. Empty = no export.
    std::wstring exportPath;
    exporting::ExportFormat exportFormat = exporting::ExportFormat::Auto;
    // Persistent SHA-256 cache (absolute path + size + last-write time). When
    // set, files whose key is unchanged are not re-read. Empty = disabled.
    std::wstring hashCacheFile;
    // Called (from the scanning thread) periodically with running totals to
    // drive a progress bar / status line. Optional.
    std::function<void(const ScanProgress&)> onProgress = nullptr;
    // Optional pointer to an external cancel flag. When set to true, the scan
    // stops gracefully at the next safe point. Owned by the caller.
    std::atomic_bool* cancel = nullptr;
    // Optional caller-owned content-hash profiler. When non-null it is enabled,
    // fed from every hash phase of the run, and a copy of its aggregate report
    // is stored in ScanReport::hashProfile. The caller keeps ownership so it can
    // read the verbose per-job records afterwards. Default: profiling off.
    profiling::HashProfiler* hashProfiler = nullptr;
};

struct ScanReport {
    ResultSet results;
    bool sourceOk = true;
    bool destinationOk = true;
    double secondsTotal = 0.0;
    double secondsEnumerateSource = 0.0;
    double secondsDestinationPass = 0.0; // destination enumeration + comparison
    double secondsHashing = 0.0;          // content verification (Phase 3)
    unsigned int hashThreadsUsed = 0;     // hash pool size actually launched (0 = none)
    EnumeratorBackend backendUsed = EnumeratorBackend::Win32; // what actually ran

    // Phase 5 -----------------------------------------------------------------
    ScanMode modeUsed = ScanMode::Presence; // Content may be degraded to Size
    bool usedSnapshot = false;              // source side loaded from a snapshot
    bool contentDegradedToSize = false;     // snapshot had no digests
    bool snapshotWritten = false;           // `snapshotOut` was produced
    bool exportWritten = false;             // `exportPath` was produced
    std::wstring exportError;
    size_t hashCacheHits = 0;               // files skipped thanks to the cache
    // Tasks that threw inside the hash worker pool. Such an exception leaves the
    // candidate reported as a read error (never a wrong verdict), but the count
    // is surfaced so the failure is not invisible.
    uint64_t hashingErrors = 0;

    // Human-readable notices to show the user (e.g. back-end fallbacks, or a
    // warning that distinct paths folded to the same case-insensitive key).
    std::vector<std::wstring> notes;
    // Distinct paths that collapsed to one record under the case policy
    // (last-wins). Non-zero means the user should be warned that a name was
    // dropped from the source index.
    size_t pathCollisions = 0;
    // Aggregate content-hash profiling report (filled only when
    // ScanOptions::hashProfiler is non-null).
    profiling::HashProfileReport hashProfile;
};

// Orchestrates a comparison run:
//   1. enumerate the source tree into a FileIndex
//   2. enumerate the destination tree and compare it against the index
// The destination is streamed (never indexed) to bound memory.
class ScanController {
public:
    explicit ScanController(bool caseSensitive) : caseSensitive_(caseSensitive) {}

    ScanReport run(const ScanOptions& options);

private:
    bool caseSensitive_;
};

} // namespace bv
