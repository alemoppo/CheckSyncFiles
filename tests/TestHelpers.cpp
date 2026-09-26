#include "TestHelpers.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Filesystem/PathUtil.h"

namespace bv {
namespace testutil {

namespace fs = std::filesystem;

namespace {

std::vector<std::filesystem::path> g_cleanup;
int g_counter = 0;

} // namespace

std::wstring MakeTempDir() {
    const fs::path tmp = fs::temp_directory_path();
    const fs::path dir = tmp / (L"bvtest_" + std::to_wstring(GetCurrentProcessId()) +
                                L"_" + std::to_wstring(g_counter++));
    fs::create_directories(dir);
    g_cleanup.push_back(dir);
    return dir.wstring();
}

// Recursively delete a directory tree using the Win32 API with the long-path
// prefix on every operation. std::filesystem::remove_all cannot handle paths
// longer than MAX_PATH (e.g. the deep-tree test), which made it hang.
bool RemoveAllWin(const std::wstring& path) {
    const std::wstring pref = pathutil::AddLongPathPrefix(path) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pref.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return RemoveDirectoryW(pathutil::AddLongPathPrefix(path).c_str()) != 0;
    }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        const std::wstring child = path + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                // Junctions / directory symlinks must NOT be traversed here:
                // a link can point back into the tree being cleaned (a cycle).
                // Removing the link itself (not its target) avoids that loop.
                RemoveDirectoryW(pathutil::AddLongPathPrefix(child).c_str());
            } else {
                RemoveAllWin(child);
            }
        } else {
            DeleteFileW(pathutil::AddLongPathPrefix(child).c_str());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return RemoveDirectoryW(pathutil::AddLongPathPrefix(path).c_str()) != 0;
}

void CleanupTempDirs() {
    for (const auto& p : g_cleanup) {
        RemoveAllWin(p.wstring());
    }
    g_cleanup.clear();
}

ScanReport RunScan(const std::wstring& src, const std::wstring& dst,
                   ScanMode mode, bool caseSensitive, unsigned int threads) {
    ScanOptions opts;
    opts.source = src;
    opts.destination = dst;
    opts.mode = mode;
    opts.caseSensitive = caseSensitive;
    opts.hashThreads = threads;
    ScanController controller(caseSensitive);
    return controller.run(opts);
}

// Path of the deepest file created by CreateLongPathTree.
std::wstring LongPathRelative() {
    std::wstring rel;
    for (int i = 0; i < 90; ++i) {
        rel += L"long_" + std::to_wstring(i) + L"\\";
    }
    return rel + L"deepfile.txt";
}

bool DenyListAccess(const std::wstring& dir, const std::wstring& mask) {
    std::wstring cmd = L"icacls \"" + dir +
                       L"\" /deny \"" + _wgetenv(L"USERNAME") +
                       L"\":" + mask + L" /C 2>nul";
    return _wsystem(cmd.c_str()) == 0;
}

void RestoreAccess(const std::wstring& dir) {
    std::wstring cmd = L"icacls \"" + dir +
                       L"\" /remove:d \"" + _wgetenv(L"USERNAME") +
                       L"\" /C 2>nul";
    _wsystem(cmd.c_str());
    cmd = L"icacls \"" + dir + L"\" /reset /C 2>nul";
    _wsystem(cmd.c_str());
}

// Writes raw bytes to a file (used to build deterministic content differences).
bool WriteFileBytes(const std::wstring& path, const char* data, size_t n) {
    const HANDLE h = CreateFileW(pathutil::AddLongPathPrefix(path).c_str(), GENERIC_WRITE,
                                 FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, data, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(h);
    return ok != FALSE && written == n;
}

std::string ReadFileBytes(const std::wstring& path) {
    std::ifstream in(pathutil::AddLongPathPrefix(path).c_str(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void BumpMtimeMinutes(const std::wstring& path, int minutes) {
    FILETIME ft{};
    const HANDLE h = CreateFileW(pathutil::AddLongPathPrefix(path).c_str(),
                                 FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    GetFileTime(h, nullptr, nullptr, &ft);
    ULARGE_INTEGER ui;
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    ui.QuadPart += static_cast<ULONGLONG>(minutes) * 60ull * 10'000'000ull; // 100ns units
    ft.dwLowDateTime = ui.LowPart;
    ft.dwHighDateTime = ui.HighPart;
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}

// Creates a directory junction `link` pointing at `target` via the shell's
// `mklink /J` (a cmd built-in). The link directory must NOT already exist.
// Junctions need no administrator rights (unlike symlinks), so this is
// exercisable on an ordinary temp directory. Building the junction reparse
// buffer by hand (FSCTL_SET_REPARSE_POINT) is fragile and version-dependent,
// so we reuse the OS tool instead -- the same approach as the icacls helpers
// above. Returns false when junction creation is not supported.
bool CreateJunction(const std::wstring& link, const std::wstring& target) {
    std::wstring cmd = L"cmd /c mklink /J \"" + link + L"\" \"" + target + L"\" >nul 2>nul";
    return _wsystem(cmd.c_str()) == 0;
}

} // namespace testutil
} // namespace bv
