// Unit and integration tests for Phase 1.
//
// Build: bv_tests. Run from any directory; all trees are created under the
// system temp directory and cleaned up.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
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
#include "Comparison/ConcurrentComparer.h"
#include "Comparison/FileComparator.h"
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
#include "Hashing/Sha256.h"
#include "ScanController.h"
#include "TestHarness.h"
#include "TestTree.h"
#include "Threading/ThreadPool.h"
#include "Util/StrictNumbers.h"

namespace {

namespace fs = std::filesystem;
using namespace bv;

std::vector<std::filesystem::path> g_cleanup;

int g_counter = 0;

std::wstring MakeTempDir() {
    const fs::path tmp = fs::temp_directory_path();
    const fs::path dir = tmp / (L"bvtest_" + std::to_wstring(GetCurrentProcessId()) +
                                L"_" + std::to_wstring(g_counter++));
    fs::create_directories(dir);
    g_cleanup.push_back(dir);
    return dir.wstring();
}

// Recursively delete a directory tree using the Win32 API with the long-path
// prefix on every operation. std::filesystem::remove_all cannot handle paths
// longer than MAX_PATH (e.g. the deep-tree test), which made it hang.
bool RemoveAllWin(const std::wstring& path) {
    const std::wstring pref = pathutil::AddLongPathPrefix(path) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pref.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return RemoveDirectoryW(pathutil::AddLongPathPrefix(path).c_str()) != 0;
    }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        const std::wstring child = path + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            RemoveAllWin(child);
        } else {
            DeleteFileW(pathutil::AddLongPathPrefix(child).c_str());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return RemoveDirectoryW(pathutil::AddLongPathPrefix(path).c_str()) != 0;
}

void CleanupTempDirs() {
    for (const auto& p : g_cleanup) {
        RemoveAllWin(p.wstring());
    }
    g_cleanup.clear();
}

struct ScopeGuard {
    std::function<void()> fn;
    ~ScopeGuard() { fn(); }
};

ScanReport RunScan(const std::wstring& src, const std::wstring& dst,
                   ScanMode mode, bool caseSensitive = false, unsigned int threads = 2) {
    ScanOptions opts;
    opts.source = src;
    opts.destination = dst;
    opts.mode = mode;
    opts.caseSensitive = caseSensitive;
    opts.hashThreads = threads;
    ScanController controller(caseSensitive);
    return controller.run(opts);
}

// Path of the deepest file created by CreateLongPathTree.
std::wstring LongPathRelative() {
    std::wstring rel;
    for (int i = 0; i < 90; ++i) {
        rel += L"long_" + std::to_wstring(i) + L"\\";
    }
    return rel + L"deepfile.txt";
}

bool DenyListAccess(const std::wstring& dir) {
    std::wstring cmd = L"icacls \"" + dir +
                       L"\" /deny \"" + _wgetenv(L"USERNAME") +
                       L"\":(OI)(CI)(RD) /C 2>nul";
    return _wsystem(cmd.c_str()) == 0;
}

void RestoreAccess(const std::wstring& dir) {
    std::wstring cmd = L"icacls \"" + dir +
                       L"\" /remove:d \"" + _wgetenv(L"USERNAME") +
                       L"\" /C 2>nul";
    _wsystem(cmd.c_str());
    cmd = L"icacls \"" + dir + L"\" /reset /C 2>nul";
    _wsystem(cmd.c_str());
}

