#pragma once

// Shared helpers for the bv_tests translation units (split out of the former
// monolithic test_main.cpp). Declared in bv::testutil; each test TU brings
// them in with `using namespace bv::testutil;` next to its own
// `using namespace bv;` (plus its own `namespace fs = std::filesystem;`).

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "Comparison/ScanMode.h"
#include "ScanController.h"

namespace bv {
namespace testutil {

std::wstring MakeTempDir();
bool RemoveAllWin(const std::wstring& path);
void CleanupTempDirs();

struct ScopeGuard {
    std::function<void()> fn;
    ~ScopeGuard() { fn(); }
};

ScanReport RunScan(const std::wstring& src, const std::wstring& dst,
                   ScanMode mode, bool caseSensitive = false, unsigned int threads = 2);

// Path of the deepest file created by CreateLongPathTree.
std::wstring LongPathRelative();

bool DenyListAccess(const std::wstring& dir, const std::wstring& mask = L"(OI)(CI)(RD)");
void RestoreAccess(const std::wstring& dir);

// Writes raw bytes to a file (used to build deterministic content differences).
bool WriteFileBytes(const std::wstring& path, const char* data, size_t n);
std::string ReadFileBytes(const std::wstring& path);

void BumpMtimeMinutes(const std::wstring& path, int minutes);

// Creates a directory junction `link` pointing at `target` via the shell's
// `mklink /J` (a cmd built-in). The link directory must NOT already exist.
// Junctions need no administrator rights (unlike symlinks). Returns false
// when junction creation is not supported.
bool CreateJunction(const std::wstring& link, const std::wstring& target);

} // namespace testutil
} // namespace bv
