// MFT vs Win32 deep diagnostic (Phase 4, investigation tool).
//
// Usage:
//   bv_mftdiag --root <path> [--gen <N>] [--churn] [--probe <N>] [--reuse]
//
//   --root <path>  compare MFT and Win32 enumeration of this root
//   --gen <N>      create a fresh N-file stress tree first (100 dirs)
//   --churn        delete+recreate the tree 3 times before the final compare,
//                  forcing MFT record re-use
//   --probe <N>    deep-probe up to N failing paths (default 5)
//
// For every difference between the two back-ends it produces a full
// file-reference diagnostic that keeps RECORD NUMBER and SEQUENCE NUMBER
// separate:
//   * Win32 FileIdInfo full reference (record + sequence)
//   * every $FILE_NAME attribute of the record (parent ref incl. sequence,
//     namespace, size, flags, timestamps, name)
//   * which $FILE_NAME is chosen for the path rebuild and why
//   * the parent chain followed to the root with a LIVE / STALE sequence
//     check at each step, reporting the exact break point (record not in
//     use, sequence mismatch, missing name, out of range, cycle, depth cap)
//   * for directories, the $INDEX_ROOT / $INDEX_ALLOCATION ($I30) child list,
//     to tell whether an index-driven reconstruction would have recovered
//     the lost children.
//
// Requires admin (raw volume MFT read). Read-only apart from --gen/--churn.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <shellapi.h>

#include "Filesystem/MftEnumerator.h"
#include "Filesystem/Win32Enumerator.h"
#include "TestTree.h"

using namespace bv;

namespace {

enum {
    A_STANDARD = 0x10,
    A_FILENAME = 0x30,
    A_DATA = 0x80,
    A_INDEX_ROOT = 0x90,
    A_INDEX_ALLOC = 0xA0,
    A_BITMAP = 0xB0,
    A_END = 0xFFFFFFFFu,
};

uint64_t g_seg = 0, g_mftStart = 0, g_mftBytes = 0;
DWORD g_cluster = 0;
HANDLE g_hVol = INVALID_HANDLE_VALUE;

uint64_t RefRecord(uint64_t r) { return r & 0xFFFFFFFFFFFFULL; }
uint16_t RefSeq(uint64_t r) { return (uint16_t)((r >> 48) & 0xFFFFULL); }

void EnableBackup() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return;
    LUID l;
    if (LookupPrivilegeValueW(nullptr, SE_BACKUP_NAME, &l)) {
        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = l;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
    }
    CloseHandle(tok);
}

bool OpenVolume(wchar_t driveLetter) {
    WCHAR dev[16];
    swprintf(dev, 16, L"\\\\.\\%c:", driveLetter);
    g_hVol = CreateFileW(dev, GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_hVol == INVALID_HANDLE_VALUE) {
        std::wprintf(L"  [volume] cannot open \\\\.\\%c: (need admin)\n", driveLetter);
        return false;
    }
    NTFS_VOLUME_DATA_BUFFER vd = {};
    DWORD ret = 0;
    if (!DeviceIoControl(g_hVol, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &vd, sizeof(vd),
                         &ret, nullptr)) {
        CloseHandle(g_hVol);
        g_hVol = INVALID_HANDLE_VALUE;
        return false;
    }
    g_seg = vd.BytesPerFileRecordSegment;
    g_mftStart = (uint64_t)vd.MftStartLcn.QuadPart * vd.BytesPerCluster;
    g_mftBytes = (uint64_t)vd.MftValidDataLength.QuadPart;
    g_cluster = vd.BytesPerCluster;
    // Force pending NTFS metadata to disk so raw MFT reads are not stale
    // (NTFS batches metadata commits; without this a just-created tree can be
    // read back in a partially-flushed state).
    FlushFileBuffers(g_hVol);
    return g_seg > 0 && g_mftBytes > 0;
}

bool ReadVolAt(uint64_t byteOff, uint8_t* out, DWORD len) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)byteOff;
    DWORD got = 0;
    return SetFilePointerEx(g_hVol, li, nullptr, FILE_BEGIN) &&
           ReadFile(g_hVol, out, len, &got, nullptr) && got == len;
}

