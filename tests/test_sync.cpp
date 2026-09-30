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
    // Present-side nature, like the real classifiers: Missing rows carry the
    // source side, Extra rows the destination side, mismatches both as files.
    if (st == Status::Missing) {
        r.srcIsDirectory = isDir;
        r.srcReparseKind = kind;
    } else if (st == Status::Extra) {
        r.dstIsDirectory = isDir;
    } else {
        r.srcIsDirectory = isDir;
        r.srcReparseKind = kind;
        r.dstIsDirectory = isDir;
    }
    return r;
}

// Mismatch row with asymmetric sides (file <-> dir, link transitions).
FileResult MakeMismatchRow(const std::wstring& rel, bool srcIsDir, ReparseKind srcKind,
                           bool dstIsDir, ReparseKind dstKind, uint64_t sizeSrc = 0,
                           uint64_t sizeDst = 0,
                           Status st = Status::SizeMismatch) {
    FileResult r;
    r.status = st;
    r.relativePath = rel;
    r.isDirectory = false;
    r.srcIsDirectory = srcIsDir;
    r.srcReparseKind = srcKind;
    r.reparseKind = dstKind;
    r.dstIsDirectory = dstIsDir;
    r.sizeSource = sizeSrc;
    r.sizeDest = sizeDst;
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
    rs.problems.push_back(MakeMismatchRow(L"j", true, ReparseKind::Junction, true,
                                          ReparseKind::Junction, 0, 0,
                                          Status::ContentMismatch));
    rs.problems.push_back(
        MakeRow(Status::Missing, L"weird", false, 0, 0, ReparseKind::Other));
    rs.problems.push_back(MakeRow(Status::ReadError, L"err.txt"));

    const SyncPlan plan = BuildSyncPlan(rs, L"C:\\A", L"C:\\B");
    CHECK_EQ(plan.actions.size(), 6ull);
    // Order: DirCreate, deletes, DirDelete, file writes, link writes.
    CHECK(plan.actions[0].op == SyncOp::DirCreate);
    CHECK(plan.actions[1].op == SyncOp::FileDelete);
    CHECK(plan.actions[2].op == SyncOp::DirDelete);
    CHECK(plan.actions[3].op == SyncOp::FileCopy);
    CHECK(plan.actions[4].op == SyncOp::FileReplace);
    CHECK(plan.actions[5].op == SyncOp::LinkReplace);
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
        MakeMismatchRow(L"j", true, ReparseKind::Junction, true, ReparseKind::Junction,
                        0, 0, Status::ContentMismatch));
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

