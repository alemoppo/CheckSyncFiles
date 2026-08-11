#pragma once

#include <cstdint>
#include <string>

namespace bv {

// Broad classification of the IO workload, used to pick a sensible default
// thread count for the hash/checksum phase.
enum class IoClass : uint8_t {
    LocalLocal,      // both roots are local disks/volumes
    LocalNetwork,    // one root is an SMB/UNC share, the other local
    NetworkNetwork,  // both roots are network shares
};

inline bool IsUncPath(const std::wstring& p) {
    // UNC: "\\server\share" or "//server/share"; also "\\?\UNC\...".
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) return true;
    return !p.empty() && (p[0] == L'\\' || p[0] == L'/');
}

inline IoClass ClassifyIoClass(const std::wstring& a, const std::wstring& b) {
    const bool na = IsUncPath(a);
    const bool nb = IsUncPath(b);
    if (na && nb) return IoClass::NetworkNetwork;
    if (na || nb) return IoClass::LocalNetwork;
    return IoClass::LocalLocal;
}

} // namespace bv
