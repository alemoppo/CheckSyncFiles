#include "Export/ExportUtil.h"

#include <cstdio>
#include <cwctype>

#include "Filesystem/PathUtil.h"

namespace bv {
namespace exporting {

std::string StatusToken(Status s) {
    switch (s) {
        case Status::Identical: return "IDENTICO";
        case Status::Missing: return "MANCANTE";
        case Status::Extra: return "EXTRA";
        case Status::SizeMismatch: return "DIM_DIVERSA";
        case Status::ContentMismatch: return "CONTENUTO_DIVERSO";
        case Status::ReadError: return "ERRORE_LETTURA";
        case Status::AccessDenied: return "ACCESSO_NEGATO";
        case Status::ChangedDuringScan: return "MODIFICATO_DURANTE_SCAN";
    }
    return "?";
}

namespace {
// True for characters a spreadsheet would read as the start of a formula or
// hyperlink when a CSV cell begins with them. Such cells are carriers for the
// classic "CSV/DDE formula injection".
bool CsvFormulaTrigger(wchar_t c) {
    return c == L'=' || c == L'+' || c == L'-' || c == L'@' ||
           c == L'\t' || c == L'\r' || c == L'\f';
}
} // namespace

std::string CsvEscape(const std::wstring& s) {
    const std::string u8 = pathutil::ToUtf8(s);
    bool need = false;
    for (wchar_t c : s) {
        if (c == L',' || c == L'"' || c == L'\n' || c == L'\r') {
            need = true;
            break;
        }
    }
    // CSV-injection defense: a cell that BEGINS with = + - @ \t \r \f would be
    // interpreted by Excel/LibreOffice as a formula (or DDE hyperlink) when the
    // export is opened directly, turning a file name like "=SUM(A1)" into an
    // executable payload. Force RFC-4180 quoting and prefix an apostrophe
    // inside the quotes -- the standard neutralization marker. Programmatic
    // consumers must strip the leading apostrophe of such cells.
    const bool formulaRisk = !s.empty() && CsvFormulaTrigger(s[0]);
    if (!need && !formulaRisk) return u8;

    std::string out;
    out.reserve(u8.size() + 8);
    out += '"';
    if (formulaRisk) out += '\'';
    for (char c : u8) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

std::string JsonEscape(const std::wstring& s) {
    const std::string u8 = pathutil::ToUtf8(s);
    std::string out;
    out.reserve(u8.size() + 8);
    for (char c : u8) {
        const unsigned int u = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (u < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", u);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string HexDigest(bool has, const std::array<uint8_t, 32>& digest) {
    if (!has) return {};
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint8_t b : digest) {
        out += hex[(b >> 4) & 0xF];
        out += hex[b & 0xF];
    }
    return out;
}

ExportFormat InferFormat(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        std::wstring ext = path.substr(dot + 1);
        for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towupper(c));
        if (ext == L"JSON") return ExportFormat::Json;
    }
    return ExportFormat::Csv;
}

} // namespace exporting
} // namespace bv