bool ApplyFixup(uint8_t* rec, size_t n) {
    if (n < 42 || *(uint32_t*)rec != 0x454C4946u) return false;
    uint16_t usOff = *(uint16_t*)(rec + 4), usSize = *(uint16_t*)(rec + 6);
    uint16_t rsize = *(uint16_t*)(rec + 28);
    if (usOff == 0 || usSize < 2 || rsize < 42 || rsize > n) return false;
    uint16_t usn = *(uint16_t*)(rec + usOff);
    uint32_t sectors = rsize / 512u;
    if ((uint32_t)usSize < sectors + 1u) return false;
    for (uint32_t i = 1; i <= sectors; ++i) {
        size_t pos = (size_t)i * 512u - 2u;
        if (pos + 2 > n) break;
        uint16_t* p = (uint16_t*)(rec + pos);
        if (*p == usn) *p = *(uint16_t*)(rec + usOff + 2u * i);
    }
    return true;
}

struct MftRun {
    int64_t vcn;
    int64_t lcn;
    int64_t len;
};
std::vector<MftRun> g_mftRuns;

struct Attr {
    uint32_t type = 0;
    bool resident = false;
    uint32_t valueLen = 0;
    const uint8_t* value = nullptr;   // resident
    const uint8_t* nonres = nullptr;  // start of non-resident header
};

void WalkAttrs(const uint8_t* rec, size_t n, const std::function<void(const Attr&)>& cb) {
    uint32_t sig = *(const uint32_t*)rec;
    if (n < 42 || sig != 0x454C4946u) return;
    uint16_t first = *(const uint16_t*)(rec + 20);
    if (first < 42 || first >= n) return;
    const uint8_t* a = rec + first;
    const uint8_t* const end = rec + n;
    while (a + 24 <= end) {
        uint32_t type = *(const uint32_t*)a;
        if (type == A_END) break;
        uint32_t len = *(const uint32_t*)(a + 4);
        if (len < 24 || a + len > end) break;
        Attr at;
        at.type = type;
        at.resident = (a[8] == 0);
        if (at.resident) {
            at.valueLen = *(const uint32_t*)(a + 16);
            at.value = a + *(const uint16_t*)(a + 20);
            if (at.value + at.valueLen > end) at.valueLen = 0;
        } else {
            at.nonres = a;
        }
        cb(at);
        a += len;
    }
}

void BuildMftRunmap() {
    if (!g_mftRuns.empty()) return;
    std::vector<uint8_t> rec0((size_t)g_seg);
    if (!ReadVolAt(g_mftStart, rec0.data(), (DWORD)g_seg) ||
        !ApplyFixup(rec0.data(), rec0.size()))
        return;
    WalkAttrs(rec0.data(), rec0.size(), [&](const Attr& at) {
        if (at.type == A_DATA && !at.resident) {
            const uint8_t* hdr = at.nonres;
            uint16_t mapOff = *(uint16_t*)(hdr + 32);
            int64_t highVcn = *(int64_t*)(hdr + 24);
            int64_t lowVcn = *(int64_t*)(hdr + 16);
            const uint8_t* r = hdr + mapOff;
            int64_t vcn = lowVcn;
            int64_t lcn = 0;
            bool firstRun = true;
            while (*r) {
                uint8_t lenb = (*r) & 0x0F;
                uint8_t offb = (*r) >> 4;
                if (lenb == 0) break;
                ++r;
                if (lenb > 8 || offb > 8) break;
                int64_t len = 0;
                for (int i = lenb - 1; i >= 0; --i) len = (len << 8) | r[i];
                int64_t lcnD = 0;
                for (int i = offb - 1; i >= 0; --i) lcnD = (lcnD << 8) | r[lenb + i];
                if (offb && (r[lenb + offb - 1] & 0x80)) lcnD -= (int64_t)1 << (8 * offb);
                r += lenb + offb;
                if (firstRun) { lcn = lcnD; firstRun = false; } else { lcn += lcnD; }
                g_mftRuns.push_back({vcn, lcn, len});
                vcn += len;
                if (vcn > highVcn) break;
            }
        }
    });
}

