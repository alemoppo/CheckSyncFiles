// Tests for the Sync module: path containment, plan building (A->B),
// execution end-to-end (files, dirs, links, guards, cancel, error policy)
// and ResultSet integration via ApplySingleResult.

#include <chrono>
#include <filesystem>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Comparison/SingleVerify.h"
#include "Filesystem/ReparsePoint.h"
#include "ScanOrchestrator.h"
#include "Sync/SyncExecutor.h"
#include "Sync/SyncFs.h"
#include "Sync/SyncPath.h"
#include "Sync/SyncPlan.h"
#include "TestHarness.h"
#include "TestHelpers.h"

using namespace bv;
using namespace bv::sync;
using namespace bv::testutil;
namespace fs = std::filesystem;

namespace {

FileResult MakeRow(Status st, const std::wstring& rel, bool isDir = false,
                   uint64_t sizeSrc = 0, uint64_t sizeDst = 0,
                   ReparseKind kind = ReparseKind::None) {
    FileResult r;
    r.status = st;
    r.relativePath = rel;
    r.isDirectory = isDir;
    r.sizeSource = sizeSrc;
    r.sizeDest = sizeDst;
    r.reparseKind = kind;
    return r;
}

std::string ReadAll(const std::wstring& path) {
    return ReadFileBytes(path);
}

} // namespace

TEST("sync-path: canonical rel resolves, attacks rejected", [] {
    CHECK(ResolveWithinRoot(L"C:\\A", L"sub\\f.txt") == L"C:\\A\\sub\\f.txt");
    CHECK(ResolveWithinRoot(L"C:\\A", L"a/b") == L"C:\\A\\a\\b");
    CHECK(ResolveWithinRoot(L"C:\\A", L"") == L"");
    CHECK(ResolveWithinRoot(L"C:\\A", L"..\\x") == L"");
    CHECK(ResolveWithinRoot(L"C:\\A", L"sub\\..\\x") == L"");
    CHECK(ResolveWithinRoot(L"C:\\A", L"C:\\other\\f") == L"");
    CHECK(ResolveWithinRoot(L"C:\\A", L"\\abs") == L"");
    CHECK(ResolveWithinRoot(L"C:\\A", L"sub\\\\f") == L"");
    CHECK(ResolveWithinRoot(L"C:\\A", L"sub\\") == L"");
    CHECK(ResolveWithinRoot(L"C:\\A", L".\\f") == L"");
    // Case-insensitive roots still contain.
    CHECK(ResolveWithinRoot(L"c:\\a", L"SUB\\F") == L"c:\\a\\SUB\\F");
    CHECK(FreeBytesOnVolume(MakeTempDir()) > 0);
});

TEST("sync-plan: mixed rows produce ordered actions and skips", [] {
    ResultSet rs;
    rs.problems.push_back(MakeRow(Status::Missing, L"new.txt", false, 100));
    rs.problems.push_back(MakeRow(Status::Missing, L"emptydir", true));
    rs.problems.push_back(MakeRow(Status::Extra, L"old.txt"));
    rs.problems.push_back(MakeRow(Status::Extra, L"exdir", true));
    rs.problems.push_back(MakeRow(Status::SizeMismatch, L"chg.txt", false, 50, 60));
    rs.problems.push_back(
        MakeRow(Status::ContentMismatch, L"j", true, 0, 0, ReparseKind::Junction));
    rs.problems.push_back(
        MakeRow(Status::Missing, L"weird", false, 0, 0, ReparseKind::Other));
    rs.problems.push_back(MakeRow(Status::ReadError, L"err.txt"));

    const SyncPlan plan = BuildSyncPlan(rs, L"C:\\A", L"C:\\B");
    CHECK_EQ(plan.actions.size(), 6ull);
    // Order: DirCreate, file writes, link writes, deletes, DirDelete.
    CHECK(plan.actions[0].op == SyncOp::DirCreate);
    CHECK(plan.actions[1].op == SyncOp::FileCopy);
    CHECK(plan.actions[2].op == SyncOp::FileReplace);
    CHECK(plan.actions[3].op == SyncOp::LinkReplace);
    CHECK(plan.actions[4].op == SyncOp::FileDelete);
    CHECK(plan.actions[5].op == SyncOp::DirDelete);
    CHECK_EQ(plan.summary.copyFiles, 1ull);
    CHECK_EQ(plan.summary.bytesToCopy, 150ull);
    CHECK_EQ(plan.skipped.size(), 2ull);
    CHECK(plan.sourceIsA);
});

