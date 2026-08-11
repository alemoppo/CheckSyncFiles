#include "FileIndex.h"

#include "PathUtil.h"

namespace bv {

std::wstring FileIndex::key(const std::wstring& relativePath) const {
    return caseSensitive_ ? relativePath : pathutil::FoldForCompare(relativePath);
}

FileIndex::BuildResult FileIndex::build(const std::wstring& root, IFileEnumerator& enumerator,
                                        const IFileEnumerator::ProgressCallback& onProgress,
                                        const std::atomic_bool* cancel) {
    BuildResult result;
    map_.clear();
    hashes_.clear();
    stats_ = {};

    const bool ok = enumerator.enumerate(
        root,
        [this, cancel](FileEntry&& e) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                return false; // stop the enumeration early
            }
            addEntry(std::move(e));
            return true;
        },
        [&result](const ScanError& err) { result.errors.push_back(err); },
        onProgress);

    result.ok = ok;
    result.stats = stats_;
    return result;
}

void FileIndex::addEntry(FileEntry&& e) {
    stats_.files += e.isDirectory ? 0 : 1;
    stats_.dirs += e.isDirectory ? 1 : 0;
    stats_.bytes += e.isDirectory ? 0 : e.size;
    // Compute the key BEFORE moving e (argument evaluation order is unspecified,
    // and std::move would leave e in a moved-from state).
    const std::wstring k = key(e.relativePath);
    map_.emplace(k, std::move(e));
}

void FileIndex::setHash(const std::wstring& relativePath,
                        const std::array<uint8_t, 32>& digest) {
    hashes_[key(relativePath)] = digest;
}

bool FileIndex::getHash(const std::wstring& relativePath,
                        std::array<uint8_t, 32>& digest) const {
    const auto it = hashes_.find(key(relativePath));
    if (it == hashes_.end()) return false;
    digest = it->second;
    return true;
}

bool FileIndex::find(const std::wstring& relativePath, FileEntry& out) const {
    const auto it = map_.find(key(relativePath));
    if (it == map_.end()) return false;
    out = it->second;
    return true;
}

bool FileIndex::tryErase(const std::wstring& relativePath, FileEntry& out) {
    const auto it = map_.find(key(relativePath));
    if (it == map_.end()) return false;
    out = std::move(it->second);
    map_.erase(it);
    return true;
}

} // namespace bv
