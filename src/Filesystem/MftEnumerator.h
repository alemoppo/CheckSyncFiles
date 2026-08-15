#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "FileEnumerator.h"

namespace bv {

// NTFS $ATTRIBUTE_LIST (0x20) entry, as parsed by MftEnumerator. NTFS builds
// this list on a base record once ANY of its attributes overflow the 1 KB slot
// (e.g. a directory whose $INDEX_ROOT [$I30] lives in an extension record); each
// entry names one attribute and the record that actually holds it. Parsed with
// a strictly bounded scan -- a malformed tail stops the walk, never overreads.
struct MftAttrListEntry {
    uint32_t type;        // NTFS attribute type code (e.g. 0x90 $INDEX_ROOT)
    std::wstring name;    // attribute name ("" for unnamed attributes)
    uint64_t record;      // referenced MFT record number
    uint16_t sequence;    // referenced record sequence number
    int64_t lowestVcn;    // first VCN of a non-resident attribute
};

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

    // Test-only seams for the $ATTRIBUTE_LIST parser (the logic that resolves a
    // directory whose $I30 lives in an extension record). They operate on raw
    // bytes so the parser can be unit-tested deterministically without a live
    // volume; normal callers should not need them.
    static bool ParseAttributeListForTest(const std::vector<uint8_t>& data,
                                          std::vector<MftAttrListEntry>& out);
    static bool VcnRangeKnownForTest(
        const std::vector<std::pair<int64_t, int64_t>>& knownRanges,
        int64_t low, int64_t high);

    // Test-only seam that reconstructs a directory's children from raw on-disk
    // MFT record bytes, running the SAME production chain a real scan uses:
    // multi-sector fixup + ParseRecord -> Pass A base-record-reference merge ->
    // Pass B $ATTRIBUTE_LIST merge -> $I30 resolution -> child name resolution
    // (WIN32 $FILE_NAME). This is the full path that resolves a directory whose
    // $I30 ($INDEX_ROOT / $INDEX_ALLOCATION) lives in an extension record, so a
    // regression in any of those merge steps fails this test deterministically,
    // without volume access.
    //
    // `records` maps MFT record number -> raw on-disk record bytes (USA-fixup
    // protected, as read from the volume). `dirRec` is the base record number of
    // the directory. Out: emitted (relativePath, recordNumber) pairs for the
    // directory's children, and `outIncomplete` mirroring enumerate()'s honesty
    // rule (true when a $I30 child could not be resolved). No volume I/O is
    // performed, so fixture records must keep the directory's $I30 resident
    // ($INDEX_ROOT only, no $INDEX_ALLOCATION leaf data).
    static bool ResolveDirectoryForTest(
        const std::map<uint64_t, std::vector<uint8_t>>& records, uint64_t dirRec,
        std::vector<std::pair<std::wstring, uint64_t>>& outEntries,
        bool& outIncomplete);
};

} // namespace bv