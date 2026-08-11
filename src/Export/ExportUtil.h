#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "Comparison/ComparisonResult.h"

namespace bv {
namespace exporting {

enum class ExportFormat : uint8_t { Auto, Csv, Json };

// Stable ASCII status token used as the first CSV column / JSON "status".
std::string StatusToken(Status s);

// RFC 4180 escaping: a field is wrapped in double quotes (and internal quotes
// doubled) when it contains a comma, a quote, a CR or an LF. File names on
// Windows may legally contain any of these.
std::string CsvEscape(const std::wstring& field);

// RFC 8259 escaping of a JSON string. Non-ASCII characters are emitted as raw
// UTF-8; control characters become \n / \t / \b / \f / \r / \uXXXX.
std::string JsonEscape(const std::wstring& field);

// Lowercase hex of a digest (64 chars), or empty when `has` is false.
std::string HexDigest(bool has, const std::array<uint8_t, 32>& digest);

// Infers the export format from the file extension (.json -> Json), else CSV.
ExportFormat InferFormat(const std::wstring& path);

} // namespace exporting
} // namespace bv