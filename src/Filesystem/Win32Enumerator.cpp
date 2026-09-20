#include "Win32Enumerator.h"

#include <cwchar>
#include <vector>

#include "Errors.h"
#include "PathUtil.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace bv {

namespace {

uint64_t MakeFileTime(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

uint64_t MakeSize64(DWORD high, DWORD low) {
    return (static_cast<uint64_t>(high) << 32) | low;
}

} // namespace

bool Win32Enumerator::enumerate(const std::wstring& root,
                                const EntryCallback& onEntry,
                                const ErrorCallback& onError,
                                const ProgressCallback& onProgress,
                                const std::atomic_bool* cancel) {
    const std::wstring normRoot = pathutil::NormalizeRoot(root);
    if (normRoot.empty()) {
        onError({L"", L"Root path is empty", ERROR_INVALID_NAME});
        return false;
    }

    const std::wstring rootWin = pathutil::AddLongPathPrefix(normRoot);
    const DWORD rootAttrs = GetFileAttributesW(rootWin.c_str());
    if (rootAttrs == INVALID_FILE_ATTRIBUTES) {
        onError({L"", L"Root path is not accessible", GetLastError()});
        return false;
    }
    if (!(rootAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
        onError({L"", L"Root path is not a directory", ERROR_DIRECTORY});
        return false;
    }

    struct Frame {
        std::wstring abs; // absolute (unprefixed) directory path
        std::wstring rel; // relative path of this directory ("" == root)
    };

    std::vector<Frame> stack;
    stack.push_back({normRoot, L""});

    uint64_t totalFiles = 0;
    uint64_t totalDirs = 0;
    uint64_t totalBytes = 0;

    while (!stack.empty()) {
        Frame f = stack.back();
        stack.pop_back();

        // Slowest-directories timing: only the FindFirst/FindNext syscalls are
        // measured, never the entry callbacks (downstream consumers may block
        // on hash-pool backpressure in Content mode; that cost belongs to the
        // hash column, not to the listing). Accumulated per directory and
        // reported once when its listing completes.
        const bool timeDirs = dirSink_ != nullptr;
        const auto clk = [&]() -> uint64_t { return timeDirs ? profiling::QpcNow() : 0; };
        uint64_t listTicks = 0;

        const std::wstring pattern = pathutil::AddLongPathPrefix(f.abs + L"\\*");
        WIN32_FIND_DATAW fd;
        uint64_t t = clk();
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        listTicks += clk() - t;
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            // ERROR_FILE_NOT_FOUND means an empty directory: not an error.
            if (err != ERROR_FILE_NOT_FOUND) {
                // The storage is gone: report and abort immediately; walking
                // further would only produce a flood of identical errors.
                if (IsDeviceDisconnectError(err)) {
                    ScanError lost;
                    lost.path = f.rel;
                    lost.message = L"dispositivo scollegato durante l'operazione";
                    lost.winError = err;
                    lost.lostDevice = true;
                    onError(lost);
                    return false;
                }
                onError({f.rel, L"Unable to enumerate directory", err});
            }
            if (onProgress) {
                onProgress(totalFiles, totalDirs, totalBytes, f.rel);
            }
            continue;
        }

        bool abort = false;
        // The first entry is already in `fd` (from FindFirstFileW); every
        // further entry comes from FindNextFileW at the top of the loop, so a
        // `continue` (e.g. for "." / "..") always advances to the next entry.
        bool first = true;
        for (;;) {
            if (!first) {
                t = clk();
                if (!FindNextFileW(h, &fd)) break;
                listTicks += clk() - t;
            }
            first = false;
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                abort = true;
                break;
            }
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            const std::wstring name = fd.cFileName;
            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool isReparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

            // Compute the relative path BEFORE invoking the callback: callbacks
            // may move the entry (FileEntry&&), which would leave e.relativePath
            // in a moved-from (empty) state.
            const std::wstring childRel = pathutil::JoinRel(f.rel, name);

            FileEntry e;
            e.relativePath = childRel;
            e.size = MakeSize64(fd.nFileSizeHigh, fd.nFileSizeLow);
            e.lastWriteTime = MakeFileTime(fd.ftLastWriteTime);
            e.attributes = fd.dwFileAttributes;
            e.fileId = 0;
            e.isDirectory = isDir;

            if (isDir) {
                ++totalDirs;
            } else {
                ++totalFiles;
                totalBytes += e.size;
            }

            // Record the entry itself, then decide whether to descend.
            if (!onEntry(std::move(e))) {
                abort = true;
                break;
            }

            // Directory reparse points are not followed (loop safety).
            if (isDir && !isReparse) {
                stack.push_back({f.abs + L"\\" + name, childRel});
            }
        }

        const DWORD loopErr = GetLastError();
        FindClose(h);
        if (abort) return true; // early stop requested by the consumer
        if (timeDirs) {
            // Scan-root-relative directory key (the walk root itself is "").
            const std::wstring rel =
                dirPrefix_.empty() ? f.rel
                                   : (f.rel.empty() ? dirPrefix_
                                                    : pathutil::JoinRel(dirPrefix_, f.rel));
            dirSink_->list.add(rel, profiling::QpcToSeconds(listTicks));
        }

        if (loopErr != ERROR_NO_MORE_FILES && loopErr != 0) {
            if (IsDeviceDisconnectError(loopErr)) {
                ScanError lost;
                lost.path = f.rel;
                lost.message = L"dispositivo scollegato durante l'operazione";
                lost.winError = loopErr;
                lost.lostDevice = true;
                onError(lost);
                return false;
            }
            onError({f.rel, L"Error while reading directory", loopErr});
        }
        if (onProgress) {
            onProgress(totalFiles, totalDirs, totalBytes, f.rel);
        }
    }

    return true;
}

} // namespace bv