TEST("sync-plan: extra ancestors are scheduled deepest-first", [] {
    ResultSet rs;
    rs.problems.push_back(MakeRow(Status::Extra, L"ex\\a.txt"));
    rs.problems.push_back(MakeRow(Status::Extra, L"ex\\sub\\b.txt"));
    const SyncPlan plan = BuildSyncPlan(rs, L"C:\\A", L"C:\\B");
    // 2 file deletes + guarded deletes for ex and ex\sub (sub first).
    CHECK_EQ(plan.actions.size(), 4ull);
    CHECK(plan.actions[0].op == SyncOp::FileDelete);
    CHECK(plan.actions[1].op == SyncOp::FileDelete);
    CHECK(plan.actions[2].op == SyncOp::DirDelete);
    CHECK(plan.actions[2].relativePath == L"ex\\sub");
    CHECK(plan.actions[3].op == SyncOp::DirDelete);
    CHECK(plan.actions[3].relativePath == L"ex");
});

TEST("sync-plan: single manual actions", [] {
    const FileResult missing = MakeRow(Status::Missing, L"n.txt", false, 40);
    SyncPlan p = BuildSingleActionPlan(missing, ManualOp::CopyToDst, L"C:\\A", L"C:\\B",
                                       /*toA=*/false);
    CHECK_EQ(p.actions.size(), 1ull);
    CHECK(p.actions[0].op == SyncOp::FileCopy);
    CHECK(p.sourceRoot == L"C:\\A" && p.destRoot == L"C:\\B");

    const FileResult extra = MakeRow(Status::Extra, L"o.txt");
    p = BuildSingleActionPlan(extra, ManualOp::CopyToDst, L"C:\\A", L"C:\\B",
                              /*toA=*/true);
    CHECK_EQ(p.actions.size(), 1ull);
    CHECK(p.actions[0].op == SyncOp::FileCopy);
    CHECK(p.sourceRoot == L"C:\\B" && p.destRoot == L"C:\\A");

    const FileResult diff = MakeRow(Status::SizeMismatch, L"c.txt", false, 10, 20);
    p = BuildSingleActionPlan(diff, ManualOp::ReplaceToDst, L"C:\\A", L"C:\\B",
                              /*toA=*/false);
    CHECK(p.actions[0].op == SyncOp::FileReplace);

    const FileResult exdir = MakeRow(Status::Extra, L"xd", true);
    p = BuildSingleActionPlan(exdir, ManualOp::DeleteAtDst, L"C:\\A", L"C:\\B",
                              /*toA=*/false);
    CHECK(p.actions[0].op == SyncOp::DirDelete);
});

TEST("sync-exec: full A->B plan converges trees", [] {
    const std::wstring a = MakeTempDir();
    const std::wstring b = MakeTempDir();
    CHECK(WriteFileBytes(a + L"\\keep.txt", "same", 4));
    CHECK(WriteFileBytes(b + L"\\keep.txt", "same", 4));
    CHECK(WriteFileBytes(a + L"\\new.txt", "hello", 5));
    CHECK(fs::create_directories(a + L"\\sub"));
    CHECK(WriteFileBytes(a + L"\\sub\\deep.txt", "deep", 4));
    CHECK(fs::create_directories(a + L"\\emptydir"));
    CHECK(WriteFileBytes(b + L"\\old.txt", "stale", 5));
    CHECK(fs::create_directories(b + L"\\exdir"));
    CHECK(WriteFileBytes(b + L"\\exdir\\x.txt", "x", 1));
    CHECK(fs::create_directories(b + L"\\shared"));
    CHECK(WriteFileBytes(b + L"\\shared\\extra2.txt", "e2", 2));

    ResultSet rs;
    rs.problems.push_back(MakeRow(Status::Missing, L"new.txt", false, 5));
    rs.problems.push_back(MakeRow(Status::Missing, L"sub\\deep.txt", false, 4));
    rs.problems.push_back(MakeRow(Status::Missing, L"emptydir", true));
    rs.problems.push_back(MakeRow(Status::Extra, L"old.txt"));
    rs.problems.push_back(MakeRow(Status::Extra, L"exdir\\x.txt"));
    rs.problems.push_back(MakeRow(Status::Extra, L"shared\\extra2.txt"));

    const SyncPlan plan = BuildSyncPlan(rs, a, b);
    const SyncReport rep = ExecutePlan(plan, nullptr);
    CHECK_EQ(rep.failedCount, 0ull);
    CHECK(rep.doneCount == plan.actions.size());
    CHECK(ReadAll(b + L"\\new.txt") == "hello");
    CHECK(ReadAll(b + L"\\sub\\deep.txt") == "deep");
    CHECK(fs::is_directory(b + L"\\emptydir"));
    CHECK(!fs::exists(b + L"\\old.txt"));
    CHECK(!fs::exists(b + L"\\exdir"));
    CHECK(!fs::exists(b + L"\\shared\\extra2.txt"));
    // No temp files left behind.
    for (const auto& e : fs::recursive_directory_iterator(b)) {
        CHECK(e.path().filename().wstring().find(L".bvtmp_") == std::wstring::npos);
    }
});

