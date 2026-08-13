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
        onProgress,
        cancel);

    result.ok = ok;
    result.stats = stats_;
    return result;
}

void FileIndex::addEntry(FileEntry&& e) {
    // Two entries may fold to the same key (e.g. "Foo.txt" and "foo.TXT" when
    // the index is case-insensitive and a case-tolerant enumerator reports
    // both). The semantics are defined as LAST-WINS: the newer entry replaces
    // the older one, and the index always holds exactly one record per key.
    // Stats reflect only the kept record, so files == size() stays true.
    const std::wstring k = key(e.relativePath);
    const bool isDir = e.isDirectory;
    const uint64_t sz = e.size;
    auto it = map_.find(k);
    if (it == map_.end()) {
        map_.emplace(k, std::move(e));
    } else {
        if (it->second.isDirectory) {
            --stats_.dirs;
        } else {
            --stats_.files;
            stats_.bytes -= it->second.size;
        }
        it->second = std::move(e); // replace, do not re-insert (keeps node stable)
    }
    if (isDir) {
        ++stats_.dirs;
    } else {
        ++stats_.files;
        stats_.bytes += sz;
    }
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
