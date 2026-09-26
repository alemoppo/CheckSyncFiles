// Full-scan integration tests (split out of the former monolithic test_main.cpp).

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
// Full scans
// ---------------------------------------------------------------------------

TEST("identical tree: everything identical, no errors", [] {
    const auto dir = MakeTempDir();
    testgen::CreateFixture(dir);
    const auto r = RunScan(dir, dir, ScanMode::Presence);
    const auto& s = r.results.stats;
    CHECK_EQ(s.identicalFiles, 6ull);
    CHECK_EQ(s.identicalDirs, 24ull);
    CHECK_EQ(s.missingFiles, 0ull);
    CHECK_EQ(s.extraFiles, 0ull);
    CHECK_EQ(s.sizeMismatch, 0ull);
    CHECK_EQ(s.readErrors + s.accessDenied, 0ull);
    CHECK(r.results.problems.empty());
});

TEST("presence mode: missing and extra detected", [] {
    const auto dir = MakeTempDir();
    testgen::CreateDifferingTrees(dir);
    const auto r = RunScan(dir + L"\\src", dir + L"\\dst", ScanMode::Presence);
    const auto& s = r.results.stats;
    CHECK_EQ(s.identicalFiles, 4ull);   // a, d, e, sub\f
    CHECK_EQ(s.identicalDirs, 1ull);    // sub
    CHECK_EQ(s.missingFiles, 1ull);     // b_missing
    CHECK_EQ(s.missingDirs, 1ull);      // empty_src
    CHECK_EQ(s.extraFiles, 1ull);       // c_extra
    CHECK_EQ(s.extraDirs, 1ull);        // empty_dst
    CHECK_EQ(s.sizeMismatch, 0ull);     // presence does not compare sizes
    CHECK_EQ(s.readErrors + s.accessDenied, 0ull);
});

TEST("size mode: size mismatch detected, same-size treated identical", [] {
    const auto dir = MakeTempDir();
    testgen::CreateDifferingTrees(dir);
    const auto r = RunScan(dir + L"\\src", dir + L"\\dst", ScanMode::Size);
    const auto& s = r.results.stats;
    CHECK_EQ(s.identicalFiles, 3ull);   // a, e (same size!), sub\f
    CHECK_EQ(s.sizeMismatch, 1ull);     // d
    CHECK_EQ(s.missingFiles, 1ull);
    CHECK_EQ(s.extraFiles, 1ull);
    CHECK_EQ(s.missingDirs, 1ull);
    CHECK_EQ(s.extraDirs, 1ull);
});

TEST("content mode: same size + different content detected", [] {
    const auto dir = MakeTempDir();
    testgen::CreateDifferingTrees(dir);
    const auto r = RunScan(dir + L"\\src", dir + L"\\dst", ScanMode::Content);
    const auto& s = r.results.stats;
    CHECK_EQ(s.identicalFiles, 2ull);   // a, sub\f (hashed and equal)
    CHECK_EQ(s.contentMismatch, 1ull);  // e: same size, different bytes
    CHECK_EQ(s.sizeMismatch, 1ull);     // d
    CHECK_EQ(s.missingFiles, 1ull);
    CHECK_EQ(s.extraFiles, 1ull);
    CHECK_EQ(s.missingDirs, 1ull);
    CHECK_EQ(s.extraDirs, 1ull);
    CHECK_EQ(r.hashThreadsUsed, 2ull);
});

TEST("content mode: identical tree hashes every file and finds nothing", [] {
    const auto dir = MakeTempDir();
    testgen::CreateFixture(dir);
    const auto r = RunScan(dir, dir, ScanMode::Content);
    const auto& s = r.results.stats;
    CHECK_EQ(s.identicalFiles, 6ull);
    CHECK_EQ(s.identicalDirs, 24ull);
    CHECK_EQ(s.contentMismatch, 0ull);
    CHECK_EQ(s.readErrors + s.accessDenied, 0ull);
    CHECK(r.results.problems.empty());
    CHECK_EQ(r.hashThreadsUsed, 2ull);
});

TEST("content mode: auto thread count resolves from the IO class", [] {
    const auto dir = MakeTempDir();
    testgen::CreateDifferingTrees(dir);
    const auto r = RunScan(dir + L"\\src", dir + L"\\dst", ScanMode::Content,
                           false, 0); // auto
    CHECK(r.hashThreadsUsed >= 1u);
});

