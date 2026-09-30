#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "Filesystem/ReparsePoint.h"

namespace bv {
namespace sync {

// Shared filesystem primitives used by the executor (single actions and full
// plans go through the same code; only plan construction differs).
// Every function validates containment of its outputs via ResolveWithinRoot
// and never follows a link/junction/reparse point. Italian error texts.

// Copies srcAbs -> dstAbs through a temp file in the destination directory
// + atomic rename, so an interrupted copy never leaves a truncated dst and
// never destroys a previous dst early. Preserves mtime + attributes.
// Refuses when src is a reparse point. `cancel` is polled during the copy.
bool CopyFileAtomic(const std::wstring& srcAbs, const std::wstring& dstAbs,
                    const std::atomic_bool* cancel, std::wstring& error);

// Creates every missing component of dirAbs (absent segments ok, file in the
// way = error). Resolves the full chain inside `root`.
bool CreateDirAll(const std::wstring& root, const std::wstring& dirAbs,
                  std::wstring& error);

// Deletes one file (clears read-only first). Absent = success (no-op).
bool DeleteFileOne(const std::wstring& abs, std::wstring& error);

// Guarded recursive delete of dirAbs: only entries whose folded rel is in
// `allowedRels` (or dirs that become empty on the way) are removed; links are
// unlinked, never followed. Absent = success. Anything unexpected aborts the
// directory with `skipped=true`: entries already removed stay removed (no
// rollback), the rest is left in place.
struct DeleteDirOutcome {
    bool ok = false;
    bool skipped = false; // unexpected content: dir left (partially) in place
    std::wstring message;
};
DeleteDirOutcome DeleteDirGuarded(const std::wstring& root, const std::wstring& dirAbs,
                                  const std::vector<std::wstring>& allowedFoldedRels);

// Live kind of a path: what the executor adapts to (TOCTOU-safe planning).
enum class LiveKind { Absent, File, Dir, LinkFile, LinkDir, Other };
LiveKind StatLiveKind(const std::wstring& abs);

} // namespace sync
} // namespace bv
