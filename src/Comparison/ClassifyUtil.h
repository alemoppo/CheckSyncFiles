#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Comparison/ComparisonResult.h"
#include "Comparison/ConcurrentSink.h"
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
// Thread-safe: callable from the enumeration workers, which share `sink`.
void ClassifyMatched(const FileEntry& src, const FileEntry& dst, ScanMode mode,
                     ConcurrentSink& sink, std::vector<ContentCandidate>& candidates);

} // namespace bv