TEST("junction: listed as an entry but never descended into (no loops)", [] {
    // A directory junction pointing back at its own root is the canonical
    // cycle: following it would re-enumerate the whole tree forever. The
    // enumerator must record it as a single directory entry and treat it as a
    // leaf. Junctions require no administrator rights, so this runs on a plain
    // temp directory (NTFS).
    const auto dir = MakeTempDir();
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";

    // src\sub\loop -> src (a loop back to the root of the scan)
    fs::create_directories(src);
    fs::create_directories(dst);
    fs::create_directories(src + L"\\sub");
    fs::create_directories(dst + L"\\sub");
    WriteFileBytes(src + L"\\a.txt", "same", 4);
    WriteFileBytes(dst + L"\\a.txt", "same", 4);
    WriteFileBytes(src + L"\\sub\\f.txt", "f", 1);
    WriteFileBytes(dst + L"\\sub\\f.txt", "f", 1);
    CHECK_MSG(CreateJunction(src + L"\\sub\\loop", src), "junction creation failed");

    // Direct enumeration: the junction is a directory entry ...
    {
        FileIndex idx(false);
        Win32Enumerator en;
        const auto br = idx.build(src, en);
        CHECK(br.ok);
        FileEntry e;
        CHECK(idx.find(L"sub\\loop", e));
        CHECK(e.isDirectory);
        CHECK((e.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
        // ... but it is never descended into: nothing exists below it, and the
        // entry set is exactly the tree's own files/dirs (a followed cycle
        // would add "sub\loop\a.txt", "sub\loop\sub", ... indefinitely).
        CHECK(!idx.find(L"sub\\loop\\a.txt", e));
        CHECK(!idx.find(L"sub\\loop\\sub", e));
        CHECK_EQ(br.stats.files, 2ull); // a.txt, sub\f.txt
        CHECK_EQ(br.stats.dirs, 2ull);  // sub, sub\loop
    }

    // End-to-end: a presence scan against a tree without the junction
    // terminates (no infinite loop) and reports exactly the junction as a
    // missing directory, with no missing files leaked from its contents.
    const auto r = RunScan(src, dst, ScanMode::Presence);
    const auto& s = r.results.stats;
    CHECK_EQ(s.identicalFiles, 2ull); // a.txt, sub\f.txt
    CHECK_EQ(s.identicalDirs, 1ull);  // sub
    CHECK_EQ(s.missingDirs, 1ull);    // sub\loop
    CHECK_EQ(s.missingFiles, 0ull);   // the junction's contents were never read
    CHECK_EQ(s.extraFiles, 0ull);
    CHECK_EQ(s.extraDirs, 0ull);
});

TEST("sha256: known vector for 'abc'", [] {
    const auto dir = MakeTempDir();
    const std::wstring path = dir + L"\\abc.bin";
    {
        const std::wstring win = pathutil::AddLongPathPrefix(path);
        HANDLE h = CreateFileW(win.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            const char data[] = "abc";
            ::WriteFile(h, data, sizeof(data) - 1, &w, nullptr);
            CloseHandle(h);
        }
    }
    std::array<uint8_t, 32> d;
    CHECK(hashing::Sha256File(path, d) == hashing::HashStatus::Ok);
    const char* expected =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    for (size_t i = 0; i < d.size(); ++i) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", d[i]);
        CHECK(hex[0] == expected[i * 2] && hex[1] == expected[i * 2 + 1]);
    }
});

TEST("cancel: mid-file hash is aborted by the cancel flag (not a read error)", [] {
    // A hashing pool job that is mid-flight inside Sha256File() when the user
    // presses Interrompi must stop within ~1 MiB (the chunk granularity the cancel
    // flag is polled at) and must NOT be reported as a read error or an Ok digest
    // -- exactly the "no verdict for a cancelled job" rule the comparer enforces.
    const auto dir = MakeTempDir();
    const std::wstring path = dir + L"\\big.bin";
    // 64 MiB: large enough that the 1 MiB read loop runs several iterations even
    // on a fast SSD, so a cancel set ~5 ms in is observed mid-read.
    const DWORD kSize = 64 * 1024 * 1024;
    {
        const std::wstring win = pathutil::AddLongPathPrefix(path);
        HANDLE h = CreateFileW(win.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) {
            std::vector<uint8_t> buf(1024 * 1024, 0xAA); // 1 MiB pattern
            for (DWORD off = 0; off + buf.size() <= kSize; off += buf.size()) {
                DWORD w = 0;
                WriteFile(h, buf.data(), static_cast<DWORD>(buf.size()), &w, nullptr);
            }
            CloseHandle(h);
        }
    }

    const std::wstring cachePath = dir + L"\\hash_cache.bin";
    std::wstring cacheErr;
    hashing::HashCache cache(cachePath, cacheErr);
    CHECK(cacheErr.empty());
    std::atomic<size_t> hits{0};

    // Flip cancel shortly after hashing starts. Because the read loop polls the
    // flag per 1 MiB chunk, the job must abort (not finish) and report Cancelled.
    std::atomic_bool cancel{false};
    std::thread armer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        cancel.store(true, std::memory_order_release);
    });

    std::array<uint8_t, 32> digest{};
    hashing::HashStatus st = hashing::HashStatus::ReadError;
    bool changed = false;
    uint64_t sz = 0, mt = 0;
    CHECK(hashing::StatFile(path, sz, mt));
    hashing::HashOneSide(path, sz, mt, changed, st, digest, true, &cache, hits,
                         nullptr, profiling::Side::Source, &cancel);
    armer.join();

    CHECK(st == hashing::HashStatus::Cancelled);
    CHECK(!changed);
    // No cache entry must have been written for the aborted file: a later lookup
    // cannot return the (incomplete) digest.
    std::array<uint8_t, 32> probe{};
    CHECK(!cache.Lookup(path, sz, mt, probe, 100, PartialPattern::Edges));
    CHECK(hits.load() == 0);
});

