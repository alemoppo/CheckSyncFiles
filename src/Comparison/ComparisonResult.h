#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "Comparison/ScanMode.h"

namespace bv {

enum class Status : uint8_t {
    Identical,
    Missing,         // present only in source
    Extra,           // present only in destination
    SizeMismatch,    // same relative path, different size
    ContentMismatch, // same path+size, different content (Phase 3)
    // Partial-verification outcomes (Content mode with percent < 100): NEVER
    // reuse Identical/ContentMismatch for these, so a future switch missing
    // the new cases fails loudly instead of misclassifying silently.
    // IdenticalPartial is a verdict value only (counted in
    // Stats::identicalPartialFiles, never stored in problems: see below).
    IdenticalPartial,      // no difference found in the sampled portions
    ContentMismatchPartial, // difference found in the sampled portions
    ReadError,       // could not be read/enumerated (non-access error)
    AccessDenied,    // access denied while enumerating or reading
    ChangedDuringScan, // file was modified between enumeration and verification
};

struct FileResult {
    Status status = Status::Identical;
    std::wstring relativePath;
    std::wstring fullPath;      // percorso assoluto (sourceRoot o destRoot + relativePath)
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

    // Partial verification actually applied to this file (Content mode with
    // percent < 100): the EFFECTIVE percent/pattern (see EffectiveLevel),
    // never the nominal user choice and never Random. Meaningful only for
    // ContentMismatchPartial rows (100/Edges otherwise).
    int verifiedPercent = 100;
    PartialPattern verifiedPattern = PartialPattern::Edges;
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
    uint64_t contentMismatch = 0; // full-content mismatches only

    // Partial-verification outcomes (Content mode with percent < 100):
    // identical-partial files are counted here, never stored in problems
    // (memory bound); partial mismatches ARE stored (they are differences).
    uint64_t identicalPartialFiles = 0;
    uint64_t contentMismatchPartial = 0;

    uint64_t readErrors = 0;
    uint64_t accessDenied = 0;
    uint64_t changedDuringScan = 0; // modified between enumeration and verification

    uint64_t bytesSource = 0; // sum of source file sizes
    uint64_t bytesDest = 0;   // sum of destination file sizes

    Stats& operator+=(const Stats& add) {
        sourceFiles += add.sourceFiles;
        sourceDirs += add.sourceDirs;
        destFiles += add.destFiles;
        destDirs += add.destDirs;
        identicalFiles += add.identicalFiles;
        identicalDirs += add.identicalDirs;
        missingFiles += add.missingFiles;
        missingDirs += add.missingDirs;
        extraFiles += add.extraFiles;
        extraDirs += add.extraDirs;
        sizeMismatch += add.sizeMismatch;
        contentMismatch += add.contentMismatch;
        identicalPartialFiles += add.identicalPartialFiles;
        contentMismatchPartial += add.contentMismatchPartial;
        readErrors += add.readErrors;
        accessDenied += add.accessDenied;
        changedDuringScan += add.changedDuringScan;
        bytesSource += add.bytesSource;
        bytesDest += add.bytesDest;
        return *this;
    }
};

// Run-level partial-verification record: what was requested vs effectively
// applied (see the run-level rules in ScanController::run). Stored in
// ScanReport, exported to JSON, and shown in the CLI/GUI run banners.
struct VerifyInfo {
    int percentRequested = 100;
    // Actually applied: < 100 only for a live-live Content compare without
    // snapshot capture (capture/index digests are always full).
    int percentEffective = 100;
    // Resolved pattern (never Random; Edges whenever the effective read is
    // full). Meaningful only when percentEffective < 100.
    PartialPattern pattern = PartialPattern::Edges;
    bool patternRandom = false; // the user chose Random (resolved once/run)
};

// Result of a comparison. Only the non-identical entries are kept in `problems`
// to bound memory on million-file trees; identical entries are counted only.
struct ResultSet {
    Stats stats;
    std::vector<FileResult> problems;
};

} // namespace bv
