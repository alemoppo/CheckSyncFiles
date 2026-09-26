// MFT back-end tests: volume scans, equivalence matrix, $ATTRIBUTE_LIST
// parser, merge passes, Win32 fallback, ParseRecord ADS filtering and
// lastWriteTime source (split out of the former monolithic test_main.cpp).

// (Preamble identical to the other test TUs: full include set for safety.)
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <thread>

#include "Comparison/ScanMode.h"
#include "Comparison/SingleVerify.h"
#include "Comparison/ConcurrentComparer.h"
#include "Comparison/FileComparator.h"
#include "Comparison/HashPhase.h"
#include "Comparison/MatchTable.h"
#include "Errors.h"
#include "Export/CsvExporter.h"
#include "Export/JsonExporter.h"
#include "Filesystem/FileIndex.h"
#include "Filesystem/FileIndexSerializer.h"
#include "Filesystem/MftEnumerator.h"
#include "Filesystem/PathUtil.h"
#include "Filesystem/Win32Enumerator.h"
#include "Hashing/HashCache.h"
#include "Hashing/PartialRead.h"
#include "Hashing/Sha256.h"
#include "Hashing/HashUtil.h"
#include "Profiling/DirTiming.h"
#include "Profiling/HashProfile.h"
#include "ScanController.h"
#include "ScanOrchestrator.h"
#include "TestHarness.h"
#include "TestHelpers.h"
#include "TestTree.h"
#include "Threading/ThreadPool.h"
#include "Util/StrictNumbers.h"

namespace fs = std::filesystem;
using namespace bv;
using namespace bv::testutil;

// ---------------------------------------------------------------------------
// MFT back-end (Phase 4). Reading the MFT needs an elevated process
// (SeBackup privilege); when the test is not elevated, MftEnumerator simply
// reports "not usable" and the test is treated as skipped, not failed.

TEST("mft: IsSupported is true on NTFS roots", [] {
    const auto dir = MakeTempDir();
    CHECK(MftEnumerator::IsSupported(dir));
});

TEST("mft: enumeration matches Win32 (needs admin, else skipped)", [] {
    const auto dir = MakeTempDir();
    testgen::CreateStressTree(dir, 300); // 3 dirs x 100 files

    std::vector<std::wstring> winPaths;
    {
        Win32Enumerator en;
        const bool ok = en.enumerate(
            dir, [&](FileEntry&& e) { winPaths.push_back(e.relativePath); return true; },
            [](const ScanError&) {});
        CHECK(ok);
    }

    std::vector<std::wstring> mftPaths;
    bool mftOk = false;
    {
        MftEnumerator en;
        mftOk = en.enumerate(
            dir, [&](FileEntry&& e) { mftPaths.push_back(e.relativePath); return true; },
            [](const ScanError&) {});
    }
    if (!mftOk) {
        // Not elevated: back-end unavailable, MFT falls back to Win32 by design.
        std::cout << "  (mft non disponibile: processo non elevato, test saltato)\n";
        return;
    }

    CHECK_EQ(winPaths.size(), mftPaths.size());
    if (winPaths.size() == mftPaths.size()) {
        auto a = winPaths, b = mftPaths;
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        CHECK(a == b);
    }
});

// ---------------------------------------------------------------------------
// MFT back-end equivalence matrix (Phase 4 investigate-and-verify).
// When run elevated, both back-ends must produce the exact same index on the
// controlled fixtures below: same file/dir counts, and no path that exists on
// one side only -- nor size/type mismatches. Not elevated: skipped.

namespace {

using RefEntry = std::pair<uint64_t, bool>; // size, isDir
using RefSet = std::map<std::wstring, RefEntry>;

bool EnumerateSet(const std::wstring& root, bool useMft, RefSet& out) {
    std::unique_ptr<IFileEnumerator> en =
        useMft ? std::unique_ptr<IFileEnumerator>(new MftEnumerator())
               : std::unique_ptr<IFileEnumerator>(new Win32Enumerator());
    const bool ok = en->enumerate(
        root, [&](FileEntry&& e) {
            out[e.relativePath] = RefEntry{e.size, e.isDirectory};
            return true;
        },
        [](const ScanError&) {});
    return ok;
}

// Prints and returns the number of differences; win/mft are the two sets.
size_t CompareMftVsWin(const std::wstring& label, RefSet& win, RefSet& mft) {
    size_t wFiles = 0, wDirs = 0, mFiles = 0, mDirs = 0, onlyW = 0, onlyM = 0, mism = 0;
    for (const auto& kv : win) { (kv.second.second ? wDirs : wFiles)++; }
    for (const auto& kv : mft) { (kv.second.second ? mDirs : mFiles)++; }
    auto iw = win.begin();
    auto im = mft.begin();
    while (iw != win.end() || im != mft.end()) {
        if (im == mft.end() || (iw != win.end() && iw->first < im->first)) {
            ++onlyW;
            ++iw;
        } else if (iw == win.end() || im->first < iw->first) {
            ++onlyM;
            ++im;
        } else {
            if (iw->second != im->second) ++mism;
            ++iw;
            ++im;
        }
    }
    std::wcout << L"  [" << label << L"] win files=" << wFiles << L" dirs=" << wDirs
               << L" mft files=" << mFiles << L" dirs=" << mDirs << L" onlyWin=" << onlyW
               << L" onlyMft=" << onlyM << L" size/typeMismatch=" << mism << L"\n";
    return onlyW + onlyM + mism;
}

} // namespace

