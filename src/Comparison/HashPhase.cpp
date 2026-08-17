#include "Comparison/HashPhase.h"

#include <algorithm>

#include "Filesystem/PathUtil.h"
#include "Hashing/HashUtil.h"

namespace bv {

namespace {

using Digest = hashing::Digest;

void AddStats(Stats& target, const Stats& add) {
    target.sourceFiles += add.sourceFiles;
    target.sourceDirs += add.sourceDirs;
    target.destFiles += add.destFiles;
    target.destDirs += add.destDirs;
    target.identicalFiles += add.identicalFiles;
    target.identicalDirs += add.identicalDirs;
    target.missingFiles += add.missingFiles;
    target.missingDirs += add.missingDirs;
    target.extraFiles += add.extraFiles;
    target.extraDirs += add.extraDirs;
    target.sizeMismatch += add.sizeMismatch;
    target.contentMismatch += add.contentMismatch;
    target.readErrors += add.readErrors;
    target.accessDenied += add.accessDenied;
    target.changedDuringScan += add.changedDuringScan;
    target.bytesSource += add.bytesSource;
    target.bytesDest += add.bytesDest;
}

// Hashes both sides of one candidate and folds the outcome into the thread-safe
// `sink`. Safe to call concurrently from any number of pool workers (stats are
// atomics, problems go through the sink mutex). A task that bails because
// cancellation was requested produces NO outcome for its candidate: it is as if
// that pair was never processed this run (no stats bucket, no FileResult), so a
// cancelled batch can never fabricate read errors or mismatches for files that
// were never actually hashed.
void HashOneCandidateInto(const ContentCandidate& c, bool offlineSource, FileIndex* index,
                          const std::wstring& sourceRoot, const std::wstring& destRoot,
                          ConcurrentSink& sink, const std::atomic_bool* cancel,
                          hashing::HashCache* cache, std::atomic<size_t>& cacheHits,
                          profiling::HashSession* session) {
    auto& stats = sink.stats();
    const auto inc = [&stats](std::atomic<uint64_t>& x) {
        x.fetch_add(1, std::memory_order_relaxed);
    };
    const auto reportReadError = [&](bool denied, bool hasSrc, bool hasDst, const Digest& sd,
                                     const Digest& dd) {
        if (denied) {
            inc(stats.accessDenied);
        } else {
            inc(stats.readErrors);
        }
        FileResult r;
        r.status = denied ? Status::AccessDenied : Status::ReadError;
        r.relativePath = c.relativePath;
        r.sizeSource = c.sizeSource;
        r.sizeDest = c.sizeDest;
        r.isDirectory = false;
        r.hasHashSource = hasSrc;
        r.hasHashDest = hasDst;
        r.hashSource = sd;
        r.hashDest = dd;
        r.errorMessage = denied ? L"accesso negato durante il calcolo dell'impronta"
                                : L"errore di lettura durante il calcolo dell'impronta";
        sink.addProblem(std::move(r));
    };

    if (cancel && cancel->load(std::memory_order_relaxed)) {
        // Cancelled before any work: drop the candidate silently. Reporting a
        // read error here would fabricate a verdict for a file that was never
        // opened.
        return;
    }

    bool changed = false;
    hashing::HashStatus srcStatus = hashing::HashStatus::ReadError;
    hashing::HashStatus dstStatus = hashing::HashStatus::ReadError;
    Digest srcDigest{};
    Digest dstDigest{};
    bool hasSrc = false;
    bool hasDst = false;
    if (offlineSource) {
        // Source device absent: use the digest captured in the snapshot.
        hasSrc = index->getHash(c.relativePath, srcDigest);
        srcStatus = hasSrc ? hashing::HashStatus::Ok : hashing::HashStatus::ReadError;
    } else {
        hashing::HashOneSide(pathutil::MakeAbsolute(sourceRoot, c.relativePath), c.sizeSource,
                             c.srcMtime, changed, srcStatus, srcDigest, true, cache, cacheHits,
                             session, profiling::Side::Source, cancel);
        hasSrc = (srcStatus == hashing::HashStatus::Ok);
        if (srcStatus == hashing::HashStatus::Cancelled ||
            (cancel && cancel->load(std::memory_order_relaxed))) return; // no verdict (cancelled)
    }
    bool dstChanged = false;
    // Don't start hashing the destination if cancellation landed on the source side.
    if (cancel && cancel->load(std::memory_order_relaxed)) return;
    hashing::HashOneSide(pathutil::MakeAbsolute(destRoot, c.relativePath), c.sizeDest, c.dstMtime,
                         dstChanged, dstStatus, dstDigest, true, cache, cacheHits, session,
                         profiling::Side::Dest, cancel);
    hasDst = (dstStatus == hashing::HashStatus::Ok);
    if (dstStatus == hashing::HashStatus::Cancelled ||
        (cancel && cancel->load(std::memory_order_relaxed))) return; // no verdict (cancelled)
    changed = changed || dstChanged;

    if (changed) {
        inc(stats.changedDuringScan);
        FileResult r;
        r.status = Status::ChangedDuringScan;
        r.relativePath = c.relativePath;
        r.sizeSource = c.sizeSource;
        r.sizeDest = c.sizeDest;
        r.isDirectory = false;
        r.errorMessage = L"file modificato durante la scansione (riverificare)";
        sink.addProblem(std::move(r));
        return;
    }

    if (srcStatus == hashing::HashStatus::Ok && dstStatus == hashing::HashStatus::Ok) {
        if (srcDigest == dstDigest) {
            inc(stats.identicalFiles);
        } else {
            inc(stats.contentMismatch);
            FileResult r;
            r.status = Status::ContentMismatch;
            r.relativePath = c.relativePath;
            r.sizeSource = c.sizeSource;
            r.sizeDest = c.sizeDest;
            r.isDirectory = false;
            r.hasHashSource = true;
            r.hasHashDest = true;
            r.hashSource = srcDigest;
            r.hashDest = dstDigest;
            sink.addProblem(std::move(r));
        }
        return;
    }

    const bool denied = srcStatus == hashing::HashStatus::NoAccess ||
                        dstStatus == hashing::HashStatus::NoAccess;
    reportReadError(denied, hasSrc, hasDst, srcDigest, dstDigest);
}

} // namespace

void SubmitHashCandidates(const std::vector<ContentCandidate>& candidates, ThreadPool& pool,
                          bool offlineSource, FileIndex* index,
                          const std::wstring& sourceRoot, const std::wstring& destRoot,
                          ConcurrentSink& sink, const std::atomic_bool* cancel,
                          hashing::HashCache* cache, std::atomic<size_t>& cacheHits,
                          std::atomic<uint64_t>* hashDone, profiling::HashProfiler* prof) {
    for (const ContentCandidate& c : candidates) {
        // The candidate is captured BY VALUE: the batch vector may be reused or
        // destroyed as soon as this call returns, and a task can never confuse
        // one candidate with a neighbouring element.
        pool.submit([c, offlineSource, index, &sourceRoot, &destRoot, &sink, cancel, cache,
                     &cacheHits, hashDone, prof] {
            // HashSession owns the profiler task slot: it bumps/decrements the
            // active-job counters and issues this task's unique job id, and it
            // is destroyed on every exit path (including a thrown exception).
            profiling::HashSession session(prof);
            // HashOneCandidateInto never throws in practice (every failure is
            // folded into the sink), but if it did the candidate would still be
            // counted as done so progress can reach 100%; the pool also records
            // the throw as a task error for the caller.
            try {
                HashOneCandidateInto(c, offlineSource, index, sourceRoot, destRoot, sink, cancel,
                                     cache, cacheHits, &session);
            } catch (...) {
                if (prof) prof->TaskFailed();
                if (hashDone) hashDone->fetch_add(1, std::memory_order_relaxed);
                throw;
            }
            if (hashDone) hashDone->fetch_add(1, std::memory_order_relaxed);
        });
    }
}

void RunHashPhase(const std::vector<ContentCandidate>& candidates, ThreadPool& pool,
                  bool offlineSource, FileIndex* index, const std::wstring& sourceRoot,
                  const std::wstring& destRoot, ResultSet& out, const std::atomic_bool* cancel,
                  const std::function<void(uint64_t done, uint64_t total)>& onProgress,
                  hashing::HashCache* cache, std::atomic<size_t>& cacheHits,
                  profiling::HashProfiler* prof) {
    ConcurrentSink sink;
    const size_t total = candidates.size();
    size_t done = 0;
    while (done < total) {
        if (cancel && cancel->load(std::memory_order_relaxed)) break;

        const size_t n = std::min(kHashBatchSize, total - done);
        std::vector<ContentCandidate> batch(candidates.begin() + done, candidates.begin() + done + n);
        SubmitHashCandidates(batch, pool, offlineSource, index, sourceRoot, destRoot, sink,
                             cancel, cache, cacheHits, nullptr, prof);
        pool.waitAll();
        done += n;
        if (onProgress) onProgress(done, total);
    }

    ResultSet s = sink.take();
    AddStats(out.stats, s.stats);
    for (FileResult& p : s.problems) out.problems.push_back(std::move(p));
}

} // namespace bv
