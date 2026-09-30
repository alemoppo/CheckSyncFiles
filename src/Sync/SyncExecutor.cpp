#include "Sync/SyncExecutor.h"

#include "Filesystem/PathUtil.h"
#include "Sync/SyncFs.h"
#include "Sync/SyncPath.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace bv {
namespace sync {
namespace {

bool WasCancelled(const std::atomic_bool* cancel) {
    return cancel && cancel->load(std::memory_order_relaxed);
}

bool IsDiskFullCode(DWORD code) {
    return code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL;
}

bool IsCancelCode(DWORD code, const std::atomic_bool* cancel) {
    return code == ERROR_REQUEST_ABORTED && WasCancelled(cancel);
}

SyncActionResult Fail(SyncOp op, const std::wstring& rel, const std::wstring& msg) {
    SyncActionResult r;
    r.op = op;
    r.relativePath = rel;
    r.message = msg;
    return r;
}

SyncActionResult Done(SyncOp op, const std::wstring& rel, const std::wstring& msg = {}) {
    SyncActionResult r;
    r.op = op;
    r.relativePath = rel;
    r.ok = true;
    r.message = msg;
    return r;
}

SyncActionResult Skip(SyncOp op, const std::wstring& rel, const std::wstring& msg) {
    SyncActionResult r;
    r.op = op;
    r.relativePath = rel;
    r.skipped = true;
    r.message = msg;
    return r;
}

// Adapts a file write to the LIVE destination: links are unlinked first, an
// empty dir is removed, anything else unexpected fails the action (never
// force-delete live content the plan did not describe).
bool PrepareFileDest(const std::wstring& dstAbs, std::wstring& error) {
    // Every failure path sets a precise code: the caller tells disk-full and
    // cancel apart only by GetLastError, so stale codes must never leak.
    switch (StatLiveKind(dstAbs)) {
        case LiveKind::Absent:
            error.clear();
            SetLastError(ERROR_SUCCESS);
            return true;
        case LiveKind::File:
            // A read-only destination cannot be overwritten: clear the flag
            // (the copy restores the source attributes afterwards).
            SetFileAttributesW(pathutil::AddLongPathPrefix(dstAbs).c_str(),
                               FILE_ATTRIBUTE_NORMAL);
            error.clear();
            SetLastError(ERROR_SUCCESS);
            return true;
        case LiveKind::LinkFile:
        case LiveKind::LinkDir: {
            const bool isDir = StatLiveKind(dstAbs) == LiveKind::LinkDir;
            const bool ok = DeleteLink(dstAbs, isDir, error);
            if (!ok && GetLastError() == ERROR_SUCCESS) SetLastError(ERROR_ACCESS_DENIED);
            return ok;
        }
        case LiveKind::Dir: {
            // Only an EMPTY dir may give way to a file (the planner clears
            // known non-empty dirs first); a non-empty one fails loudly
            // instead of being wiped.
            if (RemoveDirectoryW(pathutil::AddLongPathPrefix(dstAbs).c_str())) {
                error.clear();
                SetLastError(ERROR_SUCCESS);
                return true;
            }
            const DWORD code = GetLastError();
            error = L"la destinazione e una cartella non vuota: " + dstAbs;
            SetLastError(code);
            return false;
        }
        case LiveKind::UnsupportedReparse:
            error = L"destinazione e un reparse point non supportato (preservato): " +
                    dstAbs;
            SetLastError(ERROR_ACCESS_DENIED);
            return false;
        default:
            error = L"destinazione illeggibile: " + dstAbs;
            SetLastError(ERROR_ACCESS_DENIED);
            return false;
    }
}

bool EnsureParentDirs(const SyncPlan& plan, const std::wstring& dstAbs,
                      std::wstring& error) {
    const size_t slash = dstAbs.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
        error = L"percorso senza cartella: " + dstAbs;
        return false;
    }
    return CreateDirAll(plan.destRoot, dstAbs.substr(0, slash), error);
}

SyncActionResult RunCopy(const SyncPlan& plan, const SyncAction& a,
                         const std::wstring& srcAbs, const std::wstring& dstAbs,
                         const std::atomic_bool* cancel) {
    std::wstring err;
    if (!EnsureParentDirs(plan, dstAbs, err)) return Fail(a.op, a.relativePath, err);
    if (!PrepareFileDest(dstAbs, err)) return Fail(a.op, a.relativePath, err);
    if (!CopyFileDirect(plan.sourceRoot, srcAbs, plan.destRoot, dstAbs, cancel, err)) {
        const DWORD code = GetLastError();
        if (IsCancelCode(code, cancel)) {
            SyncActionResult r = Fail(a.op, a.relativePath, err);
            r.cancelled = true;
            return r;
        }
        return Fail(a.op, a.relativePath, err);
    }
    return Done(a.op, a.relativePath);
}

SyncActionResult RunLink(const SyncPlan& plan, const SyncAction& a,
                         const std::wstring& srcAbs, const std::wstring& dstAbs,
                         bool replace) {
    // Fresh live target (post-scan changes converge to the live tree).
    ReparseKind liveKind = ReparseKind::None;
    std::wstring target = ReadLinkTarget(srcAbs, &liveKind);
    if (target.empty()) {
        return Fail(a.op, a.relativePath, L"target del link sorgente illeggibile: " + srcAbs);
    }
    // Junctions need absolute targets; a relative one cannot be materialized.
    if (a.linkKind == ReparseKind::Junction && !(target.size() >= 2 && target[1] == L':')) {
        return Fail(a.op, a.relativePath,
                    L"junction con target relativo non creabile: " + srcAbs);
    }
    std::wstring chainWhy = CheckParentChain(plan.sourceRoot, srcAbs);
    if (!chainWhy.empty()) {
        return Fail(a.op, a.relativePath, L"sorgente non raggiungibile in sicurezza: " +
                                              chainWhy);
    }
    std::wstring err;
    if (!EnsureParentDirs(plan, dstAbs, err)) return Fail(a.op, a.relativePath, err);
    // Delete-first replace (documented limit): the old object is removed and
    // the new link is created from the fresh live target. If creation fails,
    // the destination stays absent with a clear error: re-running the sync
    // recreates it (the row becomes Missing). No temp-link dance: Windows
    // offers no reliable atomic link swap across symlink/junction/dir kinds.
    if (replace || StatLiveKind(dstAbs) != LiveKind::Absent) {
        const LiveKind dk = StatLiveKind(dstAbs);
        if (dk == LiveKind::LinkFile || dk == LiveKind::LinkDir) {
            if (!DeleteLink(dstAbs, dk == LiveKind::LinkDir, err))
                return Fail(a.op, a.relativePath, err);
        } else if (dk == LiveKind::File) {
            if (!DeleteFileOne(plan.destRoot, dstAbs, err))
                return Fail(a.op, a.relativePath, err);
        } else if (dk == LiveKind::Dir) {
            if (!RemoveDirectoryW(pathutil::AddLongPathPrefix(dstAbs).c_str())) {
                return Fail(a.op, a.relativePath,
                            L"la destinazione e una cartella non vuota: " + dstAbs);
            }
        } else {
            return Fail(a.op, a.relativePath,
                        L"destinazione non supportata (preservata): " + dstAbs);
        }
    }
    target = RebaseLinkTargetForWrite(target, plan.sourceRoot, plan.destRoot);
    if (!CreateLink(dstAbs, target, a.linkKind, err)) {
        return Fail(a.op, a.relativePath,
                    L"link rimosso ma ricreazione fallita (rieseguire la "
                    L"sincronizzazione): " +
                        err);
    }
    return Done(a.op, a.relativePath);
}

} // namespace

