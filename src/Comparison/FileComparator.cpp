#include "FileComparator.h"

#include <algorithm>

#include "Comparison/HashPhase.h"
#include "Filesystem/PathUtil.h"

namespace bv {

namespace {

constexpr uint32_t kWinErrorAccessDenied = 5; // ERROR_ACCESS_DENIED

} // namespace

bool FileComparator::run(const std::wstring& destRoot,
                         IFileEnumerator& destEnumerator,
                         ResultSet& out,
                         const IFileEnumerator::ProgressCallback& onProgress,
                         const std::atomic_bool* cancel) {
    std::vector<std::wstring> extraFolded; // every extra path (files + dirs), folded

    destRoot_ = destRoot;
    const bool destOk = destEnumerator.enumerate(
        destRoot,
        [&](FileEntry&& e) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                return false; // stop the comparison early
            }
            if (e.isDirectory) {
                ++out.stats.destDirs;
            } else {
                ++out.stats.destFiles;
                out.stats.bytesDest += e.size;
            }

            FileEntry src;
            if (source_.tryErase(e.relativePath, src)) {
                classifyMatched(src, e, out);
            } else {
                FileResult r;
                r.status = Status::Extra;
                r.relativePath = std::move(e.relativePath);
                r.sizeDest = e.size;
                r.isDirectory = e.isDirectory;
                extraFolded.push_back(pathutil::FoldForCompare(r.relativePath));
                if (e.isDirectory) {
                    ++out.stats.extraDirs;
                } else {
                    ++out.stats.extraFiles;
                }
                out.problems.push_back(std::move(r));
            }
            return true;
        },
        [&](const ScanError& err) {
            FileResult r;
            r.isDirectory = true; // errors occur on directories we cannot read
            r.relativePath = err.path;
            r.errorMessage = err.message;
            if (err.winError == kWinErrorAccessDenied) {
                r.status = Status::AccessDenied;
                ++out.stats.accessDenied;
            } else {
                r.status = Status::ReadError;
                ++out.stats.readErrors;
            }
            out.problems.push_back(std::move(r));
        },
        onProgress);

    if (!destOk) {
        // Root error: already recorded through the error callback; reporting
        // the whole source as "missing" would be misleading.
        return false;
    }

    // Only report an extra directory if it has no extra descendant: if the
    // whole subtree is extra, its children are reported individually and a
    // "directory missing" line would just be noise. Empty extra dirs matter.
    if (!extraFolded.empty()) {
        std::sort(extraFolded.begin(), extraFolded.end());
        extraFolded.erase(std::unique(extraFolded.begin(), extraFolded.end()), extraFolded.end());

        std::vector<FileResult> filtered;
        filtered.reserve(out.problems.size());
        for (FileResult& r : out.problems) {
            const bool redundantDir =
                r.status == Status::Extra && r.isDirectory &&
                pathutil::HasDescendant(extraFolded, pathutil::FoldForCompare(r.relativePath));
            if (!redundantDir) {
                filtered.push_back(std::move(r));
            }
        }
        out.problems = std::move(filtered);
    }

    recordMissing(out);
    return true;
}

void FileComparator::classifyMatched(FileEntry& src, FileEntry& dst, ResultSet& out) {
    const bool srcDir = src.isDirectory;
    const bool dstDir = dst.isDirectory;

    if (srcDir && dstDir) {
        ++out.stats.identicalDirs;
        return;
    }
    if (srcDir != dstDir) {
        // File where a directory is expected (or vice versa): definitely
        // different, classified as a size/type mismatch.
        ++out.stats.sizeMismatch;
        FileResult r;
        r.status = Status::SizeMismatch;
        r.relativePath = std::move(dst.relativePath);
        r.sizeSource = src.size;
        r.sizeDest = dst.size;
        r.isDirectory = false;
        out.problems.push_back(std::move(r));
        return;
    }

    switch (mode_) {
        case ScanMode::Presence:
            ++out.stats.identicalFiles;
            break;
        case ScanMode::Size:
            if (src.size == dst.size) {
                ++out.stats.identicalFiles;
            } else {
                ++out.stats.sizeMismatch;
                FileResult r;
                r.status = Status::SizeMismatch;
                r.relativePath = std::move(dst.relativePath);
                r.sizeSource = src.size;
                r.sizeDest = dst.size;
                r.isDirectory = false;
                out.problems.push_back(std::move(r));
            }
            break;
        case ScanMode::Content:
            if (src.size == dst.size) {
                // Same path + size: defer to the hash phase. The relative path
                // is taken from `dst` before either entry is moved/used later.
                ContentCandidate hp;
                hp.relativePath = dst.relativePath;
                hp.sizeSource = src.size;
                hp.sizeDest = dst.size;
                hp.srcMtime = src.lastWriteTime; // for change-detection + cache key
                hp.dstMtime = dst.lastWriteTime;
                pendingHashes_.push_back(std::move(hp));
            } else {
                ++out.stats.sizeMismatch;
                FileResult r;
                r.status = Status::SizeMismatch;
                r.relativePath = dst.relativePath;
                r.sizeSource = src.size;
                r.sizeDest = dst.size;
                r.isDirectory = false;
                out.problems.push_back(std::move(r));
            }
            break;
    }
}

void FileComparator::runHashing(
    ThreadPool& pool, ResultSet& out, const std::atomic_bool* cancel,
    const std::function<void(uint64_t done, uint64_t total)>& onProgress,
    hashing::HashCache* cache) {
    cacheHits_.store(0, std::memory_order_relaxed);
    const bool offline = sourceRoot_.empty(); // digests live in the index
    RunHashPhase(pendingHashes_, pool, offline, &source_, sourceRoot_, destRoot_, out, cancel,
                 onProgress, cache, cacheHits_);
}

void FileComparator::recordMissing(ResultSet& out) {
    struct Item {
        std::wstring key;
        const FileEntry* entry;
    };

    std::vector<Item> items;
    items.reserve(source_.size());
    for (const auto& kv : source_.entries()) {
        items.push_back({kv.first, &kv.second});
    }
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.key < b.key; });

    std::vector<std::wstring> keys;
    keys.reserve(items.size());
    for (const auto& it : items) keys.push_back(it.key);

    for (const auto& it : items) {
        const FileEntry& e = *it.entry;
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
    for (const auto& it : items) {
        if (!it.entry->isDirectory) continue;
        if (pathutil::HasDescendant(keys, it.key)) continue;
        FileResult r;
        r.status = Status::Missing;
        r.relativePath = it.entry->relativePath;
        r.isDirectory = true;
        out.problems.push_back(std::move(r));
    }
}

} // namespace bv
