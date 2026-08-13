#pragma once

#include "FileEnumerator.h"

namespace bv {

// Recursive, iterative (stack based) enumeration using FindFirstFileW /
// FindNextFileW. Works on every Windows filesystem and on SMB shares.
//
// Behaviour:
//   - long paths are supported via the "\\?\" prefix (UNC aware)
//   - hidden and system files are included
//   - directory reparse points (junctions / directory symlinks) are recorded
//     as directory entries but NOT followed, which guarantees there are no
//     cycles (a symlink loop can never be entered)
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
