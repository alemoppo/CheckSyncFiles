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
#include <cstdio>
#include <thread>

#include "Comparison/ScanMode.h"
#include "Comparison/FileComparator.h"
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
                                             const ProgressCallback&) override {
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

    bool sawSource = false, sawDone = false;
    uint64_t maxFiles = 0;
    opts.onProgress = [&](const bv::ScanProgress& p) {
        if (p.phase == bv::ScanPhase::EnumerateSource) {
            sawSource = true;
            maxFiles = std::max(maxFiles, p.files);
        } else if (p.phase == bv::ScanPhase::Done) {
            sawDone = true;
        }
    };

    bv::ScanController controller(false);
    controller.run(opts);
    CHECK(sawSource);
    CHECK(sawDone);
    CHECK(maxFiles >= 500);
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

int main() {
    const int rc = test::Summary();
    CleanupTempDirs();
    return rc;
}

