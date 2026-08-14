#include "Hashing/HashUtil.h"

namespace bv {
namespace hashing {

void HashOneSide(const std::wstring& absPath, uint64_t expectedSize, uint64_t expectedMtime,
                 bool& changed, HashStatus& status, Digest& digest, bool valid,
                 HashCache* cache, std::atomic<size_t>& cacheHits) {
    if (!valid) {
        status = HashStatus::ReadError;
        return;
    }
    uint64_t sz = 0;
    uint64_t mt = 0;
    if (!StatFile(absPath, sz, mt)) {
        status = HashStatus::NoAccess;
        return;
    }
    changed = (sz != expectedSize) || (mt != expectedMtime);

    if (cache && cache->Lookup(absPath, sz, mt, digest)) {
        status = HashStatus::Ok;
        cacheHits.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    status = Sha256File(absPath, digest);
    if (status != HashStatus::Ok) return;

    uint64_t szAfter = 0;
    uint64_t mtAfter = 0;
    if (StatFile(absPath, szAfter, mtAfter)) {
        if (szAfter != sz || mtAfter != mt) changed = true;
    }
    if (cache) cache->Store(absPath, sz, mt, digest);
}

} // namespace hashing
} // namespace bv