#include "TestTree.h"

#include <cstdint>
#include <filesystem>
#include <random>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Filesystem/PathUtil.h"

namespace bv {
namespace testgen {

namespace fs = std::filesystem;

bool CreateFileOfSize(const std::wstring& path, uint64_t bytes) {
    const std::wstring win = pathutil::AddLongPathPrefix(path);
    HANDLE h = CreateFileW(win.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    if (bytes > 0) {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(bytes);
        ok = SetFilePointerEx(h, li, nullptr, FILE_BEGIN) &&
             SetEndOfFile(h);
    }
    CloseHandle(h);
    return ok;
}

static void WriteContent(const std::wstring& path, const std::wstring& content) {
    const std::wstring win = pathutil::AddLongPathPrefix(path);
    HANDLE h = CreateFileW(win.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const char* data = reinterpret_cast<const char*>(content.data());
    const DWORD bytes = static_cast<DWORD>(content.size() * sizeof(wchar_t));
    ::WriteFile(h, data, bytes, &written, nullptr);
    CloseHandle(h);
}

void CreateFixture(const std::wstring& root) {
    fs::create_directories(root);
    WriteContent(root + L"\\a.txt", L"hello world");
    WriteContent(root + L"\\b.txt", L"second file");
    fs::create_directories(root + L"\\Foto\\2025");
    WriteContent(root + L"\\Foto\\2025\\pic1.jpg", L"picture one");
    WriteContent(root + L"\\Foto\\2025\\pic2.jpg", L"picture one"); // same content
    fs::create_directories(root + L"\\Unicode");
    WriteContent(root + L"\\Unicode\\\u00f9test.txt", L"unicode name");
    fs::create_directories(root + L"\\empty");
    CreateDeepPath(root, 20, L"deep.txt");
}

void CreateDifferingTrees(const std::wstring& root) {
    const std::wstring src = root + L"\\src";
    const std::wstring dst = root + L"\\dst";
    fs::create_directories(src);
    fs::create_directories(dst);

    // identical in both
    WriteContent(src + L"\\a.txt", L"same");
    WriteContent(dst + L"\\a.txt", L"same");

    // missing in destination
    WriteContent(src + L"\\b_missing.bin", L"only in source");

    // extra in destination
    WriteContent(dst + L"\\c_extra.bin", L"only in destination");

    // same name, different size
    WriteContent(src + L"\\d.bin", L"0123456789");          // 20 bytes
    WriteContent(dst + L"\\d.bin", L"01234567890123456789"); // 40 bytes

    // same name + same size, different content (size mode = identical)
    WriteContent(src + L"\\e.bin", L"AAAA"); // 8 bytes
    WriteContent(dst + L"\\e.bin", L"BBBB"); // 8 bytes

    // empty dir only in source
    fs::create_directories(src + L"\\empty_src");
    // empty dir only in destination
    fs::create_directories(dst + L"\\empty_dst");

    // directories
    fs::create_directories(src + L"\\sub");
    fs::create_directories(dst + L"\\sub");
    WriteContent(src + L"\\sub\\f.txt", L"f");
    WriteContent(dst + L"\\sub\\f.txt", L"f");
}

size_t CreateStressTree(const std::wstring& root, size_t count) {
    fs::create_directories(root);
    size_t created = 0;
    for (size_t i = 0; i < count; ++i) {
        const std::wstring dir = root + L"\\d" + std::to_wstring(i % 100);
        fs::create_directories(dir);
        const std::wstring path = dir + L"\\f" + std::to_wstring(i) + L".dat";
        if (CreateFileOfSize(path, 1)) ++created;
    }
    return created;
}

void CreateDeepPath(const std::wstring& root, int levels, const std::wstring& leafFile) {
    std::wstring cur = root;
    for (int i = 0; i < levels; ++i) {
        cur += L"\\d" + std::to_wstring(i);
        fs::create_directories(cur);
    }
    WriteContent(cur + L"\\" + leafFile, L"deep");
}

std::wstring CreateLongPathTree(const std::wstring& base) {
    std::wstring cur = base;
    // Build a chain ~90 components (~600+ chars) to exceed MAX_PATH.
    for (int i = 0; i < 90; ++i) {
        cur += L"\\long_" + std::to_wstring(i);
        const std::wstring win = pathutil::AddLongPathPrefix(cur);
        if (!CreateDirectoryW(win.c_str(), nullptr)) {
            const DWORD err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS) break;
        }
    }
    WriteContent(cur + L"\\deepfile.txt", L"long path");
    return base;
}

} // namespace testgen
} // namespace bv