bool RecordToAbs(uint64_t rec, uint64_t& absOut, uint32_t& clusterOff) {
    uint64_t byteInMft = rec * g_seg;
    int64_t targetVcn = (int64_t)(byteInMft / g_cluster);
    clusterOff = (uint32_t)(byteInMft % g_cluster);
    for (const auto& ru : g_mftRuns) {
        if (targetVcn >= ru.vcn && targetVcn < ru.vcn + ru.len) {
            absOut = (uint64_t)(ru.lcn + (targetVcn - ru.vcn)) * g_cluster + clusterOff;
            return true;
        }
    }
    return false;
}

// Read (raw, via the $MFT data-run map) the record of an MFT record number.
enum class ReadRecResult { Ok, OutOfRange, OpenFail, ReadFail };
ReadRecResult GetRecordRawViaRuns(uint64_t rec, std::vector<uint8_t>& buf) {
    BuildMftRunmap();
    uint64_t nRec = g_mftBytes / g_seg;
    if (rec >= nRec) return ReadRecResult::OutOfRange;
    uint64_t abs = 0;
    uint32_t coff = 0;
    if (g_mftRuns.empty() || !RecordToAbs(rec, abs, coff)) return ReadRecResult::OpenFail;
    buf.resize(g_seg);
    if (!ReadVolAt(abs, buf.data(), (DWORD)g_seg)) return ReadRecResult::ReadFail;
    return ReadRecResult::Ok;
}

// ---- $FILE_NAME helpers ---------------------------------------------------

struct FileNameInfo {
    uint64_t parentRef = 0;
    uint64_t realSize = 0;
    uint64_t allocSize = 0;
    uint64_t flags = 0;
    uint32_t mtime = 0;
    uint32_t ctime = 0;
    uint32_t access = 0;
    uint8_t ns = 0xFF;
    std::wstring name;
    bool valid = false;
};

// Parse ONE $FILE_NAME value (payload after the attribute header).
FileNameInfo ParseFileName(const uint8_t* v, uint32_t vlen) {
    FileNameInfo f;
    if (vlen < 66) return f;
    f.parentRef = *(uint64_t*)v;
    f.ctime = (uint32_t)*(uint64_t*)(v + 8);
    f.mtime = (uint32_t)*(uint64_t*)(v + 16);
    f.access = (uint32_t)*(uint64_t*)(v + 32);
    f.allocSize = *(uint64_t*)(v + 40);
    f.realSize = *(uint64_t*)(v + 48);
    f.flags = *(uint32_t*)(v + 56);
    f.ns = v[65];
    uint8_t nl = v[64];
    if (nl > 0 && 66u + (size_t)nl * 2u <= vlen) {
        const wchar_t* p = (const wchar_t*)(v + 66);
        f.name.assign(p, p + nl);
    }
    f.valid = true;
    return f;
}

std::vector<FileNameInfo> FileNamesOf(const std::vector<uint8_t>& rec) {
    std::vector<FileNameInfo> out;
    WalkAttrs(rec.data(), rec.size(), [&](const Attr& at) {
        if (at.type == A_FILENAME && at.resident) {
            FileNameInfo f = ParseFileName(at.value, at.valueLen);
            if (f.valid) out.push_back(std::move(f));
        }
    });
    return out;
}

// NTFS file-name namespaces.
const wchar_t* NsName(uint8_t ns) {
    switch (ns) {
        case 0: return L"POSIX";
        case 1: return L"WIN32";
        case 2: return L"DOS";
        case 3: return L"WIN32+DOS";
        default: return L"?";
    }
}

// Choose the $FILE_NAME attribute to use for rebuild:
//  * prefer the last WIN32 (ns==1) or WIN32+DOS (ns==3) name, in attribute
//    order NTFS stores the primary name first, hard-link entries after;
//  * namespace 0 (POSIX) is the raw name storage NTFS uses for its own
//    naming and is never the shell-visible dotted name we want,
//  * namespace 2 (DOS) is the 8.3 short name, never used for the real path.
//  * a file with N $FILE_NAME attrs is hard-linked: each attr is a separate
//    parent/name pair; we keep ALL for diagnostics but rebuild with the
//    Win32-equivalent one.
static const FileNameInfo* PickChainName(const std::vector<FileNameInfo>& fns) {
    const FileNameInfo* pick = nullptr;
    for (const auto& f : fns) {
        if (f.ns == 1 || f.ns == 3) {
            pick = &f; // last WIN32 name wins (NTFS appends hard links)
        }
    }
    if (!pick) for (const auto& f : fns) { pick = &f; break; }
    return pick;
}