TEST("mft: controlled equivalence matrix (needs admin, else skipped)", [] {
    bool any = false;
    const auto checkRoot = [&](const std::wstring& label, const std::wstring& root) {
        RefSet win, mft;
        if (!EnumerateSet(root, false, win)) return;   // win32 must always work
        const bool mftOk = EnumerateSet(root, true, mft);
        if (!mftOk) {
            std::wcout << L"  [" << label << L"] mft non disponibile (processo non elevato?)\n";
            return;
        }
        any = true;
        const size_t diffs = CompareMftVsWin(label, win, mft);
        CHECK_MSG(diffs == 0, "mft vs win32 have differences");
    };

    // Test 1 -- simple volume: 100 directories, 5000 files.
    {
        const auto dir = MakeTempDir();
        testgen::CreateStressTree(dir, 5000);
        checkRoot(L"stress-100x5000", dir);
    }
    // Test 2 -- nested directories A/B/C/D.
    {
        const auto dir = MakeTempDir();
        testgen::CreateDeepPath(dir, 4, L"deep.txt");
        checkRoot(L"deep-nested", dir);
    }
    // Test 3 -- unicode names (accents, CJK, emoji).
    {
        const auto dir = MakeTempDir();
        const std::wstring names[] = {L"à è ì ò ù.txt", L"日本語.txt", L"中文.txt",
                                      L"emoji🙂.txt"};
        for (const auto& n : names) {
            std::ofstream(fs::path(dir) / n).put('x');
        }
        checkRoot(L"unicode", dir);
    }
    // Test 4 -- larger than 4 GiB (sparse): size must round-trip from $DATA.
    {
        const auto dir = MakeTempDir();
        const std::wstring big = dir + L"\\big.sparse";
        HANDLE hb = CreateFileW(big.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_SPARSE_FILE, nullptr);
        if (hb != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER li;
            li.QuadPart = 4LL * 1024 * 1024 * 1024 + 17; // >4GiB
            SetFilePointerEx(hb, li, nullptr, FILE_BEGIN);
            SetEndOfFile(hb);
            CloseHandle(hb);
        }
        checkRoot(L"large->4GiB", dir);
    }
    // Test 7 -- empty directories are preserved.
    {
        const auto dir = MakeTempDir();
        fs::create_directories(dir + L"\\empty1");
        fs::create_directories(dir + L"\\a\\empty2");
        checkRoot(L"empty-dirs", dir);
    }
    // Test 6 -- hard links: one record, two paths.
    {
        const auto dir = MakeTempDir();
        fs::create_directories(dir + L"\\l1");
        fs::create_directories(dir + L"\\l2");
        const std::wstring target = dir + L"\\l1\\data.txt";
        std::ofstream(fs::path(target)).put('x');
        CreateHardLinkW((dir + L"\\l2\\alias.txt").c_str(), target.c_str(), nullptr);
        checkRoot(L"hardlinks", dir);
    }
    // Test 5 -- record reuse: create, delete, recreate (same paths).
    {
        const auto dir = MakeTempDir();
        testgen::CreateStressTree(dir, 800);
        RemoveAllWin(dir);
        fs::create_directories(dir);
        testgen::CreateStressTree(dir, 800);
        checkRoot(L"reuse-800", dir);
    }
    (void)any;
});

TEST("mft: large directory reads $INDEX_ALLOCATION blocks (needs admin)", [] {
    const auto dir = MakeTempDir();
    const std::wstring big = dir + L"\\bigdir";
    fs::create_directories(big);
    // ~300 entries with 90-char names in ONE directory: guaranteed > 4096 bytes
    // of index data, forcing NTFS past the resident $INDEX_ROOT leaf.
    std::wstring suffix(90, L'z');
    for (int i = 0; i < 300; ++i) {
        wchar_t buf[24];
        wsprintfW(buf, L"f%04u", static_cast<unsigned>(i));
        std::ofstream(fs::path(big) / (std::wstring(buf) + suffix + L".txt")).put('x');
    }

    RefSet win;
    CHECK(EnumerateSet(dir, false, win));

    const std::wstring dbg = dir + L"\\_mftdiag.txt";
    SetEnvironmentVariableW(L"BV_MFT_DEBUG_FILE", dbg.c_str());
    DeleteFileW(dbg.c_str());
    RefSet mft;
    const bool mftOk = EnumerateSet(dir, true, mft);
    SetEnvironmentVariableW(L"BV_MFT_DEBUG_FILE", nullptr);
    if (!mftOk) {
        std::wcout << L"  mft non disponibile (processo non elevato), test saltato\n";
        return;
    }

    const size_t diffs = CompareMftVsWin(L"large-indx", win, mft);
    CHECK_MSG(diffs == 0, "mft vs win32 differ on large directory");

    long indxBlocks = 0;
    if (FILE* f = _wfopen(dbg.c_str(), L"r")) {
        if (std::fscanf(f, "indxBlocks=%ld", &indxBlocks) != 1) indxBlocks = -1;
        std::fclose(f);
    }
    CHECK_MSG(indxBlocks > 0, "$INDEX_ALLOCATION leaf blocks were not exercised");
});

// ---------------------------------------------------------------------------
// Audit regression (portable, no volume dependencies): the audited bug was a
// directory whose $I30 index lives only in an EXTENSION record, referenced
// from the base record via $ATTRIBUTE_LIST (the real-world "APPUNTI 2019" case:
// base record 4609 -> extension record 4613). Before the fix the parser
// reported "directory has no readable $I30 index" and silently dropped the
// directory's files.
//
// That exact layout is produced by ntfs-3g (the Linux packer used by NAS
// devices) and cannot be created from userland on modern Windows NTFS -- which
// keeps $INDEX_ROOT resident and splits into leaf blocks instead of
// externalising it (verified empirically with wide names, near-full records,
// growth/shrink and large-ACL patterns). The regression is therefore pinned to
// the parser logic itself, deterministically, on synthetic $ATTRIBUTE_LIST
// bytes (the two test-only seams below). A second portable test recreates the
// closest structure Windows CAN produce -- a directory whose index outgrew the
// resident $INDEX_ROOT and was later shrunk -- asserting MFT == Win32 whenever
// the volume is raw-readable (otherwise skipped, as every MFT test here).

namespace {

// Push one NTFS $ATTRIBUTE_LIST entry (26-byte header + UTF-16 name) into `buf`.
void PushAttrListEntry(std::vector<uint8_t>& buf, uint32_t type,
                       const std::wstring& name, uint64_t record, uint16_t seq,
                       int64_t lowestVcn) {
    const size_t start = buf.size();
    const uint8_t nameBytes = static_cast<uint8_t>(name.size() * 2);
    const uint16_t len = static_cast<uint16_t>(26 + nameBytes);
    buf.resize(start + 26 + nameBytes);
    *reinterpret_cast<uint32_t*>(buf.data() + start) = type;
    *reinterpret_cast<uint16_t*>(buf.data() + start + 4) = len;
    buf[start + 6] = nameBytes;
    buf[start + 7] = 26;
    *reinterpret_cast<int64_t*>(buf.data() + start + 8) = lowestVcn;
    *reinterpret_cast<uint64_t*>(buf.data() + start + 16) =
        record | (static_cast<uint64_t>(seq) << 48);
    if (nameBytes) std::memcpy(buf.data() + start + 26, name.c_str(), nameBytes);
}

} // namespace