// Writes raw bytes to a file (used to build deterministic content differences).
bool WriteFileBytes(const std::wstring& path, const char* data, size_t n) {
    const HANDLE h = CreateFileW(pathutil::AddLongPathPrefix(path).c_str(), GENERIC_WRITE,
                                 FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, data, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(h);
    return ok != FALSE && written == n;
}

std::string ReadFileBytes(const std::wstring& path) {
    std::ifstream in(pathutil::AddLongPathPrefix(path).c_str(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void BumpMtimeMinutes(const std::wstring& path, int minutes) {
    FILETIME ft{};
    const HANDLE h = CreateFileW(pathutil::AddLongPathPrefix(path).c_str(),
                                 FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    GetFileTime(h, nullptr, nullptr, &ft);
    ULARGE_INTEGER ui;
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    ui.QuadPart += static_cast<ULONGLONG>(minutes) * 60ull * 10'000'000ull; // 100ns units
    ft.dwLowDateTime = ui.LowPart;
    ft.dwHighDateTime = ui.HighPart;
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}

} // namespace

// ---------------------------------------------------------------------------
// PathUtil unit tests
// ---------------------------------------------------------------------------

TEST("pathutil: root normalization", [] {
    using bv::pathutil::NormalizeRoot;
    CHECK(NormalizeRoot(L"D:\\Backup\\") == L"D:\\Backup");
    CHECK(NormalizeRoot(L"D:\\Backup") == L"D:\\Backup");
    CHECK(NormalizeRoot(L"D:\\") == L"D:\\");
    CHECK(NormalizeRoot(L"\\\\NAS\\Backup\\") == L"\\\\NAS\\Backup");
    CHECK(NormalizeRoot(L"\\\\?\\C:\\x\\y\\") == L"C:\\x\\y");
    CHECK(NormalizeRoot(L"\\\\?\\UNC\\nas\\share\\") == L"\\\\nas\\share");
    CHECK(NormalizeRoot(L"") == L"");
});

TEST("pathutil: long path prefix (UNC aware)", [] {
    using bv::pathutil::AddLongPathPrefix;
    CHECK(AddLongPathPrefix(L"C:\\x") == L"\\\\?\\C:\\x");
    CHECK(AddLongPathPrefix(L"\\\\NAS\\share") == L"\\\\?\\UNC\\NAS\\share");
    CHECK(AddLongPathPrefix(L"\\\\?\\C:\\x") == L"\\\\?\\C:\\x");
    CHECK(AddLongPathPrefix(L"\\\\?\\UNC\\NAS\\share") == L"\\\\?\\UNC\\NAS\\share");
});

TEST("pathutil: join and make absolute", [] {
    using bv::pathutil::JoinRel;
    using bv::pathutil::MakeAbsolute;
    CHECK(JoinRel(L"", L"a") == L"a");
    CHECK(JoinRel(L"a", L"b") == L"a\\b");
    CHECK(MakeAbsolute(L"D:\\root", L"") == L"D:\\root");
    CHECK(MakeAbsolute(L"D:\\root", L"x\\y") == L"D:\\root\\x\\y");
});

TEST("pathutil: case folding is ASCII + invariant", [] {
    using bv::pathutil::FoldForCompare;
    CHECK(FoldForCompare(L"Foto") == L"FOTO");
    CHECK(FoldForCompare(L"foto") == L"FOTO");
    CHECK(FoldForCompare(L"FoTo\\a.JpG") == L"FOTO\\A.JPG");
    CHECK(FoldForCompare(L"") == L"");
});

// ---------------------------------------------------------------------------
// FileIndex
// ---------------------------------------------------------------------------

TEST("file index: case policy drives lookups", [] {
    const auto dir = MakeTempDir();
    const auto tree = dir + L"\\tree";
    fs::create_directories(tree + L"\\Foo");
    {
        FILE* f = _wfopen((tree + L"\\Foo\\a.txt").c_str(), L"wb");
        fclose(f);
    }
    FileIndex ci(false);
    FileIndex cs(true);
    Win32Enumerator en;
    ci.build(tree, en);
    cs.build(tree, en);

    FileEntry e;
    CHECK(ci.find(L"FOO\\A.TXT", e));        // case-insensitive match
    CHECK(ci.find(L"Foo\\a.txt", e));
    CHECK(!cs.find(L"FOO\\A.TXT", e));       // case-sensitive: no match
    CHECK(cs.find(L"Foo\\a.txt", e));
});

namespace {

// Enumerator that replays a fixed list of entries (used to exercise the
// duplicate-key path that a real disk walk cannot easily produce).
class FakeEnumerator : public IFileEnumerator {
public:
    // noinline: keeps GCC's -Warray-bounds device-inlining false positive away
    // from the other enumerators' call sites (it mistakes the iterator loop
    // for an out-of-bounds read of the base-class object).
    __attribute__((noinline)) bool enumerate(const std::wstring&,
                                             const EntryCallback& onEntry,
                                             const ErrorCallback&,
                                             const ProgressCallback&,
                                             const std::atomic_bool* = nullptr) override {
        for (auto& p : paths) {
            FileEntry e;
            e.relativePath = p;
            e.size = 0;
            e.isDirectory = false;
            if (!onEntry(std::move(e))) return false;
        }
        return true;
    }
    std::vector<std::wstring> paths;
};

} // namespace

TEST("errors: device-disconnect predicate is precise", [] {
    using bv::IsDeviceDisconnectError;
    // The abandon-storage codes 59/64/67/995/1167/1222/1231/1236 must abort.
    CHECK(IsDeviceDisconnectError(59));
    CHECK(IsDeviceDisconnectError(64));
    CHECK(IsDeviceDisconnectError(67));
    CHECK(IsDeviceDisconnectError(995));   // ERROR_OPERATION_ABORTED
    CHECK(IsDeviceDisconnectError(1167));
    CHECK(IsDeviceDisconnectError(1222));
    CHECK(IsDeviceDisconnectError(1231));
    CHECK(IsDeviceDisconnectError(1236));
    // Everyday, non-fatal errors must NEVER abort the scan as "device gone":
    // ACL denial, missing entry, bad parameter, sharing violation, and the
    // benign end-of-directory sentinel.
    CHECK(!IsDeviceDisconnectError(2));   // ERROR_FILE_NOT_FOUND
    CHECK(!IsDeviceDisconnectError(3));   // ERROR_PATH_NOT_FOUND
    CHECK(!IsDeviceDisconnectError(5));   // ERROR_ACCESS_DENIED
    CHECK(!IsDeviceDisconnectError(32));  // ERROR_SHARING_VIOLATION
    CHECK(!IsDeviceDisconnectError(53));  // ERROR_BAD_NETPATH
    CHECK(!IsDeviceDisconnectError(87));  // ERROR_INVALID_PARAMETER
    CHECK(!IsDeviceDisconnectError(0));
});

TEST("cli: strict numeric parsing rejects junk and overflow", [] {
    using bv::util::ParseThreadCount;
    using bv::util::ParseUInt64;
    uint64_t v = 0;
    CHECK(ParseUInt64(L"0", v) && v == 0);
    CHECK(ParseUInt64(L"100", v) && v == 100);
    CHECK(ParseUInt64(L"18446744073709551615", v) && v == 18446744073709551615ull);
    CHECK(!ParseUInt64(L"", v));
    CHECK(!ParseUInt64(L"-1", v));
    CHECK(!ParseUInt64(L"12abc", v));
    CHECK(!ParseUInt64(L"abc12", v));
    CHECK(!ParseUInt64(L"18446744073709551616", v)); // overflows uint64
    CHECK(!ParseUInt64(L"1.5", v));
    CHECK(!ParseUInt64(L" 10", v));
    CHECK(!ParseUInt64(L"10 ", v));
    unsigned int t = 0;
    CHECK(ParseThreadCount(L"0", t) && t == 0);
    CHECK(ParseThreadCount(L"12", t) && t == 12);
    CHECK(ParseThreadCount(L"4096", t) && t == 4096);
    CHECK(!ParseThreadCount(L"4097", t));
    CHECK(!ParseThreadCount(L"-1", t));
    CHECK(!ParseThreadCount(L"0x10", t));
    CHECK(!ParseThreadCount(L"1e3", t));
});

TEST("file index: duplicate folded keys are last-wins with consistent stats", [] {
    // Same key under the case-insensitive policy: "Foo.txt" and "foo.TXT".
    // Last entry wins; the index holds exactly one record per key and the
    // stats count only the kept entry.
    FakeEnumerator fak;
    fak.paths = {L"Foo.txt", L"foo.TXT"};
    FileIndex idx(false);
    const auto r = idx.build(L"", fak);
    CHECK(r.ok);
    CHECK_EQ(idx.size(), 1ull);
    CHECK_EQ(r.stats.files, 1ull);
    CHECK_EQ(r.stats.dirs, 0ull);

    FileEntry e;
    CHECK(idx.find(L"Foo.txt", e));
    CHECK(idx.find(L"foo.TXT", e)); // both spellings hit the single record
    CHECK(e.relativePath == L"foo.TXT"); // and it is the last reported one
});

TEST("file index: folded-key collisions are counted for the last-wins policy", [] {
    FileIndex idx(false); // case-insensitive: "Foo.txt" and "foo.TXT" fold to one key
    FileEntry a;
    a.relativePath = L"Foo.txt";
    a.size = 10;
    FileEntry b;
    b.relativePath = L"foo.TXT";
    b.size = 20;

    idx.addEntry(std::move(a));
    CHECK_EQ(idx.collisionCount(), 0ull);
    idx.addEntry(std::move(b)); // same folded key -> last-wins, counted
    CHECK_EQ(idx.collisionCount(), 1ull);

    FileEntry got;
    CHECK(idx.find(L"Foo.txt", got));
    CHECK(got.size == 20); // the later entry won
    CHECK_EQ(idx.size(), 1ull);

    // A rebuild resets the counter so a report reflects only the last build.
    FileIndex idx2(false);
    FileEntry c;
    c.relativePath = L"x.txt";
    idx2.addEntry(std::move(c));
    CHECK_EQ(idx2.collisionCount(), 0ull);
    CHECK_EQ(idx2.size(), 1ull);
});

// ---------------------------------------------------------------------------
// Full scans
// ---------------------------------------------------------------------------

TEST("identical tree: everything identical, no errors", [] {
    const auto dir = MakeTempDir();
    ScopeGuard sg{[] {}};
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

// ---------------------------------------------------------------------------
// ThreadPool

TEST("threadpool: runs all submitted tasks and waitAll() drains", [] {
    bv::ThreadPool pool(4);
    std::atomic<int> count{0};
    const int kTasks = 5000;
    for (int i = 0; i < kTasks; ++i) {
        pool.submit([&] { count.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.waitAll();
    CHECK_EQ(count.load(), kTasks);
});

TEST("threadpool: pooled = runs concurrently on multiple threads", [] {
    bv::ThreadPool pool(8);
    const int kWorkers = 8;
    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> release{false};
    std::atomic<int> running{0};
    std::atomic<int> maxActive{0};
    std::atomic<int> done{0};

    for (int i = 0; i < kWorkers; ++i) {
        pool.submit([&] {
            const int a = running.fetch_add(1) + 1; // 1-based concurrency incl. me
            int seen = maxActive.load();
            while (a > seen && !maxActive.compare_exchange_weak(seen, a)) {}
            if (a >= kWorkers) {
                { std::lock_guard<std::mutex> lk(m); release = true; }
                cv.notify_all();
            } else {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return release.load(); });
            }
            running.fetch_sub(1);
            done.fetch_add(1);
        });
    }
    pool.waitAll();
    // With exactly 8 tasks on 8 workers all must be live simultaneously.
    CHECK_EQ(maxActive.load(), kWorkers);
    CHECK_EQ(done.load(), kWorkers);
});

TEST("threadpool: 0 threads runs tasks synchronously", [] {
    bv::ThreadPool pool(0);
    std::atomic<int> count{0};
    for (int i = 0; i < 100; ++i) {
        pool.submit([&] { count.fetch_add(1); });
    }
    pool.waitAll();
    CHECK_EQ(count.load(), 100);
});

TEST("threadpool: destructor drains queued tasks", [] {
    std::atomic<int> count{0};
    {
        bv::ThreadPool pool(2);
        const int kTasks = 500;
        for (int i = 0; i < kTasks; ++i) {
            pool.submit([&] { count.fetch_add(1); });
        }
    }
    CHECK_EQ(count.load(), 500);
});

TEST("threadpool: waitAll() does not return while a task is still running", [] {
    // Deterministic gate: the single worker parks inside the first task; a
    // second task is queued behind it. waitAll() must not return until BOTH
    // have run, even though the first is blocked on a promise.
    bv::ThreadPool pool(1);
    std::atomic<int> completed{0};
    std::atomic<bool> taskStarted{false};
    std::atomic<bool> waitReturned{false};
    std::promise<void> gate;

    pool.submit([&] {
        taskStarted.store(true, std::memory_order_release);
        gate.get_future().wait(); // block until the main thread releases us
        completed.fetch_add(1, std::memory_order_relaxed);
    });
    while (!taskStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    pool.submit([&] { completed.fetch_add(1, std::memory_order_relaxed); });

    std::thread waiter([&] {
        pool.waitAll();
        waitReturned.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_MSG(!waitReturned.load(std::memory_order_acquire),
              "waitAll() returned while the first task was still blocked");
    gate.set_value();
    waiter.join();
    CHECK(waitReturned.load(std::memory_order_acquire));
    CHECK_EQ(completed.load(), 2);
});

TEST("threadpool: concurrent submit/waitAll batches all complete before returning", [] {
    // Several threads submit their own batch and wait for it concurrently. Each
    // waitAll() must cover every task submitted before it -- including the other
    // threads' tasks -- so nothing can be left running when one producer sees its
    // waitAll() return.
    bv::ThreadPool pool(4);
    std::atomic<int> completed{0};
    const int kProducers = 4;
    const int kTasksEach = 500;
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            for (int i = 0; i < kTasksEach; ++i) {
                pool.submit([&] { completed.fetch_add(1, std::memory_order_relaxed); });
            }
            pool.waitAll();
        });
    }
    for (auto& t : producers) t.join();
    CHECK_EQ(completed.load(), kProducers * kTasksEach);
});

TEST("threadpool: task exceptions are counted, never kill the pool", [] {
    bv::ThreadPool pool(2);
    pool.submit([] { throw std::runtime_error("boom"); });
    pool.submit([] {});
    pool.waitAll();
    CHECK_MSG(pool.taskErrors() == 1, "throwing task must be counted, not silent");
    // The pool stays usable afterwards and the counter is sticky.
    std::atomic<int> ok{0};
    pool.submit([&] { ok.fetch_add(1); });
    pool.waitAll();
    CHECK_EQ(ok.load(), 1);
    CHECK_EQ(pool.taskErrors(), 1ull);
});

TEST("ioclass: classify local vs network", [] {
    using namespace bv;
    CHECK(ClassifyIoClass(L"D:\\a", L"D:\\b") == IoClass::LocalLocal);
    CHECK(ClassifyIoClass(L"D:\\a", L"\\\\NAS\\share") == IoClass::LocalNetwork);
    CHECK(ClassifyIoClass(L"\\\\NAS1\\s", L"\\\\NAS2\\t") == IoClass::NetworkNetwork);
    CHECK(IsUncPath(L"\\\\?\\UNC\\NAS\\share"));
});

// ---------------------------------------------------------------------------
// Progress + cancel (Phase 2)

TEST("progress: onProgress reports files and emits a Done phase", [] {
    const auto dir = MakeTempDir();
    testgen::CreateStressTree(dir, 500);

    bv::ScanOptions opts;
    opts.source = dir;
    opts.destination = dir;
    opts.mode = bv::ScanMode::Presence;

    // A live-live comparison enumerates both sides CONCURRENTLY: there is no
    // separate EnumerateSource phase; the combined totals are reported during
    // CompareDestination (source files + destination files).
    bool sawCompare = false, sawDone = false;
    uint64_t maxFiles = 0;
    opts.onProgress = [&](const bv::ScanProgress& p) {
        if (p.phase == bv::ScanPhase::CompareDestination) {
            sawCompare = true;
            maxFiles = std::max(maxFiles, p.files);
        } else if (p.phase == bv::ScanPhase::Done) {
            sawDone = true;
        }
    };

    bv::ScanController controller(false);
    controller.run(opts);
    CHECK(sawCompare);
    CHECK(sawDone);
    CHECK(maxFiles >= 500); // both sides have 500 files; totals exceed this
});

TEST("cancel: pre-set cancel stops the scan before it completes", [] {
    const auto dir = MakeTempDir();
    testgen::CreateStressTree(dir, 200);

    std::atomic_bool cancel{true};
    bv::ScanOptions opts;
    opts.source = dir;
    opts.destination = dir;
    opts.mode = bv::ScanMode::Presence;
    opts.cancel = &cancel;

    bv::ScanController controller(false);
    auto r = controller.run(opts);
    // Cancelled from the start: nothing got compared as identical.
    CHECK_EQ(r.results.stats.identicalFiles, 0ull);
    CHECK_EQ(r.results.stats.missingFiles, 0ull);
    CHECK_EQ(r.results.stats.extraFiles, 0ull);
});

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

// ---------------------------------------------------------------------------
// Audit Issue 1 regression: a directory whose $I30 spills past the inline
// $INDEX_ROOT must be read from its $INDEX_ALLOCATION leaf blocks (INDX, with
// the USA multi-sector fixup undone before parsing). When elevated, the test
// asserts MFT == Win32 AND that at least one allocation block was actually
// parsed (BV_MFT_DEBUG_FILE ground-truth counter). Not elevated: skipped.
// The diagnostic is opt-in and the scan itself must succeed with or without it.

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
// Phase 5: export CSV/JSON, binary snapshot, hash cache, offline compare

TEST("export: csv escaping, BOM and hex digests", [] {
    using namespace bv::exporting;
    ResultSet r;
    {
        FileResult p;
        p.status = Status::SizeMismatch;
        p.relativePath = L"sub,a\"b\nc.txt"; // comma, double quote, newline
        p.sizeSource = 10;
        p.sizeDest = 12;
        r.problems.push_back(p);
    }
    {
        FileResult p;
        p.status = Status::Extra;
        p.relativePath = L"p\u00e0\u00e8\u00e9.io"; // accents
        p.hasHashSource = true;
        p.hasHashDest = false;
        for (int i = 0; i < 32; ++i) p.hashSource[i] = static_cast<uint8_t>(i);
        r.problems.push_back(p);
    }
    {
        // CSV/DDE formula injection: a name that starts with '=', '+', '-' or
        // '@' must never be emitted bare, or a spreadsheet would evaluate it.
        FileResult p;
        p.status = Status::Missing;
        p.relativePath = L"=SUM(A1:A9).txt";
        r.problems.push_back(p);
    }
    {
        FileResult p;
        p.status = Status::Missing;
        p.relativePath = L"-cmd.xlsx";
        r.problems.push_back(p);
    }
    {
        FileResult p;
        p.status = Status::Missing;
        p.relativePath = L"@hdr";
        r.problems.push_back(p);
    }
    const std::wstring file = MakeTempDir() + L"\\out.csv";
    std::wstring err;
    CHECK(WriteCsv(file, r, err));
    const std::string bytes = ReadFileBytes(file);

    // UTF-8 BOM first, then the header row.
    CHECK(bytes.size() > 3 && bytes[0] == '\xEF' && bytes[1] == '\xBB' && bytes[2] == '\xBF');
    CHECK(bytes.find("status,path,size_source,size_destination,hash_source,hash_destination") !=
          std::string::npos);

    // RFC 4180: a field with comma/quote/newline is quoted with doubled quotes.
    CHECK(bytes.find("\"sub,a\"\"b\nc.txt\"") != std::string::npos);
    CHECK(bytes.find("DIM_DIVERSA,\"sub,a\"\"b\nc.txt\",10,12,") != std::string::npos);
    // Accented name survives as UTF-8, digest as lowercase hex.
    CHECK(bytes.find("p\xc3\xa0\xc3\xa8\xc3\xa9.io") != std::string::npos);
    CHECK(bytes.find("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f") !=
          std::string::npos);
    // Formula-injection cells are quoted with a leading apostrophe...
    CHECK(bytes.find("\"'=SUM(A1:A9).txt\"") != std::string::npos);
    CHECK(bytes.find("\"'-cmd.xlsx\"") != std::string::npos);
    CHECK(bytes.find("\"'@hdr\"") != std::string::npos);
    // ...and no bare formula prefix survives on any path cell.
    CHECK(bytes.find(",=SUM") == std::string::npos);
    CHECK(bytes.find(",-cmd") == std::string::npos);
    CHECK(bytes.find(",@hdr") == std::string::npos);
});

TEST("export: json escaping and no BOM", [] {
    using namespace bv::exporting;
    // Structural parser: brackets/braces balanced, strings closed, and NO
    // trailing comma (a ',' followed by ']' or '}' is invalid RFC 8259).
    const auto wellFormed = [](const std::string& s) -> bool {
        std::vector<char> stack;
        bool inStr = false;
        for (size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (inStr) {
                if (c == '\\') { ++i; continue; }
                if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') { inStr = true; continue; }
            if (c == '{' || c == '[') {
                stack.push_back(c);
            } else if (c == '}' || c == ']') {
                if (stack.empty()) return false;
                const char open = stack.back();
                if (open == '{' && c != '}') return false;
                if (open == '[' && c != ']') return false;
                stack.pop_back();
            } else if (c == ',') {
                size_t j = i + 1;
                while (j < s.size() && (s[j] == ' ' || s[j] == '\n' || s[j] == '\t' ||
                                        s[j] == '\r'))
                    ++j;
                if (j < s.size() && (s[j] == '}' || s[j] == ']')) return false;
            }
        }
        return stack.empty() && !inStr;
    };

    ResultSet r;
    {
        FileResult p;
        p.status = Status::ContentMismatch;
        p.relativePath = L"dir\\qu\"ote\\path\tfile.bin";
        p.sizeSource = 5;
        p.sizeDest = 5;
        r.problems.push_back(p);
    }
    {
        FileResult p;
        p.status = Status::Extra;
        p.relativePath = L"x\".txt";
        p.hasHashSource = true;
        for (int i = 0; i < 32; ++i) p.hashSource[i] = static_cast<uint8_t>(i);
        r.problems.push_back(p);
    }

    const std::wstring file = MakeTempDir() + L"\\out.json";
    std::wstring err;
    CHECK(WriteJson(file, r, err));
    const std::string bytes = ReadFileBytes(file);

    CHECK(bytes[0] == '['); // no BOM, streaming array
    CHECK(bytes.find("CONTENUTO_DIVERSO") != std::string::npos);
    // JSON escaping: quote, backslash, tab.
    CHECK(bytes.find("dir\\\\qu\\\"ote\\\\path\\tfile.bin") != std::string::npos);
    CHECK(bytes.find("\"size_source\":5,\"size_destination\":5") != std::string::npos);
    // RFC 8259: generated document must be structurally valid and, in
    // particular, must NOT end the last object with a trailing comma.
    CHECK(wellFormed(bytes));
    CHECK(bytes.find("},\n]") == std::string::npos);

    // Empty problems: still a valid, empty JSON array.
    ResultSet empty;
    const std::wstring file2 = MakeTempDir() + L"\\out_empty.json";
    CHECK(WriteJson(file2, empty, err));
    const std::string bytes2 = ReadFileBytes(file2);
    CHECK(wellFormed(bytes2));
    CHECK(bytes2.find('{') == std::string::npos);
});

TEST("snapshot: index round-trip preserves entries, hashes and case policy", [] {
    const auto dir = MakeTempDir();
    const std::wstring src = dir + L"\\src";
    fs::create_directories(src);
    CHECK(WriteFileBytes(src + L"\\a.txt", "hello world", 11));
    CHECK(WriteFileBytes(src + L"\\beta.bin", "12345", 5));

    FileIndex idx(false);
    {
        Win32Enumerator en;
        const auto br = idx.build(src, en);
        CHECK(br.ok);
    }
    CHECK_EQ(idx.size(), 2ull);
    idx.setHash(L"a.txt", {42, 43, 44});

    const std::wstring snap = dir + L"\\idx.bin";
    std::wstring err;
    CHECK(indexio::WriteSnapshot(snap, idx, src, err));
    CHECK(fs::exists(snap));

    FileIndex idx2(true); // wrong case policy on purpose: must be overridden
    std::wstring root;
    CHECK(indexio::ReadSnapshot(snap, idx2, root, err));
    CHECK_EQ(idx2.isCaseSensitive(), false);
    CHECK(root == src);
    CHECK_EQ(idx2.size(), 2ull);
    CHECK_EQ(idx2.hashCount(), 1ull);

    // Entries equal (path, size, mtime, attributes, fileId, type).
    CHECK(idx2.entries().size() == idx.entries().size());
    for (const auto& [key, e] : idx.entries()) {
        const auto it = idx2.entries().find(key);
        CHECK_MSG(it != idx2.entries().end(), "entry present after load");
        if (it != idx2.entries().end()) {
            CHECK(it->second.relativePath == e.relativePath);
            CHECK_EQ(it->second.size, e.size);
            CHECK_EQ(it->second.lastWriteTime, e.lastWriteTime);
            CHECK_EQ(it->second.attributes, e.attributes);
            CHECK_EQ(it->second.fileId, e.fileId);
            CHECK_EQ(it->second.isDirectory, e.isDirectory);
        }
    }
    CHECK(idx2.hashes() == idx.hashes());
});

TEST("snapshot: corrupt file is rejected cleanly", [] {
    const std::string junk = "not a snapshot at all, just some junk bytes";
    const std::wstring snap = MakeTempDir() + L"\\bad.bin";
    CHECK(WriteFileBytes(snap, junk.data(), junk.size()));

    FileIndex idx;
    std::wstring root, err;
    CHECK(!indexio::ReadSnapshot(snap, idx, root, err));
    CHECK(!err.empty());
});

TEST("offline: compareFrom verifies content against snapshot digests", [] {
    const auto dir = MakeTempDir();
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";
    fs::create_directories(src);
    fs::create_directories(dst);
    CHECK(WriteFileBytes(src + L"\\alpha.txt", "hello world", 11));
    CHECK(WriteFileBytes(src + L"\\beta.bin", "zebra", 5));
    fs::copy(src, dst, fs::copy_options::recursive);

    // Capture the source (Content mode embeds digests in the snapshot).
    ScanOptions cap;
    cap.source = src;
    cap.destination = src;
    cap.mode = ScanMode::Content;
    cap.hashThreads = 2;
    cap.snapshotOut = dir + L"\\src.bin";
    ScanReport r1 = ScanController(false).run(cap);
    CHECK(r1.snapshotWritten);
    CHECK_EQ(r1.results.stats.identicalFiles, 2ull);

    // Corrupt the destination's first file (same size, different bytes).
    CHECK(WriteFileBytes(dst + L"\\alpha.txt", "xxxxx world", 11));

    // Offline comparison: source device absent, digests come from snapshot.
    ScanOptions off;
    off.destination = dst;
    off.mode = ScanMode::Content;
    off.hashThreads = 2;
    off.compareFrom = dir + L"\\src.bin";
    ScanReport r2 = ScanController(false).run(off);
    CHECK(r2.sourceOk);
    CHECK(r2.usedSnapshot);
    CHECK(r2.modeUsed == ScanMode::Content); // digests present, real verification
    CHECK_EQ(r2.results.stats.sourceFiles, 2ull);
    CHECK_EQ(r2.results.stats.identicalFiles, 1ull);   // beta.bin
    CHECK_EQ(r2.results.stats.contentMismatch, 1ull);  // alpha.txt
    CHECK_EQ(r2.results.stats.missingFiles + r2.results.stats.extraFiles, 0ull);
});

TEST("offline: snapshot without hashes degrades content to size", [] {
    const auto dir = MakeTempDir();
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";
    fs::create_directories(src);
    fs::create_directories(dst);
    CHECK(WriteFileBytes(src + L"\\alpha.txt", "hello world", 11));
    fs::copy(src, dst, fs::copy_options::recursive);

    // Presence-mode snapshot: entries only, no digests.
    ScanOptions cap;
    cap.source = src;
    cap.destination = src;
    cap.mode = ScanMode::Presence;
    cap.snapshotOut = dir + L"\\src.bin";
    CHECK(ScanController(false).run(cap).snapshotWritten);

    ScanOptions off;
    off.destination = dst;
    off.mode = ScanMode::Content;
    off.compareFrom = dir + L"\\src.bin";
    ScanReport r = ScanController(false).run(off);
    CHECK(r.usedSnapshot);
    CHECK(r.contentDegradedToSize);
    CHECK(r.modeUsed == ScanMode::Size); // degraded: only sizes are verifiable
    CHECK_EQ(r.results.stats.identicalFiles, 1ull); // same size counts as identical
});

TEST("cache: second run reuses stored hashes without re-reading", [] {
    const auto tree = MakeTempDir();
    testgen::CreateStressTree(tree, 200); // 3 dirs x 100 files? -> 200 files total
    // The cache file must NOT live inside the scanned tree, or it would appear
    // as a new file on the second run.
    const std::wstring cache = MakeTempDir() + L"\\hash.bin";

    ScanOptions base;
    base.source = tree;
    base.destination = tree;
    base.mode = ScanMode::Content;
    base.hashThreads = 2;
    base.hashCacheFile = cache;

    ScanReport r1 = ScanController(false).run(base);
    CHECK_EQ(r1.results.stats.sourceFiles, r1.results.stats.identicalFiles);
    CHECK(fs::exists(cache)); // cache was written back

    ScanReport r2 = ScanController(false).run(base);
    CHECK_EQ(r2.results.stats.identicalFiles, r1.results.stats.identicalFiles);
    CHECK_MSG(r2.hashCacheHits > 0, "unchanged tree should be served from the cache");

    // The persisted file is loadable and carries entries.
    std::wstring err;
    hashing::HashCache loaded(cache, err);
    CHECK_MSG(loaded.size() > 0, "cache reloaded from disk");
});

TEST("cache: persistence round-trip preserves digests and key semantics", [] {
    const std::wstring file = MakeTempDir() + L"\\hash_rt.bin";
    std::array<uint8_t, 32> d0;
    std::array<uint8_t, 32> d1;
    for (size_t i = 0; i < d0.size(); ++i) {
        d0[i] = static_cast<uint8_t>(i);
        d1[i] = static_cast<uint8_t>(255 - i);
    }
    {
        std::wstring err;
        hashing::HashCache c(file, err);
        CHECK(err.empty());
        c.Store(L"C:\\a\\b.txt", 100, 12345, d0);
        c.Store(L"C:\\a\\c.txt", 200, 99999, d1);
        CHECK_EQ(c.size(), 2ull);
        CHECK(c.Save(err));
        CHECK(err.empty());
    }
    // A fresh instance must reload exactly what was stored.
    std::wstring err;
    hashing::HashCache c2(file, err);
    CHECK(err.empty());
    CHECK_EQ(c2.size(), 2ull);
    std::array<uint8_t, 32> got{};
    CHECK(c2.Lookup(L"C:\\a\\b.txt", 100, 12345, got));
    CHECK(got == d0);
    CHECK(c2.Lookup(L"C:\\a\\c.txt", 200, 99999, got));
    CHECK(got == d1);
    // The key is (path, size, mtime): changing any component must miss.
    CHECK(!c2.Lookup(L"C:\\a\\b.txt", 101, 12345, got));
    CHECK(!c2.Lookup(L"C:\\a\\b.txt", 100, 12346, got));
    CHECK(!c2.Lookup(L"C:\\a\\other.txt", 100, 12345, got));
});

TEST("cache: corrupt file is rejected cleanly and can be rebuilt", [] {
    const std::wstring file = MakeTempDir() + L"\\hash_corrupt.bin";
    const char junk[] = "this-is-not-a-BVHC-cache-file-at-all-0123456789";
    CHECK(WriteFileBytes(file, junk, sizeof(junk) - 1));

    std::wstring err;
    hashing::HashCache c(file, err);
    CHECK_MSG(!err.empty(), "corrupt cache must be reported, not loaded");
    CHECK_EQ(c.size(), 0ull);

    // The cache is still usable and rewrites a valid file from scratch.
    std::array<uint8_t, 32> d{};
    d[0] = 0xAB;
    c.Store(L"C:\\x\\y.bin", 7, 42, d);
    CHECK(c.Save(err));
    CHECK(err.empty());

    std::wstring err2;
    hashing::HashCache c2(file, err2);
    CHECK(err2.empty());
    CHECK_EQ(c2.size(), 1ull);
    std::array<uint8_t, 32> got{};
    CHECK(c2.Lookup(L"C:\\x\\y.bin", 7, 42, got));
    CHECK(got[0] == 0xAB);
});

TEST("cache: concurrent Lookup/Store from many threads stays consistent", [] {
    // The cache is served by hash worker threads; hammer it from several
    // threads that Store unique entries and Lookup them back, then verify
    // every entry is present with the right digest. CHECK/FAIL must not run
    // from the workers, so the assertions happen on the main thread after the
    // joins (a race would surface as a crash, a lost entry, or a wrong digest).
    const std::wstring file = MakeTempDir() + L"\\hash_conc.bin";
    std::wstring err;
    hashing::HashCache cache(file, err);
    CHECK(err.empty());

    const int kThreads = 8;
    const int kEntriesEach = 250;
    std::atomic<int> lookupsOk{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < kEntriesEach; ++i) {
                const std::wstring path = L"C:\\conc\\t" + std::to_wstring(t) +
                                          L"\\f" + std::to_wstring(i) + L".dat";
                const uint64_t size = static_cast<uint64_t>(t * 1000 + i);
                const uint64_t mtime = static_cast<uint64_t>(i * 7 + t);
                std::array<uint8_t, 32> d{};
                d[0] = static_cast<uint8_t>(t);
                d[1] = static_cast<uint8_t>(i & 0xFF);
                cache.Store(path, size, mtime, d);

                std::array<uint8_t, 32> got{};
                if (cache.Lookup(path, size, mtime, got) && got == d) {
                    lookupsOk.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& w : workers) w.join();

    CHECK_EQ(cache.size(), static_cast<size_t>(kThreads * kEntriesEach));
    CHECK_MSG(lookupsOk.load() == kThreads * kEntriesEach,
              "every entry stored by a worker must be immediately lookable");

    // A fresh instance reloads the full, uncorrupted map.
    std::wstring errSave;
    CHECK(cache.Save(errSave));
    CHECK(errSave.empty());
    std::wstring err2;
    hashing::HashCache reloaded(file, err2);
    CHECK(err2.empty());
    CHECK_EQ(reloaded.size(), static_cast<size_t>(kThreads * kEntriesEach));
});

TEST("comparator: file changed between enumeration and hash is flagged", [] {
    const auto dir = MakeTempDir();
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";
    fs::create_directories(src);
    fs::create_directories(dst);
    const std::wstring afile = src + L"\\a.txt";
    CHECK(WriteFileBytes(afile, "hello world", 11));
    fs::copy(afile, dst + L"\\a.txt");

    FileIndex srcIdx(false);
    {
        Win32Enumerator en;
        const auto br = srcIdx.build(src, en);
        CHECK(br.ok);
    }
    CHECK_EQ(srcIdx.size(), 1ull);

    // Mutate AFTER the index was built: same size, bumped mtime.
    CHECK(WriteFileBytes(afile, "xxxxx world", 11));
    BumpMtimeMinutes(afile, 1);

    FileComparator cmp(srcIdx, ScanMode::Content, src);
    ResultSet out;
    {
        Win32Enumerator en;
        CHECK(cmp.run(dst, en, out));
    }
    ThreadPool pool(2);
    cmp.runHashing(pool, out, nullptr, {}, nullptr);

    CHECK_EQ(out.stats.changedDuringScan, 1ull);
    CHECK_EQ(out.problems.size(), 1ull);
    if (out.problems.size() == 1) {
        CHECK(out.problems[0].status == Status::ChangedDuringScan);
        CHECK(out.problems[0].relativePath == L"a.txt");
    }
});

// ---------------------------------------------------------------------------
// Concurrent comparer: sharded match table, dual workers, outcome gating
// ---------------------------------------------------------------------------

namespace {

// Captures every entry Win32 finds under `root` (relative paths), for replaying
// through the fake enumerators below in any desired order.
std::vector<FileEntry> CaptureEntries(const std::wstring& root) {
    std::vector<FileEntry> out;
    Win32Enumerator en;
    const bool ok = en.enumerate(
        root, [&](FileEntry&& e) { out.push_back(std::move(e)); return true; },
        [](const ScanError&) {});
    CHECK(ok);
    return out;
}

std::unique_ptr<IFileEnumerator> MakeWin32Factory() {
    return std::unique_ptr<IFileEnumerator>(new Win32Enumerator());
}

// Plays a captured entry list back through the comparer. `before`/`after` run
// around the stream so tests can force a deterministic discovery order.
class ReplayEnumerator : public bv::IFileEnumerator {
public:
    ReplayEnumerator(std::vector<FileEntry> entries, std::function<void()> before = {},
                     std::function<void()> after = {})
        : entries_(std::move(entries)), before_(std::move(before)), after_(std::move(after)) {}
    bool enumerate(const std::wstring&, const EntryCallback& onEntry, const ErrorCallback&,
                   const ProgressCallback& onProgress, const std::atomic_bool* cancel) override {
        if (before_) before_();
        uint64_t files = 0, dirs = 0;
        for (FileEntry& e : entries_) {
            if (cancel && cancel->load(std::memory_order_relaxed)) return false;
            if (e.isDirectory) {
                ++dirs;
            } else {
                ++files;
            }
            if (!onEntry(FileEntry(e))) return false;
            if (onProgress) onProgress(files, dirs, L"");
        }
        if (after_) after_();
        return true;
    }

private:
    std::vector<FileEntry> entries_;
    std::function<void()> before_, after_;
};

// Fails before emitting anything (stands in for the MFT back-end on a volume it
// cannot read). The comparer must fall back to the next factory, not fail.
class FailImmediatelyEnumerator : public bv::IFileEnumerator {
public:
    bool enumerate(const std::wstring&, const EntryCallback&, const ErrorCallback&,
                   const ProgressCallback&, const std::atomic_bool*) override {
        return false;
    }
};

// Emits the first `emitCount` captured entries, then reports a failure -- the
// incomplete-MFT shape: a stream produced before the scan went bad.
class FailAfterNEnumerator : public bv::IFileEnumerator {
public:
    FailAfterNEnumerator(std::vector<FileEntry> entries, size_t emitCount)
        : entries_(std::move(entries)), emitCount_(emitCount) {}
    bool enumerate(const std::wstring&, const EntryCallback& onEntry, const ErrorCallback&,
                   const ProgressCallback&, const std::atomic_bool*) override {
        size_t n = 0;
        for (FileEntry& e : entries_) {
            if (n >= emitCount_) break;
            ++n;
            if (!onEntry(FileEntry(e))) return false;
        }
        return false;
    }

private:
    std::vector<FileEntry> entries_;
    size_t emitCount_;
};

// Runs the concurrent comparer against two real trees. Both sides default to
// Win32; callers can inject custom enumerator factories / a whole fallback chain.
ConcurrentComparer::Result RunConcurrent(
    const std::wstring& src, const std::wstring& dst, ScanMode mode,
    bool caseSensitive = false, unsigned int hashThreads = 2,
    ConcurrentComparer::SourceKind sourceKind = ConcurrentComparer::SourceKind::Live,
    std::vector<ConcurrentComparer::EnumeratorFactory> srcFactories = {},
    std::vector<ConcurrentComparer::EnumeratorFactory> dstFactories = {},
    FileIndex* fromIndex = nullptr, const std::atomic_bool* cancel = nullptr) {
    if (srcFactories.empty()) srcFactories.push_back(MakeWin32Factory);
    if (dstFactories.empty()) dstFactories.push_back(MakeWin32Factory);
    bv::ThreadPool pool(hashThreads);
    ConcurrentComparer cmp(caseSensitive, mode, /*acceptMft=*/false, src, dst, sourceKind,
                           fromIndex, cancel);
    return cmp.runWithFactories(std::move(srcFactories), std::move(dstFactories), pool);
}

bool SameResults(const ResultSet& a, const ResultSet& b) {
    const Stats& x = a.stats;
    const Stats& y = b.stats;
    if (x.sourceFiles != y.sourceFiles || x.sourceDirs != y.sourceDirs) return false;
    if (x.destFiles != y.destFiles || x.destDirs != y.destDirs) return false;
    if (x.identicalFiles != y.identicalFiles || x.identicalDirs != y.identicalDirs) return false;
    if (x.missingFiles != y.missingFiles || x.missingDirs != y.missingDirs) return false;
    if (x.extraFiles != y.extraFiles || x.extraDirs != y.extraDirs) return false;
    if (x.sizeMismatch != y.sizeMismatch || x.contentMismatch != y.contentMismatch) return false;
    if (x.readErrors != y.readErrors || x.accessDenied != y.accessDenied) return false;
    if (x.changedDuringScan != y.changedDuringScan) return false;
    if (x.bytesSource != y.bytesSource || x.bytesDest != y.bytesDest) return false;
    if (a.problems.size() != b.problems.size()) return false;
    for (size_t i = 0; i < a.problems.size(); ++i) {
        const FileResult& p = a.problems[i];
        const FileResult& q = b.problems[i];
        if (p.status != q.status || p.isDirectory != q.isDirectory) return false;
        if (p.relativePath != q.relativePath) return false;
        if (p.sizeSource != q.sizeSource || p.sizeDest != q.sizeDest) return false;
    }
    return true;
}

} // namespace

TEST("matchtable: paired inserts match and remove, and both orders converge", [] {
    // shardBits=2 -> 4 shards; 40 keys force several entries per shard, so the
    // same-shard lock path and intra-map matching are exercised.
    std::vector<std::wstring> keys;
    for (int i = 0; i < 40; ++i) keys.push_back(L"k" + std::to_wstring(i));

    bv::MatchTable t(2);
    std::vector<bv::MatchTable::Outcome> outcomes;
    for (const auto& k : keys) {
        FileEntry e;
        e.relativePath = k;
        FileEntry peer;
        outcomes.push_back(t.insert(k, 0, std::move(e), peer));
    }
    for (const auto& k : keys) {
        FileEntry e;
        e.relativePath = k;
        FileEntry peer;
        outcomes.push_back(t.insert(k, 1, std::move(e), peer));
    }
    CHECK_EQ(t.remaining().size(), 0ull);
    CHECK_EQ(std::count(outcomes.begin(), outcomes.end(), bv::MatchTable::Outcome::Inserted), 40);
    CHECK_EQ(std::count(outcomes.begin(), outcomes.end(), bv::MatchTable::Outcome::Matched), 40);

    // Reverse order (destination first): same net outcome.
    bv::MatchTable t2(2);
    for (const auto& k : keys) {
        FileEntry e;
        e.relativePath = k;
        FileEntry peer;
        t2.insert(k, 1, std::move(e), peer);
    }
    for (const auto& k : keys) {
        FileEntry e;
        e.relativePath = k;
        FileEntry peer;
        t2.insert(k, 0, std::move(e), peer);
    }
    CHECK_EQ(t2.remaining().size(), 0ull);
});

TEST("matchtable: same-side duplicate key replaces (last wins, like FileIndex)", [] {
    bv::MatchTable t(2);
    FileEntry a;
    a.relativePath = L"x";
    FileEntry peer;
    CHECK(t.insert(L"x", 0, std::move(a), peer) == bv::MatchTable::Outcome::Inserted);
    FileEntry b;
    b.relativePath = L"x";
    CHECK(t.insert(L"x", 0, std::move(b), peer) == bv::MatchTable::Outcome::Replaced);
    CHECK_EQ(t.pendingCount(), 1ull);
    const auto rem = t.remaining();
    CHECK_EQ(rem.size(), 1ull);
    if (rem.size() == 1) CHECK(rem[0].second.relativePath == L"x");
});

TEST("matchtable: one-sided backpressure never deadlocks while matching (tight high-water)", [] {
    // highWater=1 forces the source worker (side 0) to park at essentially every
    // insert and rely on the destination (side 1) - which is never gated - to
    // release it. If the old two-sided gate were still present this would hang.
    const int N = 2000;
    bv::MatchTable t(6, /*highWater=*/1);
    std::thread src([&] {
        for (int i = 0; i < N; ++i) {
            const std::wstring k = L"k" + std::to_wstring(i);
            FileEntry e;
            e.relativePath = k;
            FileEntry peer;
            t.insert(k, 0, std::move(e), peer);
        }
        t.setSideDone(0); // worker finished: signal completion (as runEnumWorker does)
    });
    std::thread dst([&] {
        for (int i = 0; i < N; ++i) {
            const std::wstring k = L"k" + std::to_wstring(i);
            FileEntry e;
            e.relativePath = k;
            FileEntry peer;
            t.insert(k, 1, std::move(e), peer);
        }
        t.setSideDone(1);
    });
    src.join();
    dst.join();
    CHECK_EQ(t.remaining().size(), 0ull); // every key matched and removed
});

TEST("matchtable: source flooded past limit with no peer still completes (no deadlock)", [] {
    // Both sides hold the SAME fully disjoint key sets, far above the high-water
    // mark, with the source likely to flood first: the source parks on the gate,
    // the destination - never gated - finishes and releases it via setSideDone.
    // This is the exact scenario that would deadlock a symmetric gate.
    const int N = 5000;
    bv::MatchTable t(6, /*highWater=*/4);
    std::thread src([&] {
        for (int i = 0; i < N; ++i) {
            const std::wstring k = L"s" + std::to_wstring(i);
            FileEntry e;
            e.relativePath = k;
            FileEntry peer;
            t.insert(k, 0, std::move(e), peer);
        }
        t.setSideDone(0);
    });
    std::thread dst([&] {
        for (int i = 0; i < N; ++i) {
            const std::wstring k = L"d" + std::to_wstring(i);
            FileEntry e;
            e.relativePath = k;
            FileEntry peer;
            t.insert(k, 1, std::move(e), peer);
        }
        t.setSideDone(1);
    });
    src.join();
    dst.join();
    CHECK_EQ(t.remaining().size(), size_t{2} * N); // all entries persisted (missing+extra)
});

TEST("concurrent comparer: identical fixtures are all-identical, no errors", [] {
    const auto dir = MakeTempDir();
    testgen::CreateFixture(dir);
    const auto r = RunConcurrent(dir, dir, ScanMode::Presence);
    CHECK(r.sourceStatus == ConcurrentComparer::WorkerStatus::Success);
    CHECK(r.destinationStatus == ConcurrentComparer::WorkerStatus::Success);
    const auto& s = r.results.stats;
    CHECK_EQ(s.identicalFiles, 6ull);
    CHECK_EQ(s.identicalDirs, 24ull);
    CHECK_EQ(s.missingFiles + s.extraFiles + s.missingDirs + s.extraDirs, 0ull);
    CHECK(r.results.problems.empty());
});

TEST("concurrent comparer: differing trees report missing and extra", [] {
    const auto dir = MakeTempDir();
    testgen::CreateDifferingTrees(dir);
    const auto r = RunConcurrent(dir + L"\\src", dir + L"\\dst", ScanMode::Presence);
    const auto& s = r.results.stats;
    CHECK_EQ(s.identicalFiles, 4ull);
    CHECK_EQ(s.identicalDirs, 1ull);
    CHECK_EQ(s.missingFiles, 1ull);
    CHECK_EQ(s.missingDirs, 1ull);
    CHECK_EQ(s.extraFiles, 1ull);
    CHECK_EQ(s.extraDirs, 1ull);
    CHECK_EQ(s.sizeMismatch, 0ull);
    CHECK_EQ(s.readErrors + s.accessDenied, 0ull);
});

TEST("concurrent comparer: size mode flags size mismatches", [] {
    const auto dir = MakeTempDir();
    testgen::CreateDifferingTrees(dir);
    const auto r = RunConcurrent(dir + L"\\src", dir + L"\\dst", ScanMode::Size);
    const auto& s = r.results.stats;
    CHECK_EQ(s.identicalFiles, 3ull);
    CHECK_EQ(s.sizeMismatch, 1ull);
    CHECK_EQ(s.missingFiles, 1ull);
    CHECK_EQ(s.extraFiles, 1ull);
    CHECK_EQ(s.missingDirs, 1ull);
    CHECK_EQ(s.extraDirs, 1ull);
});

TEST("concurrent comparer: content mode hashes and detects same-size mismatch", [] {
    const auto dir = MakeTempDir();
    testgen::CreateDifferingTrees(dir);
    const auto r = RunConcurrent(dir + L"\\src", dir + L"\\dst", ScanMode::Content);
    const auto& s = r.results.stats;
    CHECK_EQ(s.identicalFiles, 2ull);
    CHECK_EQ(s.contentMismatch, 1ull);
    CHECK_EQ(s.sizeMismatch, 1ull);
    CHECK_EQ(s.missingFiles, 1ull);
    CHECK_EQ(s.extraFiles, 1ull);
    CHECK_EQ(s.missingDirs, 1ull);
    CHECK_EQ(s.extraDirs, 1ull);
});

TEST("concurrent comparer: thousands of files pair across shards", [] {
    const auto dir = MakeTempDir();
    testgen::CreateStressTree(dir + L"\\src", 1000);
    fs::copy(dir + L"\\src", dir + L"\\dst", fs::copy_options::recursive);
    const auto r = RunConcurrent(dir + L"\\src", dir + L"\\dst", ScanMode::Presence);
    CHECK_EQ(r.results.stats.identicalFiles, 1000ull);
    CHECK_EQ(r.results.stats.identicalDirs, 100ull);
    CHECK_EQ(r.results.stats.missingFiles + r.results.stats.extraFiles +
                 r.results.stats.missingDirs + r.results.stats.extraDirs, 0ull);
});

TEST("concurrent comparer: discovery order does not change the outcome", [] {
    const auto dir = MakeTempDir();
    testgen::CreateDifferingTrees(dir);
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";
    const auto srcEntries = CaptureEntries(src);
    const auto dstEntries = CaptureEntries(dst);

    ConcurrentComparer::Result first;
    ConcurrentComparer::Result second;
    bv::ThreadPool pool(0);

    {
        // Order 1: the source is fully enumerated (and inserted) before the
        // destination worker may emit a single entry.
        std::atomic<bool> srcDone{false};
        auto srcFac = [&] {
            return std::unique_ptr<IFileEnumerator>(new ReplayEnumerator(
                srcEntries, {},
                [&] { srcDone.store(true, std::memory_order_release); }));
        };
        auto dstFac = [&] {
            return std::unique_ptr<IFileEnumerator>(new ReplayEnumerator(
                dstEntries,
                [&] {
                    while (!srcDone.load(std::memory_order_acquire))
                        std::this_thread::yield();
                },
                {}));
        };
        ConcurrentComparer cmp(false, ScanMode::Presence, false, src, dst,
                               ConcurrentComparer::SourceKind::Live, nullptr);
        first = cmp.runWithFactories(srcFac, dstFac, pool);
    }
    {
        // Order 2: symmetric, destination fully inserted before source.
        std::atomic<bool> dstDone{false};
        auto srcFac = [&] {
            return std::unique_ptr<IFileEnumerator>(new ReplayEnumerator(
                srcEntries,
                [&] {
                    while (!dstDone.load(std::memory_order_acquire))
                        std::this_thread::yield();
                },
                {}));
        };
        auto dstFac = [&] {
            return std::unique_ptr<IFileEnumerator>(new ReplayEnumerator(
                dstEntries, {},
                [&] { dstDone.store(true, std::memory_order_release); }));
        };
        ConcurrentComparer cmp(false, ScanMode::Presence, false, src, dst,
                               ConcurrentComparer::SourceKind::Live, nullptr);
        second = cmp.runWithFactories(srcFac, dstFac, pool);
    }

    CHECK(first.sourceStatus == ConcurrentComparer::WorkerStatus::Success);
    CHECK(first.destinationStatus == ConcurrentComparer::WorkerStatus::Success);
    CHECK(second.sourceStatus == ConcurrentComparer::WorkerStatus::Success);
    CHECK(second.destinationStatus == ConcurrentComparer::WorkerStatus::Success);
    CHECK_MSG(SameResults(first.results, second.results),
              "order must not affect the logical results");
    CHECK_EQ(first.results.stats.missingFiles, 1ull);
    CHECK_EQ(first.results.stats.extraFiles, 1ull);
});

TEST("concurrent comparer: async hashing verifies a large identical tree", [] {
    const auto dir = MakeTempDir();
    const size_t count = 300; // > one 256-candidate hash batch: two waitAll rounds
    testgen::CreateStressTree(dir + L"\\src", count);
    fs::copy(dir + L"\\src", dir + L"\\dst", fs::copy_options::recursive);
    const auto r = RunConcurrent(dir + L"\\src", dir + L"\\dst", ScanMode::Content,
                                 false, 2);
    CHECK(r.sourceStatus == ConcurrentComparer::WorkerStatus::Success);
    CHECK(r.destinationStatus == ConcurrentComparer::WorkerStatus::Success);
    CHECK_EQ(r.results.stats.identicalFiles, count);
    CHECK_EQ(r.results.stats.contentMismatch, 0ull);
    CHECK(r.results.problems.empty());
});

TEST("concurrent comparer: hash workers always use their own candidate", [] {
    // Guards against hash-phase tasks that lose track of which candidate they
    // were submitted for (e.g. by capturing a loop-local reference). Every file
    // has a distinct path AND a distinct payload of the SAME size, so every
    // pair becomes a Content-mode candidate; a task hashing the wrong file
    // would flip identical/contentMismatch counts or report the wrong path.
    const auto dir = MakeTempDir();
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";
    fs::create_directories(src);
    fs::create_directories(dst);
    const size_t count = 600; // > two 256-candidate batches: real parallel overlap
    const size_t size = 4096; // uniform size -> every pair is a candidate
    std::vector<std::string> payloads(count);
    for (size_t i = 0; i < count; ++i) {
        std::string body = "file-" + std::to_string(i) + "-|";
        while (body.size() < size) body += static_cast<char>('a' + ((i + body.size()) % 26));
        payloads[i] = std::move(body);
        const std::wstring name = L"f" + std::to_wstring(i) + L".dat";
        CHECK(WriteFileBytes(src + L"\\" + name, payloads[i].data(), payloads[i].size()));
        CHECK(WriteFileBytes(dst + L"\\" + name, payloads[i].data(), payloads[i].size()));
    }
    // Corrupt exactly one destination file (same size) so exactly one pair is
    // a content mismatch and its path is known in advance.
    std::string broken = payloads[0];
    broken[broken.size() / 2] = 'X';
    CHECK(WriteFileBytes(dst + L"\\f0.dat", broken.data(), broken.size()));

    const auto r = RunConcurrent(src, dst, ScanMode::Content, false, 4);
    CHECK(r.sourceStatus == ConcurrentComparer::WorkerStatus::Success);
    CHECK(r.destinationStatus == ConcurrentComparer::WorkerStatus::Success);
    CHECK_EQ(r.results.stats.identicalFiles, count - 1);
    CHECK_EQ(r.results.stats.contentMismatch, 1ull);
    CHECK_EQ(r.results.stats.sizeMismatch, 0ull);
    CHECK_EQ(r.results.stats.missingFiles + r.results.stats.extraFiles, 0ull);
    CHECK_EQ(r.results.problems.size(), 1ull);
    if (r.results.problems.size() == 1) {
        CHECK(r.results.problems[0].status == Status::ContentMismatch);
        CHECK(r.results.problems[0].relativePath == L"f0.dat");
    }
});

TEST("concurrent comparer: FromIndex source verifies against a live destination", [] {
    const auto dir = MakeTempDir();
    testgen::CreateDifferingTrees(dir);
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";

    FileIndex idx(false);
    {
        Win32Enumerator en;
        const auto br = idx.build(src, en);
        CHECK(br.ok);
    }
    const auto r = RunConcurrent(src, dst, ScanMode::Presence,
                                 false, 0, ConcurrentComparer::SourceKind::FromIndex,
                                 {}, {}, &idx);
    CHECK(r.sourceStatus == ConcurrentComparer::WorkerStatus::Success);
    CHECK(r.destinationStatus == ConcurrentComparer::WorkerStatus::Success);
    const auto& s = r.results.stats;
    CHECK_EQ(s.identicalFiles, 4ull);
    CHECK_EQ(s.missingFiles, 1ull);
    CHECK_EQ(s.missingDirs, 1ull);
    CHECK_EQ(s.extraFiles, 1ull);
    CHECK_EQ(s.extraDirs, 1ull);
});

TEST("concurrent comparer: pre-emission failure falls back to the next enumerator", [] {
    const auto dir = MakeTempDir();
    testgen::CreateDifferingTrees(dir);
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";

    // Stands in for "MFT unusable": fails before emitting anything, so the side
    // must transparently retry with the Win32 enumerator that follows it.
    std::vector<ConcurrentComparer::EnumeratorFactory> srcFacs;
    srcFacs.push_back([] { return std::unique_ptr<IFileEnumerator>(new FailImmediatelyEnumerator()); });
    srcFacs.push_back(MakeWin32Factory);
    std::vector<ConcurrentComparer::EnumeratorFactory> dstFacs;
    dstFacs.push_back(MakeWin32Factory);

    bv::ThreadPool pool(0);
    ConcurrentComparer cmp(false, ScanMode::Presence, false, src, dst,
                           ConcurrentComparer::SourceKind::Live, nullptr);
    const auto r = cmp.runWithFactories(std::move(srcFacs), std::move(dstFacs), pool);

    CHECK(r.sourceStatus == ConcurrentComparer::WorkerStatus::Success);
    CHECK(r.destinationStatus == ConcurrentComparer::WorkerStatus::Success);
    const auto& s = r.results.stats;
    CHECK_EQ(s.identicalFiles, 4ull);
    CHECK_EQ(s.missingFiles, 1ull);
    CHECK_EQ(s.missingDirs, 1ull);
    CHECK_EQ(s.extraFiles, 1ull);
    CHECK_EQ(s.extraDirs, 1ull);
});

TEST("concurrent comparer: emitted-then-failed side reports no missing/extra", [] {
    const auto dir = MakeTempDir();
    testgen::CreateDifferingTrees(dir);
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";
    const auto srcEntries = CaptureEntries(src);
    const auto dstEntries = CaptureEntries(dst);

    // Source emits its whole stream, then reports an incomplete scan. The pairs
    // already matched are retained, but nothing may be reported missing/extra.
    bv::ThreadPool pool(0);
    ConcurrentComparer cmp(false, ScanMode::Presence, false, src, dst,
                           ConcurrentComparer::SourceKind::Live, nullptr);
    auto srcFac = [&] {
        return std::unique_ptr<IFileEnumerator>(
            new FailAfterNEnumerator(srcEntries, srcEntries.size()));
    };
    const auto r = cmp.runWithFactories(std::move(srcFac), MakeWin32Factory, pool);

    CHECK(r.sourceStatus == ConcurrentComparer::WorkerStatus::Failed);
    CHECK(r.destinationStatus == ConcurrentComparer::WorkerStatus::Success);
    CHECK_EQ(r.results.stats.identicalFiles, 4ull); // a, d, e, sub\f
    CHECK_EQ(r.results.stats.missingFiles, 0ull);
    CHECK_EQ(r.results.stats.missingDirs, 0ull);
    CHECK_EQ(r.results.stats.extraFiles, 0ull);
    CHECK_EQ(r.results.stats.extraDirs, 0ull);
    CHECK(r.results.problems.empty());
});

TEST("concurrent comparer: failed destination after emitting reports no missing/extra", [] {
    const auto dir = MakeTempDir();
    testgen::CreateDifferingTrees(dir);
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";
    const auto dstEntries = CaptureEntries(dst);

    bv::ThreadPool pool(0);
    ConcurrentComparer cmp(false, ScanMode::Presence, false, src, dst,
                           ConcurrentComparer::SourceKind::Live, nullptr);
    auto dstFac = [&] {
        return std::unique_ptr<IFileEnumerator>(
            new FailAfterNEnumerator(dstEntries, dstEntries.size()));
    };
    const auto r = cmp.runWithFactories(MakeWin32Factory, std::move(dstFac), pool);

    CHECK(r.sourceStatus == ConcurrentComparer::WorkerStatus::Success);
    CHECK(r.destinationStatus == ConcurrentComparer::WorkerStatus::Failed);
    CHECK_EQ(r.results.stats.identicalFiles, 4ull);
    CHECK_EQ(r.results.stats.missingFiles + r.results.stats.missingDirs +
                 r.results.stats.extraFiles + r.results.stats.extraDirs, 0ull);
});

TEST("concurrent comparer: cancel before start skips missing/extra reporting", [] {
    const auto dir = MakeTempDir();
    testgen::CreateStressTree(dir + L"\\src", 60);
    fs::copy(dir + L"\\src", dir + L"\\dst", fs::copy_options::recursive);

    std::atomic_bool cancel{true};
    bv::ThreadPool pool(0);
    ConcurrentComparer cmp(false, ScanMode::Presence, false, dir + L"\\src", dir + L"\\dst",
                           ConcurrentComparer::SourceKind::Live, nullptr, &cancel);
    const auto r = cmp.runWithFactories(MakeWin32Factory, MakeWin32Factory, pool);

    CHECK(r.sourceStatus == ConcurrentComparer::WorkerStatus::Cancelled);
    CHECK(r.destinationStatus == ConcurrentComparer::WorkerStatus::Cancelled);
    CHECK_EQ(r.results.stats.missingFiles + r.results.stats.missingDirs +
                 r.results.stats.extraFiles + r.results.stats.extraDirs, 0ull);
});

TEST("concurrent comparer: fallback and partial scans produce user-facing notes", [] {
    const auto dir = MakeTempDir();
    testgen::CreateDifferingTrees(dir);
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";

    // (1) Pre-emission failure (MFT stand-in) -> transparent Win32 fallback.
    {
        std::vector<ConcurrentComparer::EnumeratorFactory> srcFacs;
        srcFacs.push_back(
            [] { return std::unique_ptr<IFileEnumerator>(new FailImmediatelyEnumerator()); });
        srcFacs.push_back(MakeWin32Factory);
        std::vector<ConcurrentComparer::EnumeratorFactory> dstFacs;
        dstFacs.push_back(MakeWin32Factory);

        bv::ThreadPool pool(0);
        ConcurrentComparer cmp(false, ScanMode::Presence, false, src, dst,
                               ConcurrentComparer::SourceKind::Live, nullptr);
        const auto r = cmp.runWithFactories(std::move(srcFacs), std::move(dstFacs), pool);
        CHECK(r.sourceStatus == ConcurrentComparer::WorkerStatus::Success);
        CHECK_MSG(!r.notes.empty(), "a transparent fallback must be reported to the user");
    }

    // (2) Emitted-then-failed (incomplete MFT shape) -> Failed with a note, and
    //     still no missing/extra fabricated verdicts.
    {
        const auto srcEntries = CaptureEntries(src);
        const auto dstEntries = CaptureEntries(dst);
        bv::ThreadPool pool(0);
        ConcurrentComparer cmp(false, ScanMode::Presence, false, src, dst,
                               ConcurrentComparer::SourceKind::Live, nullptr);
        auto srcFac = [&] {
            return std::unique_ptr<IFileEnumerator>(
                new FailAfterNEnumerator(srcEntries, srcEntries.size()));
        };
        const auto r = cmp.runWithFactories(std::move(srcFac), MakeWin32Factory, pool);
        CHECK(r.sourceStatus == ConcurrentComparer::WorkerStatus::Failed);
        CHECK_MSG(!r.notes.empty(), "an incomplete scan must be reported to the user");
        CHECK_EQ(r.results.stats.missingFiles + r.results.stats.missingDirs +
                     r.results.stats.extraFiles + r.results.stats.extraDirs, 0ull);
        (void)dstEntries;
    }
});

// ---------------------------------------------------------------------------

int main() {
    const int rc = test::Summary();
    CleanupTempDirs();
    return rc;
}

