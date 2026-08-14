#include "Comparison/HashPhase.h"

#include <algorithm>

#include "Filesystem/PathUtil.h"
#include "Hashing/HashUtil.h"

namespace bv {

namespace {

constexpr size_t kHashBatch = 256; // candidates hashed per thread-pool batch

using Digest = hashing::Digest;

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

} // namespace

void RunHashPhase(const std::vector<ContentCandidate>& candidates, ThreadPool& pool,
                  bool offlineSource, FileIndex* index, const std::wstring& sourceRoot,
                  const std::wstring& destRoot, ResultSet& out, const std::atomic_bool* cancel,
                  const std::function<void(uint64_t done, uint64_t total)>& onProgress,
                  hashing::HashCache* cache, std::atomic<size_t>& cacheHits) {
    const size_t total = candidates.size();
    size_t done = 0;
    while (done < total) {
        if (cancel && cancel->load(std::memory_order_relaxed)) break;

        const size_t n = std::min(kHashBatch, total - done);
        std::vector<Outcome> outcomes(n);
        for (size_t i = 0; i < n; ++i) {
            const ContentCandidate& c = candidates[done + i];
            pool.submit([&c, offlineSource, index, &sourceRoot, &destRoot, &outcomes, i, cache,
                         &cacheHits, cancel] {
                Outcome& o = outcomes[i];
                if (cancel && cancel->load(std::memory_order_relaxed)) return;
                if (offlineSource) {
                    // Source device absent: use the digest captured in the snapshot.
                    o.hasSrc = index->getHash(c.relativePath, o.srcDigest);
                    o.srcStatus =
                        o.hasSrc ? hashing::HashStatus::Ok : hashing::HashStatus::ReadError;
                } else {
                    hashing::HashOneSide(pathutil::MakeAbsolute(sourceRoot, c.relativePath),
                                         c.sizeSource, c.srcMtime, o.changed, o.srcStatus,
                                         o.srcDigest, true, cache, cacheHits);
                    o.hasSrc = (o.srcStatus == hashing::HashStatus::Ok);
                }
                bool dstChanged = false;
                hashing::HashOneSide(pathutil::MakeAbsolute(destRoot, c.relativePath),
                                     c.sizeDest, c.dstMtime, dstChanged, o.dstStatus, o.dstDigest,
                                     true, cache, cacheHits);
                o.hasDst = (o.dstStatus == hashing::HashStatus::Ok);
                o.changed = o.changed || dstChanged;
                o.equal = (o.srcStatus == hashing::HashStatus::Ok &&
                           o.dstStatus == hashing::HashStatus::Ok && o.srcDigest == o.dstDigest);
            });
        }
        pool.waitAll();

        for (size_t i = 0; i < n; ++i) {
            const ContentCandidate& c = candidates[done + i];
            const Outcome& o = outcomes[i];

            if (o.changed) {
                ++out.stats.changedDuringScan;
                FileResult r;
                r.status = Status::ChangedDuringScan;
                r.relativePath = c.relativePath;
                r.sizeSource = c.sizeSource;
                r.sizeDest = c.sizeDest;
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
                    r.relativePath = c.relativePath;
                    r.sizeSource = c.sizeSource;
                    r.sizeDest = c.sizeDest;
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
                r.relativePath = c.relativePath;
                r.sizeSource = c.sizeSource;
                r.sizeDest = c.sizeDest;
                r.isDirectory = false;
                r.hasHashSource = o.hasSrc;
                r.hasHashDest = o.hasDst;
                r.hashSource = o.srcDigest;
                r.hashDest = o.dstDigest;
                r.errorMessage = denied
                                     ? L"accesso negato durante il calcolo dell'impronta"
                                     : L"errore di lettura durante il calcolo dell'impronta";
                out.problems.push_back(std::move(r));
            }
        }

        done += n;
        if (onProgress) onProgress(done, total);
    }
}

} // namespace bv