#include "Threading/ThreadPool.h"

#include <algorithm>

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
    std::unique_lock<std::mutex> lk(mutex_);
    doneCv_.wait(lk, [&] { return pending_ == 0 && tasks_.empty(); });
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
            task();
        } catch (...) {
            // Never let an exception escape a worker thread.
        }
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (pending_ > 0) --pending_;
            if (pending_ == 0) doneCv_.notify_all();
        }
    }
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
