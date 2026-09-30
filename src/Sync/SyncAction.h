#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Filesystem/ReparsePoint.h"

namespace bv {
namespace sync {

// One executable filesystem step. `sourceRoot`/`destRoot` live on the plan
// (global A->B or swapped for manual B->A); the action only carries the
// relative path, so plans stay comparable, sortable and testable.
// NOTE: File/Delete/Copy/Replace prefixes (not bare CopyFile/DeleteFile):
// windows.h function-like macros rewrite those tokens even after "::".
enum class SyncOp {
    FileCopy,    // new file src -> dst (dest must not exist; races adapt)
    FileReplace, // file src -> dst, overwriting via temp + rename
    FileDelete,  // delete a file at dst (absent = success, no-op)
    DirCreate,   // create an empty dir at dst (exists = success, no-op)
    DirDelete,   // recursive delete of a dir at dst (guarded, see below)
    LinkCreate,  // create a link at dst from the live src target (rebased)
    LinkReplace, // LinkDelete + LinkCreate at dst
    LinkDelete,  // delete a link at dst itself, never its target
};

struct SyncAction {
    SyncOp op = SyncOp::FileCopy;
    std::wstring relativePath;
    bool isDirectory = false;
    ReparseKind linkKind = ReparseKind::None; // for link ops
    uint64_t bytes = 0;                       // estimated inbound bytes (copies)
};

// Short Italian label for summary/confirm/progress UI ("Copia file: a\b").
std::string DescribeAction(const SyncAction& a);

} // namespace sync
} // namespace bv
