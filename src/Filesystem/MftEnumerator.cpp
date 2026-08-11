#include "MftEnumerator.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

#include <cstdio>
#include <cwchar>
#include <cstring>

#include "PathUtil.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

// Phase 4 NTFS MFT scan. Reads the raw $MFT region (located via
// FSCTL_GET_NTFS_VOLUME_DATA at MftStartLcn*BytesPerCluster spanning
// MftValidDataLength) and reconstructs the subtree rooted at `root`, filling
// FileEntry.fileId with the MFT record number. Requires admin privileges.
// Best-effort: any unsupported/unreadable condition returns false so the caller
// falls back to Win32Enumerator without ever producing a wrong result.

namespace bv {

namespace {

constexpr uint32_t kAttrFileName = 0x30;   // NTFS_ATTR_FILENAME
constexpr uint32_t kAttrData = 0x80;       // NTFS_ATTR_DATA
constexpr uint32_t kAttrEnd = 0xFFFFFFFF;
constexpr uint64_t kMftHighestSystem = 23; // low band of volume metafiles
constexpr uint64_t kNoParent = 0xFFFFFFFFFFFFFFFFULL;

struct ParsedRec {
    uint64_t parent = 0;
    uint64_t size = 0;
    uint64_t dataSize = 0; // $DATA logical size (authoritative)
    uint64_t mtime = 0;    // FILETIME of last write
    uint32_t flags = 0;    // FILE_ATTRIBUTE_* (dir / reparse bits used)
    bool hasName = false;
    std::wstring name;     // filled only when wantName is true
};

uint64_t FileRefRecordNumber(uint64_t ref) { return ref & 0xFFFFFFFFFFFFULL; }

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

void ParseRecord(const uint8_t* rec, size_t bufSize, bool wantName, ParsedRec& out) {
    const uint16_t attrOffset = *reinterpret_cast<const uint16_t*>(rec + 20);
    if (attrOffset < 48 || (size_t)attrOffset + 16 > bufSize) return;

    const uint8_t* a = rec + attrOffset;
    const uint8_t* const end = rec + bufSize;

    while (a + 16 <= end) {
        const uint32_t type = *reinterpret_cast<const uint32_t*>(a);
        if (type == kAttrEnd) break;
        const uint32_t len = *reinterpret_cast<const uint32_t*>(a + 4);
        if (len < 16 || a + len > end) break;

        if (type == kAttrFileName && a[8] == 0) { // resident
            const uint16_t valueOff = *reinterpret_cast<const uint16_t*>(a + 20);
            const uint8_t* v = a + valueOff;
            if (a + valueOff + 66 <= end) {
                out.parent = FileRefRecordNumber(*reinterpret_cast<const uint64_t*>(v));
                out.size = *reinterpret_cast<const uint64_t*>(v + 48); // real size @0x30
                out.mtime = *reinterpret_cast<const uint64_t*>(v + 16); // modified @0x10
                out.flags = *reinterpret_cast<const uint32_t*>(v + 56);
                const uint8_t nameLen = v[64]; // length in characters
                const uint8_t ns = v[65];
                if (wantName && nameLen > 0 && ns <= 3 &&
                    a + valueOff + 66u + (size_t)nameLen * 2u <= end) {
                    const wchar_t* p = reinterpret_cast<const wchar_t*>(v + 66);
                    out.name.assign(p, p + nameLen);
                }
                out.hasName = true;
            }
        } else if (type == kAttrData && a[8] == 0) { // resident $DATA
            out.dataSize = *reinterpret_cast<const uint32_t*>(a + 16); // content length
        } else if (type == kAttrData && a[8] != 0) { // non-resident $DATA
            out.dataSize = *reinterpret_cast<const uint64_t*>(a + 48); // real size @0x30
        }
        a += len; // keep the LAST $FILE_NAME (the Win32 name)
    }
}

// A contiguous cluster run belonging to the $MFT data attribute.
struct MftRun {
    int64_t startVcn; // first VCN (clusters) of this run
    int64_t lcn;      // first LCN
    int64_t len;      // clusters
};

bool ReadVolAt(HANDLE hVol, uint64_t abs, uint8_t* out, DWORD len) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)abs;
    DWORD got = 0;
    return SetFilePointerEx(hVol, li, nullptr, FILE_BEGIN) &&
           ReadFile(hVol, out, len, &got, nullptr) && got == len;
}

