#include "Hashing/Sha256.h"

#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include "Filesystem/PathUtil.h"

namespace bv {
namespace hashing {

namespace {

constexpr DWORD kiChunk = 1024 * 1024; // 1 MiB streaming buffer

} // namespace

HashStatus Sha256File(const std::wstring& path, std::array<uint8_t, 32>& digest) {
    const std::wstring win = pathutil::AddLongPathPrefix(path);
    HANDLE h = CreateFileW(win.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_ACCESS_DENIED ? HashStatus::NoAccess
                                                     : HashStatus::ReadError;
    }

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
                                                    nullptr, 0))) {
        CloseHandle(h);
        return HashStatus::ReadError;
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        BCryptCloseAlgorithmProvider(alg, 0);
        CloseHandle(h);
        return HashStatus::ReadError;
    }

    std::vector<uint8_t> buf(kiChunk);
    bool complete = true;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr)) {
            complete = false;
            break;
        }
        if (read == 0) break;
        if (!BCRYPT_SUCCESS(BCryptHashData(hash, buf.data(), read, 0))) {
            complete = false;
            break;
        }
    }

    HashStatus result;
    if (complete &&
        BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(),
                                        static_cast<ULONG>(digest.size()), 0))) {
        result = HashStatus::Ok;
    } else {
        result = HashStatus::ReadError;
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(h);
    return result;
}

bool StatFile(const std::wstring& path, uint64_t& size, uint64_t& lastWriteTime) {
    const std::wstring win = pathutil::AddLongPathPrefix(path);
    HANDLE h = CreateFileW(win.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    BY_HANDLE_FILE_INFORMATION info{};
    bool ok = GetFileInformationByHandle(h, &info);
    if (ok) {
        size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        lastWriteTime = (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
                        info.ftLastWriteTime.dwLowDateTime;
    }
    CloseHandle(h);
    return ok;
}

} // namespace hashing
} // namespace bv