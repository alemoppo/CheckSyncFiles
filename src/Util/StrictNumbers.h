#pragma once

// Strict, exception-free parsing of unsigned CLI numeric arguments.
//
// Plain std::stoull/std::stoul throw std::invalid_argument / std::out_of_range
// on malformed or overflowing input; an uncaught exception would terminate the
// CLI with no user-facing error. These helpers accept only decimal digits and
// return false on any other input (empty, sign, whitespace, trailing junk,
// overflow), so callers can print a localized diagnostic and fail cleanly.

#include <cstdint>
#include <limits>
#include <string>

namespace bv {
namespace util {

// Parses `s` as a decimal into `out`, with an upper bound `max`. `out` is
// written only on success. Never throws.
inline bool ParseUInt64(const std::wstring& s, uint64_t& out,
                        uint64_t max = std::numeric_limits<uint64_t>::max()) {
    if (s.empty()) return false;
    uint64_t v = 0;
    for (const wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
        const uint64_t d = static_cast<uint64_t>(c - L'0');
        if (v > (max - d) / 10) return false; // would exceed `max`
        v = v * 10 + d;
    }
    out = v;
    return true;
}

// Friendly cap for --threads (0 stays meaningful: auto). Keeps a hostile or
// mistyped value from being passed on as an absurd thread count.
inline constexpr unsigned int kMaxThreads = 4096;

inline bool ParseThreadCount(const std::wstring& s, unsigned int& out) {
    uint64_t v = 0;
    if (!ParseUInt64(s, v, kMaxThreads)) return false;
    out = static_cast<unsigned int>(v);
    return true;
}

} // namespace util
} // namespace bv