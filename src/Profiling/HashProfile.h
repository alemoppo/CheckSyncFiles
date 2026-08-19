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

// Outcome of a content-comparison job (one candidate pair). A job that bails
// on cancellation is counted as Cancelled; only a pair that produced a real
// verdict ends in Identical / ContentMismatch.
enum class JobVerdict : uint8_t {
    Identical,
    ContentMismatch,
    ChangedDuringScan,
    ReadError,
    AccessDenied,
    Cancelled,
};

// Per-file read/hash timing snapshot filled by hashing::Sha256File() when a
// non-null pointer is passed. All-zero when the file was never read.
struct FileTimings {
    uint64_t readTicks = 0;   // cumulative QPC ticks inside ReadFile()
    uint64_t hashTicks = 0;   // cumulative QPC ticks inside BCryptHashData()
    uint64_t totalTicks = 0;  // whole hashing span (BCrypt open -> BCrypt close)
    uint64_t bytesRead = 0;   // total bytes actually read
};

class HashProfiler;
struct HashSession;

// One hashed side of one candidate, retained only when per-job logging is
// enabled (HashProfiler::verboseJobs). One record per file side, never per
// ReadFile. The span is ONLY the read+hash window (FileBegin..FileEnd); T1/T2
// metadata and cache lookups are accounted separately (SideAggregate fields).
struct JobRecord {
    uint64_t jobId = 0;
    Side side = Side::Source;
    std::wstring path;
    uint64_t startTick = 0;    // QPC when this side's hash span started
    uint64_t endTick = 0;      // QPC when this side's hash span finished
    uint64_t readTicks = 0;
    uint64_t hashTicks = 0;
    uint64_t totalTicks = 0;
    uint64_t bytesRead = 0;
    uint64_t expectedSize = 0;
    bool ok = false;
};

// Aggregate counters for one FILE side (A or B): the side of the file that
// was hashed (source file vs destination file), NOT the side that submitted
// the job. `totalTicks` is the read+hash span only.
struct SideAggregate {
    uint64_t files = 0;       // sides actually read (hash span invoked)
    uint64_t bytes = 0;       // bytes actually read
    uint64_t failed = 0;      // sides that did not yield a digest
    uint64_t readTicks = 0;   // total ReadFile time
    uint64_t hashTicks = 0;   // total BCryptHashData time
    uint64_t totalTicks = 0;  // total hash-span time
    // Cost of the T1/T2 change-during-scan control: the StatFile() calls
    // (each opens its own handle and issues GetFileInformationByHandle) taken
    // before hashing (T1) and after hashing (T2). In the unified single-handle
    // flow these are StatHandle() on the SAME handle (no open).
    uint64_t statT1Ticks = 0; // cumulative StatFile() time before hashing (T1)
    uint64_t statT1Count = 0; // number of T1 StatFile() calls
    uint64_t statT2Ticks = 0; // cumulative StatFile() time after hashing (T2)
    uint64_t statT2Count = 0; // number of T2 StatFile() calls
    // HashCache::Lookup wall time (only when a cache is configured).
    uint64_t cacheTicks = 0;   // cumulative Lookup() time (this file side)
    uint64_t cacheLookups = 0; // number of Lookup() calls
};

// Aggregate over JOBS, attributed to the side that SUBMITTED the job (the
// enumerator worker that flushed the batch -- SubmitHashCandidates is called
// separately from the A walk and the B walk). A job hashes BOTH file sides, so
// read/hash/stat/cache below are the SUM of the source and the destination file
// work of the job. Queue wait and execution time are per-job wall intervals
// (see the report notes for the exact perimeters).
struct JobAggregate {
    uint64_t jobs = 0;          // jobs attributed to this side
    uint64_t bytes = 0;         // expected pair bytes (sizeSource + sizeDest)
    uint64_t queueWaitTicks = 0; // sum of (start - enqueue) over jobs
    uint64_t queueWaitMax = 0;   // max queue wait over jobs
    uint64_t execTicks = 0;     // sum of job execution times
    uint64_t execMax = 0;       // max job execution time
    uint64_t readTicks = 0;     // sum of ReadFile wall time (both file sides)
    uint64_t hashTicks = 0;     // sum of BCryptHashData wall time (both sides)
    uint64_t statTicks = 0;     // sum of T1+T2 (both file sides)
    uint64_t cacheTicks = 0;    // sum of HashCache::Lookup time (both sides)
    uint64_t verdictIdentical = 0;
    uint64_t verdictMismatch = 0;
    uint64_t verdictChanged = 0;
    uint64_t verdictReadError = 0;
    uint64_t verdictAccessDenied = 0;
    uint64_t verdictCancelled = 0;
};

