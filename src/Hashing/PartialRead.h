#pragma once

#include <cstdint>

#include "Comparison/ScanMode.h"

namespace bv {
namespace partial {

// One MiB in bytes, and the full-read threshold: files at or below two MiB
// are always read completely, whatever the requested percent/pattern (the
// sampling blocks would cover them anyway, so planning is pointless).
constexpr uint64_t kPartialMib = 1048576ull;
constexpr uint64_t kFullReadThreshold = 2097152ull;

// Which byte ranges of a file to read+hash. len2 == 0 means a single block
// (Center always yields one block; Edges yields one when the tail is empty).
struct PartialReadPlan {
    uint64_t off1 = 0;
    uint64_t len1 = 0;
    uint64_t off2 = 0;
    uint64_t len2 = 0;
    bool isFullRead = true;
};

// Pure function: same (size, percent, pattern) always yields the same plan.
// Opens no files and depends on no globals or clock: this determinism is what
// lets sides A and B hash exactly the same bytes. Random is NEVER a valid
// input here (resolved once per run upstream); Edges/Center only.
PartialReadPlan ComputePartialReadPlan(uint64_t size, int percent, PartialPattern pattern);

// Effective verification level for cache keys and per-file records, derived
// from a computed plan: a plan that collapses to a full read always
// normalizes to {100, Edges} whatever was requested, so a file read fully
// under any requested combination shares one cache key. `resolvedPattern`
// must already be concrete (never Random).
struct EffectiveVerify {
    int percent = 100;
    PartialPattern pattern = PartialPattern::Edges;
};
EffectiveVerify EffectiveLevel(const PartialReadPlan& plan, int requestedPercent,
                               PartialPattern resolvedPattern);

} // namespace partial
} // namespace bv
