// PathUtil unit tests (split out of the former monolithic test_main.cpp).

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
