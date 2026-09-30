#include "Sync/SyncFs.h"

#include <cstdio>
#include <unordered_set>
#include <vector>

#include "Filesystem/PathUtil.h"
#include "Sync/SyncPath.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace bv {
namespace sync {
namespace {

std::wstring Prefixed(const std::wstring& abs) {
    return pathutil::AddLongPathPrefix(abs);
}

bool WasCancelled(const std::atomic_bool* cancel) {
    return cancel && cancel->load(std::memory_order_relaxed);
}

DWORD CopyProgressBridge(LARGE_INTEGER /*total*/, LARGE_INTEGER /*done*/, LARGE_INTEGER /*strmTotal*/,
                         LARGE_INTEGER /*strmDone*/, DWORD /*stream*/, DWORD /*reason*/,
                         HANDLE /*src*/, HANDLE /*dst*/, LPVOID data) {
    const auto* cancel = static_cast<const std::atomic_bool*>(data);
    if (!cancel) return PROGRESS_CONTINUE;
    return WasCancelled(cancel) ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

bool PreserveTimesAndAttrs(const std::wstring& srcAbs, const std::wstring& dstAbs,
                           std::wstring& error) {
    const std::wstring src = Prefixed(srcAbs);
    HANDLE hs = CreateFileW(src.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hs == INVALID_HANDLE_VALUE) {
        error = L"lettura metadati sorgente fallita: " + srcAbs;
        return false;
    }
    FILETIME ct{}, at{}, wt{};
    const bool tok = GetFileTime(hs, &ct, &at, &wt);
    CloseHandle(hs);
    if (!tok) {
        error = L"lettura date sorgente fallita: " + srcAbs;
        return false;
    }
    const DWORD attrs = GetFileAttributesW(src.c_str());
    const std::wstring dst = Prefixed(dstAbs);
    // Writable for the timestamp/attribute write, then restore (even read-only
    // sources are mirrored: B becomes A).
    SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
    HANDLE hd = CreateFileW(dst.c_str(), FILE_WRITE_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hd == INVALID_HANDLE_VALUE) {
        error = L"apertura destinazione per i metadati fallita: " + dstAbs;
        return false;
    }
    const bool wok = SetFileTime(hd, &ct, &at, &wt);
    CloseHandle(hd);
    if (!wok) {
        error = L"scrittura date destinazione fallita: " + dstAbs;
        return false;
    }
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        SetFileAttributesW(dst.c_str(), attrs & ~FILE_ATTRIBUTE_REPARSE_POINT);
    }
    return true;
}

} // namespace

LiveKind StatLiveKind(const std::wstring& abs) {
    const std::wstring win = Prefixed(abs);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(win.c_str(), GetFileExInfoStandard, &data)) {
        const DWORD code = GetLastError();
        // Only true absence converges to Absent; anything else (denied, ...)
        // must fail loudly instead of looking already-gone.
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return LiveKind::Absent;
        }
        return LiveKind::Other;
    }
    const bool isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool isReparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    if (!isReparse) return isDir ? LiveKind::Dir : LiveKind::File;
    // Never mistake an unknown/unsupported reparse for a normal link: the
    // executor preserves UnsupportedReparse instead of acting on it.
    const ReparseKind kind = GetReparseKind(abs, isDir);
    if (kind == ReparseKind::SymlinkFile || kind == ReparseKind::SymlinkDir ||
        kind == ReparseKind::Junction) {
        return isDir ? LiveKind::LinkDir : LiveKind::LinkFile;
    }
    return LiveKind::UnsupportedReparse;
}