// Decode the data-run list of a non-resident attribute into run map.
bool ParseDataRuns(const uint8_t* a, std::vector<MftRun>& runs) {
    const uint16_t mapOff = *reinterpret_cast<const uint16_t*>(a + 32);
    const int64_t lowVcn = *reinterpret_cast<const int64_t*>(a + 16);
    const int64_t highVcn = *reinterpret_cast<const int64_t*>(a + 24);
    const uint8_t* r = a + mapOff;
    int64_t vcn = lowVcn;
    int64_t lcn = 0;
    bool first = true;
    while (*r && r < a + 512) {
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
        runs.push_back({vcn, lcn, len});
        vcn += len;
        if (vcn > highVcn) break;
    }
    return !runs.empty();
}

} // namespace

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
                              const ProgressCallback& onProgress) {
    const std::wstring normRoot = pathutil::NormalizeRoot(root);
    if (normRoot.size() < 3 || normRoot[1] != L':' || normRoot[0] == L'\\' ||
        normRoot[0] == L'/') {
        return false; // caller falls back to Win32
    }
    if (!IsSupported(normRoot)) return false;

    EnableBackupPrivileges();
    const std::wstring drive = std::wstring(1, normRoot[0]) + L":\\";
    const std::wstring devVol = L"\\\\.\\" + drive.substr(0, 2); // e.g. "\\\\.\\C:"

    // Root directory MFT record number (low 64 bits of the file id).
    HANDLE hRoot = CreateFileW(pathutil::AddLongPathPrefix(normRoot).c_str(), 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (hRoot == INVALID_HANDLE_VALUE) return false;
    uint64_t rootId = 0;
    {
        FILE_ID_INFO fid = {};
        if (!GetFileInformationByHandleEx(hRoot, FileIdInfo, &fid, sizeof(fid))) {
            CloseHandle(hRoot);
            return false;
        }
        std::memcpy(&rootId, fid.FileId.Identifier, sizeof(uint64_t));
        rootId &= 0xFFFFFFFFFFFFULL; // record number (drop sequence bits)
    }
    CloseHandle(hRoot);

    // Open the volume device and locate the MFT region.
    HANDLE hVol = CreateFileW(devVol.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE) return false;
    NTFS_VOLUME_DATA_BUFFER vd = {};
    DWORD ret = 0;
    if (!DeviceIoControl(hVol, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &vd, sizeof(vd),
                         &ret, nullptr)) {
        CloseHandle(hVol);
        return false;
    }
    const uint64_t segSize = vd.BytesPerFileRecordSegment;
    const uint64_t mftStartBytes =
        static_cast<uint64_t>(vd.MftStartLcn.QuadPart) * vd.BytesPerCluster;
    const uint64_t mftBytes = static_cast<uint64_t>(vd.MftValidDataLength.QuadPart);
    if (segSize == 0 || mftBytes == 0) {
        CloseHandle(hVol);
        return false;
    }
    const uint64_t nRecords = mftBytes / segSize;

    // ---- Build the $MFT data-run map from record 0 (the MFT may be ------
    // ---- fragmented; never read it as if it were contiguous from ---------
    // ---- MftStartLcn, that would return wrong physical records. ---------
    const uint64_t cluster = vd.BytesPerCluster;
    std::vector<uint8_t> rec0((size_t)segSize);
    if (!ReadVolAt(hVol, mftStartBytes, rec0.data(), (DWORD)segSize) ||
        !ApplyFixup(rec0.data(), rec0.size())) {
        CloseHandle(hVol);
        return false;
    }
    std::vector<MftRun> runs;
    {
        const uint16_t first = *reinterpret_cast<const uint16_t*>(rec0.data() + 20);
        const uint8_t* a = rec0.data() + first;
        const uint8_t* const end = rec0.data() + rec0.size();
        while (a + 24 <= end) {
            const uint32_t type = *reinterpret_cast<const uint32_t*>(a);
            if (type == kAttrEnd) break;
            const uint32_t len = *reinterpret_cast<const uint32_t*>(a + 4);
            if (len < 24 || a + len > end) break;
            if (type == 0x80 /* $DATA */ && a[8] != 0) ParseDataRuns(a, runs); // non-resident
            a += len;
        }
    }
    if (runs.empty()) {
        CloseHandle(hVol);
        return false;
    }
    std::sort(runs.begin(), runs.end(),
              [](const MftRun& x, const MftRun& y) { return x.startVcn < y.startVcn; });

    constexpr DWORD kMaxBuf = 8 * 1024 * 1024;
    std::vector<uint8_t> buf(kMaxBuf);

    // Iterate every MFT record, in record order, by reading each run's physical
    // clusters. `cb` receives (recordIndex, recordBytes, recordSize).
    const auto foreachRecord = [&](const auto& cb) {
        for (const auto& run : runs) {
            const uint64_t runStartMft = (uint64_t)run.startVcn * cluster; // MFT-relative byte
            const uint64_t runBytes = (uint64_t)run.len * cluster;
            uint64_t within = 0;
            while (within < runBytes) {
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

    // ---- Pass A: parent record number for every named record -------------
    std::vector<uint64_t> parentOf(nRecords, kNoParent);
    foreachRecord([&](uint64_t recIndex, uint8_t* rec, size_t n) {
        if (!ApplyFixup(rec, n)) return;
        if (!(*reinterpret_cast<const uint16_t*>(rec + 22) & 1)) return; // not in use
        ParsedRec pr;
        ParseRecord(rec, n, false, pr);
        if (!pr.hasName) return;
        parentOf[recIndex] = pr.parent;
    });

    const auto inSubtree = [&](uint64_t id) -> bool {
        uint64_t cur = id;
        for (int depth = 0; depth < 256; ++depth) {
            if (cur == rootId) return true;
            cur = parentOf[cur];
            if (cur == 0 || cur == kNoParent) return false;
        }
        return false;
    };

    // ---- Pass B: collect subtree nodes + child lists ---------------------
    struct SubNode {
        std::wstring name;
        uint64_t size = 0;
        uint64_t mtime = 0;
        uint8_t isDir = 0;
        uint8_t isReparse = 0;
    };
    std::unordered_map<uint64_t, SubNode> nodes;
    std::unordered_map<uint64_t, std::vector<uint64_t>> children;
    foreachRecord([&](uint64_t recIndex, uint8_t* rec, size_t n) {
        if (!ApplyFixup(rec, n)) return;
        if (!(*reinterpret_cast<const uint16_t*>(rec + 22) & 1)) return; // not in use
        if (!inSubtree(recIndex)) return;
        ParsedRec pr;
        ParseRecord(rec, n, true, pr);
        if (!pr.hasName) return;
        // Directory / reparse come from the MFT record header flags, which are
        // reliable (the $FILE_NAME fileAttributes field is not always set).
        const uint16_t hdr = *reinterpret_cast<const uint16_t*>(rec + 22);
        const bool isDir = (hdr & 2) != 0;
        const bool isReparse = (hdr & 4) != 0;
        SubNode sn;
        sn.name = std::move(pr.name);
        sn.size = pr.dataSize ? pr.dataSize : pr.size;
        sn.mtime = pr.mtime;
        sn.isDir = isDir ? 1 : 0;
        sn.isReparse = isReparse ? 1 : 0;
        nodes[recIndex] = std::move(sn);
        children[pr.parent].push_back(recIndex);
    });
    CloseHandle(hVol);

    // Sort each parent's child list by name so DFS pops in ascending order.
    for (auto& kv : children) {
        std::sort(kv.second.begin(), kv.second.end(), [&](uint64_t a, uint64_t b) {
            return nodes[a].name < nodes[b].name;
        });
    }

    uint64_t files = 0;
    uint64_t dirs = 0;

    struct Frame {
        uint64_t id;
        std::wstring rel; // relative path of the node's parent directory
    };
    std::vector<Frame> stack;
    if (auto it = children.find(rootId); it != children.end()) {
        for (size_t k = it->second.size(); k-- > 0;) {
            stack.push_back({it->second[k], L""});
        }
    }

    while (!stack.empty()) {
        Frame f = stack.back();
        stack.pop_back();
        const SubNode& n = nodes[f.id];
        const std::wstring childRel = pathutil::JoinRel(f.rel, n.name);

        // Volume metafiles ($MFT, $Boot, ...) that FindFirstFile doesn't surface.
        if (f.id <= kMftHighestSystem && !n.name.empty() && n.name[0] == L'$') continue;

        FileEntry e;
        e.relativePath = childRel;
        e.size = n.isDir ? 0 : n.size;
        e.lastWriteTime = n.mtime;
        e.fileId = f.id;
        e.attributes = (n.isDir ? FILE_ATTRIBUTE_DIRECTORY : 0) |
                       (n.isReparse ? FILE_ATTRIBUTE_REPARSE_POINT : 0);
        e.isDirectory = n.isDir;
        if (n.isDir) {
            ++dirs;
        } else {
            ++files;
        }
        if (!onEntry(std::move(e))) return true; // consumer aborted

        if (n.isDir && !n.isReparse) {
            if (auto cit = children.find(f.id); cit != children.end()) {
                for (size_t k = cit->second.size(); k-- > 0;) {
                    stack.push_back({cit->second[k], childRel});
                }
            }
            if (onProgress) onProgress(files, dirs, childRel);
        }
    }

    (void)onError;
    return true;
}

} // namespace bv