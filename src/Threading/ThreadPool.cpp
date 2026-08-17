#include "Threading/ThreadPool.h"

#include <algorithm>

#include "Profiling/HashProfile.h"

namespace bv {

ThreadPool::ThreadPool(unsigned int nThreads) : nThreads_(nThreads) {
    workers_.reserve(nThreads_);
    for (unsigned int i = 0; i < nThreads_; ++i) {
        workers_.emplace_back(std::thread(&ThreadPool::workerLoop, this));
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (std::thread& t : workers_) {
        if (t.joinable()) t.join();
    }
}

void ThreadPool::waitAll() {
    if (nThreads_ == 0) return;
    const uint64_t t0 = profiling::QpcNow();
    bool blocked = false;
    std::unique_lock<std::mutex> lk(mutex_);
    const uint64_t target = issued_; // only tasks submitted before this call
    doneCv_.wait(lk, [&] {
        if (completed_ >= target) return true;
        if (!blocked) {
            blocked = true;
            waitAllCount_.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    });
    if (blocked) waitAllTicks_.fetch_add(profiling::QpcNow() - t0, std::memory_order_relaxed);
}

void ThreadPool::waitOutstandingBelow(uint64_t maxOutstanding) {
    if (nThreads_ == 0) return;
    const uint64_t t0 = profiling::QpcNow();
    bool blocked = false;
    std::unique_lock<std::mutex> lk(mutex_);
    {
        const uint64_t outstanding = issued_ - completed_;
        if (outstanding > maxOutstanding_.load(std::memory_order_relaxed))
            maxOutstanding_.store(outstanding, std::memory_order_relaxed);
    }
    doneCv_.wait(lk, [&] {
        if ((issued_ - completed_) <= maxOutstanding) return true;
        if (!blocked) {
            blocked = true;
            backpressureWaits_.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    });
    if (blocked)
        backpressureWaitTicks_.fetch_add(profiling::QpcNow() - t0, std::memory_order_relaxed);
}

void ThreadPool::workerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [&] { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty() && stopping_) return;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        try {
            runningTasks_.fetch_add(1, std::memory_order_relaxed);
            task();
        } catch (...) {
            // Never let an exception escape a worker; record it so the caller
            // can warn instead of the failure being invisible.
            taskErrors_.fetch_add(1, std::memory_order_relaxed);
        }
        runningTasks_.fetch_sub(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            ++completed_;
            doneCv_.notify_all();
        }
    }
}

ThreadPool::ThreadPoolMetrics ThreadPool::metrics() const {
    ThreadPoolMetrics m;
    m.maxOutstanding = maxOutstanding_.load(std::memory_order_relaxed);
    m.maxQueueDepth = maxQueueDepth_.load(std::memory_order_relaxed);
    m.backpressureWaits = backpressureWaits_.load(std::memory_order_relaxed);
    m.backpressureWaitTicks = backpressureWaitTicks_.load(std::memory_order_relaxed);
    m.waitAllCount = waitAllCount_.load(std::memory_order_relaxed);
    m.waitAllTicks = waitAllTicks_.load(std::memory_order_relaxed);
    return m;
}

unsigned int DefaultThreadCount(IoClass cls) {
    unsigned int cores = std::thread::hardware_concurrency();
    if (cores == 0) cores = 4;

    switch (cls) {
        case IoClass::LocalLocal:
            // Disk-bound: a few workers saturate a HDD; more contend.
            return std::min(cores, 8u);
        case IoClass::LocalNetwork:
        case IoClass::NetworkNetwork:
            // Network-bound: more workers just contend on the same NIC.
            return std::min(cores, 4u);
    }
    return 4;
}

} // namespace bv
