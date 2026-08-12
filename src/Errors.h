#pragma once

#include <cstdint>

namespace bv {

// Win32 error codes that indicate a network share or attached device disappeared
// mid-operation (NAS/SMB upload / USB unplug). When one of these surfaces while
// scanning a tree, further enumeration is pointless: the storage is gone.
//
// The list is deliberately conservative so ACL denials are NOT treated as
// disconnections (SMB ACL violations come back as ERROR_ACCESS_DENIED, and a
// missing path inside the tree is just ERROR_FILE_NOT_FOUND / _PATH_NOT_FOUND).
//
// ERROR_OPERATION_ABORTED (995) is the single deliberately-ambiguous entry: a
// host can return it when a blocking file operation is aborted. Two distinct
// things yield it in this tool:
//   * genuine SMB/NAS unplug -> we WANT the scan to abort as a lost device;
//   * a user-requested cancel -> already impossible here, because cancellation
//     is signalled through the entry callback (onEntry returns false and the
//     enumerator stops BEFORE any further blocking I/O), never by relying on
//     some other thread aborting a pending read. So a 995 observed by the
//     enumerator is never the tool's own cancel.
// Keep 995: dropping it would regress NAS-unplug detection to a flood of
// identical "Unable to enumerate" errors per directory.
inline bool IsDeviceDisconnectError(uint32_t winError) {
    switch (winError) {
        case 59:   // ERROR_UNEXP_NET_ERR
        case 64:   // ERROR_NETNAME_DELETED
        case 67:   // ERROR_BAD_NET_NAME ("network name cannot be found")
        case 995:  // ERROR_OPERATION_ABORTED (see note above)
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