#include "Sync/SyncPlan.h"

#include <algorithm>

#include "Filesystem/PathUtil.h"
#include "Sync/SyncPath.h"

namespace bv {
namespace sync {
namespace {

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

void PushAction(SyncPlan& plan, SyncAction a) {
    Tally(plan, a);
    plan.actions.push_back(std::move(a));
}

void PushSkip(SyncPlan& plan, const FileResult& r, const std::string& reason) {
    plan.skipped.push_back(pathutil::ToUtf8(r.relativePath) + ": " + reason);
}

// Every ancestor dir of `rel` (deepest last here; the caller sorts).
void Ancestors(const std::wstring& rel, std::vector<std::wstring>& out) {
    size_t pos = 0;
    while ((pos = rel.find(L'\\', pos)) != std::wstring::npos) {
        out.push_back(rel.substr(0, pos));
        ++pos;
    }
}

} // namespace

SyncPlan BuildSyncPlan(const ResultSet& results, const std::wstring& sourceRoot,
                       const std::wstring& destRoot, bool sourceIsA) {
    SyncPlan plan;
    plan.sourceRoot = sourceRoot;
    plan.destRoot = destRoot;
    plan.sourceIsA = sourceIsA;

    std::vector<SyncAction> createDirs;
    std::vector<SyncAction> fileWrites; // CopyFile + ReplaceFile
    std::vector<SyncAction> linkWrites; // CreateLink + ReplaceLink
    std::vector<SyncAction> deletes;    // DeleteFile + DeleteLink
    std::vector<SyncAction> deleteDirs;
    std::vector<std::wstring> dirAncestors; // deduped later

    for (const FileResult& r : results.problems) {
        // Containment first: a forged/stale row must never reach the executor.
        if (ResolveWithinRoot(destRoot, r.relativePath).empty()) {
            PushSkip(plan, r, "percorso fuori dalla radice di destinazione");
            continue;
        }
        const ReparseKind kind = r.reparseKind;
        const bool srcLink = kind != ReparseKind::None;
        switch (r.status) {
            case Status::Missing: {
                if (srcLink && !IsSupportedLink(kind)) {
                    PushSkip(plan, r, "link non supportato");
                    break;
                }
                SyncAction a;
                a.relativePath = r.relativePath;
                a.isDirectory = r.isDirectory && !srcLink;
                a.linkKind = kind;
                if (srcLink) {
                    a.op = SyncOp::LinkCreate;
                    linkWrites.push_back(a);
                } else if (r.isDirectory) {
                    a.op = SyncOp::DirCreate;
                    createDirs.push_back(a);
                } else {
                    a.op = SyncOp::FileCopy;
                    a.bytes = r.sizeSource;
                    fileWrites.push_back(a);
                }
                Tally(plan, a);
                break;
            }
            case Status::Extra: {
                if (srcLink && !IsSupportedLink(kind)) {
                    PushSkip(plan, r, "link non supportato");
                    break;
                }
                // Only planned rows join the deletable set: skipped
                // (unsupported/error) entries must block, not join, the
                // guarded recursion of an ancestor delete.
                plan.extraFoldedRels.push_back(pathutil::FoldForCompare(r.relativePath));
                SyncAction a;
                a.relativePath = r.relativePath;
                a.isDirectory = r.isDirectory && !srcLink;
                a.linkKind = kind;
                if (srcLink) {
                    a.op = SyncOp::LinkDelete;
                    deletes.push_back(a);
                } else if (r.isDirectory) {
                    a.op = SyncOp::DirDelete;
                    deleteDirs.push_back(a);
                } else {
                    a.op = SyncOp::FileDelete;
                    deletes.push_back(a);
                }
                Tally(plan, a);
                // Non-empty extra dirs have no row of their own: schedule every
                // ancestor for guarded deletion (deep-first, see below). Shared
                // ancestors abort safely at execution (unexpected content).
                Ancestors(r.relativePath, dirAncestors);
                break;
            }
            case Status::SizeMismatch:
            case Status::ContentMismatch:
            case Status::ContentMismatchPartial: {
                if (srcLink && !IsSupportedLink(kind)) {
                    PushSkip(plan, r, "link non supportato");
                    break;
                }
                SyncAction a;
                a.relativePath = r.relativePath;
                a.isDirectory = false;
                a.linkKind = kind;
                if (srcLink) {
                    a.op = SyncOp::LinkReplace;
                    linkWrites.push_back(a);
                } else {
                    a.op = SyncOp::FileReplace;
                    a.bytes = r.sizeSource;
                    fileWrites.push_back(a);
                }
                Tally(plan, a);
                break;
            }
            default:
                // Identical* never stored; ReadError/AccessDenied/
                // ChangedDuringScan are not plannable.
                PushSkip(plan, r, "stato non sincronizzabile");
                break;
        }
    }

    // Ancestor dirs of extra rows: guarded DeleteDir, deepest first,
    // deduplicated. Ancestors shared with the source abort at execution.
    std::sort(dirAncestors.begin(), dirAncestors.end());
    dirAncestors.erase(std::unique(dirAncestors.begin(), dirAncestors.end()),
                       dirAncestors.end());
    std::sort(dirAncestors.begin(), dirAncestors.end(),
              [](const std::wstring& a, const std::wstring& b) {
                  return a.size() > b.size();
              });
    for (const std::wstring& dir : dirAncestors) {
        bool known = false;
        for (const SyncAction& d : deleteDirs) {
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
        deleteDirs.push_back(a);
        Tally(plan, a);
        // Ancestors join the deletable set: sibling FileDelete actions may
        // have emptied them already (the recursion still verifies every
        // entry, so shared content keeps aborting safely).
        plan.extraFoldedRels.push_back(pathutil::FoldForCompare(dir));
    }

    // Shallow-first directory creation.
    std::sort(createDirs.begin(), createDirs.end(),
              [](const SyncAction& a, const SyncAction& b) {
                  return a.relativePath.size() < b.relativePath.size();
              });

    plan.actions.reserve(createDirs.size() + fileWrites.size() + linkWrites.size() +
                         deletes.size() + deleteDirs.size());
    plan.actions.insert(plan.actions.end(), createDirs.begin(), createDirs.end());
    plan.actions.insert(plan.actions.end(), fileWrites.begin(), fileWrites.end());
    plan.actions.insert(plan.actions.end(), linkWrites.begin(), linkWrites.end());
    plan.actions.insert(plan.actions.end(), deletes.begin(), deletes.end());
    // deleteDirs already holds childless-extra rows; ancestors appended above
    // are deepest-first, but row order among themselves is arbitrary: sort the
    // whole group deep-first for a safe bottom-up teardown.
    std::sort(deleteDirs.begin(), deleteDirs.end(),
              [](const SyncAction& a, const SyncAction& b) {
                  return a.relativePath.size() > b.relativePath.size();
              });
    plan.actions.insert(plan.actions.end(), deleteDirs.begin(), deleteDirs.end());
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
    const ReparseKind kind = row.reparseKind;
    const bool isLink = kind != ReparseKind::None;
    if (isLink && !IsSupportedLink(kind)) {
        PushSkip(plan, row, "link non supportato");
        return plan;
    }
    // DeleteDir guard set: a single delete only trusts the row itself; any
    // post-scan child aborts the recursion safely.
    plan.extraFoldedRels.push_back(pathutil::FoldForCompare(row.relativePath));
    SyncAction a;
    a.relativePath = row.relativePath;
    a.linkKind = kind;
    // Byte estimate from the side the content comes from.
    const uint64_t srcBytes = toA ? row.sizeDest : row.sizeSource;
    switch (op) {
        case ManualOp::CopyToDst:
            a.isDirectory = false;
            if (isLink) {
                a.op = SyncOp::LinkCreate;
            } else {
                a.op = SyncOp::FileCopy;
                a.bytes = srcBytes;
            }
            break;
        case ManualOp::ReplaceToDst:
            a.isDirectory = false;
            if (isLink) {
                a.op = SyncOp::LinkReplace;
            } else {
                a.op = SyncOp::FileReplace;
                a.bytes = srcBytes;
            }
            break;
        case ManualOp::DeleteAtDst:
            a.isDirectory = row.isDirectory && !isLink;
            if (isLink) {
                a.op = SyncOp::LinkDelete;
            } else if (a.isDirectory) {
                a.op = SyncOp::DirDelete;
            } else {
                a.op = SyncOp::FileDelete;
            }
            break;
        case ManualOp::CreateDirDst:
            a.isDirectory = true;
            a.op = SyncOp::DirCreate;
            break;
    }
    PushAction(plan, a);
    return plan;
}

} // namespace sync
} // namespace bv
