#include "Profiling/HashProfile.h"

#include <algorithm>

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

void HashProfiler::TaskEnd(HashSession& s) {
    if (!enabled()) return;
    activeJobs_.fetch_sub(1, std::memory_order_relaxed);
    if (!s.fullJob) return;

    // Job execution interval: [startTick, endTick] on the executing worker.
    // queueWait = startTick - enqueueTick lies OUTSIDE that interval (it
    // precedes the worker picking the task up). Both are monosource reads of
    // the session fields that only this worker wrote.
    const int i = static_cast<int>(s.side);
    uint64_t exec = 0;
    if (s.endTick > 0 && s.endTick >= s.startTick) exec = s.endTick - s.startTick;
    const uint64_t qw = s.startTick >= s.enqueueTick ? s.startTick - s.enqueueTick : 0;

    JobAggAccum& g = jobAgg_[i];
    g.jobs.fetch_add(1, std::memory_order_relaxed);
    g.bytes.fetch_add(s.sizeSource + s.sizeDest, std::memory_order_relaxed);
    g.queueWaitTicks.fetch_add(qw, std::memory_order_relaxed);
    UpdateMax(g.queueWaitMax, qw);
    g.execTicks.fetch_add(exec, std::memory_order_relaxed);
    UpdateMax(g.execMax, exec);
    g.readTicks.fetch_add(s.jobReadTicks, std::memory_order_relaxed);
    g.hashTicks.fetch_add(s.jobHashTicks, std::memory_order_relaxed);
    g.statTicks.fetch_add(s.jobStatTicks, std::memory_order_relaxed);
    g.cacheTicks.fetch_add(s.jobCacheTicks, std::memory_order_relaxed);
    switch (s.verdict) {
        case JobVerdict::Identical:
            g.vIdentical.fetch_add(1, std::memory_order_relaxed);
            break;
        case JobVerdict::ContentMismatch:
            g.vMismatch.fetch_add(1, std::memory_order_relaxed);
            break;
        case JobVerdict::ChangedDuringScan:
            g.vChanged.fetch_add(1, std::memory_order_relaxed);
            break;
        case JobVerdict::ReadError:
            g.vReadError.fetch_add(1, std::memory_order_relaxed);
            break;
        case JobVerdict::AccessDenied:
            g.vAccessDenied.fetch_add(1, std::memory_order_relaxed);
            break;
        case JobVerdict::Cancelled:
            g.vCancelled.fetch_add(1, std::memory_order_relaxed);
            break;
    }
    if (exec > 0) RecordTop(i, s);
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
    // Fold the file-side read/hash time into the owning job's accumulators
    // (per-submitter-side); on this worker, inside [start, end).
    if (s.fullJob) {
        s.jobReadTicks += t.readTicks;
        s.jobHashTicks += t.hashTicks;
    }

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

void HashProfiler::NoteStatBefore(HashSession& s, Side side, uint64_t ticks) {
    if (!enabled()) return;
    const int i = static_cast<int>(side);
    sideStatT1Ticks_[i].fetch_add(ticks, std::memory_order_relaxed);
    sideStatT1Count_[i].fetch_add(1, std::memory_order_relaxed);
    s.jobStatTicks += ticks;
}

void HashProfiler::NoteStatAfter(HashSession& s, Side side, uint64_t ticks) {
    if (!enabled()) return;
    const int i = static_cast<int>(side);
    sideStatT2Ticks_[i].fetch_add(ticks, std::memory_order_relaxed);
    sideStatT2Count_[i].fetch_add(1, std::memory_order_relaxed);
    s.jobStatTicks += ticks;
}

void HashProfiler::NoteCacheLookup(HashSession& s, Side side, uint64_t ticks) {
    if (!enabled()) return;
    const int i = static_cast<int>(side);
    sideCacheTicks_[i].fetch_add(ticks, std::memory_order_relaxed);
    sideCacheLookups_[i].fetch_add(1, std::memory_order_relaxed);
    s.jobCacheTicks += ticks;
}

void HashProfiler::NoteEmit(Side side, uint64_t totalTicks, uint64_t backpressureTicks) {
    if (!enabled()) return;
    const int i = static_cast<int>(side);
    emitTicks_[i].fetch_add(totalTicks, std::memory_order_relaxed);
    emitBackpressureTicks_[i].fetch_add(backpressureTicks, std::memory_order_relaxed);
}

void HashProfiler::MergePool(uint64_t maxOutstanding, uint64_t maxQueueDepth,
                             uint64_t backpressureWaits, uint64_t backpressureWaitTicks,
                             uint64_t waitAllCount, uint64_t waitAllTicks, uint64_t busyTicks,
                             uint64_t maxActiveWorkers, uint64_t wallTicks, uint64_t submitted,
                             uint64_t completed) {
    if (!enabled()) return;
    if (maxOutstanding > poolMaxOutstanding_.load(std::memory_order_relaxed))
        poolMaxOutstanding_.store(maxOutstanding, std::memory_order_relaxed);
    if (maxQueueDepth > poolMaxQueue_.load(std::memory_order_relaxed))
        poolMaxQueue_.store(maxQueueDepth, std::memory_order_relaxed);
    poolBackpressureWaits_.fetch_add(backpressureWaits, std::memory_order_relaxed);
    poolBackpressureWaitTicks_.fetch_add(backpressureWaitTicks, std::memory_order_relaxed);
    poolWaitAllCount_.fetch_add(waitAllCount, std::memory_order_relaxed);
    poolWaitAllTicks_.fetch_add(waitAllTicks, std::memory_order_relaxed);
    // Pools run sequentially in a scan (source capture, then the compare pass),
    // so summing their wall/busy/submitted/completed is representative; the
    // maximum active workers is kept as a max.
    poolBusyTicks_.fetch_add(busyTicks, std::memory_order_relaxed);
    if (maxActiveWorkers > poolMaxActive_.load(std::memory_order_relaxed))
        poolMaxActive_.store(maxActiveWorkers, std::memory_order_relaxed);
    poolWallTicks_.fetch_add(wallTicks, std::memory_order_relaxed);
    poolSubmitted_.fetch_add(submitted, std::memory_order_relaxed);
    poolCompleted_.fetch_add(completed, std::memory_order_relaxed);
}

void HashProfiler::RecordTop(int side, const HashSession& s) {
    const uint64_t exec = s.endTick - s.startTick;
    // Lock-free gate: skip without a lock when the job cannot beat the current
    // tenth entry. gate only grows (it replaces the tenth entry only with exec
    // >= the previous tenth), so a skip here can never drop an entrant.
    if (const uint64_t gate = topTen_[side].gate.load(std::memory_order_relaxed);
        gate != 0 && exec <= gate)
        return;
    std::lock_guard<std::mutex> lk(topTen_[side].mutex);
    std::vector<TopJob>& v = topTen_[side].v;
    if (v.size() == 10 && exec <= v.back().execTicks) return; // re-check under lock

    TopJob t;
    t.execTicks = exec;
    t.queueWaitTicks = s.startTick - s.enqueueTick;
    t.readTicks = s.jobReadTicks;
    t.hashTicks = s.jobHashTicks;
    t.statTicks = s.jobStatTicks;
    t.cacheTicks = s.jobCacheTicks;
    t.sizeSource = s.sizeSource;
    t.sizeDest = s.sizeDest;
    t.verdict = s.verdict;
    t.path = s.relPath;
    if (v.size() == 10) {
        v.back() = std::move(t);
    } else {
        v.push_back(std::move(t));
    }
    std::sort(v.begin(), v.end(),
              [](const TopJob& a, const TopJob& b) { return a.execTicks > b.execTicks; });
    if (v.size() > 10) v.resize(10);
    // gate only meaningful once the list is full (it must be 0 while size < 10
    // so nothing can be skipped until all 10 slots are occupied).
    topTen_[side].gate.store(v.size() == 10 ? v.back().execTicks : 0,
                             std::memory_order_relaxed);
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
        a.cacheTicks = sideCacheTicks_[i].load(std::memory_order_relaxed);
        a.cacheLookups = sideCacheLookups_[i].load(std::memory_order_relaxed);
        out.emitTicks[i] = emitTicks_[i].load(std::memory_order_relaxed);
        out.emitBackpressureTicks[i] = emitBackpressureTicks_[i].load(std::memory_order_relaxed);

        const JobAggAccum& g = jobAgg_[i];
        JobAggregate& j = out.jobs[i];
        j.jobs = g.jobs.load(std::memory_order_relaxed);
        j.bytes = g.bytes.load(std::memory_order_relaxed);
        j.queueWaitTicks = g.queueWaitTicks.load(std::memory_order_relaxed);
        j.queueWaitMax = g.queueWaitMax.load(std::memory_order_relaxed);
        j.execTicks = g.execTicks.load(std::memory_order_relaxed);
        j.execMax = g.execMax.load(std::memory_order_relaxed);
        j.readTicks = g.readTicks.load(std::memory_order_relaxed);
        j.hashTicks = g.hashTicks.load(std::memory_order_relaxed);
        j.statTicks = g.statTicks.load(std::memory_order_relaxed);
        j.cacheTicks = g.cacheTicks.load(std::memory_order_relaxed);
        j.verdictIdentical = g.vIdentical.load(std::memory_order_relaxed);
        j.verdictMismatch = g.vMismatch.load(std::memory_order_relaxed);
        j.verdictChanged = g.vChanged.load(std::memory_order_relaxed);
        j.verdictReadError = g.vReadError.load(std::memory_order_relaxed);
        j.verdictAccessDenied = g.vAccessDenied.load(std::memory_order_relaxed);
        j.verdictCancelled = g.vCancelled.load(std::memory_order_relaxed);

        std::lock_guard<std::mutex> lk(topTen_[i].mutex);
        out.topJobs[i].top = topTen_[i].v;
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
    out.poolBusyTicks = poolBusyTicks_.load(std::memory_order_relaxed);
    out.poolMaxActive = poolMaxActive_.load(std::memory_order_relaxed);
    out.poolWallTicks = poolWallTicks_.load(std::memory_order_relaxed);
    out.poolSubmitted = poolSubmitted_.load(std::memory_order_relaxed);
    out.poolCompleted = poolCompleted_.load(std::memory_order_relaxed);
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