#include "MftEnumerator.h"

#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include <cstdio>
#include <cwchar>
#include <cstring>

#include "PathUtil.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

// Phase 4 NTFS MFT scan (v2).
//
// Reads the raw $MFT once (located via FSCTL_GET_NTFS_VOLUME_DATA at
// MftStartLcn*BytesPerCluster spanning MftValidDataLength, accessed through
// the $MFT data-run map because the MFT is usually fragmented) and
// reconstructs the subtree rooted at `root`.
//
// v2 reconstruction model (differs from v1, which trusted each record's
// $FILE_NAME parent pointer chain bottom-up):
//
//   * File references are treated as { record_number, sequence_number }. The
//     sequence is validated against the live record at every hop. A reference
//     whose sequence does not match the current record is a STALE reference
//     (the target record was deleted and reused) and is never trusted.
//
//   * Directory membership is walked TOP-DOWN from the root using each
//     directory's own $I30 index ($INDEX_ROOT inline entries + $INDEX_ALLOCATION
//     INDX blocks). The $I30 index is exactly the structure the filesystem
//     exposes to FindFirstFileW, so a child present in the directory index is,
//     by construction, the set Win32 enumeration reports. Rebuilding from the
//     index no longer depends on the child's parent pointer being intact: a
//     file whose $FILE_NAME parent reference is stale/broken is still found
//     because its directory index entry survives.
//
//   * Parent pointers are used as a redundant union source: a record whose
//     win32 $FILE_NAME references directory D (with matching sequence) is
//     emitted as a child of D even if D's index did not list it. Index entries
//     are authoritative; chain links only add.
//
//   * Every $FILE_NAME attribute is parsed. Namespace is respected: the
//     shell-visible path uses namespace 1 (WIN32), falling back to 3 (WIN32+DOS);
//     DOS 8.3 short names (namespace 2) and the POSIX internal name (0) are not
//     used for the output path. A record with several $FILE_NAME attributes in
//     different directories is a hard link: each (parent,name) pair becomes a
//     distinct output path (our FileIndex is one-entry-per-path).
//
//   * $ATTRIBUTE_LIST (0x20) is followed. When a base record overflows its 1KB
//     slot, NTFS moves attributes to extension records and records where each
//     one lives in the base record's $ATTRIBUTE_LIST. A directory's $I30 index
//     ($INDEX_ROOT / $INDEX_ALLOCATION) can therefore live entirely in an
//     extension record; ignoring the list would report such a directory as "no
//     readable $I30 index" on a perfectly valid volume. The list is parsed
//     (resident or non-resident), and the $I30 pieces it points to are merged
//     back into the base record; a base-record-reference merge is applied as an
//     order-independent fallback when the list cannot be read. $INDEX_ALLOCATION
//     split across several records is reassembled in VCN order. Non-$I30
//     attributes in the list (SI, FN, SD, $DATA, ...) are ignored: they are
//     either already parsed inline or irrelevant to tree reconstruction.
//
//   * Incompleteness is signalled honestly: if a child listed in a subtree
//     directory's $I30 index cannot be resolved (record out of range, not in
//     use, or sequence mismatch), or a directory's $INDEX_ALLOCATION exists but
//     cannot be read, enumerate() returns false and the caller must fall back
//     to Win32Enumerator. A partial index is never reported as a successful
//     scan.
//
// FileEntry.fileId is filled with the MFT record number. Requires an elevated
// process (raw volume access). Read-only.

namespace bv {

namespace {

constexpr uint32_t kAttrFileName = 0x30;   // NTFS_ATTR_FILENAME
constexpr uint32_t kAttrAttrList = 0x20;   // NTFS_ATTR_ATTRIBUTE_LIST
constexpr uint32_t kAttrData = 0x80;       // NTFS_ATTR_DATA
constexpr uint32_t kAttrIndexRoot = 0x90;  // NTFS_ATTR_INDEX_ROOT
constexpr uint32_t kAttrIndexAlloc = 0xA0; // NTFS_ATTR_INDEX_ALLOCATION
constexpr uint32_t kAttrEnd = 0xFFFFFFFF;
constexpr uint64_t kMftHighestSystem = 23; // low band of volume metafiles
constexpr uint32_t kIndxMagic = 0x58444E49u; // "INDX"
constexpr uint64_t kMaxAttrData = 256ull << 20;
// INDEX_BLOCK size is NOT a universal constant: it is the directory's own
// $INDEX_ROOT index_block_size (value+0x08). On every NTFS volume this
// project targets it is 4096 (1 cluster, 8 x 512-byte sectors), but the
// parser reads it from the metadata instead of assuming it. 4096 is kept
// only as a defensive fallback when the $INDEX_ROOT read is incomplete.
constexpr size_t kIndexBlockSizeDefault = 4096;
constexpr size_t kMinIndexBlockSize = 512;
constexpr size_t kMaxIndexBlockSize = 256 * 1024;

// Temporary debug aid: set BV_MFT_DEBUG=1 in the environment to trace every
// point where the MFT scan bails out and why.
bool MftDebug() {
    static const int flag = [] {
        char buf[8] = {0};
        return GetEnvironmentVariableA("BV_MFT_DEBUG", buf, 8) > 0 && buf[0] == '1';
    }();
    return flag != 0;
}
#define BVDBG(...)                            \
    do {                                      \
        if (MftDebug()) std::fprintf(stderr, __VA_ARGS__); \
    } while (0)

// Optional opt-in ground-truth diagnostics, never part of normal operation:
//   BV_MFT_DEBUG_FILE=<path>  appends one line
//                             "indxBlocks=<n> indxChildrenFromBlocks=<m>"
// to <path> at the end of every successful enumerate(). It exists so the
// elevated regression test can assert that $INDEX_ALLOCATION leaf blocks were
// really parsed and contributed directory children, i.e. that the scan did
// not silently fall back to parent-pointer-only reconstruction. No output at
// all unless the environment variable is set.
std::wstring MftDiagFilePath() {
    WCHAR buf[1024];
    const DWORD n = GetEnvironmentVariableW(L"BV_MFT_DEBUG_FILE", buf, 1024);
    if (n == 0 || n >= 1024) return std::wstring();
    return buf;
}

// ---------------------------------------------------------------------------
// NTFS primitive parsing
// ---------------------------------------------------------------------------

struct FileRef {
    uint64_t rec;   // record number   (bits 0..47 of the 64-bit reference)
    uint16_t seq;   // sequence number (bits 48..63)
};

FileRef SplitRef(uint64_t v) {
    return {v & 0xFFFFFFFFFFFFULL, static_cast<uint16_t>((v >> 48) & 0xFFFF)};
}

void EnableBackupPrivileges() {
    auto enable = [](LPCWSTR name) {
        HANDLE tok = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                              &tok)) {
            return;
        }
        LUID luid = {};
        if (LookupPrivilegeValueW(nullptr, name, &luid)) {
            TOKEN_PRIVILEGES tp = {};
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Luid = luid;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
        }
        CloseHandle(tok);
    };
    enable(SE_BACKUP_NAME);
    enable(SE_RESTORE_NAME);
}

