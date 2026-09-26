#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "Comparison/ComparisonResult.h"
#include "Comparison/ScanMode.h"

namespace bv {
namespace hashing {
class HashCache;
}

// Single-file re-verification ("Riscansiona"): re-examines exactly one file
// pair without re-running a scan. The worker gets a frozen copy of everything
// it needs and never touches GUI state; the GUI applies the outcome.
struct SingleVerifyRequest {
    std::wstring relativePath; // canonical `a\b` form, as stored in FileResult
    std::wstring sourceRoot;   // A root frozen at click time (live side)
    std::wstring destRoot;     // B root frozen at click time (live side)
    ScanMode mode = ScanMode::Content; // Size or Content (GUI-mapped already)
    // Resolved content level, never Random: Random is resolved once (see
    // ResolveRandomOnce) before the request is built, so the worker is fully
    // deterministic given the request.
    ContentVerifyLevel verify;
    hashing::HashCache* cache = nullptr; // optional, may be null
    const std::atomic_bool* cancel = nullptr; // optional, may be null
};

struct SingleVerifyOutcome {
    std::wstring relativePath;
    // Complete new result. When status is Identical/IdenticalPartial there is
    // no row to show (same convention as ResultSet::problems); the caller
    // removes the old row and bumps the matching counter instead.
    FileResult result;
    bool cancelled = false; // cancel landed mid-hash: no verdict, drop it
};

// Resolves a Random pattern with one system draw (same rule as a full scan).
// Never returns Random; falls back to Edges if the RNG throws.
PartialPattern ResolveRandomOnce();

// Lowest-level reusable check: stat both sides fresh (never trusts stored
// size/mtime/presence), then reuses ClassifyMatched for Size and
// HashOneCandidateInto for Content. No enumeration, no index, no pool.
SingleVerifyOutcome VerifySingleFile(const SingleVerifyRequest& req);

// Coherent internal-error outcome for a single verification that threw an
// unhandled C++ exception: mirrors the pool task-error convention (a throw
// becomes a read error, never a wrong verdict, never std::terminate). `what`
// may be nullptr (unknown exception); the message is always recorded, never
// silent. The GUI consumes it like any ReadError (row replaced, stats fixed),
// so verifyRunning_ always clears and nothing gets stuck.
FileResult MakeInternalVerifyError(const SingleVerifyRequest& req, const char* what);

// Applies one single-verify outcome to a live ResultSet (GUI-owned, main
// thread only): finds the row by relativePath, fixes the verdict counters for
// the old->new transition, and replaces the row -- or removes it when the new
// verdict is Identical/IdenticalPartial (never stored, same convention).
// bytesSource/Dest and file/dir totals are enumeration totals and are left
// untouched. Returns false when no row matched (stale outcome, ignore it).
bool ApplySingleResult(ResultSet& set, const FileResult& fresh);

} // namespace bv
