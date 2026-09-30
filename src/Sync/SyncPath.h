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

// Parent-chain containment: every EXISTING intermediate component of `abs`
// below `root` must be a plain directory. A reparse point (junction, symlink,
// anything reparse-shaped) or a non-directory in the way is rejected, because
// the OS would resolve the operation outside the root. Absent components are
// fine (created later, each level re-checked by CreateDirAll). The leaf
// itself is NOT checked: it may legitimately be the link the operation acts
// on (per-op explicit semantics). Detection never follows links (attribute
// query reports the link itself). Returns empty when safe, else a reason.
std::wstring CheckParentChain(const std::wstring& root, const std::wstring& abs);

} // namespace sync
} // namespace bv
