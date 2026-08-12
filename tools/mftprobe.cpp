// Phase 4 root-cause probe: full-file-reference (record + sequence) tracing
// of the raw NTFS MFT for a single file, to decide whether parent-pointer
// tree reconstruction is reliable or whether directory $I30 indexes are needed.
//
//  Usage:  bv_mftprobe  --file <full-path-to-a-file-under-test-root>
//
// Requires admin privileges. Read-only. Prints:
//   * the file's full file reference from Win32 (FileIdInfo)
//   * its MFT record (header seq, $FILE_NAME attrs with FULL parent refs)
//   * the parent record's live header seq, $FILE_NAME, whether it is a dir
//   * whether each parent-pointer's sequence matches the live record (stale?)
//   * the directory's $INDEX_ROOT / $INDEX_ALLOCATION ($I30) children
//   * the parent-pointer chain up to the volume root
//   * a FindFirstFile cross-check

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <shellapi.h>

enum {
    A_STANDARD = 0x10,
    A_FILENAME = 0x30,
    A_DATA = 0x80,
    A_INDEX_ROOT = 0x90,
    A_INDEX_ALLOC = 0xA0,
    A_BITMAP = 0xB0,
    A_END = 0xFFFFFFFFu,
};

static uint64_t g_seg = 0u, g_mftStart = 0u, g_mftBytes = 0u;
static DWORD g_cluster = 0, g_sector = 0;
static HANDLE g_hVol = INVALID_HANDLE_VALUE;

static uint64_t RefRecord(uint64_t r) { return r & 0xFFFFFFFFFFFFULL; }
static uint16_t RefSeq(uint64_t r) { return (uint16_t)((r >> 48) & 0xFFFFULL); }

static void EnableBackup() {
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

static bool OpenVolume(wchar_t driveLetter) {
    WCHAR dev[16];
    swprintf(dev, 16, L"\\\\.\\%c:", driveLetter);
    g_hVol = CreateFileW(dev, GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_hVol == INVALID_HANDLE_VALUE) return false;
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
    g_sector = vd.BytesPerSector;
    // Force pending NTFS metadata to disk so raw MFT reads are not stale.
    FlushFileBuffers(g_hVol);
    return g_seg > 0 && g_mftBytes > 0;
}

static bool ReadVolAt(uint64_t byteOff, uint8_t* out, DWORD len) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)byteOff;
    DWORD got = 0;
    return SetFilePointerEx(g_hVol, li, nullptr, FILE_BEGIN) && ReadFile(g_hVol, out, len, &got, nullptr) && got == len;
}

static bool ReadMftBytes(uint64_t off, uint8_t* out, DWORD len) {
    return ReadVolAt(g_mftStart + off, out, len);
}

static bool GetRecordRaw(uint64_t rec, std::vector<uint8_t>& buf) {
    if (rec >= g_mftBytes / g_seg) return false;
    buf.resize(g_seg);
    return ReadMftBytes(rec * g_seg, buf.data(), (DWORD)g_seg);
}