bool CopyFileDirect(const std::wstring& srcRoot, const std::wstring& srcAbs,
                    const std::wstring& dstRoot, const std::wstring& dstAbs,
                    const std::atomic_bool* cancel, std::wstring& error) {
    if (WasCancelled(cancel)) {
        error = L"operazione annullata prima della copia.";
        return false;
    }
    std::wstring chainWhy = CheckParentChain(srcRoot, srcAbs);
    if (!chainWhy.empty()) {
        error = L"sorgente non raggiungibile in sicurezza: " + chainWhy;
        return false;
    }
    chainWhy = CheckParentChain(dstRoot, dstAbs);
    if (!chainWhy.empty()) {
        error = L"destinazione non raggiungibile in sicurezza: " + chainWhy;
        return false;
    }
    // Never copy THROUGH a link: links travel via CreateLink only.
    if (StatLiveKind(srcAbs) != LiveKind::File) {
        error = L"sorgente non piu un file regolare (rinominata/link?): " + srcAbs;
        return false;
    }
    // Direct overwrite by design (see header): a failed copy may truncate
    // the destination; the next comparison re-detects it for a retry.
    const BOOL copied =
        CopyFileExW(Prefixed(srcAbs).c_str(), Prefixed(dstAbs).c_str(),
                    CopyProgressBridge, const_cast<std::atomic_bool*>(cancel), nullptr,
                    0);
    if (!copied) {
        const DWORD code = GetLastError();
        if (code == ERROR_REQUEST_ABORTED && WasCancelled(cancel)) {
            error = L"copia annullata: " + srcAbs;
        } else if (code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL) {
            error = L"spazio insufficiente durante la copia: " + dstAbs;
        } else {
            error = L"copia fallita (codice " + std::to_wstring(code) + L"): " + srcAbs;
        }
        SetLastError(code); // let the executor tell disk-full/cancel apart
        return false;
    }
    if (!PreserveTimesAndAttrs(srcAbs, dstAbs, error)) {
        return false;
    }
    error.clear();
    SetLastError(ERROR_SUCCESS);
    return true;
}

bool CreateDirAll(const std::wstring& root, const std::wstring& dirAbs,
                  std::wstring& error) {
    const std::wstring chainWhy = CheckParentChain(root, dirAbs);
    if (!chainWhy.empty()) {
        error = L"cartella non raggiungibile in sicurezza: " + chainWhy;
        return false;
    }
    const std::wstring nRoot = pathutil::NormalizeRoot(root);
    // Walk down component by component so every created level is contained.
    std::wstring cur = nRoot;
    std::wstring rest = dirAbs.size() > nRoot.size() ? dirAbs.substr(nRoot.size()) : std::wstring();
    while (!rest.empty() && (rest[0] == L'\\' || rest[0] == L'/')) rest.erase(0, 1);
    size_t i = 0;
    while (i < rest.size()) {
        size_t j = i;
        while (j < rest.size() && rest[j] != L'\\' && rest[j] != L'/') ++j;
        cur += L"\\" + rest.substr(i, j - i);
        if (!CreateDirectoryW(Prefixed(cur).c_str(), nullptr)) {
            const DWORD code = GetLastError();
            if (code != ERROR_ALREADY_EXISTS) {
                error = L"creazione cartella fallita (codice " + std::to_wstring(code) +
                        L"): " + cur;
                return false;
            }
        }
        // Whatever is in the way must be a PLAIN directory: a file is
        // unrecoverable here, and a reparse point would divert everything
        // created below it outside the root (re-checked per level, not just
        // once up front, so a concurrently created link is also caught).
        const DWORD attrs = GetFileAttributesW(Prefixed(cur).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            error = L"cartella illeggibile dopo la creazione: " + cur;
            return false;
        }
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            error = L"un file blocca la creazione della cartella: " + cur;
            return false;
        }
        if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            error = L"un reparse point blocca la creazione della cartella (non seguito): " +
                    cur;
            return false;
        }
        i = j + 1;
    }
    error.clear();
    return true;
}

bool DeleteFileOne(const std::wstring& root, const std::wstring& abs,
                   std::wstring& error) {
    const std::wstring chainWhy = CheckParentChain(root, abs);
    if (!chainWhy.empty()) {
        error = L"file non raggiungibile in sicurezza: " + chainWhy;
        return false;
    }
    const std::wstring win = Prefixed(abs);
    SetFileAttributesW(win.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (DeleteFileW(win.c_str())) {
        error.clear();
        return true;
    }
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
        error.clear(); // already gone: converge, don't fail
        return true;
    }
    error = L"eliminazione file fallita (codice " + std::to_wstring(code) + L"): " + abs;
    SetLastError(code);
    return false;
}

