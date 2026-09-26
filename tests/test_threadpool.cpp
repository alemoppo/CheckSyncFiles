// ThreadPool tests (split out of the former monolithic test_main.cpp).

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
