#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Comparison/ComparisonResult.h"
#include "Sync/SyncAction.h"

namespace bv {
namespace sync {

// An ordered, executable A->B (or B->A for manual single actions) plan built
// from comparison rows. Pure data: no filesystem access at build time.
// Order: CreateDir (shallow first) -> file copies/replaces -> link
// creates/replaces -> file/link deletes -> DeleteDir (deep first).
struct SyncPlan {
    std::wstring sourceRoot; // live side the content comes from
    std::wstring destRoot;   // live side being modified
    bool sourceIsA = true;   // false for manual B->A actions
    std::vector<SyncAction> actions;

    struct Summary {
        size_t copyFiles = 0;
        size_t replaceFiles = 0;
        size_t deleteFiles = 0;
        size_t createDirs = 0;
        size_t deleteDirs = 0;
        size_t createLinks = 0;
        size_t replaceLinks = 0;
        size_t deleteLinks = 0;
        uint64_t bytesToCopy = 0; // estimated inbound bytes
    };
    Summary summary;
    // Human-readable "skipped: reason" lines for the confirm dialog.
    std::vector<std::string> skipped;
    // Folded relative paths of every Extra row: the guarded recursive delete
    // may only remove entries listed here (plus dirs that become empty).
    std::vector<std::wstring> extraFoldedRels;
};

// "Sincronizza tutto": render destRoot equal to sourceRoot.
SyncPlan BuildSyncPlan(const ResultSet& results, const std::wstring& sourceRoot,
                       const std::wstring& destRoot, bool sourceIsA = true);

// Single-row manual operations (context menu): the plan holds one action.
// DeleteOp removes whatever the row describes at the DEST side; CreateDirOp
// is for Missing-dir rows (dir must exist at the SOURCE side).
enum class ManualOp {
    CopyToDst,    // Missing file/link row -> copy/create at dst
    ReplaceToDst, // different file/link row -> replace at dst
    DeleteAtDst,  // Extra file/dir/link row -> delete at dst
    CreateDirDst, // Missing dir row -> create empty dir at dst
};

SyncPlan BuildSingleActionPlan(const FileResult& row, ManualOp op,
                               const std::wstring& rootA, const std::wstring& rootB,
                               bool toA);

} // namespace sync
} // namespace bv
