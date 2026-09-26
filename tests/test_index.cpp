// FileIndex tests (split out of the former monolithic test_main.cpp).

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
