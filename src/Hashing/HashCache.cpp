#include "Hashing/HashCache.h"

#include <cstdint>
#include <fstream>

#include "Filesystem/PathUtil.h"

namespace bv {
namespace hashing {

namespace {

constexpr uint32_t kMagic = 0x43485642; // "BVHC"
constexpr uint32_t kCacheVersion = 1;

// Sanity bound for a corrupt cache so a garbage length cannot cause a huge
// allocation (64 MiB worth of keys is already an enormous cache).
constexpr uint64_t kMaxKeyLen = 1ull << 26;
constexpr uint64_t kMaxEntries = 1ull << 30;

} // namespace

std::string HashCache::MakeKey(const std::wstring& absPath, uint64_t size, uint64_t mtime) {
    std::string k = pathutil::ToUtf8(absPath);
    k += '\x01';
    k += std::to_string(size);
    k += '\x01';
    k += std::to_string(mtime);
    return k;
}

HashCache::HashCache(const std::wstring& filePath, std::wstring& error) {
    filePath_ = pathutil::ToUtf8(filePath);

    std::ifstream in(pathutil::AddLongPathPrefix(filePath).c_str(), std::ios::binary);
    if (!in) {
        error.clear(); // missing cache is not an error
        return;
    }

    uint32_t magic = 0;
    uint32_t version = 0;
    in.read(reinterpret_cast<char*>(&magic), 4);
    in.read(reinterpret_cast<char*>(&version), 4);
    uint64_t count = 0;
    in.read(reinterpret_cast<char*>(&count), 8);
    if (!in || magic != kMagic || version != kCacheVersion || count > kMaxEntries) {
        error = L"cache hash non valida (verra ricreata): " + filePath;
        return;
    }

    bool corrupt = false;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t keyLen = 0;
        in.read(reinterpret_cast<char*>(&keyLen), 8);
        if (!in || keyLen > kMaxKeyLen) {
            corrupt = true;
            break;
        }
        std::string key(static_cast<size_t>(keyLen), '\0');
        in.read(key.data(), static_cast<std::streamsize>(keyLen));
        std::array<uint8_t, 32> digest{};
        in.read(reinterpret_cast<char*>(digest.data()), static_cast<std::streamsize>(digest.size()));
        if (!in) {
            corrupt = true;
            break;
        }
        map_[std::move(key)] = digest;
    }
    if (corrupt) {
        map_.clear();
        error = L"cache hash danneggiata (verra ricreata): " + filePath;
    } else {
        error.clear();
    }
}

bool HashCache::Lookup(const std::wstring& absPath, uint64_t size, uint64_t mtime,
                       std::array<uint8_t, 32>& digest) const {
    const auto it = map_.find(MakeKey(absPath, size, mtime));
    if (it == map_.end()) return false;
    digest = it->second;
    return true;
}

void HashCache::Store(const std::wstring& absPath, uint64_t size, uint64_t mtime,
                      const std::array<uint8_t, 32>& digest) {
    map_[MakeKey(absPath, size, mtime)] = digest;
}

bool HashCache::Save(std::wstring& error) const {
    std::ofstream out(pathutil::AddLongPathPrefix(pathutil::FromUtf8(filePath_)).c_str(),
                      std::ios::binary | std::ios::out | std::ios::trunc);
    if (!out) {
        error = L"impossibile scrivere la cache hash: " + pathutil::FromUtf8(filePath_);
        return false;
    }

    out.write(reinterpret_cast<const char*>(&kMagic), 4);
    out.write(reinterpret_cast<const char*>(&kCacheVersion), 4);
    const uint64_t count = map_.size();
    out.write(reinterpret_cast<const char*>(&count), 8);
    for (const auto& kv : map_) {
        const uint64_t keyLen = kv.first.size();
        out.write(reinterpret_cast<const char*>(&keyLen), 8);
        out.write(kv.first.data(), static_cast<std::streamsize>(keyLen));
        out.write(reinterpret_cast<const char*>(kv.second.data()),
                  static_cast<std::streamsize>(kv.second.size()));
        if (!out.good()) {
            error = L"errore di scrittura della cache hash";
            return false;
        }
    }
    out.flush();
    if (!out.good()) {
        error = L"errore di scrittura della cache hash";
        return false;
    }
    error.clear();
    return true;
}

} // namespace hashing
} // namespace bv