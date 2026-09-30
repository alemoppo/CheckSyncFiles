#include "Sync/SyncPath.h"

#include "Filesystem/PathUtil.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace bv {
namespace sync {

std::wstring ResolveWithinRoot(const std::wstring& root, const std::wstring& rel) {
    if (rel.empty()) return {};
    // Absolute or UNC-looking rel: reject outright.
    if (rel.size() >= 2 && rel[1] == L':') return {};
    if (rel[0] == L'\\' || rel[0] == L'/') return {};
    const wchar_t last = rel[rel.size() - 1];
    if (last == L'\\' || last == L'/') return {}; // no trailing separator
    // Component scan: no ".." anywhere (canonical rels never have one) and
    // no empty components from doubled separators.
    size_t i = 0;
    while (i < rel.size()) {
        size_t j = i;
        while (j < rel.size() && rel[j] != L'\\' && rel[j] != L'/') ++j;
        const size_t len = j - i;
        if (len == 0) return {}; // leading/doubled/trailing separator
        if (len == 2 && rel[i] == L'.' && rel[i + 1] == L'.') return {};
        // A lone "." is harmless but non-canonical: reject for strictness.
        if (len == 1 && rel[i] == L'.') return {};
        i = j + 1;
    }
    std::wstring flat = rel;
    for (wchar_t& c : flat) {
        if (c == L'/') c = L'\\';
    }
    const std::wstring nRoot = pathutil::NormalizeRoot(root);
    if (nRoot.empty()) return {};
    const std::wstring abs = nRoot + L"\\" + flat;
    // Folded prefix + separator boundary containment.
    const std::wstring fRoot = pathutil::FoldForCompare(nRoot);
    const std::wstring fAbs = pathutil::FoldForCompare(abs);
    if (fAbs.size() <= fRoot.size()) return {};
    if (fAbs.compare(0, fRoot.size(), fRoot) != 0) return {};
    if (fAbs[fRoot.size()] != L'\\') return {};
    return abs;
}

std::wstring CheckParentChain(const std::wstring& root, const std::wstring& abs) {
    const std::wstring nRoot = pathutil::NormalizeRoot(root);
    if (nRoot.empty()) return L"percorso fuori radice";
    if (pathutil::FoldForCompare(abs) == pathutil::FoldForCompare(nRoot)) {
        return {}; // the root itself: no intermediate components to check
    }
    if (abs.size() <= nRoot.size()) return L"percorso fuori radice";
    std::wstring rest = abs.substr(nRoot.size());
    if (rest.empty() || rest[0] != L'\\') return L"percorso fuori radice";
    rest.erase(0, 1);
    // All components but the leaf: each existing one must be a plain dir.
    std::wstring cur = nRoot;
    size_t i = 0;
    while (i < rest.size()) {
        size_t j = i;
        while (j < rest.size() && rest[j] != L'\\') ++j;
        const bool last = (j == rest.size());
        cur += L"\\" + rest.substr(i, j - i);
        if (!last) {
            const DWORD attrs = GetFileAttributesW(pathutil::AddLongPathPrefix(cur).c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES) {
                const DWORD code = GetLastError();
                if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                    return {}; // absent: created later, re-checked per level
                }
                return L"componente intermedio illeggibile: " + cur;
            }
            if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                return L"componente intermedio non cartella: " + cur;
            }
            if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                return L"componente intermedio e un reparse point (non seguito): " + cur;
            }
        }
        i = j + 1;
    }
    return {};
}

uint64_t FreeBytesOnVolume(const std::wstring& path) {
    const std::wstring win = pathutil::AddLongPathPrefix(pathutil::NormalizeRoot(path));
    ULARGE_INTEGER free = {};
    if (!GetDiskFreeSpaceExW(win.c_str(), &free, nullptr, nullptr)) return 0;
    return free.QuadPart;
}

} // namespace sync
} // namespace bv
