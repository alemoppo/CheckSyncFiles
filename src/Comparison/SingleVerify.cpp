#include "Comparison/SingleVerify.h"

#include <cstddef>
#include <random>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Comparison/ClassifyUtil.h"
#include "Comparison/ConcurrentSink.h"
#include "Comparison/HashPhase.h"
#include "Filesystem/PathUtil.h"
#include "Hashing/HashCache.h"
#include "Hashing/PartialRead.h"
#include "Hashing/Sha256.h"

namespace bv {

PartialPattern ResolveRandomOnce() {
    try {
        std::random_device rd;
        return (rd() & 1u) ? PartialPattern::Center : PartialPattern::Edges;
    } catch (...) {
        return PartialPattern::Edges; // RNG failure: stay concrete
    }
}

namespace {

// Win32 "not found" errors: the side is absent (nothing to verify there).
bool IsNotFound(DWORD err) {
    return err == ERROR_FILE_NOT_FOUND ||  // 2
           err == ERROR_PATH_NOT_FOUND;    // 3
}

// Fresh filesystem stat of one side. Mirrors what enumeration observes
// (attributes + size + mtime) without opening the file for reading; the
// content read itself happens later inside HashOneSide.
struct SideStat {
    bool present = false;
    bool isDirectory = false;
    bool metaError = false; // exists, but metadata unreadable
    bool denied = false;    // ... specifically access-denied
    uint64_t size = 0;
    uint64_t mtime = 0; // Windows FILETIME, like FileEntry
    uint32_t attributes = 0;
};

SideStat StatOneSide(const std::wstring& absPath) {
    SideStat st;
    const std::wstring win = pathutil::AddLongPathPrefix(absPath);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(win.c_str(), GetFileExInfoStandard, &data)) {
        const DWORD err = GetLastError();
        if (IsNotFound(err)) return st; // absent
        st.metaError = true;
        st.denied = (err == ERROR_ACCESS_DENIED);
        return st;
    }
    st.present = true;
    st.attributes = data.dwFileAttributes;
    st.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!st.isDirectory) {
        st.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        st.mtime = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                   data.ftLastWriteTime.dwLowDateTime;
    }
    return st;
}

FileEntry ToEntry(const std::wstring& rel, const SideStat& st) {
    FileEntry e;
    e.relativePath = rel;
    e.size = st.size;
    e.lastWriteTime = st.mtime;
    e.attributes = st.attributes;
    e.fileId = 0;
    e.isDirectory = st.isDirectory;
    return e;
}

// Error row for a side whose metadata itself was unreadable. Mirrors the
// errorRoot policy of a full scan: the failed side when the other side is
// fine, otherwise the destination as the deterministic fallback. Preserves
// the size of whichever side actually produced data (callers pass 0 for the
// failed side -- never invent one); like Missing/Extra rows, the unknown
// side keeps the default 0, the project convention for "size unavailable".
FileResult MetaErrorRow(const std::wstring& rel, bool denied,
                        const std::wstring& failedRoot, const std::wstring& fallbackRoot,
                        bool otherSideOk, uint64_t sizeSource, uint64_t sizeDest) {
    FileResult r;
    r.status = denied ? Status::AccessDenied : Status::ReadError;
    r.fullPath = pathutil::MakeAbsolute(otherSideOk ? failedRoot : fallbackRoot, rel);
    r.relativePath = rel;
    r.sizeSource = sizeSource;
    r.sizeDest = sizeDest;
    r.isDirectory = false;
    r.errorMessage = denied ? L"accesso negato durante la lettura dei metadati"
                            : L"errore di lettura dei metadati del file";
    return r;
}

} // namespace

FileResult MakeInternalVerifyError(const SingleVerifyRequest& req, const char* what) {
    FileResult r;
    r.status = Status::ReadError;
    // Both-failed fallback convention: destination side shown.
    r.fullPath = pathutil::MakeAbsolute(req.destRoot, req.relativePath);
    r.relativePath = req.relativePath;
    r.isDirectory = false;
    std::wstring msg = L"errore interno durante la verifica singola";
    if (what != nullptr && what[0] != '\0') {
        msg += L": " + pathutil::FromUtf8(what);
    }
    r.errorMessage = std::move(msg);
    return r;
}

