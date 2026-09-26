// Content-hash profiling and unified single-handle hash flow tests
// (split out of the former monolithic test_main.cpp).

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
