#include "Export/JsonExporter.h"

#include <fstream>
#include <string>

#include "Export/ExportUtil.h"
#include "Filesystem/PathUtil.h"

namespace bv {
namespace exporting {

bool WriteJson(const std::wstring& filePath, const ResultSet& result, std::wstring& error) {
    std::ofstream out(pathutil::AddLongPathPrefix(filePath).c_str(),
                      std::ios::binary | std::ios::out | std::ios::trunc);
    if (!out) {
        error = L"impossibile creare il file di esportazione: " + filePath;
        return false;
    }

    out << "[\n";
    bool first = true;
    for (const FileResult& p : result.problems) {
        // RFC 8259: no trailing comma -- separator goes between items only.
        if (!first) out << ",\n";
        first = false;
        out << "{\"status\":\"" << StatusToken(p.status)
            << "\",\"path\":\"" << JsonEscape(p.relativePath)
            << "\",\"size_source\":" << std::to_string(p.sizeSource)
            << ",\"size_destination\":" << std::to_string(p.sizeDest)
            << ",\"hash_source\":\"" << HexDigest(p.hasHashSource, p.hashSource)
            << "\",\"hash_destination\":\"" << HexDigest(p.hasHashDest, p.hashDest)
            << "\"}";
        if (!out.good()) {
            error = L"errore di scrittura durante l'esportazione JSON: " + filePath;
            return false;
        }
    }
    out << "\n]\n";

    out.flush();
    if (!out.good()) {
        error = L"errore di scrittura durante l'esportazione JSON: " + filePath;
        return false;
    }
    error.clear();
    return true;
}

} // namespace exporting
} // namespace bv