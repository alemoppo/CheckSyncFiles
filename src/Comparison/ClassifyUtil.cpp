#include "Comparison/ClassifyUtil.h"

namespace bv {

bool ClassifyMatched(const FileEntry& src, const FileEntry& dst, ScanMode mode,
                     ConcurrentSink& sink, std::vector<ContentCandidate>& candidates) {
    auto& stats = sink.stats();
    const auto inc = [&stats](std::atomic<uint64_t>& c) {
        c.fetch_add(1, std::memory_order_relaxed);
    };

    const bool srcDir = src.isDirectory;
    const bool dstDir = dst.isDirectory;

    if (srcDir && dstDir) {
        inc(stats.identicalDirs);
        return false;
    }
    if (srcDir != dstDir) {
        // File where a directory is expected (or vice versa): definitely
        // different, classified as a size/type mismatch.
        inc(stats.sizeMismatch);
        FileResult r;
        r.status = Status::SizeMismatch;
        r.relativePath = dst.relativePath;
        r.sizeSource = src.size;
        r.sizeDest = dst.size;
        r.isDirectory = false;
        sink.addProblem(std::move(r));
        return false;
    }

    auto recordSizeMismatch = [&](const FileEntry& s, const FileEntry& d) {
        inc(stats.sizeMismatch);
        FileResult r;
        r.status = Status::SizeMismatch;
        r.relativePath = d.relativePath;
        r.sizeSource = s.size;
        r.sizeDest = d.size;
        r.isDirectory = false;
        sink.addProblem(std::move(r));
    };

    switch (mode) {
        case ScanMode::Presence:
            inc(stats.identicalFiles);
            break;
        case ScanMode::Size:
            if (src.size == dst.size) {
                inc(stats.identicalFiles);
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