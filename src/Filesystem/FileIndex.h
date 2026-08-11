#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "FileEnumerator.h"

namespace bv {

// In-memory index of an enumerated tree, keyed by relative path.
//
// Key normalization follows the configured case policy:
//   - case-insensitive (default, Windows/SMB): keys are upper-cased with an
//     invariant locale, mirroring NTFS semantics
//   - case-sensitive: keys are the original relative paths
//
// The map key is the folded path; FileEntry always keeps the original-case
// relative path for display and export.
//
// Memory note: for very large trees the folded key + original path double the
// string storage. A more compact layout (arena / sorted vector) is a planned
// optimization; the current map is chosen for correctness first.
class FileIndex {
public:
    struct BuildStats {
        uint64_t files = 0;
        uint64_t dirs = 0;
        uint64_t bytes = 0; // sum of file sizes
    };

    struct BuildResult {
        bool ok = false;
        BuildStats stats;
        std::vector<ScanError> errors;
    };

    using Map = std::unordered_map<std::wstring, FileEntry>;

    explicit FileIndex(bool caseSensitive = false) : caseSensitive_(caseSensitive) {}

    // Enumerates `root` and inserts every entry. Errors are collected and the
    // build continues; `ok` is false only if the root itself was unusable.
    // `onProgress` (optional) is forwarded to the enumerator. `cancel` (optional)
    // short-circuits the build when *cancel becomes true.
    BuildResult build(const std::wstring& root, IFileEnumerator& enumerator,
                      const IFileEnumerator::ProgressCallback& onProgress = {},
                      const std::atomic_bool* cancel = nullptr);

    // Looks up a relative path. Returns false if not present.
    bool find(const std::wstring& relativePath, FileEntry& out) const;

    // Looks up a relative path, removes the entry and returns it.
    // Used by the comparator to mark matched entries as it streams the
    // destination tree, so that remaining entries are exactly the missing ones.
    bool tryErase(const std::wstring& relativePath, FileEntry& out);

    size_t size() const { return map_.size(); }
    bool empty() const { return map_.empty(); }

    const Map& entries() const { return map_; }
    const BuildStats& stats() const { return stats_; }

private:
    std::wstring key(const std::wstring& relativePath) const;

    Map map_;
    BuildStats stats_;
    bool caseSensitive_;
};

} // namespace bv