// ---- $I30 -----------------------------------------------------------------

void DumpIndexEntries(const uint8_t* nodeStart, const uint8_t* coverEnd, const char* who) {
    uint32_t eoff = *(uint32_t*)(nodeStart);
    const uint8_t* e = nodeStart + eoff;
    for (;;) {
        if (e + 16 > coverEnd) { std::wprintf(L"        %s: entry truncated\n", who); break; }
        uint64_t childRef = *(uint64_t*)e;
        uint16_t elen = *(uint16_t*)(e + 8);
        uint16_t klen = *(uint16_t*)(e + 10);
        uint16_t eflags = *(uint16_t*)(e + 12);
        if (elen < 16 || e + elen > coverEnd) { std::wprintf(L"        %s: entry len bad\n", who); break; }
        std::wstring cname;
        if (klen >= 66 && e + 16 + klen <= coverEnd) {
            const uint8_t* key = e + 16;
            uint8_t nl = key[64];
            if (66u + (size_t)nl * 2u <= klen) {
                const wchar_t* p = (const wchar_t*)(key + 66);
                cname.assign(p, p + nl);
            }
        }
        std::wprintf(L"        %s child=0x%016llX rec=%llu seq=%u name=[%ls]\n",
                     who, (unsigned long long)childRef, (unsigned long long)RefRecord(childRef),
                     (unsigned)RefSeq(childRef), cname.c_str());
        if (eflags & 2) break; // LAST_ENTRY
        e += elen;
    }
}

// Resolve the non-resident $INDEX_ALLOCATION data runs into full data.
bool ReadNonResidentRuns(const Attr& at, std::vector<uint8_t>& out) {
    const uint8_t* hdr = at.nonres;
    uint16_t mapOff = *(uint16_t*)(hdr + 32);
    int64_t highestVcn = *(int64_t*)(hdr + 24);
    uint64_t dataSize = *(uint64_t*)(hdr + 48);
    if (mapOff == 0 || dataSize == 0 || dataSize > (64u << 20) || highestVcn < 0) return false;
    size_t totalBytes = (size_t)(highestVcn + 1) * g_cluster;
    std::vector<uint8_t> tmp(totalBytes);
    const uint8_t* r = hdr + mapOff;
    int64_t vcn = 0;
    int64_t lcn = 0;
    bool first = true;
    while (*r) {
        uint8_t lenb = (*r) & 0x0F;
        uint8_t offb = (*r) >> 4;
        if (lenb == 0) break;
        ++r;
        if (lenb > 8 || offb > 8 || r + lenb + offb > hdr + 4096) return false;
        int64_t len = 0;
        for (int i = lenb - 1; i >= 0; --i) len = (len << 8) | r[i];
        int64_t lcnD = 0;
        for (int i = offb - 1; i >= 0; --i) lcnD = (lcnD << 8) | r[lenb + i];
        if (offb && (r[lenb + offb - 1] & 0x80)) lcnD -= (int64_t)1 << (8 * offb);
        r += lenb + offb;
        if (first) { lcn = lcnD; first = false; } else { lcn += lcnD; }
        for (int64_t k = 0; k < len; ++k) {
            size_t byteOff = (size_t)vcn * g_cluster;
            if (byteOff + g_cluster <= tmp.size())
                ReadVolAt((uint64_t)(lcn + k) * g_cluster, tmp.data() + byteOff, g_cluster);
            ++vcn;
        }
    }
    if (dataSize < tmp.size()) tmp.resize(dataSize);
    out.swap(tmp);
    return !out.empty();
}

