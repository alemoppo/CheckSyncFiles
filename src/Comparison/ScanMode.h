#pragma once

namespace bv {

enum class ScanMode {
    Presence, // only check that every source path exists in destination
    Size,     // presence + file size
    Content,  // presence + size + SHA-256 content hash (Phase 3)
};

// Sampling pattern for partial content verification (see ContentVerifyLevel).
// Random is NEVER resolved per file or per side: ScanController::run resolves
// it once per run to Edges or Center, and only the resolved value travels
// downstream (FileResult::verifiedPattern is therefore never Random).
enum class PartialPattern : uint8_t {
    Edges,  // head block + tail block (odd block goes to the head)
    Center, // single block centred on MiB-aligned 1 MiB blocks
    Random, // resolved once per run to Edges or Center (never used literally)
};

// How much of each file Content mode reads for verification. Travels next to
// ScanMode::Content (ScanOptions, CLI, GUI state) with a default that
// reproduces today's behaviour exactly: percent 100 reads every byte like the
// current full Content mode, and pattern is ignored in that case.
struct ContentVerifyLevel {
    int percent = 100; // 1..100 (% of file bytes sampled); 100 == full read
    PartialPattern pattern = PartialPattern::Edges;
};

} // namespace bv
