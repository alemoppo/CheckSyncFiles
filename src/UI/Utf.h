#pragma once

#include <string>

namespace bv::ui {

// Wide (UTF-16 on Windows) <-> UTF-8 conversion helpers for the SDL3 GUI,
// which deals in UTF-8 (const char*) strings.
std::string ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);

} // namespace bv::ui