// Resolve the multi-sector-fixup words of every 512-byte sector in the record.
bool ApplyFixup(uint8_t* rec, size_t bufSize) {
    if (bufSize < 48) return false;
    if (*reinterpret_cast<const uint32_t*>(rec) != 0x454C4946u) return false; // "FILE"
    const uint16_t usOffset = *reinterpret_cast<const uint16_t*>(rec + 4);
    const uint16_t usSize = *reinterpret_cast<const uint16_t*>(rec + 6);
    if (usOffset == 0 || usSize < 2 || usOffset + (size_t)usSize * 2u > bufSize) return false;
    const uint16_t recordSize = *reinterpret_cast<const uint16_t*>(rec + 28);
    if (recordSize < 48 || (size_t)recordSize > bufSize) return false;

    const uint16_t usn = *reinterpret_cast<const uint16_t*>(rec + usOffset);
    const uint32_t sectors = recordSize / 512u;
    if ((uint32_t)usSize < sectors + 1u) return false; // damaged record

    for (uint32_t i = 1; i <= sectors; ++i) {
        const size_t pos = (size_t)i * 512u - 2u;
        if (pos + 2 > bufSize) break;
        uint16_t* p = reinterpret_cast<uint16_t*>(rec + pos);
        if (*p == usn) {
            *p = *reinterpret_cast<const uint16_t*>(rec + usOffset + 2u * i);
        }
    }
    return true;
}

// NTFS INDEX_BLOCK (INDX) multi-sector fixup. INDX records use the same USA
// mechanism as FILE records -- the last 2 bytes of every 512-byte sector hold
// the update sequence number (USN) on disk and the true bytes live in the
// UPDATE_SEQUENCE_ARRAY at `usa_ofs` -- but the header layout is different
// (INDEX_RECORD_HEADER, not FILE_RECORD_HEADER), so this must NOT reuse
// ApplyFixup():
//
//   block+0x00 4  magic "INDX"
//   block+0x04 2  usa_ofs (offset of the USA array)
//   block+0x06 2  usa_count (words in the array: 1 USN + one per sector)
//   block+0x08 8  lsn
//   block+0x10 8  index block VCN
//   block+0x18    INDEX_HEADER ("node": entries_offset, index_length, ...)
//
// Returns false (corrupt block) if any check fails: INDX magic mismatch (the
// caller checks the same bytes), USA array out of the block bounds, missing
// USN+replacement words for every sector, or a sector tail that does not
// equal the USN. A corrupt INDEX_BLOCK must never be parsed: callers mark the
// scan incomplete (== Win32 fallback) instead of silently skipping it.
//
// NOTE: this low-level INDX fixup is intentionally duplicated in
// tools/mftdiag.cpp and tools/mftprobe.cpp (raw parsing for diagnostics);
// keep the three copies in sync.
bool UndoFixupIndexBlock(uint8_t* block, size_t blockSize, uint32_t bytesPerSector) {
    if (blockSize < 48 || bytesPerSector < 1 || bytesPerSector > blockSize) return false;
    if (blockSize % bytesPerSector != 0) return false;
    const uint16_t usOffset = *reinterpret_cast<const uint16_t*>(block + 4);
    const uint16_t usCount = *reinterpret_cast<const uint16_t*>(block + 6);
    if (usOffset == 0 || (size_t)usOffset + (size_t)usCount * 2u > blockSize) return false;
    const uint32_t sectors = blockSize / bytesPerSector;
    if ((uint32_t)usCount < sectors + 1u) return false; // need the USN + one per sector

    const uint16_t usn = *reinterpret_cast<const uint16_t*>(block + usOffset);
    for (uint32_t i = 1; i <= sectors; ++i) {
        const size_t pos = (size_t)i * bytesPerSector - 2u;
        if (pos + 2 > blockSize) return false;
        uint16_t* p = reinterpret_cast<uint16_t*>(block + pos);
        if (*p != usn) return false; // block not fixup-protected as expected
        *p = *reinterpret_cast<const uint16_t*>(block + usOffset + 2u * i);
    }
    return true;
}

bool ReadVolAt(HANDLE hVol, uint64_t abs, uint8_t* out, DWORD len) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)abs;
    DWORD got = 0;
    return SetFilePointerEx(hVol, li, nullptr, FILE_BEGIN) &&
           ReadFile(hVol, out, len, &got, nullptr) && got == len;
}

// A contiguous cluster run belonging to the $MFT data attribute.
struct MftRun {
    int64_t startVcn; // first VCN (clusters) of this run
    int64_t lcn;      // first LCN
    int64_t len;      // clusters
};

// Decode the data-run list of a non-resident attribute header `a`.
std::vector<MftRun> ParseDataRuns(const uint8_t* a, size_t aSize) {
    std::vector<MftRun> runs;
    const uint16_t mapOff = *reinterpret_cast<const uint16_t*>(a + 32);
    const int64_t lowVcn = *reinterpret_cast<const int64_t*>(a + 16);
    const int64_t highVcn = *reinterpret_cast<const int64_t*>(a + 24);
    if (mapOff == 0 || mapOff >= aSize) return runs;
    const uint8_t* r = a + mapOff;
    const uint8_t* const aEnd = a + aSize;
    int64_t vcn = lowVcn;
    int64_t lcn = 0;
    bool first = true;
    while (*r && r < aEnd) {
        uint8_t lenb = (*r) & 0x0F;
        uint8_t offb = (*r) >> 4;
        if (lenb == 0) break;
        ++r;
        if (lenb > 8 || offb > 8 || r + lenb + offb > aEnd) break;
        int64_t len = 0;
        for (int i = lenb - 1; i >= 0; --i) len = (len << 8) | r[i];
        int64_t lcnD = 0;
        for (int i = offb - 1; i >= 0; --i) lcnD = (lcnD << 8) | r[lenb + i];
        if (offb && (r[lenb + offb - 1] & 0x80)) lcnD -= (int64_t)1 << (8 * offb);
        r += lenb + offb;
        if (first) {
            lcn = lcnD;
            first = false;
        } else {
            lcn += lcnD;
        }
        runs.push_back({vcn, lcn, len});
        vcn += len;
        if (vcn > highVcn) break;
    }
    return runs;
}

// ---------------------------------------------------------------------------
// Per-record parsed data
// ---------------------------------------------------------------------------

struct NameInfo {
    FileRef parent;      // parent directory file reference (full, with seq)
    uint8_t ns = 0xFF;   // $FILE_NAME namespace
    uint64_t mtime = 0;  // modified time (FILETIME) from this $FILE_NAME
    std::wstring name;
};

struct IndexChild {
    FileRef ref;         // child file reference recorded in the directory index
    std::wstring name;   // key name (per-directory ground truth)
};

struct RecInfo {
    bool parsed = false;
    bool inUse = false;
    bool isDir = false;
    bool isReparse = false;
    uint16_t seq = 0;
    uint64_t mtime = 0;    // best-known last-write time
    uint64_t dataSize = 0; // $DATA logical size (authoritative)
    std::vector<NameInfo> names;      // every $FILE_NAME attribute
    std::vector<IndexChild> children; // $I30 children resolved so far
    std::vector<uint8_t> idxRoot;     // copy of the resident $INDEX_ROOT value
    // $INDEX_ALLOCATION ($I30): one non-resident attribute header copy per piece
    // -- the base record's inline piece plus, after $ATTRIBUTE_LIST following,
    // any pieces held by extension records. Read in VCN order at resolution time.
    std::vector<std::vector<uint8_t>> idxAllocHdrs;
    uint32_t idxBlockSize = 0;        // index_block_size from $INDEX_ROOT (value+0x08)
    bool indexResolved = false;       // $I30 fully resolved (root + allocation)
    // $ATTRIBUTE_LIST (0x20): resident value / non-resident header copy. The
    // list maps every attribute of a multi-record file to the record that
    // actually holds it (a directory's $I30 may live in an extension record).
    std::vector<uint8_t> attrList;
    std::vector<uint8_t> attrListHdr;
};

