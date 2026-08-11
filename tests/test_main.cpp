// Unit and integration tests for Phase 1.
//
// Build: bv_tests. Run from any directory; all trees are created under the
// system temp directory and cleaned up.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <array>
#include <cstdio>
#include <thread>

#include "Comparison/ScanMode.h"
#include "Filesystem/FileIndex.h"
#include "Filesystem/MftEnumerator.h"
#include "Filesystem/PathUtil.h"
#include "Filesystem/Win32Enumerator.h"
#include "Hashing/Sha256.h"
#include "ScanController.h"
#include "TestHarness.h"
#include "TestTree.h"
#include "Threading/ThreadPool.h"

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

int main() {
    const int rc = test::Summary();
    CleanupTempDirs();
    return rc;
}
