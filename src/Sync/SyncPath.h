#pragma once

#include <cstdint>
#include <string>

namespace bv {
namespace sync {

// Centralized path containment for every sync write/delete.
//
// Rules (strict by design; comparison rows are canonical but rows can be
// stale, forged by a hostile snapshot, or raced by the live filesystem):
//   - `rel` must be relative: no drive letter, no leading slash/backslash,
//     no ".." component at all (canonical rels never contain one);
//   - forward slashes are normalized to backslashes;
//   - the joined absolute path must stay inside `root` after normalization
//     (case-insensitive prefix + separator boundary, so C:\AB never passes
//     for C:\A).
// Returns the absolute (unprefixed) path, or empty when rejected.
std::wstring ResolveWithinRoot(const std::wstring& root, const std::wstring& rel);

// Free bytes available on the volume hosting `path` (0 on error).
uint64_t FreeBytesOnVolume(const std::wstring& path);

} // namespace sync
} // namespace bv
