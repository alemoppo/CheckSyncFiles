#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Comparison/ComparisonResult.h"
#include "Comparison/ConcurrentSink.h"
#include "Comparison/RowCapture.h"
#include "Comparison/ScanMode.h"
#include "Filesystem/FileEntry.h"

namespace bv {

// A same-relative-path + same-size file pair deferred to the content
// verification phase. Mtimes are captured at enumeration time and used for
// change detection and for the hash-cache key.
struct ContentCandidate {
    std::wstring relativePath;
    uint64_t sizeSource = 0;
    uint64_t sizeDest = 0;
    uint64_t srcMtime = 0; // Windows FILETIME at enumeration time
    uint64_t dstMtime = 0;
};

// Classifies a matched pair (same relative path present on both sides).
//
// In Content mode, same-size file pairs are appended to `candidates` (the
// caller owns the vector; the concurrent comparer guards its own instance);
// every other outcome updates `sink` stats and possibly appends a problem.
// Returns true when a content candidate was appended (and false otherwise), so
// the caller can count/drain pending hash work.
// Thread-safe: callable from the enumeration workers, which share `sink`.
// `rowSink` (optional, null by default) observes every FINALIZED row with the
// entries it was computed from (identicals included; content pairs are NOT
// final here -- they are observed later by the hash phase).
bool ClassifyMatched(const FileEntry& src, const FileEntry& dst, ScanMode mode,
                     ConcurrentSink& sink, std::vector<ContentCandidate>& candidates,
                     const std::wstring& destRoot, const std::wstring& sourceRoot,
                     IRowSink* rowSink = nullptr);

// Link-aware verdict for a matched pair where at least one side is a reparse
// point. Pure function (no sink): lets the serial and concurrent classifiers
// share the exact same link semantics while keeping their own stats/rows.
//
// - `handled=false`: neither side is a link; classify normally.
// - `handled=true, identical=true`: same supported link kind + equal targets
//   (`identicalIsDir` selects the identical files/dirs counter).
// - `handled=true, identical=false`: `row` carries the verdict
//   (SizeMismatch for kind changes, ContentMismatch for different targets
//   with sha256(target) digests, ReadError for unsupported/unreadable links).
struct LinkDecision {
    bool handled = false;
    bool identical = false;
    bool identicalIsDir = false;
    FileResult row;
};

LinkDecision TryClassifyLinks(const FileEntry& src, const FileEntry& dst,
                              const std::wstring& sourceRoot,
                              const std::wstring& destRoot);

} // namespace bv