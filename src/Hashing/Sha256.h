#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace bv {
namespace profiling {
struct FileTimings;
}

namespace hashing {

enum class HashStatus : uint8_t {
    Ok,        // digest filled with the SHA-256 (32 bytes)
    Cancelled, // stopped early because the cancel flag was set (no digest)
    NoAccess,  // could not open the file (access denied)
    ReadError, // open succeeded but read / digest failed
};

// Streams `path` through the Windows CNG (BCrypt) SHA-256 provider into digest.
//
// Reads the file in 1 MiB chunks so files larger than 4 GiB work. Only returns
// Ok and fills `digest` when every byte has been hashed successfully. Uses the
// long-path prefix internally, so paths beyond MAX_PATH are handled.
//
// `cancel`, when non-null, is polled once per 1 MiB chunk: if it turns true the
// stream stops early and returns Cancelled (no digest, no fake read error) so a
// running hashing pool can be drained quickly on Interrompi instead of reading
// the rest of every in-flight file to the end.
HashStatus Sha256File(const std::wstring& path, std::array<uint8_t, 32>& digest,
                      profiling::FileTimings* timings = nullptr,
                      const std::atomic_bool* cancel = nullptr);

// Opens the file read-only and reports its current size and last-write time
// (FILETIME ticks). Returns false when the file cannot be opened. Used by the
// hash cache (path+size+mtime key) and by "changed during scan" detection.
bool StatFile(const std::wstring& path, uint64_t& size, uint64_t& lastWriteTime);

// ---------------------------------------------------------------------------
// Single-handle helpers for HashOneSide (the unified hash-handle flow). These
// are INTERNAL: declared here solely so HashUtil.cpp (same namespace, same
// module) can open ONE file handle, run T1 -> hash -> T2 on it and close once,
// instead of the previous StatFile/Sha256File/StatFile triple-open sequence.
// ---------------------------------------------------------------------------

// Hashes the ALREADY-OPEN `h` into digest. Never closes `h` -- the caller owns
// the handle and must close it. `timings` matches Sha256File semantics: the
// <false> instantiation compiles the exact read loop with zero timing overhead,
// <true> records read/hash/total time around ReadFile / BCryptHashData without
// changing the loop. Explicitly instantiated for <false> and <true> in
// Sha256.cpp, so `HashOneSide` can pick either at the call site.
template <bool Profile>
HashStatus Sha256FileFromHandle(HANDLE h, std::array<uint8_t, 32>& digest,
                                profiling::FileTimings* timings,
                                const std::atomic_bool* cancel = nullptr);

// Reports the current size and last-write time (FILETIME ticks) of an
// ALREADY-OPEN handle, reusing exactly the conversion StatFile() performs on
// BY_HANDLE_FILE_INFORMATION. Returns false when GetFileInformationByHandle
// fails (practically unreachable on a successfully-opened handle).
bool StatHandle(HANDLE h, uint64_t& size, uint64_t& lastWriteTime);

// T2 seam for the change-during-scan check: true when the (size, mtime)
// captured AFTER hashing differs from the pair captured BEFORE (T1). Kept as a
// helper so the before/after comparison logic is unit-testable without a real,
// timing-dependent mid-read mutation.
inline bool HashMetadataChanged(uint64_t sizeBefore, uint64_t mtimeBefore,
                                uint64_t sizeAfter, uint64_t mtimeAfter) {
    return sizeBefore != sizeAfter || mtimeBefore != mtimeAfter;
}

} // namespace hashing
} // namespace bv