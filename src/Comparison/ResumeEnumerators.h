#pragma once

// Enumerators for the resume path (Phase 1).
//
// Resume re-verifies only the remainder set: paths that are new, changed, or
// were never finalized. Both wrappers below keep the traversal COMPLETE and
// filter only the EMISSION, so errors surface exactly as in a fresh scan:
//
// - IndexEnumerator feeds an already-built FileIndex (the remainder side)
//   entry by entry. Used as the comparer source factory; the index is owned
//   by the caller and must outlive the run.
// - FilteredEnumerator wraps a live back-end (Win32/MFT) and emits only the
//   entries whose folded key is in the remainder set. Directories outside the
//   set are still descended into (never pruned); errors pass through
//   unfiltered so the comparer rediscovers them itself.
//
// Progress counts reflect EMITTED entries (not traversed ones), so progress
// and per-side stats stay consistent with what the comparer actually sees.

#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

#include "Filesystem/FileEnumerator.h"
#include "Filesystem/FileIndex.h"
#include "Filesystem/PathUtil.h"

namespace bv {

// Feeds every entry of `index` through the enumerator protocol.
class IndexEnumerator : public IFileEnumerator {
public:
    explicit IndexEnumerator(const FileIndex* index) : index_(index) {}

    bool enumerate(const std::wstring& /*root*/, const EntryCallback& onEntry,
                   const ErrorCallback& /*onError*/,
                   const ProgressCallback& onProgress = {},
                   const std::atomic_bool* cancel = nullptr) override {
        uint64_t files = 0, dirs = 0, bytes = 0;
        for (const auto& kv : index_->entries()) {
            if (cancel && cancel->load(std::memory_order_relaxed)) break;
            const FileEntry& e = kv.second;
            if (e.isDirectory) {
                ++dirs;
            } else {
                ++files;
                bytes += e.size;
            }
            FileEntry copy = e;
            if (!onEntry(std::move(copy))) break;
            if (onProgress) onProgress(files, dirs, bytes, e.relativePath);
        }
        return true; // the index is already built: nothing can fail here
    }

private:
    const FileIndex* index_;
};

// Emits only allow-listed entries from a live back-end; traversal, errors,
// cancellation and progress plumbing are otherwise transparent.
class FilteredEnumerator : public IFileEnumerator {
public:
    // `allowedFolded` (folded relative-path keys) is referenced, not copied:
    // it must outlive the run, like the wrapped enumerator's roots.
    FilteredEnumerator(std::unique_ptr<IFileEnumerator> inner,
                       const std::unordered_set<std::wstring>& allowedFolded,
                       bool caseSensitive)
        : inner_(std::move(inner)), allowed_(allowedFolded), caseSensitive_(caseSensitive) {}

    void setDirListSink(profiling::DirListSink* sink,
                        const std::wstring& relPrefix = std::wstring()) override {
        inner_->setDirListSink(sink, relPrefix);
    }

    bool enumerate(const std::wstring& root, const EntryCallback& onEntry,
                   const ErrorCallback& onError,
                   const ProgressCallback& onProgress = {},
                   const std::atomic_bool* cancel = nullptr) override {
        uint64_t files = 0, dirs = 0, bytes = 0;
        return inner_->enumerate(
            root,
            [&](FileEntry&& e) -> bool {
                const std::wstring key = caseSensitive_
                                             ? e.relativePath
                                             : pathutil::FoldForCompare(e.relativePath);
                if (allowed_.find(key) == allowed_.end()) return true; // skip: keep walking
                if (e.isDirectory) {
                    ++dirs;
                } else {
                    ++files;
                    bytes += e.size;
                }
                const std::wstring path = e.relativePath;
                if (!onEntry(std::move(e))) return false;
                if (onProgress) onProgress(files, dirs, bytes, path);
                return true;
            },
            onError,
            // Inner progress (full-traversal counts) would overstate the
            // filtered stream: report our own emitted counts instead, keeping
            // the live path for continuity.
            [&](uint64_t /*f*/, uint64_t /*d*/, uint64_t /*b*/, const std::wstring& path) {
                if (onProgress) onProgress(files, dirs, bytes, path);
            },
            cancel);
    }

private:
    std::unique_ptr<IFileEnumerator> inner_;
    const std::unordered_set<std::wstring>& allowed_;
    bool caseSensitive_;
};

} // namespace bv
