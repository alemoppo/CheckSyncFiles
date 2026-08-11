#pragma once

// Builds realistic on-disk test trees. Also used by the testgen tool.

#include <cstdint>
#include <string>
#include <vector>

namespace bv {
namespace testgen {

struct TreeSpec {
    std::wstring root; // parent directory; the tree is created directly under it
};

// Creates a small, well-known fixture with known "expected" outcome when
// compared against itself (all identical) — see the unit tests.
// Layout under root:
//   a.txt                 (identical)
//   b.txt                 (identical)
//   Foto/2025/pic1.jpg    (identical)
//   Foto/2025/pic2.jpg    (identical, same content different name)
//   Unicode/\u00f9test.txt
//   empty/
//   deep/a/b/c/.../deep.txt
void CreateFixture(const std::wstring& root);

// Creates two trees (root/src and root/dst) with deliberate differences,
// used by the CLI testgen tool and integration tests.
void CreateDifferingTrees(const std::wstring& root);

// Creates `count` small files spread over `dirsPerLevel` directories
// (stress / scale test). Returns the number of files actually created.
size_t CreateStressTree(const std::wstring& root, size_t count);

// Creates one file of `bytes` bytes with pseudo-random content.
bool CreateFileOfSize(const std::wstring& path, uint64_t bytes);

// Creates a nested directory chain under root with the given number of levels.
void CreateDeepPath(const std::wstring& root, int levels, const std::wstring& leafFile);

// Returns a path that exceeds MAX_PATH (for long-path tests).
std::wstring CreateLongPathTree(const std::wstring& base);

} // namespace testgen
} // namespace bv