SyncReport ExecutePlan(const SyncPlan& plan, const std::atomic_bool* cancel,
                       const SyncProgressFn& onProgress) {
    SyncReport report;
    const size_t total = plan.actions.size();
    size_t done = 0;
    const auto progress = [&](const std::wstring& rel) {
        if (onProgress) onProgress(done, total, rel);
    };
    const auto finishItem = [&](SyncActionResult r) {
        if (r.ok) ++report.doneCount;
        else if (r.skipped) ++report.skippedCount;
        else ++report.failedCount;
        report.items.push_back(std::move(r));
        ++done;
    };
    const auto abortRemaining = [&](size_t from, bool cancelled, bool diskFull,
                                    const std::wstring& why) {
        for (size_t i = from; i < plan.actions.size(); ++i) {
            SyncActionResult r;
            r.op = plan.actions[i].op;
            r.relativePath = plan.actions[i].relativePath;
            r.cancelled = true;
            r.message = why;
            report.items.push_back(std::move(r));
        }
        report.cancelled = cancelled;
        report.diskFullAbort = diskFull;
        done = total;
    };

    // Free-space precheck: inbound bytes clearly beyond the free space block
    // the whole plan before anything runs.
    if (plan.summary.bytesToCopy > 0) {
        const uint64_t free = FreeBytesOnVolume(plan.destRoot);
        if (free > 0 && plan.summary.bytesToCopy > free) {
            SyncActionResult r;
            r.message = L"spazio insufficiente: servono circa " +
                        std::to_wstring(plan.summary.bytesToCopy) + L" byte, liberi " +
                        std::to_wstring(free) + L". Sincronizzazione non avviata.";
            report.items.push_back(std::move(r));
            ++report.failedCount;
            report.diskFullAbort = true;
            if (onProgress) onProgress(0, total, L"");
            return report;
        }
    }

    for (size_t i = 0; i < plan.actions.size(); ++i) {
        const SyncAction& a = plan.actions[i];
        if (WasCancelled(cancel)) {
            abortRemaining(i, /*cancelled=*/true, /*diskFull=*/false,
                           L"interrotta dall'utente");
            return report;
        }
        progress(a.relativePath);
        const std::wstring srcAbs = ResolveWithinRoot(plan.sourceRoot, a.relativePath);
        const std::wstring dstAbs = ResolveWithinRoot(plan.destRoot, a.relativePath);
        if ((a.op != SyncOp::FileDelete && a.op != SyncOp::DirDelete &&
             a.op != SyncOp::LinkDelete && srcAbs.empty()) ||
            dstAbs.empty()) {
            finishItem(Fail(a.op, a.relativePath, L"percorso fuori radice"));
            continue;
        }
        switch (a.op) {
            case SyncOp::FileCopy:
            case SyncOp::FileReplace: {
                SyncActionResult r = RunCopy(plan, a, srcAbs, dstAbs, cancel);
                const DWORD code = GetLastError();
                if (!r.ok && !r.cancelled && IsDiskFullCode(code)) {
                    finishItem(std::move(r));
                    abortRemaining(i + 1, /*cancelled=*/true, /*diskFull=*/true,
                                   L"interrotta: disco pieno");
                    return report;
                }
                finishItem(std::move(r));
                break;
            }
            case SyncOp::FileDelete: {
                std::wstring err;
                if (DeleteFileOne(plan.destRoot, dstAbs, err)) {
                    finishItem(Done(a.op, a.relativePath, err)); // err holds no-op notes
                } else {
                    finishItem(Fail(a.op, a.relativePath, err));
                }
                break;
            }
            case SyncOp::DirCreate: {
                // Adapts to a conflicting live destination: a file or a
                // supported link gives way (the plan row describes this
                // path); anything else fails loudly.
                std::wstring err;
                bool clearOk = true;
                switch (StatLiveKind(dstAbs)) {
                    case LiveKind::Absent:
                    case LiveKind::Dir:
                        break;
                    case LiveKind::File:
                        clearOk = DeleteFileOne(plan.destRoot, dstAbs, err);
                        break;
                    case LiveKind::LinkFile:
                    case LiveKind::LinkDir: {
                        const LiveKind dk = StatLiveKind(dstAbs);
                        clearOk = DeleteLink(dstAbs, dk == LiveKind::LinkDir, err);
                        break;
                    }
                    default:
                        clearOk = false;
                        err = L"destinazione non supportata (preservata): " + dstAbs;
                        break;
                }
                if (clearOk && CreateDirAll(plan.destRoot, dstAbs, err)) {
                    finishItem(Done(a.op, a.relativePath));
                } else {
                    finishItem(Fail(a.op, a.relativePath, err));
                }
                break;
            }
            case SyncOp::DirDelete: {
                DeleteDirOutcome o = DeleteDirGuarded(plan.destRoot, dstAbs,
                                                       plan.extraFoldedRels, cancel);
                if (o.cancelled) {
                    SyncActionResult r = Fail(a.op, a.relativePath, o.message);
                    r.cancelled = true;
                    finishItem(std::move(r));
                    abortRemaining(i + 1, /*cancelled=*/true, /*diskFull=*/false,
                                   L"interrotta dall'utente");
                    return report;
                }
                if (o.ok) {
                    finishItem(Done(a.op, a.relativePath));
                } else if (o.skipped) {
                    finishItem(Skip(a.op, a.relativePath, o.message));
                } else {
                    finishItem(Fail(a.op, a.relativePath, o.message));
                }
                break;
            }
            case SyncOp::LinkCreate:
            case SyncOp::LinkReplace: {
                SyncActionResult r =
                    RunLink(plan, a, srcAbs, dstAbs, a.op == SyncOp::LinkReplace);
                finishItem(std::move(r));
                break;
            }
            case SyncOp::LinkDelete: {
                std::wstring err;
                if (const std::wstring chainWhy = CheckParentChain(plan.destRoot, dstAbs);
                    !chainWhy.empty()) {
                    finishItem(Fail(a.op, a.relativePath,
                                    L"link non raggiungibile in sicurezza: " + chainWhy));
                    break;
                }
                const LiveKind dk = StatLiveKind(dstAbs);
                if (dk == LiveKind::Absent) {
                    finishItem(Done(a.op, a.relativePath, L"gia assente"));
                } else if (dk == LiveKind::LinkFile || dk == LiveKind::LinkDir) {
                    if (DeleteLink(dstAbs, dk == LiveKind::LinkDir, err)) {
                        finishItem(Done(a.op, a.relativePath));
                    } else {
                        finishItem(Fail(a.op, a.relativePath, err));
                    }
                } else if (dk == LiveKind::UnsupportedReparse) {
                    finishItem(Fail(a.op, a.relativePath,
                                    L"reparse point non supportato (preservato): " +
                                        dstAbs));
                } else {
                    finishItem(Fail(a.op, a.relativePath,
                                    L"non e piu un link, preservato: " + dstAbs));
                }
                break;
            }
        }
    }
    if (onProgress) onProgress(total, total, L"");
    return report;
}

} // namespace sync
} // namespace bv