// One entry of a per-side "top most expensive jobs" list. Retained only while
// it sits in the top-10 of a side (memory O(10) per side; the profiler never
// stores every job).
struct TopJob {
    uint64_t execTicks = 0;       // total job execution time (wall)
    uint64_t queueWaitTicks = 0;  // start - enqueue (wall)
    uint64_t readTicks = 0;       // ReadFile wall time (both file sides)
    uint64_t hashTicks = 0;       // BCryptHashData wall time (both file sides)
    uint64_t statTicks = 0;       // T1+T2 wall time (both file sides)
    uint64_t cacheTicks = 0;      // HashCache::Lookup wall time (both sides)
    uint64_t sizeSource = 0;
    uint64_t sizeDest = 0;
    JobVerdict verdict = JobVerdict::Identical;
    std::wstring path;
};

// Top-10 list of one side, kept sorted by execTicks descending.
struct SideTopTen {
    std::vector<TopJob> top;
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
    double activeSecondsA = 0.0; // union length of the A-side hash spans
    double activeSecondsB = 0.0;
    double overlapSeconds = 0.0; // wall time with >=1 A and >=1 B active
    // Pool / backpressure metrics merged from the worker pools by the caller.
    uint64_t backpressureWaits = 0;
    double backpressureWaitSeconds = 0.0;
    uint64_t waitAllCount = 0;
    double waitAllSeconds = 0.0;
    uint64_t maxOutstandingTasks = 0;
    uint64_t maxQueueDepth = 0;
    // Pool utilization (merged via MergePool; ticks are raw QPC):
    uint64_t poolBusyTicks = 0;   // total wall time workers spent running tasks
    uint64_t poolMaxActive = 0;   // max simultaneous workers running tasks
    uint64_t poolWallTicks = 0;   // pool lifetime wall time (ctor -> metrics snapshot)
    uint64_t poolSubmitted = 0;   // tasks submitted to the pool(s)
    uint64_t poolCompleted = 0;   // tasks completed by the pool(s)
    // average active workers is derived: poolBusyTicks / poolWallTicks.

    // Per-side emit-callback breakdown. emitTicks[i] is the total wall time the
    // side's worker spent inside its enumeration entry callback (the same
    // call the MFT profiler reports as emit_consumer); emitBackpressureTicks[i]
    // is the part of it spent pushing batches to the hash pool (blocked in
    // waitOutstandingBelow()/FlushHashCandidates()). emitTicks - backpressure =
    // the actual callback + MatchTable work, so one can tell how much of
    // emit_consumer is pool backpressure. Indexed by profiling::Side.
    uint64_t emitTicks[2] = {0, 0};
    uint64_t emitBackpressureTicks[2] = {0, 0};

    // Per-submitter-side job aggregates and top-10 (see structs above).
    JobAggregate jobs[2];
    SideTopTen topJobs[2];
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

    // --- per-side file hash lifecycle (wraps one Sha256FileFromHandle span) ---
    void FileBegin(HashSession& s, Side side, const std::wstring& path, uint64_t expectedSize);
    void FileEnd(HashSession& s, Side side, const std::wstring& path, uint64_t expectedSize,
                 const FileTimings& t, bool ok);

    // --- T1/T2 change-during-scan control cost (StatHandle/StatFile calls) ---
    // `s` is the owning session so the time also lands in the job aggregates.
    void NoteStatBefore(HashSession& s, Side side, uint64_t ticks); // T1 before hashing
    void NoteStatAfter(HashSession& s, Side side, uint64_t ticks);  // T2 after hashing

    // --- HashCache::Lookup wall time (only when a cache is configured) ---
    void NoteCacheLookup(HashSession& s, Side side, uint64_t ticks);

    // Per-side emit-callback profiling: `totalTicks` is the wall time the side's
    // worker spent inside its onEntry callback (the enumerator emit path, the
    // same thing MFT reports as emit_consumer); `backpressureTicks` is the part
    // of it spent blocked pushing batches to the hash pool (waitOutstandingBelow
    // via FlushHashCandidates). Lets emit time be separated from backpressure per
    // side. Gated by enabled(); a no-op on a normal scan.
    void NoteEmit(Side side, uint64_t totalTicks, uint64_t backpressureTicks);

    // Merges ThreadPool metrics into the report (called once per pool used by
    // the scan; never alters synchronization). The extra pool-utilization fields
    // are passive counters read from ThreadPool::metrics().
    void MergePool(uint64_t maxOutstanding, uint64_t maxQueueDepth, uint64_t backpressureWaits,
                   uint64_t backpressureWaitTicks, uint64_t waitAllCount, uint64_t waitAllTicks,
                   uint64_t busyTicks, uint64_t maxActiveWorkers, uint64_t wallTicks,
                   uint64_t submitted, uint64_t completed);

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

