#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace bv {
namespace profiling {
struct FileTimings;
}

namespace hashing {

enum class HashStatus : uint8_t {
    Ok,        // digest filled with the SHA-256 (32 bytes)
    NoAccess,  // could not open the file (access denied)
    ReadError, // open succeeded but read / digest failed
};

// Streams `path` through the Windows CNG (BCrypt) SHA-256 provider into digest.
//
// Reads the file in 1 MiB chunks so files larger than 4 GiB work. Only returns
// Ok and fills `digest` when every byte has been hashed successfully. Uses the
// long-path prefix internally, so paths beyond MAX_PATH are handled.
HashStatus Sha256File(const std::wstring& path, std::array<uint8_t, 32>& digest);

// Same, but additionally fills `timings` (when non-null) with per-file
// read/hash/total QPC timing and the number of bytes actually read. Passing a
// non-null pointer is the only way to enable the timing capture; the two-argument
// form above compiles to the exact same code path with zero timing overhead.
HashStatus Sha256File(const std::wstring& path, std::array<uint8_t, 32>& digest,
                      profiling::FileTimings* timings);

// Opens the file read-only and reports its current size and last-write time
// (FILETIME ticks). Returns false when the file cannot be opened. Used by the
// hash cache (path+size+mtime key) and by "changed during scan" detection.
bool StatFile(const std::wstring& path, uint64_t& size, uint64_t& lastWriteTime);

} // namespace hashing
} // namespace bv