// `$FILE_NAME` namespace:
//   0 = POSIX, 1 = WIN32, 2 = DOS (8.3 short), 3 = WIN32+DOS.
const NameInfo* PickWin32Name(const RecInfo& r) {
    const NameInfo* pick = nullptr;
    for (const auto& n : r.names) {
        if (n.ns == 1) pick = &n; // last WIN32 name wins (hard links append)
    }
    if (!pick) {
        for (const auto& n : r.names) {
            if (n.ns == 3) { pick = &n; break; }
        }
    }
    if (!pick) {
        for (const auto& n : r.names) {
            if (n.ns != 0xFF) { pick = &n; break; }
        }
    }
    return pick;
}

// The name of `r` used when adding it as a child of directory `dirRef`.
//
// The name is taken from the record's own $FILE_NAME, NOT from the directory
// index key: NTFS stores BOTH the Win32 (long) name and the DOS 8.3 short name
// as separate $FILE_NAME attributes, and the $I30 index may key the same
// record under either. The sort order of the DOS key means the "wrong" entry
// can come first, so the index key is not a reliable display name. The
// record's matching attribute for `dirRef` with namespace WIN32 (1) is the
// name the shell shows.
std::wstring ChildNameOf(const RecInfo& r, FileRef dirRef) {
    std::wstring fallback;
    for (const auto& n : r.names) {
        if (n.parent.rec != dirRef.rec || n.parent.seq != dirRef.seq) continue;
        if (n.ns == 1) return n.name;
        fallback = n.name;
    }
    if (!fallback.empty()) return fallback;
    const NameInfo* p = PickWin32Name(r);
    return p ? p->name : std::wstring();
}

// Attribute name from a raw attribute header (resident or non-resident): the
// header's NameLength (a+9) counts characters, NameOffset (a+10) is relative to
// the header start. Returns an empty string for unnamed attributes.
std::wstring AttrNameOf(const uint8_t* a, uint32_t len) {
    const uint8_t nl = a[9];
    const uint16_t no = *reinterpret_cast<const uint16_t*>(a + 10);
    if (nl == 0) return std::wstring();
    if ((size_t)no + (size_t)nl * 2u > len) return std::wstring();
    const wchar_t* p = reinterpret_cast<const wchar_t*>(a + no);
    return std::wstring(p, p + nl);
}

// True when any header in `hdrs` already covers the VCN range of `h`: the same
// $INDEX_ALLOCATION piece must never be merged twice (dedupe for the two merge
// paths -- Pass A base-record-reference merge and Pass B attribute-list follow).
bool VcnRangeKnown(const std::vector<std::vector<uint8_t>>& hdrs,
                   const std::vector<uint8_t>& h) {
    if (h.size() < 56) return true;
    const int64_t low = *reinterpret_cast<const int64_t*>(h.data() + 16);
    const int64_t high = *reinterpret_cast<const int64_t*>(h.data() + 24);
    for (const auto& e : hdrs) {
        if (e.size() < 56) continue;
        const int64_t el = *reinterpret_cast<const int64_t*>(e.data() + 16);
        const int64_t eh = *reinterpret_cast<const int64_t*>(e.data() + 24);
        if (low <= eh && high >= el) return true;
    }
    return false;
}

// Parse one record (after fixup) into `out`.
void ParseRecord(const uint8_t* rec, size_t bufSize, RecInfo& out) {
    // Parse bounded, defensive: a malformed record may never cause reads past
    // the buffer. Every field access below is guarded either by the record
    // bounds (bufSize / end) or by the enclosing attribute's own length (len).
    if (bufSize < 48) return;
    const uint16_t attrOffset = *reinterpret_cast<const uint16_t*>(rec + 20);
    if (attrOffset < 48 || (size_t)attrOffset + 24 > bufSize) return;
    out.seq = *reinterpret_cast<const uint16_t*>(rec + 16);
    const uint16_t hdr = *reinterpret_cast<const uint16_t*>(rec + 22);
    out.inUse = (hdr & 1) != 0;
    out.isDir = (hdr & 2) != 0;
    out.isReparse = (hdr & 4) != 0;

    const uint8_t* a = rec + attrOffset;
    const uint8_t* const end = rec + bufSize;

    while (a + 16 <= end) {
        const uint32_t type = *reinterpret_cast<const uint32_t*>(a);
        if (type == kAttrEnd) break;
        const uint32_t len = *reinterpret_cast<const uint32_t*>(a + 4);
        // Resident attributes have a 24-byte header (valueLength@+16,
        // valueOffset@+20); non-resident ones at least 56 bytes. Requiring
        // len>=24 keeps the common header reads below in bounds.
        if (len < 24 || a + len > end) break;
        const bool nonResident = a[8] != 0;
        // Attribute values must lie inside [a, a+len), never spilling into the
        // following attribute; the value bounds below are checked against `len`.

        if (type == kAttrFileName && !nonResident) { // resident $FILE_NAME
            const uint16_t valueOff = *reinterpret_cast<const uint16_t*>(a + 20);
            const uint32_t valueLen = *reinterpret_cast<const uint32_t*>(a + 16);
            if (valueOff >= 24 && (size_t)valueOff + valueLen <= len) {
                const uint8_t* v = a + valueOff;
                // NAME fixed part is 66 bytes (parent file ref @0, mtime @0x10,
                // name length @64, namespace @65), then 2 bytes per character.
                if (valueLen >= 68) {
                    NameInfo n;
                    n.parent = SplitRef(*reinterpret_cast<const uint64_t*>(v));
                    n.mtime = *reinterpret_cast<const uint64_t*>(v + 16); // modified @0x10
                    n.ns = v[65];
                    const uint8_t nameLen = v[64]; // length in characters
                    if (nameLen > 0 && (size_t)nameLen * 2u <= valueLen - 66u) {
                        const wchar_t* p = reinterpret_cast<const wchar_t*>(v + 66);
                        n.name.assign(p, p + nameLen);
                    }
                    const uint64_t nameMtime = n.mtime;
                    out.names.push_back(std::move(n));
                    out.mtime = nameMtime; // best-known last-write (last wins, as v1)
                }
            }
        } else if (type == kAttrData && !nonResident) { // resident $DATA
            const uint32_t valueLen = *reinterpret_cast<const uint32_t*>(a + 16);
            out.dataSize = valueLen; // content length
        } else if (type == kAttrData && nonResident) { // non-resident $DATA
            if (len >= 56) { // real size lives at +0x30 (8 bytes)
                out.dataSize = *reinterpret_cast<const uint64_t*>(a + 48);
            }
        } else if (type == kAttrAttrList && !nonResident) { // resident $ATTRIBUTE_LIST
            const uint16_t valueOff = *reinterpret_cast<const uint16_t*>(a + 20);
            const uint32_t valueLen = *reinterpret_cast<const uint32_t*>(a + 16);
            if (valueOff >= 24 && (size_t)valueOff + valueLen <= len) {
                out.attrList.assign(a + valueOff, a + valueOff + valueLen);
            }
        } else if (type == kAttrAttrList && nonResident) { // non-resident $ATTRIBUTE_LIST
            // copy the whole attribute header (incl. the run map) because the
            // record buffer is recycled between records; the value is read from
            // the volume later, like $INDEX_ALLOCATION.
            const size_t copyLen = std::min<size_t>(len, 4096);
            out.attrListHdr.assign(a, a + copyLen);
        } else if (type == kAttrIndexRoot && !nonResident) {
            const std::wstring nm = AttrNameOf(a, len);
            if (nm.empty() || nm == L"$I30") { // directory $I30 index root
                const uint16_t valueOff = *reinterpret_cast<const uint16_t*>(a + 20);
                const uint32_t valueLen = *reinterpret_cast<const uint32_t*>(a + 16);
                if (valueOff >= 24 && (size_t)valueOff + valueLen <= len) {
                    out.idxRoot.assign(a + valueOff, a + valueOff + valueLen);
                    if (valueLen >= 16) {
                        // $INDEX_ROOT header: indexed_attr_type@+0, collation@+4,
                        // index_block_size@+8, clusters_per_index_block@+12.
                        out.idxBlockSize =
                            *reinterpret_cast<const uint32_t*>(a + valueOff + 8);
                    }
                }
            }
        } else if (type == kAttrIndexAlloc) {
            const std::wstring nm = AttrNameOf(a, len);
            if (nm.empty() || nm == L"$I30") { // directory $I30 allocation
                // copy the whole attribute header (incl. the run map) because the
                // record buffer is recycled between records. Stray (un-parented)
                // $INDEX_ALLOCATION attributes on the same record are harmless.
                const size_t copyLen = std::min<size_t>(len, 4096);
                out.idxAllocHdrs.push_back(std::vector<uint8_t>(a, a + copyLen));
            }
        }
        a += len;
    }
    out.parsed = true;
}