DeleteDirOutcome DeleteDirGuarded(const std::wstring& root, const std::wstring& dirAbs,
                                  const std::vector<std::wstring>& allowedFoldedRels) {
    DeleteDirOutcome out;
    const std::wstring chainWhy = CheckParentChain(root, dirAbs);
    if (!chainWhy.empty()) {
        out.message = L"cartella non raggiungibile in sicurezza: " + chainWhy;
        return out;
    }
    const std::wstring nRoot = pathutil::NormalizeRoot(root);
    const std::wstring baseRel =
        dirAbs.size() > nRoot.size() ? dirAbs.substr(nRoot.size() + 1) : std::wstring();
    if (baseRel.empty()) {
        out.message = L"rifiuto: la radice non si elimina.";
        return out; // never delete the root itself
    }
    const std::wstring win = Prefixed(dirAbs);
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(win.c_str(), GetFileExInfoStandard, &data)) {
            const DWORD code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                out.ok = true; // already gone
                return out;
            }
            out.message = L"lettura cartella fallita (codice " + std::to_wstring(code) +
                          L"): " + dirAbs;
            return out;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            out.message = L"non e piu una cartella: " + dirAbs;
            return out;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            out.message = L"e diventata un link (non seguita): " + dirAbs;
            return out;
        }
    }
    std::unordered_set<std::wstring> allowed(allowedFoldedRels.begin(),
                                             allowedFoldedRels.end());
    const auto isAllowed = [&](const std::wstring& rel) {
        return allowed.find(pathutil::FoldForCompare(rel)) != allowed.end();
    };
    // Iterative depth-first teardown; links unlinked, never traversed. Each
    // frame visit re-lists its directory, so concurrent changes converge
    // instead of looping on stale handles.
    struct Frame {
        std::wstring abs;
        std::wstring rel; // root-relative, canonical
        bool dirty = false; // something was removed inside during this run
    };
    std::vector<Frame> stack;
    stack.push_back({dirAbs, baseRel, false});
    const auto abortSkip = [&](const std::wstring& entryAbs, const std::wstring& why) {
        out.skipped = true;
        out.message = why + L": " + entryAbs;
    };
    while (!stack.empty()) {
        Frame& top = stack.back();
        std::vector<std::pair<std::wstring, DWORD>> children;
        {
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW((Prefixed(top.abs) + L"\\*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) {
                const DWORD code = GetLastError();
                if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
                    abortSkip(top.abs, L"lettura cartella fallita");
                    return out;
                }
            } else {
                do {
                    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                        continue;
                    children.emplace_back(fd.cFileName, fd.dwFileAttributes);
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
        bool descended = false;
        for (const auto& [name, attrs] : children) {
            const std::wstring childAbs = top.abs + L"\\" + name;
            const std::wstring childRel = top.rel.empty() ? name : top.rel + L"\\" + name;
            const bool isDir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool isReparse = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if (isReparse) {
                // Unlink, never follow. Allowed only if the link itself is
                // listed AND it is a supported link: an unknown/unsupported
                // reparse point is never unlinked blindly, even when listed.
                if (!isAllowed(childRel)) {
                    abortSkip(childAbs, L"link non atteso, cartella preservata");
                    return out;
                }
                const ReparseKind childKind = GetReparseKind(childAbs, isDir);
                if (childKind != ReparseKind::SymlinkFile &&
                    childKind != ReparseKind::SymlinkDir &&
                    childKind != ReparseKind::Junction) {
                    abortSkip(childAbs,
                              L"reparse point non supportato, cartella preservata");
                    return out;
                }
                std::wstring err;
                if (!DeleteLink(childAbs, isDir, err)) {
                    abortSkip(childAbs, L"eliminazione link fallita");
                    return out;
                }
                top.dirty = true;
                continue;
            }
            if (!isDir) {
                if (!isAllowed(childRel)) {
                    abortSkip(childAbs, L"file non atteso, cartella preservata");
                    return out;
                }
                std::wstring err;
                if (!DeleteFileOne(root, childAbs, err)) {
                    out.message = err;
                    return out; // hard error, not a skip
                }
                top.dirty = true;
                continue;
            }
            // Subdirectory: descend and let the same rules decide. An
            // unexpected entry anywhere below aborts the whole tree.
            stack.push_back({childAbs, childRel, false});
            descended = true;
            break; // process the child frame before siblings
        }
        if (descended) continue;
        // No (more) children: remove this level only if it was listed itself
        // or we emptied it now. An untouched unlisted dir is shared content
        // (it exists on the source side too): keep it, skip the tree.
        if (!isAllowed(top.rel) && !top.dirty) {
            abortSkip(top.abs, L"cartella condivisa con la sorgente, preservata");
            return out;
        }
        SetFileAttributesW(Prefixed(top.abs).c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!RemoveDirectoryW(Prefixed(top.abs).c_str())) {
            const DWORD code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
                abortSkip(top.abs, L"rimozione cartella fallita");
                return out;
            }
        }
        stack.pop_back();
        if (!stack.empty()) stack.back().dirty = true;
    }
    out.ok = true;
    return out;
}

} // namespace sync
} // namespace bv
