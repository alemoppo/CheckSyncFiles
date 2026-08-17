// Unit and integration tests for Phase 1.
//
// Build: bv_tests. Run from any directory; all trees are created under the
// system temp directory and cleaned up.

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
#include "Hashing/Sha256.h"
#include "Hashing/HashUtil.h"
#include "Profiling/HashProfile.h"
#include "ScanController.h"
#include "ScanOrchestrator.h"
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
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                // Junctions / directory symlinks must NOT be traversed here:
                // a link can point back into the tree being cleaned (a cycle).
                // Removing the link itself (not its target) avoids that loop.
                RemoveDirectoryW(pathutil::AddLongPathPrefix(child).c_str());
            } else {
                RemoveAllWin(child);
            }
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

bool DenyListAccess(const std::wstring& dir, const std::wstring& mask = L"(OI)(CI)(RD)") {
    std::wstring cmd = L"icacls \"" + dir +
                       L"\" /deny \"" + _wgetenv(L"USERNAME") +
                       L"\":" + mask + L" /C 2>nul";
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

// Creates a directory junction `link` pointing at `target` via the shell's
// `mklink /J` (a cmd built-in). The link directory must NOT already exist.
// Junctions need no administrator rights (unlike symlinks), so this is
// exercisable on an ordinary temp directory. Building the junction reparse
// buffer by hand (FSCTL_SET_REPARSE_POINT) is fragile and version-dependent,
// so we reuse the OS tool instead -- the same approach as the icacls helpers
// above. Returns false when junction creation is not supported.
bool CreateJunction(const std::wstring& link, const std::wstring& target) {
    std::wstring cmd = L"cmd /c mklink /J \"" + link + L"\" \"" + target + L"\" >nul 2>nul";
    return _wsystem(cmd.c_str()) == 0;
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

TEST("threadpool: waitOutstandingBelow() under the cap returns without waiting", [] {
    bv::ThreadPool pool(4);
    std::atomic<int> done{0};
    for (int i = 0; i < 10; ++i) pool.submit([&] { done.fetch_add(1); });
    // A generous cap is satisfied immediately: no blocking, unlike waitAll().
    pool.waitOutstandingBelow(100);
    pool.waitAll();
    CHECK_EQ(done.load(), 10);
});

TEST("threadpool: waitOutstandingBelow() blocks until in-flight drops below the cap", [] {
    bv::ThreadPool pool(1);
    std::atomic<bool> taskStarted{false};
    std::atomic<bool> waitReturned{false};
    std::promise<void> gate;

    pool.submit([&] {
        taskStarted.store(true, std::memory_order_release);
        gate.get_future().wait();
    });
    while (!taskStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    // The single task is still running => 1 outstanding. Cap 0 must not be met
    // until the task finishes.
    std::thread waiter([&] {
        pool.waitOutstandingBelow(0);
        waitReturned.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_MSG(!waitReturned.load(std::memory_order_acquire),
              "waitOutstandingBelow(0) returned while a task was still in flight");
    gate.set_value();
    waiter.join();
    CHECK(waitReturned.load(std::memory_order_acquire));
});

TEST("threadpool: waitOutstandingBelow() throttles, not drains", [] {
    // pool(1): task 0 blocks on the first gate; tasks 1..7 block on a second
    // gate kept closed until after the wait returns. While both gates are shut,
    // 8 tasks are submitted and none has finished (outstanding == 8), so
    // waitOutstandingBelow(7) must NOT return. Opening only the first gate lets
    // exactly task 0 finish: outstanding drops to 7 (the cap) and the wait
    // returns while tasks 1..7 are still blocked -- the pool is throttled, not
    // drained. Releasing the second gate then finishes everything.
    bv::ThreadPool pool(1);
    const int k = 8;
    std::atomic<int> started{0};
    std::promise<void> gate0;
    std::promise<void> gate1p;
    std::shared_future<void> gate1 = gate1p.get_future();

    pool.submit([&] {
        started.fetch_add(1);
        gate0.get_future().wait();
    });
    for (int i = 1; i < k; ++i) {
        pool.submit([&] {
            started.fetch_add(1);
            gate1.wait();
        });
    }
    // Task 0 has been dequeued and is blocked; tasks 1..7 sit queued behind it,
    // so all 8 tasks are submitted-but-not-finished (outstanding == 8).
    while (started.load() < 1) std::this_thread::yield();

    std::atomic<bool> waitReturned{false};
    std::thread waiter([&] {
        pool.waitOutstandingBelow(k - 1);
        waitReturned.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_MSG(!waitReturned.load(std::memory_order_acquire),
              "waitOutstandingBelow(k-1) returned while k tasks were in flight");
    gate0.set_value();
    waiter.join();
    CHECK(waitReturned.load(std::memory_order_acquire));
    // Only some of the work completed before the wait returned (in-flight fell to
    // the cap); the rest is still queued/blocked -- the pool was NOT drained.
    const int startedAtReturn = started.load();
    CHECK_MSG(startedAtReturn >= 1 && startedAtReturn < k,
              "waitOutstandingBelow() returned only after partial completion");
    gate1p.set_value();
    pool.waitAll();
    CHECK_EQ(started.load(), k);
});

// ---------------------------------------------------------------------------
// Content-hash profiling (Phase: instrumentation)
// ---------------------------------------------------------------------------

TEST("sha256: timings fill bytesRead and separate read/hash time", [] {
    // A file larger than the 1 MiB streaming chunk forces several ReadFile /
    // BCryptHashData iterations, so the cumulative counters are exercised.
    const auto dir = MakeTempDir();
    const std::wstring path = dir + L"\\big.bin";
    const size_t kBytes = 2 * 1024 * 1024 + 12345; // > 1 MiB chunk, odd tail
    {
        std::vector<char> data(kBytes, 'x');
        const HANDLE h = CreateFileW(pathutil::AddLongPathPrefix(path).c_str(), GENERIC_WRITE,
                                     0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            CHECK(WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr));
            CloseHandle(h);
        }
    }

    std::array<uint8_t, 32> plain;
    CHECK(hashing::Sha256File(path, plain) == hashing::HashStatus::Ok);

    std::array<uint8_t, 32> profiled;
    bv::profiling::FileTimings t;
    CHECK(hashing::Sha256File(path, profiled, &t) == hashing::HashStatus::Ok);
    CHECK(plain == profiled); // timing must not change the digest
    CHECK_EQ(t.bytesRead, static_cast<uint64_t>(kBytes));
    CHECK(t.totalTicks > 0);
    // read + hash fit inside the total span; both are non-negative by construction.
    CHECK(t.readTicks + t.hashTicks <= t.totalTicks);
});

TEST("profiling: live-live candidates record both sides per job", [] {
    const auto dir = MakeTempDir();
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";
    fs::create_directories(src);
    fs::create_directories(dst);
    const std::vector<std::pair<std::wstring, size_t>> files = {
        {L"a.txt", 1000}, {L"b.bin", 2500000}, {L"sub\\c.txt", 4096}};
    for (const auto& [rel, n] : files) {
        const std::wstring relDir = (rel.find(L'\\') != std::wstring::npos)
                                        ? rel.substr(0, rel.find(L'\\'))
                                        : L"";
        if (!relDir.empty()) {
            fs::create_directories(src + L"\\" + relDir);
            fs::create_directories(dst + L"\\" + relDir);
        }
        const std::string body(n, 'z');
        CHECK(WriteFileBytes(src + L"\\" + rel, body.data(), body.size()));
        CHECK(WriteFileBytes(dst + L"\\" + rel, body.data(), body.size()));
    }

    std::vector<ContentCandidate> candidates;
    for (const auto& [rel, n] : files) {
        uint64_t sz = 0, mt = 0;
        CHECK(hashing::StatFile(src + L"\\" + rel, sz, mt));
        uint64_t dsz = 0, dmt = 0;
        CHECK(hashing::StatFile(dst + L"\\" + rel, dsz, dmt));
        ContentCandidate c;
        c.relativePath = rel;
        c.sizeSource = sz;
        c.sizeDest = dsz;
        c.srcMtime = mt;
        c.dstMtime = dmt;
        candidates.push_back(std::move(c));
    }

    std::atomic<size_t> hits{0};
    std::atomic_bool cancel{false};
    bv::profiling::HashProfiler prof(/*verboseJobs=*/true);
    prof.setEnabled(true);
    {
        ConcurrentSink sink;
        ThreadPool pool(2);
        SubmitHashCandidates(candidates, pool, /*offline=*/false, nullptr, src, dst, sink,
                             &cancel, nullptr, hits, nullptr, &prof);
        pool.waitAll();
        const ResultSet r = sink.take();
        CHECK_EQ(r.stats.identicalFiles, 3ull);
        CHECK_EQ(r.stats.changedDuringScan, 0ull);
    }

    bv::profiling::HashProfileReport rep;
    prof.Finalize(rep);
    CHECK_EQ(rep.tasks, 3ull);             // one task per candidate
    CHECK_EQ(rep.taskFailed, 0ull);
    CHECK_EQ(rep.activeJobsAtEnd, 0ull);   // every task released its slot
    CHECK_MSG(rep.maxActiveJobs >= 1, "at least one hash job ran");
    const auto& a = rep.side[static_cast<int>(bv::profiling::Side::Source)];
    const auto& b = rep.side[static_cast<int>(bv::profiling::Side::Dest)];
    CHECK_EQ(a.files, 3ull);
    CHECK_EQ(b.files, 3ull);
    CHECK_EQ(a.bytes, b.bytes);
    CHECK_EQ(a.failed, 0ull);
    CHECK_EQ(b.failed, 0ull);
    CHECK(a.totalTicks > 0 && b.totalTicks > 0);
    // Each side was hashed once per candidate: 3 A records + 3 B records.
    CHECK_EQ(prof.jobRecords().size(), 6ull);
    for (const auto& r : prof.jobRecords()) {
        CHECK(r.ok);
        CHECK_EQ(r.bytesRead, r.expectedSize);
        CHECK(r.readTicks + r.hashTicks <= r.totalTicks);
    }
});

TEST("profiling: disabled profiler records nothing", [] {
    const auto dir = MakeTempDir();
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";
    fs::create_directories(src);
    fs::create_directories(dst);
    CHECK(WriteFileBytes(src + L"\\f.txt", "hello", 5));
    CHECK(WriteFileBytes(dst + L"\\f.txt", "hello", 5));

    std::vector<ContentCandidate> candidates;
    {
        uint64_t sz = 0, mt = 0;
        uint64_t dsz = 0, dmt = 0;
        CHECK(hashing::StatFile(src + L"\\f.txt", sz, mt));
        CHECK(hashing::StatFile(dst + L"\\f.txt", dsz, dmt));
        ContentCandidate c;
        c.relativePath = L"f.txt";
        c.sizeSource = sz;
        c.sizeDest = dsz;
        c.srcMtime = mt;
        c.dstMtime = dmt;
        candidates.push_back(std::move(c));
    }

    std::atomic<size_t> hits{0};
    std::atomic_bool cancel{false};
    bv::profiling::HashProfiler prof; // left disabled
    {
        ConcurrentSink sink;
        ThreadPool pool(2);
        SubmitHashCandidates(candidates, pool, /*offline=*/false, nullptr, src, dst, sink,
                             &cancel, nullptr, hits, nullptr, &prof);
        pool.waitAll();
        const ResultSet r = sink.take();
        CHECK_EQ(r.stats.identicalFiles, 1ull); // behaviour unchanged
    }
    bv::profiling::HashProfileReport rep;
    prof.Finalize(rep);
    CHECK_EQ(rep.tasks, 0ull);
    CHECK(prof.jobRecords().empty());
});

TEST("threadpool: metrics track backpressure waits and outstanding", [] {
    bv::ThreadPool pool(1);
    std::atomic<bool> taskStarted{false};
    std::atomic<bool> waitReturned{false};
    std::promise<void> gate;

    pool.submit([&] {
        taskStarted.store(true, std::memory_order_release);
        gate.get_future().wait();
    });
    while (!taskStarted.load(std::memory_order_acquire)) std::this_thread::yield();
    pool.submit([] {}); // second task queued behind the blocked one
    // A moment later the pool's passive counters must reflect 2 outstanding /
    // 1 queued, and the blocking wait below must be measured.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        const auto m = pool.metrics();
        CHECK_MSG(m.maxOutstanding >= 2, "submitted-but-not-finished must be >= 2");
        CHECK_MSG(m.maxQueueDepth >= 1, "one task must sit in the queue");
    }
    std::thread waiter([&] {
        pool.waitOutstandingBelow(0);
        waitReturned.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_MSG(!waitReturned.load(std::memory_order_acquire),
              "waitOutstandingBelow(0) returned while a task was blocked");
    gate.set_value();
    waiter.join();
    CHECK(waitReturned.load(std::memory_order_acquire));
    const auto m = pool.metrics();
    CHECK_MSG(m.backpressureWaits >= 1, "the blocking wait must be counted");
    CHECK_MSG(m.backpressureWaitTicks > 0, "the blocking wait must be timed");
    pool.waitAll();
});

TEST("profiling: ScanController content run fills report and keeps results", [] {
    const auto dir = MakeTempDir();
    const size_t count = 200;
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";
    testgen::CreateStressTree(src, count);
    fs::copy(src, dst, fs::copy_options::recursive);

    bv::ScanOptions opts;
    opts.source = src;
    opts.destination = dst;
    opts.mode = bv::ScanMode::Content;
    opts.hashThreads = 2;
    bv::profiling::HashProfiler prof;
    opts.hashProfiler = &prof;
    ScanController controller(false);
    const ScanReport report = controller.run(opts);

    CHECK_EQ(report.results.stats.identicalFiles, count);
    CHECK(report.results.problems.empty());
    CHECK_EQ(report.hashProfile.tasks, count);
    CHECK_EQ(report.hashProfile.activeJobsAtEnd, 0ull);
    CHECK_EQ(report.hashProfile.side[0].files, count);
    CHECK_EQ(report.hashProfile.side[1].files, count);
    CHECK_EQ(report.hashProfile.side[0].bytes, report.hashProfile.side[1].bytes);
    CHECK(report.hashProfile.side[0].bytes > 0);
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

} // namespace

TEST("mft: external $I30 via $ATTRIBUTE_LIST resolves children (in-memory fixture)", [] {
    // Full production chain -- USA fixup, ParseRecord, Pass A base-record
    // reference merge, Pass B $ATTRIBUTE_LIST merge, $I30 resolution, WIN32
    // child name resolution -- on the audited layout. A regression in ANY merge
    // step leaves the parser-only synthetic tests green but makes this fail.
    auto records = BuildAuditedFixture();
    std::vector<std::pair<std::wstring, uint64_t>> entries;
    bool incomplete = false;
    const bool resolved =
        MftEnumerator::ResolveDirectoryForTest(records, 4609, entries, incomplete);
    CHECK(resolved);
    CHECK(!incomplete);
    if (!resolved || incomplete) return; // nothing meaningful to assert past here
    CHECK_EQ(entries.size(), 3u);
    if (entries.size() != 3) return;
    CHECK(entries[0].first == L"file1.txt");
    CHECK_EQ(entries[0].second, 4610u);
    CHECK(entries[1].first == L"file2.txt");
    CHECK_EQ(entries[1].second, 4611u);
    CHECK(entries[2].first == L"sub");
    CHECK_EQ(entries[2].second, 4612u);

    // Honesty rule: drop the extension record and the directory has no
    // resolvable $I30 -- the seam must report incompleteness, never silence.
    auto broken = BuildAuditedFixture();
    broken.erase(4613);
    std::vector<std::pair<std::wstring, uint64_t>> empty;
    bool brokenIncomplete = false;
    CHECK(!MftEnumerator::ResolveDirectoryForTest(broken, 4609, empty, brokenIncomplete));
    CHECK(brokenIncomplete);
    CHECK(empty.empty());
});

TEST("mft: $ATTRIBUTE_LIST external $INDEX_ALLOCATION [$I30] is merged", [] {
    // The full $ATTRIBUTE_LIST -> external NON-RESIDENT $INDEX_ALLOCATION chain:
    // base 4613 carries only the list; extension 4609 holds the (empty) root
    // and the VCN-0 leaf, extension 4614 the VCN-1 leaf. The seam drives the
    // SAME production merge (MergePassAFromRecord / MergePassBFromList) and the
    // real index reconstruction (ReadIndexAllocationStream +
    // ParseIndexAllocationData); all data is in memory, no volume access.
    auto fx = BuildExternalIaFixture();
    std::vector<std::pair<std::wstring, uint64_t>> entries;
    bool incomplete = false;
    size_t pieces = 999;
    const bool resolved = MftEnumerator::ResolveDirectoryForTest(
        fx.records, 4613, entries, incomplete, &fx.clusters, 4096, 512, &pieces);
    CHECK(resolved);
    CHECK(!incomplete);
    if (!resolved || incomplete) return;
    // Dedupe observable: VCN 0 is referenced TWICE in the list (and VCN 1 once
    // by Pass A) -- yet only ONE piece per VCN range reaches the stream, so
    // exactly two distinct pieces are merged.
    CHECK_EQ(pieces, 2u);
    // Every child from both leaf blocks is present exactly once: VCN 0
    // contributed file1/file2, VCN 1 file3/file4. The merged pieces arrive in
    // NON-VCN order (Pass A runs before Pass B, so VCN 1 is pushed first), so
    // the reassembly is only complete if the stream is rebuilt by lowestVcn --
    // never by record/list order and never just the first piece.
    CHECK_EQ(entries.size(), 4u);
    if (entries.size() != 4) return;
    CHECK(entries[0].first == L"file1.txt");
    CHECK_EQ(entries[0].second, 4620u);
    CHECK(entries[1].first == L"file2.txt");
    CHECK_EQ(entries[1].second, 4621u);
    CHECK(entries[2].first == L"file3.txt");
    CHECK_EQ(entries[2].second, 4622u);
    CHECK(entries[3].first == L"file4.txt");
    CHECK_EQ(entries[3].second, 4623u);
    // No child is emitted twice (a duplicated piece must not duplicate a child).
    for (size_t i = 0; i < entries.size(); ++i) {
        for (size_t j = i + 1; j < entries.size(); ++j) {
            CHECK(entries[i].second != entries[j].second);
        }
    }
});

TEST("mft: $ATTRIBUTE_LIST referencing a bad extension is not silently complete", [] {
    // A malformed follow must never yield a directory that LOOKS complete: the
    // seam must report incompleteness. Two corruptions: a wrong sequence number
    // and a reference to a nonexistent record. In both, the only $I30 source is
    // extension 4609 BELOW the base (Pass A's ascending-order merge cannot reach
    // it), so a rejected list entry leaves the directory with no readable $I30.
    const auto baseWithList = [&](std::vector<uint8_t> list) {
        std::map<uint64_t, std::vector<uint8_t>> records;
        auto base = BuildFileRecord(4613, 1, 0x0003, 0);
        AppendResidentAttr(base, 0x30, L"", BuildFileNameValue(4600, 1, L"EXT_IA_DIR", 1));
        list.push_back(0);
        AppendResidentAttr(base, 0x20, L"", list);
        FinishRecord(base);
        records[4613] = std::move(base);
        auto ext = BuildFileRecord(4609, 1, 0x0003, 4613ull | (1ull << 48));
        AppendResidentAttr(ext, 0x90, L"$I30", BuildEmptyIndexRootValue());
        AppendNonResidentAttr(ext, 0xA0, L"$I30", 0, 0, 100, 4096);
        FinishRecord(ext);
        records[4609] = std::move(ext);
        return records;
    };

    // Wrong sequence number on every $I30 entry: the follow rejects the piece
    // (the live record's sequence 1 != the listed 2).
    std::vector<uint8_t> wrongSeq;
    PushAttrListEntry(wrongSeq, 0x90, L"$I30", 4609, 2, 0);
    PushAttrListEntry(wrongSeq, 0xA0, L"$I30", 4609, 2, 0);
    auto r1 = baseWithList(wrongSeq);
    std::vector<std::pair<std::wstring, uint64_t>> e1;
    bool inc1 = false;
    CHECK(!MftEnumerator::ResolveDirectoryForTest(r1, 4613, e1, inc1));
    CHECK(inc1);
    CHECK(e1.empty());

    // Nonexistent record: the follow must not guess at the missing piece.
    std::vector<uint8_t> missing;
    PushAttrListEntry(missing, 0x90, L"$I30", 4700, 1, 0);
    PushAttrListEntry(missing, 0xA0, L"$I30", 4700, 1, 0);
    auto r2 = baseWithList(missing);
    std::vector<std::pair<std::wstring, uint64_t>> e2;
    bool inc2 = false;
    CHECK(!MftEnumerator::ResolveDirectoryForTest(r2, 4613, e2, inc2));
    CHECK(inc2);
    CHECK(e2.empty());
});

TEST("mft: per-directory Win32 fallback enumerates a subtree with prefixed paths", [] {
    // The per-directory Win32 fallback seam drives Win32Enumerator on ONE
    // directory and joins every emitted relative path with the directory's
    // scan-root-relative prefix, so the subtree plugs into the same tree/flow
    // the MFT walk builds. Real temp tree: no volume, no elevation required.
    const std::wstring dir = MakeTempDir();
    const std::wstring sub = dir + L"\\sub";
    fs::create_directories(sub + L"\\deep");
    CHECK(WriteFileBytes(dir + L"\\a.txt", "a", 1));
    CHECK(WriteFileBytes(dir + L"\\b.bin", "bb", 2));
    CHECK(WriteFileBytes(sub + L"\\c.txt", "ccc", 3));
    CHECK(WriteFileBytes(sub + L"\\deep\\d.txt", "dddd", 4));

    std::vector<std::wstring> rels;
    std::vector<std::wstring> progPaths;
    uint64_t fbFiles = 0;
    uint64_t fbDirs = 0;
    uint64_t fbBytes = 0;
    const auto st = MftEnumerator::EnumerateWin32Subtree(
        dir, L"fb", fbFiles, fbDirs, fbBytes,
        [&](FileEntry&& e) {
            rels.push_back(e.relativePath);
            return true;
        },
        [](const ScanError&) {},
        [&](uint64_t, uint64_t, uint64_t, const std::wstring& p) { progPaths.push_back(p); }, nullptr);
    CHECK(st == MftEnumerator::SubtreeStatus::Ok);
    CHECK_EQ(rels.size(), 6u);
    if (rels.size() != 6) return;
    CHECK_EQ(fbFiles, 4u);
    CHECK_EQ(fbDirs, 2u);
    CHECK_EQ(fbBytes, 10u); // 1+2+3+4 bytes of the four files
    CHECK(std::find(rels.begin(), rels.end(), L"fb\\a.txt") != rels.end());
    CHECK(std::find(rels.begin(), rels.end(), L"fb\\b.bin") != rels.end());
    CHECK(std::find(rels.begin(), rels.end(), L"fb\\sub") != rels.end());
    CHECK(std::find(rels.begin(), rels.end(), L"fb\\sub\\c.txt") != rels.end());
    CHECK(std::find(rels.begin(), rels.end(), L"fb\\sub\\deep") != rels.end());
    // Subdirectories are fully enumerated in the one call, so the MFT caller
    // must NOT descend again (a second descent would duplicate entries).
    CHECK(std::find(rels.begin(), rels.end(), L"fb\\sub\\deep\\d.txt") != rels.end());
    // Progress reports the prefixed, scan-root-relative directory paths.
    CHECK(std::find(progPaths.begin(), progPaths.end(), L"fb\\sub\\deep") != progPaths.end());

    // Empty prefix == the scan root itself: paths stay relative to the root.
    std::vector<std::wstring> rootRels;
    uint64_t rf = 0;
    uint64_t rd = 0;
    uint64_t rbytes = 0;
    const auto st2 = MftEnumerator::EnumerateWin32Subtree(
        dir, L"", rf, rd, rbytes,
        [&](FileEntry&& e) {
            rootRels.push_back(e.relativePath);
            return true;
        },
        [](const ScanError&) {}, {}, nullptr);
    CHECK(st2 == MftEnumerator::SubtreeStatus::Ok);
    CHECK_EQ(rootRels.size(), 6u);
    CHECK_EQ(rf, 4u);
    CHECK_EQ(rd, 2u);
    CHECK_EQ(rbytes, 10u);
    CHECK(std::find(rootRels.begin(), rootRels.end(), L"a.txt") != rootRels.end());
    CHECK(std::find(rootRels.begin(), rootRels.end(), L"sub\\deep\\d.txt") != rootRels.end());

    // Consumer abort: onEntry returns false -> Aborted, never Ok.
    uint64_t af = 0;
    uint64_t ad = 0;
    uint64_t abyte = 0;
    int seen = 0;
    const auto st3 = MftEnumerator::EnumerateWin32Subtree(
        dir, L"", af, ad, abyte, [&](FileEntry&&) { return ++seen < 3; },
        [](const ScanError&) {}, {}, nullptr);
    CHECK(st3 == MftEnumerator::SubtreeStatus::Aborted);

    // Unreadable root (does not exist): Unreadable, not DeviceLost.
    uint64_t uf = 0;
    uint64_t ud = 0;
    uint64_t ubyte = 0;
    const auto st4 = MftEnumerator::EnumerateWin32Subtree(
        dir + L"\\no_such_dir", L"", uf, ud, ubyte, [](FileEntry&&) { return true; },
        [](const ScanError&) {}, {}, nullptr);
    CHECK(st4 == MftEnumerator::SubtreeStatus::Unreadable);
});

TEST("mft: fallback Win32 error path is relative to the scan root (prefixed once)", [] {
    // Regression for a concrete bug: when the per-directory Win32 fallback
    // reported a ScanError (e.g. an ACL-denied subdirectory inside the subtree),
    // the error's `path` was relative to the FALLBACK directory only, not the
    // scan root -- so it was attributed to the wrong location. The fix joins
    // every error path with the fallback directory's relPrefix, exactly as the
    // entry callback does (ScanError.path documented scan-root-relative in
    // FileEnumerator.h).
    const std::wstring dir = MakeTempDir();
    fs::create_directories(dir + L"\\sub\\denied");
    fs::create_directories(dir + L"\\sub\\deep");
    CHECK(WriteFileBytes(dir + L"\\a.txt", "a", 1));
    CHECK(WriteFileBytes(dir + L"\\sub\\denied\\secret.txt", "s", 1));
    CHECK(WriteFileBytes(dir + L"\\sub\\deep\\d.txt", "dddd", 4));

    if (!DenyListAccess(dir + L"\\sub\\denied")) {
        std::cout << "  (icacls non disponibile, test saltato)\n";
        RestoreAccess(dir + L"\\sub\\denied");
        return;
    }
    ScopeGuard restore{[&] { RestoreAccess(dir + L"\\sub\\denied"); }};

    std::vector<ScanError> errs;
    uint64_t ef = 0, ed = 0, eb = 0;
    const auto st = MftEnumerator::EnumerateWin32Subtree(
        dir, L"fb", ef, ed, eb,
        [](FileEntry&&) { return true; },
        [&](const ScanError& e) { errs.push_back(e); },
        [](uint64_t, uint64_t, uint64_t, const std::wstring&) {}, nullptr);

    // The walk completed cleanly for the rest of the tree (the denied dir is a
    // non-fatal error, never a device loss / unreadable root).
    CHECK(st == MftEnumerator::SubtreeStatus::Ok);
    CHECK(!errs.empty());
    // At least one non-lostDevice error must point at the denied dir, reported
    // scan-root-relative as "fb\\sub\\denied" -- the prefix applied exactly once.
    const auto it = std::find_if(errs.begin(), errs.end(), [](const ScanError& e) {
        return !e.lostDevice && !e.path.empty();
    });
    CHECK_MSG(it != errs.end(), "expected a subdirectory access error");
    if (it != errs.end()) {
        CHECK(it->path == L"fb\\sub\\denied");
        // The fallback prefix ("fb") must appear exactly once.
        CHECK_EQ(it->path.find(L"fb\\fb"), std::wstring::npos);
    }
});

TEST("mft: walk -> unresolvable $I30 -> Win32 fallback (real temp tree)", [] {
    // Deterministic exercise of the REAL trigger path (walk -> $I30 failure ->
    // Win32 fallback) via the WalkDirectoryStepForTest seam. The seam runs the
    // SAME per-directory step enumerate() uses; the fallback then enumerates a
    // real temp tree. No volume/admin needed.
    const std::wstring dir = MakeTempDir();
    fs::create_directories(dir + L"\\sub\\deep");
    CHECK(WriteFileBytes(dir + L"\\a.txt", "a", 1));
    CHECK(WriteFileBytes(dir + L"\\b.bin", "bb", 2));
    CHECK(WriteFileBytes(dir + L"\\sub\\c.txt", "ccc", 3));
    CHECK(WriteFileBytes(dir + L"\\sub\\deep\\d.txt", "dddd", 4));

    std::atomic_bool cancel{false};

    // Fixture A: directory 4609 whose $I30 lists child ref 4610 which does NOT
    // exist in the record set (4610 >= nRecords=4610). The walk cannot resolve
    // it -> needWin32Fallback -> Win32 fallback on the real `dir`.
    {
        std::map<uint64_t, std::vector<uint8_t>> records;
        auto d = BuildFileRecord(4609, 1, 0x0003, 0); // in-use + directory
        AppendResidentAttr(d, 0x30, L"", BuildFileNameValue(4608, 1, L"dir4609", 1));
        AppendResidentAttr(d, 0x90, L"$I30", BuildIndexRootValueWith({{4610ull | (1ull << 48), L"MISSING"}}));
        FinishRecord(d);
        records[4609] = std::move(d);

        std::vector<std::pair<std::wstring, uint64_t>> mftEntries;
        std::vector<FileEntry> fbEntries;
        size_t fallbackDirs = 0;
        const auto out = MftEnumerator::WalkDirectoryStepForTest(
            records, 4609, L"", dir, mftEntries,
            [&](FileEntry&& e) {
                fbEntries.push_back(std::move(e));
                return true;
            },
            [](const ScanError&) {},
            [](uint64_t, uint64_t, uint64_t, const std::wstring&) {}, &cancel, &fallbackDirs);

        // The fallback fired exactly once for directory 4609.
        CHECK(out == MftEnumerator::DirWalkOutcome::FallbackOk);
        CHECK_EQ(fallbackDirs, 1u);
        // No MFT-sourced children: the directory was either fully resolved or
        // fully Win32-enumerated, never partially (no MFT entries emitted).
        CHECK(mftEntries.empty());
        // The whole subtree came from Win32: every entry has fileId==0 and the
        // expected prefixed (empty relPrefix -> no prefix) names.
        CHECK(!fbEntries.empty());
        for (const auto& e : fbEntries) {
            CHECK_EQ(e.fileId, 0u); // Win32-sourced, never MFT
        }
        std::set<std::wstring> got;
        for (const auto& e : fbEntries) got.insert(e.relativePath);
        CHECK(got.count(L"a.txt"));
        CHECK(got.count(L"b.bin"));
        CHECK(got.count(L"sub"));
        CHECK(got.count(L"sub\\c.txt"));
        CHECK(got.count(L"sub\\deep"));
        CHECK(got.count(L"sub\\deep\\d.txt"));
    }

    // Fixture B: directory 4609 listing a REAL, resolvable child 4610 (file).
    // The walk resolves it from the MFT -> no fallback, children carry their
    // MFT record number (siblings/parent stay MFT-backed).
    {
        std::map<uint64_t, std::vector<uint8_t>> records;
        auto d = BuildFileRecord(4609, 1, 0x0003, 0);
        AppendResidentAttr(d, 0x30, L"", BuildFileNameValue(4608, 1, L"dir4609", 1));
        AppendResidentAttr(d, 0x90, L"$I30",
                           BuildIndexRootValueWith({{4610ull | (1ull << 48), L"file1.txt"}}));
        FinishRecord(d);
        records[4609] = std::move(d);
        auto f = BuildFileRecord(4610, 1, 0x0001, 0); // in-use + file
        AppendResidentAttr(f, 0x30, L"", BuildFileNameValue(4609, 1, L"file1.txt", 1));
        FinishRecord(f);
        records[4610] = std::move(f);

        std::vector<std::pair<std::wstring, uint64_t>> mftEntries;
        std::vector<FileEntry> fbEntries;
        size_t fallbackDirs = 0;
        const auto out = MftEnumerator::WalkDirectoryStepForTest(
            records, 4609, L"", dir, mftEntries,
            [&](FileEntry&& e) {
                fbEntries.push_back(std::move(e));
                return true;
            },
            [](const ScanError&) {},
            [](uint64_t, uint64_t, uint64_t, const std::wstring&) {}, nullptr, &fallbackDirs);

        CHECK(out == MftEnumerator::DirWalkOutcome::MftResolved);
        CHECK_EQ(fallbackDirs, 0u);
        CHECK(fbEntries.empty());            // Win32 never ran
        CHECK_EQ(mftEntries.size(), 1u);
        CHECK(mftEntries[0].first == L"file1.txt");
        CHECK_EQ(mftEntries[0].second, 4610u); // MFT record number, not 0
    }
});

TEST("mft: walk -> extension record carrying $FILE_NAME is not emitted as a phantom child", [] {
    // Regression for the real E: volume bug (mcnext "sNa-sNb.zip"). NTFS moved a
    // file's $FILE_NAME into an EXTENSION record (base record reference @+32 != 0)
    // when the base record's fixed 1 KB slot overflowed with a huge $DATA run
    // list. The base record then holds NO $FILE_NAME at all, and the parent
    // directory's $I30 still references only the BASE record. Before the fix the
    // parent-pointer union registered the extension record (its $FILE_NAME points
    // at this directory) as a standalone child, so the walk emitted the same path
    // TWICE: once from the $I30 with the base's real size, once as a phantom under
    // the extension record, whose size is 0 (extension records carry no $DATA) --
    // the CSV signature seen on E: (DIM_DIVERSA + EXTRA for the same path).
    // After the fix the walk must list the file exactly once, under the BASE
    // record, and the extension record must never surface as a child.
    const uint64_t baseRef = 4701ull | (1ull << 48);
    const std::vector<uint8_t> dataVal(128, 0xAB); // resident base $DATA

    std::map<uint64_t, std::vector<uint8_t>> records;
    auto d = BuildFileRecord(4700, 1, 0x0003, 0); // parent dir: in-use + directory
    AppendResidentAttr(d, 0x30, L"", BuildFileNameValue(4699, 1, L"dir4700", 1));
    AppendResidentAttr(d, 0x90, L"$I30",
                       BuildIndexRootValueWith({{baseRef, L"s2a-s2b.zip"}}));
    FinishRecord(d);
    records[4700] = std::move(d);

    auto base = BuildFileRecord(4701, 1, 0x0001, 0); // base: in-use + file, NO $FILE_NAME
    AppendResidentAttr(base, 0x80, L"", dataVal);
    FinishRecord(base);
    const std::vector<uint8_t> baseBytes = base; // copy before the move below
    records[4701] = std::move(base);

    auto ext = BuildFileRecord(4702, 1, 0x0001, baseRef); // extension of 4701
    AppendResidentAttr(ext, 0x30, L"", BuildFileNameValue(4700, 1, L"s2a-s2b.zip", 0));
    FinishRecord(ext);
    records[4702] = std::move(ext);

    // The base carries the real size ($DATA) but no name; the extension carries
    // the name but no $DATA (size 0) -- the phantom-twins arrangement on E:.
    const auto baseResult = MftEnumerator::ParseRecordForTest(baseBytes);
    CHECK(baseResult.parsed && baseResult.inUse && !baseResult.isDir);
    CHECK_EQ(baseResult.dataSize, 128u);
    const auto extResult = MftEnumerator::ParseRecordForTest(records[4702]);
    CHECK(extResult.parsed && extResult.inUse);
    CHECK_EQ(extResult.dataSize, 0u);

    std::vector<std::pair<std::wstring, uint64_t>> mftEntries;
    std::vector<FileEntry> fbEntries;
    size_t fallbackDirs = 0;
    std::atomic_bool cancel{false};
    const auto out = MftEnumerator::WalkDirectoryStepForTest(
        records, 4700, L"", L"", mftEntries,
        [&](FileEntry&& e) { fbEntries.push_back(std::move(e)); return true; },
        [](const ScanError&) {}, [](uint64_t, uint64_t, uint64_t, const std::wstring&) {},
        &cancel, &fallbackDirs);

    CHECK(out == MftEnumerator::DirWalkOutcome::MftResolved);
    CHECK_EQ(fallbackDirs, 0u);
    CHECK(fbEntries.empty()); // Win32 never ran
    CHECK_EQ(mftEntries.size(), 1u); // exactly one emission, never a phantom twin
    CHECK(mftEntries[0].first == L"s2a-s2b.zip");
    CHECK_EQ(mftEntries[0].second, 4701u); // the BASE record, never the extension 4702
});

TEST("mft: walk -> base owning $FILE_NAME + extension $FILE_NAMEs stays one identity per dir (dedupe + hard link)", [] {
    // The complement of the regression above: the base record e31b1e5 fixes can
    // also ALREADY carry a $FILE_NAME of its own while its extension records
    // relocate MORE $FILE_NAMEs. Three distinct invariants are pinned here and
    // each must hold together (none was exercised by the synthetic suite before):
    //
    //   * Caso C -- an extension record repeats the base's OWN (parent,ns,name)
    //     $FILE_NAME: the MergePassAFromRecord dedupe must drop the duplicate, so
    //     the name list stays {mixed.bin} and the directory emits ONE child.
    //   * Caso E (hard link via extension) -- a second parent (dir 4600) indexes
    //     the SAME base record, and its $FILE_NAME (alias.bin) lives ONLY in the
    //     extension record 4702. By construction real NTFS hard links come BEFORE
    //     the merge, so the base's link under dir 4600 must be emitted exactly
    //     once, appended with the extension's name -- never as a phantom child of
    //     record 4702.
    //   * Caso D -- multiple extension records (4702 + 4703) must all collapse
    //     into the base identity; neither extension may surface as a child.
    //
    //   dir 4700 index -> base 4701 "mixed.bin"
    //   dir 4600 index -> base 4701 "alias.bin"
    //   base 4701: $FILE_NAME(parent 4700, "mixed.bin") + $DATA 128, no baseRef
    //   ext1 4702 (baseRef 4701): $FILE_NAME(parent 4600, "alias.bin")
    //   ext2 4703 (baseRef 4701): $FILE_NAME(parent 4700, "mixed.bin")  [dup of base]
    const uint64_t baseRef = 4701ull | (1ull << 48);
    std::map<uint64_t, std::vector<uint8_t>> records;

    auto d0 = BuildFileRecord(4700, 1, 0x0003, 0); // in-use + directory
    AppendResidentAttr(d0, 0x30, L"", BuildFileNameValue(4699, 1, L"dir4700", 1));
    AppendResidentAttr(d0, 0x90, L"$I30",
                       BuildIndexRootValueWith({{baseRef, L"mixed.bin"}}));
    FinishRecord(d0);
    records[4700] = std::move(d0);

    auto d1 = BuildFileRecord(4600, 1, 0x0003, 0); // in-use + directory
    AppendResidentAttr(d1, 0x30, L"", BuildFileNameValue(4599, 1, L"dir4600", 1));
    AppendResidentAttr(d1, 0x90, L"$I30",
                       BuildIndexRootValueWith({{baseRef, L"alias.bin"}}));
    FinishRecord(d1);
    records[4600] = std::move(d1);

    auto base = BuildFileRecord(4701, 1, 0x0001, 0); // in-use + file, owns mixed.bin
    AppendResidentAttr(base, 0x30, L"", BuildFileNameValue(4700, 1, L"mixed.bin", 1));
    AppendResidentAttr(base, 0x80, L"", std::vector<uint8_t>(128, 0xAB));
    FinishRecord(base);
    records[4701] = std::move(base);

    auto ext1 = BuildFileRecord(4702, 1, 0x0001, baseRef); // hard-link name here
    AppendResidentAttr(ext1, 0x30, L"", BuildFileNameValue(4600, 1, L"alias.bin", 1));
    FinishRecord(ext1);
    records[4702] = std::move(ext1);

    auto ext2 = BuildFileRecord(4703, 1, 0x0001, baseRef); // duplicate of base's own
    AppendResidentAttr(ext2, 0x30, L"", BuildFileNameValue(4700, 1, L"mixed.bin", 1));
    FinishRecord(ext2);
    records[4703] = std::move(ext2);

    std::vector<std::pair<std::wstring, uint64_t>> d0Entries;
    std::vector<std::pair<std::wstring, uint64_t>> d1Entries;
    std::vector<FileEntry> fbEntries;
    size_t fallbackDirs = 0;
    std::atomic_bool cancel{false};
    const auto walk0 = MftEnumerator::WalkDirectoryStepForTest(
        records, 4700, L"", L"", d0Entries,
        [&](FileEntry&& e) { fbEntries.push_back(std::move(e)); return true; },
        [](const ScanError&) {}, [](uint64_t, uint64_t, uint64_t, const std::wstring&) {},
        &cancel, &fallbackDirs);
    CHECK(walk0 == MftEnumerator::DirWalkOutcome::MftResolved);
    CHECK_EQ(fallbackDirs, 0u);
    CHECK_EQ(d0Entries.size(), 1u); // dedupe: the duplicate ext name adds no child
    if (d0Entries.size() == 1) {
        CHECK(d0Entries[0].first == L"mixed.bin");
        CHECK_EQ(d0Entries[0].second, 4701u); // the base, never an extension
    }

    size_t fallbackDirs2 = 0;
    std::atomic_bool cancel2{false};
    const auto walk1 = MftEnumerator::WalkDirectoryStepForTest(
        records, 4600, L"", L"", d1Entries,
        [&](FileEntry&& e) { fbEntries.push_back(std::move(e)); return true; },
        [](const ScanError&) {}, [](uint64_t, uint64_t, uint64_t, const std::wstring&) {},
        &cancel2, &fallbackDirs2);
    CHECK(walk1 == MftEnumerator::DirWalkOutcome::MftResolved);
    CHECK_EQ(fallbackDirs2, 0u);
    CHECK_EQ(d1Entries.size(), 1u); // hard link resolves exactly once
    if (d1Entries.size() == 1) {
        CHECK(d1Entries[0].first == L"alias.bin");
        CHECK_EQ(d1Entries[0].second, 4701u); // same base record: a real hard link
    }
    CHECK(fbEntries.empty()); // Win32 fallback never ran
});

TEST("mft: cancellation during Win32 fallback stops the walk, never reports a failure", [] {
    // Deterministic, no-sleep test of cancellation *during* the per-directory
    // fallback. The fallback drives Win32Enumerator on a real temp tree; the
    // consumer flips `cancel` on the first entry. Win32Enumerator must stop
    // emitting and return cleanly: the fallback is reported Ok (NOT Aborted/
    // Failed/Unreadable/DeviceLost) and no entries are emitted after the abort
    // point. (The orchestrator's Cancelled-vs-Failed verdict comes from
    // ConcurrentComparer::runOneStep observing the same flag post-walk, see
    // src/Comparison/ConcurrentComparer.cpp:376 -- covered by the cancel tests.)
    const std::wstring dir = MakeTempDir();
    const int kN = 80;
    for (int i = 0; i < kN; ++i) {
        CHECK(WriteFileBytes(dir + L"\\f" + std::to_wstring(i) + L".txt", "x", 1));
    }

    std::atomic_bool cancel{false};
    std::vector<FileEntry> seen;
    size_t emitted = 0;
    uint64_t ef = 0, ed = 0, ebytes = 0;
    const auto st = MftEnumerator::EnumerateWin32Subtree(
        dir, L"", ef, ed, ebytes,
        [&](FileEntry&& e) {
            seen.push_back(std::move(e));
            ++emitted;
            cancel.store(true, std::memory_order_release); // request cancel mid-walk
            return true;
        },
        [](const ScanError&) {}, [](uint64_t, uint64_t, uint64_t, const std::wstring&) {}, &cancel);

    // Cancel was honored at the first opportunity: Win32Enumerator stops emitting
    // on the next loop check, so exactly the one entry that requested cancellation
    // is observed -- the rest of the 80 files are never emitted.
    CHECK_EQ(emitted, 1u);
    CHECK_EQ(seen.size(), 1u);
    // No entries were emitted AFTER the abort point.
    CHECK_MSG(seen.size() == emitted, "entries must stop once cancel is observed");
    // Cancellation is not a failure: Ok, not Aborted/Failed/Unreadable/Lost.
    CHECK(st == MftEnumerator::SubtreeStatus::Ok);
});

TEST("mft: fallback Win32 returns Unreadable -> WalkDirectoryStep returns FallbackUnreadable, scan incomplete", [] {
    // Regression for a bug where FallbackUnreadable was silently converted to
    // FallbackOk: the directory was unreadable through BOTH the MFT and the
    // Win32 fallback, but the walk reported success and emitted nothing --
    // silently dropping the subtree. The fix returns FallbackUnreadable and
    // sets the per-directory incomplete flag (the production walk's analog of
    // `return !incomplete`), so the caller knows the scan missed data.
    //
    // The MFT fixture lists an out-of-range child, so the walk cannot
    // reconstruct directory 4609 and triggers the Win32 fallback. We point the
    // fallback at a NON-EXISTENT subtree path: Win32Enumerator::enumerate
    // fails GetFileAttributesW on the missing root and returns false (without
    // setting lostDevice), so EnumerateWin32Subtree returns Unreadable.
    const std::wstring dir = MakeTempDir();
    fs::create_directories(dir);
    const std::wstring missingRoot = dir + L"\\ghost"; // never created: path absent

    std::atomic_bool cancel{false};
    bool incomplete = false;
    std::vector<ScanError> errs;

    // Fixture: directory 4609 lists child 4610 (out of range -> nRecords=4610)
    // -> MFT cannot reconstruct -> Win32 fallback fires on `missingRoot`.
    {
        std::map<uint64_t, std::vector<uint8_t>> records;
        auto d = BuildFileRecord(4609, 1, 0x0003, 0); // in-use + directory
        AppendResidentAttr(d, 0x30, L"", BuildFileNameValue(4608, 1, L"dir4609", 1));
        AppendResidentAttr(d, 0x90, L"$I30",
                           BuildIndexRootValueWith({{4610ull | (1ull << 48), L"MISSING"}}));
        FinishRecord(d);
        records[4609] = std::move(d);

        std::vector<std::pair<std::wstring, uint64_t>> mftEntries;
        size_t fallbackDirs = 0;
        const auto out = MftEnumerator::WalkDirectoryStepForTest(
            records, 4609, L"", missingRoot, mftEntries,
            [&](FileEntry&&) { CHECK_MSG(false, "no entries should be emitted on failure"); return true; },
            [&](const ScanError& e) { errs.push_back(e); },
            [](uint64_t, uint64_t, uint64_t, const std::wstring&) {}, &cancel, &fallbackDirs,
            4096, 512, &incomplete);

        // The Win32 fallback was attempted (dir was unreadable on both sides).
        CHECK_EQ(fallbackDirs, 1u);
        // Bug fix: NOT FallbackOk -- the subtree is genuinely missing.
        CHECK(out == MftEnumerator::DirWalkOutcome::FallbackUnreadable);
        // No MFT-sourced children: the directory was not resolved.
        CHECK(mftEntries.empty());
        // Scan-level completeness flag must be set: the caller sees incomplete.
        CHECK(incomplete);
        // At least one scan-error was reported (the missing root).
        CHECK(!errs.empty());
    }
});

TEST("mft: cancellation propagates through WalkDirectoryStep -> caller (clean stop, not failure)", [] {
    // Integration test for cancellation DURING the per-directory Win32 fallback,
    // exercised end-to-end through WalkDirectoryStepForTest (not just
    // EnumerateWin32Subtree in isolation). Verifies:
    //  - the fallback is actually activated (needWin32Fallback);
    //  - the consumer's cancel flag is observed mid-subtree;
    //  - Win32Enumerator stops emitting (no entries after the abort point);
    //  - the walk reports a CLEAN stop (FallbackOk, NOT a failure/Unreadable/
    //    DeviceLost): cancellation is distinct from failure;
    //  - the caller does NOT transform it into a Failed/Completed outcome.
    //
    // (The whole-scan Cancelled-vs-Completed verdict is produced by
    // ConcurrentComparer::runOneStep observing the same cancel flag
    // post-walk -- see src/Comparison/ConcurrentComparer.cpp:376 -- which is
    // backend-agnostic and out of scope for this per-directory seam.)
    const std::wstring dir = MakeTempDir();
    const int kN = 80;
    for (int i = 0; i < kN; ++i) {
        CHECK(WriteFileBytes(dir + L"\\f" + std::to_wstring(i) + L".txt", "x", 1));
    }

    std::atomic_bool cancel{false};
    std::vector<FileEntry> seen;
    size_t emitted = 0;

    {
        std::map<uint64_t, std::vector<uint8_t>> records;
        auto d = BuildFileRecord(4609, 1, 0x0003, 0);
        AppendResidentAttr(d, 0x30, L"", BuildFileNameValue(4608, 1, L"dir4609", 1));
        AppendResidentAttr(d, 0x90, L"$I30",
                           BuildIndexRootValueWith({{4610ull | (1ull << 48), L"MISSING"}}));
        FinishRecord(d);
        records[4609] = std::move(d);

        std::vector<std::pair<std::wstring, uint64_t>> mftEntries;
        size_t fallbackDirs = 0;
        const auto out = MftEnumerator::WalkDirectoryStepForTest(
            records, 4609, L"", dir, mftEntries,
            [&](FileEntry&& e) {
                seen.push_back(std::move(e));
                ++emitted;
                cancel.store(true, std::memory_order_release); // request cancel mid-walk
                return true;
            },
            [](const ScanError&) {},
            [](uint64_t, uint64_t, uint64_t, const std::wstring&) {}, &cancel, &fallbackDirs);

        // The fallback fired for the directory with the unresolvable child.
        CHECK_EQ(fallbackDirs, 1u);
        // Cancellation is a CLEAN stop, not a failure: FallbackOk (not
        // FallbackUnreadable / FallbackDeviceLost / FallbackAborted).
        CHECK(out == MftEnumerator::DirWalkOutcome::FallbackOk);
        // No MFT entries (fallback was used).
        CHECK(mftEntries.empty());
        // Cancel honoured: stop on the very first entry that set the flag.
        CHECK_EQ(emitted, 1u);
        CHECK_EQ(seen.size(), 1u);
        CHECK_MSG(seen.size() == emitted,
                  "entries must stop once cancel is observed");
    }

    // --- Consumer-abort path: onEntry returns false -> Aborted propagates ---
    // The cancel flag and the consumer-abort flag are orthogonal mechanisms:
    //  - cancel flag -> Win32Enumerator stops early (Ok)
    //  - onEntry returns false -> consumerAbort -> EnumerateWin32Subtree
    //    reports Aborted -> WalkDirectoryStep reports FallbackAborted
    cancel.store(false, std::memory_order_release);
    std::vector<FileEntry> seen2;
    size_t emitted2 = 0;
    {
        std::map<uint64_t, std::vector<uint8_t>> records;
        auto d = BuildFileRecord(4609, 1, 0x0003, 0);
        AppendResidentAttr(d, 0x30, L"", BuildFileNameValue(4608, 1, L"dir4609", 1));
        AppendResidentAttr(d, 0x90, L"$I30",
                           BuildIndexRootValueWith({{4610ull | (1ull << 48), L"MISSING"}}));
        FinishRecord(d);
        records[4609] = std::move(d);

        std::vector<std::pair<std::wstring, uint64_t>> mftEntries;
        size_t fallbackDirs = 0;
        const auto out = MftEnumerator::WalkDirectoryStepForTest(
            records, 4609, L"", dir, mftEntries,
            [&](FileEntry&& e) {
                seen2.push_back(std::move(e));
                ++emitted2;
                return false; // consumer aborts
            },
            [](const ScanError&) {},
            [](uint64_t, uint64_t, uint64_t, const std::wstring&) {}, &cancel, &fallbackDirs);

        CHECK_EQ(fallbackDirs, 1u);
        // Aborted propagated, NOT swallowed as FallbackOk.
        CHECK(out == MftEnumerator::DirWalkOutcome::FallbackAborted);
        CHECK(mftEntries.empty());
        // Stop on the first emitted entry (the one that returned false).
        CHECK_EQ(emitted2, 1u);
        CHECK_EQ(seen2.size(), 1u);
    }
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

TEST("snapshot: cancelled source hashing stores no all-zero digest", [] {
    // Bug regression: a hash job that bails on cancellation used to leave its
    // slot at the default all-zero digest, which was then written into the index
    // (and later persisted to the snapshot, misread as a content mismatch). A
    // skipped job must leave NO entry instead.
    const auto dir = MakeTempDir();
    const std::wstring src = dir + L"\\src";
    fs::create_directories(src);
    const int kFiles = 64;
    for (int i = 0; i < kFiles; ++i) {
        CHECK(WriteFileBytes(src + L"\\f" + std::to_wstring(i) + L".dat", "data", 4));
    }

    FileIndex index(false);
    {
        Win32Enumerator en;
        const auto br = index.build(src, en);
        CHECK(br.ok);
    }
    CHECK_EQ(index.size(), size_t{kFiles});

    // Positive control: with no cancellation every file is hashed with a real,
    // non-zero digest.
    {
        std::atomic_bool noCancel{false};
        std::atomic<size_t> hits{0};
        ThreadPool pool(1);
        HashSourceIndex(index, src, pool, &noCancel, nullptr, hits, nullptr);
        CHECK_EQ(index.hashCount(), size_t{kFiles});
        std::array<uint8_t, 32> zero{};
        for (const auto& kv : index.entries()) {
            if (kv.second.isDirectory) continue;
            std::array<uint8_t, 32> d{};
            CHECK(index.getHash(kv.second.relativePath, d));
            CHECK_MSG(d != zero, "a real hash must never be all-zero");
        }
    }

    // Cancelled mid-batch, deterministically. A "gate" job is submitted to the
    // pool before the batch and parks the single worker (the pool queue is FIFO,
    // so the gate is run before any hash job and blocks there). The
    // onBatchSubmitted seam fires only AFTER every hash job of the batch is
    // enqueued but BEFORE they are drained, so at that moment all 64 jobs are
    // submitted and none can have started. The seam then (1) sets the cancel
    // flag and (2) opens the gate. Every queued job that subsequently executes
    // therefore OBSERVES cancellation before it begins hashing and takes the
    // early-bailout path: no sleeps, no reliance on the worker being slower
    // than the submitting thread. The skipped-mid-batch path is exercised for
    // every file, so the index must hold no digest at all.
    FileIndex interrupted(false);
    {
        Win32Enumerator en;
        const auto br = interrupted.build(src, en);
        CHECK(br.ok);
    }
    {
        std::atomic_bool cancel{false};
        std::atomic<size_t> hits{0};
        ThreadPool pool(1);

        // The gate blocks the single worker until the seam opens it. `opened`
        // makes set_value idempotent: the seam opens the gate on the happy path,
        // and the RAII guard opens it on unwind (if the seam never ran, e.g. a
        // submission failure), so the worker -- and the pool destructor that
        // joins it -- can never hang.
        std::promise<void> gate;
        std::shared_future<void> gateOpens = gate.get_future();
        bool gateOpened = false;
        auto openGate = [&] {
            if (gateOpened) return;
            gateOpened = true;
            gate.set_value();
        };
        pool.submit([gateOpens] { gateOpens.wait(); });
        struct GateGuard {
            std::function<void()> open;
            ~GateGuard() { open(); }
        } gateGuard{openGate};

        std::function<void()> afterSubmit = [&] {
            cancel.store(true); // 1) cancellation first...
            openGate();         // 2) ...then let the queued jobs run: all bail
        };
        HashSourceIndex(interrupted, src, pool, &cancel, nullptr, hits, nullptr, afterSubmit);

        // Because no job can have started before the flag was set, every job was
        // skipped: the index must contain no digest entry at all.
        CHECK_MSG(interrupted.hashCount() == 0,
                  "a cancelled batch must skip every queued job, deterministically");
        std::array<uint8_t, 32> zero{};
        for (const auto& kv : interrupted.entries()) {
            if (kv.second.isDirectory) continue;
            std::array<uint8_t, 32> d{};
            if (interrupted.getHash(kv.second.relativePath, d)) {
                CHECK_MSG(d != zero, "an all-zero digest must never be stored");
            }
        }
    }
});

TEST("snapshot: cancelled capture is never written as a complete snapshot", [] {
    // Cancelling mid-capture (after the first hash batch) must not produce a
    // snapshot at all: an incomplete capture cannot be presented as a complete
    // one. Trace: HashSourceIndex -> report.sourceOk -> WriteSnapshot. The
    // destination pass is skipped too (guarded on report.sourceOk).
    const auto dir = MakeTempDir();
    const std::wstring src = dir + L"\\src";
    fs::create_directories(src);
    testgen::CreateStressTree(src, 300); // > one 256-file hash batch

    std::atomic_bool cancel{false};
    const std::wstring snap = dir + L"\\src.bin";
    ScanOptions opts;
    opts.source = src;
    opts.destination = src;
    opts.mode = ScanMode::Content;
    opts.hashThreads = 1;
    opts.snapshotOut = snap;
    opts.cancel = &cancel;
    opts.onProgress = [&](const ScanProgress& p) {
        if (p.phase == ScanPhase::Hashing) cancel.store(true);
    };

    const ScanReport r = ScanController(false).run(opts);
    CHECK(!r.snapshotWritten);
    CHECK(!r.sourceOk);
    CHECK_MSG(!fs::exists(snap), "an interrupted capture must not leave a snapshot on disk");
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

// ---------------------------------------------------------------------------
// Unified single-handle hash flow (T1 -> lookup? -> hash -> T2 -> close).
// HashOneSide now opens the file once and runs the change-during-scan control
// (T1/T2) on the SAME handle used for hashing, instead of StatFile/Sha256File/
// StatFile with three opens. These tests pin the observable behaviour: T1
// mismatch, T1 valid + T2 unchanged, cache hit (no hash, no T2), access error,
// and the T2 comparison seam.
// ---------------------------------------------------------------------------

TEST("hashing: unified handle T1 mismatch flags changed, digest still correct", [] {
    // T1 captures size/mtime BEFORE hashing and compares against the expected
    // (enumeration-time) values; a mismatch sets `changed` inside HashOneSide,
    // but the file is still read and hashed (the verdict is decided by the
    // caller via HashPhase).
    const auto dir = MakeTempDir();
    const std::wstring file = dir + L"\\a.txt";
    CHECK(WriteFileBytes(file, "hello world", 11));
    uint64_t sz = 0, mt = 0;
    CHECK(hashing::StatFile(file, sz, mt));

    std::atomic<size_t> hits{0};
    hashing::Digest d{};
    bool changed = false;
    hashing::HashStatus st = hashing::HashStatus::ReadError;
    // mtime bumped: T1 != expected -> changed, but the file is read and hashed.
    hashing::HashOneSide(file, sz, mt + 1, changed, st, d, true, nullptr, hits);
    CHECK(st == hashing::HashStatus::Ok);
    CHECK(changed);

    // The returned digest must equal the real SHA-256 of the file (hash ran).
    hashing::Digest ref{};
    CHECK(hashing::Sha256File(file, ref) == hashing::HashStatus::Ok);
    CHECK(d == ref);
});

TEST("hashing: unified handle T1 valid + hash + T2 unchanged yields Ok, not changed", [] {
    // Matching expected metadata and no mutation during the read: T2 sees the
    // very same size/mtime, `changed` stays false and the digest is correct.
    const auto dir = MakeTempDir();
    const std::wstring file = dir + L"\\b.txt";
    CHECK(WriteFileBytes(file, "content", 7));
    uint64_t sz = 0, mt = 0;
    CHECK(hashing::StatFile(file, sz, mt));

    std::atomic<size_t> hits{0};
    hashing::Digest d{};
    bool changed = true; // must be cleared when the file is truly stable
    hashing::HashStatus st = hashing::HashStatus::ReadError;
    hashing::HashOneSide(file, sz, mt, changed, st, d, true, nullptr, hits);
    CHECK(st == hashing::HashStatus::Ok);
    CHECK(!changed);
    hashing::Digest ref{};
    CHECK(hashing::Sha256File(file, ref) == hashing::HashStatus::Ok);
    CHECK(d == ref);
});

TEST("hashing: unified handle returns NoAccess when the file cannot be opened", [] {
    const auto dir = MakeTempDir();
    const std::wstring ghost = dir + L"\\missing_during_scan.txt";

    std::atomic<size_t> hits{0};
    hashing::Digest d{};
    bool changed = false;
    hashing::HashStatus st = hashing::HashStatus::ReadError;
    // valid=true but the path does not exist: the single CreateFile fails.
    hashing::HashOneSide(ghost, 0, 0, changed, st, d, true, nullptr, hits);
    CHECK(st == hashing::HashStatus::NoAccess);
    CHECK(!changed);
    CHECK_EQ(hits.load(), 0u);

    // valid=false stays the discarded/cancelled path: ReadError, never NoAccess.
    st = hashing::HashStatus::ReadError;
    hashing::HashOneSide(ghost, 0, 0, changed, st, d, false, nullptr, hits);
    CHECK(st == hashing::HashStatus::ReadError);
});

TEST("hashing: unified handle T1/T2 comparison seam detects any metadata drift", [] {
    // The T2 change-during-scan decision is HashMetadataChanged(before, after).
    // A real mutation in the tiny window between T1 and T2 (the hash of a small
    // file) cannot be produced deterministically without sleeps, so this seam
    // pins the comparison logic directly instead of racing the hashing thread.
    using hashing::HashMetadataChanged;
    CHECK(!HashMetadataChanged(100, 1000, 100, 1000)); // untouched
    CHECK(HashMetadataChanged(100, 1000, 101, 1000));  // size changed
    CHECK(HashMetadataChanged(100, 1000, 100, 1001));  // mtime changed
    CHECK(HashMetadataChanged(100, 1000, 101, 1001));  // both changed
    CHECK(HashMetadataChanged(0, 0, 0, 1));
    CHECK(!HashMetadataChanged(0, 0, 0, 0));
});

TEST("hashing: unified handle cache hit skips hashing and T2", [] {
    const auto dir = MakeTempDir();
    const std::wstring file = dir + L"\\c.txt";
    CHECK(WriteFileBytes(file, "hello world", 11));
    uint64_t sz = 0, mt = 0;
    CHECK(hashing::StatFile(file, sz, mt));

    const std::wstring cacheFile = MakeTempDir() + L"\\hash_uni.bin";
    std::wstring err;
    hashing::HashCache cache(cacheFile, err);
    CHECK(err.empty());
    std::atomic<size_t> hits{0};

    // Profiler proves WHERE the work happens: T1/T2 and the hash job are
    // counted by the control points, without touching the scanned tree.
    bv::profiling::HashProfiler prof(/*verboseJobs=*/true);
    prof.setEnabled(true);

    // Call 1: cold cache -> T1 + hash + T2 + store (one hash job).
    {
        bv::profiling::HashSession session(&prof);
        hashing::Digest d{};
        bool changed = true;
        hashing::HashStatus st = hashing::HashStatus::ReadError;
        hashing::HashOneSide(file, sz, mt, changed, st, d, true, &cache, hits, &session,
                             bv::profiling::Side::Source);
        CHECK(st == hashing::HashStatus::Ok);
        CHECK(!changed);
        CHECK_EQ(hits.load(), 0u);
        CHECK_EQ(prof.jobRecords().size(), 1u); // one real hash job
    }

    // Call 2: unchanged file -> cache hit. MUST skip hashing AND skip T2.
    {
        bv::profiling::HashSession session(&prof);
        hashing::Digest d{};
        bool changed = true;
        hashing::HashStatus st = hashing::HashStatus::ReadError;
        hashing::HashOneSide(file, sz, mt, changed, st, d, true, &cache, hits, &session,
                             bv::profiling::Side::Source);
        CHECK(st == hashing::HashStatus::Ok);
        CHECK(!changed);
        CHECK_EQ(hits.load(), 1u);
        CHECK_MSG(d != hashing::Digest{}, "digest must come from the cache");
        CHECK_EQ(prof.jobRecords().size(), 1u); // still exactly one hash job
    }

    bv::profiling::HashProfileReport rep;
    prof.Finalize(rep);
    const auto& a = rep.side[0];
    CHECK_EQ(a.files, 1u);       // hashed exactly once (no re-read on the hit)
    CHECK_EQ(a.statT1Count, 2u); // T1 ran on BOTH calls (metadata must be fresh)
    CHECK_EQ(a.statT2Count, 1u); // T2 ran only on the miss, never on the hit
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

TEST("hash phase: cancelled hash jobs fabricate no read errors", [] {
    // Bug regression: a hash task that bails on cancellation must not report its
    // candidate as a read error (the file was never opened). Here the cancel
    // flag is ALREADY set, so every submitted job takes the early-bailout path:
    // the sink must stay completely untouched (no stats, no FileResult).
    const auto dir = MakeTempDir();
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";
    fs::create_directories(src);
    fs::create_directories(dst);
    CHECK(WriteFileBytes(src + L"\\same.txt", "hello world", 11));
    CHECK(WriteFileBytes(src + L"\\diff.txt", "aaaaaa", 6));
    fs::copy(src, dst, fs::copy_options::recursive);
    CHECK(WriteFileBytes(dst + L"\\diff.txt", "bbbbbb", 6)); // same size, different bytes

    // Build the same candidates the comparer would collect (same path + size),
    // using each side's real stat so change-detection stays quiet.
    std::vector<ContentCandidate> candidates;
    for (const auto& rel : {L"same.txt", L"diff.txt"}) {
        uint64_t sz = 0, mt = 0;
        CHECK(hashing::StatFile(src + L"\\" + rel, sz, mt));
        ContentCandidate c;
        c.relativePath = rel;
        c.sizeSource = sz;
        uint64_t dsz = 0, dmt = 0;
        CHECK(hashing::StatFile(dst + L"\\" + rel, dsz, dmt));
        c.sizeDest = dsz;
        c.srcMtime = mt;
        c.dstMtime = dmt;
        candidates.push_back(std::move(c));
    }

    std::atomic<size_t> hits{0};

    // Positive control: without cancellation the jobs classify normally.
    {
        std::atomic_bool noCancel{false};
        ConcurrentSink sink;
        ThreadPool pool(2);
        SubmitHashCandidates(candidates, pool, /*offline=*/false, nullptr, src, dst, sink,
                             &noCancel, nullptr, hits);
        pool.waitAll();
        const ResultSet ok = sink.take();
        CHECK_EQ(ok.stats.identicalFiles, 1ull);   // same.txt
        CHECK_EQ(ok.stats.contentMismatch, 1ull);  // diff.txt
        CHECK_EQ(ok.stats.readErrors, 0ull);
        CHECK_EQ(ok.stats.accessDenied, 0ull);
        CHECK_EQ(ok.problems.size(), 1ull);
    }

    // Cancelled: every job bails before touching the sink. Before the fix each
    // bailed job produced a fabricated ReadError ("errore di lettura durante il
    // calcolo dell'impronta") -- that must never happen for a never-opened file.
    {
        std::atomic_bool cancelled{true};
        ConcurrentSink sink;
        ThreadPool pool(2);
        SubmitHashCandidates(candidates, pool, /*offline=*/false, nullptr, src, dst, sink,
                             &cancelled, nullptr, hits);
        pool.waitAll();
        const ResultSet r = sink.take();
        CHECK_EQ(r.stats.identicalFiles, 0ull);
        CHECK_EQ(r.stats.contentMismatch, 0ull);
        CHECK_EQ(r.stats.readErrors, 0ull);
        CHECK_EQ(r.stats.accessDenied, 0ull);
        CHECK_EQ(r.problems.size(), 0ull);
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
        uint64_t files = 0, dirs = 0, bytes = 0;
        for (FileEntry& e : entries_) {
            if (cancel && cancel->load(std::memory_order_relaxed)) return false;
            if (e.isDirectory) {
                ++dirs;
            } else {
                ++files;
                bytes += e.size;
            }
            if (!onEntry(FileEntry(e))) return false;
            if (onProgress) onProgress(files, dirs, bytes, L"");
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

// Fails before emitting anything, but reports an error first -- the exact MFT
// shape that triggered the stale-error bug: the failed attempt's onError is
// staged and must be DROPPED when a Win32 fallback then succeeds.
class FailImmediatelyWithErrorEnumerator : public bv::IFileEnumerator {
public:
    explicit FailImmediatelyWithErrorEnumerator(bv::ScanError err)
        : err_(std::move(err)) {}
    bool enumerate(const std::wstring&, const EntryCallback&, const ErrorCallback& onError,
                   const ProgressCallback&, const std::atomic_bool*) override {
        onError(err_);
        return false;
    }

private:
    bv::ScanError err_;
};

// Emits `emitCount` entries, reports an error, then fails -- the incomplete-MFT
// shape that ALSO leaves an error behind (the attempt is the final outcome, so
// its error must stay visible in the sink).
class FailAfterNWithErrorEnumerator : public bv::IFileEnumerator {
public:
    FailAfterNWithErrorEnumerator(std::vector<FileEntry> entries, size_t emitCount,
                                  bv::ScanError err)
        : entries_(std::move(entries)), emitCount_(emitCount), err_(std::move(err)) {}
    bool enumerate(const std::wstring&, const EntryCallback& onEntry, const ErrorCallback& onError,
                   const ProgressCallback&, const std::atomic_bool*) override {
        size_t n = 0;
        for (FileEntry& e : entries_) {
            if (n >= emitCount_) break;
            ++n;
            if (!onEntry(FileEntry(e))) return false;
        }
        onError(err_);
        return false;
    }

private:
    std::vector<FileEntry> entries_;
    size_t emitCount_;
    bv::ScanError err_;
};

// Emits its entries, counting each one (so another enumerator can gate on "this
// side has produced enough"), stops early as soon as `cancel` is set, and if it
// reaches the end of the stream first, waits for cancellation before reporting
// it -- so a test can deterministically end this side as Cancelled while the
// other side is still enumerating. The per-entry cancel check makes this the
// mid-stream cancellation shape for the content-overlap tests.
class CountingCancelAwareEnumerator : public bv::IFileEnumerator {
public:
    CountingCancelAwareEnumerator(std::vector<FileEntry> entries, std::atomic<int>* emitted,
                                  const std::atomic_bool* cancel)
        : entries_(std::move(entries)), emitted_(emitted), cancel_(cancel) {}
    bool enumerate(const std::wstring&, const EntryCallback& onEntry, const ErrorCallback&,
                   const ProgressCallback&, const std::atomic_bool*) override {
        for (FileEntry& e : entries_) {
            if (cancel_ && cancel_->load(std::memory_order_acquire)) return false;
            if (emitted_) emitted_->fetch_add(1, std::memory_order_relaxed);
            if (!onEntry(FileEntry(e))) return false;
        }
        if (cancel_) {
            while (!cancel_->load(std::memory_order_acquire)) std::this_thread::yield();
        }
        return false; // cancellation was requested during the walk
    }

private:
    std::vector<FileEntry> entries_;
    std::atomic<int>* emitted_;
    const std::atomic_bool* cancel_;
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

TEST("matchtable: parked source wakes on a match (capacity freed without setSideDone)", [] {
    // highWater=1: the source stores "a" (pending=1), then its very next insert
    // parks. The destination then matches "a" (pending -> 0); that match must
    // wake the parked source WITHOUT setSideDone - the source proceeds, stores
    // "b", and only then is done. The park is observed deterministically via
    // throttleWaiters() (the source is "parked" exactly while that counter is
    // 1), never via a sleep, so this has no timing race. A lost wakeup or a
    // notify issued under the shard lock would hang the source here.
    bv::MatchTable t(2, /*highWater=*/1);
    std::atomic<bool> dstMatched{false};
    std::atomic<bool> srcStoredB{false};

    std::thread src([&] {
        FileEntry a;
        a.relativePath = L"a";
        FileEntry peer;
        t.insert(L"a", 0, std::move(a), peer); // pending 1 -> next insert parks
        FileEntry b;
        b.relativePath = L"b";
        t.insert(L"b", 0, std::move(b), peer); // parked until pending drops below 1
        srcStoredB.store(true, std::memory_order_release);
        t.setSideDone(0);
    });
    std::thread dst([&] {
        while (t.throttleWaiters() == 0) std::this_thread::yield();
        // Deterministic: the source is parked before the match happens, so the
        // wake must come from the match's notify_one, not from completion.
        FileEntry a;
        a.relativePath = L"a";
        FileEntry peer;
        dstMatched.store(t.insert(L"a", 1, std::move(a), peer) == bv::MatchTable::Outcome::Matched,
                         std::memory_order_release);
        // Intentionally delay setSideDone(1) until the source resumed: the wake
        // had to come from the match, proving capacity release wakes a waiter.
        while (!srcStoredB.load(std::memory_order_acquire)) std::this_thread::yield();
        t.setSideDone(1);
    });
    src.join();
    dst.join();

    CHECK(dstMatched.load(std::memory_order_acquire));
    CHECK(srcStoredB.load(std::memory_order_acquire));
    // "a" was consumed; "b" (source-only) remains pending until finalization.
    CHECK_EQ(t.pendingCount(), 1ull);
    const auto rem = t.remaining();
    CHECK_EQ(rem.size(), 1ull);
    if (rem.size() == 1) CHECK(rem[0].second.relativePath == L"b");
});

TEST("matchtable: cancellation wakes a parked source (destination setSideDone path)", [] {
    // The source fills the table (highWater=1, stored "a") and parks on "b".
    // Cancellation is then requested while it is parked; the destination
    // observes the flag and stops, calling setSideDone - the existing
    // cancellation path - which must wake the source (its predicate sees
    // cancel_), so it finishes instead of blocking on backpressure forever.
    bv::MatchTable t(2, /*highWater=*/1);
    std::atomic_bool cancel{false};
    t.setCancel(&cancel);
    std::atomic<bool> srcCompleted{false};

    std::thread src([&] {
        FileEntry a;
        a.relativePath = L"a";
        FileEntry peer;
        t.insert(L"a", 0, std::move(a), peer); // pending 1
        FileEntry b;
        b.relativePath = L"b";
        t.insert(L"b", 0, std::move(b), peer); // parks
        srcCompleted.store(true, std::memory_order_release);
        t.setSideDone(0);
    });
    std::thread dst([&] {
        while (t.throttleWaiters() == 0) std::this_thread::yield();
        cancel.store(true, std::memory_order_release);
        t.setSideDone(1); // destination sees cancellation and stops
    });
    src.join();
    dst.join();

    CHECK(srcCompleted.load(std::memory_order_acquire)); // no hang: source woke and finished
});

TEST("matchtable: match-and-remove decreases pending by exactly one", [] {
    // Accounting property: a cross-side match consumes the stored peer without
    // ever temporarily bumping the counter (old_count -> old_count - 1), and
    // the incoming entry is not stored.
    bv::MatchTable t(2);
    FileEntry e;
    e.relativePath = L"a";
    FileEntry peer;
    CHECK(t.insert(L"a", 0, std::move(e), peer) == bv::MatchTable::Outcome::Inserted);
    CHECK_EQ(t.pendingCount(), 1ull);
    e.relativePath = L"b";
    CHECK(t.insert(L"b", 0, std::move(e), peer) == bv::MatchTable::Outcome::Inserted);
    CHECK_EQ(t.pendingCount(), 2ull);
    e.relativePath = L"a";
    CHECK(t.insert(L"a", 1, std::move(e), peer) == bv::MatchTable::Outcome::Matched);
    CHECK(peer.relativePath == L"a");
    CHECK_EQ(t.pendingCount(), 1ull);
    CHECK_EQ(t.remaining().size(), 1ull);
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

TEST("concurrent comparer: >batch content overlap is correct, deterministic, progress-consistent", [] {
    // 600 same-size files on both sides: several 256-candidate batches are
    // submitted while both workers are still enumerating (live A + live B). The
    // overlapped run must match the serial reference exactly, and hash progress
    // must never report done > total or a regressing count, ending at done ==
    // total == every candidate.
    const auto dir = MakeTempDir();
    const size_t count = 600; // > kHashBatchSize (256): several overlapping batches
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";
    testgen::CreateStressTree(src, count);
    fs::copy(src, dst, fs::copy_options::recursive);

    ConcurrentComparer::Result serial;
    {
        bv::ThreadPool pool(0); // synchronous submit: no overlap, a reference
        ConcurrentComparer cmp(false, ScanMode::Content, false, src, dst,
                               ConcurrentComparer::SourceKind::Live, nullptr);
        serial = cmp.runWithFactories(MakeWin32Factory, MakeWin32Factory, pool);
    }

    std::vector<std::pair<uint64_t, uint64_t>> emissions;
    bool invariantOk = true;
    ConcurrentComparer::Result parallel;
    {
        bv::ThreadPool pool(4);
        ConcurrentComparer cmp(false, ScanMode::Content, false, src, dst,
                               ConcurrentComparer::SourceKind::Live, nullptr);
        auto onHashProgress = [&](uint64_t done, uint64_t total) {
            if (done > total) invariantOk = false; // never more done than discovered
            if (!emissions.empty()) {
                if (done < emissions.back().first) invariantOk = false;   // done is monotone
                if (total < emissions.back().second) invariantOk = false; // total is monotone
            }
            emissions.emplace_back(done, total);
        };
        parallel = cmp.runWithFactories(MakeWin32Factory, MakeWin32Factory, pool,
                                        {}, onHashProgress);
    }

    CHECK(parallel.sourceStatus == ConcurrentComparer::WorkerStatus::Success);
    CHECK(parallel.destinationStatus == ConcurrentComparer::WorkerStatus::Success);
    CHECK_EQ(parallel.results.stats.identicalFiles, count);
    CHECK(parallel.results.problems.empty());
    CHECK_MSG(SameResults(serial.results, parallel.results),
              "overlapped hashing must not change the logical results");
    CHECK_MSG(invariantOk, "hash progress violated done<=total or monotonicity");
    CHECK(!emissions.empty());
    // The last emission (after the final drain) reports a fully completed run.
    CHECK_EQ(emissions.back().first, count);
    CHECK_EQ(emissions.back().second, count);
});

TEST("concurrent comparer: FromIndex content overlap verifies a large tree against the live destination",
     [] {
    // Offline source (FromIndex): digests come from the index, the destination
    // is read live. 600 candidates again exceed the batch size, so batches are
    // submitted while the destination is still enumerating. Only the
    // destination is ever touched for hashing.
    const auto dir = MakeTempDir();
    const size_t count = 600;
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";
    testgen::CreateStressTree(src, count);
    fs::copy(src, dst, fs::copy_options::recursive);

    FileIndex idx(false);
    {
        Win32Enumerator en;
        const auto br = idx.build(src, en);
        CHECK(br.ok);
    }
    for (const auto& [key, e] : idx.entries()) {
        if (e.isDirectory) continue;
        std::array<uint8_t, 32> digest;
        CHECK(hashing::Sha256File(src + L"\\" + e.relativePath, digest) == hashing::HashStatus::Ok);
        idx.setHash(e.relativePath, digest);
    }
    CHECK_EQ(idx.hashCount(), count);

    bv::ThreadPool pool(4);
    ConcurrentComparer cmp(false, ScanMode::Content, false, src, dst,
                           ConcurrentComparer::SourceKind::FromIndex, &idx);
    const auto r = cmp.runWithFactories(MakeWin32Factory, MakeWin32Factory, pool);

    CHECK(r.sourceStatus == ConcurrentComparer::WorkerStatus::Success);
    CHECK(r.destinationStatus == ConcurrentComparer::WorkerStatus::Success);
    CHECK_EQ(r.results.stats.identicalFiles, count);
    CHECK_EQ(r.results.stats.contentMismatch, 0ull);
    CHECK(r.results.problems.empty());
});

TEST("concurrent comparer: cancel mid-content-hash drains submitted jobs, fabricates no verdicts", [] {
    // Cancellation is requested while content candidates are still being hashed
    // on a live pool. Both workers must stop promptly, the run must return
    // (submitted hash jobs drain -- no deadlock in the bounded in-flight wait),
    // and no missing/extra may be fabricated from a partial walk.
    const auto dir = MakeTempDir();
    const size_t count = 600;
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";
    testgen::CreateStressTree(src, count);
    fs::copy(src, dst, fs::copy_options::recursive);
    const auto srcEntries = CaptureEntries(src);
    const auto dstEntries = CaptureEntries(dst);

    std::atomic_bool cancel{false};
    std::atomic<int> destEmitted{0};
    bv::ThreadPool pool(4);
    ConcurrentComparer cmp(false, ScanMode::Content, false, src, dst,
                           ConcurrentComparer::SourceKind::Live, nullptr, &cancel);
    // The destination emits entries (counting them) and stops as soon as cancel
    // is set. The source emits everything and only then requests cancellation,
    // parked behind a FIFO gate job in the hash pool: with 600 files and at most
    // 100 directories, the first 400 emitted entries always include >= 256 file
    // candidates, so one full hash batch is guaranteed to already be enqueued
    // before the gate is submitted. The pool runs tasks in submission order, so
    // the gate fires only once every earlier hash job -- the whole first batch --
    // has completed and committed its verdict: cancellation is never signalled
    // before at least one batch is classified, and any batch submitted after the
    // gate observes the flag and takes the early-bailout path.
    auto srcFac = [&] {
        return std::unique_ptr<IFileEnumerator>(new ReplayEnumerator(
            srcEntries, {},
            [&] {
                while (destEmitted.load(std::memory_order_acquire) < 400)
                    std::this_thread::yield();
                std::promise<void> gateDone;
                std::future<void> done = gateDone.get_future();
                pool.submit([&] {
                    cancel.store(true, std::memory_order_release);
                    gateDone.set_value();
                });
                done.wait();
            }));
    };
    auto dstFac = [&] {
        return std::unique_ptr<IFileEnumerator>(
            new CountingCancelAwareEnumerator(dstEntries, &destEmitted, &cancel));
    };
    const auto r = cmp.runWithFactories(std::move(srcFac), std::move(dstFac), pool);

    // Cancellation was requested during the walk (the source sets it as its
    // final act), so neither side is a clean Success: both report Cancelled.
    CHECK(r.sourceStatus == ConcurrentComparer::WorkerStatus::Cancelled);
    CHECK(r.destinationStatus == ConcurrentComparer::WorkerStatus::Cancelled);
    CHECK_EQ(r.results.stats.missingFiles + r.results.stats.missingDirs +
                 r.results.stats.extraFiles + r.results.stats.extraDirs, 0ull);
    CHECK_MSG(r.results.stats.identicalFiles > 0,
              "content pairs submitted before cancellation must still be classified");
});

TEST("concurrent comparer: destination failure with submitted hashes drains without hanging", [] {
    // The destination emits enough entries to submit content-hash work and then
    // reports an incomplete scan. The already-submitted hash jobs must drain,
    // the run must return, and the failed side's partial walk must not fabricate
    // missing/extra verdicts.
    const auto dir = MakeTempDir();
    const size_t count = 600;
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";
    testgen::CreateStressTree(src, count);
    fs::copy(src, dst, fs::copy_options::recursive);
    const auto dstEntries = CaptureEntries(dst);

    bv::ThreadPool pool(4);
    ConcurrentComparer cmp(false, ScanMode::Content, false, src, dst,
                           ConcurrentComparer::SourceKind::Live, nullptr);
    auto dstFac = [&] {
        return std::unique_ptr<IFileEnumerator>(
            new FailAfterNEnumerator(dstEntries, dstEntries.size()));
    };
    const auto r = cmp.runWithFactories(MakeWin32Factory, std::move(dstFac), pool);

    CHECK(r.sourceStatus == ConcurrentComparer::WorkerStatus::Success);
    CHECK(r.destinationStatus == ConcurrentComparer::WorkerStatus::Failed);
    CHECK_EQ(r.results.stats.missingFiles + r.results.stats.missingDirs +
                 r.results.stats.extraFiles + r.results.stats.extraDirs, 0ull);
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

TEST("concurrent comparer: discarded failed attempt leaves no stale error in the sink", [] {
    // Regression test for the MFT -> Win32 fallback: an MFT-like enumerator that
    // reports an error and then fails BEFORE emitting any entry is completely
    // discarded in favour of the Win32 back-end that follows it. Its error must
    // NOT survive as a permanent result after the Win32 fallback succeeds --
    // otherwise a healthy filesystem would look like it failed.
    const auto dir = MakeTempDir();
    testgen::CreateDifferingTrees(dir);
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";

    bv::ScanError mftErr;
    mftErr.path = L"";
    mftErr.message = L"MFT scan incomplete: directory $INDEX_ALLOCATION unreadable (record 5)";
    mftErr.winError = ERROR_INVALID_DATA;

    std::vector<ConcurrentComparer::EnumeratorFactory> srcFacs;
    srcFacs.push_back([&] {
        return std::unique_ptr<IFileEnumerator>(new FailImmediatelyWithErrorEnumerator(mftErr));
    });
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
    CHECK_MSG(s.readErrors == 0 && s.accessDenied == 0,
              "the discarded MFT attempt's error must not survive a Win32 fallback");
    for (const FileResult& p : r.results.problems) {
        CHECK_MSG(p.status != Status::ReadError && p.status != Status::AccessDenied,
                  "no stale error from the discarded attempt may reach the results");
    }
});

TEST("concurrent comparer: terminal failure keeps its error in the sink", [] {
    // The counterpart: when a FailedEmpty attempt has NO fallback left, the
    // side genuinely failed and its error is the only explanation -- it MUST be
    // kept in the sink.
    const auto dir = MakeTempDir();
    testgen::CreateDifferingTrees(dir);
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";

    bv::ScanError err;
    err.path = L"";
    err.message = L"root path is not accessible";
    err.winError = ERROR_PATH_NOT_FOUND;

    bv::ThreadPool pool(0);
    ConcurrentComparer cmp(false, ScanMode::Presence, false, src, dst,
                           ConcurrentComparer::SourceKind::Live, nullptr);
    auto srcFac = [&] {
        return std::unique_ptr<IFileEnumerator>(new FailImmediatelyWithErrorEnumerator(err));
    };
    const auto r = cmp.runWithFactories(std::move(srcFac), MakeWin32Factory, pool);

    CHECK(r.sourceStatus == ConcurrentComparer::WorkerStatus::Failed);
    CHECK(r.destinationStatus == ConcurrentComparer::WorkerStatus::Success);
    CHECK_EQ(r.results.stats.readErrors, 1ull);
    CHECK_EQ(r.results.stats.accessDenied, 0ull);
    bool found = false;
    for (const FileResult& p : r.results.problems) {
        if (p.status == Status::ReadError) {
            found = true;
            CHECK(p.relativePath == L"");
        }
    }
    CHECK_MSG(found, "the terminal failure's error must be visible in the results");
});

TEST("concurrent comparer: emitted-then-failed attempt keeps its error in the sink", [] {
    // FailedWithEntries: the attempt emitted entries (too late to fall back), so
    // it is the final outcome and its error describes a real, incomplete scan.
    // The error must stay visible, and no missing/extra may be fabricated.
    const auto dir = MakeTempDir();
    testgen::CreateDifferingTrees(dir);
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";
    const auto srcEntries = CaptureEntries(src);

    bv::ScanError err;
    err.path = L"sub";
    err.message = L"MFT scan incomplete: index child record missing or sequence mismatch";
    err.winError = ERROR_INVALID_DATA;

    bv::ThreadPool pool(0);
    ConcurrentComparer cmp(false, ScanMode::Presence, false, src, dst,
                           ConcurrentComparer::SourceKind::Live, nullptr);
    auto srcFac = [&] {
        return std::unique_ptr<IFileEnumerator>(
            new FailAfterNWithErrorEnumerator(srcEntries, srcEntries.size(), err));
    };
    const auto r = cmp.runWithFactories(std::move(srcFac), MakeWin32Factory, pool);

    CHECK(r.sourceStatus == ConcurrentComparer::WorkerStatus::Failed);
    CHECK(r.destinationStatus == ConcurrentComparer::WorkerStatus::Success);
    CHECK_EQ(r.results.stats.identicalFiles, 4ull);
    CHECK_EQ(r.results.stats.missingFiles + r.results.stats.missingDirs +
                 r.results.stats.extraFiles + r.results.stats.extraDirs, 0ull);
    CHECK_EQ(r.results.stats.readErrors, 1ull);
    bool found = false;
    for (const FileResult& p : r.results.problems) {
        if (p.status == Status::ReadError) {
            found = true;
            CHECK(p.relativePath == L"sub");
        }
    }
    CHECK_MSG(found, "the incomplete scan's error must be visible in the results");
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
// ScanOrchestrator: the previous worker must be reaped outside the lock.
// ---------------------------------------------------------------------------

// Bounded poll for a completed scan (running_ false and resultsReady_ true).
bool WaitForRunDone(bv::ScanOrchestrator& orch, int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const bv::ScanOrchestrator::UiSnapshot st = orch.snapshot();
        if (!st.running && st.resultsReady) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// Deterministically exercises the deadlock window that existed while
// startLiveScan()/startSnapshotScan() joined the previous worker with mtx_
// held. The first worker is parked between its final state update (running_ ==
// false, mtx_ released) and notify() -- it is still joinable but still needs
// mtx_ to exit. A second start then reaps it; with the old code that second
// start blocked inside worker_.join() while holding mtx_, so the parked worker
// could never run notify() -> circular wait. The gate is released from inside
// the second start's own lock scope (setStartLockedHook), which orders the
// wake-up so the worker always needs mtx_ exactly while the second start holds
// it -- no timing involved.
void RunReapUnderLockRegressionTest(bool snapshot) {
    using namespace std::chrono_literals;
    const std::wstring src = MakeTempDir();
    const std::wstring dst = MakeTempDir();
    CHECK(WriteFileBytes(src + L"\\a.txt", "aaa", 3));
    CHECK(WriteFileBytes(dst + L"\\b.txt", "bbb", 3));
    const std::wstring snapFile = MakeTempDir() + L"\\idx.bin";

    // Heap-allocated so a regression can leak it instead of hanging the suite:
    // its destructor joins the parked worker, which a deadlock leaves stuck.
    bv::ScanOrchestrator* orch = new bv::ScanOrchestrator();
    orch->setSource(src);
    orch->setDest(dst);

    std::atomic<bool> workerParked{false};
    auto releaseWorker = std::make_shared<std::promise<void>>();
    const std::shared_future<void> releaseFut = releaseWorker->get_future();

    // Park the worker after running_ is observable false but before notify().
    // A later run ending fires the hook again; that is harmless because the
    // atomic is idempotent and wait() on a satisfied shared_future returns.
    orch->setBeforeNotifyHook([&workerParked, releaseFut] {
        workerParked.store(true, std::memory_order_release);
        releaseFut.wait();
    });

    const bool firstOk =
        snapshot ? orch->startSnapshotScan(snapFile) : orch->startLiveScan();
    if (!firstOk) {
        delete orch;
        CHECK_MSG(false, "first scan did not start");
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!workerParked.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    if (!workerParked.load(std::memory_order_acquire)) {
        delete orch;
        CHECK_MSG(false, "first scan never reached the pre-notify window");
        return;
    }

    // Release the parked worker exactly while the second start holds mtx_, the
    // instant the old code was blocked inside worker_.join() under the lock.
    orch->setStartLockedHook([releaseWorker] { releaseWorker->set_value(); });

    auto secondResult = std::make_shared<std::promise<bool>>();
    std::future<bool> secondFut = secondResult->get_future();
    std::thread t2([orch, snapshot, secondResult, snapFile] {
        const bool ok =
            snapshot ? orch->startSnapshotScan(snapFile) : orch->startLiveScan();
        secondResult->set_value(ok);
    });

    if (secondFut.wait_for(5s) != std::future_status::ready) {
        // Deadlocked: the worker can no longer reach notify() because the
        // second start holds mtx_ while joining it. Detach the blocked start
        // and leak the orchestrator so the harness finishes instead of hanging
        // forever in a destructor join.
        t2.detach();
        (void)orch; // intentional leak; only reachable on the pre-fix code
        CHECK_MSG(false,
                  "second start blocked: previous worker could not exit while it "
                  "was joined under mtx_");
        return;
    }
    try {
        releaseWorker->set_value();
    } catch (const std::future_error&) {
    }
    CHECK(secondFut.get());
    t2.join();
    delete orch;
}

TEST("orchestrator: startLiveScan while previous worker is winding down does not deadlock", [] {
    RunReapUnderLockRegressionTest(false);
});

TEST("orchestrator: startSnapshotScan while previous worker is winding down does not deadlock", [] {
    RunReapUnderLockRegressionTest(true);
});

TEST("orchestrator: start, finish, then start again reaps the previous worker (no terminate)", [] {
    const std::wstring src = MakeTempDir();
    const std::wstring dst = MakeTempDir();
    CHECK(WriteFileBytes(src + L"\\a.txt", "aaa", 3));
    CHECK(WriteFileBytes(dst + L"\\b.txt", "bbb", 3));
    const std::wstring snapFile = MakeTempDir() + L"\\idx.bin";

    bv::ScanOrchestrator orch;
    orch.setSource(src);
    orch.setDest(dst);

    CHECK(orch.startLiveScan());
    CHECK_MSG(WaitForRunDone(orch, 5000), "first live scan did not finish");

    CHECK(orch.startLiveScan());
    CHECK_MSG(WaitForRunDone(orch, 5000), "second live scan did not finish");

    CHECK(orch.startSnapshotScan(snapFile));
    CHECK_MSG(WaitForRunDone(orch, 5000), "snapshot scan did not finish");

    CHECK(orch.startLiveScan());
    CHECK_MSG(WaitForRunDone(orch, 5000), "live scan after snapshot did not finish");

    // Reaching the destructor with every worker reaped is the point of the
    // test: assigning a new std::thread over an un-joined worker_ would have
    // called std::terminate() during one of the starts above.
});

TEST("orchestrator: successful live scan exposes both sides as ok", [] {
    const std::wstring src = MakeTempDir();
    const std::wstring dst = MakeTempDir();
    CHECK(WriteFileBytes(src + L"\\a.txt", "aaa", 3));
    CHECK(WriteFileBytes(dst + L"\\b.txt", "bbb", 3));

    bv::ScanOrchestrator orch;
    orch.setSource(src);
    orch.setDest(dst);
    CHECK(orch.startLiveScan());
    CHECK_MSG(WaitForRunDone(orch, 5000), "live scan did not finish");

    const auto st = orch.snapshot();
    CHECK(st.resultsReady);
    CHECK(!st.cancelled);
    CHECK(st.sourceOk);
    CHECK(st.destinationOk);
});

TEST("orchestrator: failed source is exposed as incomplete, not successful", [] {
    const std::wstring dir = MakeTempDir();
    const std::wstring dst = MakeTempDir();
    CHECK(WriteFileBytes(dst + L"\\b.txt", "bbb", 3));
    const std::wstring missing = dir + L"\\does_not_exist_source";

    bv::ScanOrchestrator orch;
    orch.setSource(missing);
    orch.setDest(dst);
    CHECK(orch.startLiveScan());
    CHECK_MSG(WaitForRunDone(orch, 5000), "failed-source scan did not finish");

    const auto st = orch.snapshot();
    CHECK(st.resultsReady);
    CHECK(!st.cancelled);
    CHECK(!st.sourceOk);
    CHECK(st.destinationOk);
});

TEST("orchestrator: failed destination is exposed as incomplete, not successful", [] {
    const std::wstring dir = MakeTempDir();
    const std::wstring src = MakeTempDir();
    CHECK(WriteFileBytes(src + L"\\a.txt", "aaa", 3));
    const std::wstring missing = dir + L"\\does_not_exist_dest";

    bv::ScanOrchestrator orch;
    orch.setSource(src);
    orch.setDest(missing);
    CHECK(orch.startLiveScan());
    CHECK_MSG(WaitForRunDone(orch, 5000), "failed-destination scan did not finish");

    const auto st = orch.snapshot();
    CHECK(st.resultsReady);
    CHECK(!st.cancelled);
    CHECK(st.sourceOk);
    CHECK(!st.destinationOk);
});

TEST("orchestrator: both sides failed is exposed as incomplete, not successful", [] {
    const std::wstring dir = MakeTempDir();
    const std::wstring missingA = dir + L"\\does_not_exist_src";
    const std::wstring missingB = dir + L"\\does_not_exist_dst";

    bv::ScanOrchestrator orch;
    orch.setSource(missingA);
    orch.setDest(missingB);
    CHECK(orch.startLiveScan());
    CHECK_MSG(WaitForRunDone(orch, 5000), "both-failed scan did not finish");

    const auto st = orch.snapshot();
    CHECK(st.resultsReady);
    CHECK(!st.cancelled);
    CHECK(!st.sourceOk);
    CHECK(!st.destinationOk);
});

TEST("orchestrator: cancelled scan stays distinct from failure", [] {
    const std::wstring src = MakeTempDir();
    const std::wstring dst = MakeTempDir();
    CHECK(WriteFileBytes(src + L"\\a.txt", "aaa", 3));
    CHECK(WriteFileBytes(dst + L"\\b.txt", "bbb", 3));

    bv::ScanOrchestrator orch;
    orch.setSource(src);
    orch.setDest(dst);

    // Park the worker in the existing test seam (between its final state update
    // and notify()), then cancel. The snapshot must then report cancelled
    // (checked by the UI before the failure/success branches) regardless of the
    // per-side ok flags. Cancellation itself is exercised deterministically;
    // the comparer-level cancelled/partial-result behaviour is covered by the
    // comparer tests (e.g. "cancel mid-content-hash...").
    std::atomic<bool> workerParked{false};
    std::promise<void> releaseWorker;
    std::future<void> releaseFut = releaseWorker.get_future();
    orch.setBeforeNotifyHook([&] {
        workerParked.store(true, std::memory_order_release);
        releaseFut.wait();
    });

    CHECK(orch.startLiveScan());
    while (!workerParked.load(std::memory_order_acquire)) std::this_thread::yield();
    orch.stop();
    releaseWorker.set_value();
    CHECK_MSG(WaitForRunDone(orch, 5000), "cancelled scan did not finish");

    const auto st = orch.snapshot();
    CHECK(st.resultsReady);
    CHECK_MSG(st.cancelled, "a run stopped by the user must be reported as cancelled");
});

// ---------------------------------------------------------------------------
// Phase 5: ADS size filtering tests

// Helper: append a resident $DATA attribute to a record.
// `name` is the attribute name (empty = unnamed $DATA, e.g. L"Zone.Identifier" for ADS).
// The $DATA value layout (resident): offset +16 from attribute start contains the
// value length (valueLen), and ParseRecord reads this as out.dataSize.
// We set up the attribute so that at offset +16, valueLen = dataSize.
static void AppendResidentDataAttr(std::vector<uint8_t>& rec, uint32_t type,
                                    const std::wstring& name, uint32_t dataSize) {
    const uint32_t nameBytes = static_cast<uint32_t>(name.size() * 2);
    // valueLength field at offset +16 from attribute start
    const uint32_t valueLen = dataSize;
    const uint32_t attrLen = 24 + nameBytes + valueLen;
    const size_t start = rec.size();
    rec.resize(start + attrLen);
    *reinterpret_cast<uint32_t*>(rec.data() + start) = type;               // attribute type
    *reinterpret_cast<uint32_t*>(rec.data() + start + 4) = attrLen;       // attribute length
    rec[start + 9] = static_cast<uint8_t>(name.size());                   // name length (0 = unnamed)
    *reinterpret_cast<uint16_t*>(rec.data() + start + 10) = 24;           // name offset
    *reinterpret_cast<uint32_t*>(rec.data() + start + 16) = valueLen;      // valueLength -> ParseRecord reads this as dataSize
    *reinterpret_cast<uint16_t*>(rec.data() + start + 20) = 24 + nameBytes; // value offset
    if (nameBytes) std::memcpy(rec.data() + start + 24, name.c_str(), nameBytes);
    // Value content starts at offset 24; fill with placeholder so buffer is valid
    std::memset(rec.data() + start + 24 + nameBytes, 0xAA, valueLen);
}

// Helper: build a file MFT record with a resident $DATA attribute (unnamed or ADS).
// `fileName` is the $FILE_NAME name; `adsNames` are optional named ADS (e.g.
// L"Zone.Identifier") appended AFTER the unnamed $DATA. The unnamed $DATA's
// resident size is `unnamedSize` -- kept small because a resident value must fit
// inside the 1024-byte record (a real resident $DATA caps out at ~970 bytes).
static auto BuildFileRecordWithData(
    uint64_t recNo, uint16_t seq, uint16_t flags, uint64_t baseRef,
    const wchar_t* fileName, const std::vector<std::wstring>& adsNames,
    uint32_t unnamedSize) {
    auto r = BuildFileRecord(recNo, seq, flags, baseRef);
    AppendResidentDataAttr(r, 0x80 /* kAttrData */, {}, unnamedSize); // unnamed $DATA
    for (const auto& ads : adsNames) {
        AppendResidentDataAttr(r, 0x80 /* kAttrData */, ads, 26); // named ADS
    }
    AppendResidentAttr(r, 0x30 /* kAttrFileName */, L"", BuildFileNameValue(4609, 1, fileName, 1));
    FinishRecord(r);
    return r;
}

// Test A: Unnamed $DATA solamente => dataSize == 48
TEST("mft: ParseRecord unnamed $DATA only", [] {
    auto rec = BuildFileRecordWithData(1000, 1, 0x0001, 0, L"test.txt", {}, 48);
    auto result = MftEnumerator::ParseRecordForTest(rec);
    CHECK_EQ(result.dataSize, 48u);
});

// Test B: Unnamed $DATA + named ADS => dataSize == 48 (ADS must NOT overwrite)
TEST("mft: ParseRecord unnamed $DATA + named ADS", [] {
    auto rec = BuildFileRecordWithData(1001, 1, 0x0001, 0, L"test.txt", {L"Zone.Identifier"}, 48);
    auto result = MftEnumerator::ParseRecordForTest(rec);
    // The unnamed $DATA (48) must win over the named ADS (26).
    CHECK_EQ(result.dataSize, 48u);
});

// Test C: Unnamed $DATA + più ADS => dataSize == 48
TEST("mft: ParseRecord unnamed $DATA + multiple ADS", [] {
    auto rec = BuildFileRecordWithData(1002, 1, 0x0001, 0, L"test.txt",
                                       {L"Zone.Identifier", L"Zone.Identifier.2"}, 48);
    auto result = MftEnumerator::ParseRecordForTest(rec);
    // The named ADS must not overwrite dataSize.
    CHECK_EQ(result.dataSize, 48u);
});

// Test D: Ordine degli attributi - named $DATA dopo unnamed $DATA
// Questo test verifica che, anche quando $DATA:Zone.Identifier viene dopo
// l'unnamed $DATA, dataSize rimane 48 e non diventa 26.
TEST("mft: ParseRecord attribute order named after unnamed", [] {
    // Costruiamo il record con unnamed $DATA prima, poi Zone.Identifier dopo.
    // L'attributo $DATA con nome vuoto viene processato per primo (dataSize=48),
    // poi Zone.Identifier viene processato ma viene saltato grazie ad AttrNameOf check.
    auto rec = BuildFileRecordWithData(1003, 1, 0x0001, 0, L"test.txt", {L"Zone.Identifier"}, 48);
    auto result = MftEnumerator::ParseRecordForTest(rec);
    // Il named $DATA non deve sovrascrivere dataSize.
    CHECK_EQ(result.dataSize, 48u);
});

// Test E1: Resident unnamed $DATA + resident named ADS
TEST("mft: ParseRecord resident unnamed + resident named ADS", [] {
    auto rec = BuildFileRecordWithData(1004, 1, 0x0001, 0, L"test.txt", {L"Zone.Identifier"}, 48);
    auto result = MftEnumerator::ParseRecordForTest(rec);
    // Solo l'unnamed $DATA contribuisce alla size.
    CHECK_EQ(result.dataSize, 48u);
});

// Test E2: Non-resident named ADS (no unnamed $DATA) => dataSize == 0
// Verifica che il controllo AttrNameOf funzioni anche quando l'unico $DATA è named.
TEST("mft: ParseRecord non-resident named ADS only dataSize 0", [] {
    // Costruiamo un record con un solo $DATA named (nessun unnamed).
    // L'attributo $DATA è non-resident con name="Zone.Identifier".
    // Il parser deve ignorarlo e lasciare dataSize=0 (valore iniziale).
    auto r = BuildFileRecord(1005, 1, 0x0001, 0);
    AppendResidentAttr(r, 0x30 /* kAttrFileName */, L"", BuildFileNameValue(4609, 1, L"test.txt", 1));
    AppendNonResidentAttr(r, 0x80 /* kAttrData */, L"Zone.Identifier", 0, 0, 1, 0);
    FinishRecord(r);
    auto result = MftEnumerator::ParseRecordForTest(r);
    // dataSize deve rimanere 0 (nessun unnamed $DATA), il named ADS deve essere ignorato.
    CHECK_EQ(result.dataSize, 0u);
});

// ---------------------------------------------------------------------------
// Phase 4b: MFT lastWriteTime source ($STANDARD_INFORMATION preferred)

// The MFT mtime fix: FileEntry.lastWriteTime must come from
// $STANDARD_INFORMATION (+0x08 modified time) -- the timestamp
// GetFileInformationByHandle() reports during content hashing -- NOT from
// $FILE_NAME (+0x10). The two diverge systematically on real volumes, and
// comparing the $FILE_NAME value against the hashing-time stat produced a
// volume-wide false MODIFICATO_DURANTE_SCAN.
TEST("mft: lastWriteTime prefers $STANDARD_INFORMATION over $FILE_NAME", [] {
    const uint64_t fnMtime = 0x1122334455667788ull;   // $FILE_NAME mtime
    const uint64_t siModified = 0x8877665544332211ull; // SI modified (distinct)
    auto r = BuildFileRecord(1100, 1, 0x0001, 0); // in-use + file
    AppendResidentAttr(r, 0x10 /* kAttrStdInfo */, L"",
                       BuildStandardInfoValue(0x1111, siModified, 0x2222, 0x3333));
    AppendResidentAttr(r, 0x30 /* kAttrFileName */, L"",
                       BuildFileNameValueWithMtime(4609, 1, L"t.txt", 1, fnMtime));
    FinishRecord(r);
    const auto result = MftEnumerator::ParseRecordForTest(r);
    CHECK(result.parsed && result.inUse && !result.isDir);
    CHECK_EQ(result.mtime, fnMtime);
    CHECK_EQ(result.standardMtime, siModified);
    CHECK(result.standardMtime != result.mtime);
    // FileEntry.lastWriteTime must pick the SI timestamp.
    CHECK_EQ(result.lastWriteTime, siModified);
});

// Fallback: no $STANDARD_INFORMATION -> lastWriteTime falls back to the
// $FILE_NAME timestamp (the pre-fix behaviour).
TEST("mft: lastWriteTime falls back to $FILE_NAME without $STANDARD_INFORMATION", [] {
    const uint64_t fnMtime = 0x1122334455667788ull;
    auto r = BuildFileRecord(1101, 1, 0x0001, 0); // in-use + file, no SI
    AppendResidentAttr(r, 0x30 /* kAttrFileName */, L"",
                       BuildFileNameValueWithMtime(4609, 1, L"t.txt", 1, fnMtime));
    FinishRecord(r);
    const auto result = MftEnumerator::ParseRecordForTest(r);
    CHECK(result.parsed && result.inUse && !result.isDir);
    CHECK_EQ(result.mtime, fnMtime);
    CHECK_EQ(result.standardMtime, 0ull); // no SI attribute
    CHECK_EQ(result.lastWriteTime, fnMtime); // fallback to $FILE_NAME
});

// ---------------------------------------------------------------------------
// Phase 5: export CSV/JSON, binary snapshot, hash cache, offline compare

int main() {
    const int rc = test::Summary();
    CleanupTempDirs();
    return rc;
}