// Print the $I30 (INDEX_ROOT + INDEX_ALLOCATION) children of `rec`.
void DumpDirIndex(const std::vector<uint8_t>& rec) {
    WalkAttrs(rec.data(), rec.size(), [&](const Attr& at) {
        if (at.type == A_INDEX_ROOT && at.resident) {
            std::wprintf(L"    $INDEX_ROOT valueLen=%u\n", (unsigned)at.valueLen);
            if (at.valueLen >= 32) DumpIndexEntries(at.value + 16, at.value + at.valueLen, "root");
        }
        if (at.type == A_INDEX_ALLOC) {
            std::vector<uint8_t> blocks;
            if (ReadNonResidentRuns(at, blocks)) {
                for (size_t off = 0; off + 4096 <= blocks.size(); off += 4096) {
                    const uint8_t* b = blocks.data() + off;
                    if (*(uint32_t*)b != 0x58444E49u /*INDX*/) continue;
                    DumpIndexEntries(b + 16, b + 4096, "block");
                }
            } else {
                std::wprintf(L"    $INDEX_ALLOCATION present but unreadable\n");
            }
        }
    });
}

// ---- Win32 helpers --------------------------------------------------------

uint64_t Win32FileRef(const std::wstring& path) {
    HANDLE h = CreateFileW((L"\\\\?\\" + path).c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FILE_ID_INFO f = {};
    uint64_t ref = 0;
    if (GetFileInformationByHandleEx(h, FileIdInfo, &f, sizeof(f)))
        memcpy(&ref, f.FileId.Identifier, sizeof(uint64_t));
    CloseHandle(h);
    return ref;
}

// ---- deep probe (the 11 diagnostic points) --------------------------------

// Prints the full file-reference chain from a child record up to the root,
// keeping record and sequence separate, and reports the exact break reason.
// mode: "file" | "dir-missing".
void ProbeRecord(uint64_t recNo, const std::wstring& winPath, const char* mode) {
    std::wprintf(L"\n=== PROBE (%ls) path=[%ls] ===\n", mode, winPath.c_str());

    // (1) record number + (2) sequence number
    std::vector<uint8_t> buf;
    ReadRecResult rr = GetRecordRawViaRuns(recNo, buf);
    if (rr == ReadRecResult::OutOfRange) {
        std::wprintf(L"record %llu OUT OF RANGE (valid n=%llu) -> cannot probe\n",
                     (unsigned long long)recNo, (unsigned long long)(g_mftBytes / g_seg));
        return;
    }
    if (rr != ReadRecResult::Ok) {
        std::wprintf(L"record %llu READ FAILED\n", (unsigned long long)recNo);
        return;
    }
    uint16_t recSeq = *(uint16_t*)(buf.data() + 16);
    uint16_t rflags = *(uint16_t*)(buf.data() + 22);
    std::wprintf(L"(1) record number        = %llu\n", (unsigned long long)recNo);
    std::wprintf(L"(2) record header seq    = %u\n", (unsigned)recSeq);
    std::wprintf(L"    record flags         = 0x%04x inUse=%d isDir=%d reparse=%d\n",
                 (unsigned)rflags, (rflags & 1) ? 1 : 0, (rflags & 2) ? 1 : 0,
                 (rflags & 4) ? 1 : 0);

    uint64_t winRef = Win32FileRef(winPath);
    if (winRef) {
        std::wprintf(L"    Win32 FileIdInfo     = 0x%016llX rec=%llu seq=%u  (seq%s)\n",
                     (unsigned long long)winRef, (unsigned long long)RefRecord(winRef),
                     (unsigned)RefSeq(winRef),
                     (RefRecord(winRef) == recNo) ? L" matches rec" : L" != recNo!");
    }

    // (3) every $FILE_NAME
    std::vector<FileNameInfo> fns = FileNamesOf(buf);
    if (fns.empty()) {
        std::wprintf(L"(3) $FILE_NAME          = NONE (no parent link; cannot rebuild)\n");
        return;
    }
    std::wprintf(L"(3) $FILE_NAME attributes (%zu):\n", fns.size());
    uint64_t pRef = 0;
    for (size_t i = 0; i < fns.size(); ++i) {
        const FileNameInfo& f = fns[i];
        std::wprintf(
            L"    [%zu] parent=0x%016llX rec=%llu seq=%u ns=%ls(%u) nameLen=%zu "
            L"size=%llu alloc=%llu flags=0x%08llx hwTM=%08x ctime=%08x [%ls]\n",
            i, (unsigned long long)f.parentRef, (unsigned long long)RefRecord(f.parentRef),
            (unsigned)RefSeq(f.parentRef), NsName(f.ns), (unsigned)f.ns, f.name.size(),
            (unsigned long long)f.realSize, (unsigned long long)f.allocSize,
            (unsigned long long)f.flags, (unsigned)f.mtime, (unsigned)f.ctime, f.name.c_str());
        if (f.ns == 1 || f.ns == 3) pRef = f.parentRef; // last WIN32 one
    }
    if (pRef == 0) {
        for (const auto& f : fns) { pRef = f.parentRef; break; }
        std::wprintf(L"    (no WIN32 namespace name; using first as fallback)\n");
    }

    // (4)(5)(6) parent reference components
    uint64_t pRec = RefRecord(pRef);
    uint16_t pSeq = RefSeq(pRef);
    std::wprintf(L"(4) parent file reference = 0x%016llX\n", (unsigned long long)pRef);
    std::wprintf(L"(5) parent record number  = %llu\n", (unsigned long long)pRec);
    std::wprintf(L"(6) parent seq (in ref)   = %u\n", (unsigned)pSeq);

    // (10) follow the chain; (11) exact break point
    // root of the drive the probe runs on is record 5 on NTFS; verify live
    std::wstring drive = L"C:\\";
    if (winPath.size() >= 2 && winPath[1] == L':') drive = std::wstring(1, winPath[0]) + L":\\";
    uint64_t rootFileRef = Win32FileRef(drive);
    uint64_t rootRec = RefRecord(rootFileRef);
    std::wprintf(L"(10) root of drive       = 0x%016llX rec=%llu seq=%u\n",
                 (unsigned long long)rootFileRef, (unsigned long long)rootRec,
                 (unsigned)RefSeq(rootFileRef));

    std::wprintf(L"     parent chain (record+seq, LIVE/STALE):\n");
    std::set<uint64_t> seen;
    uint64_t curRef = pRef;
    bool reachedRoot = (pRec == rootRec);
    const wchar_t* breakReason = nullptr;
    int steps = 0;
    for (; steps < 16; ++steps) {
        std::vector<uint8_t> pb;
        ReadRecResult pr = GetRecordRawViaRuns(pRec, pb);
        if (pr == ReadRecResult::OutOfRange) {
            std::wprintf(L"     [%d] rec=%llu OUT-OF-RANGE (beyond MFT valid data)\n",
                         steps, (unsigned long long)pRec);
            breakReason = L"parent record out of range";
            break;
        }
        if (pr != ReadRecResult::Ok) {
            std::wprintf(L"     [%d] rec=%llu READ-FAILED\n", steps, (unsigned long long)pRec);
            breakReason = L"parent record unreadable";
            break;
        }
        uint16_t liveSeq = *(uint16_t*)(pb.data() + 16);
        uint16_t lflags = *(uint16_t*)(pb.data() + 22);
        std::vector<FileNameInfo> pfns = FileNamesOf(pb);
        const FileNameInfo* parentPick = PickChainName(pfns);
        bool inUse = (lflags & 1) != 0;
        bool liveSeqOk = (liveSeq == pSeq && pSeq != 0);
        std::wprintf(
            L"     [%d] rec=%llu refSeq=%u liveSeq=%u inUse=%d isDir=%d -> %ls  name=[%ls]\n",
            steps, (unsigned long long)pRec, (unsigned)pSeq, (unsigned)liveSeq,
            inUse ? 1 : 0, (lflags & 2) ? 1 : 0,
            (liveSeqOk && inUse) ? L"LIVE" : L"**STALE/INVALID**",
            parentPick ? parentPick->name.c_str() : L"(no $FILE_NAME)");
        // (9) parent's own $FILE_NAME attributes
        if (steps == 0) {
            std::wprintf(L"(9) parent $FILE_NAME attrs:\n");
            for (size_t i = 0; i < pfns.size(); ++i) {
                std::wprintf(
                    L"      [%zu] parent=0x%016llX rec=%llu seq=%u ns=%ls size=%llu flags=0x%08llx [%ls]\n",
                    i, (unsigned long long)pfns[i].parentRef,
                    (unsigned long long)RefRecord(pfns[i].parentRef),
                    (unsigned)RefSeq(pfns[i].parentRef), NsName(pfns[i].ns),
                    (unsigned long long)pfns[i].realSize, (unsigned long long)pfns[i].flags,
                    pfns[i].name.c_str());
            }
            if (strlen(mode) > 3) { // dir: show $I30 when probing missing dirs
                std::wprintf(L"     parent dir $I30 child list (INDEX_ROOT/ALLOCATION):\n");
                DumpDirIndex(pb);
            }
        }
        if (!inUse) { breakReason = L"parent record NOT IN USE (deleted/never flushed)"; break; }
        if (!liveSeqOk) { breakReason = L"SEQUENCE MISMATCH (stale reference)"; break; }
        if (pRec == rootRec) { reachedRoot = true; break; }
        if (!parentPick || parentPick->parentRef == 0) {
            breakReason = L"record has no $FILE_NAME parent link";
            break;
        }
        if (seen.count(pRec)) { breakReason = L"CYCLE in parent chain"; break; }
        seen.insert(pRec);
        curRef = parentPick->parentRef;
        pRec = RefRecord(curRef);
        pSeq = RefSeq(curRef);
    }
    if (steps == 16 && !reachedRoot) breakReason = breakReason ? breakReason : L"depth cap 16";

    std::wprintf(L"(11) chain result = %ls%s\n",
                 reachedRoot ? L"reached root OK" : (breakReason ? L"BROKEN -> " : L"BROKEN -> "),
                 reachedRoot ? L"" : (breakReason ? breakReason : L"unknown"));
}

void ProbePath(const std::wstring& path, bool isDir) {
    uint64_t ref = Win32FileRef(path);
    if (!ref) {
        std::wprintf(L"  (cannot open [%ls] via Win32)\n", path.c_str());
        return;
    }
    ProbeRecord(RefRecord(ref), path, isDir ? "dir-missing" : "file-missing");
}

// ---- enumeration helpers --------------------------------------------------

struct WEntry {
    uint64_t size = 0;
    bool isDir = false;
};

std::map<std::wstring, WEntry> EnumerateMft(const std::wstring& root, bool& ok) {
    std::map<std::wstring, WEntry> out;
    MftEnumerator en;
    ok = en.enumerate(
        root,
        [&](FileEntry&& e) { out[e.relativePath] = WEntry{e.size, e.isDirectory}; return true; },
        [](const ScanError&) {});
    return out;
}

std::map<std::wstring, WEntry> EnumerateWin(const std::wstring& root, bool& ok) {
    std::map<std::wstring, WEntry> out;
    Win32Enumerator en;
    ok = en.enumerate(
        root,
        [&](FileEntry&& e) { out[e.relativePath] = WEntry{e.size, e.isDirectory}; return true; },
        [](const ScanError&) {});
    return out;
}

// ---- comparison + diagnostics --------------------------------------------

void CompareAndDiag(const std::wstring& root, size_t probeLimit) {
    std::wprintf(L"\n[compare] root=%ls\n", root.c_str());
    bool wok = false, mok = false;
    std::map<std::wstring, WEntry> win = EnumerateWin(root, wok);
    std::map<std::wstring, WEntry> mft = EnumerateMft(root, mok);
    std::wprintf(L"  win32 enumerate ok=%d  mft enumerate ok=%d\n", wok ? 1 : 0, mok ? 1 : 0);
    if (!mok) {
        std::wprintf(L"  MFT not usable (no admin?) -- nothing to compare.\n");
        return;
    }

    uint64_t winFiles = 0, winDirs = 0, mftFiles = 0, mftDirs = 0;
    for (const auto& kv : win) { if (kv.second.isDir) ++winDirs; else ++winFiles; }
    for (const auto& kv : mft) { if (kv.second.isDir) ++mftDirs; else ++mftFiles; }
    std::wprintf(L"  MFT   files=%llu dirs=%llu total=%zu\n", mftFiles, mftDirs, mft.size());
    std::wprintf(L"  Win32 files=%llu dirs=%llu total=%zu\n", winFiles, winDirs, win.size());

    std::vector<std::wstring> onlyMft, onlyWin;
    std::vector<std::pair<std::wstring, bool>> sizeMismatch; // path,isDir
    auto iw = win.begin();
    auto im = mft.begin();
    while (iw != win.end() || im != mft.end()) {
        if (im == mft.end() || (iw != win.end() && iw->first < im->first)) {
            onlyWin.push_back(iw->first);
            ++iw;
        } else if (iw == win.end() || im->first < iw->first) {
            onlyMft.push_back(im->first);
            ++im;
        } else {
            if (iw->second.size != im->second.size || iw->second.isDir != im->second.isDir) {
                sizeMismatch.emplace_back(iw->first, iw->second.isDir);
            }
            ++iw;
            ++im;
        }
    }
    std::wprintf(L"  Only MFT:   %zu\n", onlyMft.size());
    std::wprintf(L"  Only Win32: %zu\n", onlyWin.size());
    std::wprintf(L"  Mismatch (size/type): %zu\n", sizeMismatch.size());

    size_t shown = 0;
    for (const auto& p : onlyWin) {
        if (shown >= probeLimit) break;
        ProbePath(p, false);
        ++shown;
    }
    // missing directories need extra care: probe AND show $I30
    size_t dirsShown = 0;
    for (const auto& p : onlyWin) {
        if (dirsShown >= probeLimit) break;
        auto it = win.find(p);
        if (it != win.end() && it->second.isDir) {
            ProbePath(p, true);
            ++dirsShown;
        }
    }
    size_t shownM = 0;
    for (const auto& p : onlyMft) {
        if (shownM >= probeLimit) break;
        std::wprintf(L"\n=== PROBE (mft-only) path=[%ls] (no Win32 ref)\n", p.c_str());
        ++shownM;
    }
    (void)sizeMismatch;
}

} // namespace

