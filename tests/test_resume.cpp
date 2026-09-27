// Phase 1 resume tests: ResumePlan partitioning (in-memory indexes) and
// end-to-end save/resume through ScanController (on-disk trees).
//
// Soundness invariant checked throughout: a resumed run produces results
// IDENTICAL to a fresh run over the same trees (same stats, same problems).

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Comparison/ScanMode.h"
#include "Filesystem/FileIndex.h"
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
                   PartialPattern pattern = PartialPattern::Edges) {
    ScanOptions opts;
    opts.source = src;
    opts.destination = dst;
    opts.mode = mode;
    opts.sessionOut = sessionOut;
    opts.resumeFrom = resumeFrom;
    opts.verifyLevel.percent = verifyPercent;
    opts.verifyLevel.pattern = pattern;
    return ScanController(false).run(opts);
}

bool WriteText(const std::wstring& path, const std::string& data) {
    std::ofstream f(fs::path(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.flush();
    return static_cast<bool>(f);
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
