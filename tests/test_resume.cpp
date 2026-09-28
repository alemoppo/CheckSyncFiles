// Phase 1 resume tests: ResumePlan partitioning (in-memory indexes) and
// end-to-end save/resume through ScanController (on-disk trees).
//
// Soundness invariant checked throughout: a resumed run produces results
// IDENTICAL to a fresh run over the same trees (same stats, same problems).

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Comparison/ScanMode.h"
#include "Filesystem/FileIndex.h"
#include "Filesystem/FileIndexSerializer.h"
#include "ScanController.h"
#include "Session/ResumePlan.h"
#include "Session/SessionLogic.h"
#include "Session/SessionStore.h"
#include "TestHarness.h"
#include "TestTree.h"

namespace fs = std::filesystem;
using namespace bv;
using namespace bv::session;

namespace {

struct TempDir {
    std::wstring path;
    TempDir() {
        static int counter = 0;
        const fs::path dir = fs::temp_directory_path() /
                             (L"bvresume_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                              std::to_wstring(counter++));
        fs::create_directories(dir);
        path = dir.wstring();
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(fs::path(path), ec);
    }
};

FileEntry MkEntry(const wchar_t* rel, uint64_t size, uint64_t mtime, bool isDir = false) {
    FileEntry e;
    e.relativePath = rel;
    e.size = size;
    e.lastWriteTime = mtime;
    e.isDirectory = isDir;
    return e;
}

JournalEntry MkJournal(const wchar_t* rel, Status verdict, uint64_t sizeA, uint64_t mtimeA,
                       uint64_t sizeB, uint64_t mtimeB, bool isDir = false) {
    JournalEntry e;
    e.relativePath = rel;
    e.sizeA = sizeA;
    e.mtimeA = mtimeA;
    e.sizeB = sizeB;
    e.mtimeB = mtimeB;
    e.verdict = verdict;
    e.isDirectory = isDir;
    return e;
}

ScanSession MkSession(std::vector<JournalEntry> journal) {
    ScanSession s;
    s.sessionId = GenerateSessionId();
    s.createdAtUnix = NowUnixSeconds();
    s.sourceA = L"A";
    s.sourceB = L"B";
    s.journal = std::move(journal);
    return s;
}

bool StatsEqual(const Stats& a, const Stats& b) {
    return a.sourceFiles == b.sourceFiles && a.sourceDirs == b.sourceDirs &&
           a.destFiles == b.destFiles && a.destDirs == b.destDirs &&
           a.identicalFiles == b.identicalFiles && a.identicalDirs == b.identicalDirs &&
           a.missingFiles == b.missingFiles && a.missingDirs == b.missingDirs &&
           a.extraFiles == b.extraFiles && a.extraDirs == b.extraDirs &&
           a.sizeMismatch == b.sizeMismatch && a.contentMismatch == b.contentMismatch &&
           a.identicalPartialFiles == b.identicalPartialFiles &&
           a.contentMismatchPartial == b.contentMismatchPartial &&
           a.readErrors == b.readErrors && a.accessDenied == b.accessDenied &&
           a.changedDuringScan == b.changedDuringScan && a.bytesSource == b.bytesSource &&
           a.bytesDest == b.bytesDest;
}

bool ProblemsEqual(const std::vector<FileResult>& a, const std::vector<FileResult>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].status != b[i].status || a[i].relativePath != b[i].relativePath ||
            a[i].isDirectory != b[i].isDirectory || a[i].sizeSource != b[i].sizeSource ||
            a[i].sizeDest != b[i].sizeDest)
            return false;
    }
    return true;
}

ScanReport RunScan(const std::wstring& src, const std::wstring& dst, ScanMode mode,
                   const std::wstring& sessionOut = L"",
                   const std::wstring& resumeFrom = L"", int verifyPercent = 100,
                   PartialPattern pattern = PartialPattern::Edges, uint64_t checkpointRows = 0,
                   uint64_t checkpointSecs = 0, unsigned threads = 0,
                   const std::wstring& compareFrom = L"", const std::wstring& snapshotOut = L"") {
    ScanOptions opts;
    opts.source = src;
    opts.destination = dst;
    opts.mode = mode;
    opts.sessionOut = sessionOut;
    opts.resumeFrom = resumeFrom;
    opts.verifyLevel.percent = verifyPercent;
    opts.verifyLevel.pattern = pattern;
    opts.checkpointRows = checkpointRows;
    opts.checkpointSecs = checkpointSecs;
    opts.hashThreads = threads;
    opts.compareFrom = compareFrom;
    opts.snapshotOut = snapshotOut;
    return ScanController(false).run(opts);
}