// ---------------------------------------------------------------------------
// $I30 index parsing
// ---------------------------------------------------------------------------

// Parse the entries of one INDEX_HEADER node (root node or a 4096 INDX block).
void ParseIndexNode(const uint8_t* node, const uint8_t* nodeEnd,
                    std::vector<IndexChild>& dst) {
    if (nodeEnd - node < 16) return;
    const uint32_t entriesOff = *reinterpret_cast<const uint32_t*>(node);
    const uint32_t indexLen = *reinterpret_cast<const uint32_t*>(node + 4);
    if (entriesOff == 0 || entriesOff >= indexLen) return;
    if (indexLen > (uint32_t)(nodeEnd - node)) return;
    const uint8_t* e = node + entriesOff;
    const uint8_t* const eEnd = node + indexLen;
    for (;;) {
        if (e + 16 > eEnd) break;
        const uint64_t childRef = *reinterpret_cast<const uint64_t*>(e);
        const uint16_t elen = *reinterpret_cast<const uint16_t*>(e + 8);
        const uint16_t klen = *reinterpret_cast<const uint16_t*>(e + 10);
        const uint16_t eflags = *reinterpret_cast<const uint16_t*>(e + 12);
        if (elen < 16 || e + elen > eEnd) break;
        if (eflags & 2) break; // end-of-node marker
        if (childRef != 0) {
            IndexChild ic;
            ic.ref = SplitRef(childRef);
            if (klen >= 66 && e + 16 + klen <= eEnd) {
                const uint8_t* key = e + 16;
                const uint8_t nl = key[64];
                if (66u + (size_t)nl * 2u <= klen) {
                    const wchar_t* p = reinterpret_cast<const wchar_t*>(key + 66);
                    ic.name.assign(p, p + nl);
                }
            }
            dst.push_back(std::move(ic));
        }
        e += elen;
    }
}

// Parse the resident $INDEX_ROOT value into `dst`.
void ParseIndexRootValue(const std::vector<uint8_t>& v, std::vector<IndexChild>& dst) {
    const uint8_t* node = v.data() + 16;
    ParseIndexNode(node, v.data() + v.size(), dst);
}

// Parse the whole $INDEX_ALLOCATION stream of a directory. The stream is
// `data.size()` bytes made of INDEX_BLOCK records, each `blockSize` bytes
// wide. blockSize comes from the directory's own $INDEX_ROOT
// (index_block_size at value+0x08); the caller supplies it. On NTFS it is
// almost always 4096 (1 cluster of 8 x 512-byte sectors), but it is read from
// metadata, not assumed.
//
// Every INDX block carries a USA (multi-sector) fixup that must be undone
// BEFORE parsing (UndoFixupIndexBlock). The node/INDEX_HEADER of a block sits
// at block+0x18 -- right after the 24-byte INDEX_RECORD_HEADER -- NOT at
// block+0x10, where the block VCN field lives (reading the VCN as the node
// yields entries_offset 0/1 and silently parses nothing).
//
// DIFFERENT FROM $INDEX_ROOT: a resident $INDEX_ROOT value needs no separate
// fixup, because it lives inside the FILE record whose own ApplyFixup() has
// already restored its sector tails; and its node sits at value+0x10 (16-byte
// INDEX_ROOT header: indexed_attr_type, collation, index_block_size,
// clusters_per_index_block), not +0x18.
//
// A stream may legitimately contain UNUSED blocks: NTFS sizes $INDEX_ALLOCATION
// in cluster multiples and leaves free blocks (no INDX magic, zeroed) interleaved
// or at the tail for future growth. Those are skipped, NOT treated as corruption
// (every large directory index has this slack). A block that carries INDX magic
// but fails the USA fixup, however, is a genuinely corrupt USED block: parsing
// it would read overwritten sector tails as index data.
//
// Returns SIZE_MAX when a fixup fails on an INDX block (corrupt: caller marks
// the scan incomplete), else the number of INDX blocks parsed.
size_t ParseIndexAllocationData(std::vector<uint8_t>& data, size_t blockSize,
                                uint32_t bytesPerSector, std::vector<IndexChild>& dst) {
    if (blockSize < kMinIndexBlockSize || blockSize > kMaxIndexBlockSize) return SIZE_MAX;
    size_t blocks = 0;
    for (size_t off = 0; off + blockSize <= data.size(); off += blockSize) {
        uint8_t* b = data.data() + off;
        if (*reinterpret_cast<const uint32_t*>(b) != kIndxMagic) continue; // unused/free block
        if (!UndoFixupIndexBlock(b, blockSize, bytesPerSector)) return SIZE_MAX;
        ParseIndexNode(b + 24, b + blockSize, dst); // INDEX_HEADER at block+0x18
        ++blocks;
    }
    return blocks;
}

