#include "Profiling/HashProfile.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace bv {
namespace profiling {

uint64_t QpcNow() {
    LARGE_INTEGER f;
    QueryPerformanceCounter(&f);
    return static_cast<uint64_t>(f.QuadPart);
}

double QpcFrequency() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    return freq;
}

void HashProfiler::UpdateMax(std::atomic<uint64_t>& m, uint64_t v) {
    uint64_t cur = m.load(std::memory_order_relaxed);
    while (cur < v && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

void HashProfiler::TaskBegin(HashSession& s) {
    if (!enabled()) return;
    s.jobId = nextJobId_.fetch_add(1, std::memory_order_relaxed) + 1;
    tasks_.fetch_add(1, std::memory_order_relaxed);
    const uint64_t cur = activeJobs_.fetch_add(1, std::memory_order_relaxed) + 1;
    UpdateMax(maxActiveJobs_, cur);
}

void HashProfiler::TaskEnd(HashSession&) {
    if (!enabled()) return;
    activeJobs_.fetch_sub(1, std::memory_order_relaxed);
}

void HashProfiler::TaskFailed() {
    if (!enabled()) return;
    taskFailed_.fetch_add(1, std::memory_order_relaxed);
}

void HashProfiler::FileBegin(HashSession& s, Side side, const std::wstring&, uint64_t) {
    if (!enabled()) return;
    s.fileBeginTick = QpcNow();
    const int i = static_cast<int>(side);
    const uint64_t cur =
        (i == 0 ? activeA_ : activeB_).fetch_add(1, std::memory_order_relaxed) + 1;
    UpdateMax(i == 0 ? maxActiveA_ : maxActiveB_, cur);
    const uint64_t ab = activeA_.load(std::memory_order_relaxed) +
                        activeB_.load(std::memory_order_relaxed);
    UpdateMax(maxAB_, ab);
}

void HashProfiler::FileEnd(HashSession& s, Side side, const std::wstring& path,
                           uint64_t expectedSize, const FileTimings& t, bool ok) {
    if (!enabled()) return;
    const int i = static_cast<int>(side);
    (i == 0 ? activeA_ : activeB_).fetch_sub(1, std::memory_order_relaxed);
    sideFiles_[i].fetch_add(1, std::memory_order_relaxed);
    sideBytes_[i].fetch_add(t.bytesRead, std::memory_order_relaxed);
    if (!ok) sideFailed_[i].fetch_add(1, std::memory_order_relaxed);
    sideReadTicks_[i].fetch_add(t.readTicks, std::memory_order_relaxed);
    sideHashTicks_[i].fetch_add(t.hashTicks, std::memory_order_relaxed);
    sideTotalTicks_[i].fetch_add(t.totalTicks, std::memory_order_relaxed);

    const uint64_t start = s.fileBeginTick;
    const uint64_t end = QpcNow();
    if (i == 0) {
        unionA_.insert(start, end);
    } else {
        unionB_.insert(start, end);
    }
    unionAB_.insert(start, end);

    if (verbose_) {
        std::lock_guard<std::mutex> lk(recordsMutex_);
        JobRecord r;
        r.jobId = s.jobId;
        r.side = side;
        r.path = path;
        r.startTick = start;
        r.endTick = end;
        r.readTicks = t.readTicks;
        r.hashTicks = t.hashTicks;
        r.totalTicks = t.totalTicks;
        r.bytesRead = t.bytesRead;
        r.expectedSize = expectedSize;
        r.ok = ok;
        records_.push_back(std::move(r));
    }
}

void HashProfiler::NoteStatBefore(Side side, uint64_t ticks) {
    if (!enabled()) return;
    const int i = static_cast<int>(side);
    sideStatT1Ticks_[i].fetch_add(ticks, std::memory_order_relaxed);
    sideStatT1Count_[i].fetch_add(1, std::memory_order_relaxed);
}

void HashProfiler::NoteStatAfter(Side side, uint64_t ticks) {
    if (!enabled()) return;
    const int i = static_cast<int>(side);
    sideStatT2Ticks_[i].fetch_add(ticks, std::memory_order_relaxed);
    sideStatT2Count_[i].fetch_add(1, std::memory_order_relaxed);
}

void HashProfiler::MergePool(uint64_t maxOutstanding, uint64_t maxQueueDepth,
                             uint64_t backpressureWaits, uint64_t backpressureWaitTicks,
                             uint64_t waitAllCount, uint64_t waitAllTicks) {
    if (!enabled()) return;
    if (maxOutstanding > poolMaxOutstanding_.load(std::memory_order_relaxed))
        poolMaxOutstanding_.store(maxOutstanding, std::memory_order_relaxed);
    if (maxQueueDepth > poolMaxQueue_.load(std::memory_order_relaxed))
        poolMaxQueue_.store(maxQueueDepth, std::memory_order_relaxed);
    poolBackpressureWaits_.fetch_add(backpressureWaits, std::memory_order_relaxed);
    poolBackpressureWaitTicks_.fetch_add(backpressureWaitTicks, std::memory_order_relaxed);
    poolWaitAllCount_.fetch_add(waitAllCount, std::memory_order_relaxed);
    poolWaitAllTicks_.fetch_add(waitAllTicks, std::memory_order_relaxed);
}

void HashProfiler::Finalize(HashProfileReport& out) const {
    out.tasks = tasks_.load(std::memory_order_relaxed);
    out.taskFailed = taskFailed_.load(std::memory_order_relaxed);
    out.activeJobsAtEnd = activeJobs_.load(std::memory_order_relaxed);
    for (int i = 0; i < 2; ++i) {
        SideAggregate& a = out.side[i];
        a.files = sideFiles_[i].load(std::memory_order_relaxed);
        a.bytes = sideBytes_[i].load(std::memory_order_relaxed);
        a.failed = sideFailed_[i].load(std::memory_order_relaxed);
        a.readTicks = sideReadTicks_[i].load(std::memory_order_relaxed);
        a.hashTicks = sideHashTicks_[i].load(std::memory_order_relaxed);
        a.totalTicks = sideTotalTicks_[i].load(std::memory_order_relaxed);
        a.statT1Ticks = sideStatT1Ticks_[i].load(std::memory_order_relaxed);
        a.statT1Count = sideStatT1Count_[i].load(std::memory_order_relaxed);
        a.statT2Ticks = sideStatT2Ticks_[i].load(std::memory_order_relaxed);
        a.statT2Count = sideStatT2Count_[i].load(std::memory_order_relaxed);
    }
    out.maxActiveJobs = maxActiveJobs_.load(std::memory_order_relaxed);
    out.maxActiveA = maxActiveA_.load(std::memory_order_relaxed);
    out.maxActiveB = maxActiveB_.load(std::memory_order_relaxed);
    out.maxAB = maxAB_.load(std::memory_order_relaxed);

    const double secs = 1.0 / QpcFrequency();
    const uint64_t ua = unionA_.length();
    const uint64_t ub = unionB_.length();
    const uint64_t uab = unionAB_.length();
    out.activeSecondsA = static_cast<double>(ua) * secs;
    out.activeSecondsB = static_cast<double>(ub) * secs;
    const double overlap = static_cast<double>(ua) + static_cast<double>(ub) -
                           static_cast<double>(uab);
    out.overlapSeconds = overlap > 0.0 ? overlap * secs : 0.0;

    out.backpressureWaits = poolBackpressureWaits_.load(std::memory_order_relaxed);
    out.backpressureWaitSeconds =
        static_cast<double>(poolBackpressureWaitTicks_.load(std::memory_order_relaxed)) * secs;
    out.waitAllCount = poolWaitAllCount_.load(std::memory_order_relaxed);
    out.waitAllSeconds =
        static_cast<double>(poolWaitAllTicks_.load(std::memory_order_relaxed)) * secs;
    out.maxOutstandingTasks = poolMaxOutstanding_.load(std::memory_order_relaxed);
    out.maxQueueDepth = poolMaxQueue_.load(std::memory_order_relaxed);
}

void HashProfiler::IntervalUnion::insert(uint64_t start, uint64_t end) {
    if (end <= start) return;
    std::lock_guard<std::mutex> lk(mutex_);
    // Merge [start, end) into the disjoint segment map, keeping total_ exact.
    auto it = segs_.lower_bound(start);
    if (it != segs_.begin()) {
        auto prev = std::prev(it);
        if (prev->second >= start) {
            start = prev->first;
            if (prev->second >= end) return; // already fully covered
            total_ -= prev->second - prev->first;
            segs_.erase(prev);
            it = segs_.lower_bound(start);
        }
    }
    uint64_t segEnd = end;
    while (it != segs_.end() && it->first <= segEnd) {
        total_ -= it->second - it->first;
        if (it->second > segEnd) segEnd = it->second;
        it = segs_.erase(it);
    }
    segs_.emplace(start, segEnd);
    total_ += segEnd - start;
}

uint64_t HashProfiler::IntervalUnion::length() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return total_;
}

} // namespace profiling
} // namespace bv
