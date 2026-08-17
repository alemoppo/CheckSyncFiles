#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bv {
namespace profiling {

// ---------------------------------------------------------------------------
// High-resolution timing over QueryPerformanceCounter. The frequency is cached
// on first use, so QpcToSeconds() is a single division.
// ---------------------------------------------------------------------------
uint64_t QpcNow();
double QpcFrequency();
inline double QpcToSeconds(uint64_t ticks) {
    return static_cast<double>(ticks) / QpcFrequency();
}

// The side a hashed file belongs to: A = source, B = destination.
enum class Side : uint8_t { Source = 0, Dest = 1 };
inline const char* SideName(Side s) { return s == Side::Source ? "A" : "B"; }

// Per-file read/hash timing snapshot filled by hashing::Sha256File() when a
// non-null pointer is passed. All-zero when the file was never read.
struct FileTimings {
    uint64_t readTicks = 0;   // cumulative QPC ticks inside ReadFile()
    uint64_t hashTicks = 0;   // cumulative QPC ticks inside BCryptHashData()
    uint64_t totalTicks = 0;  // whole Sha256File() span (open -> close)
    uint64_t bytesRead = 0;   // total bytes actually read
};

class HashProfiler;
struct HashSession;

// One hashed side of one candidate, retained only when per-job logging is
// enabled (HashProfiler::verboseJobs). One record per file side, never per
// ReadFile.
struct JobRecord {
    uint64_t jobId = 0;
    Side side = Side::Source;
    std::wstring path;
    uint64_t startTick = 0;    // QPC when this side's Sha256File started
    uint64_t endTick = 0;      // QPC when this side's Sha256File finished
    uint64_t readTicks = 0;
    uint64_t hashTicks = 0;
    uint64_t totalTicks = 0;
    uint64_t bytesRead = 0;
    uint64_t expectedSize = 0;
    bool ok = false;
};

// Aggregate counters for one side (A or B).
struct SideAggregate {
    uint64_t files = 0;       // sides actually read (Sha256File invoked)
    uint64_t bytes = 0;       // bytes actually read
    uint64_t failed = 0;      // sides that did not yield a digest
    uint64_t readTicks = 0;   // total ReadFile time
    uint64_t hashTicks = 0;   // total BCryptHashData time
    uint64_t totalTicks = 0;  // total Sha256File() span time
    // Cost of the T1/T2 change-during-scan control: the StatFile() calls
    // (each opens its own handle and issues GetFileInformationByHandle) taken
    // before hashing (T1) and after hashing (T2).
    uint64_t statT1Ticks = 0; // cumulative StatFile() time before hashing (T1)
    uint64_t statT1Count = 0; // number of T1 StatFile() calls
    uint64_t statT2Ticks = 0; // cumulative StatFile() time after hashing (T2)
    uint64_t statT2Count = 0; // number of T2 StatFile() calls
};

// Final aggregate report, filled by HashProfiler::Finalize().
struct HashProfileReport {
    uint64_t tasks = 0;         // candidate tasks that started in the pool
    uint64_t taskFailed = 0;    // tasks that threw
    uint64_t activeJobsAtEnd = 0; // live task counter when the report is built (must be 0)
    SideAggregate side[2];
    uint64_t maxActiveJobs = 0;  // max concurrent hash tasks
    uint64_t maxActiveA = 0;
    uint64_t maxActiveB = 0;
    uint64_t maxAB = 0;          // max simultaneous A + B side hashes
    double activeSecondsA = 0.0; // union length of the A-side intervals
    double activeSecondsB = 0.0;
    double overlapSeconds = 0.0; // wall time with >=1 A and >=1 B active
    // Pool / backpressure metrics merged from the worker pools by the caller.
    uint64_t backpressureWaits = 0;
    double backpressureWaitSeconds = 0.0;
    uint64_t waitAllCount = 0;
    double waitAllSeconds = 0.0;
    uint64_t maxOutstandingTasks = 0;
    uint64_t maxQueueDepth = 0;
};

// Thread-safe accumulator for content-hash profiling. One instance per scan,
// owned by the caller (the CLI passes it through ScanOptions::hashProfiler).
// Disabled by default: every method early-outs unless setEnabled(true) was
// called, so an idle profiler adds no measurable work to a normal scan.
class HashProfiler {
public:
    explicit HashProfiler(bool verboseJobs = false) : verbose_(verboseJobs) {}

    void setEnabled(bool e) { enabled_.store(e, std::memory_order_relaxed); }
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

    // --- task lifecycle (one candidate, runs on a pool worker) ---
    void TaskBegin(HashSession& s);
    void TaskEnd(HashSession& s);
    void TaskFailed();

