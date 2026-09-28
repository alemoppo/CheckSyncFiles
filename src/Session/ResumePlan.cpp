#include "Session/ResumePlan.h"

#include <unordered_map>
#include <unordered_set>

#include "Filesystem/PathUtil.h"
#include "Session/SessionLogic.h"

namespace bv {
namespace session {

namespace {

bool IsErrorVerdict(Status v) {
    return v == Status::ReadError || v == Status::AccessDenied ||
           v == Status::ChangedDuringScan;
}

// True when `rel` is exactly `dir` or strictly under it (`dir` + '\').
bool AtOrUnder(const std::wstring& rel, const std::wstring& dir) {
    if (dir.empty()) return true; // root-level error poisons everything
    if (rel.size() < dir.size()) return false;
    if (rel.compare(0, dir.size(), dir) != 0) return false;
    return rel.size() == dir.size() || rel[dir.size()] == L'\\';
}

bool UnderAnyErrorDir(const std::wstring& rel, const std::vector<std::wstring>& errorDirs) {
    for (const auto& d : errorDirs) {
        if (AtOrUnder(rel, d)) return true;
    }
    return false;
}

} // namespace

// Mirrors the engine's per-entry + per-verdict counting (onEntry +
// ClassifyMatched + hash outcomes + finalizeMissingExtra) so merged stats are
// indistinguishable from a fresh run's. Side file/dir/byte counts come from
// the caller-provided observations (exact per-side types, including file/dir
// SizeMismatch pairs); the verdict counters come from the journaled verdict.
void AccumulateJournalStats(Stats& st, const JournalEntry& e, bool hasA, bool aIsDir,
                            uint64_t aSize, bool hasB, bool bIsDir, uint64_t bSize) {
    if (hasA) {
        if (aIsDir) {
            ++st.sourceDirs;
        } else {
            ++st.sourceFiles;
            st.bytesSource += aSize;
        }
    }
    if (hasB) {
        if (bIsDir) {
            ++st.destDirs;
        } else {
            ++st.destFiles;
            st.bytesDest += bSize;
        }
    }
    switch (e.verdict) {
        case Status::Identical:
            if (e.isDirectory) ++st.identicalDirs;
            else ++st.identicalFiles;
            break;
        case Status::IdenticalPartial: ++st.identicalPartialFiles; break;
        case Status::SizeMismatch: ++st.sizeMismatch; break;
        case Status::ContentMismatch: ++st.contentMismatch; break;
        case Status::ContentMismatchPartial: ++st.contentMismatchPartial; break;
        case Status::Missing:
            if (e.isDirectory) ++st.missingDirs;
            else ++st.missingFiles;
            break;
        case Status::Extra:
            if (e.isDirectory) ++st.extraDirs;
            else ++st.extraFiles;
            break;
        case Status::ReadError:
        case Status::AccessDenied:
        case Status::ChangedDuringScan: break; // never reused (see PlanResume)
    }
}

namespace {

// Rebuilds the display row for a reused non-identical entry. Roots follow the
// engine's fullPath convention (Missing -> source, Extra/SizeMismatch ->
// destination, ContentMismatch -> source on a live run).
FileResult ReusedFileResult(const JournalEntry& e, const std::wstring& rootA,
                            const std::wstring& rootB) {
    FileResult r;
    r.relativePath = e.relativePath;
    r.isDirectory = e.isDirectory;
    switch (e.verdict) {
        case Status::Missing:
            r.status = Status::Missing;
            r.fullPath = pathutil::MakeAbsolute(rootA, e.relativePath);
            r.sizeSource = e.sizeA;
            break;
        case Status::Extra:
            r.status = Status::Extra;
            r.fullPath = pathutil::MakeAbsolute(rootB, e.relativePath);
            r.sizeDest = e.sizeB;
            break;
        case Status::SizeMismatch:
            r.status = Status::SizeMismatch;
            r.fullPath = pathutil::MakeAbsolute(rootB, e.relativePath);
            r.sizeSource = e.sizeA;
            r.sizeDest = e.sizeB;
            r.isDirectory = false;
            break;
        case Status::ContentMismatch:
            r.status = Status::ContentMismatch;
            r.fullPath = pathutil::MakeAbsolute(rootA, e.relativePath);
            r.sizeSource = e.sizeA;
            r.sizeDest = e.sizeB;
            r.hasHashSource = e.hasHashA;
            r.hasHashDest = e.hasHashB;
            r.hashSource = e.hashA;
            r.hashDest = e.hashB;
            r.isDirectory = false;
            break;
        case Status::ContentMismatchPartial:
            r.status = Status::ContentMismatchPartial;
            r.fullPath = pathutil::MakeAbsolute(rootA, e.relativePath);
            r.sizeSource = e.sizeA;
            r.sizeDest = e.sizeB;
            r.hasHashSource = e.hasHashA;
            r.hasHashDest = e.hasHashB;
            r.hashSource = e.hashA;
            r.hashDest = e.hashB;
            r.verifiedPercent = e.verifiedPercent;
            r.verifiedPattern = e.verifiedPattern;
            r.isDirectory = false;
            break;
        case Status::Identical:
        case Status::IdenticalPartial:
        case Status::ReadError:
        case Status::AccessDenied:
        case Status::ChangedDuringScan: break; // never stored; see caller
    }
    return r;
}

} // namespace

bool PlanResume(const ResumeInput& in, ResumePlan& out, std::string& detail) {
    if (!in.session || !in.currentA || !in.currentB) {
        detail = "invalid resume input";
        return false;
    }
    const bool caseSensitive = in.currentA->isCaseSensitive();
    const auto fold = [&](const std::wstring& rel) {
        return caseSensitive ? rel : pathutil::FoldForCompare(rel);
    };
    ResumePlan plan(caseSensitive);
    // Folded keys already decided (reused, routed to the remainder, or
    // dropped). Used afterwards to pick up current paths the journal never
    // saw (new files must be verified, not silently skipped).
    std::unordered_set<std::wstring> consumed;
    // The journal is append-only, so the LAST record of a path wins: index it
    // first and skip earlier duplicates, otherwise a path would be reused and
    // counted twice (in reusedStats, reusedEntries and the remainder sets).
    // Defense in depth -- the store never writes duplicates, but a
    // hand-edited or merged journal might hold them.
    std::unordered_map<std::wstring, size_t> lastIndex;
    for (size_t i = 0; i < in.session->journal.size(); ++i)
        lastIndex[fold(in.session->journal[i].relativePath)] = i;
    for (size_t i = 0; i < in.session->journal.size(); ++i) {
        const JournalEntry& e = in.session->journal[i];
        if (lastIndex[fold(e.relativePath)] != i) continue; // superseded duplicate
        FileEntry curA, curB;
        const bool foundA = in.currentA->find(e.relativePath, curA);
        const bool foundB = in.currentB->find(e.relativePath, curB);
        consumed.insert(fold(e.relativePath));

        // Error verdicts describe an environment, not a version: always
        // re-verify from current state, never reuse.
        if (IsErrorVerdict(e.verdict)) {
            ++plan.errorRows;
            ++plan.stale;
            if (foundA) plan.remainderA.addEntry(std::move(curA));
            if (foundB) plan.remainderB.addEntry(std::move(curB));
            if (!foundA && !foundB) {
                if (UnderAnyErrorDir(e.relativePath, in.errorDirsA) ||
                    UnderAnyErrorDir(e.relativePath, in.errorDirsB)) {
                    detail = "unprovable absence inside an errored subtree";
                    return false;
                }
                ++plan.droppedBothGone;
                --plan.stale; // dropped, not re-verified
            }
            continue;
        }

        const bool expectA = (e.verdict != Status::Extra);
        const bool expectB = (e.verdict != Status::Missing);
        const bool inErrA = UnderAnyErrorDir(e.relativePath, in.errorDirsA);
        const bool inErrB = UnderAnyErrorDir(e.relativePath, in.errorDirsB);

        if (!foundA && !foundB) {
            // Gone from both sides: drop, unless an enumeration error makes
            // the absence unprovable.
            if (inErrA || inErrB) {
                detail = "unprovable absence inside an errored subtree";
                return false;
            }
            ++plan.droppedBothGone;
            continue;
        }
        if (foundA != expectA || foundB != expectB) {
            // Presence changed: the world moved, re-verify current state.
            ++plan.stale;
            if (foundA) plan.remainderA.addEntry(std::move(curA));
            if (foundB) plan.remainderB.addEntry(std::move(curB));
            continue;
        }
        if (inErrA || inErrB) {
            // Present, but under a partially-listed directory: fingerprints
            // cannot be trusted, re-verify.
            ++plan.stale;
            if (foundA) plan.remainderA.addEntry(std::move(curA));
            if (foundB) plan.remainderB.addEntry(std::move(curB));
            continue;
        }
        // A SizeMismatch involving a directory side is always re-verified:
        // the single isDirectory flag cannot reconstruct per-side types, so
        // only the engine can re-decide it.
        if (e.verdict == Status::SizeMismatch && (curA.isDirectory || curB.isDirectory)) {
            ++plan.stale;
            if (foundA) plan.remainderA.addEntry(std::move(curA));
            if (foundB) plan.remainderB.addEntry(std::move(curB));
            continue;
        }
        // Presence matches on every implied side: the saved digest (if any)
        // is reusable only for the exact saved version. Directory rows carry
        // no content, so they need presence (checked) plus still-a-directory,
        // never mtime: dir mtimes flutter on live filesystems (external
        // touches re-list the directory), while real child changes have their
        // own rows that go stale independently.
        bool stale = false;
        if (e.isDirectory) {
            stale = (expectA && !curA.isDirectory) || (expectB && !curB.isDirectory);
        } else if (expectA && expectB) {
            stale = NeedsReverify(e, curA.size, curA.lastWriteTime, curB.size,
                                  curB.lastWriteTime);
        } else if (expectA) {
            stale = FingerprintStale(e.sizeA, e.mtimeA, curA.size, curA.lastWriteTime);
        } else {
            stale = FingerprintStale(e.sizeB, e.mtimeB, curB.size, curB.lastWriteTime);
        }
        if (stale) {
            ++plan.stale;
            if (foundA) plan.remainderA.addEntry(std::move(curA));
            if (foundB) plan.remainderB.addEntry(std::move(curB));
            continue;
        }
        ++plan.reused;
        plan.reusedEntries.push_back(e);
        AccumulateJournalStats(plan.reusedStats, e, expectA, curA.isDirectory, curA.size,
                               expectB, curB.isDirectory, curB.size);
        if (e.verdict != Status::Identical && e.verdict != Status::IdenticalPartial)
            plan.reusedProblems.push_back(ReusedFileResult(e, in.rootA, in.rootB));
    }
    // Current paths the journal never saw (created after the save, or never
    // finalized): route them to the remainder sets. Without this, new files
    // would be silently skipped by the filtered re-verification pass.
    for (const auto& kv : in.currentA->entries()) {
        if (consumed.find(fold(kv.second.relativePath)) == consumed.end()) {
            FileEntry copy = kv.second;
            plan.remainderA.addEntry(std::move(copy));
            ++plan.stale;
        }
    }
    for (const auto& kv : in.currentB->entries()) {
        if (consumed.find(fold(kv.second.relativePath)) == consumed.end()) {
            FileEntry copy = kv.second;
            plan.remainderB.addEntry(std::move(copy));
            ++plan.stale;
        }
    }
    out = std::move(plan);
    return true;
}

JournalEntry ToJournalEntry(const ClassifiedRow& row) {
    JournalEntry e;
    e.relativePath = row.relativePath;
    if (row.hasA) {
        e.sizeA = row.entryA.size;
        e.mtimeA = row.entryA.lastWriteTime;
    }
    if (row.hasB) {
        e.sizeB = row.entryB.size;
        e.mtimeB = row.entryB.lastWriteTime;
    }
    e.hasHashA = row.hasHashA;
    e.hasHashB = row.hasHashB;
    e.hashA = row.hashA;
    e.hashB = row.hashB;
    e.verdict = row.verdict;
    e.verifiedPercent = row.verifiedPercent;
    e.verifiedPattern = row.verifiedPattern;
    e.isDirectory = row.isDirectory;
    return e;
}

} // namespace session
} // namespace bv
