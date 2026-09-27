#include "Comparison/ClassifyUtil.h"

#include "Filesystem/PathUtil.h"

namespace bv {

bool ClassifyMatched(const FileEntry& src, const FileEntry& dst, ScanMode mode,
                     ConcurrentSink& sink, std::vector<ContentCandidate>& candidates,
                     const std::wstring& destRoot, IRowSink* rowSink) {
    auto& stats = sink.stats();
    const auto inc = [&stats](std::atomic<uint64_t>& c) {
        c.fetch_add(1, std::memory_order_relaxed);
    };
    // Finalized-row observer (session capture). Content candidates are NOT
    // final: they are observed by the hash phase once digests exist.
    const auto emit = [&](Status verdict, bool isDir) {
        if (!rowSink) return;
        ClassifiedRow row;
        row.relativePath = dst.relativePath;
        row.hasA = true;
        row.hasB = true;
        row.entryA = src;
        row.entryB = dst;
        row.verdict = verdict;
        row.isDirectory = isDir;
        rowSink->onRow(std::move(row));
    };

    const bool srcDir = src.isDirectory;
    const bool dstDir = dst.isDirectory;

    if (srcDir && dstDir) {
        inc(stats.identicalDirs);
        emit(Status::Identical, true);
        return false;
    }
    if (srcDir != dstDir) {
        // File where a directory is expected (or vice versa): definitely
        // different, classified as a size/type mismatch.
        inc(stats.sizeMismatch);
        FileResult r;
        r.status = Status::SizeMismatch;
        r.fullPath = pathutil::MakeAbsolute(destRoot, dst.relativePath);
        r.relativePath = dst.relativePath;
        r.sizeSource = src.size;
        r.sizeDest = dst.size;
        r.isDirectory = false;
        sink.addProblem(std::move(r));
        emit(Status::SizeMismatch, false);
        return false;
    }

    auto recordSizeMismatch = [&](const FileEntry& s, const FileEntry& d) {
        inc(stats.sizeMismatch);
        FileResult r;
        r.status = Status::SizeMismatch;
        r.fullPath = pathutil::MakeAbsolute(destRoot, d.relativePath);
        r.relativePath = d.relativePath;
        r.sizeSource = s.size;
        r.sizeDest = d.size;
        r.isDirectory = false;
        sink.addProblem(std::move(r));
        emit(Status::SizeMismatch, false);
    };

    switch (mode) {
        case ScanMode::Presence:
            inc(stats.identicalFiles);
            emit(Status::Identical, false);
            break;
        case ScanMode::Size:
            if (src.size == dst.size) {
                inc(stats.identicalFiles);
                emit(Status::Identical, false);
            } else {
                recordSizeMismatch(src, dst);
            }
            break;
        case ScanMode::Content:
            if (src.size == dst.size) {
                // Same path + size: defer to the hash phase. The relative path
                // is taken from `dst` before either entry is used afterwards.
                ContentCandidate c;
                c.relativePath = dst.relativePath;
                c.sizeSource = src.size;
                c.sizeDest = dst.size;
                c.srcMtime = src.lastWriteTime; // for change detection + cache key
                c.dstMtime = dst.lastWriteTime;
                candidates.push_back(std::move(c));
                return true; // a content candidate was added
            } else {
                recordSizeMismatch(src, dst);
            }
            break;
    }
    return false; // no content candidate added
}

} // namespace bv