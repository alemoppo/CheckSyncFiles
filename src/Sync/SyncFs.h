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

// Copies srcAbs -> dstAbs DIRECTLY over the destination (deliberate design:
// no temp file, so huge files never need double space; an interrupted copy
// may leave a truncated dst, which the next comparison re-detects for a
// retry). Preserves mtime + attributes. Refuses when src is a reparse point.
// Both parent chains must be link-free (CheckParentChain). `cancel` is polled
// during the copy.
bool CopyFileDirect(const std::wstring& srcRoot, const std::wstring& srcAbs,
                    const std::wstring& dstRoot, const std::wstring& dstAbs,
                    const std::atomic_bool* cancel, std::wstring& error);

// Creates every missing component of dirAbs (absent segments ok, file or
// reparse point in the way = error). Every created level is verified
// link-free, so a junction can never divert the creation outside `root`.
bool CreateDirAll(const std::wstring& root, const std::wstring& dirAbs,
                  std::wstring& error);

// Deletes one file (clears read-only first). Absent = success (no-op).
// The parent chain must be link-free.
bool DeleteFileOne(const std::wstring& root, const std::wstring& abs,
                   std::wstring& error);

// Guarded recursive delete of dirAbs: only entries whose folded rel is in
// `allowedRels` (or dirs that become empty on the way) are removed; links are
// unlinked, never followed. Absent = success. Anything unexpected aborts the
// directory with `skipped=true`: entries already removed stay removed (no
// rollback), the rest is left in place. `cancel` (nullable) is polled between
// entries and before recursing/removing: on cancel the walk stops at once
// with `cancelled=true`, leaving the partial state in place.
struct DeleteDirOutcome {
    bool ok = false;
    bool skipped = false;   // unexpected content: dir left (partially) in place
    bool cancelled = false; // user cancel: stopped early, partial state kept
    std::wstring message;
};
DeleteDirOutcome DeleteDirGuarded(const std::wstring& root, const std::wstring& dirAbs,
                                  const std::vector<std::wstring>& allowedFoldedRels,
                                  const std::atomic_bool* cancel = nullptr);

// Live kind of a path: what the executor adapts to (TOCTOU-safe planning).
// Supported links resolve to LinkFile/LinkDir; anything else reparse-shaped
// (unknown tag, unreadable, unsupported) is UnsupportedReparse and must never
// be mistaken for a normal link: the executor preserves instead of acting.
enum class LiveKind { Absent, File, Dir, LinkFile, LinkDir, UnsupportedReparse, Other };
LiveKind StatLiveKind(const std::wstring& abs);

} // namespace sync
} // namespace bv
