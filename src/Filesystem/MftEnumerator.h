#pragma once

#include <string>

#include "FileEnumerator.h"

namespace bv {

// NTFS Master File Table scan (Phase 4).
//
// Reads the volume's raw $MFT once and reconstructs the subtree rooted at
// `root`, instead of the directory-by-directory FindFirstFile walk. This trades
// extra RAM/CPU (the whole MFT must be read) for a single sequential pass over
// the volume metadata, which can be much faster than many small directory reads
// on deep or fragmented trees.
//
// Constraints:
//   - ONLY works when `root` is a plain path on a LOCAL NTFS volume (no UNC).
//     Directory reparse points (symlinks/junctions) are reported but NOT
//     followed (same loop-safety guarantee as Win32Enumerator).
//   - Best-effort: if the volume is not NTFS, $MFT cannot be opened, or parsing
//     cannot proceed, enumerate() returns false and the caller must fall back to
//     Win32Enumerator (never silently produce a wrong result).
//   - FileEntry.fileId is filled with the MFT record number.
class MftEnumerator : public IFileEnumerator {
public:
    // True when `root` is on a local NTFS volume we expect to be able to scan
    // ($MFT readable). Used to pick the back-end with automatic fallback.
    static bool IsSupported(const std::wstring& root);

    bool enumerate(const std::wstring& root,
                   const EntryCallback& onEntry,
                   const ErrorCallback& onError,
                   const ProgressCallback& onProgress = {},
                   const std::atomic_bool* cancel = nullptr) override;
};

} // namespace bv