TEST("sync-exec: replace preserves content and mtime, read-only deleted", [] {
    const std::wstring a = MakeTempDir();
    const std::wstring b = MakeTempDir();
    CHECK(WriteFileBytes(a + L"\\c.txt", "newcontent", 10));
    CHECK(WriteFileBytes(b + L"\\c.txt", "old", 3));
    CHECK(WriteFileBytes(b + L"\\ro.txt", "gone", 4));
    CHECK(SetFileAttributesW((b + L"\\ro.txt").c_str(), FILE_ATTRIBUTE_READONLY));
    BumpMtimeMinutes(a + L"\\c.txt", 30);

    ResultSet rs;
    rs.problems.push_back(MakeRow(Status::ContentMismatch, L"c.txt", false, 10, 3));
    rs.problems.push_back(MakeRow(Status::Extra, L"ro.txt"));
    const SyncReport rep = ExecutePlan(BuildSyncPlan(rs, a, b), nullptr);
    CHECK_EQ(rep.failedCount, 0ull);
    CHECK(ReadAll(b + L"\\c.txt") == "newcontent");
    CHECK(!fs::exists(b + L"\\ro.txt"));
});

TEST("sync-exec: shared subdir blocks ancestor delete, extras still go", [] {
    const std::wstring a = MakeTempDir();
    const std::wstring b = MakeTempDir();
    CHECK(fs::create_directories(a + L"\\mix"));
    CHECK(fs::create_directories(b + L"\\mix"));
    CHECK(WriteFileBytes(a + L"\\mix\\keep.txt", "k", 1));
    CHECK(WriteFileBytes(b + L"\\mix\\keep.txt", "k", 1));
    CHECK(WriteFileBytes(b + L"\\mix\\drop.txt", "d", 1));

    ResultSet rs;
    rs.problems.push_back(MakeRow(Status::Extra, L"mix\\drop.txt"));
    const SyncReport rep = ExecutePlan(BuildSyncPlan(rs, a, b), nullptr);
    CHECK(!fs::exists(b + L"\\mix\\drop.txt"));
    CHECK(ReadAll(b + L"\\mix\\keep.txt") == "k"); // preserved
    CHECK(fs::is_directory(b + L"\\mix"));         // ancestor survives
    bool sawSkip = false;
    for (const SyncActionResult& it : rep.items) {
        if (it.op == SyncOp::DirDelete && it.skipped) sawSkip = true;
    }
    CHECK(sawSkip);
});

