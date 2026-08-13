#include "ScanController.h"

#include <chrono>
#include <functional>
#include <memory>

#include "Comparison/FileComparator.h"
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

// Computes the digest of every file in `index` (all under `root`), storing them
// back in the index. Used by the snapshot-capture path in Content mode so the
// snapshot embeds source digests and the live comparison can reuse them (the
// source is then never read twice). Optionally feeds/reads the hash cache.
void HashSourceIndex(FileIndex& index, const std::wstring& root, ThreadPool& pool,
                     const std::atomic_bool* cancel, hashing::HashCache* cache,
                     std::atomic<size_t>& cacheHits,
                     const std::function<void(uint64_t done, uint64_t total)>& onProgress) {
    std::vector<std::wstring> files;
    for (const auto& kv : index.entries()) {
        if (!kv.second.isDirectory) files.push_back(kv.second.relativePath);
    }

    const size_t total = files.size();
    size_t done = 0;
    while (done < total && !(cancel && cancel->load(std::memory_order_relaxed))) {
        const size_t n = std::min<size_t>(256, total - done);
        std::vector<std::array<uint8_t, 32>> digests(n);
        for (size_t i = 0; i < n; ++i) {
            const std::wstring& rel = files[done + i];
            pool.submit([&root, &rel, &d = digests[i], cache, &cacheHits] {
                const std::wstring abs = pathutil::MakeAbsolute(root, rel);
                uint64_t sz = 0;
                uint64_t mt = 0;
                if (!hashing::StatFile(abs, sz, mt)) return;
                if (cache && cache->Lookup(abs, sz, mt, d)) {
                    cacheHits.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (hashing::Sha256File(abs, d) == hashing::HashStatus::Ok) {
                    if (cache) cache->Store(abs, sz, mt, d);
                }
            });
        }
        pool.waitAll();
        for (size_t i = 0; i < n; ++i) {
            index.setHash(files[done + i], digests[i]);
        }
        done += n;
        if (onProgress) onProgress(done, total);
    }
}

} // namespace

