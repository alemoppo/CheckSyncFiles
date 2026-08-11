#include "ScanController.h"

#include <chrono>
#include <functional>
#include <memory>

#include "Comparison/FileComparator.h"
#include "Filesystem/FileIndex.h"
#include "Filesystem/MftEnumerator.h"
#include "Filesystem/Win32Enumerator.h"
#include "Threading/IoClass.h"
#include "Threading/ThreadPool.h"

namespace bv {

namespace {

double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

ScanReport ScanController::run(const ScanOptions& options) {
    ScanReport report;
    report.backendUsed = EnumeratorBackend::Win32;

    const double t0 = NowSeconds();

    FileIndex sourceIndex(caseSensitive_);

    // Pick the enumeration back-end (MFT scans the raw NTFS index and falls
    // back to Win32 if it cannot run).
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

    // Run a pass with the primary back-end; if it cannot enumerate (returns
    // false, i.e. the mount is not NTFS / $MFT unreadable), retry with Win32.
    const auto runWithFallback = [&](const std::function<bool(IFileEnumerator&)>& pass) -> bool {
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

    const auto emitProgress = [&](ScanPhase phase, uint64_t files, uint64_t dirs,
                                  const std::wstring& path) {
        if (options.onProgress) {
            ScanProgress p;
            p.phase = phase;
            p.files = files;
            p.dirs = dirs;
            p.currentPath = path;
            options.onProgress(p);
        }
    };

    const double t1 = NowSeconds();
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
    const double t2 = NowSeconds();
    report.secondsEnumerateSource = t2 - t1;

    // Source enumeration errors become result entries (only the back-end that
    // actually ran reports them; a fallback rebuilds the index from scratch).
    for (const ScanError& err : build.errors) {
        FileResult r;
        r.status = (err.winError == 5) ? Status::AccessDenied : Status::ReadError;
        r.relativePath = err.path;
        r.errorMessage = err.message;
        r.isDirectory = true;
        report.results.problems.push_back(std::move(r));
        if (r.status == Status::AccessDenied) {
            ++report.results.stats.accessDenied;
        } else {
            ++report.results.stats.readErrors;
        }
    }

    // Resolve the hash-pool size up front so the caller can show the "threads
    // actually launched" number even for auto. A pool is only created in
    // Content mode; otherwise nothing is launched.
    const unsigned int hashThreads =
        (options.mode == ScanMode::Content)
            ? (options.hashThreads == 0
                   ? DefaultThreadCount(ClassifyIoClass(options.source, options.destination))
                   : options.hashThreads)
            : 0;
    report.hashThreadsUsed = hashThreads;

    FileComparator comparator(sourceIndex, options.mode, options.source);
    const IFileEnumerator::ProgressCallback destProgress =
        [&](uint64_t files, uint64_t dirs, const std::wstring& path) {
            emitProgress(ScanPhase::CompareDestination, files, dirs, path);
        };
    const bool destOk = runWithFallback([&](IFileEnumerator& it) {
        return comparator.run(options.destination, it, report.results, destProgress,
                              options.cancel);
    });
    report.destinationOk = destOk;
    if (options.cancel && options.cancel->load()) {
        report.sourceOk = false;
    }

    // Content verification (Phase 3): hash every same-path+size candidate pair.
    if (options.mode == ScanMode::Content && destOk &&
        !(options.cancel && options.cancel->load())) {
        ThreadPool hashPool(hashThreads);
        const auto hashProgress = [&](uint64_t done, uint64_t total) {
            emitProgress(ScanPhase::Hashing, done, total, L"");
        };
        const double th0 = NowSeconds();
        comparator.runHashing(hashPool, report.results, options.cancel, hashProgress);
        report.secondsHashing = NowSeconds() - th0;
    }

    if (options.onProgress) {
        ScanProgress p;
        p.phase = ScanPhase::Done;
        p.files = report.results.stats.sourceFiles;
        p.dirs = report.results.stats.sourceDirs;
        options.onProgress(p);
    }

    const double t3 = NowSeconds();
    report.secondsDestinationPass = t3 - t2;
    report.secondsTotal = t3 - t0;

    return report;
}

} // namespace bv