// Read the clusters of a non-resident attribute's data runs into `out` starting
// at byte offset `dstOff`. `out` must already be sized for the whole stream.
bool ReadNonResidentAttrInto(HANDLE hVol, uint64_t cluster,
                             const std::vector<uint8_t>& hdrCopy,
                             std::vector<uint8_t>& out, size_t dstOff) {
    if (hdrCopy.size() < 56) return false;
    const uint16_t mapOff = *reinterpret_cast<const uint16_t*>(hdrCopy.data() + 32);
    const int64_t lowVcn = *reinterpret_cast<const int64_t*>(hdrCopy.data() + 16);
    const int64_t highVcn = *reinterpret_cast<const int64_t*>(hdrCopy.data() + 24);
    if (mapOff == 0 || mapOff >= hdrCopy.size() || highVcn < lowVcn) return false;
    const uint8_t* r = hdrCopy.data() + mapOff;
    const uint8_t* const rEnd = hdrCopy.data() + hdrCopy.size();
    int64_t vcn = lowVcn;
    int64_t lcn = 0;
    bool first = true;
    while (*r && r < rEnd) {
        const uint8_t lenb = (*r) & 0x0F;
        const uint8_t offb = (*r) >> 4;
        if (lenb == 0) break;
        ++r;
        if (lenb > 8 || offb > 8 || r + lenb + offb > rEnd) break;
        int64_t len = 0;
        for (int i = lenb - 1; i >= 0; --i) len = (len << 8) | r[i];
        int64_t lcnD = 0;
        for (int i = offb - 1; i >= 0; --i) lcnD = (lcnD << 8) | r[lenb + i];
        if (offb && (r[lenb + offb - 1] & 0x80)) lcnD -= (int64_t)1 << (8 * offb);
        r += lenb + offb;
        if (first) { lcn = lcnD; first = false; } else { lcn += lcnD; }
        for (int64_t k = 0; k < len; ++k) {
            const size_t byteOff = dstOff + (size_t)(vcn - lowVcn) * cluster;
            if (byteOff + cluster <= out.size()) {
                ReadVolAt(hVol, (uint64_t)(lcn + k) * cluster, out.data() + byteOff,
                          (DWORD)cluster);
            }
            ++vcn;
        }
    }
    return true;
}

// Read the full data of a single non-resident attribute from its stored header
// copy (used for $ATTRIBUTE_LIST, which is one whole attribute).
bool ReadNonResidentAttr(HANDLE hVol, uint64_t cluster, const std::vector<uint8_t>& hdrCopy,
                         std::vector<uint8_t>& out) {
    if (hdrCopy.size() < 56) return false;
    const int64_t lowVcn = *reinterpret_cast<const int64_t*>(hdrCopy.data() + 16);
    const int64_t highVcn = *reinterpret_cast<const int64_t*>(hdrCopy.data() + 24);
    const uint64_t dataSize = *reinterpret_cast<const uint64_t*>(hdrCopy.data() + 48);
    if (dataSize == 0 || dataSize > kMaxAttrData || highVcn < lowVcn) return false;
    const uint64_t totalBytes = (uint64_t)(highVcn - lowVcn + 1) * cluster;
    if (totalBytes > kMaxAttrData || totalBytes < dataSize) return false;
    std::vector<uint8_t> tmp((size_t)totalBytes);
    if (!ReadNonResidentAttrInto(hVol, cluster, hdrCopy, tmp, 0)) return false;
    if (tmp.size() > dataSize) tmp.resize((size_t)dataSize);
    out.swap(tmp);
    return !out.empty();
}

// Rebuild the full $INDEX_ALLOCATION ($I30) stream of a directory from one or
// more non-resident attribute header copies -- the base record's inline piece
// plus any pieces held by extension records ($ATTRIBUTE_LIST redirection). The
// pieces are merged by VCN: each header covers [lowVcn..highVcn] and its bytes
// are written into the shared buffer at that VCN position, so a stream split
// across records is reassembled exactly in the order NTFS stored it. Returns
// false when a piece cannot be read (caller marks the scan incomplete).
bool ReadIndexAllocationStream(HANDLE hVol, uint64_t cluster,
                               const std::vector<std::vector<uint8_t>>& hdrs,
                               std::vector<uint8_t>& out) {
    if (hdrs.empty()) return false;
    struct Piece {
        int64_t low;
        int64_t high;
        const std::vector<uint8_t>* h;
    };
    std::vector<Piece> pieces;
    pieces.reserve(hdrs.size());
    for (const auto& h : hdrs) {
        if (h.size() < 56) return false;
        const int64_t low = *reinterpret_cast<const int64_t*>(h.data() + 16);
        const int64_t high = *reinterpret_cast<const int64_t*>(h.data() + 24);
        if (high < low || low < 0) return false;
        pieces.push_back({low, high, &h});
    }
    std::sort(pieces.begin(), pieces.end(),
              [](const Piece& x, const Piece& y) { return x.low < y.low; });
    const int64_t minVcn = pieces.front().low;
    const int64_t maxVcn = pieces.back().high;
    const uint64_t spanBytes = (uint64_t)(maxVcn - minVcn + 1) * cluster;
    if (spanBytes > kMaxAttrData) return false;
    std::vector<uint8_t> tmp((size_t)spanBytes);
    for (const auto& pc : pieces) {
        const size_t dstOff = (size_t)(pc.low - minVcn) * cluster;
        if (!ReadNonResidentAttrInto(hVol, cluster, *pc.h, tmp, dstOff)) return false;
    }
    // The real (valid-data) size is stored in the piece that starts at VCN 0.
    uint64_t realSize = *reinterpret_cast<const uint64_t*>(pieces.front().h->data() + 48);
    if (realSize > tmp.size()) realSize = tmp.size();
    if (realSize < tmp.size()) tmp.resize((size_t)realSize);
    out.swap(tmp);
    return true;
}