TEST("mft: $ATTRIBUTE_LIST parser follows an external $I30 (synthetic)", [] {
    // Recreate the audited attribute list: SI/FN/$DATA stay in the base record
    // (4609), $INDEX_ROOT [$I30] and $INDEX_ALLOCATION [$I30] moved to the
    // extension record 4613 (lowestVcn 0 and 4 respectively).
    std::vector<uint8_t> list;
    PushAttrListEntry(list, 0x10, L"", 4609, 1, 0);                  // $STANDARD_INFORMATION
    PushAttrListEntry(list, 0x30, L"", 4609, 1, 0);                  // $FILE_NAME
    PushAttrListEntry(list, 0x90, L"$I30", 4613, 1, 0);              // $INDEX_ROOT  -> ext
    PushAttrListEntry(list, 0xA0, L"$I30", 4613, 1, 4);              // $INDEX_ALLOCATION -> ext
    PushAttrListEntry(list, 0x80, L"", 4609, 1, 0);                  // $DATA
    list.push_back(0);                                               // terminator

    std::vector<MftAttrListEntry> entries;
    CHECK(MftEnumerator::ParseAttributeListForTest(list, entries));
    CHECK_EQ(entries.size(), 5u);
    CHECK_EQ(entries[0].type, 0x10u);
    CHECK(entries[0].name.empty());
    CHECK_EQ(entries[0].record, 4609u);
    CHECK_EQ(entries[0].sequence, 1u);
    CHECK_EQ(entries[1].type, 0x30u);
    CHECK_EQ(entries[2].type, 0x90u);
    CHECK(entries[2].name == L"$I30");
    CHECK_EQ(entries[2].record, 4613u);
    CHECK_EQ(entries[2].sequence, 1u);
    CHECK_EQ(entries[2].lowestVcn, 0);
    CHECK_EQ(entries[3].type, 0xA0u);
    CHECK(entries[3].name == L"$I30");
    CHECK_EQ(entries[3].record, 4613u);
    CHECK_EQ(entries[3].lowestVcn, 4);
    CHECK_EQ(entries[4].type, 0x80u);
});

TEST("mft: $ATTRIBUTE_LIST parser is bounded on malformed data (synthetic)", [] {
    // A valid entry followed by a tail that claims more bytes than exist must
    // stop cleanly (entries so far returned, no overread, no crash).
    std::vector<uint8_t> list;
    PushAttrListEntry(list, 0x90, L"$I30", 4613, 1, 0);
    PushAttrListEntry(list, 0x30, L"", 4609, 1, 0);
    list.push_back(0x00); // truncated second entry: type present, length missing

    std::vector<MftAttrListEntry> entries;
    CHECK(MftEnumerator::ParseAttributeListForTest(list, entries));
    CHECK_EQ(entries.size(), 2u);
    CHECK_EQ(entries[0].record, 4613u);
    CHECK_EQ(entries[1].type, 0x30u);

    // Empty list -> no entries, no failure.
    std::vector<MftAttrListEntry> empty;
    CHECK(MftEnumerator::ParseAttributeListForTest({}, empty));
    CHECK(empty.empty());

    // Terminator as first entry -> nothing parsed.
    std::vector<uint8_t> term(2, 0);
    std::vector<MftAttrListEntry> t;
    CHECK(MftEnumerator::ParseAttributeListForTest(term, t));
    CHECK(t.empty());
});

TEST("mft: $INDEX_ALLOCATION piece dedupe by VCN range (synthetic)", [] {
    using Range = std::pair<int64_t, int64_t>;
    // Adjacent pieces (no overlap) must not be deduped; overlapping ones must.
    const std::vector<Range> known{{0, 3}, {8, 11}};
    CHECK(!MftEnumerator::VcnRangeKnownForTest(known, 4, 7));  // gap: new piece
    CHECK(MftEnumerator::VcnRangeKnownForTest(known, 2, 5));   // overlaps [0,3]
    CHECK(MftEnumerator::VcnRangeKnownForTest(known, 9, 9));   // inside [8,11]
    CHECK(MftEnumerator::VcnRangeKnownForTest(known, 11, 14)); // touches [8,11]
    CHECK(!MftEnumerator::VcnRangeKnownForTest(known, 12, 14));
    // A malformed range (low > high) is treated as already known (never merged).
    CHECK(MftEnumerator::VcnRangeKnownForTest(known, 6, 1));
});

TEST("mft: directory index reassembly across growth and shrink (portable)", [] {
    // Reproduce the closest structure Windows can create: a directory whose
    // index outgrew the resident $INDEX_ROOT (leaf blocks) and was later
    // shrunk -- the same history as the audited APPUNTI 2019 directory, minus
    // the ntfs-3g externalised-root layout. Exercises the $INDEX_ALLOCATION
    // stream reassembly the fix rewrote. Skipped when the volume is not raw
    // readable (the standing convention for every MFT test).
    const auto dir = MakeTempDir();
    const std::wstring big = dir + L"\\bigdir";
    fs::create_directories(big);
    std::wstring suffix(90, L'z');
    for (int i = 0; i < 1200; ++i) {
        wchar_t buf[24];
        wsprintfW(buf, L"f%04u", static_cast<unsigned>(i));
        std::ofstream(fs::path(big) / (std::wstring(buf) + suffix + L".txt")).put('x');
    }
    // Shrink to a handful of entries (delete all but 8).
    size_t remaining = 0;
    for (auto it = fs::directory_iterator(big); it != fs::directory_iterator(); ++it) {
        if (remaining >= 8) {
            std::error_code ec;
            fs::remove(it->path(), ec);
        } else {
            ++remaining;
        }
    }

    RefSet win;
    CHECK(EnumerateSet(dir, false, win));
    RefSet mft;
    const bool mftOk = EnumerateSet(dir, true, mft);
    if (!mftOk) {
        std::wcout << L"  mft non disponibile (processo non elevato), test saltato\n";
        return;
    }
    const size_t diffs = CompareMftVsWin(L"growth-shrink", win, mft);
    CHECK_MSG(diffs == 0, "mft vs win32 differ on growth/shrink directory");
});

