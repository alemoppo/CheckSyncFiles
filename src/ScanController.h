#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "Comparison/ComparisonResult.h"
#include "Comparison/ScanMode.h"
#include "Filesystem/FileEnumerator.h"

namespace bv {

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
    std::wstring currentPath; // directory currently being processed
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
    // Called (from the scanning thread) periodically with running totals to
    // drive a progress bar / status line. Optional.
    std::function<void(const ScanProgress&)> onProgress = nullptr;
    // Optional pointer to an external cancel flag. When set to true, the scan
    // stops gracefully at the next safe point. Owned by the caller.
    std::atomic_bool* cancel = nullptr;
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