static bool ApplyFixup(uint8_t* rec, size_t n) {
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

// NTFS INDEX_BLOCK (INDX) multi-sector fixup -- IDENTICAL COPY of the low-level
// logic in src/Filesystem/MftEnumerator.cpp and tools/mftdiag.cpp; keep the
// three copies in sync. INDEX_BLOCK uses the same USA trick as FILE records
// but with an INDEX_RECORD_HEADER (magic "INDX", usa_ofs@+4, usa_count@+6,
// lsn@+8, vcn@+0x10), so it must not borrow ApplyFixup(). Returns false for a
// corrupt block (out-of-bounds USA, missing USN/tail words, tail != USN).
static bool UndoFixupIndexBlock(uint8_t* block, size_t blockSize, uint32_t bytesPerSector) {
    if (blockSize < 48 || bytesPerSector < 1 || bytesPerSector > blockSize ||
        blockSize % bytesPerSector != 0)
        return false;
    uint16_t usOffset = *(uint16_t*)(block + 4);
    uint16_t usCount = *(uint16_t*)(block + 6);
    if (usOffset == 0 || (size_t)usOffset + (size_t)usCount * 2u > blockSize) return false;
    uint32_t sectors = blockSize / bytesPerSector;
    if ((uint32_t)usCount < sectors + 1u) return false;
    uint16_t usn = *(uint16_t*)(block + usOffset);
    for (uint32_t i = 1; i <= sectors; ++i) {
        size_t pos = (size_t)i * bytesPerSector - 2u;
        if (pos + 2 > blockSize) return false;
        uint16_t* p = (uint16_t*)(block + pos);
        if (*p != usn) return false;
        *p = *(uint16_t*)(block + usOffset + 2u * i);
    }
    return true;
}

struct Attr {
    uint32_t type = 0;
    bool resident = false;
    uint32_t valueLen = 0;
    const uint8_t* value = nullptr;   // resident
    const uint8_t* nonres = nullptr;  // start of non-resident header
};

static void WalkAttrs(const uint8_t* rec, size_t n,
                      const std::function<void(const Attr&)>& cb) {
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

// Resolve the non-resident $INDEX_ALLOCATION data runs into a byte vector of
// the attribute's full data (dataSize bytes).
static bool ReadNonResidentRuns(const Attr& at, std::vector<uint8_t>& out) {
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
        if (first) {
            lcn = lcnD;
            first = false;
        } else {
            lcn += lcnD;
        }
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

// ---- MFT via its own $DATA data runs (handles fragmentation) ----
struct MftRun {
    int64_t vcn; // first VCN (in clusters) of this run
    int64_t lcn;
    int64_t len;
};
static std::vector<MftRun> g_mftRuns;

static void BuildMftRunmap() {
    if (!g_mftRuns.empty()) return;
    std::vector<uint8_t> rec0;
    if (!GetRecordRaw(0, rec0)) return;
    WalkAttrs(rec0.data(), rec0.size(), [&](const Attr& at) {
        if (at.type == A_DATA && !at.resident) {
            const uint8_t* hdr = at.nonres;
            uint16_t mapOff = *(uint16_t*)(hdr + 32);
            int64_t highVcn = *(int64_t*)(hdr + 24);
            int64_t lowVcn = *(int64_t*)(hdr + 16);
            const uint8_t* r = hdr + mapOff;
            int64_t vcn = lowVcn;
            int64_t lcn = 0;
            bool first = true;
            while (*r) {
                uint8_t lenb = (*r) & 0x0F;
                uint8_t offb = (*r) >> 4;
                if (lenb == 0) break;
                ++r;
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
                g_mftRuns.push_back({vcn, lcn, len});
                vcn += len;
                if (vcn > highVcn) break;
            }
        }
    });
    std::wprintf(L"  [$MFT runs] count=%zu\n", g_mftRuns.size());
    int shown = 0;
    for (const auto& ru : g_mftRuns) {
        if (shown < 4 || (int)g_mftRuns.size() - shown <= 2)
            std::wprintf(L"   vcn=%lld lcn=%lld len=%lld  (expected MftStartLcn=%lld)\n",
                         (long long)ru.vcn, (long long)ru.lcn, (long long)ru.len,
                         (long long)g_mftStart / g_cluster);
        ++shown;
    }
}

static bool RecordToAbs(uint64_t rec, uint64_t& absOut, uint32_t& clusterOff) {
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

static bool GetRecordRawViaRuns(uint64_t rec, std::vector<uint8_t>& buf) {
    BuildMftRunmap();
    uint64_t abs = 0;
    uint32_t coff = 0;
    if (g_mftRuns.empty() || !RecordToAbs(rec, abs, coff)) return false;
    buf.resize(g_seg);
    return ReadVolAt(abs, buf.data(), (DWORD)g_seg);
}

static void DumpIndexHeader(const uint8_t* nodeHeader, const char* who) {
    uint32_t entriesOffset = *(uint32_t*)(nodeHeader);
    uint32_t indexLength = *(uint32_t*)(nodeHeader + 4);
    uint8_t flags = *(uint8_t*)(nodeHeader + 12);
    std::wprintf(L"    %s node: entriesOff=%u indexLen=%u flags(hasSubnode)=%u\n",
                 who, (unsigned)entriesOffset, (unsigned)indexLength, (unsigned)flags);
}

// Enumerate $I30 index entries from an INDEX_HEADER node with the given
// covering bytes (root node start is the node, block start is block).
static void DumpIndexEntries(const uint8_t* nodeStart, const uint8_t* coverEnd,
                             const char* who) {
    uint32_t eoff = *(uint32_t*)(nodeStart);
    const uint8_t* e = nodeStart + eoff;
    for (;;) {
        if (e + 16 > coverEnd) { std::wprintf(L"    %s: entry truncated\n", who); break; }
        uint64_t childRef = *(uint64_t*)e;
        uint16_t elen = *(uint16_t*)(e + 8);
        uint16_t klen = *(uint16_t*)(e + 10);
        uint16_t eflags = *(uint16_t*)(e + 12);
        if (elen < 16 || e + elen > coverEnd) { std::wprintf(L"    %s: entry len bad\n", who); break; }
        std::wstring cname;
        if (klen >= 66 && e + 16 + klen <= coverEnd) {
            const uint8_t* key = e + 16;
            uint8_t nl = key[64];
            if (66u + (size_t)nl * 2u <= klen) {
                const wchar_t* p = (const wchar_t*)(key + 66);
                cname.assign(p, p + nl);
            }
        }
        std::wprintf(L"    %s child ref=0x%016llX rec=%llu seq=%u elen=%u klen=%u flags=%u name=[%ls]\n",
                     who, (unsigned long long)childRef, (unsigned long long)RefRecord(childRef),
                     (unsigned)RefSeq(childRef), (unsigned)elen, (unsigned)klen, (unsigned)eflags,
                     cname.c_str());
        if (eflags & 2) break; // LAST_ENTRY
        e += elen;
    }
}

static void PrintFileName(const uint8_t* v, uint32_t vlen, const char* label) {
    if (vlen < 66) { std::wprintf(L"%s: value too short (%u)\n", label, (unsigned)vlen); return; }
    uint64_t parentRef = *(uint64_t*)v;
    uint64_t size = *(uint64_t*)(v + 16);
    uint8_t nl = v[64], ns = v[65];
    std::wstring name;
    if (nl > 0 && 66u + (size_t)nl * 2u <= vlen) {
        const wchar_t* p = (const wchar_t*)(v + 66);
        name.assign(p, p + nl);
    }
    std::wprintf(L"%s parent=0x%016llX rec=%llu seq=%u size=%llu ns=%u nameLen=%u [%ls]\n",
                 label, (unsigned long long)parentRef, (unsigned long long)RefRecord(parentRef),
                 (unsigned)RefSeq(parentRef), (unsigned long long)size, (unsigned)ns, (unsigned)nl,
                 name.c_str());
}

static std::wstring FilenameOf(const std::vector<uint8_t>& rec) {
    std::wstring out;
    WalkAttrs(rec.data(), rec.size(), [&](const Attr& at) {
        if (at.type == A_FILENAME && at.resident && at.valueLen >= 66) {
            uint8_t nl = at.value[64];
            if (66u + (size_t)nl * 2u <= at.valueLen) {
                const wchar_t* p = (const wchar_t*)(at.value + 66);
                out.assign(p, p + nl);
            }
        }
    });
    return out;
}

static uint64_t Win32FileRef(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FILE_ID_INFO f = {};
    uint64_t ref = 0;
    if (GetFileInformationByHandleEx(h, FileIdInfo, &f, sizeof(f)))
        memcpy(&ref, f.FileId.Identifier, sizeof(uint64_t));
    CloseHandle(h);
    return ref;
}

static void DumpDirIndex(const std::vector<uint8_t>& rec, const char* label) {
    uint16_t rflags = *(uint16_t*)(rec.data() + 22);
    std::wprintf(L"  %s record flags=0x%x isDir=%d\n", label, (unsigned)rflags,
                 (rflags & 2) ? 1 : 0);
    std::wstring nm = FilenameOf(rec);
    std::wprintf(L"  %s name=[%ls]\n", label, nm.c_str());
    uint32_t blkSizeOf = 0; // index_block_size from $INDEX_ROOT (value+0x08)
    WalkAttrs(rec.data(), rec.size(), [&](const Attr& at) {
        if (at.type == A_FILENAME && at.resident) PrintFileName(at.value, at.valueLen, "    fn");
        if (at.type == A_INDEX_ROOT && at.resident) {
            std::wprintf(L"  %s $INDEX_ROOT valueLen=%u\n", label, (unsigned)at.valueLen);
            if (at.valueLen >= 16) {
                // indexed_attr_type@+0, collation@+4, index_block_size@+8,
                // clusters_per_index_block@+12
                blkSizeOf = *(uint32_t*)(at.value + 8);
            }
            if (at.valueLen >= 32) {
                DumpIndexHeader(at.value + 16, "root");
                DumpIndexEntries(at.value + 16, at.value + at.valueLen, "root");
            }
        }
        if (at.type == A_INDEX_ALLOC) {
            std::wprintf(L"  %s $INDEX_ALLOCATION present (non-resident)\n", label);
            std::vector<uint8_t> blocks;
            if (ReadNonResidentRuns(at, blocks)) {
                // block size from the directory's own $INDEX_ROOT; fall back to
                // 4096 (the value NTFS uses on all supported volumes).
                size_t bs = (blkSizeOf >= 512) ? blkSizeOf : 4096;
                // parse each index block: USA fixup FIRST, then INDEX_HEADER at
                // block+0x18 (NOT +0x10 where the block VCN field lives).
                for (size_t off = 0; off + bs <= blocks.size(); off += bs) {
                    uint8_t* b = blocks.data() + off;
                    if (*(uint32_t*)b != 0x58444E49u /*INDX*/) continue;
                    if (!UndoFixupIndexBlock(b, bs, g_sector)) {
                        std::wprintf(L"  %s  index block @%zu CORRUPT (fixup failed)\n",
                                     label, off);
                        continue;
                    }
                    DumpIndexHeader(b + 0x18, "alloc-block");
                    DumpIndexEntries(b + 0x18, b + bs, "alloc-block");
                }
            } else {
                std::wprintf(L"  %s  (could not read index blocks)\n", label);
            }
        }
    });
}

int main() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring file;
    for (int i = 1; i < argc;) {
        if (wcscmp(argv[i], L"--file") == 0 && i + 1 < argc) file = argv[++i];
        ++i;
    }
    if (file.empty()) { std::wprintf(L"usage: bv_mftprobe --file <path> (admin)\n"); return 1; }
    EnableBackup();
    wchar_t dl = (file.size() >= 2 && file[1] == L':') ? file[0] : L'C';
    if (!OpenVolume(dl)) { std::wprintf(L"FATAL: open volume %c: (run elevated)\n", dl); return 2; }

    std::wprintf(L"== Win32 side ==\n");
    uint64_t fileRef = Win32FileRef(file);
    std::wprintf(L"file path   = %ls\n", file.c_str());
    std::wprintf(L"file win32  = 0x%016llX  rec=%llu seq=%u\n", (unsigned long long)fileRef,
                 (unsigned long long)RefRecord(fileRef), (unsigned)RefSeq(fileRef));
    uint64_t recNo = RefRecord(fileRef);

    std::vector<uint8_t> buf;
    if (!GetRecordRaw(recNo, buf)) { std::wprintf(L"record %llu out of range\n", (unsigned long long)recNo); return 3; }
    std::wprintf(L"\n== file MFT record #%llu ==\n", (unsigned long long)recNo);
    uint16_t live = *(uint16_t*)(buf.data() + 16);
    std::wprintf(L"headerSeq=%u vs win32 seq=%u -> %ls\n", (unsigned)live, (unsigned)RefSeq(fileRef),
                 (live == RefSeq(fileRef)) ? L"MATCH" : L"MISMATCH");
    std::wprintf(L"file record flags=0x%x\n", (unsigned)*(uint16_t*)(buf.data() + 22));

    std::wprintf(L"\n== file $FILE_NAME attributes ==\n");
    uint64_t firstParentRef = 0;
    WalkAttrs(buf.data(), buf.size(), [&](const Attr& at) {
        if (at.type == A_FILENAME && at.resident) {
            PrintFileName(at.value, at.valueLen, "  fn");
            if (firstParentRef == 0) firstParentRef = *(uint64_t*)at.value;
        }
    });
    if (firstParentRef == 0) { std::wprintf(L"!! no parent link\n"); return 4; }

    std::wprintf(L"\n== immediate parent directory ==\n");
    {
        uint64_t pRec = RefRecord(firstParentRef);
        uint16_t pSeq = RefSeq(firstParentRef);
        std::vector<uint8_t> pb;
        GetRecordRaw(pRec, pb);
        std::vector<uint8_t> pbr;
        GetRecordRawViaRuns(pRec, pbr);
        uint16_t liveSeqC = *(uint16_t*)(pb.data() + 16);
        uint16_t liveSeqR = *(uint16_t*)(pbr.data() + 16);
        std::wstring dirN;
        WalkAttrs(pbr.data(), pbr.size(), [&](const Attr& at) {
            if (at.type == A_FILENAME && at.resident && at.valueLen >= 66) {
                uint8_t nl = at.value[64];
                const wchar_t* p = (const wchar_t*)(at.value + 66);
                dirN.assign(p, p + nl);
            }
        });
        std::wprintf(L"parent rec=%llu seqRef=%u\n", (unsigned long long)pRec, (unsigned)pSeq);
        std::wprintf(L"  contiguous read: seq=%u  name=[%ls]\n", (unsigned)liveSeqC, L"");
        std::wprintf(L"  via-$MFT runs  : seq=%u  name=[%ls] isDir=%d\n", (unsigned)liveSeqR,
                     dirN.c_str(), (*(uint16_t*)(pbr.data() + 22) & 2) ? 1 : 0);
        size_t pos = file.rfind(L'\\');
        std::wstring pd = (pos == std::wstring::npos) ? file : file.substr(0, pos);
        uint64_t liveDirRef = Win32FileRef(pd);
        std::wprintf(L"ACTUAL parent dir (Win32) = %ls\n", pd.c_str());
        std::wprintf(L"  live dir ref = 0x%016llX rec=%llu seq=%u\n",
                     (unsigned long long)liveDirRef, (unsigned long long)RefRecord(liveDirRef),
                     (unsigned)RefSeq(liveDirRef));
        DumpDirIndex(pbr, "parent");
        std::wprintf(L"  parent's own parent links:\n");
        WalkAttrs(pbr.data(), pbr.size(), [&](const Attr& at) {
            if (at.type == A_FILENAME && at.resident) PrintFileName(at.value, at.valueLen, "    fn");
        });
    }

    std::wprintf(L"\n== parent-pointer chain up to root ==\n");
    {
        uint64_t curRef = firstParentRef;
        int d = 0;
        uint64_t rootRef = Win32FileRef(std::wstring(1, dl) + L":\\");
        while (d < 16) {
            uint64_t pRec = RefRecord(curRef);
            uint16_t pSeq = RefSeq(curRef);
            std::vector<uint8_t> pb;
            if (!GetRecordRaw(pRec, pb)) { std::wprintf(L"[%d] rec=%llu unreadable\n", d, (unsigned long long)pRec); break; }
            uint16_t ls = *(uint16_t*)(pb.data() + 16);
            std::wstring nm = FilenameOf(pb);
            std::wprintf(L"[%d] rec=%llu seqRef=%u live=%u %ls name=[%ls]\n",
                         d, (unsigned long long)pRec, (unsigned)pSeq, (unsigned)ls,
                         (ls == pSeq) ? L"LIVE" : L"**STALE**", nm.c_str());
            if (pRec == RefRecord(rootRef)) { std::wprintf(L"    reached root record.\n"); break; }
            // next parent from this record's own $FILE_NAME
            uint64_t next = 0;
            WalkAttrs(pb.data(), pb.size(), [&](const Attr& at) {
                if (at.type == A_FILENAME && at.resident) next = *(uint64_t*)at.value;
            });
            if (next == 0) { std::wprintf(L"    chain ends (record has no parent link).\n"); break; }
            curRef = next;
            ++d;
        }
    }

    std::wprintf(L"\n== FindFirstFile cross-check: entries in parent dir ==\n");
    {
        size_t pos2 = file.rfind(L'\\');
        std::wstring dir = (pos2 == std::wstring::npos) ? file : file.substr(0, pos2);
        std::wprintf(L"dir=%ls\n", dir.c_str());
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            std::wprintf(L"  (cannot enumerate)\n");
        } else {
            int n = 0;
            do {
                std::wstring nm = fd.cFileName;
                if (nm == L"." || nm == L"..") continue;
                if (n < 8) std::wprintf(L"  [%ls]\n", nm.c_str());
                ++n;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
            std::wprintf(L"  parent dir total entries (excl . ..) = %d\n", n);
        }
    }

    CloseHandle(g_hVol);
    return 0;
}