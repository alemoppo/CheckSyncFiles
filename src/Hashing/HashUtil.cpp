#include "Hashing/HashUtil.h"

#include "Filesystem/PathUtil.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace bv {
namespace hashing {

void HashOneSide(const std::wstring& absPath, uint64_t expectedSize, uint64_t expectedMtime,
                 bool& changed, HashStatus& status, Digest& digest, bool valid,
                 HashCache* cache, std::atomic<size_t>& cacheHits,
                 profiling::HashSession* session, profiling::Side side,
                 const std::atomic_bool* cancel) {
    if (!valid) {
        status = HashStatus::ReadError;
        return;
    }
    const bool prof = session && session->prof && session->prof->enabled();

    // Unified single-handle flow:
    //
    //   CreateFile
    //     -> GetFileInformationByHandle (T1)
    //     -> cache lookup (hit => return WITHOUT hashing AND without T2)
    //     -> ReadFile + BCryptHashData
    //     -> GetFileInformationByHandle (T2)
    //     -> CloseHandle
    //
    // replaces the previous StatFile/Sha256File/StatFile sequence (which opened
    // and closed the file three times). The handle keeps the hash handle's
    // FILE_SHARE_READ | FILE_SHARE_WRITE share mode with NO FILE_SHARE_DELETE,
    // so a concurrent delete is blocked for the whole T1..T2 window exactly as
    // it was during the old hashing open.
    const std::wstring win = pathutil::AddLongPathPrefix(absPath);
    HANDLE h = CreateFileW(win.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // Same verdict as the old T1 StatFile() failure: NoAccess.
        status = HashStatus::NoAccess;
        return;
    }

    // T1: metadata BEFORE hashing, read from the shared handle.
    uint64_t sz = 0;
    uint64_t mt = 0;
    const uint64_t t1Start = prof ? profiling::QpcNow() : 0;
    if (!StatHandle(h, sz, mt)) {
        if (prof) session->prof->NoteStatBefore(side, profiling::QpcNow() - t1Start);
        CloseHandle(h);
        status = HashStatus::NoAccess;
        return;
    }
    if (prof) session->prof->NoteStatBefore(side, profiling::QpcNow() - t1Start);
    changed = (sz != expectedSize) || (mt != expectedMtime);

    // Cache hit under the CURRENT (size, mtime) key: reuse the digest and
    // return without hashing and without T2 (identical to the previous flow).
    if (cache && cache->Lookup(absPath, sz, mt, digest)) {
        CloseHandle(h);
        status = HashStatus::Ok;
        cacheHits.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    profiling::FileTimings ft;
    if (prof) session->prof->FileBegin(*session, side, absPath, expectedSize);
    if (prof) {
        status = Sha256FileFromHandle<true>(h, digest, &ft, cancel);
    } else {
        status = Sha256FileFromHandle<false>(h, digest, nullptr, cancel);
    }
    if (prof) session->prof->FileEnd(*session, side, absPath, expectedSize, ft,
                                     status == HashStatus::Ok);
    if (status != HashStatus::Ok) {
        CloseHandle(h);
        return;
    }

    // T2: metadata AFTER hashing, read from the SAME handle.
    uint64_t szAfter = 0;
    uint64_t mtAfter = 0;
    const uint64_t t2Start = prof ? profiling::QpcNow() : 0;
    if (StatHandle(h, szAfter, mtAfter)) {
        if (prof) session->prof->NoteStatAfter(side, profiling::QpcNow() - t2Start);
        if (HashMetadataChanged(sz, mt, szAfter, mtAfter)) changed = true;
    } else if (prof) {
        session->prof->NoteStatAfter(side, profiling::QpcNow() - t2Start);
    }
    CloseHandle(h);
    if (cache) cache->Store(absPath, sz, mt, digest);
}

} // namespace hashing
} // namespace bv