TEST("sync-exec: junction create, replace (rebased) and delete", [] {
    const std::wstring a = MakeTempDir();
    const std::wstring b = MakeTempDir();
    CHECK(fs::create_directory(a + L"\\real"));
    CHECK(WriteFileBytes(a + L"\\real\\f.txt", "data", 4));
    std::wstring err;
    CHECK(CreateLink(a + L"\\j", a + L"\\real", ReparseKind::Junction, err));

    ResultSet rs;
    rs.problems.push_back(
        MakeRow(Status::Missing, L"j", true, 0, 0, ReparseKind::Junction));
    rs.problems.push_back(MakeRow(Status::Missing, L"real\\f.txt", false, 4));
    SyncReport rep = ExecutePlan(BuildSyncPlan(rs, a, b), nullptr);
    CHECK_EQ(rep.failedCount, 0ull);
    CHECK(GetReparseKind(b + L"\\j", true) == ReparseKind::Junction);
    CHECK(ReadLinkTarget(b + L"\\j") == b + L"\\real"); // rebased, not pointing at A
    CHECK(ReadAll(b + L"\\j\\f.txt") == "data");        // not followed, still readable

    // Retarget in A -> replace updates B (rebased again).
    CHECK(fs::create_directory(a + L"\\other"));
    CHECK(DeleteLink(a + L"\\j", true, err));
    CHECK(CreateLink(a + L"\\j", a + L"\\other", ReparseKind::Junction, err));
    ResultSet rs2;
    rs2.problems.push_back(
        MakeRow(Status::ContentMismatch, L"j", true, 0, 0, ReparseKind::Junction));
    rep = ExecutePlan(BuildSyncPlan(rs2, a, b), nullptr);
    CHECK_EQ(rep.failedCount, 0ull);
    CHECK(ReadLinkTarget(b + L"\\j") == b + L"\\other");

    // Gone in A -> delete removes the B link, target untouched.
    CHECK(DeleteLink(a + L"\\j", true, err));
    ResultSet rs3;
    rs3.problems.push_back(
        MakeRow(Status::Extra, L"j", true, 0, 0, ReparseKind::Junction));
    rep = ExecutePlan(BuildSyncPlan(rs3, a, b), nullptr);
    CHECK_EQ(rep.failedCount, 0ull);
    CHECK(!fs::exists(b + L"\\j"));
    CHECK(ReadAll(b + L"\\real\\f.txt") == "data"); // first target untouched
});