ScanReport ScanController::run(const ScanOptions& options) {
    ScanReport report;
    report.backendUsed = EnumeratorBackend::Win32;
    report.modeUsed = options.mode;

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
    unsigned int hashThreadsActive = 0;

    const auto emitProgress = [&](ScanPhase phase, uint64_t files, uint64_t dirs,
                                  const std::wstring& path) {
        if (options.onProgress) {
            ScanProgress p;
            p.phase = phase;
            p.files = files;
            p.dirs = dirs;
            p.currentPath = path;
            p.threads = hashThreadsActive;
            options.onProgress(p);
        }
    };

    // Enumeration back-end selection (MFT scans the raw NTFS index and falls
    // back to Win32 if it cannot run). Used by both the source pass (when
    // enumerating live) and the destination pass.
    bool wantMft = false;
    switch (options.backend) {
        case EnumeratorBackend::Mft:
            wantMft = true;
            break;
        case EnumeratorBackend::Win32:
            wantMft = false;
            break;
        case EnumeratorBackend::Auto:
            wantMft = MftEnumerator::IsSupported(options.source) &&
                     MftEnumerator::IsSupported(options.destination);
            break;
    }
    std::unique_ptr<IFileEnumerator> primary =
        wantMft ? std::unique_ptr<IFileEnumerator>(new MftEnumerator())
                : std::unique_ptr<IFileEnumerator>(new Win32Enumerator());
    std::unique_ptr<IFileEnumerator> fallback(new Win32Enumerator());

    const auto runWithFallback =
        [&](const std::function<bool(IFileEnumerator&)>& pass) -> bool {
        if (wantMft) {
            if (pass(*primary)) {
                report.backendUsed = EnumeratorBackend::Mft;
                return true;
            }
            if (fallback.get() != primary.get()) {
                if (pass(*fallback)) {
                    report.backendUsed = EnumeratorBackend::Win32;
                    return true;
                }
            }
            report.backendUsed = EnumeratorBackend::Win32;
            return false;
        }
        return pass(*primary);
    };

    const auto resolveHashThreads = [&]() -> unsigned int {
        return options.hashThreads == 0
                   ? DefaultThreadCount(ClassifyIoClass(options.source, options.destination))
                   : options.hashThreads;
    };

    FileIndex sourceIndex(caseSensitive_);
    bool sourceHashesReady = false; // comparator trusts the digests in the index

    // ---------------------------------------------------------------------
    // 1. Source side: enumerate it live or load it from a snapshot.
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
            sourceHashesReady = true;
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
    } else {
        const IFileEnumerator::ProgressCallback sourceProgress =
            [&](uint64_t files, uint64_t dirs, const std::wstring& path) {
                emitProgress(ScanPhase::EnumerateSource, files, dirs, path);
            };
        FileIndex::BuildResult build;
        const bool sourceOk = runWithFallback([&](IFileEnumerator& it) {
            build = sourceIndex.build(options.source, it, sourceProgress, options.cancel);
            return build.ok;
        });
        report.sourceOk = sourceOk;
        report.results.stats.sourceFiles = build.stats.files;
        report.results.stats.sourceDirs = build.stats.dirs;
        report.results.stats.bytesSource = build.stats.bytes;
        report.secondsEnumerateSource = NowSeconds() - t1;

        // Source enumeration errors become result entries (only the back-end
        // that actually ran reports them; a fallback rebuilds the index).
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

        // 1b. Capture the source index as a snapshot (optional). In Content mode
        // the source files are hashed first so the snapshot embeds their
        // digests; the comparison below then reuses them instead of reading the
        // source a second time.
        if (haveSnapshot && report.sourceOk) {
            if (mode == ScanMode::Content) {
                const unsigned int srcThreads = resolveHashThreads();
                hashThreadsActive = srcThreads;
                ThreadPool sourcePool(srcThreads);
                std::atomic<size_t> hits{0};
                const auto hashProgress = [&](uint64_t done, uint64_t total) {
                    emitProgress(ScanPhase::Hashing, done, total, L"");
                };
                const double th0 = NowSeconds();
                HashSourceIndex(sourceIndex, options.source, sourcePool, options.cancel,
                                cache.get(), hits, hashProgress);
                hashThreadsActive = 0;
                report.secondsHashing += NowSeconds() - th0;
                report.hashCacheHits += hits.load();
                report.hashThreadsUsed = srcThreads;
                sourceHashesReady = true;
            }
            std::wstring werr;
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

    // ---------------------------------------------------------------------
    // 2. Destination pass: enumerate + compare against the source index.
    // ---------------------------------------------------------------------
    if (haveDest && report.sourceOk && !(options.cancel && options.cancel->load())) {
        const IFileEnumerator::ProgressCallback destProgress =
            [&](uint64_t files, uint64_t dirs, const std::wstring& path) {
                emitProgress(ScanPhase::CompareDestination, files, dirs, path);
            };

        std::unique_ptr<FileComparator> comparator;
        if (mode == ScanMode::Content && sourceHashesReady) {
            comparator.reset(new FileComparator(sourceIndex, mode));
        } else {
            comparator.reset(new FileComparator(sourceIndex, mode, options.source));
        }

        const bool destOk = runWithFallback([&](IFileEnumerator& it) {
            return comparator->run(options.destination, it, report.results, destProgress,
                                   options.cancel);
        });
        report.destinationOk = destOk;

        if (mode == ScanMode::Content && destOk &&
            !(options.cancel && options.cancel->load())) {
            const unsigned int destThreads = resolveHashThreads();
            report.hashThreadsUsed = destThreads;
            hashThreadsActive = destThreads;
            ThreadPool hashPool(destThreads);
            const auto hashProgress = [&](uint64_t done, uint64_t total) {
                emitProgress(ScanPhase::Hashing, done, total, L"");
            };
            const double th1 = NowSeconds();
            comparator->runHashing(hashPool, report.results, options.cancel, hashProgress,
                                   cache.get());
            hashThreadsActive = 0;
            report.secondsHashing += NowSeconds() - th1;
            report.hashCacheHits += comparator->cacheHits();
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
    report.secondsDestinationPass = t3 - t1 - report.secondsEnumerateSource;
    report.secondsTotal = t3 - t0;

    return report;
}

} // namespace bv