TEST("cancel: a clean pre-cancelled hash returns Cancelled immediately", [] {
    const auto dir = MakeTempDir();
    const std::wstring path = dir + L"\\small.bin";
    CHECK(WriteFileBytes(path, "hello world", 11));
    std::array<uint8_t, 32> digest{};
    std::atomic_bool cancel{true};
    const hashing::HashStatus st = hashing::Sha256File(path, digest, nullptr, &cancel);
    CHECK(st == hashing::HashStatus::Cancelled);
});

TEST("empty directories are reported in both directions", [] {
    const auto dir = MakeTempDir();
    fs::create_directories(dir + L"\\src\\only_src");
    fs::create_directories(dir + L"\\dst\\only_dst");
    const auto r = RunScan(dir + L"\\src", dir + L"\\dst", ScanMode::Presence);
    CHECK_EQ(r.results.stats.missingDirs, 1ull);
    CHECK_EQ(r.results.stats.extraDirs, 1ull);
});

TEST("unicode filenames are matched", [] {
    const auto dir = MakeTempDir();
    fs::create_directories(dir + L"\\src\\Unicode");
    fs::create_directories(dir + L"\\dst\\Unicode");
    {
        FILE* f = _wfopen((dir + L"\\src\\Unicode\\\u00e9\u4e2d\u6587.txt").c_str(), L"wb");
        if (f) { fputs("x", f); fclose(f); }
        f = _wfopen((dir + L"\\dst\\Unicode\\\u00e9\u4e2d\u6587.txt").c_str(), L"wb");
        if (f) { fputs("x", f); fclose(f); }
    }
    const auto r = RunScan(dir + L"\\src", dir + L"\\dst", ScanMode::Size);
    CHECK_EQ(r.results.stats.identicalFiles, 1ull);
    CHECK_EQ(r.results.stats.missingFiles + r.results.stats.extraFiles, 0ull);
});

TEST("case-insensitive policy: Foo == foo, A.txt == a.TXT", [] {
    const auto dir = MakeTempDir();
    fs::create_directories(dir + L"\\src\\Foo");
    fs::create_directories(dir + L"\\dst\\foo");
    {
        FILE* f = _wfopen((dir + L"\\src\\Foo\\a.txt").c_str(), L"wb");
        if (f) { fputs("data", f); fclose(f); }
        f = _wfopen((dir + L"\\dst\\foo\\A.TXT").c_str(), L"wb");
        if (f) { fputs("data", f); fclose(f); }
    }
    const auto r1 = RunScan(dir + L"\\src", dir + L"\\dst", ScanMode::Size);
    CHECK_EQ(r1.results.stats.identicalFiles, 1ull);
    CHECK_EQ(r1.results.stats.missingFiles + r1.results.stats.extraFiles, 0ull);

    const auto r2 = RunScan(dir + L"\\src", dir + L"\\dst", ScanMode::Size, true);
    CHECK_EQ(r2.results.stats.missingFiles, 1ull); // Foo\a.txt
    CHECK_EQ(r2.results.stats.extraFiles, 1ull);   // foo\A.TXT
    CHECK_EQ(r2.results.stats.identicalFiles, 0ull);
});