// ---------------------------------------------------------------------------
// The synthetic tests above pin the $ATTRIBUTE_LIST *parser* (the seams
// ParseAttributeListForTest / VcnRangeKnownForTest decode raw bytes directly),
// but they do NOT traverse the merge that was actually fixed: Pass A
// (base-record reference) and Pass B ($ATTRIBUTE_LIST follow) reassemble a
// directory's $I30 into the base record before the walk resolves it. A
// regression that silently stops merging -- the audited bug -- would leave
// those parser tests green (verified by mutation check, before the fixture
// below existed). The fixture closes that gap: it feeds raw on-disk records of
// the audited layout through the SAME production merge chain (shared helpers,
// not a copy), so the suite turns red the moment any merge step regresses,
// deterministically and without volume access.

namespace {

// ---- NTFS on-disk record builders (test fixture, not production logic). ----

// $FILE_NAME value: 66-byte fixed part + UTF-16 name (namespace `ns`).
std::vector<uint8_t> BuildFileNameValue(uint64_t parentRec, uint16_t parentSeq,
                                        const std::wstring& name, uint8_t ns) {
    std::vector<uint8_t> v(66, 0);
    *reinterpret_cast<uint64_t*>(v.data()) =
        parentRec | (static_cast<uint64_t>(parentSeq) << 48);
    v[64] = static_cast<uint8_t>(name.size());
    v[65] = ns;
    const size_t off = v.size();
    v.resize(off + name.size() * 2);
    std::memcpy(v.data() + off, name.c_str(), name.size() * 2);
    return v;
}

// $FILE_NAME value with an explicit modified-time field (offset +0x10), so a
// fixture can make the $FILE_NAME timestamp distinct from $STANDARD_INFORMATION.
std::vector<uint8_t> BuildFileNameValueWithMtime(uint64_t parentRec, uint16_t parentSeq,
                                                 const std::wstring& name, uint8_t ns,
                                                 uint64_t mtime) {
    auto v = BuildFileNameValue(parentRec, parentSeq, name, ns);
    *reinterpret_cast<uint64_t*>(v.data() + 0x10) = mtime;
    return v;
}

// $STANDARD_INFORMATION value (72 bytes on modern NTFS): creation @+0x00,
// modified/last-write @+0x08, MFT changed @+0x10, accessed @+0x18. The
// modified field is the timestamp GetFileInformationByHandle() reports.
std::vector<uint8_t> BuildStandardInfoValue(uint64_t creation, uint64_t modified,
                                            uint64_t mftChanged, uint64_t accessed) {
    std::vector<uint8_t> v(72, 0);
    *reinterpret_cast<uint64_t*>(v.data() + 0x00) = creation;
    *reinterpret_cast<uint64_t*>(v.data() + 0x08) = modified;
    *reinterpret_cast<uint64_t*>(v.data() + 0x10) = mftChanged;
    *reinterpret_cast<uint64_t*>(v.data() + 0x18) = accessed;
    return v;
}

// Append one resident attribute (24-byte header + optional UTF-16 name + value).
void AppendResidentAttr(std::vector<uint8_t>& rec, uint32_t type,
                        const std::wstring& name, const std::vector<uint8_t>& value) {
    const uint32_t nameBytes = static_cast<uint32_t>(name.size() * 2);
    const uint32_t len = 24 + nameBytes + static_cast<uint32_t>(value.size());
    const size_t start = rec.size();
    rec.resize(start + len);
    *reinterpret_cast<uint32_t*>(rec.data() + start) = type;
    *reinterpret_cast<uint32_t*>(rec.data() + start + 4) = len;
    rec[start + 9] = static_cast<uint8_t>(name.size());
    *reinterpret_cast<uint16_t*>(rec.data() + start + 10) = 24;
    *reinterpret_cast<uint32_t*>(rec.data() + start + 16) = static_cast<uint32_t>(value.size());
    *reinterpret_cast<uint16_t*>(rec.data() + start + 20) = 24 + nameBytes;
    if (nameBytes) std::memcpy(rec.data() + start + 24, name.c_str(), nameBytes);
    std::memcpy(rec.data() + start + 24 + nameBytes, value.data(), value.size());
}

// 1024-byte MFT record with a valid USA fixup. The update sequence number is
// 0xFFFF and no 512-byte sector tail holds that value, so ApplyFixup finds
// nothing to restore and the record parses exactly as built. Starts as the
// 56-byte FILE_RECORD_HEADER region; attributes are appended by the caller at
// offset 56, and FinishRecord pads the record to its final 1024-byte size.
std::vector<uint8_t> BuildFileRecord(uint64_t recNo, uint16_t seq, uint16_t flags,
                                     uint64_t baseRef) {
    std::vector<uint8_t> rec(0x38, 0);
    std::memcpy(rec.data(), "FILE", 4);
    *reinterpret_cast<uint16_t*>(rec.data() + 4) = 0x30;  // USA offset
    *reinterpret_cast<uint16_t*>(rec.data() + 6) = 3;     // USA count (2 sectors + 1)
    *reinterpret_cast<uint16_t*>(rec.data() + 16) = seq;
    *reinterpret_cast<uint16_t*>(rec.data() + 20) = 0x38; // first attribute offset
    *reinterpret_cast<uint16_t*>(rec.data() + 22) = flags;
    *reinterpret_cast<uint64_t*>(rec.data() + 32) = baseRef;
    *reinterpret_cast<uint16_t*>(rec.data() + 42) = static_cast<uint16_t>(recNo);
    *reinterpret_cast<uint16_t*>(rec.data() + 0x30) = 0xFFFF; // USA sequence number
    return rec;
}

// Append the end-of-attribute marker, pad the record to 1024 bytes and finalize
// the size fields (record size at +28 is what ApplyFixup reads as the fixup
// region span).
void FinishRecord(std::vector<uint8_t>& rec) {
    const size_t used = rec.size();
    const size_t start = rec.size();
    rec.resize(start + 4, 0);
    *reinterpret_cast<uint32_t*>(rec.data() + start) = 0xFFFFFFFFu;
    rec.resize(1024, 0);
    *reinterpret_cast<uint32_t*>(rec.data() + 24) = static_cast<uint32_t>(used + 4);
    *reinterpret_cast<uint32_t*>(rec.data() + 28) = 1024; // allocated / record size
}

// Resident $INDEX_ROOT [$I30] value: 0x10 INDEX_ROOT header + 0x10 INDEX_HEADER
// + three leaf entries + end-of-node marker.
std::vector<uint8_t> BuildIndexRootValue() {
    std::vector<uint8_t> v(0x20, 0);
    *reinterpret_cast<uint32_t*>(v.data() + 0) = 0x30; // indexed attr type: $FILE_NAME
    *reinterpret_cast<uint32_t*>(v.data() + 4) = 0x01; // collation rule: FILENAME
    *reinterpret_cast<uint32_t*>(v.data() + 8) = 4096; // index_block_size
    *reinterpret_cast<uint32_t*>(v.data() + 12) = 1;   // clusters_per_index_block
    *reinterpret_cast<uint32_t*>(v.data() + 16) = 16;  // entries offset (rel. INDEX_HEADER)
    const auto addEntry = [&](uint64_t childRef, const std::wstring& name, uint16_t flags) {
        std::vector<uint8_t> key(66, 0);
        key[64] = static_cast<uint8_t>(name.size());
        key[65] = 1; // WIN32 namespace
        const size_t keyOff = key.size();
        key.resize(keyOff + name.size() * 2);
        std::memcpy(key.data() + keyOff, name.c_str(), name.size() * 2);
        const uint16_t klen = static_cast<uint16_t>(key.size());
        const uint16_t elen = static_cast<uint16_t>(16 + klen);
        const size_t start = v.size();
        v.resize(start + elen);
        *reinterpret_cast<uint64_t*>(v.data() + start) = childRef;
        *reinterpret_cast<uint16_t*>(v.data() + start + 8) = elen;
        *reinterpret_cast<uint16_t*>(v.data() + start + 10) = klen;
        *reinterpret_cast<uint16_t*>(v.data() + start + 12) = flags;
        std::memcpy(v.data() + start + 16, key.data(), klen);
    };
    addEntry(4610ull | (1ull << 48), L"file1.txt", 0);
    addEntry(4611ull | (1ull << 48), L"file2.txt", 0);
    addEntry(4612ull | (1ull << 48), L"sub", 0);
    addEntry(0, L"", 0x0002); // end-of-node marker
    *reinterpret_cast<uint32_t*>(v.data() + 20) =
        static_cast<uint32_t>(v.size() - 16); // index_length (rel. INDEX_HEADER)
    return v;
}

// Like BuildIndexRootValue but with an arbitrary child set (child file reference
// -> name), each recorded as a single $INDEX_ROOT [$I30] entry. Used to drive
// the MFT walk's per-directory fallback decision deterministically: a child
// reference that does not resolve to a live record forces needWin32Fallback.
std::vector<uint8_t> BuildIndexRootValueWith(
    const std::vector<std::pair<uint64_t, std::wstring>>& children) {
    std::vector<uint8_t> v(0x20, 0);
    *reinterpret_cast<uint32_t*>(v.data() + 0) = 0x30; // indexed attr: $FILE_NAME
    *reinterpret_cast<uint32_t*>(v.data() + 4) = 0x01; // collation: FILENAME
    *reinterpret_cast<uint32_t*>(v.data() + 8) = 4096; // index_block_size
    *reinterpret_cast<uint32_t*>(v.data() + 12) = 1;   // clusters_per_index_block
    *reinterpret_cast<uint32_t*>(v.data() + 16) = 16;  // entries offset (rel. INDEX_HEADER)
    const auto addEntry = [&](uint64_t childRef, const std::wstring& name, uint16_t flags) {
        std::vector<uint8_t> key(66, 0);
        key[64] = static_cast<uint8_t>(name.size());
        key[65] = 1; // WIN32 namespace
        const size_t keyOff = key.size();
        key.resize(keyOff + name.size() * 2);
        std::memcpy(key.data() + keyOff, name.c_str(), name.size() * 2);
        const uint16_t klen = static_cast<uint16_t>(key.size());
        const uint16_t elen = static_cast<uint16_t>(16 + klen);
        const size_t start = v.size();
        v.resize(start + elen);
        *reinterpret_cast<uint64_t*>(v.data() + start) = childRef;
        *reinterpret_cast<uint16_t*>(v.data() + start + 8) = elen;
        *reinterpret_cast<uint16_t*>(v.data() + start + 10) = klen;
        *reinterpret_cast<uint16_t*>(v.data() + start + 12) = flags;
        std::memcpy(v.data() + start + 16, key.data(), klen);
    };
    for (const auto& c : children) addEntry(c.first, c.second, 0);
    addEntry(0, L"", 0x0002); // end-of-node marker
    *reinterpret_cast<uint32_t*>(v.data() + 20) =
        static_cast<uint32_t>(v.size() - 16); // index_length (rel. INDEX_HEADER)
    return v;
}

// The audited base record 4609 $ATTRIBUTE_LIST: its $I30 index root lives ONLY
// in extension record 4613.
std::vector<uint8_t> BuildAttrList() {
    std::vector<uint8_t> list;
    PushAttrListEntry(list, 0x10, L"", 4609, 1, 0);     // $STANDARD_INFORMATION
    PushAttrListEntry(list, 0x30, L"", 4609, 1, 0);     // $FILE_NAME
    PushAttrListEntry(list, 0x90, L"$I30", 4613, 1, 0); // $INDEX_ROOT -> extension
    PushAttrListEntry(list, 0x80, L"", 4609, 1, 0);     // $DATA
    return list;
}

// Raw records of the audited layout: base 4609 (dir, seq 1) carries the
// $ATTRIBUTE_LIST; extension 4613 (base reference 4609) holds the $INDEX_ROOT
// [$I30]; children 4610/4611/4612 are files. Mirrors the real "APPUNTI 2019"
// case (E:\nas_4tb_1\...\APPUNTI 2019, records 4609..4613) with the list kept
// resident so the chain runs without volume access.
std::map<uint64_t, std::vector<uint8_t>> BuildAuditedFixture() {
    std::map<uint64_t, std::vector<uint8_t>> records;

    auto base = BuildFileRecord(4609, 1, 0x0003, 0); // in use + directory
    AppendResidentAttr(base, 0x30, L"", BuildFileNameValue(4608, 1, L"APPUNTI 2019", 1));
    AppendResidentAttr(base, 0x20, L"", BuildAttrList());
    FinishRecord(base);
    records[4609] = std::move(base);

    auto ext = BuildFileRecord(4613, 1, 0x0003, 4609ull | (1ull << 48)); // extension of 4609
    AppendResidentAttr(ext, 0x90, L"$I30", BuildIndexRootValue());
    FinishRecord(ext);
    records[4613] = std::move(ext);

    const auto mkFile = [&](uint64_t recNo, const wchar_t* name) {
        auto r = BuildFileRecord(recNo, 1, 0x0001, 0);
        AppendResidentAttr(r, 0x30, L"", BuildFileNameValue(4609, 1, name, 1));
        FinishRecord(r);
        records[recNo] = std::move(r);
    };
    mkFile(4610, L"file1.txt");
    mkFile(4611, L"file2.txt");
    mkFile(4612, L"sub");
    return records;
}

// ---- $ATTRIBUTE_LIST -> external NON-RESIDENT $INDEX_ALLOCATION fixture ----

// Append one NON-RESIDENT attribute (0x40 header + optional UTF-16 name + a
// single-run mapping-pairs list) to `rec`. `lcn` is the piece's absolute first
// LCN (fits one signed byte; run length 1 cluster); lowVcn..highVcn is the
// piece's VCN range; `dataSize` is the attribute's logical data length -- the
// VCN-0 extent carries the WHOLE stream's size, as NTFS does for a split
// attribute (ReadIndexAllocationStream reads the real size from the VCN-0
// piece's header).
void AppendNonResidentAttr(std::vector<uint8_t>& rec, uint32_t type,
                           const std::wstring& name, int64_t lowVcn, int64_t highVcn,
                           uint32_t lcn, uint64_t dataSize) {
    const uint32_t nameBytes = static_cast<uint32_t>(name.size() * 2);
    const uint32_t mapOff = 0x40 + nameBytes;
    const uint32_t len = mapOff + 4; // header + name + 1-run list + terminator
    const size_t start = rec.size();
    rec.resize(start + len, 0);
    *reinterpret_cast<uint32_t*>(rec.data() + start) = type;
    *reinterpret_cast<uint32_t*>(rec.data() + start + 4) = len;
    rec[start + 8] = 0x40; // non-resident form code
    rec[start + 9] = static_cast<uint8_t>(name.size());
    *reinterpret_cast<uint16_t*>(rec.data() + start + 10) = 0x40; // name offset
    *reinterpret_cast<int64_t*>(rec.data() + start + 0x10) = lowVcn;
    *reinterpret_cast<int64_t*>(rec.data() + start + 0x18) = highVcn;
    *reinterpret_cast<uint16_t*>(rec.data() + start + 0x20) = static_cast<uint16_t>(mapOff);
    *reinterpret_cast<uint64_t*>(rec.data() + start + 0x28) =
        static_cast<uint64_t>(highVcn - lowVcn + 1) * 4096; // allocated size
    *reinterpret_cast<uint64_t*>(rec.data() + start + 0x30) = dataSize; // real size
    *reinterpret_cast<uint64_t*>(rec.data() + start + 0x38) = dataSize; // valid data
    if (nameBytes) std::memcpy(rec.data() + start + 0x40, name.c_str(), nameBytes);
    uint8_t* mp = rec.data() + start + mapOff;
    mp[0] = 0x11;                    // lenb=1, offb=1
    mp[1] = 1;                       // run length: 1 cluster
    mp[2] = static_cast<uint8_t>(lcn);
    mp[3] = 0x00;                    // end of mapping pairs
}

// Resident $INDEX_ROOT [$I30] value with an EMPTY root node: every child lives
// in the $INDEX_ALLOCATION leaves. Carries the directory's index_block_size.
std::vector<uint8_t> BuildEmptyIndexRootValue() {
    std::vector<uint8_t> v(0x30, 0);
    *reinterpret_cast<uint32_t*>(v.data() + 0) = 0x30; // indexed attr: $FILE_NAME
    *reinterpret_cast<uint32_t*>(v.data() + 4) = 0x01; // collation: FILENAME
    *reinterpret_cast<uint32_t*>(v.data() + 8) = 4096; // index_block_size
    *reinterpret_cast<uint32_t*>(v.data() + 12) = 1;   // clusters_per_index_block
    *reinterpret_cast<uint32_t*>(v.data() + 16) = 16;  // entries offset (rel. INDEX_HEADER)
    *reinterpret_cast<uint32_t*>(v.data() + 20) = 16;  // index_length: just the marker
    *reinterpret_cast<uint16_t*>(v.data() + 0x28) = 16;     // marker elen
    *reinterpret_cast<uint16_t*>(v.data() + 0x2A) = 0;      // marker klen
    *reinterpret_cast<uint16_t*>(v.data() + 0x2C) = 0x0002; // marker flags: last entry
    return v;
}

// A 4096-byte $INDEX_ALLOCATION INDX leaf block holding `entries`. The block
// carries a valid USA fixup: every 512-byte sector tail holds the USN (0xFFFF)
// and the true bytes live in the update-sequence array at 0x28, so
// UndoFixupIndexBlock accepts it and restores the tails. The INDEX_HEADER sits
// at block+0x18 as the production parser expects.
std::vector<uint8_t> BuildIndxBlock(
    uint64_t vcn, const std::vector<std::pair<uint64_t, std::wstring>>& entries) {
    std::vector<uint8_t> b(4096, 0);
    std::memcpy(b.data(), "INDX", 4);
    *reinterpret_cast<uint16_t*>(b.data() + 4) = 0x28; // usa_ofs
    *reinterpret_cast<uint16_t*>(b.data() + 6) = 9;    // usa_count (USN + 8 sectors)
    *reinterpret_cast<uint64_t*>(b.data() + 0x10) = vcn; // index block VCN
    *reinterpret_cast<uint16_t*>(b.data() + 0x28) = 0xFFFF; // USN
    for (uint32_t i = 1; i <= 8; ++i) {
        *reinterpret_cast<uint16_t*>(b.data() + i * 512 - 2) = 0xFFFF;
    }
    *reinterpret_cast<uint32_t*>(b.data() + 0x18) = 0x28; // entries offset (rel. node)
    size_t nodeLen = 0x28;
    size_t pos = 0x40;
    const auto addEntry = [&](uint64_t childRef, const std::wstring& name, uint16_t flags) {
        std::vector<uint8_t> key(66, 0);
        key[64] = static_cast<uint8_t>(name.size());
        key[65] = 1; // WIN32 namespace
        const size_t keyOff = key.size();
        key.resize(keyOff + name.size() * 2);
        std::memcpy(key.data() + keyOff, name.c_str(), name.size() * 2);
        const uint16_t klen = static_cast<uint16_t>(key.size());
        const uint16_t elen = static_cast<uint16_t>(16 + klen);
        *reinterpret_cast<uint64_t*>(b.data() + pos) = childRef;
        *reinterpret_cast<uint16_t*>(b.data() + pos + 8) = elen;
        *reinterpret_cast<uint16_t*>(b.data() + pos + 10) = klen;
        *reinterpret_cast<uint16_t*>(b.data() + pos + 12) = flags;
        std::memcpy(b.data() + pos + 16, key.data(), klen);
        pos += elen;
        nodeLen += elen;
    };
    for (const auto& e : entries) addEntry(e.first, e.second, 0);
    addEntry(0, L"", 0x0002); // end-of-node marker
    *reinterpret_cast<uint32_t*>(b.data() + 0x1C) = static_cast<uint32_t>(nodeLen);
    *reinterpret_cast<uint32_t*>(b.data() + 0x20) = static_cast<uint32_t>(nodeLen);
    return b;
}

// Base record 4613's $ATTRIBUTE_LIST for the external-$INDEX_ALLOCATION layout:
// the $I30 index root lives in extension 4609, the VCN-0 $INDEX_ALLOCATION piece
// lives in extension 4609 too (BELOW the base, so Pass A's ascending merge
// cannot reach it -- VCN 0 is reachable ONLY through this list, i.e. Pass B).
// The VCN-0 piece is listed TWICE so the test can prove VcnRangeKnown dedupes
// real duplicates instead of merging a second copy. VCN 1 (extension 4614, ABOVE
// the base) is NOT listed: it is reachable ONLY via Pass A's base-record-ref
// merge. The two merge passes are therefore each indispensable and disjoint --
// disabling either one leaves exactly half the tree.
std::vector<uint8_t> BuildExternalIaList() {
    std::vector<uint8_t> list;
    PushAttrListEntry(list, 0x10, L"", 4613, 1, 0);     // $STANDARD_INFORMATION
    PushAttrListEntry(list, 0x30, L"", 4613, 1, 0);     // $FILE_NAME
    PushAttrListEntry(list, 0x90, L"$I30", 4609, 1, 0); // $INDEX_ROOT -> extension 4609
    PushAttrListEntry(list, 0xA0, L"$I30", 4609, 1, 0); // $INDEX_ALLOCATION VCN 0
    PushAttrListEntry(list, 0xA0, L"$I30", 4609, 1, 0); // duplicate VCN 0 entry
    PushAttrListEntry(list, 0x80, L"", 4613, 1, 0);     // $DATA
    list.push_back(0); // terminator
    return list;
}

struct ExternalIaFixture {
    std::map<uint64_t, std::vector<uint8_t>> records;
    std::vector<uint8_t> clusters; // in-memory "volume": INDX blocks by LCN
};

// Records + INDX data of the external NON-RESIDENT $INDEX_ALLOCATION layout.
//
//   base 4613 (dir, seq 1): $ATTRIBUTE_LIST only, NO inline $I30
//      |-- ext 4609 (base-ref 4613, seq 1): $INDEX_ROOT [$I30] (empty root)
//      |     + $INDEX_ALLOCATION [$I30] VCN 0..0 (leaf block: file1/file2)
//      `-- ext 4614 (base-ref 4613, seq 1): $INDEX_ALLOCATION [$I30] VCN 1..1
//            (leaf block: file3/file4)
//
// Deliberate, documented asymmetry: ext 4609 sits BELOW the base, so Pass A's
// ascending-order merge (the base must already be parsed when its extension is
// seen) cannot reach it -- VCN 0 is reachable ONLY through the $ATTRIBUTE_LIST
// (Pass B). ext 4614 sits ABOVE the base and is NOT listed, so VCN 1 is
// reachable ONLY through Pass A's base-record-reference merge. The two passes
// are disjoint and each indispensable: disabling either one leaves exactly half
// the tree (the fixture cannot be "saved" by the other pass). Children are
// plain files 4620..4623; the leaf blocks live in `clusters` at LCN 100 (VCN 0)
// and LCN 60 (VCN 1).
ExternalIaFixture BuildExternalIaFixture() {
    ExternalIaFixture fx;

    auto base = BuildFileRecord(4613, 1, 0x0003, 0); // in use + directory
    AppendResidentAttr(base, 0x30, L"", BuildFileNameValue(4600, 1, L"EXT_IA_DIR", 1));
    AppendResidentAttr(base, 0x20, L"", BuildExternalIaList());
    FinishRecord(base);
    fx.records[4613] = std::move(base);

    auto extLow = BuildFileRecord(4609, 1, 0x0003, 4613ull | (1ull << 48));
    AppendResidentAttr(extLow, 0x90, L"$I30", BuildEmptyIndexRootValue());
    AppendNonResidentAttr(extLow, 0xA0, L"$I30", 0, 0, 100, 8192);
    FinishRecord(extLow);
    fx.records[4609] = std::move(extLow);

    auto extHigh = BuildFileRecord(4614, 1, 0x0003, 4613ull | (1ull << 48));
    AppendNonResidentAttr(extHigh, 0xA0, L"$I30", 1, 1, 60, 8192);
    FinishRecord(extHigh);
    fx.records[4614] = std::move(extHigh);

    const auto mkFile = [&](uint64_t recNo, const wchar_t* name) {
        auto r = BuildFileRecord(recNo, 1, 0x0001, 0);
        AppendResidentAttr(r, 0x30, L"", BuildFileNameValue(4613, 1, name, 1));
        FinishRecord(r);
        fx.records[recNo] = std::move(r);
    };
    mkFile(4620, L"file1.txt");
    mkFile(4621, L"file2.txt");
    mkFile(4622, L"file3.txt");
    mkFile(4623, L"file4.txt");

    const uint32_t kCluster = 4096;
    fx.clusters.assign(512 * kCluster, 0);
    auto blk0 = BuildIndxBlock(0, {{4620ull | (1ull << 48), L"file1.txt"},
                                   {4621ull | (1ull << 48), L"file2.txt"}});
    std::memcpy(fx.clusters.data() + 100 * kCluster, blk0.data(), blk0.size());
    auto blk1 = BuildIndxBlock(1, {{4622ull | (1ull << 48), L"file3.txt"},
                                   {4623ull | (1ull << 48), L"file4.txt"}});
    std::memcpy(fx.clusters.data() + 60 * kCluster, blk1.data(), blk1.size());
    return fx;
}

// ---- Pass B $FILE_NAME / $STANDARD_INFORMATION extension fixtures ----------

// $STANDARD_INFORMATION modified time relocated into the extension record of the
// inverted-order fixture (distinct from the $FILE_NAME mtime, so the test can
// prove the SI value, not the FN value, reaches the base).
const uint64_t kInvertedSiModified = 0x1122334455667788ull;

// Fixture for the inverted-order Pass B regression: the BASE record has a HIGHER
// record number than its EXTENSION, so Pass A's ascending streaming merge (the
// base must already be parsed when its extension is seen) cannot fire. Only
// Pass B's $ATTRIBUTE_LIST follow can recover the relocated attributes.
//
//   dir  5000 (dir, seq 1): $I30 keys the base 5002 as "index_key_name"
//   ext  5001 (seq 1, base-ref 5002): $FILE_NAME(parent 5000, "real_name.bin",
//                                   ns 1, distinct mtime) + $STANDARD_INFORMATION
//   base 5002 (file, seq 1): $DATA 128 resident, NO inline $FILE_NAME / SI,
//                            $ATTRIBUTE_LIST: 0x30+0x10 -> ext 5001
//
// The index key "index_key_name" deliberately differs from the $FILE_NAME
// ("real_name.bin") so the name the directory resolves is observable: only a
// successful Pass B merge makes ChildNameOf prefer the record's own WIN32
// $FILE_NAME over the index key.
std::map<uint64_t, std::vector<uint8_t>> BuildInvertedOrderFixture() {
    const uint64_t dir = 5000;
    const uint64_t ext = 5001;
    const uint64_t base = 5002;
    const uint64_t baseRef = base | (1ull << 48);
    std::map<uint64_t, std::vector<uint8_t>> records;

    auto d = BuildFileRecord(dir, 1, 0x0003, 0); // in use + directory
    AppendResidentAttr(d, 0x30, L"", BuildFileNameValue(4999, 1, L"dir5000", 1));
    AppendResidentAttr(d, 0x90, L"$I30",
                       BuildIndexRootValueWith({{baseRef, L"index_key_name"}}));
    FinishRecord(d);
    records[dir] = std::move(d);

    auto e = BuildFileRecord(ext, 1, 0x0001, baseRef); // extension of 5002
    AppendResidentAttr(e, 0x30, L"",
                       BuildFileNameValueWithMtime(dir, 1, L"real_name.bin", 1, 0xABCDEF));
    AppendResidentAttr(e, 0x10, L"",
                       BuildStandardInfoValue(0x1111, kInvertedSiModified, 0x2222, 0x3333));
    FinishRecord(e);
    records[ext] = std::move(e);

    auto b = BuildFileRecord(base, 1, 0x0001, 0); // in use + file
    AppendResidentAttr(b, 0x80, L"", std::vector<uint8_t>(128, 0xAB));
    std::vector<uint8_t> list;
    PushAttrListEntry(list, 0x30, L"", ext, 1, 0); // $FILE_NAME -> extension 5001
    PushAttrListEntry(list, 0x10, L"", ext, 1, 0); // $STANDARD_INFORMATION -> 5001
    PushAttrListEntry(list, 0x80, L"", base, 1, 0); // $DATA -> itself (skipped)
    list.push_back(0);
    AppendResidentAttr(b, 0x20, L"", list);
    FinishRecord(b);
    records[base] = std::move(b);
    return records;
}

// Fixture where BOTH merge passes reach the same extension record: base 5002
// (LOWER number, parsed first) initially carries NO $FILE_NAME, so when the
// extension 5003 (HIGHER number) is parsed Pass A DOES append "name.bin" to it
// (the base is already parsed). The base's $ATTRIBUTE_LIST then points Pass B
// at the same extension, so Pass B re-encounters the SAME name already present
// and must reject it. After Pass A + Pass B the base holds exactly ONE copy --
// only a name that Pass A added and Pass B re-sees can prove both passes ran.
//
//   dir  5000 (dir): $I30 -> base 5002 "name.bin"
//   base 5002 (file): NO inline $FILE_NAME; $DATA 128 resident
//                     + $ATTRIBUTE_LIST: 0x30 -> ext 5003
//   ext  5003 (base-ref 5002): $FILE_NAME(parent 5000, "name.bin", ns 1)
std::map<uint64_t, std::vector<uint8_t>> BuildPassAThenBFixture() {
    const uint64_t dir = 5000;
    const uint64_t base = 5002;
    const uint64_t ext = 5003;
    const uint64_t baseRef = base | (1ull << 48);
    std::map<uint64_t, std::vector<uint8_t>> records;

    auto d = BuildFileRecord(dir, 1, 0x0003, 0); // in use + directory
    AppendResidentAttr(d, 0x30, L"", BuildFileNameValue(4999, 1, L"dir5000", 1));
    AppendResidentAttr(d, 0x90, L"$I30", BuildIndexRootValueWith({{baseRef, L"name.bin"}}));
    FinishRecord(d);
    records[dir] = std::move(d);

    auto e = BuildFileRecord(ext, 1, 0x0001, baseRef); // extension of 5002
    AppendResidentAttr(e, 0x30, L"", BuildFileNameValue(dir, 1, L"name.bin", 1));
    FinishRecord(e);
    records[ext] = std::move(e);

    auto b = BuildFileRecord(base, 1, 0x0001, 0); // in use + file, NO $FILE_NAME yet
    AppendResidentAttr(b, 0x80, L"", std::vector<uint8_t>(128, 0xAB));
    std::vector<uint8_t> list;
    PushAttrListEntry(list, 0x30, L"", ext, 1, 0); // $FILE_NAME -> extension 5003
    PushAttrListEntry(list, 0x80, L"", base, 1, 0); // $DATA -> itself (skipped)
    list.push_back(0);
    AppendResidentAttr(b, 0x20, L"", list);
    FinishRecord(b);
    records[base] = std::move(b);
    return records;
}
