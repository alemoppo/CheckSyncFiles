#include "Sync/SyncPlan.h"

#include <algorithm>

#include "Filesystem/PathUtil.h"
#include "Sync/SyncPath.h"

namespace bv {
namespace sync {
namespace {

// A plan holds groups emitted in execution order:
//   DirCreate (absent targets) -> FileDelete/LinkDelete -> DirDelete
//   (deep-first) -> FileCopy/FileReplace -> LinkCreate/LinkReplace.
// Deletes run before writes so a destination cleared for a type change is
// gone when the replacement is written; the guarded DirDelete never removes
// a path the source side claims (see nonExtraRels below).
struct Groups {
    std::vector<SyncAction> createDirs;
    std::vector<SyncAction> deletes; // FileDelete + LinkDelete
    std::vector<SyncAction> deleteDirs;
    std::vector<SyncAction> fileWrites; // FileCopy + FileReplace
    std::vector<SyncAction> linkWrites; // LinkCreate + LinkReplace
};

bool IsSupportedLink(ReparseKind k) {
    return k == ReparseKind::SymlinkFile || k == ReparseKind::SymlinkDir ||
           k == ReparseKind::Junction;
}

void Tally(SyncPlan& plan, const SyncAction& a) {
    switch (a.op) {
        case SyncOp::FileCopy:
            ++plan.summary.copyFiles;
            plan.summary.bytesToCopy += a.bytes;
            break;
        case SyncOp::FileReplace:
            ++plan.summary.replaceFiles;
            plan.summary.bytesToCopy += a.bytes;
            break;
        case SyncOp::FileDelete: ++plan.summary.deleteFiles; break;
        case SyncOp::DirCreate: ++plan.summary.createDirs; break;
        case SyncOp::DirDelete: ++plan.summary.deleteDirs; break;
        case SyncOp::LinkCreate: ++plan.summary.createLinks; break;
        case SyncOp::LinkReplace:
            ++plan.summary.replaceLinks;
            break;
        case SyncOp::LinkDelete: ++plan.summary.deleteLinks; break;
    }
}

void PushSkip(SyncPlan& plan, const FileResult& r, const std::string& reason) {
    plan.skipped.push_back(pathutil::ToUtf8(r.relativePath) + ": " + reason);
}

void PushSkipRel(SyncPlan& plan, const std::wstring& rel, const std::string& reason) {
    plan.skipped.push_back(pathutil::ToUtf8(rel) + ": " + reason);
}

// One side of a replacement, with explicit type (never inferred from the
// other side): the source side drives copy/replace selection, the
// destination side drives the clearing deletes.
struct OneSide {
    bool isDir = false;
    ReparseKind kind = ReparseKind::None;
    uint64_t bytes = 0; // source side only: estimated inbound bytes
};

SyncAction MakeAction(SyncOp op, const std::wstring& rel, const OneSide& src) {
    SyncAction a;
    a.op = op;
    a.relativePath = rel;
    a.isDirectory = src.isDir;
    a.linkKind = src.kind;
    a.bytes = src.bytes;
    return a;
}

// Missing-like: the source side exists, the destination side is absent.
void AppendMissingLike(SyncPlan& plan, Groups& g, const std::wstring& rel,
                       const OneSide& src, bool allowDir) {
    if (!IsSupportedLink(src.kind) && src.kind != ReparseKind::None) {
        PushSkipRel(plan, rel, "sorgente link non supportato");
        return;
    }
    const bool srcLink = src.kind != ReparseKind::None;
    if (srcLink) {
        SyncAction a = MakeAction(SyncOp::LinkCreate, rel, src);
        Tally(plan, a);
        g.linkWrites.push_back(std::move(a));
    } else if (src.isDir) {
        if (!allowDir) {
            PushSkipRel(plan, rel, "cartelle: solo eliminazione in questa direzione");
            return;
        }
        SyncAction a = MakeAction(SyncOp::DirCreate, rel, src);
        Tally(plan, a);
        g.createDirs.push_back(std::move(a));
    } else {
        SyncAction a = MakeAction(SyncOp::FileCopy, rel, src);
        Tally(plan, a);
        g.fileWrites.push_back(std::move(a));
    }
}

// Extra-like: the destination side exists, the source side is absent.
void AppendExtraLike(SyncPlan& plan, Groups& g, const std::wstring& rel,
                     const OneSide& dst, std::vector<std::wstring>& dirAncestors,
                     bool scheduleAncestors) {
    if (!IsSupportedLink(dst.kind) && dst.kind != ReparseKind::None) {
        PushSkipRel(plan, rel, "destinazione link non supportato");
        return;
    }
    plan.extraFoldedRels.push_back(pathutil::FoldForCompare(rel));
    const bool dstLink = dst.kind != ReparseKind::None;
    SyncAction a;
    if (dstLink) {
        a = MakeAction(SyncOp::LinkDelete, rel, dst);
        Tally(plan, a);
        g.deletes.push_back(std::move(a));
    } else if (dst.isDir) {
        a = MakeAction(SyncOp::DirDelete, rel, dst);
        Tally(plan, a);
        g.deleteDirs.push_back(std::move(a));
    } else {
        a = MakeAction(SyncOp::FileDelete, rel, dst);
        Tally(plan, a);
        g.deletes.push_back(std::move(a));
    }
    if (scheduleAncestors) {
        size_t pos = 0;
        while ((pos = rel.find(L'\\', pos)) != std::wstring::npos) {
            dirAncestors.push_back(rel.substr(0, pos));
            ++pos;
        }
    }
}

// Mismatch replace driven by the SOURCE side (copies reproduce the source):
// a source file is copied, a source dir is created, a source link is
// recreated. Clearing deletes for a conflicting destination are emitted into
// the delete groups so they run before the write. Returns false (skip
// pushed) when either side is unplannable.
bool AppendMismatchReplace(SyncPlan& plan, Groups& g, const std::wstring& rel,
                           const OneSide& src, const OneSide& dst) {
    if (!IsSupportedLink(src.kind) && src.kind != ReparseKind::None) {
        PushSkipRel(plan, rel, "sorgente link non supportato");
        return false;
    }
    if (!IsSupportedLink(dst.kind) && dst.kind != ReparseKind::None) {
        PushSkipRel(plan, rel, "destinazione reparse non supportato");
        return false;
    }
    const bool srcLink = src.kind != ReparseKind::None;
    const bool dstLink = dst.kind != ReparseKind::None;
    if (srcLink) {
        // A conflicting real directory must be cleared first (the executor
        // only gives way to links for empty dirs).
        if (!dstLink && dst.isDir) {
            SyncAction d = MakeAction(SyncOp::DirDelete, rel, dst);
            Tally(plan, d);
            g.deleteDirs.push_back(std::move(d));
        }
        SyncAction a = MakeAction(SyncOp::LinkReplace, rel, src);
        Tally(plan, a);
        g.linkWrites.push_back(std::move(a));
        return true;
    }
    if (src.isDir) {
        // Destination must become a directory. No clearing delete is needed:
        // the executor adapts DirCreate to a conflicting live file/link
        // (deleted inline), while a conflicting dir is kept for row
        // convergence. A separate delete would race the adaptation (deleting
        // a directory as a file errors out).
        SyncAction a = MakeAction(SyncOp::DirCreate, rel, src);
        Tally(plan, a);
        g.createDirs.push_back(std::move(a));
        return true;
    }
    // Source plain file.
    if (!dstLink && dst.isDir) {
        // Possibly non-empty: guarded delete first, then the copy.
        SyncAction d = MakeAction(SyncOp::DirDelete, rel, dst);
        Tally(plan, d);
        g.deleteDirs.push_back(std::move(d));
    }
    // A conflicting destination link is unlinked inline by the executor.
    SyncAction a = MakeAction(SyncOp::FileReplace, rel, src);
    Tally(plan, a);
    g.fileWrites.push_back(std::move(a));
    return true;
}

// An ancestor scheduled for guarded deletion must not be a path the source
// side claims (it must survive): skip it when it equals, or is a parent of,
// any non-Extra row.
bool IsClaimedBySource(const std::vector<std::wstring>& sortedNonExtra,
                       const std::wstring& dir) {
    auto it = std::lower_bound(sortedNonExtra.begin(), sortedNonExtra.end(), dir);
    if (it != sortedNonExtra.end() && *it == dir) return true;
    const std::wstring prefix = dir + L"\\";
    it = std::lower_bound(sortedNonExtra.begin(), sortedNonExtra.end(), prefix);
    return it != sortedNonExtra.end() && it->compare(0, prefix.size(), prefix) == 0;
}

} // namespace

SyncPlan BuildSyncPlan(const ResultSet& results, const std::wstring& sourceRoot,
                       const std::wstring& destRoot, bool sourceIsA) {
    SyncPlan plan;
    plan.sourceRoot = sourceRoot;
    plan.destRoot = destRoot;
    plan.sourceIsA = sourceIsA;
    Groups g;
    std::vector<std::wstring> dirAncestors;
    std::vector<std::wstring> nonExtraRels; // folded, sorted below

    for (const FileResult& r : results.problems) {
        // Containment first: a forged/stale row must never reach the executor.
        if (ResolveWithinRoot(destRoot, r.relativePath).empty()) {
            PushSkip(plan, r, "percorso fuori dalla radice di destinazione");
            continue;
        }
        if (r.status != Status::Extra) {
            nonExtraRels.push_back(pathutil::FoldForCompare(r.relativePath));
        }
        switch (r.status) {
            case Status::Missing: {
                const OneSide src{r.srcIsDirectory, r.srcReparseKind, r.sizeSource};
                AppendMissingLike(plan, g, r.relativePath, src, /*allowDir=*/true);
                break;
            }
            case Status::Extra: {
                const OneSide dst{r.dstIsDirectory, r.reparseKind, 0};
                AppendExtraLike(plan, g, r.relativePath, dst, dirAncestors,
                                /*scheduleAncestors=*/true);
                break;
            }
            case Status::SizeMismatch:
            case Status::ContentMismatch:
            case Status::ContentMismatchPartial: {
                const OneSide src{r.srcIsDirectory, r.srcReparseKind, r.sizeSource};
                const OneSide dst{r.dstIsDirectory, r.reparseKind, 0};
                AppendMismatchReplace(plan, g, r.relativePath, src, dst);
                break;
            }
            default:
                // Identical* never stored; ReadError/AccessDenied/
                // ChangedDuringScan are not plannable.
                PushSkip(plan, r, "stato non sincronizzabile");
                break;
        }
    }

    // Ancestor dirs of extra rows: guarded DirDelete, deepest first,
    // deduplicated, minus source-claimed paths. Ancestors join the deletable
    // set (sibling deletes may have emptied them; the recursion still
    // verifies every entry, so shared content keeps aborting safely).
    std::sort(nonExtraRels.begin(), nonExtraRels.end());
    nonExtraRels.erase(std::unique(nonExtraRels.begin(), nonExtraRels.end()),
                       nonExtraRels.end());
    std::sort(dirAncestors.begin(), dirAncestors.end());
    dirAncestors.erase(std::unique(dirAncestors.begin(), dirAncestors.end()),
                       dirAncestors.end());
    std::sort(dirAncestors.begin(), dirAncestors.end(),
              [](const std::wstring& a, const std::wstring& b) {
                  return a.size() > b.size();
              });
    for (const std::wstring& dir : dirAncestors) {
        // nonExtraRels is folded: fold the candidate too (case-insensitive).
        if (IsClaimedBySource(nonExtraRels, pathutil::FoldForCompare(dir))) continue;
        bool known = false;
        for (const SyncAction& d : g.deleteDirs) {
            if (d.relativePath == dir) {
                known = true;
                break;
            }
        }
        if (known) continue;
        SyncAction a;
        a.op = SyncOp::DirDelete;
        a.relativePath = dir;
        a.isDirectory = true;
        g.deleteDirs.push_back(a);
        Tally(plan, a);
        plan.extraFoldedRels.push_back(pathutil::FoldForCompare(dir));
    }

    std::sort(g.createDirs.begin(), g.createDirs.end(),
              [](const SyncAction& a, const SyncAction& b) {
                  return a.relativePath.size() < b.relativePath.size();
              });
    std::sort(g.deleteDirs.begin(), g.deleteDirs.end(),
              [](const SyncAction& a, const SyncAction& b) {
                  return a.relativePath.size() > b.relativePath.size();
              });

    plan.actions.reserve(g.createDirs.size() + g.deletes.size() + g.deleteDirs.size() +
                         g.fileWrites.size() + g.linkWrites.size());
    plan.actions.insert(plan.actions.end(), g.createDirs.begin(), g.createDirs.end());
    plan.actions.insert(plan.actions.end(), g.deletes.begin(), g.deletes.end());
    plan.actions.insert(plan.actions.end(), g.deleteDirs.begin(), g.deleteDirs.end());
    plan.actions.insert(plan.actions.end(), g.fileWrites.begin(), g.fileWrites.end());
    plan.actions.insert(plan.actions.end(), g.linkWrites.begin(), g.linkWrites.end());
    return plan;
}

SyncPlan BuildSingleActionPlan(const FileResult& row, ManualOp op,
                               const std::wstring& rootA, const std::wstring& rootB,
                               bool toA) {
    SyncPlan plan;
    plan.sourceRoot = toA ? rootB : rootA;
    plan.destRoot = toA ? rootA : rootB;
    plan.sourceIsA = !toA;
    if (ResolveWithinRoot(plan.destRoot, row.relativePath).empty() ||
        ResolveWithinRoot(plan.sourceRoot, row.relativePath).empty()) {
        PushSkip(plan, row, "percorso fuori radice");
        return plan;
    }
    Groups g;
    std::vector<std::wstring> noAncestors;
    // Source/destination sides from the row, oriented by the manual direction.
    const OneSide sideA{row.srcIsDirectory, row.srcReparseKind,
                        row.sizeSource};
    const OneSide sideB{row.dstIsDirectory, row.reparseKind, row.sizeDest};
    const OneSide& src = toA ? sideB : sideA;
    const OneSide& dst = toA ? sideA : sideB;
    switch (op) {
        case ManualOp::CopyToDst:
            // Offered on Missing (src present) and Extra (B->A) rows: the
            // source side must exist. Directory copies are not offered.
            AppendMissingLike(plan, g, row.relativePath, src, /*allowDir=*/false);
            break;
        case ManualOp::ReplaceToDst:
            AppendMismatchReplace(plan, g, row.relativePath, src, dst);
            break;
        case ManualOp::DeleteAtDst:
            AppendExtraLike(plan, g, row.relativePath, dst, noAncestors,
                            /*scheduleAncestors=*/false);
            break;
        case ManualOp::CreateDirDst:
            AppendMissingLike(plan, g, row.relativePath, src, /*allowDir=*/true);
            break;
    }
    plan.extraFoldedRels.push_back(pathutil::FoldForCompare(row.relativePath));
    // Direct insert (same group order as BuildSyncPlan): the Append* helpers
    // above already called Tally for every action, so PushAction here would
    // count plan.summary twice.
    plan.actions.reserve(g.createDirs.size() + g.deletes.size() + g.deleteDirs.size() +
                         g.fileWrites.size() + g.linkWrites.size());
    plan.actions.insert(plan.actions.end(), g.createDirs.begin(), g.createDirs.end());
    plan.actions.insert(plan.actions.end(), g.deletes.begin(), g.deletes.end());
    plan.actions.insert(plan.actions.end(), g.deleteDirs.begin(), g.deleteDirs.end());
    plan.actions.insert(plan.actions.end(), g.fileWrites.begin(), g.fileWrites.end());
    plan.actions.insert(plan.actions.end(), g.linkWrites.begin(), g.linkWrites.end());
    return plan;
}

} // namespace sync
} // namespace bv
