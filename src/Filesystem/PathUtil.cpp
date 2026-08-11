#include "PathUtil.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace bv {
namespace pathutil {

std::wstring NormalizeRoot(const std::wstring& root) {
    if (root.empty()) return root;
    std::wstring s = root;

    // Remove an explicit long-path prefix.
    if (s.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        s = L"\\\\" + s.substr(8);
    } else if (s.rfind(L"\\\\?\\", 0) == 0) {
        s = s.substr(4);
    }

    // Trim trailing backslashes, but never reduce below a root form.
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();

    if (s == L"\\\\") s = L"\\";
    return s;
}

std::wstring JoinRel(const std::wstring& rel, const std::wstring& name) {
    if (rel.empty()) return name;
    return rel + L"\\" + name;
}

std::wstring MakeAbsolute(const std::wstring& rootAbs, const std::wstring& rel) {
    if (rel.empty()) return rootAbs;
    return rootAbs + L"\\" + rel;
}

std::wstring AddLongPathPrefix(const std::wstring& abs) {
    if (abs.rfind(L"\\\\?\\", 0) == 0) return abs;
    if (abs.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + abs.substr(2);
    return L"\\\\?\\" + abs;
}

std::wstring FoldForCompare(const std::wstring& s) {
    if (s.empty()) return s;
    // Allow room for possible multi-char case-folding expansions.
    std::wstring out(s.size() * 4, L'\0');
    int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE,
                          s.data(), static_cast<int>(s.size()),
                          &out[0], static_cast<int>(out.size()),
                          nullptr, nullptr, 0);
    if (n > 0) {
        out.resize(n);
        return out;
    }
    // Fallback: ASCII-only uppercasing (should never happen on Windows).
    out = s;
    for (wchar_t& c : out) {
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    }
    return out;
}

} // namespace pathutil
} // namespace bv
