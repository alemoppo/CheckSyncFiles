#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include "Hashing/HashCache.h"
#include "Hashing/Sha256.h"

namespace bv {
namespace hashing {

using Digest = std::array<uint8_t, 32>;

// Hashes one side (source or destination) of a candidate pair, honouring the
// optional persistent cache and detecting changes between enumeration time
// (`expectedSize`/`expectedMtime`, captured from the entry) and the moment the
// file is actually read:
//   - current stat != expected          -> `changed` (written while we waited)
//   - file mutated between pre- and post-hash stats -> `changed`
// A cache hit under the *current* (size, mtime) key is always sound: the cached
// digest was computed for exactly the current file state.
//
// Shared by the serial comparator and the concurrent comparer. Thread-safe:
// safe to call concurrently from any number of pool workers.
void HashOneSide(const std::wstring& absPath, uint64_t expectedSize, uint64_t expectedMtime,
                 bool& changed, HashStatus& status, Digest& digest, bool valid,
                 HashCache* cache, std::atomic<size_t>& cacheHits);

} // namespace hashing
} // namespace bv