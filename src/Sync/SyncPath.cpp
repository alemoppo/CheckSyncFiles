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

uint64_t FreeBytesOnVolume(const std::wstring& path) {
    const std::wstring win = pathutil::AddLongPathPrefix(pathutil::NormalizeRoot(path));
    ULARGE_INTEGER free = {};
    if (!GetDiskFreeSpaceExW(win.c_str(), &free, nullptr, nullptr)) return 0;
    return free.QuadPart;
}

} // namespace sync
} // namespace bv
