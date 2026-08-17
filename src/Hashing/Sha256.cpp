#include "Hashing/Sha256.h"

#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include "Filesystem/PathUtil.h"
#include "Profiling/HashProfile.h"

namespace bv {
namespace hashing {

namespace {

constexpr DWORD kiChunk = 1024 * 1024; // 1 MiB streaming buffer

// Shared implementation. `Profile` selects at compile time whether the QPC
// instrumentation is compiled in: the non-profiling instantiation (the common
// path, used by the two-argument Sha256File) is identical to the original loop
// with zero timing overhead, while the profiling instantiation records
// read/hash/total time and the bytes read around the existing ReadFile /
// BCryptHashData calls WITHOUT changing the loop structure or behaviour.
template <bool Profile>
HashStatus Sha256FileImpl(const std::wstring& path, std::array<uint8_t, 32>& digest,
                          profiling::FileTimings* timings) {
    const uint64_t t0 = profiling::QpcNow();
    const auto finalize = [&](uint64_t readT, uint64_t hashT, uint64_t bytes) {
        if constexpr (Profile) {
            if (timings) {
                timings->readTicks = readT;
                timings->hashTicks = hashT;
                timings->bytesRead = bytes;
                timings->totalTicks = profiling::QpcNow() - t0;
            }
        }
    };

    const std::wstring win = pathutil::AddLongPathPrefix(path);
    HANDLE h = CreateFileW(win.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        finalize(0, 0, 0);
        return GetLastError() == ERROR_ACCESS_DENIED ? HashStatus::NoAccess
                                                     : HashStatus::ReadError;
    }

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
                                                    nullptr, 0))) {
        CloseHandle(h);
        finalize(0, 0, 0);
        return HashStatus::ReadError;
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        BCryptCloseAlgorithmProvider(alg, 0);
        CloseHandle(h);
        finalize(0, 0, 0);
        return HashStatus::ReadError;
    }

    std::vector<uint8_t> buf(kiChunk);
    bool complete = true;
    uint64_t readT = 0;
    uint64_t hashT = 0;
    uint64_t bytes = 0;
    for (;;) {
        uint64_t tr0 = 0;
        if constexpr (Profile) tr0 = profiling::QpcNow();
        DWORD read = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr)) {
            complete = false;
            if constexpr (Profile) readT += profiling::QpcNow() - tr0;
            break;
        }
        if constexpr (Profile) {
            readT += profiling::QpcNow() - tr0;
            bytes += read;
        }
        if (read == 0) break;
        uint64_t th0 = 0;
        if constexpr (Profile) th0 = profiling::QpcNow();
        if (!BCRYPT_SUCCESS(BCryptHashData(hash, buf.data(), read, 0))) {
            complete = false;
            if constexpr (Profile) hashT += profiling::QpcNow() - th0;
            break;
        }
        if constexpr (Profile) hashT += profiling::QpcNow() - th0;
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
    finalize(readT, hashT, bytes);
    return result;
}

} // namespace

HashStatus Sha256File(const std::wstring& path, std::array<uint8_t, 32>& digest) {
    return Sha256FileImpl<false>(path, digest, nullptr);
}

HashStatus Sha256File(const std::wstring& path, std::array<uint8_t, 32>& digest,
                      profiling::FileTimings* timings) {
    return Sha256FileImpl<true>(path, digest, timings);
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
