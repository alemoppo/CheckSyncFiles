#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace bv {
namespace hashing {

// Persistent SHA-256 cache keyed by (absolute path, size, last-write time):
// when a file is unchanged the digest is reused and the file is not re-read.
// This is a safety/performance knob only: it is enabled via an explicit flag
// and never changes a verdict, it only shortens a re-verification of a tree
// that has not changed.
//
// Threading contract: Lookup() is const and safe to call from the hash worker
// threads; Store() must only be called from the single thread that drains a
// batch of hash results (the workers never mutate the map).
class HashCache {
public:
    // Loads `filePath` if present and valid. A missing file is fine (empty
    // cache); a corrupt file is reported through `error` and ignored so a
    // broken cache never blocks a scan.
    explicit HashCache(const std::wstring& filePath, std::wstring& error);

    bool Lookup(const std::wstring& absPath, uint64_t size, uint64_t mtime,
                std::array<uint8_t, 32>& digest) const;
    void Store(const std::wstring& absPath, uint64_t size, uint64_t mtime,
               const std::array<uint8_t, 32>& digest);

    // Writes the whole cache back. Returns false and fills `error` on I/O error.
    bool Save(std::wstring& error) const;

    size_t size() const { return map_.size(); }

    // Combined key (path \x01 size \x01 mtime). The separator is a control
    // character, which NTFS forbids inside file names, so it cannot collide.
    static std::string MakeKey(const std::wstring& absPath, uint64_t size, uint64_t mtime);

private:
    std::string filePath_; // UTF-8
    std::unordered_map<std::string, std::array<uint8_t, 32>> map_;
};

} // namespace hashing
} // namespace bv