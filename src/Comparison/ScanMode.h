#pragma once

namespace bv {

enum class ScanMode {
    Presence, // only check that every source path exists in destination
    Size,     // presence + file size
    Content,  // presence + size + SHA-256 content hash (Phase 3)
};

} // namespace bv
