#pragma once

#include <cstdint>
#include <string>

namespace bv {

// Real kind of a reparse point. The enumerators only see
// FILE_ATTRIBUTE_REPARSE_POINT; this resolves the actual tag via
// FSCTL_GET_REPARSE_POINT (see ReparsePoint.cpp).
enum class ReparseKind : uint8_t {
    None = 0,      // not a reparse point (or unknown on read error)
    SymlinkFile,   // IO_REPARSE_TAG_SYMLINK pointing at a file
    SymlinkDir,    // IO_REPARSE_TAG_SYMLINK pointing at a directory
    Junction,      // IO_REPARSE_TAG_MOUNT_POINT (always a directory)
    Other,         // any other reparse tag: visible, never synced (MVP)
};

// Resolves the reparse tag of `absPath` WITHOUT following the link.
// `isDirectory` is the caller-known entry type (from enumeration) and
// decides SymlinkFile vs SymlinkDir. Returns None when the path is not a
// reparse point or the tag cannot be read; `error` (optional) gets details.
ReparseKind GetReparseKind(const std::wstring& absPath, bool isDirectory,
                           std::wstring* error = nullptr);

// Reads the link target of a symlink/junction WITHOUT following it.
// Returns the SubstituteName with any "\\?\" prefix stripped (empty on
// error); `kindOut` (optional) receives the resolved kind.
std::wstring ReadLinkTarget(const std::wstring& absPath, ReparseKind* kindOut = nullptr,
                            std::wstring* error = nullptr);

// Creates a file symlink, directory symlink or junction at `linkPath`
// pointing at `target` (absolute or drive-relative; junctions need an
// absolute target). Parent directory must exist. Symlink creation needs
// SeCreateSymbolicLinkPrivilege or Developer Mode; failures are reported
// in `error`, never thrown.
bool CreateLink(const std::wstring& linkPath, const std::wstring& target, ReparseKind kind,
                std::wstring& error);

// Deletes a link itself, never its target: DeleteFileW for file symlinks,
// RemoveDirectoryW for directory symlinks and junctions.
bool DeleteLink(const std::wstring& linkPath, bool isDirectory, std::wstring& error);

// Comparison form of a link target: an absolute target inside `ownRoot` is
// mapped to a root-relative surrogate ("<root>\rel"), so a mirrored pair
// (A\real vs B\real) compares equal; anything else compares verbatim.
// Lets "B mirrors A" converge instead of flagging every absolute link.
std::wstring NormalizeLinkTargetForCompare(const std::wstring& target,
                                           const std::wstring& ownRoot);

// Write form of a source link target into the other root: an absolute target
// inside `fromRoot` is rebased onto `toRoot`; anything else is kept verbatim
// (relative targets and outside-root absolutes travel unchanged).
std::wstring RebaseLinkTargetForWrite(const std::wstring& target,
                                      const std::wstring& fromRoot,
                                      const std::wstring& toRoot);

// The link's own last-write time (FILETIME ticks), without following it.
// Returns 0 when the link cannot be opened.
uint64_t StatLinkItself(const std::wstring& absPath);

// Resolves a known-reparse entry into comparable link form: kind, target,
// size (UTF-8 target byte length, 0 for directory links) and the link's own
// mtime. Returns false when the tag is unsupported or the target cannot be
// read; `kind` is still set when the tag itself was readable. Lets both
// enumerators produce identical FileEntry values for the same link.
bool ResolveLinkEntry(const std::wstring& absPath, bool isDirectory, ReparseKind& kind,
                      std::wstring& target, uint64_t& linkSize, uint64_t& linkMtime);

} // namespace bv
