#pragma once

#include <array>
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
    using HashMap = std::unordered_map<std::wstring, std::array<uint8_t, 32>>;

    explicit FileIndex(bool caseSensitive = false) : caseSensitive_(caseSensitive) {}

    // Enumerates `root` and inserts every entry. Errors are collected and the
    // build continues; `ok` is false only if the root itself was unusable.
    // `onProgress` (optional) is forwarded to the enumerator. `cancel` (optional)
    // short-circuits the build when *cancel becomes true.
    BuildResult build(const std::wstring& root, IFileEnumerator& enumerator,
                      const IFileEnumerator::ProgressCallback& onProgress = {},
                      const std::atomic_bool* cancel = nullptr);

    // Inserts a single entry keeping stats_ consistent. Used by the snapshot
    // loader to rebuild an index previously serialized to disk.
    void addEntry(FileEntry&& e);

    // Looks up a relative path. Returns false if not present.
    bool find(const std::wstring& relativePath, FileEntry& out) const;

    // Looks up a relative path, removes the entry and returns it.
    // Used by the comparator to mark matched entries as it streams the
    // destination tree, so that remaining entries are exactly the missing ones.
    bool tryErase(const std::wstring& relativePath, FileEntry& out);

    // Optional per-entry SHA-256 of the source file. Filled when a snapshot is
    // captured in Content mode, and consumed by offline comparisons (the source
    // device is absent, so its digest must come from the snapshot).
    void setHash(const std::wstring& relativePath, const std::array<uint8_t, 32>& digest);
    bool getHash(const std::wstring& relativePath, std::array<uint8_t, 32>& digest) const;
    const HashMap& hashes() const { return hashes_; }
    size_t hashCount() const { return hashes_.size(); }

    size_t size() const { return map_.size(); }
    bool empty() const { return map_.empty(); }
    bool isCaseSensitive() const { return caseSensitive_; }

    // Number of addEntry() calls that hit an already-present folded key and were
    // resolved by the documented last-wins policy. A non-zero count means two
    // distinct paths (e.g. "Foo.txt" and "foo.TXT") collapse to one record; the
    // caller should surface a warning so the user knows a name was dropped.
    size_t collisionCount() const { return collisionCount_; }

    const Map& entries() const { return map_; }
    const BuildStats& stats() const { return stats_; }

private:
    std::wstring key(const std::wstring& relativePath) const;

    Map map_;
    HashMap hashes_;
    BuildStats stats_;
    bool caseSensitive_;
    size_t collisionCount_ = 0;
};

} // namespace bv
