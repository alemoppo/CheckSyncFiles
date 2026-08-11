#pragma once

#include <cstdint>
#include <string>

namespace bv {

// A single record collected during a filesystem scan.
//
// `relativePath` is the canonical form used for comparison:
//   - backslash ('\\') separated, no leading '\\', no trailing '\\'
//   - relative to the scan root (e.g. "Foto\\2025\\foto001.jpg")
//   - keeps the original case as returned by the filesystem
struct FileEntry {
    std::wstring relativePath;
    uint64_t size = 0;           // bytes (0 for directories)
    uint64_t lastWriteTime = 0;  // Windows FILETIME (100ns since 1601-01-01)
    uint32_t attributes = 0;     // Win32 file attributes
    uint64_t fileId = 0;         // filesystem-unique file id (0 if unknown; MFT scanner will fill it)
    bool isDirectory = false;
};

} // namespace bv
