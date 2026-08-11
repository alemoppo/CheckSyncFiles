#pragma once

#include <string>

#include "Comparison/ComparisonResult.h"

namespace bv {
namespace exporting {

// Writes `result` to `filePath` as UTF-8 CSV (BOM-prefixed so Excel decodes
// accented paths) with columns:
//   status,path,size_source,size_destination,hash_source,hash_destination
// One row per problem entry (identical files are only counted, not listed).
// Returns false and fills `error` when the file could not be created/written.
bool WriteCsv(const std::wstring& filePath, const ResultSet& result, std::wstring& error);

} // namespace exporting
} // namespace bv