// Parse the entries of a raw $ATTRIBUTE_LIST (0x20) value -- the bytes of a
// resident attribute value or the read data of a non-resident one. Entry layout:
//
//   +0x00 4 type (attribute type code; 0 = terminator)
//   +0x04 2 length of this entry
//   +0x06 1 attribute-name length, in bytes
//   +0x07 1 attribute-name offset
//   +0x08 8 lowest VCN (non-resident attributes)
//   +0x10 8 file reference (record | sequence<<48) of the owning record
//   +0x18 2 instance
//
// Strictly bounded: an entry whose length is <26 or runs past the end of the
// buffer stops the walk (the remaining tail is not guessed at), as does the
// terminator (type==0 / length==0). The scan never reads outside `data`.
bool ParseAttrListData(const uint8_t* p, size_t n, std::vector<MftAttrListEntry>& out) {
    const uint8_t* const end = p + n;
    while (p + 26 <= end) {
        const uint32_t type = *reinterpret_cast<const uint32_t*>(p);
        const uint16_t len = *reinterpret_cast<const uint16_t*>(p + 4);
        if (type == 0 || len == 0) break;
        if (len < 26 || p + len > end) break; // malformed tail: stop, do not guess
        MftAttrListEntry e;
        e.type = type;
        const uint8_t nameLen = p[6];
        const uint8_t nameOff = p[7];
        e.lowestVcn = *reinterpret_cast<const int64_t*>(p + 8);
        const FileRef where = SplitRef(*reinterpret_cast<const uint64_t*>(p + 16));
        e.record = where.rec;
        e.sequence = where.seq;
        if (nameLen && nameLen % 2 == 0 && (size_t)nameOff + nameLen <= len) {
            const wchar_t* w = reinterpret_cast<const wchar_t*>(p + nameOff);
            e.name.assign(w, w + nameLen / 2);
        }
        out.push_back(std::move(e));
        p += len;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// The enumerator
// ---------------------------------------------------------------------------

bool MftEnumerator::IsSupported(const std::wstring& root) {
    const std::wstring norm = pathutil::NormalizeRoot(root);
    if (norm.size() < 3 || norm[1] != L':' || norm[0] == L'\\' || norm[0] == L'/') return false;
    const std::wstring drive = std::wstring(1, norm[0]) + L":\\";
    WCHAR fsName[48] = {0};
    if (!GetVolumeInformationW(drive.c_str(), nullptr, 0, nullptr, nullptr, nullptr,
                               fsName, 48)) {
        return false;
    }
    return _wcsicmp(fsName, L"NTFS") == 0;
}

bool MftEnumerator::enumerate(const std::wstring& root,
                              const EntryCallback& onEntry,
                              const ErrorCallback& onError,
                              const ProgressCallback& onProgress,
                              const std::atomic_bool* cancel) {
    const std::wstring normRoot = pathutil::NormalizeRoot(root);
    if (normRoot.size() < 3 || normRoot[1] != L':' || normRoot[0] == L'\\' ||
        normRoot[0] == L'/') {
        BVDBG("mft[1] root path unsupported\\n");
        return false; // caller falls back to Win32
    }
    if (!IsSupported(normRoot)) { BVDBG("mft[1b] not NTFS\\n"); return false; }

    EnableBackupPrivileges();
    const std::wstring drive = std::wstring(1, normRoot[0]) + L":\\";
    const std::wstring devVol = L"\\\\.\\" + drive.substr(0, 2); // e.g. "\\\\.\\C:"

    // Root directory file reference (record + sequence) from Win32.
    HANDLE hRoot = CreateFileW(pathutil::AddLongPathPrefix(normRoot).c_str(), 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (hRoot == INVALID_HANDLE_VALUE) { BVDBG("mft[2] cannot open root\\n"); return false; }
    uint64_t rootRefWin = 0;
    {
        FILE_ID_INFO fid = {};
        if (!GetFileInformationByHandleEx(hRoot, FileIdInfo, &fid, sizeof(fid))) {
            CloseHandle(hRoot);
            BVDBG("mft[3] FileIdInfo failed\\n");
            return false;
        }
        std::memcpy(&rootRefWin, fid.FileId.Identifier, sizeof(uint64_t));
    }
    CloseHandle(hRoot);
    const FileRef rootRef = SplitRef(rootRefWin);

    // Open the volume device and read the NTFS geometry.
    HANDLE hVol = CreateFileW(devVol.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE) { BVDBG("mft[4] cannot open volume\\n"); return false; }
    NTFS_VOLUME_DATA_BUFFER vd = {};
    DWORD ret = 0;
    if (!DeviceIoControl(hVol, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &vd, sizeof(vd),
                         &ret, nullptr)) {
        CloseHandle(hVol);
        BVDBG("mft[5] NTFS_VOLUME_DATA failed\\n");
        return false;
    }
const uint64_t segSize = vd.BytesPerFileRecordSegment;
    const uint64_t cluster = vd.BytesPerCluster;
    const uint32_t bytesPerSector = vd.BytesPerSector;
    const uint64_t mftStartBytes =
        static_cast<uint64_t>(vd.MftStartLcn.QuadPart) * vd.BytesPerCluster;
    const uint64_t mftBytes = static_cast<uint64_t>(vd.MftValidDataLength.QuadPart);
    if (segSize == 0 || mftBytes == 0 || cluster == 0 || bytesPerSector == 0) {
        CloseHandle(hVol);
        BVDBG("mft[6] bad geometry seg=%llu mft=%llu clus=%llu\n",(unsigned long long)segSize,(unsigned long long)mftBytes,(unsigned long long)cluster);
        return false;
    }
    // Force pending NTFS metadata to disk before the raw read; otherwise a
    // freshly-created tree may be read back in a partially-flushed state.
    FlushFileBuffers(hVol);
    const uint64_t nRecords = mftBytes / segSize;

    // ---- $MFT data-run map from record 0 (fragmentation-aware) -------------
    std::vector<uint8_t> rec0((size_t)segSize);
    if (!ReadVolAt(hVol, mftStartBytes, rec0.data(), (DWORD)segSize) ||
        !ApplyFixup(rec0.data(), rec0.size())) {
        CloseHandle(hVol);
        BVDBG("mft[7] record 0 read/fixup failed\\n");
        return false;
    }
    std::vector<MftRun> runs;
    {
        const uint16_t first = *reinterpret_cast<const uint16_t*>(rec0.data() + 20);
        const uint8_t* a = rec0.data() + first;
        const uint8_t* const end = rec0.data() + segSize;
        while (a + 24 <= end) {
            const uint32_t type = *reinterpret_cast<const uint32_t*>(a);
            if (type == kAttrEnd) break;
            const uint32_t len = *reinterpret_cast<const uint32_t*>(a + 4);
            if (len < 24 || a + len > end) break;
            if (type == kAttrData && a[8] != 0) {
                runs = ParseDataRuns(a, (size_t)(end - a)); // first $DATA = $MFT itself
                break;
            }
            a += len;
        }
    }
    if (runs.empty()) {
        CloseHandle(hVol);
        BVDBG("mft[8] $MFT run map empty\\n");
        return false;
    }
    std::sort(runs.begin(), runs.end(),
              [](const MftRun& x, const MftRun& y) { return x.startVcn < y.startVcn; });

    // Iterate every MFT record, in record order, reading each run's physical
    // clusters. `cb` receives (recordIndex, recordBytes, recordSize).
    constexpr DWORD kMaxBuf = 8 * 1024 * 1024;
    std::vector<uint8_t> buf(kMaxBuf);
    const auto cancelledNow = [&]() {
        return cancel && cancel->load(std::memory_order_relaxed);
    };
    const auto foreachRecord = [&](const auto& cb) {
        for (const auto& run : runs) {
            const uint64_t runStartMft = (uint64_t)run.startVcn * cluster; // MFT-relative byte
            const uint64_t runBytes = (uint64_t)run.len * cluster;
            uint64_t within = 0;
            while (within < runBytes) {
                if (cancelledNow()) return; // long raw sweep: poll the cancel flag
                const DWORD want = (DWORD)std::min<uint64_t>(kMaxBuf, runBytes - within);
                LARGE_INTEGER li;
                li.QuadPart = (LONGLONG)((uint64_t)run.lcn * cluster + within);
                DWORD got = 0;
                if (!SetFilePointerEx(hVol, li, nullptr, FILE_BEGIN) ||
                    !ReadFile(hVol, buf.data(), want, &got, nullptr) || got == 0) break;
                const uint64_t baseRec = (runStartMft + within) / segSize;
                const size_t nRecs = got / segSize;
                for (size_t i = 0; i < nRecs; ++i) {
                    const uint64_t recIndex = baseRec + i;
                    if (recIndex >= nRecords) break;
                    cb(recIndex, buf.data() + i * segSize, (size_t)segSize);
                }
                within += got;
            }
        }
    };

    // ---- Pass A: parse every record ----------------------------------------
    std::vector<RecInfo> recs(nRecords);
    // Opt-in ground-truth counter: directories whose $I30 was reassembled from
    // extension records (via base-record reference or $ATTRIBUTE_LIST). Reported
    // through BV_MFT_DEBUG_FILE; see the regression test.
    size_t diagExtI30Merged = 0;
    foreachRecord([&](uint64_t recIndex, uint8_t* rec, size_t n) {
        RecInfo& r = recs[recIndex];
        if (!ApplyFixup(rec, n)) return;
        ParseRecord(rec, n, r);
        // An extension record (base record reference @+32 != 0) carries
        // attributes that belong to the base record. Merge its $I30 pieces now:
        // the base record always has a lower record number, so it was already
        // parsed, and the sequence check rejects references to a reused record.
        const FileRef baseRef = SplitRef(*reinterpret_cast<const uint64_t*>(rec + 32));
        if (baseRef.rec != 0 && baseRef.rec != recIndex && baseRef.rec < nRecords) {
            RecInfo& base = recs[baseRef.rec];
            if (base.parsed && base.inUse && base.seq == baseRef.seq) {
                if (base.idxRoot.empty() && !r.idxRoot.empty()) {
                    base.idxRoot = r.idxRoot;
                    base.idxBlockSize = r.idxBlockSize;
                    ++diagExtI30Merged;
                }
                for (const auto& h : r.idxAllocHdrs) {
                    if (VcnRangeKnown(base.idxAllocHdrs, h)) continue;
                    base.idxAllocHdrs.push_back(h);
                    ++diagExtI30Merged;
                }
            }
        }
    });
    if (cancelledNow()) {
        CloseHandle(hVol);
        return true; // aborted during the raw MFT sweep
    }

    // ---- Pass B: follow $ATTRIBUTE_LIST (0x20) ------------------------------
    // A record whose attributes overflowed its 1KB slot lists them here, each
    // entry pointing at the record that actually holds the attribute. NTFS
    // requires the base record to carry an attribute list once ANY attribute
    // moved to an extension record. A directory's $INDEX_ROOT /
    // $INDEX_ALLOCATION ($I30) may live entirely in such an extension record,
    // which is exactly what makes the parser report "directory has no readable
    // $I30 index" on a perfectly valid volume. Reassemble the $I30 pieces into
    // the base record's RecInfo here (the Pass A base-record-reference merge is
    // the order-independent fallback when the list itself is unreadable).
    // Non-$I30 attributes (SI, FN, SD, $DATA, ...) are deliberately ignored:
    // they are either parsed inline already or irrelevant to tree reconstruction.
    for (uint64_t i = 0; i < nRecords; ++i) {
        if (cancelledNow()) {
            CloseHandle(hVol);
            return true;
        }
        RecInfo& r = recs[i];
        if (!r.parsed || !r.inUse) continue;
        if (r.attrList.empty() && r.attrListHdr.empty()) continue;

        std::vector<uint8_t> listData;
        if (!r.attrList.empty()) {
            listData = r.attrList;
        } else if (!ReadNonResidentAttr(hVol, cluster, r.attrListHdr, listData)) {
            continue; // cannot follow: the $I30 checks in the walk stay honest
        }
        std::vector<MftAttrListEntry> entries;
        ParseAttrListData(listData.data(), listData.size(), entries);
        for (const auto& e : entries) {
            const bool isI30 = e.name.empty() || e.name == L"$I30";
            if ((e.type == kAttrIndexRoot || e.type == kAttrIndexAlloc) && isI30 &&
                e.record != i && e.record < nRecords && recs[e.record].parsed &&
                recs[e.record].inUse && recs[e.record].seq == e.sequence) {
                const RecInfo& src = recs[e.record];
                if (e.type == kAttrIndexRoot) {
                    if (r.idxRoot.empty() && !src.idxRoot.empty()) {
                        r.idxRoot = src.idxRoot;
                        r.idxBlockSize = src.idxBlockSize;
                        ++diagExtI30Merged;
                    }
                } else if (!src.idxAllocHdrs.empty()) {
                    for (const auto& h : src.idxAllocHdrs) {
                        if (VcnRangeKnown(r.idxAllocHdrs, h)) continue;
                        r.idxAllocHdrs.push_back(h);
                        ++diagExtI30Merged;
                    }
                }
            }
        }
    }

    bool incomplete = false;
    // Reports an unreadable directory; `dirPath` is the path (relative to the
    // scan root) of the directory whose $I30 index or child could not be
    // resolved, so the caller can show which subtree the scan missed.
    auto failIncomplete = [&](const std::wstring& dirPath, const wchar_t* why,
                              uint64_t rec) {
        incomplete = true;
        if (onError) {
            ScanError e;
            e.path = dirPath;
            e.message = std::wstring(L"MFT scan incomplete: ") + why + L" (record " +
                        std::to_wstring(rec) + L")";
            e.winError = ERROR_INVALID_DATA;
            onError(e);
        }
    };

    // ---- Root structural validation ----------------------------------------
    if (rootRef.rec >= nRecords || !recs[rootRef.rec].inUse ||
        recs[rootRef.rec].seq != rootRef.seq) {
        BVDBG("mft[9] root invalid rec=%llu n=%llu inUse=%d seq=%u liveSeq=%u\\n",
              (unsigned long long)rootRef.rec, (unsigned long long)nRecords,
              rootRef.rec < nRecords ? (recs[rootRef.rec].inUse ? 1 : 0) : -1,
              (unsigned)rootRef.seq,
              rootRef.rec < nRecords ? (unsigned)recs[rootRef.rec].seq : 0);
        CloseHandle(hVol);
        return false; // the recorded root does not exist as such in the MFT
    }
    const RecInfo& rootInfo = recs[rootRef.rec];
    if (!rootInfo.isDir) {
        BVDBG("mft[10] root is not a directory\\n");
        CloseHandle(hVol);
        return false;
    }
    // NTFS structural root note: the volume root is record 5 and its own
    // $FILE_NAME parent pointer refers to itself -- that is the structural
    // invariant by which a volume root is recognisable without trusting a
    // hard-coded record number. We locate the scan root via the Win32 file id
    // (source of truth) and verify the invariant when the scan root happens to
    // be the volume root. Any other directory root is a normal directory and
    // carries no self-parent.
    if (rootRef.rec == 5) {
        bool selfParent = false;
        for (const auto& nm : rootInfo.names) {
            if (nm.parent.rec == rootRef.rec && nm.parent.seq == rootRef.seq) selfParent = true;
        }
        BVDBG("mft[10b] volume root record 5: self-parent=%d\\n", selfParent ? 1 : 0);
        if (!selfParent) {
            CloseHandle(hVol);
            return false;
        }
    }

    // ---- Reverse index: record number -> children (for the chain union) ----
    // Each entry links a record whose win32 $FILE_NAME points to that parent.
    std::unordered_map<uint64_t, std::vector<uint32_t>> revChildren;
    revChildren.reserve(nRecords);
    for (uint64_t i = 0; i < nRecords; ++i) {
        if (cancelledNow()) {
            CloseHandle(hVol);
            return true;
        }
        const RecInfo& r = recs[i];
        if (!r.parsed || !r.inUse || r.names.empty()) continue;
        for (const auto& nm : r.names) {
            if (nm.ns != 0xFF) revChildren[nm.parent.rec].push_back((uint32_t)i);
        }
    }

    // ---- Top-down walk from the root ---------------------------------------
    struct ChildEntry {
        std::wstring name;
        bool viaIndex;
    };

    std::vector<std::pair<uint64_t, std::wstring>> work;
    work.push_back({rootRef.rec, L""});

    uint64_t files = 0;
    uint64_t dirs = 0;
    // Opt-in ground-truth counters (see MftDiagFilePath below).
    size_t diagIndexBlocks = 0;
    size_t diagIndexChildren = 0;

    const auto isMetafile = [](uint64_t recNo, const std::wstring& nm) {
        return recNo <= kMftHighestSystem && !nm.empty() && nm[0] == L'$';
    };

    while (!work.empty()) {
        if (cancelledNow()) {
            CloseHandle(hVol);
            return true;
        }
        const auto top = work.back();
        work.pop_back();
        const uint64_t dirRec = top.first;
        const std::wstring dirRel = top.second;

        RecInfo& d = recs[dirRec];

        // Resolve the directory's full $I30 (root entries + allocation leaf
        // blocks). The resident root entries are parsed here (not in Pass A) so
        // that an $INDEX_ROOT merged back from an extension record via
        // $ATTRIBUTE_LIST is already present when the node is walked.
        if (!d.indexResolved) {
            d.indexResolved = true;
            if (!d.idxRoot.empty()) {
                ParseIndexRootValue(d.idxRoot, d.children);
            }
            if (!d.idxAllocHdrs.empty()) {
                std::vector<uint8_t> data;
                if (!ReadIndexAllocationStream(hVol, cluster, d.idxAllocHdrs, data)) {
                    failIncomplete(dirRel, L"directory $INDEX_ALLOCATION unreadable",
                                   dirRec);
                } else {
                    const size_t blk = (d.idxBlockSize >= kMinIndexBlockSize &&
                                        d.idxBlockSize <= kMaxIndexBlockSize)
                                           ? d.idxBlockSize
                                           : kIndexBlockSizeDefault;
                    const size_t parsed =
                        ParseIndexAllocationData(data, blk, bytesPerSector, d.children);
                    if (parsed == SIZE_MAX) {
                        failIncomplete(dirRel, L"directory $INDEX_ALLOCATION block corrupt",
                                       dirRec);
                    } else {
                        diagIndexBlocks += parsed;
                    }
                }
            }
            if (d.children.empty() && d.idxRoot.empty()) {
                failIncomplete(dirRel, L"directory has no readable $I30 index", dirRec);
            }
        }

        // Collect this directory's children: index entries first (authoritative),
        // then parent-pointer union (redundant source).
        std::unordered_map<uint64_t, ChildEntry> kids;
        for (const auto& c : d.children) {
            const uint64_t cr = c.ref.rec;
            if (cr >= nRecords || !recs[cr].parsed || !recs[cr].inUse ||
                recs[cr].seq != c.ref.seq) {
                failIncomplete(dirRel, L"index child record missing or sequence mismatch",
                               cr);
                continue;
            }
            // Prefer the record's WIN32 name for this parent (the index key can
            // be the DOS 8.3 short name and is not a reliable display name).
            const std::wstring recName = ChildNameOf(recs[cr], {dirRec, d.seq});
            kids.emplace(cr, ChildEntry{recName.empty() ? c.name : recName, true});
            ++diagIndexChildren;
        }
        if (auto it = revChildren.find(dirRec); it != revChildren.end()) {
            for (uint32_t cr : it->second) {
                RecInfo& cRec = recs[cr];
                if (!cRec.parsed || !cRec.inUse) continue;
                // stale parent references (reused directory record) are caught
                // by the sequence check: only links carrying this directory's
                // live sequence are trusted.
                bool linkOk = false;
                for (const auto& nm : cRec.names) {
                    if (nm.parent.rec == dirRec && nm.parent.seq == d.seq) linkOk = true;
                }
                if (!linkOk) continue;
                if (kids.count(cr)) continue;
                kids.emplace(cr, ChildEntry{ChildNameOf(cRec, {dirRec, d.seq}), false});
            }
        }

        // Emit children in alphabetical order (depth-first), skipping the
        // volume metafile band ($MFT, $Boot, ...).
        std::vector<std::pair<uint64_t, ChildEntry>> ordered(kids.begin(), kids.end());
        std::sort(ordered.begin(), ordered.end(),
                  [](const std::pair<uint64_t, ChildEntry>& a,
                     const std::pair<uint64_t, ChildEntry>& b) { return a.second.name < b.second.name; });

        for (size_t k = ordered.size(); k-- > 0;) {
            const uint64_t cr = ordered[k].first;
            const std::wstring& cname = ordered[k].second.name;
            if (isMetafile(cr, cname)) continue;
            const RecInfo& cRec = recs[cr];

            const std::wstring childRel = pathutil::JoinRel(dirRel, cname);

            FileEntry e;
            e.relativePath = childRel;
            e.size = cRec.isDir ? 0 : (cRec.dataSize ? cRec.dataSize : 0);
            e.lastWriteTime = cRec.mtime;
            e.fileId = cr;
            e.attributes = (cRec.isDir ? FILE_ATTRIBUTE_DIRECTORY : 0) |
                           (cRec.isReparse ? FILE_ATTRIBUTE_REPARSE_POINT : 0);
            e.isDirectory = cRec.isDir;
            if (cRec.isDir) {
                ++dirs;
            } else {
                ++files;
            }
            if (!onEntry(std::move(e))) {
                CloseHandle(hVol);
                return true; // consumer aborted
            }

            if (cRec.isDir && !cRec.isReparse) {
                work.push_back({cr, childRel});
            }
        }
        if (onProgress) onProgress(files, dirs, dirRel);
    }

    (void)onError;
    CloseHandle(hVol);

    // Opt-in ground-truth diagnostics (BV_MFT_DEBUG_FILE): report how many
    // $INDEX_ALLOCATION blocks were parsed, how many children were sourced from
    // directory indexes (as opposed to the parent-pointer union), and how many
    // directories needed their $I30 reassembled from extension records via
    // $ATTRIBUTE_LIST. Used by the elevated regression tests; silent when the
    // variable is not set.
    if (const std::wstring diagPath = MftDiagFilePath(); !diagPath.empty()) {
        if (FILE* f = _wfopen(diagPath.c_str(), L"a")) {
            std::fprintf(f, "indxBlocks=%llu indxChildren=%llu extI30=%llu\n",
                         (unsigned long long)diagIndexBlocks,
                         (unsigned long long)diagIndexChildren,
                         (unsigned long long)diagExtI30Merged);
            std::fclose(f);
        }
    }
    return !incomplete;
}

bool MftEnumerator::ParseAttributeListForTest(const std::vector<uint8_t>& data,
                                              std::vector<MftAttrListEntry>& out) {
    out.clear();
    if (data.empty()) return true;
    return ParseAttrListData(data.data(), data.size(), out);
}

bool MftEnumerator::VcnRangeKnownForTest(
    const std::vector<std::pair<int64_t, int64_t>>& knownRanges,
    int64_t low, int64_t high) {
    if (low > high) return true; // malformed range is treated as already known
    for (const auto& r : knownRanges) {
        if (low <= r.second && high >= r.first) return true;
    }
    return false;
}

} // namespace bv