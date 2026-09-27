#pragma once

// Pure session logic: staleness checks and resume-compatibility checks.
// No I/O, no threads, no engine coupling beyond the model types: every
// function here is a deterministic function of its inputs and is unit-tested
// directly.

#include <cstdint>
#include <string>

#include "Session/ScanSession.h"

namespace bv {
namespace session {

// A saved (size, mtime) fingerprint is stale when the current observation
// differs. Delegates to the engine's own rule (HashMetadataChanged in
// Hashing/Sha256.h: exact inequality on size OR mtime) so sessions never
// invent a divergent mtime semantics.
bool FingerprintStale(uint64_t savedSize, uint64_t savedMtime, uint64_t curSize,
                      uint64_t curMtime);

// True when the journaled row can no longer be trusted for the current tree
// state: either side's fingerprint changed. Directories carry no content, but
// the same conservative rule applies (a changed dir mtime means "look again").
bool NeedsReverify(const JournalEntry& saved, uint64_t curSizeA, uint64_t curMtimeA,
                   uint64_t curSizeB, uint64_t curMtimeB);

struct CompatResult {
    bool compatible = false;
    // Human-readable reason (narrow/UTF-8); meaningful when !compatible.
    std::string reason;
};

// Decides whether a saved session may be resumed under `current` settings.
// Correctness-relevant settings (mode, case policy, backend, effective verify
// level) must match; performance-only settings (hashThreads) are ignored, so
// retuning the thread count never invalidates a session. Returns
// {true, "compatible"} on match.
CompatResult CheckResumeCompatible(const ScanSettings& saved,
                                   const ScanSettings& current);

// Opaque unique session id: hex(<unix nanos>)-hex(<process-wide counter>).
// Unique enough for checkpoint files on one machine; not a UUID.
std::string GenerateSessionId();

// Current UTC time as Unix seconds.
uint64_t NowUnixSeconds();

const char* SessionStateName(SessionState s);

} // namespace session
} // namespace bv
