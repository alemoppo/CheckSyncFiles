#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "IoClass.h"

namespace bv {

// A simple fixed-size pool of worker threads that pull tasks from a shared
// queue. There is NEVER a thread per file: workers stay alive and serve any
// submitted task. Tasks are type-erased callables (std::function<void()>).
//
// Thread-safety: submit() may be called from any thread. waitAll() blocks until
// every task submitted before the call has completed; it must NOT be called
// from inside a task (would deadlock).
//
// Completion tracking uses a monotonic generation counter: submit() bumps
// `issued_`, a worker bumps `completed_` after each task finishes, and waitAll()
// snapshots `issued_` at entry and waits until `completed_` reaches it. Both
// are only touched under `mutex_`, so a submit racing a worker's completion can
// never make waitAll() return while a task is still queued or running -- and
// several threads may call waitAll() concurrently, each waiting for its own
// snapshot. A task that throws is counted by taskErrors() (the caller can warn)
// instead of escaping the worker.
//
// The destructor joins all workers, finishing any queued/running task. Tasks
// are never interrupted between submit and the thread shutdown.
class ThreadPool {
public:
    // nThreads == 0 is allowed and means "no threads": submit() then runs the
    // task synchronously on the calling thread (useful for tests/debugging).
    explicit ThreadPool(unsigned int nThreads);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <typename F>
    void submit(F&& f) {
        if (nThreads_ == 0) {
            f();
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (stopping_) return;
            tasks_.emplace(std::forward<F>(f));
            ++issued_;
            // Profiling-only maxima (relaxed atomics; no behavioural change).
            const uint64_t outstanding = issued_ - completed_;
            const uint64_t running = runningTasks_.load(std::memory_order_relaxed);
            if (outstanding > maxOutstanding_.load(std::memory_order_relaxed))
                maxOutstanding_.store(outstanding, std::memory_order_relaxed);
            const uint64_t queueDepth = outstanding > running ? outstanding - running : 0;
            if (queueDepth > maxQueueDepth_.load(std::memory_order_relaxed))
                maxQueueDepth_.store(queueDepth, std::memory_order_relaxed);
        }
        cv_.notify_one();
    }

    // Blocks until every task submitted before the call has finished. Safe to
    // call from any external thread (e.g. the GUI / UI thread).
    void waitAll();

    // Bounded backpressure for producers that submit in bursts: blocks until
    // at most `maxOutstanding` tasks are submitted-but-not-finished, then
    // returns. This lets a producer submit work and keep going without ever
    // draining the pool (unlike waitAll), while still keeping the internal
    // queue size bounded. Safe to call from any external thread, including
    // several producers at once (each waits for the shared in-flight count to
    // drop); must NOT be called from inside a task (would deadlock).
    void waitOutstandingBelow(uint64_t maxOutstanding);

    unsigned int threadCount() const { return nThreads_; }

    // Number of tasks that threw while running. Exceptions never kill a worker;
    // this counter lets a caller surface the failure (e.g. as a read error in
    // the final report) instead of swallowing it silently.
    uint64_t taskErrors() const { return taskErrors_.load(std::memory_order_relaxed); }

    // Snapshot of the pool's profiling counters. These are passive maxima /
    // blocking gauges recorded without altering the scheduling, backpressure or
    // synchronization semantics; used only to report how the pool actually
    // behaved during a scan.
    struct ThreadPoolMetrics {
        uint64_t maxOutstanding = 0;       // max submitted-but-not-finished tasks
        uint64_t maxQueueDepth = 0;        // max tasks queued (not yet started)
        uint64_t backpressureWaits = 0;    // times waitOutstandingBelow() blocked
        uint64_t backpressureWaitTicks = 0; // QPC ticks spent inside that wait
        uint64_t waitAllCount = 0;          // times waitAll() blocked
        uint64_t waitAllTicks = 0;          // QPC ticks spent inside waitAll()
    };
    ThreadPoolMetrics metrics() const;

private:
    void workerLoop();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable doneCv_;
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    bool stopping_ = false;
    uint64_t issued_ = 0;     // total tasks submitted
    uint64_t completed_ = 0;  // total tasks finished (issued_ - completed_ = in flight)
    std::atomic<uint64_t> taskErrors_{0};
    unsigned int nThreads_;

    // Profiling counters (passive, relaxed atomics; never gate or block).
    std::atomic<uint64_t> runningTasks_{0};       // workers currently executing
    std::atomic<uint64_t> maxOutstanding_{0};     // max submitted-but-not-finished
    std::atomic<uint64_t> maxQueueDepth_{0};      // max queued-but-not-started
    std::atomic<uint64_t> backpressureWaits_{0};
    std::atomic<uint64_t> backpressureWaitTicks_{0};
    std::atomic<uint64_t> waitAllCount_{0};
    std::atomic<uint64_t> waitAllTicks_{0};
};

// Picks a sensible default worker count for an IO class. Never exceeds a small
// cap: for network traffic more threads mostly contend on the same NIC.
unsigned int DefaultThreadCount(IoClass cls);

} // namespace bv
