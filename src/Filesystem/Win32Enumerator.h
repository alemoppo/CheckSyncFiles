#pragma once

#include "FileEnumerator.h"

namespace bv {

// Recursive, iterative (stack based) enumeration using FindFirstFileW /
// FindNextFileW. Works on every Windows filesystem and on SMB shares.
//
// Behaviour:
//   - long paths are supported via the "\\?\" prefix (UNC aware)
//   - hidden and system files are included
//   - reparse points (symlinks, junctions, mount points) are recorded as plain
//     entries but are NEVER traversed, regardless of what they point to:
//       * a directory reparse point is listed as a directory entry
//         (isDirectory == true, attributes contain FILE_ATTRIBUTE_REPARSE_POINT)
//         and is not descended into
//       * a file reparse point is listed as a file entry
//     This is an intentional policy: a reparse point may point at any path,
//     including one of its own ancestors (a cycle). Following it would make the
//     scan loop forever. Treating every reparse point as a leaf guarantees
//     termination on any tree, adversarial or not.
//   - per-directory failures are reported through the error callback and do
//     not stop the scan
class Win32Enumerator : public IFileEnumerator {
public:
    bool enumerate(const std::wstring& root,
                   const EntryCallback& onEntry,
                   const ErrorCallback& onError,
                   const ProgressCallback& onProgress = {},
                   const std::atomic_bool* cancel = nullptr) override;
};

} // namespace bv
