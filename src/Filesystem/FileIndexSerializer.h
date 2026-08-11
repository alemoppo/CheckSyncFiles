#pragma once

#include <string>

#include "FileIndex.h"

namespace bv {
namespace indexio {

// Serializes a FileIndex to a compact binary snapshot (no JSON: for million-file
// trees the binary format is tens of MB, not hundreds-of-MB JSON text). The
// snapshot embeds the per-entry SHA-256 digests captured during a Content-mode
// capture, which is what makes offline content verification possible.
//
// Layout (all integers little-endian):
//   "BVSI", u32 version (1), u8 caseSensitive,
//   u32 sourceRootLen + UTF-8 source root,
//   u64 files, u64 dirs, u64 bytes, u64 count,
//   per entry: u64 pathLen + UTF-8 path, u64 size, u64 lastWriteTime,
//              u32 attributes, u64 fileId, u8 isDirectory,
//              u8 hasHash + 32 hash bytes (only when hasHash)
bool WriteSnapshot(const std::wstring& filePath, const FileIndex& index,
                   const std::wstring& sourceRoot, std::wstring& error);

// Loads a snapshot produced by WriteSnapshot. On success `index` is rebuilt
// with the same case policy and `sourceRootOut` receives the captured root.
// Returns false and fills `error` for missing/corrupt files.
bool ReadSnapshot(const std::wstring& filePath, FileIndex& index,
                  std::wstring& sourceRootOut, std::wstring& error);

} // namespace indexio
} // namespace bv