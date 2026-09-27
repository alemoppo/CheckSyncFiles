#pragma once

// Persistent scan-session model (Phase 0).
//
// This header is ONLY the logical model: plain data describing a scan that can
// be interrupted and resumed later. It deliberately contains no runtime state
// (no threads, pools, handles, MatchTable, profilers, DirTiming): those are
// rebuilt from scratch by the future resume engine and must never enter the
// persisted format. See SessionStore.h for the BVSS v1 on-disk layout.
//
// JOURNAL FINALITY DISCIPLINE (read before journaling anything):
// The journal holds results that are determined and reusable, never
// provisional one-sided "sightings". Missing/Extra verdicts require a
// completed enumeration of the opposite side (the live comparer gates them
// until both sides succeed), so journaling a path seen on side A alone as
// "Missing" mid-enumeration would be wrong: side B may simply not have
// produced it yet. The future resume engine must therefore:
//   1. journal only finalized pair outcomes (identical / size / content /
//      partial / read-error rows whose verdict no longer depends on unseen
//      input), each carrying the exact (size, mtime[, digest]) version it was
//      computed from;
//   2. at resume, re-enumerate and combine journal + fresh enumeration before
//      deriving any Missing/Extra verdict.
// SessionStore itself is verdict-agnostic (it persists whatever entries it is
// given); enforcing the discipline above is the engine's job (Phase 1).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "Comparison/ScanMode.h" // before ComparisonResult.h (which needs PartialPattern)
#include "Comparison/ComparisonResult.h"

namespace bv {
namespace session {

// On-disk schema version of the BVSS session format. Bumped only by an
// explicit format change; SessionStore refuses to load newer versions.
inline constexpr uint32_t kSchemaVersion = 1;

enum class SessionState : uint8_t {
    InProgress = 0,  // scan running; checkpoints may be in flight
    Checkpointed = 1, // last checkpoint written cleanly, scan may continue
    Interrupted = 2, // stopped (cancel / crash / close) before completion
    Completed = 3,   // scan finished; journal is the full result set
};

// Scan inputs that define what the saved results mean. Split conceptually
// into correctness-relevant settings (mode, case policy, backend, effective
// verify level: changing them can invalidate saved verdicts) and
// performance-only settings (hashThreads: never invalidates a session).
// CheckResumeCompatible() (SessionLogic.h) implements exactly that split.
struct ScanSettings {
    ScanMode mode = ScanMode::Content;
    bool caseSensitive = false;
    // "auto" | "win32" | "mft" (stored as text to avoid coupling the format
    // to the engine's EnumeratorBackend enum).
    std::string backend = "auto";
    // Requested AND effective verify level (see VerifyInfo): the effective
    // values are what the verdicts were computed with.
    VerifyInfo verify;
    // Performance only: changing it must NOT invalidate a session.
    unsigned hashThreads = 0;
};

struct SideProgress {
    uint64_t files = 0;
    uint64_t dirs = 0;
    uint64_t bytes = 0; // cumulative file sizes visited on this side
};

struct CheckpointInfo {
    uint64_t seq = 0;   // monotonically increasing checkpoint counter
    uint64_t atUnix = 0; // Unix seconds (UTC) when it was written
};

// One finalized, reusable result row.
//
// `verdict` + size/mtime/digest form an indivisible unit: a digest is valid
// ONLY for the exact (size, mtime) version recorded here. A path whose
// current size or mtime differs from the saved ones must be re-verified from
// scratch (see NeedsReverify in SessionLogic.h), even if a digest is present.
// There is intentionally no "path -> digest" shortcut.
struct JournalEntry {
    std::wstring relativePath; // canonical `a\b` form, as in FileEntry
    // Side A (source) observation the verdict was computed from.
    uint64_t sizeA = 0;
    uint64_t mtimeA = 0; // Windows FILETIME, as in FileEntry
    bool hasHashA = false;
    std::array<uint8_t, 32> hashA{};
    // Side B (destination) observation the verdict was computed from.
    uint64_t sizeB = 0;
    uint64_t mtimeB = 0;
    bool hasHashB = false;
    std::array<uint8_t, 32> hashB{};
    // Finalized verdict (see the finality discipline above).
    Status verdict = Status::Identical;
    // Effective verify level applied to this row (Content mode, partial).
    int verifiedPercent = 100;
    PartialPattern verifiedPattern = PartialPattern::Edges;
    bool isDirectory = false;

    bool operator==(const JournalEntry& o) const {
        return relativePath == o.relativePath && sizeA == o.sizeA &&
               mtimeA == o.mtimeA && sizeB == o.sizeB && mtimeB == o.mtimeB &&
               hasHashA == o.hasHashA && hasHashB == o.hasHashB &&
               (!hasHashA || hashA == o.hashA) && (!hasHashB || hashB == o.hashB) &&
               verdict == o.verdict && verifiedPercent == o.verifiedPercent &&
               verifiedPattern == o.verifiedPattern && isDirectory == o.isDirectory;
    }
    bool operator!=(const JournalEntry& o) const { return !(*this == o); }
};

// The full persistent session: everything the future resume engine needs to
// decide, per journaled row, "reusable or re-verify", plus the inputs needed
// to reconstruct the remaining work. No runtime state.
struct ScanSession {
    uint32_t schemaVersion = kSchemaVersion;
    std::string sessionId;   // opaque unique id (see GenerateSessionId)
    uint64_t createdAtUnix = 0; // Unix seconds (UTC)
    std::wstring sourceA;    // live root, or snapshot path for offline runs
    std::wstring sourceB;    // destination root
    ScanSettings settings;
    SessionState state = SessionState::InProgress;
    SideProgress progressA;
    SideProgress progressB;
    Stats stats; // cumulative counters over journaled rows
    CheckpointInfo checkpoint;
    std::vector<JournalEntry> journal; // finalized rows only (see above)
};

} // namespace session
} // namespace bv