    // Per-side job aggregates for the side that SUBMITTED the jobs.
    struct JobAggAccum {
        std::atomic<uint64_t> jobs{0};
        std::atomic<uint64_t> bytes{0};
        std::atomic<uint64_t> queueWaitTicks{0};
        std::atomic<uint64_t> queueWaitMax{0};
        std::atomic<uint64_t> execTicks{0};
        std::atomic<uint64_t> execMax{0};
        std::atomic<uint64_t> readTicks{0};
        std::atomic<uint64_t> hashTicks{0};
        std::atomic<uint64_t> statTicks{0};
        std::atomic<uint64_t> cacheTicks{0};
        std::atomic<uint64_t> vIdentical{0};
        std::atomic<uint64_t> vMismatch{0};
        std::atomic<uint64_t> vChanged{0};
        std::atomic<uint64_t> vReadError{0};
        std::atomic<uint64_t> vAccessDenied{0};
        std::atomic<uint64_t> vCancelled{0};
    };

    // Fixed-capacity top-10 holder (O(10) memory per side). `gate` is the
    // current tenth entry's exec time (0 while fewer than 10 entries): it only
    // grows, because every insertion that replaces the tenth entry brings in an
    // exec >= the previous tenth, so `if (exec <= gate) skip` can never drop a
    // job that would enter the list.
    struct TopTen {
        mutable std::mutex mutex;
        std::vector<TopJob> v;
        std::atomic<uint64_t> gate{0};
    };

    // Called at job end (TaskEnd) once the job is in the pool; keeps top_[side]
    // at exactly the 10 longest jobs of that side.
    void RecordTop(int side, const HashSession& s);

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

    // Aggregate per-file-side counters (indexed by Side).
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
    std::atomic<uint64_t> sideCacheTicks_[2]{0, 0};
    std::atomic<uint64_t> sideCacheLookups_[2]{0, 0};

    // Per-submitter-side job aggregates and per-side top-10.
    JobAggAccum jobAgg_[2];
    TopTen topTen_[2];

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
    std::atomic<uint64_t> poolBusyTicks_{0};
    std::atomic<uint64_t> poolMaxActive_{0};
    std::atomic<uint64_t> poolWallTicks_{0};
    std::atomic<uint64_t> poolSubmitted_{0};
    std::atomic<uint64_t> poolCompleted_{0};

    // Per-side emit-callback wall time and the backpressure portion of it
    // (filled through NoteEmit, read in Finalize).
    std::atomic<uint64_t> emitTicks_[2]{0, 0};
    std::atomic<uint64_t> emitBackpressureTicks_[2]{0, 0};

    // Verbose records.
    mutable std::mutex recordsMutex_;
    std::vector<JobRecord> records_;
};

// Lightweight per-task context: bundles the profiler and this task's unique job
// id. Created on the worker stack; the destructor always releases the task slot
// (including on the exception path).
//
// The per-job timing fields are written ONLY by the worker thread that owns the
// session (the session is a stack object inside one task), so they are plain
// (non-atomic) accumulators -- no cross-thread sharing. The SubmitHashCandidates
// closure sets `fullJob = true` and the timestamps; the snapshot-capture path
// (HashSourceIndex) also creates sessions but never marks them full, so the job
// aggregates stay scoped to content-comparison jobs.
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

    // Per-job timing (owned by the executing worker; meaningful only when the
    // SubmitHashCandidates closure set fullJob).
    bool fullJob = false;        // this task is a full content-comparison job
    Side side = Side::Source;    // side that submitted the job (call site)
    uint64_t enqueueTick = 0;    // QPC before pool.submit for this candidate
    uint64_t startTick = 0;      // QPC when the worker started executing the task
    uint64_t endTick = 0;        // QPC when the comparison finished
    uint64_t sizeSource = 0;     // candidate sizes (expected)
    uint64_t sizeDest = 0;
    JobVerdict verdict = JobVerdict::Identical;
    std::wstring relPath;        // candidate relative path (for the top-10)
    // Disjoint wall accumulators over both file sides, all inside [start, end):
    uint64_t jobReadTicks = 0;   // ReadFile
    uint64_t jobHashTicks = 0;   // BCryptHashData
    uint64_t jobStatTicks = 0;   // T1 + T2
    uint64_t jobCacheTicks = 0;  // HashCache::Lookup
};

} // namespace profiling
} // namespace bv