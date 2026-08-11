#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bv {

enum class Status : uint8_t {
    Identical,
    Missing,         // present only in source
    Extra,           // present only in destination
    SizeMismatch,    // same relative path, different size
    ContentMismatch, // same path+size, different content (Phase 3)
    ReadError,       // could not be read/enumerated (non-access error)
    AccessDenied,    // access denied while enumerating or reading
    ChangedDuringScan, // file was modified between enumeration and verification
};

struct FileResult {
    Status status = Status::Identical;
    std::wstring relativePath;
    uint64_t sizeSource = 0;
    uint64_t sizeDest = 0;
    std::wstring errorMessage; // for ReadError / AccessDenied / ChangedDuringScan
    bool isDirectory = false;

    // Digests captured during content verification (Content mode). Both are set
    // for ContentMismatch; on a read error only the verifying side may have one.
    // `hasHashX` is false when no digest was produced (kept empty).
    bool hasHashSource = false;
    bool hasHashDest = false;
    std::array<uint8_t, 32> hashSource{};
    std::array<uint8_t, 32> hashDest{};
};

struct Stats {
    uint64_t sourceFiles = 0;
    uint64_t sourceDirs = 0;
    uint64_t destFiles = 0;
    uint64_t destDirs = 0;

    uint64_t identicalFiles = 0;
    uint64_t identicalDirs = 0;
    uint64_t missingFiles = 0;
    uint64_t missingDirs = 0;
    uint64_t extraFiles = 0;
    uint64_t extraDirs = 0;
    uint64_t sizeMismatch = 0;
    uint64_t contentMismatch = 0;

    uint64_t readErrors = 0;
    uint64_t accessDenied = 0;
    uint64_t changedDuringScan = 0; // modified between enumeration and verification

    uint64_t bytesSource = 0; // sum of source file sizes
    uint64_t bytesDest = 0;   // sum of destination file sizes
};

// Result of a comparison. Only the non-identical entries are kept in `problems`
// to bound memory on million-file trees; identical entries are counted only.
struct ResultSet {
    Stats stats;
    std::vector<FileResult> problems;
};

} // namespace bv
