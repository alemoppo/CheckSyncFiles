#include "FileIndexSerializer.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <string>

#include "PathUtil.h"

namespace bv {
namespace indexio {

namespace {

constexpr uint32_t kMagic = 0x49535642;      // "BVSI"
constexpr uint32_t kVersion = 1;
constexpr uint64_t kMaxPathLen = 1ull << 24;  // sanity bound on corrupt input
constexpr uint64_t kMaxEntries = 1ull << 31;

struct Writer {
    std::ofstream out;

    explicit Writer(const std::wstring& path) : out(pathutil::AddLongPathPrefix(path).c_str(),
                                                   std::ios::binary | std::ios::out |
                                                       std::ios::trunc) {}

    bool PutU8(uint8_t v) {
        out.write(reinterpret_cast<const char*>(&v), 1);
        return out.good();
    }
    bool PutU32(uint32_t v) {
        out.write(reinterpret_cast<const char*>(&v), 4);
        return out.good();
    }
    bool PutU64(uint64_t v) {
        out.write(reinterpret_cast<const char*>(&v), 8);
        return out.good();
    }
    bool PutStr(const std::string& s) {
        return PutU64(s.size()) && out.write(s.data(), static_cast<std::streamsize>(s.size())) &&
               out.good();
    }
    bool PutBytes(const void* p, size_t n) {
        out.write(static_cast<const char*>(p), static_cast<std::streamsize>(n));
        return out.good();
    }
};

struct Reader {
    std::ifstream in;
    bool valid = true;

    explicit Reader(const std::wstring& path) : in(pathutil::AddLongPathPrefix(path).c_str(),
                                                    std::ios::binary | std::ios::in) {}

    uint8_t GetU8() {
        uint8_t v = 0;
        in.read(reinterpret_cast<char*>(&v), 1);
        if (!in) valid = false;
        return v;
    }
    uint32_t GetU32() {
        uint32_t v = 0;
        in.read(reinterpret_cast<char*>(&v), 4);
        if (!in) valid = false;
        return v;
    }
    uint64_t GetU64() {
        uint64_t v = 0;
        in.read(reinterpret_cast<char*>(&v), 8);
        if (!in) valid = false;
        return v;
    }
    bool GetStr(std::string& out) {
        const uint64_t n = GetU64();
        if (!valid || n > kMaxPathLen) {
            valid = false;
            return false;
        }
        out.resize(static_cast<size_t>(n));
        in.read(out.data(), static_cast<std::streamsize>(n));
        if (!in) valid = false;
        return valid;
    }
    bool GetBytes(void* p, size_t n) {
        in.read(static_cast<char*>(p), static_cast<std::streamsize>(n));
        if (!in) valid = false;
        return valid;
    }
};

} // namespace

bool WriteSnapshot(const std::wstring& filePath, const FileIndex& index,
                   const std::wstring& sourceRoot, std::wstring& error) {
    Writer w(filePath);
    if (!w.out) {
        error = L"impossibile creare lo snapshot: " + filePath;
        return false;
    }

    const std::string rootU8 = pathutil::ToUtf8(sourceRoot);
    const bool ok = w.PutU32(kMagic) && w.PutU32(kVersion) &&
                    w.PutU8(index.isCaseSensitive() ? 1 : 0) && w.PutStr(rootU8) &&
                    w.PutU64(index.stats().files) && w.PutU64(index.stats().dirs) &&
                    w.PutU64(index.stats().bytes) && w.PutU64(index.size());
    if (!ok) {
        error = L"errore di scrittura dello snapshot (header): " + filePath;
        return false;
    }

    for (const auto& kv : index.entries()) {
        const FileEntry& e = kv.second;
        const std::string u8 = pathutil::ToUtf8(e.relativePath);
        std::array<uint8_t, 32> digest{};
        const bool hasHash = index.getHash(e.relativePath, digest);
        bool entryOk = w.PutStr(u8) && w.PutU64(e.size) && w.PutU64(e.lastWriteTime) &&
                       w.PutU32(e.attributes) && w.PutU64(e.fileId) &&
                       w.PutU8(e.isDirectory ? 1 : 0) && w.PutU8(hasHash ? 1 : 0);
        if (hasHash) entryOk = entryOk && w.PutBytes(digest.data(), digest.size());
        if (!entryOk) {
            error = L"errore di scrittura dello snapshot (entry): " + filePath;
            return false;
        }
    }

    w.out.flush();
    if (!w.out) {
        error = L"errore di scrittura dello snapshot: " + filePath;
        return false;
    }
    error.clear();
    return true;
}

bool ReadSnapshot(const std::wstring& filePath, FileIndex& index,
                  std::wstring& sourceRootOut, std::wstring& error) {
    Reader r(filePath);
    if (!r.in) {
        error = L"impossibile aprire lo snapshot: " + filePath;
        return false;
    }

    if (r.GetU32() != kMagic || r.GetU32() != kVersion) {
        error = L"file non riconosciuto come snapshot BackupVerifier: " + filePath;
        return false;
    }
    const bool caseSensitive = r.GetU8() != 0;
    {
        std::string rootU8;
        if (!r.GetStr(rootU8)) {
            error = L"snapshot corrotto (radice sorgente): " + filePath;
            return false;
        }
        sourceRootOut = pathutil::FromUtf8(rootU8);
    }
    (void)r.GetU64(); // serialized stats are informational; addEntry() recomputes them
    (void)r.GetU64();
    (void)r.GetU64();
    const uint64_t count = r.GetU64();
    if (!r.valid || count > kMaxEntries) {
        error = L"snapshot corrotto (statistiche): " + filePath;
        return false;
    }

    FileIndex loaded(caseSensitive);
    for (uint64_t i = 0; i < count; ++i) {
        std::string pathU8;
        if (!r.GetStr(pathU8)) {
            error = L"snapshot corrotto (percorso): " + filePath;
            return false;
        }
        FileEntry e;
        e.relativePath = pathutil::FromUtf8(pathU8);
        e.size = r.GetU64();
        e.lastWriteTime = r.GetU64();
        e.attributes = r.GetU32();
        e.fileId = r.GetU64();
        e.isDirectory = r.GetU8() != 0;
        const bool hasHash = r.GetU8() != 0;
        if (!r.valid) {
            error = L"snapshot corrotto (entry): " + filePath;
            return false;
        }
        loaded.addEntry(std::move(e));
        if (hasHash) {
            std::array<uint8_t, 32> digest{};
            if (!r.GetBytes(digest.data(), digest.size())) {
                error = L"snapshot corrotto (hash): " + filePath;
                return false;
            }
            loaded.setHash(pathutil::FromUtf8(pathU8), digest);
        }
    }
    if (!r.valid) {
        error = L"snapshot non terminato correttamente: " + filePath;
        return false;
    }

    index = std::move(loaded);
    error.clear();
    return true;
}

} // namespace indexio
} // namespace bv