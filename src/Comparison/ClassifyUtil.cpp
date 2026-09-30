#include "Comparison/ClassifyUtil.h"

#include "Filesystem/PathUtil.h"
#include "Hashing/Sha256.h"

namespace bv {

namespace {

bool IsSupportedLink(ReparseKind k) {
    return k == ReparseKind::SymlinkFile || k == ReparseKind::SymlinkDir ||
           k == ReparseKind::Junction;
}

} // namespace

LinkDecision TryClassifyLinks(const FileEntry& src, const FileEntry& dst,
                              const std::wstring& sourceRoot,
                              const std::wstring& destRoot) {
    LinkDecision d;
    if (src.reparseKind == ReparseKind::None && dst.reparseKind == ReparseKind::None)
        return d;
    d.handled = true;
    d.row.relativePath = dst.relativePath;
    d.row.fullPath = pathutil::MakeAbsolute(destRoot, dst.relativePath);
    d.row.isDirectory = false;
    d.row.reparseKind = dst.reparseKind;

    if (src.reparseKind != dst.reparseKind) {
        // Link vs plain entry (or link-kind change): a type mismatch.
        d.row.status = Status::SizeMismatch;
        d.row.sizeSource = src.size;
        d.row.sizeDest = dst.size;
        return d;
    }
    if (!IsSupportedLink(src.reparseKind) || src.linkTarget.empty() || dst.linkTarget.empty()) {
        d.row.status = Status::ReadError;
        d.row.reparseKind = ReparseKind::Other;
        d.row.errorMessage = L"reparse point non supportato dal confronto";
        return d;
    }
    // Mirrored absolute targets (A\real vs B\real) compare equal; anything
    // else compares verbatim. Digests/sizes use the normalized form so both
    // sides share the same basis.
    const std::wstring srcNorm = NormalizeLinkTargetForCompare(src.linkTarget, sourceRoot);
    const std::wstring dstNorm = NormalizeLinkTargetForCompare(dst.linkTarget, destRoot);
    if (srcNorm == dstNorm) {
        d.identical = true;
        d.identicalIsDir = src.isDirectory && dst.isDirectory;
        return d;
    }
    // Different targets: ContentMismatch carrying sha256(target) digests, so
    // the row has the same shape as a content mismatch (sizes = target
    // lengths, never file bytes: targets are never followed).
    d.row.status = Status::ContentMismatch;
    const std::string srcU8 = pathutil::ToUtf8(srcNorm);
    const std::string dstU8 = pathutil::ToUtf8(dstNorm);
    d.row.sizeSource = srcU8.size();
    d.row.sizeDest = dstU8.size();
    d.row.hasHashSource = hashing::Sha256Bytes(srcU8.data(), srcU8.size(), d.row.hashSource);
    d.row.hasHashDest = hashing::Sha256Bytes(dstU8.data(), dstU8.size(), d.row.hashDest);
    return d;
}

bool ClassifyMatched(const FileEntry& src, const FileEntry& dst, ScanMode mode,
                     ConcurrentSink& sink, std::vector<ContentCandidate>& candidates,
                     const std::wstring& destRoot, const std::wstring& sourceRoot,
                     IRowSink* rowSink) {
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

    // Links are tree elements compared by (kind, target), never by content.
    if (LinkDecision link = TryClassifyLinks(src, dst, sourceRoot, destRoot); link.handled) {
        if (link.identical) {
            if (link.identicalIsDir) {
                inc(stats.identicalDirs);
                emit(Status::Identical, true);
            } else {
                inc(stats.identicalFiles);
                emit(Status::Identical, false);
            }
        } else {
            switch (link.row.status) {
                case Status::ContentMismatch: inc(stats.contentMismatch); break;
                case Status::ReadError: inc(stats.readErrors); break;
                default: inc(stats.sizeMismatch); break;
            }
            const Status verdict = link.row.status;
            sink.addProblem(std::move(link.row));
            emit(verdict, false);
        }
        return false;
    }

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