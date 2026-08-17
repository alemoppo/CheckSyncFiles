#include "Hashing/HashUtil.h"

namespace bv {
namespace hashing {

void HashOneSide(const std::wstring& absPath, uint64_t expectedSize, uint64_t expectedMtime,
                 bool& changed, HashStatus& status, Digest& digest, bool valid,
                 HashCache* cache, std::atomic<size_t>& cacheHits,
                 profiling::HashSession* session, profiling::Side side) {
    if (!valid) {
        status = HashStatus::ReadError;
        return;
    }
    const bool prof = session && session->prof && session->prof->enabled();

    uint64_t sz = 0;
    uint64_t mt = 0;
    const uint64_t t1Start = prof ? profiling::QpcNow() : 0;
    if (!StatFile(absPath, sz, mt)) {
        if (prof) session->prof->NoteStatBefore(side, profiling::QpcNow() - t1Start);
        status = HashStatus::NoAccess;
        return;
    }
    if (prof) session->prof->NoteStatBefore(side, profiling::QpcNow() - t1Start);
    changed = (sz != expectedSize) || (mt != expectedMtime);

    if (cache && cache->Lookup(absPath, sz, mt, digest)) {
        status = HashStatus::Ok;
        cacheHits.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    profiling::FileTimings ft;
    if (prof) session->prof->FileBegin(*session, side, absPath, expectedSize);
    status = Sha256File(absPath, digest, prof ? &ft : nullptr);
    if (prof) session->prof->FileEnd(*session, side, absPath, expectedSize, ft,
                                     status == HashStatus::Ok);
    if (status != HashStatus::Ok) return;

    uint64_t szAfter = 0;
    uint64_t mtAfter = 0;
    const uint64_t t2Start = prof ? profiling::QpcNow() : 0;
    if (StatFile(absPath, szAfter, mtAfter)) {
        if (prof) session->prof->NoteStatAfter(side, profiling::QpcNow() - t2Start);
        if (szAfter != sz || mtAfter != mt) changed = true;
    } else if (prof) {
        session->prof->NoteStatAfter(side, profiling::QpcNow() - t2Start);
    }
    if (cache) cache->Store(absPath, sz, mt, digest);
}

} // namespace hashing
} // namespace bv