TEST("deep nesting (20 levels) is enumerated", [] {
    const auto dir = MakeTempDir();
    testgen::CreateDeepPath(dir, 20, L"deep.txt");
    const auto r = RunScan(dir, dir, ScanMode::Presence);
    CHECK_EQ(r.results.stats.identicalFiles, 1ull);
    CHECK_EQ(r.results.stats.identicalDirs, 20ull);
    CHECK(r.results.problems.empty());
});

TEST("long paths beyond MAX_PATH are handled", [] {
    const auto dir = MakeTempDir();
    testgen::CreateLongPathTree(dir);
    FileIndex index(false);
    Win32Enumerator en;
    const auto res = index.build(dir, en);
    CHECK(res.ok);
    FileEntry e;
    CHECK_MSG(index.find(LongPathRelative(), e), "deep file not found");
    CHECK_EQ(e.size, 18ull); // "long path" -> 9 chars * 2 bytes
});

TEST("very large sparse files (>4GB) compare by size without reading", [] {
    const auto dir = MakeTempDir();
    fs::create_directories(dir + L"\\src");
    fs::create_directories(dir + L"\\dst");
    const uint64_t big = 0x100000001ull; // 4 GiB + 1
    CHECK(testgen::CreateFileOfSize(dir + L"\\src\\big.bin", big));
    CHECK(testgen::CreateFileOfSize(dir + L"\\dst\\big.bin", big));
    const auto r = RunScan(dir + L"\\src", dir + L"\\dst", ScanMode::Size);
    CHECK_EQ(r.results.stats.identicalFiles, 1ull);
    CHECK_EQ(r.results.stats.bytesDest, big);
    CHECK_EQ(r.results.stats.bytesSource, big);
});

TEST("non-existent roots produce clean errors, not crashes", [] {
    const auto dir = MakeTempDir();
    testgen::CreateFixture(dir);
    const std::wstring missing = dir + L"\\does_not_exist_xyz";
    const auto r = RunScan(missing, dir, ScanMode::Presence);
    CHECK(!r.sourceOk);
    CHECK(r.destinationOk);
    CHECK(!r.results.problems.empty()); // root error recorded

    const auto r2 = RunScan(dir, missing, ScanMode::Presence);
    CHECK(r2.sourceOk);
    CHECK(!r2.destinationOk);
});

TEST("access denied subdirectory is reported and scan continues", [] {
    const auto dir = MakeTempDir();
    testgen::CreateDifferingTrees(dir);
    const auto denied = dir + L"\\dst\\denied";
    fs::create_directories(denied);
    {
        FILE* f = _wfopen((denied + L"\\secret.txt").c_str(), L"wb");
        if (f) { fputs("s", f); fclose(f); }
    }

    if (!DenyListAccess(denied)) {
        std::cout << "  (icacls non disponibile, test saltato)\n";
        RestoreAccess(denied);
        return;
    }
    ScopeGuard restore{[&] { RestoreAccess(denied); }};

    const auto r = RunScan(dir + L"\\src", dir + L"\\dst", ScanMode::Presence);
    CHECK_EQ(r.results.stats.accessDenied, 1ull);
    // a.txt at the root still compared: the scan continued past the error.
    CHECK_EQ(r.results.stats.identicalFiles, 4ull);
});

TEST("stress: 5000 files across 100 directories", [] {
    const auto dir = MakeTempDir();
    const size_t n = testgen::CreateStressTree(dir, 5000);
    CHECK_EQ(n, 5000ull);
    const auto r = RunScan(dir, dir, ScanMode::Presence);
    CHECK_EQ(r.results.stats.identicalFiles, 5000ull);
    CHECK_EQ(r.results.stats.identicalDirs, 100ull);
});