    // --- per-side file hash lifecycle (wraps one Sha256File call) ---
    void FileBegin(HashSession& s, Side side, const std::wstring& path, uint64_t expectedSize);
    void FileEnd(HashSession& s, Side side, const std::wstring& path, uint64_t expectedSize,
                 const FileTimings& t, bool ok);

    // --- T1/T2 change-during-scan control cost (the StatFile() calls) ---
    void NoteStatBefore(Side side, uint64_t ticks); // T1: StatFile() before hashing
    void NoteStatAfter(Side side, uint64_t ticks);  // T2: StatFile() after hashing

    // Merges ThreadPool metrics into the report (called once per pool used by
    // the scan; never alters synchronization).
    void MergePool(uint64_t maxOutstanding, uint64_t maxQueueDepth, uint64_t backpressureWaits,
                   uint64_t backpressureWaitTicks, uint64_t waitAllCount,
                   uint64_t waitAllTicks);

    void Finalize(HashProfileReport& out) const;

    // Verbose per-side records (only when constructed with verboseJobs = true).
    const std::vector<JobRecord>& jobRecords() const { return records_; }

private:
    static void UpdateMax(std::atomic<uint64_t>& m, uint64_t v);
    // Union of intervals on one timeline (exact total length of the union).
    class IntervalUnion {
    public:
        void insert(uint64_t start, uint64_t end);
        uint64_t length() const;
    private:
        mutable std::mutex mutex_;
        std::map<uint64_t, uint64_t> segs_; // disjoint [start, end)
        uint64_t total_ = 0;
    };

    std::atomic<bool> enabled_{false};
    bool verbose_ = false;

    // Concurrency counters.
    std::atomic<uint64_t> tasks_{0};
    std::atomic<uint64_t> taskFailed_{0};
    std::atomic<uint64_t> activeJobs_{0};
    std::atomic<uint64_t> maxActiveJobs_{0};
    std::atomic<uint64_t> activeA_{0};
    std::atomic<uint64_t> activeB_{0};
    std::atomic<uint64_t> maxActiveA_{0};
    std::atomic<uint64_t> maxActiveB_{0};
    std::atomic<uint64_t> maxAB_{0};
    std::atomic<uint64_t> nextJobId_{0};

    // Aggregate per-side counters (indexed by Side).
    std::atomic<uint64_t> sideFiles_[2]{0, 0};
    std::atomic<uint64_t> sideBytes_[2]{0, 0};
    std::atomic<uint64_t> sideFailed_[2]{0, 0};
    std::atomic<uint64_t> sideReadTicks_[2]{0, 0};
    std::atomic<uint64_t> sideHashTicks_[2]{0, 0};
    std::atomic<uint64_t> sideTotalTicks_[2]{0, 0};
    std::atomic<uint64_t> sideStatT1Ticks_[2]{0, 0};
    std::atomic<uint64_t> sideStatT1Count_[2]{0, 0};
    std::atomic<uint64_t> sideStatT2Ticks_[2]{0, 0};
    std::atomic<uint64_t> sideStatT2Count_[2]{0, 0};

    // A/B overlap measurement.
    IntervalUnion unionA_;
    IntervalUnion unionB_;
    IntervalUnion unionAB_;

    // Pool metrics (merged by the controller).
    std::atomic<uint64_t> poolMaxOutstanding_{0};
    std::atomic<uint64_t> poolMaxQueue_{0};
    std::atomic<uint64_t> poolBackpressureWaits_{0};
    std::atomic<uint64_t> poolBackpressureWaitTicks_{0};
    std::atomic<uint64_t> poolWaitAllCount_{0};
    std::atomic<uint64_t> poolWaitAllTicks_{0};

    // Verbose records.
    mutable std::mutex recordsMutex_;
    std::vector<JobRecord> records_;
};

// Lightweight per-task context: bundles the profiler and this task's unique job
// id. Created on the worker stack; the destructor always releases the task slot
// (including on the exception path).
struct HashSession {
    explicit HashSession(HashProfiler* p) : prof(p) {
        if (prof) prof->TaskBegin(*this);
    }
    ~HashSession() {
        if (prof) prof->TaskEnd(*this);
    }
    HashSession(const HashSession&) = delete;
    HashSession& operator=(const HashSession&) = delete;

    HashProfiler* prof = nullptr;
    uint64_t jobId = 0;
    uint64_t fileBeginTick = 0; // QPC recorded by the last FileBegin on this task
};

} // namespace profiling
} // namespace bv
