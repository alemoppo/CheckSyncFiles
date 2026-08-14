#include "Comparison/ConcurrentComparer.h"

#include <algorithm>
#include <thread>
#include <utility>

#include "Filesystem/MftEnumerator.h"
#include "Filesystem/PathUtil.h"
#include "Filesystem/Win32Enumerator.h"

namespace bv {

namespace {

constexpr uint32_t kWinErrorAccessDenied = 5; // ERROR_ACCESS_DENIED

std::wstring CanonicalKey(const std::wstring& relativePath, bool caseSensitive) {
    return caseSensitive ? relativePath : pathutil::FoldForCompare(relativePath);
}

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

std::unique_ptr<IFileEnumerator> MakeWin32() {
    return std::unique_ptr<IFileEnumerator>(new Win32Enumerator());
}
std::unique_ptr<IFileEnumerator> MakeMft() {
    return std::unique_ptr<IFileEnumerator>(new MftEnumerator());
}

} // namespace

ConcurrentComparer::Result ConcurrentComparer::run(
    ThreadPool& hashPool, const ProgressCallback& onProgress,
    const HashProgressCallback& onHashProgress, hashing::HashCache* cache) {
    std::vector<EnumeratorFactory> srcFactories;
    std::vector<EnumeratorFactory> dstFactories;
    if (acceptMft_) {
        srcFactories = {MakeMft, MakeWin32};
        dstFactories = {MakeMft, MakeWin32};
    } else {
        srcFactories = {MakeWin32};
        dstFactories = {MakeWin32};
    }
    return runImpl(std::move(srcFactories), std::move(dstFactories), hashPool, onProgress,
                   onHashProgress, cache);
}

ConcurrentComparer::Result ConcurrentComparer::runWithFactories(
    EnumeratorFactory sourceFactory, EnumeratorFactory destFactory, ThreadPool& hashPool,
    const ProgressCallback& onProgress, const HashProgressCallback& onHashProgress,
    hashing::HashCache* cache) {
    std::vector<EnumeratorFactory> src;
    src.push_back(std::move(sourceFactory));
    std::vector<EnumeratorFactory> dst;
    dst.push_back(std::move(destFactory));
    return runImpl(std::move(src), std::move(dst), hashPool, onProgress, onHashProgress, cache);
}

ConcurrentComparer::Result ConcurrentComparer::runWithFactories(
    std::vector<EnumeratorFactory> sourceFactories,
    std::vector<EnumeratorFactory> destFactories, ThreadPool& hashPool,
    const ProgressCallback& onProgress, const HashProgressCallback& onHashProgress,
    hashing::HashCache* cache) {
    return runImpl(std::move(sourceFactories), std::move(destFactories), hashPool, onProgress,
                   onHashProgress, cache);
}

ConcurrentComparer::Result ConcurrentComparer::runImpl(
    std::vector<EnumeratorFactory> sourceFactories,
    std::vector<EnumeratorFactory> destFactories, ThreadPool& hashPool,
    const ProgressCallback& onProgress, const HashProgressCallback& onHashProgress,
    hashing::HashCache* cache) {
    onProgress_ = onProgress;
    onHashProgress_ = onHashProgress;
    cacheHits_.store(0, std::memory_order_relaxed);
    totalFiles_.store(0, std::memory_order_relaxed);
    totalDirs_.store(0, std::memory_order_relaxed);

    MatchTable table(6);
    table.setCancel(cancel_); // set once here, before any worker starts (see setCancel)
    ConcurrentSink sink;
    std::vector<ContentCandidate> candidatesA;
    std::vector<ContentCandidate> candidatesB;
    WorkerState stateA;
    WorkerState stateB;
    WorkerStatus sourceStatus = WorkerStatus::Failed;
    WorkerStatus destStatus = WorkerStatus::Failed;

    std::thread workerA([&] {
        if (sourceKind_ == SourceKind::FromIndex) {
            sourceStatus = runIndexWorker(table, sink, candidatesA);
        } else {
            sourceStatus = runEnumWorker(0, sourceFactories, table, sink, candidatesA, stateA);
        }
    });
    std::thread workerB([&] {
        destStatus = runEnumWorker(1, destFactories, table, sink, candidatesB, stateB);
    });
    workerA.join();
    workerB.join();

    Result r;
    r.sourceStatus = sourceStatus;
    r.destinationStatus = destStatus;

    const bool clean = sourceStatus == WorkerStatus::Success && destStatus == WorkerStatus::Success;
    ResultSet post; // results produced after both workers have stopped

    if (clean) {
        if (mode_ == ScanMode::Content) {
            std::vector<ContentCandidate> all;
            all.reserve(candidatesA.size() + candidatesB.size());
            for (auto& c : candidatesA) all.push_back(std::move(c));
            for (auto& c : candidatesB) all.push_back(std::move(c));

            const bool offline = (sourceKind_ == SourceKind::FromIndex);
            RunHashPhase(all, hashPool, offline, offline ? fromIndex_ : nullptr, sourceRoot_,
                         destRoot_, post, cancel_, onHashProgress_, cache, cacheHits_);
        }
        finalizeMissingExtra(table, post);
    }

    r.results = sink.take();
    AddStats(r.results.stats, post.stats);
    for (FileResult& p : post.problems) r.results.problems.push_back(std::move(p));
    sortProblems(r.results);
    return r;
}

void ConcurrentComparer::onEntry(int side, FileEntry e, MatchTable& table, ConcurrentSink& sink,
                                 std::vector<ContentCandidate>& candidates) {
    auto& stats = sink.stats();
    const auto inc = [&stats](std::atomic<uint64_t>& c) {
        c.fetch_add(1, std::memory_order_relaxed);
    };
    if (e.isDirectory) {
        inc(side == 0 ? stats.sourceDirs : stats.destDirs);
    } else {
        inc(side == 0 ? stats.sourceFiles : stats.destFiles);
        (side == 0 ? stats.bytesSource : stats.bytesDest)
            .fetch_add(e.size, std::memory_order_relaxed);
    }

    const std::wstring key = CanonicalKey(e.relativePath, caseSensitive_);
    FileEntry peer;
    const MatchTable::Outcome outcome = table.insert(key, side, std::move(e), peer);
    if (outcome != MatchTable::Outcome::Matched) return;

    // The shard lock is released: classify outside any table lock.
    FileEntry src;
    FileEntry dst;
    if (side == 0) {
        src = std::move(e);
        dst = std::move(peer);
    } else {
        src = std::move(peer);
        dst = std::move(e);
    }
    ClassifyMatched(src, dst, mode_, sink, candidates);
}

void ConcurrentComparer::onError(const ScanError& err, ConcurrentSink& sink) {
    FileResult r;
    r.isDirectory = true; // errors occur on directories we cannot read
    r.relativePath = err.path;
    r.errorMessage = err.lostDevice
                         ? std::wstring(L"dispositivo scollegato durante l'operazione (riverificare)")
                         : err.message;
    auto& stats = sink.stats();
    if (err.winError == kWinErrorAccessDenied) {
        r.status = Status::AccessDenied;
        stats.accessDenied.fetch_add(1, std::memory_order_relaxed);
    } else {
        r.status = Status::ReadError;
        stats.readErrors.fetch_add(1, std::memory_order_relaxed);
    }
    sink.addProblem(std::move(r));
}

ConcurrentComparer::WorkerStatus ConcurrentComparer::runEnumWorker(
    int side, const std::vector<EnumeratorFactory>& factories, MatchTable& table,
    ConcurrentSink& sink, std::vector<ContentCandidate>& candidates, WorkerState& state) {
    bool canceled = false;
    const std::wstring& root = (side == 0) ? sourceRoot_ : destRoot_;

    for (const auto& factory : factories) {
        if (canceled) {
            table.setSideDone(side);
            return WorkerStatus::Cancelled;
        }
        std::unique_ptr<IFileEnumerator> enumerator = factory();
        if (!enumerator) continue;

        bool emitted = false;
        const bool ok = enumerator->enumerate(
            root,
            [&](FileEntry&& e) -> bool {
                if (cancel_ && cancel_->load(std::memory_order_relaxed)) {
                    canceled = true;
                    return false; // stop this side early (same as the serial path)
                }
                emitted = true;
                onEntry(side, std::move(e), table, sink, candidates);
                return true;
            },
            [&](const ScanError& err) { onError(err, sink); },
            [&](uint64_t files, uint64_t dirs, const std::wstring& path) {
                totalFiles_.fetch_add(files - state.lastFiles, std::memory_order_relaxed);
                state.lastFiles = files;
                totalDirs_.fetch_add(dirs - state.lastDirs, std::memory_order_relaxed);
                state.lastDirs = dirs;
                if (onProgress_) {
                    onProgress_(totalFiles_.load(std::memory_order_relaxed),
                                totalDirs_.load(std::memory_order_relaxed), path);
                }
            },
            cancel_);

        if (ok) {
            table.setSideDone(side);
            // An enumerator that polls `cancel` itself may abort before the entry
            // callback ever fires and still return true; such a side was not
            // scanned and must be reported Cancelled, never as a clean Success.
            if (canceled ||
                (cancel_ && cancel_->load(std::memory_order_relaxed))) {
                return WorkerStatus::Cancelled;
            }
            return WorkerStatus::Success;
        }
        // The enumerator failed. If it emited entries before failing (an MFT
        // scan that went incomplete mid-walk), the result is not trustworthy:
        // fall back is impossible because entries already streamed into the
        // table may have matched the other side. The side is marked failed and
        // nothing is reported as missing/extra.
        if (emitted) {
            table.setSideDone(side);
            return WorkerStatus::Failed;
        }
        // Pre-stream failure with zero entries (root access, not NTFS, ...):
        // safe to try the next enumerator (MFT -> Win32 fallback).
    }

    table.setSideDone(side);
    return canceled ? WorkerStatus::Cancelled : WorkerStatus::Failed;
}

ConcurrentComparer::WorkerStatus ConcurrentComparer::runIndexWorker(
    MatchTable& table, ConcurrentSink& sink, std::vector<ContentCandidate>& candidates) {
    const bool canceled = cancel_ && cancel_->load(std::memory_order_relaxed);
    if (!canceled) {
        for (const auto& kv : fromIndex_->entries()) {
            if (cancel_ && cancel_->load(std::memory_order_relaxed)) break;
            // Copy the entry; the index stays untouched (matches are side-removed
            // only from the table).
            onEntry(0, kv.second, table, sink, candidates);
        }
    }
    table.setSideDone(0);
    return cancel_ && cancel_->load(std::memory_order_relaxed) ? WorkerStatus::Cancelled
                                                              : WorkerStatus::Success;
}

void ConcurrentComparer::finalizeMissingExtra(MatchTable& table, ResultSet& out) {
    auto remaining = table.remaining();

    std::vector<std::pair<std::wstring, FileEntry>> sourceItems;
    std::vector<std::pair<std::wstring, FileEntry>> destItems;
    sourceItems.reserve(remaining.size());
    destItems.reserve(remaining.size());
    for (auto& kv : remaining) {
        std::wstring key = CanonicalKey(kv.second.relativePath, caseSensitive_);
        if (kv.first == 0) {
            sourceItems.emplace_back(std::move(key), std::move(kv.second));
        } else {
            destItems.emplace_back(std::move(key), std::move(kv.second));
        }
    }
    auto byKey = [](const std::pair<std::wstring, FileEntry>& a,
                    const std::pair<std::wstring, FileEntry>& b) { return a.first < b.first; };
    std::sort(sourceItems.begin(), sourceItems.end(), byKey);
    std::sort(destItems.begin(), destItems.end(), byKey);

    std::vector<std::wstring> sourceKeys;
    sourceKeys.reserve(sourceItems.size());
    for (const auto& it : sourceItems) sourceKeys.push_back(it.first);

    // Missing: present only in source.
    for (const auto& it : sourceItems) {
        const FileEntry& e = it.second;
        if (e.isDirectory) {
            ++out.stats.missingDirs;
        } else {
            ++out.stats.missingFiles;
            FileResult r;
            r.status = Status::Missing;
            r.relativePath = e.relativePath;
            r.sizeSource = e.size;
            r.isDirectory = false;
            out.problems.push_back(std::move(r));
        }
    }
    // Report only empty missing directories (children are reported separately).
    for (const auto& it : sourceItems) {
        const FileEntry& e = it.second;
        if (!e.isDirectory) continue;
        if (pathutil::HasDescendant(sourceKeys, it.first)) continue;
        FileResult r;
        r.status = Status::Missing;
        r.relativePath = e.relativePath;
        r.isDirectory = true;
        out.problems.push_back(std::move(r));
    }

    // Extra: present only in destination.
    std::vector<std::wstring> extraFolded;
    extraFolded.reserve(destItems.size());
    for (const auto& it : destItems) {
        const FileEntry& e = it.second;
        if (e.isDirectory) {
            ++out.stats.extraDirs;
        } else {
            ++out.stats.extraFiles;
        }
        extraFolded.push_back(pathutil::FoldForCompare(e.relativePath));
        FileResult r;
        r.status = Status::Extra;
        r.relativePath = e.relativePath;
        r.sizeDest = e.size;
        r.isDirectory = e.isDirectory;
        out.problems.push_back(std::move(r));
    }
    // Suppress extra directories whose whole subtree is already reported.
    if (!extraFolded.empty()) {
        std::sort(extraFolded.begin(), extraFolded.end());
        extraFolded.erase(std::unique(extraFolded.begin(), extraFolded.end()),
                          extraFolded.end());
        std::vector<FileResult> filtered;
        filtered.reserve(out.problems.size());
        for (FileResult& r : out.problems) {
            const bool redundantDir =
                r.status == Status::Extra && r.isDirectory &&
                pathutil::HasDescendant(extraFolded, pathutil::FoldForCompare(r.relativePath));
            if (!redundantDir) filtered.push_back(std::move(r));
        }
        out.problems = std::move(filtered);
    }
}

void ConcurrentComparer::sortProblems(ResultSet& out) {
    if (out.problems.size() < 2) return;
    std::vector<std::pair<std::wstring, FileResult>> items;
    items.reserve(out.problems.size());
    for (FileResult& r : out.problems) {
        items.emplace_back(CanonicalKey(r.relativePath, caseSensitive_), std::move(r));
    }
    std::sort(items.begin(), items.end(),
              [](const std::pair<std::wstring, FileResult>& a,
                 const std::pair<std::wstring, FileResult>& b) { return a.first < b.first; });
    out.problems.clear();
    out.problems.reserve(items.size());
    for (auto& it : items) out.problems.push_back(std::move(it.second));
}

} // namespace bv
