#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace bv {
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

} // namespace hashing
} // namespace bv