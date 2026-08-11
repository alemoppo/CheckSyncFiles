#include "Export/CsvExporter.h"

#include <fstream>
#include <string>

#include "Export/ExportUtil.h"
#include "Filesystem/PathUtil.h"

namespace bv {
namespace exporting {

bool WriteCsv(const std::wstring& filePath, const ResultSet& result, std::wstring& error) {
    std::ofstream out(pathutil::AddLongPathPrefix(filePath).c_str(),
                      std::ios::binary | std::ios::out | std::ios::trunc);
    if (!out) {
        error = L"impossibile creare il file di esportazione: " + filePath;
        return false;
    }

    // UTF-8 BOM: makes Excel and Notepad recognise the encoding.
    const char bom[3] = {'\xEF', '\xBB', '\xBF'};
    out.write(bom, 3);

    out << "status,path,size_source,size_destination,hash_source,hash_destination\n";
    for (const FileResult& p : result.problems) {
        out << StatusToken(p.status) << ','
            << CsvEscape(p.relativePath) << ','
            << std::to_string(p.sizeSource) << ','
            << std::to_string(p.sizeDest) << ','
            << HexDigest(p.hasHashSource, p.hashSource) << ','
            << HexDigest(p.hasHashDest, p.hashDest) << '\n';
        if (!out.good()) {
            error = L"errore di scrittura durante l'esportazione CSV: " + filePath;
            return false;
        }
    }

    out.flush();
    if (!out.good()) {
        error = L"errore di scrittura durante l'esportazione CSV: " + filePath;
        return false;
    }
    error.clear();
    return true;
}

} // namespace exporting
} // namespace bv