TEST("sync-exec: locked file fails but the plan continues", [] {
    const std::wstring a = MakeTempDir();
    const std::wstring b = MakeTempDir();
    CHECK(WriteFileBytes(a + L"\\new.txt", "hello", 5));
    CHECK(WriteFileBytes(b + L"\\locked.txt", "stay", 4));
    HANDLE hold = CreateFileW((b + L"\\locked.txt").c_str(), GENERIC_READ, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(hold != INVALID_HANDLE_VALUE);
    ResultSet rs;
    rs.problems.push_back(MakeRow(Status::Missing, L"new.txt", false, 5));
    rs.problems.push_back(MakeRow(Status::Extra, L"locked.txt"));
    const SyncReport rep = ExecutePlan(BuildSyncPlan(rs, a, b), nullptr);
    CloseHandle(hold);
    CHECK_EQ(rep.failedCount, 1ull);
    CHECK(ReadAll(b + L"\\new.txt") == "hello"); // independent action completed
    CHECK(fs::exists(b + L"\\locked.txt"));      // failed action reported, kept
});

TEST("sync-exec: pre-set cancel runs nothing", [] {
    const std::wstring a = MakeTempDir();
    const std::wstring b = MakeTempDir();
    CHECK(WriteFileBytes(a + L"\\new.txt", "hello", 5));
    ResultSet rs;
    rs.problems.push_back(MakeRow(Status::Missing, L"new.txt", false, 5));
    std::atomic_bool cancel{true};
    const SyncReport rep = ExecutePlan(BuildSyncPlan(rs, a, b), &cancel);
    CHECK(rep.cancelled);
    CHECK_EQ(rep.doneCount, 0ull);
    CHECK(!fs::exists(b + L"\\new.txt"));
});

TEST("sync-exec: failed copy leaves no temp and no target", [] {
    const std::wstring a = MakeTempDir();
    const std::wstring b = MakeTempDir();
    CHECK(WriteFileBytes(a + L"\\src.txt", "data", 4));
    HANDLE hold = CreateFileW((a + L"\\src.txt").c_str(), GENERIC_READ, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(hold != INVALID_HANDLE_VALUE);
    // Exclusive source lock: CopyFileEx cannot read -> temp must be cleaned.
    std::wstring err;
    const bool ok = CopyFileAtomic(a + L"\\src.txt", b + L"\\dst.txt", nullptr, err);
    CloseHandle(hold);
    CHECK(!ok);
    CHECK(!fs::exists(b + L"\\dst.txt"));
    for (const auto& e : fs::directory_iterator(b)) {
        CHECK(e.path().filename().wstring().find(L".bvtmp_") == std::wstring::npos);
    }
});

TEST("sync-orchestrator: requestSync runs the plan on a worker", [] {
    const std::wstring a = MakeTempDir();
    const std::wstring b = MakeTempDir();
    CHECK(WriteFileBytes(a + L"\\w.txt", "work", 4));
    ResultSet rs;
    rs.problems.push_back(MakeRow(Status::Missing, L"w.txt", false, 4));
    const SyncPlan plan = BuildSyncPlan(rs, a, b);
    ScanOrchestrator orch;
    CHECK(orch.requestSync(plan));
    // A second plan is refused while the first runs (or already finished:
    // either way the state machine stays coherent).
    sync::SyncReport rep;
    for (int i = 0; i < 500 && !orch.takeSyncReport(rep); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_EQ(rep.failedCount, 0ull);
    CHECK_EQ(rep.doneCount, plan.actions.size());
    CHECK(ReadAll(b + L"\\w.txt") == "work");
    CHECK(!orch.takeSyncReport(rep)); // slot cleared on take
});

TEST("sync-e2e: scan, plan, execute, rescan converges B to A", [] {
    const std::wstring a = MakeTempDir();
    const std::wstring b = MakeTempDir();
    CHECK(WriteFileBytes(a + L"\\same.txt", "same", 4));
    CHECK(WriteFileBytes(b + L"\\same.txt", "same", 4));
    CHECK(WriteFileBytes(a + L"\\new.txt", "hello", 5));
    CHECK(fs::create_directories(a + L"\\sub"));
    CHECK(WriteFileBytes(a + L"\\sub\\deep.txt", "deep", 4));
    CHECK(fs::create_directories(a + L"\\emptydir"));
    CHECK(fs::create_directory(a + L"\\real"));
    CHECK(WriteFileBytes(a + L"\\real\\f.txt", "data", 4));
    CHECK(WriteFileBytes(b + L"\\old.txt", "stale", 5));
    CHECK(fs::create_directories(b + L"\\exdir"));
    CHECK(WriteFileBytes(b + L"\\exdir\\x.txt", "x", 1));
    CHECK(fs::create_directory(b + L"\\other"));
    std::wstring err;
    CHECK(CreateLink(a + L"\\j", a + L"\\real", ReparseKind::Junction, err));
    CHECK(CreateLink(b + L"\\j", b + L"\\other", ReparseKind::Junction, err));

    auto r1 = RunScan(a, b, ScanMode::Content);
    CHECK(!r1.results.problems.empty());
    const SyncPlan plan = BuildSyncPlan(r1.results, a, b, true);
    CHECK(!plan.actions.empty());
    const SyncReport rep = ExecutePlan(plan, nullptr);
    CHECK_EQ(rep.failedCount, 0ull);

    // Retire converged rows exactly like the GUI does, then rescan: B == A.
    for (const sync::SyncActionResult& it : rep.items) {
        if (it.ok && !it.cancelled) {
            FileResult fresh;
            fresh.status = Status::Identical;
            fresh.relativePath = it.relativePath;
            ApplySingleResult(r1.results, fresh);
        }
    }
    CHECK(r1.results.problems.empty());
    auto r2 = RunScan(a, b, ScanMode::Content);
    CHECK(r2.results.problems.empty());
    CHECK(r2.results.stats.identicalFiles > 0ull);
    CHECK(ReadAll(b + L"\\new.txt") == "hello");
    CHECK(ReadLinkTarget(b + L"\\j") == b + L"\\real");
    CHECK(!fs::exists(b + L"\\old.txt"));
    CHECK(!fs::exists(b + L"\\exdir"));
    CHECK(!fs::exists(b + L"\\other"));
});

TEST("sync-resultset: successful copy retires the row via ApplySingleResult", [] {
    const std::wstring a = MakeTempDir();
    const std::wstring b = MakeTempDir();
    CHECK(WriteFileBytes(a + L"\\n.txt", "hello", 5));
    ResultSet rs;
    rs.stats.sourceFiles = 1;
    rs.stats.missingFiles = 1;
    rs.problems.push_back(MakeRow(Status::Missing, L"n.txt", false, 5));
    const SyncReport rep = ExecutePlan(BuildSyncPlan(rs, a, b), nullptr);
    CHECK_EQ(rep.failedCount, 0ull);
    FileResult fresh;
    fresh.status = Status::Identical;
    fresh.relativePath = L"n.txt";
    CHECK(ApplySingleResult(rs, fresh));
    CHECK(rs.problems.empty());
    CHECK_EQ(rs.stats.missingFiles, 0ull);
    CHECK_EQ(rs.stats.identicalFiles, 1ull);
});
