#pragma once

#include <string>
#include <vector>

namespace bv {
namespace pathutil {

// Cleans a user-supplied root path:
//  - strips a leading "\\?\" or "\\?\UNC\" prefix if present
//  - removes trailing backslashes (keeping roots like "D:\\" and "\\\\server\\share")
// Returns an empty string for an empty input.
std::wstring NormalizeRoot(const std::wstring& root);

// Joins a relative path and a single path component with '\\'.
// JoinRel(L"", L"a") == L"a", JoinRel(L"a", L"b") == L"a\\b".
std::wstring JoinRel(const std::wstring& rel, const std::wstring& name);

// Returns the absolute path for root + relative path (no long-path prefix).
std::wstring MakeAbsolute(const std::wstring& rootAbs, const std::wstring& rel);

// Prepends the "\\?\" long-path prefix (or "\\?\UNC\" for UNC paths) so that
// Win32 APIs accept paths longer than MAX_PATH and UNC paths.
std::wstring AddLongPathPrefix(const std::wstring& abs);

// Case-folds a string for comparison. Windows/SMB filesystems are
// case-insensitive; this mirrors NTFS behaviour (uppercase, invariant locale).
// Used as the key normalization for case-insensitive matching.
std::wstring FoldForCompare(const std::wstring& s);

// True if the sorted (ascending) `keys` list contains an entry that is a strict
// descendant of `dirKey`, i.e. starts with dirKey + "\\". Used to suppress
// redundant "directory" lines when the whole subtree is already reported.
bool HasDescendant(const std::vector<std::wstring>& keys, const std::wstring& dirKey);

// Wide (UTF-16 on Windows) <-> UTF-8 conversion via CP_UTF8, used by the
// exporters and the binary snapshot / hash-cache formats.
std::string ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);

} // namespace pathutil
} // namespace bv
