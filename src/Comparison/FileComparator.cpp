#include "FileComparator.h"

#include <algorithm>

#include "Filesystem/PathUtil.h"
#include "Hashing/Sha256.h"

namespace bv {

namespace {

constexpr size_t kHashBatch = 256; // pairs hashed per thread-pool batch

} // namespace

namespace {

using Digest = std::array<uint8_t, 32>;

// Hashes one side (source or destination) of a candidate pair, honouring the
// optional persistent cache and detecting changes between enumeration time
// (`expectedSize`/`expectedMtime`, captured from the entry) and the moment the
// file is actually read:
//   - current stat != expected          -> `changed` (written while we waited)
//   - file mutated between pre- and post-hash stats -> `changed`
// A cache hit under the *current* (size, mtime) key is always sound: the cached
// digest was computed for exactly the current file state.
void HashOneSide(const std::wstring& absPath, uint64_t expectedSize, uint64_t expectedMtime,
                 bool& changed, hashing::HashStatus& status, Digest& digest, bool valid,
                 hashing::HashCache* cache, std::atomic<size_t>& cacheHits) {
    if (!valid) {
        status = hashing::HashStatus::ReadError;
        return;
    }
    uint64_t sz = 0;
    uint64_t mt = 0;
    if (!hashing::StatFile(absPath, sz, mt)) {
        status = hashing::HashStatus::NoAccess;
        return;
    }
    changed = (sz != expectedSize) || (mt != expectedMtime);

    if (cache && cache->Lookup(absPath, sz, mt, digest)) {
        status = hashing::HashStatus::Ok;
        cacheHits.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    status = hashing::Sha256File(absPath, digest);
    if (status != hashing::HashStatus::Ok) return;

    uint64_t szAfter = 0;
    uint64_t mtAfter = 0;
    if (hashing::StatFile(absPath, szAfter, mtAfter)) {
        if (szAfter != sz || mtAfter != mt) changed = true;
    }
    if (cache) cache->Store(absPath, sz, mt, digest);
}

} // namespace

namespace {

constexpr uint32_t kWinErrorAccessDenied = 5; // ERROR_ACCESS_DENIED

bool StartsWith(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// True if the sorted `keys` list contains an entry that is a strict descendant
// of `dirKey` (i.e. starts with dirKey + "\\").
bool HasDescendant(const std::vector<std::wstring>& keys, const std::wstring& dirKey) {
    const std::wstring prefix = dirKey + L"\\";
    auto it = std::lower_bound(keys.begin(), keys.end(), prefix);
    for (; it != keys.end() && StartsWith(*it, prefix); ++it) {
        return true;
    }
    return false;
}

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
                HasDescendant(extraFolded, pathutil::FoldForCompare(r.relativePath));
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
                HashPair hp;
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
    struct Outcome {
        hashing::HashStatus srcStatus = hashing::HashStatus::ReadError;
        hashing::HashStatus dstStatus = hashing::HashStatus::ReadError;
        bool equal = false;
        bool changed = false;
        bool hasSrc = false;
        bool hasDst = false;
        Digest srcDigest{};
        Digest dstDigest{};
    };

    cacheHits_.store(0, std::memory_order_relaxed);
    const bool offline = sourceRoot_.empty(); // digests live in the index

    const size_t total = pendingHashes_.size();
    size_t done = 0;
    while (done < total) {
        if (cancel && cancel->load(std::memory_order_relaxed)) break;

        const size_t n = std::min(kHashBatch, total - done);
        std::vector<Outcome> outcomes(n);
        for (size_t i = 0; i < n; ++i) {
            const HashPair& hp = pendingHashes_[done + i];
            pool.submit([this, &hp, &o = outcomes[i], cache, offline] {
                if (offline) {
                    // Source device absent: use the digest captured in the snapshot.
                    o.hasSrc = source_.getHash(hp.relativePath, o.srcDigest);
                    o.srcStatus = o.hasSrc ? hashing::HashStatus::Ok : hashing::HashStatus::ReadError;
                } else {
                    HashOneSide(pathutil::MakeAbsolute(sourceRoot_, hp.relativePath),
                                hp.sizeSource, hp.srcMtime, o.changed, o.srcStatus, o.srcDigest,
                                true, cache, cacheHits_);
                    o.hasSrc = (o.srcStatus == hashing::HashStatus::Ok);
                }
                bool dstChanged = false;
                HashOneSide(pathutil::MakeAbsolute(destRoot_, hp.relativePath),
                            hp.sizeDest, hp.dstMtime, dstChanged, o.dstStatus, o.dstDigest,
                            true, cache, cacheHits_);
                o.hasDst = (o.dstStatus == hashing::HashStatus::Ok);
                o.changed = o.changed || dstChanged;
                o.equal = (o.srcStatus == hashing::HashStatus::Ok &&
                           o.dstStatus == hashing::HashStatus::Ok && o.srcDigest == o.dstDigest);
            });
        }
        pool.waitAll();

        for (size_t i = 0; i < n; ++i) {
            const HashPair& hp = pendingHashes_[done + i];
            const Outcome& o = outcomes[i];

            if (o.changed) {
                ++out.stats.changedDuringScan;
                FileResult r;
                r.status = Status::ChangedDuringScan;
                r.relativePath = hp.relativePath;
                r.sizeSource = hp.sizeSource;
                r.sizeDest = hp.sizeDest;
                r.isDirectory = false;
                r.errorMessage = L"file modificato durante la scansione (riverificare)";
                out.problems.push_back(std::move(r));
                continue;
            }

            if (o.srcStatus == hashing::HashStatus::Ok &&
                o.dstStatus == hashing::HashStatus::Ok) {
                if (o.equal) {
                    ++out.stats.identicalFiles;
                } else {
                    ++out.stats.contentMismatch;
                    FileResult r;
                    r.status = Status::ContentMismatch;
                    r.relativePath = hp.relativePath;
                    r.sizeSource = hp.sizeSource;
                    r.sizeDest = hp.sizeDest;
                    r.isDirectory = false;
                    r.hasHashSource = true;
                    r.hasHashDest = true;
                    r.hashSource = o.srcDigest;
                    r.hashDest = o.dstDigest;
                    out.problems.push_back(std::move(r));
                }
            } else {
                const bool denied = o.srcStatus == hashing::HashStatus::NoAccess ||
                                    o.dstStatus == hashing::HashStatus::NoAccess;
                if (denied) {
                    ++out.stats.accessDenied;
                } else {
                    ++out.stats.readErrors;
                }
                FileResult r;
                r.status = denied ? Status::AccessDenied : Status::ReadError;
                r.relativePath = hp.relativePath;
                r.sizeSource = hp.sizeSource;
                r.sizeDest = hp.sizeDest;
                r.isDirectory = false;
                r.hasHashSource = o.hasSrc;
                r.hasHashDest = o.hasDst;
                r.hashSource = o.srcDigest;
                r.hashDest = o.dstDigest;
                r.errorMessage = denied ? L"accesso negato durante il calcolo dell'impronta"
                                        : L"errore di lettura durante il calcolo dell'impronta";
                out.problems.push_back(std::move(r));
            }
        }

        done += n;
        if (onProgress) onProgress(done, total);
    }
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
        if (HasDescendant(keys, it.key)) continue;
        FileResult r;
        r.status = Status::Missing;
        r.relativePath = it.entry->relativePath;
        r.isDirectory = true;
        out.problems.push_back(std::move(r));
    }
}

} // namespace bv
