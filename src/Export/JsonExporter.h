#pragma once

#include <string>

#include "Comparison/ComparisonResult.h"
#include "Profiling/DirTiming.h"

namespace bv {
namespace exporting {

// Writes `result` to `filePath` as a streaming JSON array (UTF-8, no BOM).
// Rows are written one at a time, so memory stays bounded on exports with
// millions of entries. Object shape:
//   {"status": "...", "path": "...", "size_source": N, "size_destination": N,
//    "hash_source": "...", "hash_destination": "..."}
// Returns false and fills `error` when the file could not be created/written.
bool WriteJson(const std::wstring& filePath, const ResultSet& result, std::wstring& error);

// Same problems array wrapped in an object together with the
// slowest-directories section:
//   {"problems": [...], "slowest_dirs": {"list_a": [{"dir": "...",
//    "seconds": N}], "walk_a": [...], "list_b": [...], "walk_b": [...],
//    "hash_a": [...], "hash_b": [...], "hash_cache_hits": N}}
// `hashCacheHits` is the run-global counter (not per-directory). Seconds are
// JSON numbers (fractional). Kept as a separate overload so the plain array
// shape above stays byte-identical for existing consumers.
// Semantics (no schema impact): `list_*`/`walk_*` are exact top-N of the
// measured directories; `hash_a`/`hash_b` are estimate-ordered candidates
// from the bounded Space-Saving aggregation, so their order may differ from
// the true ranking for close values.
bool WriteJson(const std::wstring& filePath, const ResultSet& result,
               const profiling::DirTimingReport& timing, uint64_t hashCacheHits,
               const VerifyInfo& verify, std::wstring& error);

} // namespace exporting
} // namespace bv