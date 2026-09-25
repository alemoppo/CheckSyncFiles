#include "Hashing/PartialRead.h"

namespace bv {
namespace partial {

PartialReadPlan ComputePartialReadPlan(uint64_t size, int percent, PartialPattern pattern) {
    PartialReadPlan p;
    // Rule 1: tiny files always read fully (blocks would cover them anyway).
    if (size <= kFullReadThreshold) {
        p.isFullRead = true;
        return p;
    }
    // Rule 2: percent 100 (or above, defensively) is today's full Content.
    if (percent >= 100) {
        p.isFullRead = true;
        return p;
    }
    if (percent < 1) percent = 1; // struct contract is 1..100; never zero blocks
    if (pattern == PartialPattern::Random) pattern = PartialPattern::Edges; // defensive

    // Target 1 MiB blocks, round-half-up, integer arithmetic only (bit-exact
    // determinism between sides A and B depends on this; never float):
    //   target = round(size * percent / (100 * MiB))
    // Overflow-safe split: size = q*D + r with D = 100 MiB, so r*percent and
    // q*percent both fit comfortably in 64 bits for any file size.
    const uint64_t D = 100ull * kPartialMib;
    const uint64_t q = size / D;
    const uint64_t r = size % D;
    const uint64_t up = static_cast<uint64_t>(percent);
    uint64_t target = q * up + (r * up + D / 2) / D;
    if (target == 0) target = 1; // rounding must never yield zero blocks

    // ceil(size / MiB) without overflow: the whole-file block count used to
    // detect plans that already cover the file (then read it fully).
    const uint64_t wholeMibs = (size + kPartialMib - 1) / kPartialMib;

    if (pattern == PartialPattern::Center) {
        // NOTE ON CENTER (deliberate discretization, not a bug): the centre is
        // a sampling aligned to 1 MiB blocks, NOT the exact mathematical
        // centre of the file: when the size is not a multiple of one MiB the
        // trailing fraction is never sampled and the chosen block does not sit
        // on the true midpoint. Do NOT "fix" this later with fractional
        // offsets: both sides must agree bit-exactly, and integer blocks are
        // what guarantees it.
        if (target >= wholeMibs) {
            p.isFullRead = true;
            return p;
        }
        const uint64_t numBlocks = size / kPartialMib; // floor; trailing fraction unsampled
        const uint64_t discard = numBlocks - target;   // >= 0 (shown: target <= numBlocks)
        // Centre the target blocks; an odd side-excess stays BEFORE the
        // centre, never after (same "towards the start" convention as Edges).
        const uint64_t before = (discard + 1) / 2;
        p.off1 = before * kPartialMib;
        p.len1 = target * kPartialMib;
        p.off2 = 0;
        p.len2 = 0; // single block
        p.isFullRead = false;
        return p;
    }

    // Edges: head block from offset zero, tail block ending exactly at EOF,
    // with the odd block (if the total is odd) assigned to the head.
    if (target >= wholeMibs) {
        p.isFullRead = true;
        return p;
    }
    const uint64_t head = (target + 1) / 2;
    const uint64_t tail = target / 2;
    p.off1 = 0;
    p.len1 = head * kPartialMib;
    if (tail == 0) {
        p.off2 = 0;
        p.len2 = 0; // single block (head only)
    } else {
        p.len2 = tail * kPartialMib;
        p.off2 = size - p.len2; // safe: target*MiB < size was checked above
    }
    p.isFullRead = false;
    return p;
}

EffectiveVerify EffectiveLevel(const PartialReadPlan& plan, int requestedPercent,
                               PartialPattern resolvedPattern) {
    if (plan.isFullRead) return EffectiveVerify{100, PartialPattern::Edges};
    if (resolvedPattern == PartialPattern::Random) resolvedPattern = PartialPattern::Edges;
    return EffectiveVerify{requestedPercent, resolvedPattern};
}

} // namespace partial
} // namespace bv
