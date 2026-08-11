#pragma once

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
// every task submitted so far has completed; it must NOT be called from inside
// a task (would deadlock).
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
            ++pending_;
        }
        cv_.notify_one();
    }

    // Blocks until all tasks submitted up to now have finished. Safe to call
    // from any external thread (e.g. the GUI / UI thread).
    void waitAll();

    unsigned int threadCount() const { return nThreads_; }

private:
    void workerLoop();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable doneCv_;
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    bool stopping_ = false;
    unsigned int pending_ = 0;
    unsigned int nThreads_;
};

// Picks a sensible default worker count for an IO class. Never exceeds a small
// cap: for network traffic more threads mostly contend on the same NIC.
unsigned int DefaultThreadCount(IoClass cls);

} // namespace bv
