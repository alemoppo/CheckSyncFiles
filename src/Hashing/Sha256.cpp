#include "Hashing/Sha256.h"

#include <vector>

#include <bcrypt.h>

#include "Filesystem/PathUtil.h"
#include "Profiling/HashProfile.h"

namespace bv {
namespace hashing {

namespace {

constexpr DWORD kiChunk = 1024 * 1024; // 1 MiB streaming buffer

// Path-opening wrapper used by the two public Sha256File overloads: open the
// file, hash the handle, close it, and report `totalTicks` over the WHOLE
// Sha256File() span (open -> close), matching the pre-refactor semantics. The
// actual ReadFile / BCryptHashData loop lives in Sha256FileFromHandle below and
// is shared with HashOneSide's unified single-handle flow.
template <bool Profile>
HashStatus Sha256FileImpl(const std::wstring& path, std::array<uint8_t, 32>& digest,
                          profiling::FileTimings* timings) {
    const uint64_t t0 = profiling::QpcNow();
    const std::wstring win = pathutil::AddLongPathPrefix(path);
    HANDLE h = CreateFileW(win.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if constexpr (Profile) {
            if (timings) {
                timings->readTicks = 0;
                timings->hashTicks = 0;
                timings->bytesRead = 0;
                timings->totalTicks = profiling::QpcNow() - t0;
            }
        }
        return GetLastError() == ERROR_ACCESS_DENIED ? HashStatus::NoAccess
                                                     : HashStatus::ReadError;
    }

    const HashStatus result = Sha256FileFromHandle<Profile>(h, digest, timings);
    CloseHandle(h);
    if constexpr (Profile) {
        if (timings) timings->totalTicks = profiling::QpcNow() - t0;
    }
    return result;
}

} // namespace

// Read + hash of an ALREADY-OPEN handle. It NEVER closes `h` (the caller owns
// and closes the handle), so HashOneSide can reuse it after T1 and re-stat it
// at T2 without extra opens. `Profile` selects at compile time whether the QPC
// instrumentation is compiled in: the non-profiling instantiation is identical
// to the original loop with zero timing overhead, while the profiling
// instantiation records read/hash/total time and the bytes read around the
// existing ReadFile / BCryptHashData calls WITHOUT changing the loop structure
// or behaviour.
template <bool Profile>
HashStatus Sha256FileFromHandle(HANDLE h, std::array<uint8_t, 32>& digest,
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

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
                                                    nullptr, 0))) {
        finalize(0, 0, 0);
        return HashStatus::ReadError;
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        BCryptCloseAlgorithmProvider(alg, 0);
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
    finalize(readT, hashT, bytes);
    return result;
}

// Explicit instantiations declared in the header: make both compile-time
// variants linkable from HashUtil.cpp without putting the BCrypt/Windows types
// in the public header.
template HashStatus Sha256FileFromHandle<false>(HANDLE, std::array<uint8_t, 32>&,
                                                profiling::FileTimings*);
template HashStatus Sha256FileFromHandle<true>(HANDLE, std::array<uint8_t, 32>&,
                                               profiling::FileTimings*);

HashStatus Sha256File(const std::wstring& path, std::array<uint8_t, 32>& digest) {
    return Sha256FileImpl<false>(path, digest, nullptr);
}

HashStatus Sha256File(const std::wstring& path, std::array<uint8_t, 32>& digest,
                      profiling::FileTimings* timings) {
    return Sha256FileImpl<true>(path, digest, timings);
}

bool StatHandle(HANDLE h, uint64_t& size, uint64_t& lastWriteTime) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info)) return false;
    size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    lastWriteTime = (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
                    info.ftLastWriteTime.dwLowDateTime;
    return true;
}

bool StatFile(const std::wstring& path, uint64_t& size, uint64_t& lastWriteTime) {
    const std::wstring win = pathutil::AddLongPathPrefix(path);
    HANDLE h = CreateFileW(win.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = StatHandle(h, size, lastWriteTime);
    CloseHandle(h);
    return ok;
}

} // namespace hashing
} // namespace bv