int MainImpl(int argc, wchar_t** argv) {
    std::wstring root, genRoot;
    size_t gen = 0;
    bool churn = false;
    size_t probeLimit = 5;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--root" && i + 1 < argc) root = argv[++i];
        else if (a == L"--gen" && i + 1 < argc) gen = std::stoull(argv[++i]);
        else if (a == L"--probe" && i + 1 < argc) probeLimit = std::stoull(argv[++i]);
        else if (a == L"--churn") churn = true;
        else { std::wcerr << L"Argomento sconosciuto: " << a << L"\n"; return 1; }
    }
    if (root.empty()) root = L"C:\\Users\\alemo\\AppData\\Local\\Temp\\opencode_mftdiag_root";
    if (gen == 0) gen = 5000;

    std::wprintf(L"== bv_mftdiag ==\n");

    EnableBackup();
    wchar_t dl = (root.size() >= 2 && root[1] == L':') ? root[0] : L'C';
    if (!OpenVolume(dl)) {
        std::wprintf(L"FATAL: cannot open volume (need admin).\n");
        return 2;
    }

    if (churn) {
        for (int r = 1; r <= 3; ++r) {
            testgen::CreateStressTree(root, gen);
            for (int attempt = 0; attempt < 3; ++attempt) {
                std::error_code ec;
                std::filesystem::remove_all(root, ec);
                if (!ec) break;
            }
            std::wprintf(L"  [churn] round %d done (created+deleted)\n", r);
        }
        // final tree: created now so records are fresh and reused
        testgen::CreateStressTree(root, gen);
        std::wprintf(L"  [churn] final %zu-file tree created\n", gen);
    }

    if (gen > 0 && !churn) {
        testgen::CreateStressTree(root, gen);
        std::wprintf(L"  [gen] created %zu-file stress tree at %ls\n", gen, root.c_str());
    }

    CompareAndDiag(root, probeLimit);

    CloseHandle(g_hVol);
    return 0;
}

int main() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    const int rc = MainImpl(argc, argv);
    LocalFree(argv);
    return rc;
}