// Orchestrator, progress and cancel tests
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
