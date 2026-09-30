#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "Sync/SyncPlan.h"

namespace bv {
namespace sync {

struct SyncActionResult {
    SyncOp op = SyncOp::FileCopy;
    std::wstring relativePath;
    bool ok = false;
    bool skipped = false;   // safe no-op: shared content, already converged, ...
    bool cancelled = false; // user cancel or disk-full abort
    std::wstring message;   // details for failures / notable no-ops
};

struct SyncReport {
    std::vector<SyncActionResult> items;
    size_t doneCount = 0;
    size_t failedCount = 0;
    size_t skippedCount = 0;
    bool cancelled = false;    // user asked to stop: remaining actions untouched
    bool diskFullAbort = false; // remaining actions untouched, no rollback
};

// (done, total, currentRel) progress callback, called on the worker thread.
using SyncProgressFn = std::function<void(size_t, size_t, const std::wstring&)>;

// Executes every action in plan order on the calling thread (the orchestrator
// runs this on its worker). Error policy: independent failures are collected
// and execution continues; user cancel and disk-full stop the plan (remaining
// actions are reported as cancelled/skipped, completed ones stay valid, no
// rollback). Starts with a free-space precheck: when the inbound bytes
// clearly exceed the free space, nothing runs.
SyncReport ExecutePlan(const SyncPlan& plan, const std::atomic_bool* cancel,
                       const SyncProgressFn& onProgress = {});

} // namespace sync
} // namespace bv
