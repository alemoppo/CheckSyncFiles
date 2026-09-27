#pragma once

// Resume planning (Phase 1): split a loaded session's journal against two
// freshly enumerated trees into reusable rows and remainder sets.
//
// A journaled row is REUSED only when it is still provably valid:
//   - its verdict is not an error verdict (ReadError / AccessDenied /
//     ChangedDuringScan are always re-verified: they describe an environment,
//     not a stable file version);
//   - both sides are present exactly as the verdict implies (Missing = A-only,
//     Extra = B-only, anything else = both) -- a presence change means the
//     world moved and the row is stale;
//   - size+mtime on every present side still match (see NeedsReverify: a saved
//     digest is valid only for the exact version it was computed from);
//   - the path is not at-or-under a directory whose enumeration reported an
//     error (a partial listing cannot prove anything).
// Everything else goes to the remainder sets (with CURRENT entries, never the
// saved copies) for normal re-verification, or is dropped when the path is
// gone from both sides and outside any errored subtree.
//
// Absence claims need complete enumerations: PlanResume refuses (returns
// false) when either side failed at the root, and when a journaled path is
// missing inside an errored subtree (absence unprovable there). No partial
// resume in Phase 1.

#include <string>
#include <vector>

#include "Comparison/ComparisonResult.h"
#include "Comparison/RowCapture.h"
#include "Filesystem/FileIndex.h"
#include "Session/ScanSession.h"

namespace bv {
namespace session {

struct ResumePlan {
    explicit ResumePlan(bool caseSensitive)
        : remainderA(caseSensitive), remainderB(caseSensitive) {}

    FileIndex remainderA; // current entries still to verify (source side)
    FileIndex remainderB; // current entries still to verify (dest side)
    std::vector<JournalEntry> reusedEntries; // still-valid rows (for re-saving)
    std::vector<FileResult> reusedProblems;  // their non-identical FileResults
    Stats reusedStats; // stats contributions of reused rows (engine-compatible)

    size_t reused = 0;         // rows reused as-is
    size_t stale = 0;          // paths routed to the remainder sets (stale
                               // journal rows plus current paths never seen)
    size_t droppedBothGone = 0; // journaled paths gone from both sides
    size_t errorRows = 0;      // error-verdict rows forced to re-verify
};

struct ResumeInput {
    const ScanSession* session = nullptr;
    const FileIndex* currentA = nullptr; // fresh full enumeration (source)
    const FileIndex* currentB = nullptr; // fresh full enumeration (dest)
    std::wstring rootA;                  // display roots for rebuilt rows
    std::wstring rootB;
    // Relative paths of directories whose enumeration reported errors (from
    // FileIndex::BuildResult::errors); anything at-or-under them is stale.
    std::vector<std::wstring> errorDirsA;
    std::vector<std::wstring> errorDirsB;
};

// Builds the plan. Returns false (with `detail`) when a resume cannot be
// proven sound: incomplete enumerations or unprovable absences. `out` is
// untouched on failure.
bool PlanResume(const ResumeInput& in, ResumePlan& out, std::string& detail);

// Converts a captured row into a journal entry (session save path).
JournalEntry ToJournalEntry(const ClassifiedRow& row);

// Adds one journaled row's stats contributions with engine-compatible counting
// (mirrors onEntry + ClassifyMatched + hash outcomes + finalizeMissingExtra).
// Side presence/types/sizes are caller-provided: current entries at plan time,
// captured-row data at checkpoint time. The verdict counters come from `e`.
void AccumulateJournalStats(Stats& st, const JournalEntry& e, bool hasA, bool aIsDir,
                            uint64_t aSize, bool hasB, bool bIsDir, uint64_t bSize);

} // namespace session
} // namespace bv