SingleVerifyOutcome VerifySingleFile(const SingleVerifyRequest& req) {
    SingleVerifyOutcome out;
    out.relativePath = req.relativePath;
    if (req.cancel != nullptr && req.cancel->load(std::memory_order_relaxed)) {
        out.cancelled = true; // asked to stop before any work: no verdict
        return out;
    }

    const std::wstring absA =
        pathutil::MakeAbsolute(pathutil::NormalizeRoot(req.sourceRoot), req.relativePath);
    const std::wstring absB =
        pathutil::MakeAbsolute(pathutil::NormalizeRoot(req.destRoot), req.relativePath);
    const SideStat a = StatOneSide(absA);
    const SideStat b = StatOneSide(absB);

    if (a.metaError || b.metaError) {
        // A side exists but its metadata is unreadable: same verdict family
        // as a full scan (AccessDenied vs ReadError), no content touched.
        // Reports the first failing side (A first, deterministic).
        if (a.metaError) {
            // A produced nothing: sizeSource stays 0, sizeDest kept when B
            // was stat-ed fine.
            out.result = MetaErrorRow(req.relativePath, a.denied, req.sourceRoot,
                                      req.destRoot, b.present, 0,
                                      b.present ? b.size : 0);
        } else {
            out.result = MetaErrorRow(req.relativePath, b.denied, req.destRoot,
                                      req.sourceRoot, a.present,
                                      a.present ? a.size : 0, 0);
        }
        return out;
    }
    if (!a.present && !b.present) {
        // Gone from both sides since the scan: nothing to verify against.
        // Keeps the row visible as a read error instead of silently dropping
        // it (both-failed fallback convention: destination side shown).
        FileResult r;
        r.status = Status::ReadError;
        r.fullPath = pathutil::MakeAbsolute(req.destRoot, req.relativePath);
        r.relativePath = req.relativePath;
        r.isDirectory = false;
        r.errorMessage = L"file assente su entrambi i lati";
        out.result = std::move(r);
        return out;
    }
    if (a.present != b.present) {
        // One side appeared/vanished since the scan: Missing/Extra exactly
        // like the finalize step of a full scan (source-side / dest-side).
        FileResult r;
        r.isDirectory = false;
        if (a.present) {
            r.status = Status::Missing;
            r.fullPath = pathutil::MakeAbsolute(req.sourceRoot, req.relativePath);
            r.sizeSource = a.size;
        } else {
            r.status = Status::Extra;
            r.fullPath = pathutil::MakeAbsolute(req.destRoot, req.relativePath);
            r.sizeDest = b.size;
        }
        r.relativePath = req.relativePath;
        out.result = std::move(r);
        return out;
    }

    // Both sides present: reuse the exact classification of a full scan,
    // including file<->dir mismatches. A throwaway sink collects the single
    // outcome; global stats are the GUI's job (see ApplySingleResult).
    ConcurrentSink sink;
    std::atomic<size_t> cacheHits{0};
    if (req.mode == ScanMode::Size) {
        std::vector<ContentCandidate> noCandidates;
        ClassifyMatched(ToEntry(req.relativePath, a), ToEntry(req.relativePath, b),
                        ScanMode::Size, sink, noCandidates, req.destRoot);
    } else {
        std::vector<ContentCandidate> candidates;
        ClassifyMatched(ToEntry(req.relativePath, a), ToEntry(req.relativePath, b),
                        ScanMode::Content, sink, candidates, req.destRoot);
        for (const ContentCandidate& c : candidates) {
            HashOneCandidateInto(c, /*offlineSource=*/false, /*index=*/nullptr,
                                 req.sourceRoot, req.destRoot, sink, req.cancel, req.cache,
                                 cacheHits, nullptr, nullptr, nullptr, req.verify);
        }
    }
    ResultSet one = sink.take();
    if (req.cancel != nullptr && req.cancel->load(std::memory_order_relaxed)) {
        out.cancelled = true; // no verdict from partial work, same rule
        return out;
    }
    if (!one.problems.empty()) {
        out.result = std::move(one.problems.front());
        return out;
    }
    // No row: the verdict was Identical or IdenticalPartial (counted in the
    // throwaway sink stats, never stored -- same convention). Rebuild the
    // effective level for the record fields with a pure recompute over the
    // same inputs (deterministic, cannot diverge).
    FileResult r;
    r.relativePath = req.relativePath;
    r.fullPath = pathutil::MakeAbsolute(req.sourceRoot, req.relativePath);
    r.sizeSource = a.size;
    r.sizeDest = b.size;
    r.isDirectory = false;
    if (req.mode == ScanMode::Content && req.verify.percent < 100) {
        const partial::PartialReadPlan plan = partial::ComputePartialReadPlan(
            a.size, req.verify.percent, req.verify.pattern);
        const partial::EffectiveVerify eff =
            partial::EffectiveLevel(plan, req.verify.percent, req.verify.pattern);
        if (!plan.isFullRead) {
            r.status = Status::IdenticalPartial;
            r.verifiedPercent = eff.percent;
            r.verifiedPattern = eff.pattern;
            out.result = std::move(r);
            return out;
        }
    }
    r.status = Status::Identical;
    out.result = std::move(r);
    return out;
}

bool ApplySingleResult(ResultSet& set, const FileResult& fresh) {
    // Saturating counter fix: a stale/foreign row must never wrap a counter.
    const auto bump = [&](uint64_t& counter, int delta) {
        if (delta < 0) {
            if (counter > 0) counter -= 1;
        } else {
            counter += 1;
        }
    };
    const auto bumpStatus = [&](Status s, int delta) {
        switch (s) {
            case Status::Identical: bump(set.stats.identicalFiles, delta); break;
            case Status::Missing: bump(set.stats.missingFiles, delta); break;
            case Status::Extra: bump(set.stats.extraFiles, delta); break;
            case Status::SizeMismatch: bump(set.stats.sizeMismatch, delta); break;
            case Status::ContentMismatch: bump(set.stats.contentMismatch, delta); break;
            case Status::IdenticalPartial: bump(set.stats.identicalPartialFiles, delta); break;
            case Status::ContentMismatchPartial:
                bump(set.stats.contentMismatchPartial, delta);
                break;
            case Status::ReadError: bump(set.stats.readErrors, delta); break;
            case Status::AccessDenied: bump(set.stats.accessDenied, delta); break;
            case Status::ChangedDuringScan: bump(set.stats.changedDuringScan, delta); break;
        }
    };
    for (size_t i = 0; i < set.problems.size(); ++i) {
        if (set.problems[i].relativePath != fresh.relativePath) continue;
        bumpStatus(set.problems[i].status, -1);
        if (fresh.status == Status::Identical || fresh.status == Status::IdenticalPartial) {
            bumpStatus(fresh.status, +1);
            set.problems.erase(set.problems.begin() + static_cast<ptrdiff_t>(i));
        } else {
            bumpStatus(fresh.status, +1);
            set.problems[i] = fresh;
        }
        return true;
    }
    return false; // stale outcome (row gone after a new scan): ignore it
}

} // namespace bv
