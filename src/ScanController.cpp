#include "ScanController.h"

#include <chrono>
#include <functional>
#include <memory>

#include "Comparison/ConcurrentComparer.h"
#include "Export/CsvExporter.h"
#include "Export/JsonExporter.h"
#include "Filesystem/FileIndex.h"
#include "Filesystem/FileIndexSerializer.h"
#include "Filesystem/MftEnumerator.h"
#include "Filesystem/PathUtil.h"
#include "Filesystem/Win32Enumerator.h"
#include "Hashing/HashCache.h"
#include "Hashing/Sha256.h"
#include "Threading/IoClass.h"
#include "Threading/ThreadPool.h"

namespace bv {

namespace {

double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// One digest slot of a hash batch. `skipped` records that no digest was produced
// for this slot (the job bailed on cancellation, or the file could not be read
// or hashed): the slot must then NOT be written into the index. Writing the
// default all-zero digest would be persisted to the snapshot and later misread
// as a content mismatch, so a missing digest is represented by "no hash entry"
// instead of a bogus digest.
struct HashSlot {
    std::array<uint8_t, 32> digest{};
    bool skipped = false;
};

} // namespace

// Computes the digest of every file in `index` (all under `root`), storing them
// back in the index. Used by the snapshot-capture path in Content mode so the
// snapshot embeds source digests and the offline comparison can reuse them (the
// source is then never read twice). Optionally feeds/reads the hash cache.
//
// A slot whose job never produced a digest (cancellation landed while the batch
// was in flight, or the file could not be hashed) is marked `skipped` and is
// left WITHOUT an entry in the index: it is simply as if that file was never
// hashed this run. The caller decides whether an interrupted capture is still
// written as a snapshot.
//
// `onBatchSubmitted` is a test seam: it fires right after a batch's jobs are
// submitted and before they are drained, so a test can flip the cancel flag at
// a well-defined point and deterministically exercise the mid-batch bailout.
// Production callers leave it empty.
void HashSourceIndex(FileIndex& index, const std::wstring& root, ThreadPool& pool,
                     const std::atomic_bool* cancel, hashing::HashCache* cache,
                     std::atomic<size_t>& cacheHits,
                     const std::function<void(uint64_t done, uint64_t total)>& onProgress,
                     std::function<void()> onBatchSubmitted,
                     profiling::HashProfiler* prof) {
    std::vector<std::wstring> files;
    for (const auto& kv : index.entries()) {
        if (!kv.second.isDirectory) files.push_back(kv.second.relativePath);
    }

    const size_t total = files.size();
    size_t done = 0;
    while (done < total && !(cancel && cancel->load(std::memory_order_relaxed))) {
        const size_t n = std::min<size_t>(256, total - done);
        std::vector<HashSlot> slots(n);
        for (size_t i = 0; i < n; ++i) {
            // Capture the stable vector index, not a reference to the element:
            // `files` is fully built before the loop and never modified, and the
            // batch-local `slots` below is drained by pool.waitAll() before the
            // batch scope ends, so both stay valid for the whole task lifetime.
            // `root` is a const reference parameter alive through this call.
            const size_t relIndex = done + i;
            pool.submit([&files, &slots, relIndex, i, &root, cache, &cacheHits, cancel, prof] {
                profiling::HashSession session(prof);
                HashSlot& slot = slots[i];
                if (cancel && cancel->load(std::memory_order_relaxed)) {
                    slot.skipped = true;
                    return;
                }
                const std::wstring& rel = files[relIndex];
                const std::wstring abs = pathutil::MakeAbsolute(root, rel);
                uint64_t sz = 0;
                uint64_t mt = 0;
                if (!hashing::StatFile(abs, sz, mt)) {
                    slot.skipped = true;
                    return;
                }
                if (cache && cache->Lookup(abs, sz, mt, slot.digest)) {
                    cacheHits.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                profiling::FileTimings ft;
                const bool pf = prof && prof->enabled();
                if (pf) prof->FileBegin(session, profiling::Side::Source, abs, sz);
                const bool ok =
                    hashing::Sha256File(abs, slot.digest, pf ? &ft : nullptr) ==
                    hashing::HashStatus::Ok;
                if (pf) prof->FileEnd(session, profiling::Side::Source, abs, sz, ft, ok);
                if (ok) {
                    if (cache) cache->Store(abs, sz, mt, slot.digest);
                } else {
                    slot.skipped = true;
                }
            });
        }
        if (onBatchSubmitted) onBatchSubmitted();
        pool.waitAll();
        for (size_t i = 0; i < n; ++i) {
            if (!slots[i].skipped) index.setHash(files[done + i], slots[i].digest);
        }
        done += n;
        if (onProgress) onProgress(done, total);
    }
}

ScanReport ScanController::run(const ScanOptions& options) {
    ScanReport report;
    report.backendUsed = EnumeratorBackend::Win32;
    report.modeUsed = options.mode;

    // When the caller provided a profiler it is always enabled (the caller
    // decides whether to collect by handing it over or not).
    profiling::HashProfiler* hashProf = options.hashProfiler;
    if (hashProf) hashProf->setEnabled(true);

    const double t0 = NowSeconds();
    const bool haveCompare = !options.compareFrom.empty();
    const bool haveSnapshot = !options.snapshotOut.empty();
    const bool haveDest = !options.destination.empty();

    if (haveCompare && haveSnapshot) {
        report.sourceOk = false;
        FileResult r;
        r.status = Status::ReadError;
        r.isDirectory = true;
        r.errorMessage = L"--compare e --snapshot-out sono mutuamente esclusivi";
        report.results.problems.push_back(std::move(r));
        ++report.results.stats.readErrors;
        return report;
    }

    ScanMode mode = options.mode;

    // Optional persistent SHA-256 cache (path + size + last-write time).
    std::unique_ptr<hashing::HashCache> cache;
    if (!options.hashCacheFile.empty()) {
        std::wstring cacheErr;
        cache.reset(new hashing::HashCache(options.hashCacheFile, cacheErr));
        if (!cacheErr.empty()) {
            FileResult r;
            r.status = Status::ReadError;
            r.isDirectory = true;
            r.errorMessage = std::move(cacheErr);
            report.results.problems.push_back(std::move(r));
            ++report.results.stats.readErrors;
        }
    }

    // Live count of hash workers, reported in ScanProgress during the Hashing
    // phase so a UI can show the actual pool size while it runs (0 elsewhere).
    std::atomic<unsigned int> hashThreadsActive{0};

    const auto emitProgress = [&](ScanPhase phase, uint64_t files, uint64_t dirs,
                                  uint64_t bytes, const std::wstring& path) {
        if (options.onProgress) {
            ScanProgress p;
            p.phase = phase;
            p.files = files;
            p.dirs = dirs;
            p.bytes = bytes;
            p.currentPath = path;
            p.threads = hashThreadsActive.load(std::memory_order_relaxed);
            options.onProgress(p);
        }
    };

    // MFT backend decision. Auto picks MFT only when BOTH roots are on a local
    // NTFS volume; in offline mode the source root is never touched, so only the
    // destination is checked (matches the old "compareFrom means no source pass").
    const bool acceptMft = [&]() -> bool {
        switch (options.backend) {
            case EnumeratorBackend::Mft:
                return true;
            case EnumeratorBackend::Win32:
                return false;
            case EnumeratorBackend::Auto: {
                const bool destMft = MftEnumerator::IsSupported(options.destination);
                if (haveCompare) return destMft; // source device not consulted
                return destMft && MftEnumerator::IsSupported(options.source);
            }
        }
        return false;
    }();
    report.backendUsed = acceptMft ? EnumeratorBackend::Mft : EnumeratorBackend::Win32;

    const auto resolveHashThreads = [&]() -> unsigned int {
        return options.hashThreads == 0
                   ? DefaultThreadCount(ClassifyIoClass(options.source, options.destination))
                   : options.hashThreads;
    };

    // The source side is pre-built (snapshot loaded, or index captured for
    // --snapshot-out); a pure live-live scan has no source pass at all.
    const bool sourceFromIndex = haveCompare || haveSnapshot;
    FileIndex sourceIndex(caseSensitive_);

    // ---------------------------------------------------------------------
    // 1. Source side: load it from a snapshot, or capture it for the snapshot
    //    output. A plain live-live comparison skips this phase entirely: the
    //    source is enumerated concurrently with the destination below.
    // ---------------------------------------------------------------------
    const double t1 = NowSeconds();
    if (haveCompare) {
        std::wstring err;
        std::wstring loadedRoot;
        if (indexio::ReadSnapshot(options.compareFrom, sourceIndex, loadedRoot, err)) {
            report.usedSnapshot = true;
            report.results.stats.sourceFiles = sourceIndex.stats().files;
            report.results.stats.sourceDirs = sourceIndex.stats().dirs;
            report.results.stats.bytesSource = sourceIndex.stats().bytes;
            report.sourceOk = true;
            report.secondsEnumerateSource = NowSeconds() - t1;
            if (mode == ScanMode::Content && sourceIndex.hashCount() == 0 &&
                sourceIndex.size() > 0) {
                mode = ScanMode::Size; // snapshot has no digests: cannot verify content
                report.contentDegradedToSize = true;
                report.modeUsed = mode;
            }
        } else {
            report.sourceOk = false;
            FileResult r;
            r.status = Status::ReadError;
            r.isDirectory = true;
            r.errorMessage = std::move(err);
            report.results.problems.push_back(std::move(r));
            ++report.results.stats.readErrors;
        }
    } else if (haveSnapshot) {
        const IFileEnumerator::ProgressCallback sourceProgress =
            [&](uint64_t files, uint64_t dirs, uint64_t bytes, const std::wstring& path) {
                emitProgress(ScanPhase::EnumerateSource, files, dirs, bytes, path);
            };
        FileIndex::BuildResult build;
        bool sourceOk = false;
        if (acceptMft) {
            // MFT first, Win32 as a full rebuild on any failure (the index is
            // only populated through this single pass, so a fallback is safe).
            MftEnumerator mft;
            Win32Enumerator win32;
            if (MftEnumerator::IsSupported(options.source)) {
                build = sourceIndex.build(options.source, mft, sourceProgress, options.cancel);
                sourceOk = build.ok;
                if (!sourceOk) {
                    build = sourceIndex.build(options.source, win32, sourceProgress,
                                              options.cancel);
                    sourceOk = build.ok;
                    report.backendUsed = sourceOk ? EnumeratorBackend::Win32
                                                  : EnumeratorBackend::Mft;
                } else {
                    report.backendUsed = EnumeratorBackend::Mft;
                }
            } else {
                build = sourceIndex.build(options.source, win32, sourceProgress, options.cancel);
                sourceOk = build.ok;
                report.backendUsed = EnumeratorBackend::Win32;
            }
        } else {
            Win32Enumerator win32;
            build = sourceIndex.build(options.source, win32, sourceProgress, options.cancel);
            sourceOk = build.ok;
        }
        report.sourceOk = sourceOk;
        report.results.stats.sourceFiles = build.stats.files;
        report.results.stats.sourceDirs = build.stats.dirs;
        report.results.stats.bytesSource = build.stats.bytes;
        report.secondsEnumerateSource = NowSeconds() - t1;

        // Source enumeration errors become result entries.
        for (const ScanError& err : build.errors) {
            FileResult r;
            r.status = (err.winError == 5) ? Status::AccessDenied : Status::ReadError;
            r.relativePath = err.path;
            r.errorMessage = err.message;
            r.isDirectory = true;
            if (err.lostDevice) {
                r.errorMessage = L"dispositivo scollegato durante l'operazione (riverificare)";
            }
            report.results.problems.push_back(std::move(r));
            if (r.status == Status::AccessDenied) {
                ++report.results.stats.accessDenied;
            } else {
                ++report.results.stats.readErrors;
            }
        }

        if (options.cancel && options.cancel->load()) {
            report.sourceOk = false;
        }

        // Capture the source index as a snapshot (optional). In Content mode the
        // source files are hashed first so the snapshot embeds their digests;
        // the comparison below then reuses them instead of reading the source a
        // second time.
        if (haveSnapshot && report.sourceOk) {
            if (mode == ScanMode::Content) {
                const unsigned int srcThreads = resolveHashThreads();
                hashThreadsActive.store(srcThreads, std::memory_order_relaxed);
                ThreadPool sourcePool(srcThreads);
                std::atomic<size_t> hits{0};
                const auto hashProgress = [&](uint64_t done, uint64_t total) {
                    emitProgress(ScanPhase::Hashing, done, total, 0, L"");
                };
                const double th0 = NowSeconds();
                HashSourceIndex(sourceIndex, options.source, sourcePool, options.cancel,
                                cache.get(), hits, hashProgress, {}, hashProf);
                hashThreadsActive.store(0, std::memory_order_relaxed);
                report.secondsHashing += NowSeconds() - th0;
                report.hashCacheHits += hits.load();
                report.hashThreadsUsed = srcThreads;
                report.hashingErrors += sourcePool.taskErrors();
                if (hashProf) {
                    const ThreadPool::ThreadPoolMetrics m = sourcePool.metrics();
                    hashProf->MergePool(m.maxOutstanding, m.maxQueueDepth, m.backpressureWaits,
                                        m.backpressureWaitTicks, m.waitAllCount, m.waitAllTicks);
                }
            }

            // If hashing was interrupted (cancellation landed while a batch was
            // in flight), some digests were never computed. An incomplete capture
            // must never be presented as a complete snapshot, so the source side
            // is marked not-ok and the write below (and the destination pass, both
            // guarded on report.sourceOk) are skipped -- mirroring the
            // enumeration-cancellation check above.
            if (options.cancel && options.cancel->load()) {
                report.sourceOk = false;
            }

            std::wstring werr;
            if (report.sourceOk) {
                report.snapshotWritten =
                    indexio::WriteSnapshot(options.snapshotOut, sourceIndex, options.source, werr);
                if (!report.snapshotWritten) {
                    FileResult r;
                    r.status = Status::ReadError;
                    r.isDirectory = true;
                    r.errorMessage = std::move(werr);
                    report.results.problems.push_back(std::move(r));
                    ++report.results.stats.readErrors;
                }
            }
        }
    }

    // Distinct paths that folded to the same case-insensitive key were resolved
    // last-wins while building the source index; surface it so the caller can
    // warn the user that a name was dropped.
    report.pathCollisions = sourceIndex.collisionCount();

    // ---------------------------------------------------------------------
    // 2. Destination pass: enumerate + compare against the source, CONCURRENTLY.
    //    When the source was pre-built (offline / snapshot capture) it is fed
    //    from the index; otherwise the two drives are enumerated in parallel.
    // ---------------------------------------------------------------------
    if (haveDest && report.sourceOk && !(options.cancel && options.cancel->load())) {
        ThreadPool hashPool(mode == ScanMode::Content ? resolveHashThreads() : 0);
        if (mode == ScanMode::Content) report.hashThreadsUsed = hashPool.threadCount();

        const IFileEnumerator::ProgressCallback enumProgress =
            [&](uint64_t files, uint64_t dirs, uint64_t bytes, const std::wstring& path) {
                emitProgress(ScanPhase::CompareDestination, files, dirs, bytes, path);
            };
        double hashStart = 0.0;
        double compareHashSeconds = 0.0;
        bool hashing = false;
        const auto hashProgress = [&](uint64_t done, uint64_t total) {
            if (!hashing) {
                hashing = true;
                hashStart = NowSeconds();
                hashThreadsActive.store(hashPool.threadCount(), std::memory_order_relaxed);
            }
            emitProgress(ScanPhase::Hashing, done, total, 0, L"");
            if (done >= total) {
                hashing = false;
                hashThreadsActive.store(0, std::memory_order_relaxed);
                compareHashSeconds += NowSeconds() - hashStart;
            }
        };

        ConcurrentComparer comparer(caseSensitive_, mode, acceptMft, options.source,
                                    options.destination,
                                    sourceFromIndex ? ConcurrentComparer::SourceKind::FromIndex
                                                    : ConcurrentComparer::SourceKind::Live,
                                    sourceFromIndex ? &sourceIndex : nullptr, options.cancel,
                                    hashProf);

        ConcurrentComparer::Result cr = comparer.run(hashPool, enumProgress, hashProgress,
                                                     cache.get());
        hashThreadsActive.store(0, std::memory_order_relaxed);

        if (!sourceFromIndex) {
            report.sourceOk = cr.sourceStatus == ConcurrentComparer::WorkerStatus::Success;
        }
        report.destinationOk = cr.destinationStatus == ConcurrentComparer::WorkerStatus::Success;
        report.results = std::move(cr.results);
        report.secondsHashing += compareHashSeconds;
        report.hashCacheHits += comparer.cacheHits();
        report.hashingErrors += hashPool.taskErrors();
        for (std::wstring& n : cr.notes) report.notes.push_back(std::move(n));
        if (hashProf) {
            const ThreadPool::ThreadPoolMetrics m = hashPool.metrics();
            hashProf->MergePool(m.maxOutstanding, m.maxQueueDepth, m.backpressureWaits,
                                m.backpressureWaitTicks, m.waitAllCount, m.waitAllTicks);
        }

        if (cache) {
            std::wstring werr;
            if (!cache->Save(werr) && !werr.empty()) {
                FileResult r;
                r.status = Status::ReadError;
                r.isDirectory = true;
                r.errorMessage = std::move(werr);
                report.results.problems.push_back(std::move(r));
                ++report.results.stats.readErrors;
            }
        }
    }

    // ---------------------------------------------------------------------
    // 3. Export the problems (non-identical entries) as CSV/JSON (optional).
    // ---------------------------------------------------------------------
    if (!options.exportPath.empty()) {
        exporting::ExportFormat fmt = options.exportFormat == exporting::ExportFormat::Auto
                                          ? exporting::InferFormat(options.exportPath)
                                          : options.exportFormat;
        std::wstring werr;
        const bool ok = fmt == exporting::ExportFormat::Json
                            ? exporting::WriteJson(options.exportPath, report.results, werr)
                            : exporting::WriteCsv(options.exportPath, report.results, werr);
        report.exportWritten = ok;
        report.exportError = std::move(werr);
    }

    if (options.onProgress) {
        ScanProgress p;
        p.phase = ScanPhase::Done;
        p.files = report.results.stats.sourceFiles;
        p.dirs = report.results.stats.sourceDirs;
        options.onProgress(p);
    }

    const double t3 = NowSeconds();
    report.secondsDestinationPass = t3 - t0 - report.secondsEnumerateSource -
                                    report.secondsHashing;
    report.secondsTotal = t3 - t0;

    if (hashProf) hashProf->Finalize(report.hashProfile);

    return report;
}

} // namespace bv