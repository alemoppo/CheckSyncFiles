#pragma once

#include <string>

#include "Comparison/ComparisonResult.h"

namespace bv {
namespace exporting {

// Writes `result` to `filePath` as a streaming JSON array (UTF-8, no BOM).
// Rows are written one at a time, so memory stays bounded on exports with
// millions of entries. Object shape:
//   {"status": "...", "path": "...", "size_source": N, "size_destination": N,
//    "hash_source": "...", "hash_destination": "..."}
// Returns false and fills `error` when the file could not be created/written.
bool WriteJson(const std::wstring& filePath, const ResultSet& result, std::wstring& error);

} // namespace exporting
} // namespace bv