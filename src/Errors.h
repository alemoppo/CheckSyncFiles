#pragma once

#include <cstdint>

namespace bv {

// Win32 error codes that indicate a network share or attached device disappeared
// mid-operation (NAS/SMB upload / USB unplug). When one of these surfaces while
// scanning a tree, further enumeration is pointless: the storage is gone.
// The list is deliberately conservative so ACL denials are NOT treated as
// disconnections (SMB ACL violations come back as ERROR_ACCESS_DENIED).
inline bool IsDeviceDisconnectError(uint32_t winError) {
    switch (winError) {
        case 59:   // ERROR_UNEXP_NET_ERR
        case 64:   // ERROR_NETNAME_DELETED
        case 67:   // ERROR_BAD_NET_NAME ("network name cannot be found")
        case 995:  // ERROR_OPERATION_ABORTED
        case 1167: // ERROR_DEVICE_NOT_CONNECTED
        case 1222: // ERROR_NO_NETWORK
        case 1231: // ERROR_NETWORK_UNREACHABLE
        case 1236: // ERROR_CONNECTION_ABORTED
            return true;
        default:
            return false;
    }
}

} // namespace bv