TEST("sync-exec: failed copy is reported, no target created", [] {
    const std::wstring a = MakeTempDir();
    const std::wstring b = MakeTempDir();
    CHECK(WriteFileBytes(a + L"\\src.txt", "data", 4));
    HANDLE hold = CreateFileW((a + L"\\src.txt").c_str(), GENERIC_READ, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(hold != INVALID_HANDLE_VALUE);
    // Exclusive source lock: CopyFileEx cannot read -> clear error, no target.
    // (Direct copy by design: no temp file exists anymore.)
    std::wstring err;
    const bool ok =
        CopyFileDirect(a, a + L"\\src.txt", b, b + L"\\dst.txt", nullptr, err);
    CloseHandle(hold);
    CHECK(!ok);
    CHECK(!err.empty());
    CHECK(!fs::exists(b + L"\\dst.txt"));
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

namespace {

// File symlink fixture: true when created; on failure `err` tells why (the
// caller skips when it is the documented privilege error).
bool TryFileSymlink(const std::wstring& link, const std::wstring& target,
                    std::wstring& err) {
    return CreateLink(link, target, ReparseKind::SymlinkFile, err);
}

#define SKIP_WITHOUT_PRIVILEGE(err)                                              \
    do {                                                                         \
        CHECK((err).find(L"privilegi") != std::wstring::npos);                    \
        return;                                                                  \
    } while (0)

} // namespace

TEST("sync-path: parent chain rejects intermediate files and links", [] {
    const std::wstring b = MakeTempDir();
    const std::wstring ext = MakeTempDir();
    CHECK(WriteFileBytes(ext + L"\\sentinel.txt", "keep", 4));
    CHECK(fs::create_directories(b + L"\\plain"));
    CHECK(WriteFileBytes(b + L"\\afile.txt", "x", 1));
    std::wstring err;
    CHECK(CreateLink(b + L"\\sub", ext, ReparseKind::Junction, err));
    // Normal chains pass, including absent tails.
    CHECK(CheckParentChain(b, b + L"\\plain\\new\\f.txt").empty());
    CHECK(CheckParentChain(b, b + L"\\fresh\\f.txt").empty());
    // A file in the way is rejected.
    CHECK(!CheckParentChain(b, b + L"\\afile.txt\\f.txt").empty());
    // A junction in the way is rejected (never followed).
    CHECK(!CheckParentChain(b, b + L"\\sub\\file.txt").empty());
    // The leaf itself may be anything: only parents are checked.
    CHECK(CheckParentChain(b, b + L"\\sub").empty());
    // CreateDirAll refuses through the junction but works beside it.
    CHECK(!CreateDirAll(b, b + L"\\sub\\newdir", err));
    CHECK(CreateDirAll(b, b + L"\\plain\\newdir", err));
    CHECK(!fs::exists(ext + L"\\newdir"));
    CHECK(ReadAll(ext + L"\\sentinel.txt") == "keep");
});

TEST("sync-plan: file/dir mismatches plan by source side", [] {
    // A file / B empty dir: clear the dir, then write the file.
    {
        ResultSet rs;
        rs.problems.push_back(MakeMismatchRow(L"foo", false, ReparseKind::None, true,
                                              ReparseKind::None, 10, 0));
        const SyncPlan plan = BuildSyncPlan(rs, L"C:\\A", L"C:\\B");
        CHECK_EQ(plan.actions.size(), 2ull);
        CHECK(plan.actions[0].op == SyncOp::DirDelete);
        CHECK(plan.actions[1].op == SyncOp::FileReplace);
        CHECK_EQ(plan.summary.bytesToCopy, 10ull);
    }
    // A dir / B file: a single DirCreate (the executor clears the live file
    // inline; a separate delete would race the adaptation).
    {
        ResultSet rs;
        rs.problems.push_back(MakeMismatchRow(L"foo", true, ReparseKind::None, false,
                                              ReparseKind::None));
        const SyncPlan plan = BuildSyncPlan(rs, L"C:\\A", L"C:\\B");
        CHECK_EQ(plan.actions.size(), 1ull);
        CHECK(plan.actions[0].op == SyncOp::DirCreate);
    }
    // A file / B supported link: plain replace (executor unlinks).
    {
        ResultSet rs;
        rs.problems.push_back(MakeMismatchRow(L"x", false, ReparseKind::None, false,
                                              ReparseKind::SymlinkFile, 10, 0));
        const SyncPlan plan = BuildSyncPlan(rs, L"C:\\A", L"C:\\B");
        CHECK_EQ(plan.actions.size(), 1ull);
        CHECK(plan.actions[0].op == SyncOp::FileReplace);
    }
    // A link / B file: link replace.
    {
        ResultSet rs;
        rs.problems.push_back(MakeMismatchRow(L"x", false, ReparseKind::SymlinkFile,
                                              false, ReparseKind::None));
        const SyncPlan plan = BuildSyncPlan(rs, L"C:\\A", L"C:\\B");
        CHECK_EQ(plan.actions.size(), 1ull);
        CHECK(plan.actions[0].op == SyncOp::LinkReplace);
    }
    // Unsupported either side: skipped, never executed.
    {
        ResultSet rs;
        rs.problems.push_back(MakeMismatchRow(L"u1", false, ReparseKind::Other, false,
                                              ReparseKind::None));
        rs.problems.push_back(MakeMismatchRow(L"u2", false, ReparseKind::None, false,
                                              ReparseKind::Unknown));
        rs.problems.push_back(
            MakeRow(Status::Missing, L"u3", false, 0, 0, ReparseKind::Unknown));
        const SyncPlan plan = BuildSyncPlan(rs, L"C:\\A", L"C:\\B");
        CHECK(plan.actions.empty());
        CHECK_EQ(plan.skipped.size(), 3ull);
    }
});

TEST("sync-plan: source-claimed ancestors are never scheduled for delete", [] {
    ResultSet rs;
    // B\foo must become a dir (A has a dir): its extra child is deleted
    // individually, but foo itself must survive.
    rs.problems.push_back(MakeMismatchRow(L"foo", true, ReparseKind::None, false,
                                          ReparseKind::None));
    rs.problems.push_back(MakeRow(Status::Extra, L"foo\\bar.txt"));
    const SyncPlan plan = BuildSyncPlan(rs, L"C:\\A", L"C:\\B");
    for (const SyncAction& a : plan.actions) {
        CHECK(!(a.op == SyncOp::DirDelete && a.relativePath == L"foo"));
    }
    bool hasDelete = false, hasCreate = false;
    for (const SyncAction& a : plan.actions) {
        if (a.op == SyncOp::FileDelete && a.relativePath == L"foo\\bar.txt")
            hasDelete = true;
        if (a.op == SyncOp::DirCreate && a.relativePath == L"foo") hasCreate = true;
    }
    CHECK(hasDelete && hasCreate);
});

TEST("sync-exec: A file over B empty and non-empty dir converges", [] {
    for (int variant = 0; variant < 2; ++variant) {
        const std::wstring a = MakeTempDir();
        const std::wstring b = MakeTempDir();
        CHECK(WriteFileBytes(a + L"\\foo", "content", 7));
        CHECK(fs::create_directories(b + L"\\foo"));
        ResultSet rs;
        if (variant == 1) {
            CHECK(WriteFileBytes(b + L"\\foo\\stale.txt", "s", 1));
            rs.problems.push_back(MakeRow(Status::Extra, L"foo\\stale.txt"));
        }
        rs.problems.push_back(MakeMismatchRow(L"foo", false, ReparseKind::None, true,
                                              ReparseKind::None, 7, 0));
        const SyncReport rep = ExecutePlan(BuildSyncPlan(rs, a, b), nullptr);
        CHECK_EQ(rep.failedCount, 0ull);
        CHECK(ReadAll(b + L"\\foo") == "content");
        CHECK(!fs::is_directory(b + L"\\foo"));
    }
});

TEST("sync-exec: A dir over B file converges with children", [] {
    const std::wstring a = MakeTempDir();
    const std::wstring b = MakeTempDir();
    CHECK(fs::create_directories(a + L"\\foo"));
    CHECK(WriteFileBytes(a + L"\\foo\\child.txt", "child", 5));
    CHECK(WriteFileBytes(b + L"\\foo", "blocker", 7));
    ResultSet rs;
    rs.problems.push_back(MakeRow(Status::Missing, L"foo\\child.txt", false, 5));
    rs.problems.push_back(MakeMismatchRow(L"foo", true, ReparseKind::None, false,
                                          ReparseKind::None, 0, 7));
    const SyncReport rep = ExecutePlan(BuildSyncPlan(rs, a, b), nullptr);
    CHECK_EQ(rep.failedCount, 0ull);
    CHECK(fs::is_directory(b + L"\\foo"));
    CHECK(ReadAll(b + L"\\foo\\child.txt") == "child");
});

TEST("sync-exec: file/symlink transitions converge (or skip without rights)", [] {
    // A file / B symlink -> plain file.
    {
        const std::wstring a = MakeTempDir();
        const std::wstring b = MakeTempDir();
        CHECK(WriteFileBytes(a + L"\\v.txt", "v", 1));
        CHECK(WriteFileBytes(b + L"\\real.txt", "r", 1));
        std::wstring linkErr;
        if (!TryFileSymlink(b + L"\\alias", b + L"\\real.txt", linkErr)) {
            SKIP_WITHOUT_PRIVILEGE(linkErr);
        }
        CHECK(WriteFileBytes(a + L"\\alias", "plain", 5));
        ResultSet rs;
        rs.problems.push_back(MakeMismatchRow(L"alias", false, ReparseKind::None, false,
                                              ReparseKind::SymlinkFile, 5, 1));
        const SyncReport rep = ExecutePlan(BuildSyncPlan(rs, a, b), nullptr);
        CHECK_EQ(rep.failedCount, 0ull);
        CHECK(GetReparseKind(b + L"\\alias", false) == ReparseKind::None);
        CHECK(ReadAll(b + L"\\alias") == "plain");
    }
    // A symlink / B file -> link (rebased).
    {
        const std::wstring a = MakeTempDir();
        const std::wstring b = MakeTempDir();
        CHECK(WriteFileBytes(a + L"\\target.txt", "t", 1));
        std::wstring linkErr;
        if (!TryFileSymlink(a + L"\\alias", a + L"\\target.txt", linkErr)) {
            SKIP_WITHOUT_PRIVILEGE(linkErr);
        }
        CHECK(WriteFileBytes(b + L"\\alias", "plain", 5));
        ResultSet rs;
        rs.problems.push_back(MakeMismatchRow(L"alias", false, ReparseKind::SymlinkFile,
                                              false, ReparseKind::None));
        const SyncReport rep = ExecutePlan(BuildSyncPlan(rs, a, b), nullptr);
        CHECK_EQ(rep.failedCount, 0ull);
        CHECK(GetReparseKind(b + L"\\alias", false) == ReparseKind::SymlinkFile);
        CHECK(ReadLinkTarget(b + L"\\alias") == b + L"\\target.txt");
    }
    // Symlink retarget -> link replace.
    {
        const std::wstring a = MakeTempDir();
        const std::wstring b = MakeTempDir();
        CHECK(WriteFileBytes(a + L"\\one.txt", "1", 1));
        CHECK(WriteFileBytes(a + L"\\two.txt", "2", 1));
        CHECK(WriteFileBytes(b + L"\\one.txt", "1", 1));
        std::wstring linkErr;
        if (!TryFileSymlink(a + L"\\alias", a + L"\\one.txt", linkErr)) {
            SKIP_WITHOUT_PRIVILEGE(linkErr);
        }
        if (!TryFileSymlink(b + L"\\alias", b + L"\\two.txt", linkErr)) {
            SKIP_WITHOUT_PRIVILEGE(linkErr);
        }
        ResultSet rs;
        rs.problems.push_back(MakeMismatchRow(L"alias", false, ReparseKind::SymlinkFile,
                                              false, ReparseKind::SymlinkFile, 1, 1,
                                              Status::ContentMismatch));
        const SyncReport rep = ExecutePlan(BuildSyncPlan(rs, a, b), nullptr);
        CHECK_EQ(rep.failedCount, 0ull);
        CHECK(ReadLinkTarget(b + L"\\alias") == b + L"\\one.txt");
    }
});

TEST("sync-exec: extra symlink copies B->A as a link", [] {
    const std::wstring a = MakeTempDir();
    const std::wstring b = MakeTempDir();
    CHECK(WriteFileBytes(b + L"\\real.txt", "r", 1));
    std::wstring linkErr;
    if (!TryFileSymlink(b + L"\\alias", b + L"\\real.txt", linkErr)) {
        SKIP_WITHOUT_PRIVILEGE(linkErr);
    }
    FileResult row;
    row.status = Status::Extra;
    row.relativePath = L"alias";
    row.reparseKind = ReparseKind::SymlinkFile;
    row.dstIsDirectory = false;
    const SyncPlan plan =
        BuildSingleActionPlan(row, ManualOp::CopyToDst, a, b, /*toA=*/true);
    CHECK_EQ(plan.actions.size(), 1ull);
    CHECK(plan.actions[0].op == SyncOp::LinkCreate);
    const SyncReport rep = ExecutePlan(plan, nullptr);
    CHECK_EQ(rep.failedCount, 0ull);
    CHECK(GetReparseKind(a + L"\\alias", false) == ReparseKind::SymlinkFile);
    CHECK(ReadLinkTarget(a + L"\\alias") == a + L"\\real.txt");
});

TEST("sync-exec: write through a dest junction is refused, outside untouched", [] {
    const std::wstring a = MakeTempDir();
    const std::wstring b = MakeTempDir();
    const std::wstring ext = MakeTempDir();
    CHECK(WriteFileBytes(a + L"\\payload.txt", "payload", 7));
    CHECK(WriteFileBytes(ext + L"\\sentinel.txt", "keep", 4));
    std::wstring err;
    CHECK(CreateLink(b + L"\\sub", ext, ReparseKind::Junction, err));
    ResultSet rs;
    rs.problems.push_back(MakeRow(Status::Missing, L"sub\\file.txt", false, 7));
    // NOTE: a real scan would never emit this row (the enumerator does not
    // descend into junctions); it simulates a stale/forged row to prove the
    // executor refuses instead of writing outside B.
    const SyncReport rep = ExecutePlan(BuildSyncPlan(rs, a, b), nullptr);
    CHECK_EQ(rep.failedCount, 1ull);
    CHECK(!fs::exists(ext + L"\\file.txt"));
    CHECK(ReadAll(ext + L"\\sentinel.txt") == "keep");
    CHECK(!fs::exists(ext + L"\\payload.txt"));
});

TEST("sync-resultset: directory counters retire correctly", [] {
    // Missing dir -> created: missingDirs down, identicalDirs up.
    {
        ResultSet rs;
        FileResult row;
        row.status = Status::Missing;
        row.relativePath = L"d";
        row.isDirectory = true;
        row.srcIsDirectory = true;
        rs.problems.push_back(row);
        rs.stats.missingDirs = 1;
        FileResult fresh;
        fresh.status = Status::Identical;
        fresh.relativePath = L"d";
        CHECK(ApplySingleResult(rs, fresh));
        CHECK(rs.problems.empty());
        CHECK_EQ(rs.stats.missingDirs, 0ull);
        CHECK_EQ(rs.stats.identicalDirs, 1ull);
        CHECK_EQ(rs.stats.identicalFiles, 0ull);
    }
    // Extra dir -> deleted: extraDirs down, NO identical bump (absent now).
    {
        ResultSet rs;
        FileResult row;
        row.status = Status::Extra;
        row.relativePath = L"d";
        row.isDirectory = true;
        row.dstIsDirectory = true;
        rs.problems.push_back(row);
        rs.stats.extraDirs = 1;
        FileResult fresh;
        fresh.status = Status::Identical;
        fresh.relativePath = L"d";
        CHECK(ApplySingleResult(rs, fresh));
        CHECK(rs.problems.empty());
        CHECK_EQ(rs.stats.extraDirs, 0ull);
        CHECK_EQ(rs.stats.identicalDirs, 0ull);
    }
    // Extra file -> deleted: extraFiles down, identicalFiles untouched.
    {
        ResultSet rs;
        FileResult row;
        row.status = Status::Extra;
        row.relativePath = L"f";
        rs.problems.push_back(row);
        rs.stats.extraFiles = 1;
        FileResult fresh;
        fresh.status = Status::Identical;
        fresh.relativePath = L"f";
        CHECK(ApplySingleResult(rs, fresh));
        CHECK(rs.problems.empty());
        CHECK_EQ(rs.stats.extraFiles, 0ull);
        CHECK_EQ(rs.stats.identicalFiles, 0ull);
    }
});