bool WriteText(const std::wstring& path, const std::string& data) {
    std::ofstream f(fs::path(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.flush();
    return static_cast<bool>(f);
}

bool WriteBytes(const std::wstring& path, const char* data, size_t n) {
    std::ofstream f(fs::path(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data, static_cast<std::streamsize>(n));
    f.flush();
    return static_cast<bool>(f);
}

std::string ReadBytes(const std::wstring& path) {
    std::ifstream f(fs::path(path), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void DumpProblems(const std::vector<FileResult>& problems) {
    for (const FileResult& p : problems) {
        printf("  [dbg] problem status=%d dir=%d rel=%ls sA=%llu sD=%llu\n", (int)p.status,
               (int)p.isDirectory, p.relativePath.c_str(),
               (unsigned long long)p.sizeSource, (unsigned long long)p.sizeDest);
    }
}

std::wstring MakeTwinTrees(const std::wstring& dir) {
    const std::wstring src = dir + L"\\src";
    const std::wstring dst = dir + L"\\dst";
    testgen::CreateFixture(src);
    std::error_code ec;
    fs::copy(fs::path(src), fs::path(dst), fs::copy_options::recursive, ec);
    CHECK(!ec);
    return src; // dst = dir + L"\\dst" recomputed by callers
}

} // namespace

// ---------------------------------------------------------------------------
// ResumePlan unit tests (in-memory indexes, no filesystem)
// ---------------------------------------------------------------------------

TEST("resume: identical rows are reused when fingerprints match", [] {
    FileIndex curA(false), curB(false);
    curA.addEntry(MkEntry(L"a.txt", 100, 1000));
    curB.addEntry(MkEntry(L"a.txt", 100, 1000));
    ScanSession s = MkSession({MkJournal(L"a.txt", Status::Identical, 100, 1000, 100, 1000)});
    ResumeInput in{&s, &curA, &curB, L"A", L"B", {}, {}};
    ResumePlan plan(false);
    std::string detail;
    CHECK(PlanResume(in, plan, detail));
    CHECK(plan.reused == 1u);
    CHECK(plan.stale == 0u);
    CHECK(plan.remainderA.empty() && plan.remainderB.empty());
    CHECK(plan.reusedProblems.empty()); // identicals are counted, not stored
    CHECK(plan.reusedStats.identicalFiles == 1u);
    CHECK(plan.reusedStats.sourceFiles == 1u);
    CHECK(plan.reusedStats.destFiles == 1u);
    CHECK(plan.reusedStats.bytesSource == 100u);
});

TEST("resume: duplicate journal paths count once, last wins", [] {
    FileIndex curA(false), curB(false);
    curA.addEntry(MkEntry(L"a.txt", 100, 1000));
    curB.addEntry(MkEntry(L"a.txt", 100, 1000));
    // Same path twice with different verdicts (append-only journal): only the
    // LAST record may be reused/counted.
    ScanSession s = MkSession({MkJournal(L"a.txt", Status::Identical, 100, 1000, 100, 1000),
                               MkJournal(L"a.txt", Status::ContentMismatch, 100, 1000, 100,
                                         1000)});
    ResumeInput in{&s, &curA, &curB, L"A", L"B", {}, {}};
    ResumePlan plan(false);
    std::string detail;
    CHECK(PlanResume(in, plan, detail));
    CHECK(plan.reused == 1u);
    CHECK(plan.stale == 0u);
    CHECK(plan.reusedEntries.size() == 1u);
    CHECK(plan.reusedEntries[0].verdict == Status::ContentMismatch);
    CHECK(plan.reusedStats.identicalFiles == 0u);
    CHECK(plan.reusedStats.contentMismatch == 1u);
    CHECK(plan.reusedStats.sourceFiles == 1u);
    CHECK(plan.reusedStats.destFiles == 1u);
    CHECK(plan.reusedProblems.size() == 1u);
    CHECK(plan.reusedProblems[0].status == Status::ContentMismatch);
});

TEST("resume: file turned directory is stale even with matching size/mtime", [] {
    FileIndex curA(false), curB(false);
    curA.addEntry(MkEntry(L"a.txt", 100, 1000));
    // A directory crafted with the same size/mtime: fingerprints match, only
    // the type differs. The saved file row must NOT be reused.
    FileEntry dirB = MkEntry(L"a.txt", 100, 1000, true);
    curB.addEntry(std::move(dirB));
    ScanSession s =
        MkSession({MkJournal(L"a.txt", Status::Identical, 100, 1000, 100, 1000)});
    ResumeInput in{&s, &curA, &curB, L"A", L"B", {}, {}};
    ResumePlan plan(false);
    std::string detail;
    CHECK(PlanResume(in, plan, detail));
    CHECK(plan.reused == 0u);
    CHECK(plan.stale == 1u);
    CHECK(plan.remainderB.size() == 1u);
    FileEntry got;
    CHECK(plan.remainderB.find(L"a.txt", got));
    CHECK(got.isDirectory); // the remainder holds the CURRENT entry
});

TEST("resume: directory turned file is stale", [] {
    FileIndex curA(false), curB(false);
    curA.addEntry(MkEntry(L"d", 0, 5000)); // now a file, same size/mtime
    curB.addEntry(MkEntry(L"d", 0, 5000, true));
    ScanSession s = MkSession({MkJournal(L"d", Status::Identical, 0, 5000, 0, 5000, true)});
    ResumeInput in{&s, &curA, &curB, L"A", L"B", {}, {}};
    ResumePlan plan(false);
    std::string detail;
    CHECK(PlanResume(in, plan, detail));
    CHECK(plan.reused == 0u);
    CHECK(plan.stale == 1u);
    CHECK(plan.remainderA.size() == 1u);
    FileEntry got;
    CHECK(plan.remainderA.find(L"d", got));
    CHECK(!got.isDirectory);
});

TEST("resume: file turned directory on both sides is stale", [] {
    FileIndex curA(false), curB(false);
    curA.addEntry(MkEntry(L"a.txt", 100, 1000, true));
    curB.addEntry(MkEntry(L"a.txt", 100, 1000, true));
    ScanSession s =
        MkSession({MkJournal(L"a.txt", Status::Identical, 100, 1000, 100, 1000)});
    ResumeInput in{&s, &curA, &curB, L"A", L"B", {}, {}};
    ResumePlan plan(false);
    std::string detail;
    CHECK(PlanResume(in, plan, detail));
    CHECK(plan.reused == 0u);
    CHECK(plan.stale == 1u);
    CHECK(plan.remainderA.size() == 1u);
    CHECK(plan.remainderB.size() == 1u);
});

TEST("resume: missing file turned directory is stale", [] {
    FileIndex curA(false), curB(false);
    curA.addEntry(MkEntry(L"f.txt", 10, 100, true)); // same size/mtime, now a dir
    ScanSession s = MkSession({MkJournal(L"f.txt", Status::Missing, 10, 100, 0, 0)});
    ResumeInput in{&s, &curA, &curB, L"A", L"B", {}, {}};
    ResumePlan plan(false);
    std::string detail;
    CHECK(PlanResume(in, plan, detail));
    CHECK(plan.reused == 0u);
    CHECK(plan.stale == 1u);
    CHECK(plan.remainderA.size() == 1u);
    FileEntry got;
    CHECK(plan.remainderA.find(L"f.txt", got));
    CHECK(got.isDirectory);
});

TEST("resume: extra file turned directory is stale", [] {
    FileIndex curA(false), curB(false);
    curB.addEntry(MkEntry(L"f.txt", 10, 100, true)); // same size/mtime, now a dir
    ScanSession s = MkSession({MkJournal(L"f.txt", Status::Extra, 0, 0, 10, 100)});
    ResumeInput in{&s, &curA, &curB, L"A", L"B", {}, {}};
    ResumePlan plan(false);
    std::string detail;
    CHECK(PlanResume(in, plan, detail));
    CHECK(plan.reused == 0u);
    CHECK(plan.stale == 1u);
    CHECK(plan.remainderB.size() == 1u);
    FileEntry got;
    CHECK(plan.remainderB.find(L"f.txt", got));
    CHECK(got.isDirectory);
});

TEST("resume: size or mtime change forces re-verification", [] {
    for (int variant = 0; variant < 3; ++variant) {
        FileIndex curA(false), curB(false);
        const uint64_t sizeA = (variant == 0) ? 101u : 100u;
        const uint64_t mtimeA = (variant == 1) ? 1001u : 1000u;
        const uint64_t mtimeB = (variant == 2) ? 1001u : 1000u;
        curA.addEntry(MkEntry(L"a.txt", sizeA, mtimeA));
        curB.addEntry(MkEntry(L"a.txt", 100, mtimeB));
        ScanSession s =
            MkSession({MkJournal(L"a.txt", Status::Identical, 100, 1000, 100, 1000)});
        ResumeInput in{&s, &curA, &curB, L"A", L"B", {}, {}};
        ResumePlan plan(false);
        std::string detail;
        CHECK(PlanResume(in, plan, detail));
        CHECK(plan.reused == 0u);
        CHECK(plan.stale == 1u);
        CHECK(plan.remainderA.size() == 1u);
        CHECK(plan.remainderB.size() == 1u);
    }
});

TEST("resume: presence changes route to remainder, double absence drops", [] {
    // Was Missing (A-only), now present on both: stale, both sides re-verified.
    {
        FileIndex curA(false), curB(false);
        curA.addEntry(MkEntry(L"f.txt", 10, 100));
        curB.addEntry(MkEntry(L"f.txt", 10, 100));
        ScanSession s = MkSession({MkJournal(L"f.txt", Status::Missing, 10, 100, 0, 0)});
        ResumeInput in{&s, &curA, &curB, L"A", L"B", {}, {}};
        ResumePlan plan(false);
        std::string detail;
        CHECK(PlanResume(in, plan, detail));
        CHECK(plan.stale == 1u);
        CHECK(plan.remainderA.size() == 1u);
        CHECK(plan.remainderB.size() == 1u);
    }
    // Was Identical, now gone from both: dropped silently.
    {
        FileIndex curA(false), curB(false);
        ScanSession s = MkSession({MkJournal(L"f.txt", Status::Identical, 10, 100, 10, 100)});
        ResumeInput in{&s, &curA, &curB, L"A", L"B", {}, {}};
        ResumePlan plan(false);
        std::string detail;
        CHECK(PlanResume(in, plan, detail));
        CHECK(plan.reused == 0u);
        CHECK(plan.stale == 0u);
        CHECK(plan.droppedBothGone == 1u);
    }
    // Missing still missing: reused (absence re-proven by complete lists).
    {
        FileIndex curA(false), curB(false);
        curA.addEntry(MkEntry(L"f.txt", 10, 100));
        ScanSession s = MkSession({MkJournal(L"f.txt", Status::Missing, 10, 100, 0, 0)});
        ResumeInput in{&s, &curA, &curB, L"A", L"B", {}, {}};
        ResumePlan plan(false);
        std::string detail;
        CHECK(PlanResume(in, plan, detail));
        CHECK(plan.reused == 1u);
        CHECK(plan.reusedProblems.size() == 1u);
        CHECK(plan.reusedProblems[0].status == Status::Missing);
        CHECK(plan.reusedStats.missingFiles == 1u);
    }
});

TEST("resume: error verdicts are never reused", [] {
    for (Status err : {Status::ReadError, Status::AccessDenied, Status::ChangedDuringScan}) {
        FileIndex curA(false), curB(false);
        curA.addEntry(MkEntry(L"f.txt", 10, 100));
        curB.addEntry(MkEntry(L"f.txt", 10, 100));
        ScanSession s = MkSession({MkJournal(L"f.txt", err, 10, 100, 10, 100)});
        ResumeInput in{&s, &curA, &curB, L"A", L"B", {}, {}};
        ResumePlan plan(false);
        std::string detail;
        CHECK(PlanResume(in, plan, detail));
        CHECK(plan.reused == 0u);
        CHECK(plan.errorRows == 1u);
        CHECK(plan.remainderA.size() == 1u);
        CHECK(plan.remainderB.size() == 1u);
    }
});

TEST("resume: errored subtrees force stale, unprovable absence aborts", [] {
    // Present under an errored dir: stale even with matching fingerprints.
    {
        FileIndex curA(false), curB(false);
        curA.addEntry(MkEntry(L"sub\\f.txt", 10, 100));
        curB.addEntry(MkEntry(L"sub\\f.txt", 10, 100));
        ScanSession s =
            MkSession({MkJournal(L"sub\\f.txt", Status::Identical, 10, 100, 10, 100)});
        ResumeInput in{&s, &curA, &curB, L"A", L"B", {L"sub"}, {}};
        ResumePlan plan(false);
        std::string detail;
        CHECK(PlanResume(in, plan, detail));
        CHECK(plan.reused == 0u);
        CHECK(plan.stale == 1u);
    }
    // Absent inside an errored subtree: absence unprovable -> refuse.
    {
        FileIndex curA(false), curB(false);
        ScanSession s =
            MkSession({MkJournal(L"sub\\f.txt", Status::Identical, 10, 100, 10, 100)});
        ResumeInput in{&s, &curA, &curB, L"A", L"B", {L"sub"}, {}};
        ResumePlan plan(false);
        std::string detail;
        CHECK(!PlanResume(in, plan, detail));
        CHECK(!detail.empty());
    }
});

// ---------------------------------------------------------------------------
// End-to-end: save + resume through ScanController
// ---------------------------------------------------------------------------

TEST("resume: session-out saves a complete journal of identical trees", [] {
    TempDir tmp;
    const std::wstring src = MakeTwinTrees(tmp.path);
    const std::wstring dst = tmp.path + L"\\dst";
    const std::wstring base = tmp.path + L"\\sess";
    ScanReport r = RunScan(src, dst, ScanMode::Content, base);
    CHECK(r.sourceOk);
    CHECK(r.destinationOk);
    CHECK(r.sessionSaved);
    ScanSession loaded;
    LoadOutcome o = LoadSession(base, loaded);
    CHECK(o.ok);
    CHECK(!o.journalTruncated);
    CHECK(!loaded.journal.empty());
    // Identical trees: every journaled row is Identical.
    for (const JournalEntry& e : loaded.journal) CHECK(e.verdict == Status::Identical);
    CHECK(loaded.settings.mode == ScanMode::Content);
});

TEST("resume: unchanged trees resume to results identical to a fresh run", [] {
    TempDir tmp;
    const std::wstring src = MakeTwinTrees(tmp.path);
    const std::wstring dst = tmp.path + L"\\dst";
    const std::wstring base = tmp.path + L"\\sess";
    ScanReport first = RunScan(src, dst, ScanMode::Content, base);
    CHECK(first.sessionSaved);
    ScanReport resumed = RunScan(src, dst, ScanMode::Content, L"", base);
    CHECK(resumed.sourceOk);
    CHECK(resumed.destinationOk);
    CHECK(resumed.usedSession);
    CHECK(resumed.sessionReused > 0u);
    CHECK(resumed.sessionStale == 0u);
    ScanReport fresh = RunScan(src, dst, ScanMode::Content);
    CHECK(StatsEqual(resumed.results.stats, fresh.results.stats));
    CHECK(ProblemsEqual(resumed.results.problems, fresh.results.problems));
});

TEST("resume: changed/added/deleted files match a fresh run", [] {
    TempDir tmp;
    const std::wstring src = MakeTwinTrees(tmp.path);
    const std::wstring dst = tmp.path + L"\\dst";
    const std::wstring base = tmp.path + L"\\sess";
    ScanReport first = RunScan(src, dst, ScanMode::Content, base);
    CHECK(first.sessionSaved);
    // Same-size content change (stale via mtime), plus add + delete. The
    // replacement keeps the on-disk size so the verdict must be a content
    // mismatch, not a size mismatch. Checked writes so a silent failure
    // cannot mask a missed detection.
    const uint64_t aSize = fs::file_size(fs::path(src + L"\\a.txt"));
    CHECK(aSize > 0u);
    CHECK(WriteText(src + L"\\a.txt", std::string((size_t)aSize, 'Q')));
    CHECK(WriteText(src + L"\\nuovo.txt", "brand new file"));
    std::error_code ec;
    CHECK(fs::remove(fs::path(dst + L"\\b.txt"), ec));
    ScanReport resumed = RunScan(src, dst, ScanMode::Content, L"", base);
    CHECK(resumed.sourceOk);
    CHECK(resumed.destinationOk);
    CHECK(resumed.usedSession);
    CHECK(resumed.sessionStale >= 3u);
    ScanReport fresh = RunScan(src, dst, ScanMode::Content);
    if (!StatsEqual(resumed.results.stats, fresh.results.stats) ||
        !ProblemsEqual(resumed.results.problems, fresh.results.problems)) {
        printf("  [dbg] resumed problems:\n");
        DumpProblems(resumed.results.problems);
        printf("  [dbg] fresh problems:\n");
        DumpProblems(fresh.results.problems);
        printf("  [dbg] reused=%llu stale=%llu\n", (unsigned long long)resumed.sessionReused,
               (unsigned long long)resumed.sessionStale);
    }
    CHECK(StatsEqual(resumed.results.stats, fresh.results.stats));
    CHECK(ProblemsEqual(resumed.results.problems, fresh.results.problems));
    // The changed file is really detected (not blindly reused).
    bool foundMismatch = false;
    for (const FileResult& p : resumed.results.problems) {
        if (p.relativePath == L"a.txt" && p.status == Status::ContentMismatch)
            foundMismatch = true;
    }
    CHECK(foundMismatch);
});

TEST("resume: snapshot-out captures the current source during a resumed run", [] {
    TempDir tmp;
    const std::wstring src = MakeTwinTrees(tmp.path);
    const std::wstring dst = tmp.path + L"\\dst";
    const std::wstring base = tmp.path + L"\\sess";
    const std::wstring snap = tmp.path + L"\\snap.bin";
    ScanReport first = RunScan(src, dst, ScanMode::Content, base);
    CHECK(first.sessionSaved);
    // Resumed run that also refreshes the snapshot from the current source.
    ScanReport r = RunScan(src, dst, ScanMode::Content, L"", base, 100,
                           PartialPattern::Edges, 0, 0, 0, L"" /*compare*/, snap);
    CHECK(r.sourceOk);
    CHECK(r.destinationOk);
    CHECK(r.usedSession);
    CHECK(r.snapshotWritten);
    FileIndex idx(false);
    std::wstring root, err;
    CHECK(indexio::ReadSnapshot(snap, idx, root, err));
    CHECK(root == src);
    CHECK(idx.stats().files > 0u);
    ScanReport fresh = RunScan(src, dst, ScanMode::Content);
    CHECK(StatsEqual(r.results.stats, fresh.results.stats));
    CHECK(ProblemsEqual(r.results.problems, fresh.results.problems));
});

TEST("resume: offline resume against a snapshot matches offline fresh", [] {
    TempDir tmp;
    const std::wstring src = MakeTwinTrees(tmp.path);
    const std::wstring dst = tmp.path + L"\\dst";
    const std::wstring base = tmp.path + L"\\sess";
    const std::wstring snap = tmp.path + L"\\snap.bin";
    // Live run saving snapshot + session from the same state.
    ScanReport first =
        RunScan(src, dst, ScanMode::Content, base, L"", 100, PartialPattern::Edges, 0, 0,
                0, L"" /*compare*/, snap);
    CHECK(first.sessionSaved);
    CHECK(first.snapshotWritten);
    // Offline resume: source device absent (empty source), digests from snapshot.
    ScanReport resumed = RunScan(L"", dst, ScanMode::Content, L"", base, 100,
                                 PartialPattern::Edges, 0, 0, 0, snap);
    CHECK(resumed.sourceOk);
    CHECK(resumed.destinationOk);
    CHECK(resumed.usedSession);
    CHECK(resumed.usedSnapshot);
    ScanReport freshOffline = RunScan(L"", dst, ScanMode::Content, L"", L"", 100,
                                      PartialPattern::Edges, 0, 0, 0, snap);
    CHECK(StatsEqual(resumed.results.stats, freshOffline.results.stats));
    CHECK(ProblemsEqual(resumed.results.problems, freshOffline.results.problems));
});

TEST("resume: cumulative wall time grows monotonically across chained runs", [] {
    TempDir tmp;
    const std::wstring src = MakeTwinTrees(tmp.path);
    const std::wstring dst = tmp.path + L"\\dst";
    const std::wstring base = tmp.path + L"\\sess";
    ScanReport first = RunScan(src, dst, ScanMode::Content, base);
    CHECK(first.sessionSaved);
    ScanSession s1;
    CHECK(LoadSession(base, s1).ok);
    ScanReport second = RunScan(src, dst, ScanMode::Content, L"", base);
    CHECK(second.usedSession);
    CHECK(second.sessionTotalMillis >= s1.runMillis);
    // A resumed run that re-saves chains the accumulation forward.
    ScanReport third = RunScan(src, dst, ScanMode::Content, base, base);
    CHECK(third.usedSession);
    CHECK(third.sessionSaved);
    ScanSession s3;
    CHECK(LoadSession(base, s3).ok);
    CHECK(s3.runMillis >= s1.runMillis);
    CHECK(third.sessionTotalMillis >= s3.runMillis);
});

TEST("resume: truncated journal still resumes, flags reported, results match fresh", [] {
    TempDir tmp;
    const std::wstring src = MakeTwinTrees(tmp.path);
    const std::wstring dst = tmp.path + L"\\dst";
    const std::wstring base = tmp.path + L"\\sess";
    ScanReport first = RunScan(src, dst, ScanMode::Content, base);
    CHECK(first.sessionSaved);
    // Damage the journal mid-record (a torn crash tail).
    const std::string jb = ReadBytes(bv::session::JournalPath(base));
    CHECK(!jb.empty());
    CHECK(WriteBytes(bv::session::JournalPath(base), jb.data(), jb.size() / 2));
    ScanReport resumed = RunScan(src, dst, ScanMode::Content, L"", base);
    CHECK(resumed.sourceOk);
    CHECK(resumed.destinationOk);
    CHECK(resumed.usedSession);
    CHECK(resumed.sessionJournalTruncated);
    CHECK(resumed.sessionRecovered > 0u);
    CHECK(!resumed.sessionFellBackToPrev);
    ScanReport fresh = RunScan(src, dst, ScanMode::Content);
    CHECK(StatsEqual(resumed.results.stats, fresh.results.stats));
    CHECK(ProblemsEqual(resumed.results.problems, fresh.results.problems));
});

TEST("resume: corrupt context falls back to .prev and reports it", [] {
    TempDir tmp;
    const std::wstring src = MakeTwinTrees(tmp.path);
    const std::wstring dst = tmp.path + L"\\dst";
    const std::wstring base = tmp.path + L"\\sess";
    ScanReport first = RunScan(src, dst, ScanMode::Content, base);
    CHECK(first.sessionSaved);
    ScanReport again = RunScan(src, dst, ScanMode::Content, base);
    CHECK(again.sessionSaved); // second save rotates a good .prev
    const char junk[] = "{not valid json";
    CHECK(WriteBytes(bv::session::ContextPath(base), junk, sizeof(junk) - 1));
    ScanReport resumed = RunScan(src, dst, ScanMode::Content, L"", base);
    CHECK(resumed.sourceOk);
    CHECK(resumed.destinationOk);
    CHECK(resumed.usedSession);
    CHECK(resumed.sessionFellBackToPrev);
    ScanReport fresh = RunScan(src, dst, ScanMode::Content);
    CHECK(StatsEqual(resumed.results.stats, fresh.results.stats));
    CHECK(ProblemsEqual(resumed.results.problems, fresh.results.problems));
});

TEST("resume: degraded Size session is never promoted to Content", [] {
    TempDir tmp;
    const std::wstring src = tmp.path + L"\\src";
    const std::wstring dst = tmp.path + L"\\dst";
    fs::create_directories(fs::path(src));
    fs::create_directories(fs::path(dst));
    // Same size, different content: Size sees identical, Content sees mismatch.
    CHECK(WriteText(src + L"\\a.txt", "hello world"));
    CHECK(WriteText(dst + L"\\a.txt", "HELLO WORLD"));
    const std::wstring snap = tmp.path + L"\\snap.bin";
    const std::wstring base = tmp.path + L"\\sess";
    // Presence snapshot: no digests.
    ScanReport cap = RunScan(src, L"", ScanMode::Presence, L"", L"", 100,
                             PartialPattern::Edges, 0, 0, 0, L"", snap);
    CHECK(cap.snapshotWritten);
    // Offline run requested as Content degrades to Size; session is saved.
    ScanReport degraded = RunScan(L"", dst, ScanMode::Content, base, L"", 100,
                                  PartialPattern::Edges, 0, 0, 0, snap);
    CHECK(degraded.sourceOk);
    CHECK(degraded.contentDegradedToSize);
    CHECK(degraded.sessionSaved);
    // The persisted mode must be the EFFECTIVE one (Size), not requested Content.
    ScanSession saved;
    CHECK(LoadSession(base, saved).ok);
    CHECK(saved.settings.mode == ScanMode::Size);
    // Resuming it requesting Content must refuse cleanly: reusing Size-made
    // Identical rows as Content would skip hashing and miss the mismatch.
    ScanReport refused = RunScan(src, dst, ScanMode::Content, L"", base);
    CHECK(!refused.sourceOk);
    // Resuming with matching Size settings works and reuses the identical row.
    ScanReport sized = RunScan(src, dst, ScanMode::Size, L"", base);
    CHECK(sized.sourceOk);
    CHECK(sized.usedSession);
    CHECK(sized.sessionReused > 0u);
    ScanReport freshSize = RunScan(src, dst, ScanMode::Size);
    CHECK(StatsEqual(sized.results.stats, freshSize.results.stats));
    // Sanity: a true Content run really sees the mismatch.
    ScanReport freshContent = RunScan(src, dst, ScanMode::Content);
    bool foundMismatch = false;
    for (const FileResult& p : freshContent.results.problems) {
        if (p.relativePath == L"a.txt" && p.status == Status::ContentMismatch)
            foundMismatch = true;
    }
    CHECK(foundMismatch);
});

TEST("resume: incompatible settings fail cleanly", [] {
    TempDir tmp;
    const std::wstring src = MakeTwinTrees(tmp.path);
    const std::wstring dst = tmp.path + L"\\dst";
    const std::wstring base = tmp.path + L"\\sess";
    ScanReport first = RunScan(src, dst, ScanMode::Content, base);
    CHECK(first.sessionSaved);
    // Different mode: must refuse, never fabricate.
    ScanReport bad = RunScan(src, dst, ScanMode::Size, L"", base);
    CHECK(!bad.sourceOk);
    CHECK(!bad.results.problems.empty());
    CHECK(!bad.usedSession);
});

TEST("resume: partial verify round-trips under identical settings", [] {
    TempDir tmp;
    const std::wstring src = MakeTwinTrees(tmp.path);
    const std::wstring dst = tmp.path + L"\\dst";
    const std::wstring base = tmp.path + L"\\sess";
    ScanReport first = RunScan(src, dst, ScanMode::Content, base, L"", 50, PartialPattern::Edges);
    CHECK(first.sessionSaved);
    ScanReport resumed = RunScan(src, dst, ScanMode::Content, L"", base, 50, PartialPattern::Edges);
    CHECK(resumed.sourceOk);
    CHECK(resumed.usedSession);
    // Random is resolved per run, so only the deterministic pattern is used here.
    ScanReport fresh = RunScan(src, dst, ScanMode::Content, L"", L"", 50, PartialPattern::Edges);
    CHECK(StatsEqual(resumed.results.stats, fresh.results.stats));
});

TEST("resume: aggressive checkpointing stays correct under race", [] {
    // checkpointRows=1 maximizes pump/final interleaving. Loop several times:
    // correctness must hold whether or not the background thread fires.
    for (int iter = 0; iter < 5; ++iter) {
        TempDir tmp;
        const std::wstring src = MakeTwinTrees(tmp.path);
        const std::wstring dst = tmp.path + L"\\dst";
        const std::wstring base = tmp.path + L"\\sess";
        ScanReport r = RunScan(src, dst, ScanMode::Content, base, L"", 100,
                               PartialPattern::Edges, 1 /*checkpointRows*/);
        CHECK(r.sourceOk);
        CHECK(r.destinationOk);
        CHECK(r.sessionSaved);
        ScanReport fresh = RunScan(src, dst, ScanMode::Content);
        CHECK(StatsEqual(r.results.stats, fresh.results.stats));
        CHECK(ProblemsEqual(r.results.problems, fresh.results.problems));
        ScanSession loaded;
        LoadOutcome o = LoadSession(base, loaded);
        CHECK(o.ok);
        CHECK(!o.journalTruncated);
        CHECK(loaded.state == SessionState::Completed);
        CHECK(o.journalRecovered == loaded.journal.size());
        CHECK(!loaded.journal.empty());
    }
});

TEST("resume: cancelled run saves an interrupted session that resumes cleanly", [] {
    TempDir tmp;
    const std::wstring src = MakeTwinTrees(tmp.path);
    const std::wstring dst = tmp.path + L"\\dst";
    const std::wstring base = tmp.path + L"\\sess";
    // Cancel once hashing starts: matched rows already exist, pending hash
    // verdicts are dropped by construction (never fabricated).
    std::atomic_bool cancel{false};
    int progressCalls = 0;
    ScanOptions opts;
    opts.source = src;
    opts.destination = dst;
    opts.mode = ScanMode::Content;
    opts.sessionOut = base;
    opts.cancel = &cancel;
    opts.onProgress = [&](const ScanProgress& p) {
        ++progressCalls;
        if (p.phase == ScanPhase::Hashing || progressCalls > 30) cancel.store(true);
    };
    ScanReport interrupted = ScanController(false).run(opts);
    CHECK(progressCalls > 0);
    CHECK(interrupted.sessionSaved);
    ScanSession loaded;
    LoadOutcome o = LoadSession(base, loaded);
    CHECK(o.ok);
    CHECK(!o.journalTruncated);
    CHECK(loaded.state == SessionState::Interrupted);
    CHECK(!loaded.journal.empty());
    // Resume the interrupted session: must equal a fresh uninterrupted run.
    ScanReport resumed = RunScan(src, dst, ScanMode::Content, L"", base);
    CHECK(resumed.sourceOk);
    CHECK(resumed.destinationOk);
    CHECK(resumed.usedSession);
    ScanReport fresh = RunScan(src, dst, ScanMode::Content);
    CHECK(StatsEqual(resumed.results.stats, fresh.results.stats));
    CHECK(ProblemsEqual(resumed.results.problems, fresh.results.problems));
});

TEST("resume: periodic checkpoints fire mid-run", [] {
    // Long enough run (>150ms anywhere) that the 100ms pump wakes at least
    // once mid-run: checkpoint.seq >= 2 proves a periodic AppendJournal ran
    // before the incremental final save (seq == 1 would mean final-only).
    TempDir tmp;
    const std::wstring src = tmp.path + L"\\src";
    const std::wstring dst = tmp.path + L"\\dst";
    fs::create_directories(fs::path(src));
    // Real (non-sparse) content so hashing takes longer than the 100ms pump
    // slice: sparse files hash in milliseconds and the run would end before
    // the pump ever wakes.
    std::string chunk(65536, '\0');
    {
        std::mt19937 rng(12345);
        for (char& c : chunk) c = static_cast<char>(rng() & 0xFF);
    }
    for (int i = 0; i < 24; ++i) {
        wchar_t name[32];
        swprintf(name, 32, L"big%02d.bin", i);
        std::ofstream f(fs::path(src + L"\\" + name), std::ios::binary | std::ios::trunc);
        CHECK(static_cast<bool>(f));
        for (int k = 0; k < 192; ++k) f.write(chunk.data(), chunk.size()); // 12 MiB
        f.flush();
        CHECK(static_cast<bool>(f));
    }
    std::error_code ec;
    fs::copy(fs::path(src), fs::path(dst), fs::copy_options::recursive, ec);
    CHECK(!ec);
    const std::wstring base = tmp.path + L"\\sess";
    // Single hash thread stretches the run well past the 100ms pump slice on
    // any hardware (576 MiB hashed serially), so periodic checkpoints must fire.
    ScanReport r = RunScan(src, dst, ScanMode::Content, base, L"", 100,
                           PartialPattern::Edges, 5 /*checkpointRows*/, 0, 1 /*threads*/);
    CHECK(r.sourceOk);
    CHECK(r.destinationOk);
    CHECK(r.sessionSaved);
    ScanSession loaded;
    LoadOutcome o = LoadSession(base, loaded);
    CHECK(o.ok);
    CHECK(!o.journalTruncated);
    CHECK(loaded.state == SessionState::Completed);
    CHECK(loaded.checkpoint.seq >= 2u);
    CHECK(o.journalRecovered == loaded.journal.size());
    CHECK(!loaded.journal.empty());
    ScanReport fresh = RunScan(src, dst, ScanMode::Content);
    CHECK(StatsEqual(r.results.stats, fresh.results.stats));
    CHECK(ProblemsEqual(r.results.problems, fresh.results.problems));
});

TEST("resume: checkpoint overhead is bounded (informational)", [] {
    TempDir tmp;
    const std::wstring src = tmp.path + L"\\src";
    const std::wstring dst = tmp.path + L"\\dst";
    CHECK(testgen::CreateStressTree(src, 600) == 600u);
    std::error_code ec;
    fs::copy(fs::path(src), fs::path(dst), fs::copy_options::recursive, ec);
    CHECK(!ec);
    const std::wstring base = tmp.path + L"\\sess";
    const auto t0 = std::chrono::steady_clock::now();
    ScanReport plain = RunScan(src, dst, ScanMode::Content);
    const auto t1 = std::chrono::steady_clock::now();
    ScanReport checked =
        RunScan(src, dst, ScanMode::Content, base, L"", 100, PartialPattern::Edges, 60);
    const auto t2 = std::chrono::steady_clock::now();
    CHECK(checked.sessionSaved);
    CHECK(StatsEqual(checked.results.stats, plain.results.stats));
    CHECK(ProblemsEqual(checked.results.problems, plain.results.problems));
    ScanSession loaded;
    CHECK(LoadSession(base, loaded).ok);
    const auto plainMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    const auto checkedMs = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    const uint64_t ncp = loaded.checkpoint.seq > 0 ? loaded.checkpoint.seq : 1;
    std::cout << "  [bench] checkpoint overhead: plain " << plainMs << " ms, checkpointed "
              << checkedMs << " ms over " << ncp << " checkpoints (~"
              << (checkedMs > plainMs ? (checkedMs - plainMs) / ncp : 0) << " ms/checkpoint)\n";
    CHECK(checkedMs < 4 * plainMs + 5000);
});

TEST("resume: hand-crafted error rows are re-verified, never reused", [] {
    TempDir tmp;
    const std::wstring base = tmp.path + L"\\sess";
    // Craft a session: one identical row (stale by absence on B) + one error
    // row for a present file. Neither may be reused.
    FileIndex curA(false);
    curA.addEntry(MkEntry(L"a.txt", 17, 2000));
    FileEntry got;
    CHECK(curA.find(L"a.txt", got));
    ScanSession s = MkSession({
        MkJournal(L"a.txt", Status::Identical, got.size, got.lastWriteTime, got.size,
                  got.lastWriteTime),
        MkJournal(L"b.txt", Status::ReadError, 7, 3000, 7, 3000),
    });
    // b.txt really exists with other fingerprints: forces re-verify path.
    FileIndex curB(false);
    curB.addEntry(MkEntry(L"b.txt", 999, 9999));
    std::wstring err;
    CHECK(SaveSession(base, s, err));
    // Plan directly against synthetic currents: error row must go stale.
    ResumeInput in{&s, &curA, &curB, L"A", L"B", {}, {}};
    ResumePlan plan(false);
    std::string detail;
    CHECK(PlanResume(in, plan, detail));
    // a.txt expected on both sides but missing on B -> stale; the b.txt error
    // row is forced to re-verify and is never reused.
    CHECK(plan.reused == 0u);
    CHECK(plan.stale == 2u);
    CHECK(plan.errorRows == 1u);
    CHECK(plan.remainderA.size() == 1u);
    CHECK(plan.remainderB.size() == 1u);
});
