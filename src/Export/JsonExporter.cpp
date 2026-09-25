#include "Export/JsonExporter.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "Export/ExportUtil.h"
#include "Filesystem/PathUtil.h"

namespace bv {
namespace exporting {

namespace {

// One problem object. Shared by both overloads so the array shape stays
// byte-identical.
void WriteProblemObject(std::ofstream& out, const FileResult& p) {
    out << "{\"status\":\"" << StatusToken(p.status)
        << "\",\"path\":\"" << JsonEscape(p.relativePath)
        << "\",\"size_source\":" << std::to_string(p.sizeSource)
        << ",\"size_destination\":" << std::to_string(p.sizeDest)
        << ",\"hash_source\":\"" << HexDigest(p.hasHashSource, p.hashSource)
        << "\",\"hash_destination\":\"" << HexDigest(p.hasHashDest, p.hashDest)
        << "\"}";
}

// One slowest-directories list: "name":[{"dir":"...","seconds":N},...].
// `lastList` selects the separator (no trailing comma before the next key).
void WriteDirList(std::ofstream& out, const char* name,
                  const std::vector<profiling::DirEntry>& entries, bool lastList = false) {
    out << "\"" << name << "\":[";
    bool first = true;
    for (const profiling::DirEntry& e : entries) {
        if (!first) out << ",";
        first = false;
        out << "{\"dir\":\"" << JsonEscape(e.dir) << "\",\"seconds\":" << e.seconds << "}";
    }
    out << "]" << (lastList ? "" : ",");
}

// Lowercase pattern token for the run-level verify section. Resolved
// upstream: Random can never reach the export.
const char* VerifyPatternToken(PartialPattern pattern) {
    return pattern == PartialPattern::Center ? "center" : "edges";
}

} // namespace

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
        WriteProblemObject(out, p);
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

bool WriteJson(const std::wstring& filePath, const ResultSet& result,
               const profiling::DirTimingReport& timing, uint64_t hashCacheHits,
               const VerifyInfo& verify, std::wstring& error) {
    std::ofstream out(pathutil::AddLongPathPrefix(filePath).c_str(),
                      std::ios::binary | std::ios::out | std::ios::trunc);
    if (!out) {
        error = L"impossibile creare il file di esportazione: " + filePath;
        return false;
    }

    out << "{\"problems\":[\n";
    bool first = true;
    for (const FileResult& p : result.problems) {
        if (!first) out << ",\n";
        first = false;
        WriteProblemObject(out, p);
        if (!out.good()) {
            error = L"errore di scrittura durante l'esportazione JSON: " + filePath;
            return false;
        }
    }
    out << "\n],\"slowest_dirs\":{";
    WriteDirList(out, "list_a", timing.listA);
    WriteDirList(out, "walk_a", timing.walkA);
    WriteDirList(out, "list_b", timing.listB);
    WriteDirList(out, "walk_b", timing.walkB);
    WriteDirList(out, "hash_a", timing.hashA);
    WriteDirList(out, "hash_b", timing.hashB, /*lastList=*/true);
    out << ",\"hash_cache_hits\":" << hashCacheHits;
    // Run-level verification mode (always present, marked complete when the
    // effective read was full): requested percent, resolved pattern, and
    // whether Random was resolved for this run.
    out << ",\"verify\":{\"mode\":\""
        << (verify.percentEffective < 100 ? "partial" : "full") << "\""
        << ",\"percent_requested\":" << verify.percentRequested
        << ",\"pattern\":\"" << VerifyPatternToken(verify.pattern) << "\""
        << ",\"random\":" << (verify.patternRandom ? "true" : "false") << "}}\n";

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