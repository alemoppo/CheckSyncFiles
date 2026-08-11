#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "FileEntry.h"

namespace bv {

// A non-fatal error produced during enumeration. The scan continues.
struct ScanError {
    std::wstring path;    // relative path of the failing directory ("" for the root)
    std::wstring message; // human readable (localized) message
    uint32_t winError = 0;
    // Set when the underlying storage disappeared (NAS/USB disconnected) while
    // scanning. The enumerator stops afterwards: continuing would only produce
    // more noise. The caller should surface this and let the user re-attach.
    bool lostDevice = false;
};

// Interface implemented by all enumeration back-ends:
//   - Win32Enumerator : regular FindFirstFile/FindNextFile walk (all filesystems)
//   - MftEnumerator   : NTFS Master File Table scan (Phase 4)
//   - NetworkEnumerator : SMB-aware walk, currently = Win32Enumerator
class IFileEnumerator {
public:
    using EntryCallback = std::function<bool(FileEntry&&)>; // return false to abort
    using ErrorCallback = std::function<void(const ScanError&)>;
    // Called after each directory is finished (so the totals grow); currentPath
    // is the directory just completed (owned by the enumerator during the call).
    using ProgressCallback =
        std::function<void(uint64_t files, uint64_t dirs, const std::wstring& currentPath)>;

    virtual ~IFileEnumerator() = default;

    // Enumerates every file and directory under `root` in depth-first order.
    // Directories themselves are reported as entries (with isDirectory=true).
    // Returns true on success or early abort requested by the callback;
    // false if the root itself could not be accessed.
    //
    // `onProgress` (optional) is invoked periodically with running totals to
    // drive a progress bar; a default no-op is used when omitted.
    virtual bool enumerate(const std::wstring& root,
                           const EntryCallback& onEntry,
                           const ErrorCallback& onError,
                           const ProgressCallback& onProgress = {}) = 0;
};

} // namespace bv
