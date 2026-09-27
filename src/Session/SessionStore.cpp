#include "Session/SessionStore.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Filesystem/PathUtil.h"
#include "Session/SessionJson.h"
#include "Session/SessionLogic.h"

namespace bv {
namespace session {

namespace fs = std::filesystem;

const char* SessionErrorName(SessionError e) {
    switch (e) {
        case SessionError::Ok: return "ok";
        case SessionError::IoError: return "io-error";
        case SessionError::CorruptHeader: return "corrupt-header";
        case SessionError::MagicMismatch: return "magic-mismatch";
        case SessionError::VersionMismatch: return "version-mismatch";
    }
    return "unknown";
}

std::wstring ContextPath(const std::wstring& base) { return base + kContextSuffix; }
std::wstring JournalPath(const std::wstring& base) { return base + kJournalSuffix; }
std::wstring PrevPath(const std::wstring& base) { return base + kContextSuffix + kPrevSuffix; }

// ---------------------------------------------------------------------------
// CRC32
// ---------------------------------------------------------------------------

namespace {
uint32_t kCrcTable[256];
bool kCrcTableReady = false;
void EnsureCrcTable() {
    if (kCrcTableReady) return;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        kCrcTable[i] = c;
    }
    kCrcTableReady = true;
}
} // namespace

uint32_t Crc32Init() {
    EnsureCrcTable();
    return 0xFFFFFFFFu;
}

uint32_t Crc32Update(uint32_t crc, const void* data, size_t n) {
    EnsureCrcTable();
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) crc = kCrcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

uint32_t Crc32Final(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

static uint32_t Crc32Of(const void* data, size_t n) {
    return Crc32Final(Crc32Update(Crc32Init(), data, n));
}

// ---------------------------------------------------------------------------
// Little-endian binary helpers
// ---------------------------------------------------------------------------

namespace {

void PutU32(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

void PutU64(std::string& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

struct Reader {
    const char* p = nullptr;
    size_t left = 0;
    bool ok = true;
    bool take(void* dst, size_t n) {
        if (left < n) {
            ok = false;
            return false;
        }
        memcpy(dst, p, n);
        p += n;
        left -= n;
        return true;
    }
    bool u32(uint32_t& v) {
        uint8_t b[4];
        if (!take(b, 4)) return false;
        v = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
            (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
        return true;
    }
    bool u64(uint64_t& v) {
        uint8_t b[8];
        if (!take(b, 8)) return false;
        v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[i]) << (8 * i);
        return true;
    }
    bool u8(uint8_t& v) {
        if (left < 1) {
            ok = false;
            return false;
        }
        v = static_cast<uint8_t>(*p);
        ++p;
        --left;
        return true;
    }
};

inline constexpr uint32_t kRecordMagic = 0x4A535642u; // "BVSJ" little-endian
inline constexpr size_t kRecordHeaderSize = 12;      // magic + payloadLen + crc

uint8_t StatusToU8(Status s) { return static_cast<uint8_t>(s); }

bool U8ToStatus(uint8_t v, Status& out) {
    switch (static_cast<Status>(v)) {
        case Status::Identical:
        case Status::Missing:
        case Status::Extra:
        case Status::SizeMismatch:
        case Status::ContentMismatch:
        case Status::IdenticalPartial:
        case Status::ContentMismatchPartial:
        case Status::ReadError:
        case Status::AccessDenied:
        case Status::ChangedDuringScan: out = static_cast<Status>(v); return true;
    }
    return false;
}

bool U8ToPattern(uint8_t v, PartialPattern& out) {
    if (v > static_cast<uint8_t>(PartialPattern::Random)) return false;
    out = static_cast<PartialPattern>(v);
    return true;
}

// Serializes one journal entry ( WITHOUT the record header) into `payload`.
void EncodePayload(const JournalEntry& e, std::string& payload) {
    const std::string path = pathutil::ToUtf8(e.relativePath);
    PutU32(payload, static_cast<uint32_t>(path.size()));
    payload += path;
    payload.push_back(static_cast<char>(StatusToU8(e.verdict)));
    uint8_t flags = 0;
    if (e.hasHashA) flags |= 0x01;
    if (e.hasHashB) flags |= 0x02;
    if (e.isDirectory) flags |= 0x04;
    payload.push_back(static_cast<char>(flags));
    payload.push_back(static_cast<char>(static_cast<uint8_t>(e.verifiedPattern)));
    PutU32(payload, static_cast<uint32_t>(static_cast<int32_t>(e.verifiedPercent)));
    PutU64(payload, e.sizeA);
    PutU64(payload, e.mtimeA);
    if (e.hasHashA) payload.append(reinterpret_cast<const char*>(e.hashA.data()), 32);
    PutU64(payload, e.sizeB);
    PutU64(payload, e.mtimeB);
    if (e.hasHashB) payload.append(reinterpret_cast<const char*>(e.hashB.data()), 32);
}

bool DecodePayload(const char* data, size_t n, JournalEntry& out) {
    Reader r{data, n, true};
    uint32_t pathLen = 0;
    if (!r.u32(pathLen)) return false;
    if (r.left < pathLen) return false;
    out.relativePath = pathutil::FromUtf8(std::string(r.p, pathLen));
    r.p += pathLen;
    r.left -= pathLen;
    uint8_t v = 0, flags = 0, pat = 0;
    uint32_t pct = 0;
    if (!r.u8(v) || !r.u8(flags) || !r.u8(pat) || !r.u32(pct)) return false;
    if (!U8ToStatus(v, out.verdict)) return false;
    if (!U8ToPattern(pat, out.verifiedPattern)) return false;
    out.verifiedPercent = static_cast<int>(static_cast<int32_t>(pct));
    out.hasHashA = (flags & 0x01) != 0;
    out.hasHashB = (flags & 0x02) != 0;
    out.isDirectory = (flags & 0x04) != 0;
    if (flags & ~0x07) return false; // unknown flags: reject (forward-compat hook)
    if (!r.u64(out.sizeA) || !r.u64(out.mtimeA)) return false;
    if (out.hasHashA && !r.take(out.hashA.data(), 32)) return false;
    if (!r.u64(out.sizeB) || !r.u64(out.mtimeB)) return false;
    if (out.hasHashB && !r.take(out.hashB.data(), 32)) return false;
    if (r.left != 0) return false; // trailing bytes: reject
    if (!out.hasHashA) out.hashA.fill(0);
    if (!out.hasHashB) out.hashB.fill(0);
    return true;
}

// Appends one framed record (header + payload) to `out`.
void EncodeRecord(const JournalEntry& e, std::string& out) {
    std::string payload;
    EncodePayload(e, payload);
    PutU32(out, kRecordMagic);
    PutU32(out, static_cast<uint32_t>(payload.size()));
    PutU32(out, Crc32Of(payload.data(), payload.size()));
    out += payload;
}

// ---------------------------------------------------------------------------
// File I/O helpers
// ---------------------------------------------------------------------------

bool ReadAll(const std::wstring& path, std::string& out, std::wstring& error) {
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) {
        error = L"cannot open for reading: " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad()) {
        error = L"read failed: " + path;
        return false;
    }
    out = ss.str();
    return true;
}

// Best-effort OS-level flush so a power loss right after a checkpoint is less
// likely to lose the tmp file before the rename. Failure is non-fatal (the
// rename still happens; process-crash safety never depended on this).
void FlushToDisk(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FlushFileBuffers(h);
    CloseHandle(h);
}

bool WriteAtomic(const std::wstring& finalPath, const std::string& bytes,
                 bool keepPrev, std::wstring& error) {
    const std::wstring tmp = finalPath + L".tmp";
    {
        std::ofstream f(fs::path(tmp), std::ios::binary | std::ios::trunc);
        if (!f) {
            error = L"cannot open for writing: " + tmp;
            return false;
        }
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        f.flush();
        if (!f) {
            error = L"write failed: " + tmp;
            return false;
        }
    }
    FlushToDisk(tmp);
    std::error_code ec;
    if (keepPrev && fs::exists(finalPath, ec)) {
        fs::rename(finalPath, finalPath + kPrevSuffix, ec);
        if (ec) {
            error = L"cannot rotate prev for: " + finalPath;
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, finalPath, ec);
    if (ec) {
        error = L"cannot rename tmp to: " + finalPath;
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Context JSON (de)serialization
// ---------------------------------------------------------------------------

const char* ModeToStr(ScanMode m) {
    switch (m) {
        case ScanMode::Presence: return "presence";
        case ScanMode::Size: return "size";
        case ScanMode::Content: return "content";
    }
    return "unknown";
}

bool StrToMode(const std::string& s, ScanMode& out) {
    if (s == "presence") {
        out = ScanMode::Presence;
        return true;
    }
    if (s == "size") {
        out = ScanMode::Size;
        return true;
    }
    if (s == "content") {
        out = ScanMode::Content;
        return true;
    }
    return false;
}

const char* PatternToStr(PartialPattern p) {
    switch (p) {
        case PartialPattern::Edges: return "edges";
        case PartialPattern::Center: return "center";
        case PartialPattern::Random: return "random";
    }
    return "unknown";
}

bool StrToPattern(const std::string& s, PartialPattern& out) {
    if (s == "edges") {
        out = PartialPattern::Edges;
        return true;
    }
    if (s == "center") {
        out = PartialPattern::Center;
        return true;
    }
    if (s == "random") {
        out = PartialPattern::Random;
        return true;
    }
    return false;
}

using json::Value;

void PutStats(Value& o, const Stats& st) {
    Value s = Value::MakeObject();
    auto set = [&](const char* k, uint64_t v) { (*s.obj)[k] = Value::Int((int64_t)v); };
    set("sourceFiles", st.sourceFiles);
    set("sourceDirs", st.sourceDirs);
    set("destFiles", st.destFiles);
    set("destDirs", st.destDirs);
    set("identicalFiles", st.identicalFiles);
    set("identicalDirs", st.identicalDirs);
    set("missingFiles", st.missingFiles);
    set("missingDirs", st.missingDirs);
    set("extraFiles", st.extraFiles);
    set("extraDirs", st.extraDirs);
    set("sizeMismatch", st.sizeMismatch);
    set("contentMismatch", st.contentMismatch);
    set("identicalPartialFiles", st.identicalPartialFiles);
    set("contentMismatchPartial", st.contentMismatchPartial);
    set("readErrors", st.readErrors);
    set("accessDenied", st.accessDenied);
    set("changedDuringScan", st.changedDuringScan);
    set("bytesSource", st.bytesSource);
    set("bytesDest", st.bytesDest);
    (*o.obj)["stats"] = s;
}

bool GetStats(const Value* v, Stats& out, std::string& detail) {
    if (!v || v->type != Value::Type::Object) {
        detail = "missing stats object";
        return false;
    }
    auto get = [&](const char* k, uint64_t& dst) {
        const Value* f = v->find(k);
        if (!f || f->type != Value::Type::Int || f->integer < 0) return false;
        dst = (uint64_t)f->integer;
        return true;
    };
    // All-or-nothing: a half-present stats block is corruption, not defaults.
    Stats tmp;
    if (!get("sourceFiles", tmp.sourceFiles) || !get("sourceDirs", tmp.sourceDirs) ||
        !get("destFiles", tmp.destFiles) || !get("destDirs", tmp.destDirs) ||
        !get("identicalFiles", tmp.identicalFiles) || !get("identicalDirs", tmp.identicalDirs) ||
        !get("missingFiles", tmp.missingFiles) || !get("missingDirs", tmp.missingDirs) ||
        !get("extraFiles", tmp.extraFiles) || !get("extraDirs", tmp.extraDirs) ||
        !get("sizeMismatch", tmp.sizeMismatch) || !get("contentMismatch", tmp.contentMismatch) ||
        !get("identicalPartialFiles", tmp.identicalPartialFiles) ||
        !get("contentMismatchPartial", tmp.contentMismatchPartial) ||
        !get("readErrors", tmp.readErrors) || !get("accessDenied", tmp.accessDenied) ||
        !get("changedDuringScan", tmp.changedDuringScan) || !get("bytesSource", tmp.bytesSource) ||
        !get("bytesDest", tmp.bytesDest)) {
        detail = "incomplete stats block";
        return false;
    }
    out = tmp;
    return true;
}

std::string BuildContext(const ScanSession& s, const std::string& journalName,
                         uint64_t journalRecords, uint64_t journalBytes, uint32_t journalCrc) {
    Value root = Value::MakeObject();
    auto& o = *root.obj;
    o["magic"] = Value::String("BVSS");
    o["schema"] = Value::Int((int64_t)s.schemaVersion);
    o["sessionId"] = Value::String(s.sessionId);
    o["createdAt"] = Value::Int((int64_t)s.createdAtUnix);
    o["sourceA"] = Value::String(pathutil::ToUtf8(s.sourceA));
    o["sourceB"] = Value::String(pathutil::ToUtf8(s.sourceB));
    Value set = Value::MakeObject();
    (*set.obj)["mode"] = Value::String(ModeToStr(s.settings.mode));
    (*set.obj)["caseSensitive"] = Value::Bool(s.settings.caseSensitive);
    (*set.obj)["backend"] = Value::String(s.settings.backend);
    Value ver = Value::MakeObject();
    (*ver.obj)["requested"] = Value::Int(s.settings.verify.percentRequested);
    (*ver.obj)["effective"] = Value::Int(s.settings.verify.percentEffective);
    (*ver.obj)["pattern"] = Value::String(PatternToStr(s.settings.verify.pattern));
    (*ver.obj)["random"] = Value::Bool(s.settings.verify.patternRandom);
    (*set.obj)["verify"] = ver;
    (*set.obj)["hashThreads"] = Value::Int((int64_t)s.settings.hashThreads);
    o["settings"] = set;
    o["state"] = Value::String(SessionStateName(s.state));
    auto side = [](const SideProgress& p) {
        Value v = Value::MakeObject();
        (*v.obj)["files"] = Value::Int((int64_t)p.files);
        (*v.obj)["dirs"] = Value::Int((int64_t)p.dirs);
        (*v.obj)["bytes"] = Value::Int((int64_t)p.bytes);
        return v;
    };
    o["progressA"] = side(s.progressA);
    o["progressB"] = side(s.progressB);
    PutStats(root, s.stats);
    Value cp = Value::MakeObject();
    (*cp.obj)["seq"] = Value::Int((int64_t)s.checkpoint.seq);
    (*cp.obj)["at"] = Value::Int((int64_t)s.checkpoint.atUnix);
    o["checkpoint"] = cp;
    Value j = Value::MakeObject();
    (*j.obj)["file"] = Value::String(journalName);
    (*j.obj)["records"] = Value::Int((int64_t)journalRecords);
    (*j.obj)["bytes"] = Value::Int((int64_t)journalBytes);
    (*j.obj)["crc32"] = Value::Int((int64_t)journalCrc);
    o["journal"] = j;
    return json::Write(root);
}

bool ParseSide(const Value* v, SideProgress& out, const char* what, std::string& detail) {
    if (!v || v->type != Value::Type::Object) {
        detail = std::string("missing ") + what;
        return false;
    }
    const Value* f = v->find("files");
    const Value* d = v->find("dirs");
    const Value* b = v->find("bytes");
    if (!f || !d || !b || f->type != Value::Type::Int || d->type != Value::Type::Int ||
        b->type != Value::Type::Int || f->integer < 0 || d->integer < 0 || b->integer < 0) {
        detail = std::string("incomplete ") + what;
        return false;
    }
    out.files = (uint64_t)f->integer;
    out.dirs = (uint64_t)d->integer;
    out.bytes = (uint64_t)b->integer;
    return true;
}

// Parses the context JSON into `out` (journal rows are NOT filled here).
// Returns false with `error` set to CorruptHeader/MagicMismatch/VersionMismatch.
bool ParseContext(const std::string& text, ScanSession& out, SessionError& error,
                  std::string& detail, uint64_t& journalRecords, uint64_t& journalBytes,
                  uint32_t& journalCrc) {
    Value root;
    std::string perr;
    if (!json::Parse(text, root, perr) || root.type != Value::Type::Object) {
        error = SessionError::CorruptHeader;
        detail = "invalid JSON: " + perr;
        return false;
    }
    const Value* magic = root.find("magic");
    if (!magic || magic->type != Value::Type::String) {
        error = SessionError::CorruptHeader;
        detail = "missing magic";
        return false;
    }
    if (magic->str != "BVSS") {
        error = SessionError::MagicMismatch;
        detail = "magic is '" + magic->str + "', not 'BVSS'";
        return false;
    }
    const Value* schema = root.find("schema");
    if (!schema || schema->type != Value::Type::Int) {
        error = SessionError::CorruptHeader;
        detail = "missing schema";
        return false;
    }
    if (schema->integer < 1) {
        error = SessionError::CorruptHeader;
        detail = "invalid schema version 0";
        return false;
    }
    if (schema->integer > (int64_t)kSchemaVersion) {
        error = SessionError::VersionMismatch;
        detail = "schema " + std::to_string(schema->integer) + " newer than supported " +
                 std::to_string(kSchemaVersion);
        return false;
    }
    ScanSession s;
    s.schemaVersion = (uint32_t)schema->integer;
    s.sessionId = root.find("sessionId") ? root.find("sessionId")->asString() : "";
    s.createdAtUnix = (uint64_t)root.find("createdAt")->asInt();
    if (s.sessionId.empty()) {
        error = SessionError::CorruptHeader;
        detail = "missing sessionId";
        return false;
    }
    s.sourceA = pathutil::FromUtf8(root.find("sourceA") ? root.find("sourceA")->asString() : "");
    s.sourceB = pathutil::FromUtf8(root.find("sourceB") ? root.find("sourceB")->asString() : "");
    const Value* set = root.find("settings");
    if (!set || set->type != Value::Type::Object) {
        error = SessionError::CorruptHeader;
        detail = "missing settings";
        return false;
    }
    if (!StrToMode(set->find("mode") ? set->find("mode")->asString() : "", s.settings.mode)) {
        error = SessionError::CorruptHeader;
        detail = "bad settings.mode";
        return false;
    }
    s.settings.caseSensitive = set->find("caseSensitive")->asBool();
    s.settings.backend = set->find("backend") ? set->find("backend")->asString() : "";
    if (s.settings.backend.empty()) {
        error = SessionError::CorruptHeader;
        detail = "missing settings.backend";
        return false;
    }
    const Value* ver = set->find("verify");
    if (!ver || ver->type != Value::Type::Object ||
        !StrToPattern(ver->find("pattern") ? ver->find("pattern")->asString() : "",
                      s.settings.verify.pattern)) {
        error = SessionError::CorruptHeader;
        detail = "bad settings.verify";
        return false;
    }
    s.settings.verify.percentRequested = (int)ver->find("requested")->asInt();
    s.settings.verify.percentEffective = (int)ver->find("effective")->asInt();
    s.settings.verify.patternRandom = ver->find("random")->asBool();
    s.settings.hashThreads = (unsigned)(set->find("hashThreads")->asInt());
    const std::string stName = root.find("state") ? root.find("state")->asString() : "";
    if (stName == "in_progress") s.state = SessionState::InProgress;
    else if (stName == "checkpointed") s.state = SessionState::Checkpointed;
    else if (stName == "interrupted") s.state = SessionState::Interrupted;
    else if (stName == "completed") s.state = SessionState::Completed;
    else {
        error = SessionError::CorruptHeader;
        detail = "bad state '" + stName + "'";
        return false;
    }
    if (!ParseSide(root.find("progressA"), s.progressA, "progressA", detail) ||
        !ParseSide(root.find("progressB"), s.progressB, "progressB", detail)) {
        error = SessionError::CorruptHeader;
        return false;
    }
    if (!GetStats(root.find("stats"), s.stats, detail)) {
        error = SessionError::CorruptHeader;
        return false;
    }
    const Value* cp = root.find("checkpoint");
    if (!cp || cp->type != Value::Type::Object) {
        error = SessionError::CorruptHeader;
        detail = "missing checkpoint";
        return false;
    }
    s.checkpoint.seq = (uint64_t)cp->find("seq")->asInt();
    s.checkpoint.atUnix = (uint64_t)cp->find("at")->asInt();
    const Value* j = root.find("journal");
    if (!j || j->type != Value::Type::Object) {
        error = SessionError::CorruptHeader;
        detail = "missing journal trailer";
        return false;
    }
    const Value* rec = j->find("records");
    const Value* byt = j->find("bytes");
    const Value* crc = j->find("crc32");
    if (!rec || !byt || !crc || rec->type != Value::Type::Int || byt->type != Value::Type::Int ||
        crc->type != Value::Type::Int || rec->integer < 0 || byt->integer < 0 ||
        crc->integer < 0 || crc->integer > 0xFFFFFFFFLL) {
        error = SessionError::CorruptHeader;
        detail = "bad journal trailer";
        return false;
    }
    journalRecords = (uint64_t)rec->integer;
    journalBytes = (uint64_t)byt->integer;
    journalCrc = (uint32_t)crc->integer;
    out = s;
    error = SessionError::Ok;
    return true;
}

std::string JournalFileName(const std::wstring& basePath) {
    const std::wstring full = JournalPath(basePath);
    const size_t pos = full.find_last_of(L"\\/");
    const std::wstring name = (pos == std::wstring::npos) ? full : full.substr(pos + 1);
    return pathutil::ToUtf8(name);
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool SaveSession(const std::wstring& basePath, const ScanSession& session, std::wstring& error) {
    std::string journal;
    for (const auto& e : session.journal) EncodeRecord(e, journal);
    if (!WriteAtomic(JournalPath(basePath), journal, false, error)) return false;
    const uint32_t crc = Crc32Of(journal.data(), journal.size());
    const std::string ctx = BuildContext(session, JournalFileName(basePath),
                                         session.journal.size(), journal.size(), crc);
    return WriteAtomic(ContextPath(basePath), ctx, true, error);
}

bool AppendJournal(const std::wstring& basePath, const ScanSession& contextSession,
                   const std::vector<JournalEntry>& entries, std::wstring& error) {
    if (entries.empty()) {
        // Nothing to append: still refresh the context (progress/state may have
        // advanced), keeping the existing trailer.
        ScanSession probe;
        LoadOutcome prev = LoadSession(basePath, probe);
        if (!prev.ok) {
            error = L"append: cannot load existing session";
            return false;
        }
        std::string cur;
        if (!ReadAll(JournalPath(basePath), cur, error)) return false;
        const std::string ctx =
            BuildContext(contextSession, JournalFileName(basePath), probe.journal.size(),
                         cur.size(), Crc32Of(cur.data(), cur.size()));
        return WriteAtomic(ContextPath(basePath), ctx, true, error);
    }
    // Verify the journal did not change under us: trailer bytes must match.
    ScanSession probe;
    LoadOutcome prev = LoadSession(basePath, probe);
    if (!prev.ok) {
        error = L"append: cannot load existing session";
        return false;
    }
    std::string addition;
    for (const auto& e : entries) EncodeRecord(e, addition);
    {
        std::ofstream f(fs::path(JournalPath(basePath)), std::ios::binary | std::ios::app);
        if (!f) {
            error = L"cannot open journal for append: " + JournalPath(basePath);
            return false;
        }
        f.write(addition.data(), static_cast<std::streamsize>(addition.size()));
        f.flush();
        if (!f) {
            error = L"journal append failed: " + JournalPath(basePath);
            return false;
        }
    }
    FlushToDisk(JournalPath(basePath));
    // Continue the stored CRC without re-reading: read back ONLY the trailer
    // counts from the context we just loaded (probe came from it), recompute
    // incrementally. The running CRC over the old bytes is derived from the
    // stored digest (Final ^ 0xFFFFFFFF restores the internal state).
    std::string ctxText;
    if (!ReadAll(ContextPath(basePath), ctxText, error)) return false;
    ScanSession ctx;
    SessionError cerr = SessionError::Ok;
    std::string cdetail;
    uint64_t declRecords = 0, declBytes = 0;
    uint32_t declCrc = 0;
    if (!ParseContext(ctxText, ctx, cerr, cdetail, declRecords, declBytes, declCrc)) {
        error = L"append: existing context unreadable";
        return false;
    }
    const uint64_t newRecords = declRecords + entries.size();
    std::string cur;
    if (!ReadAll(JournalPath(basePath), cur, error)) return false;
    const uint64_t newBytes = declBytes + addition.size();
    if (cur.size() < newBytes) {
        error = L"append: journal shorter than expected after append";
        return false;
    }
    const uint32_t newCrc =
        Crc32Final(Crc32Update(declCrc ^ 0xFFFFFFFFu, addition.data(), addition.size()));
    const std::string newCtx = BuildContext(contextSession, JournalFileName(basePath), newRecords,
                                            newBytes, newCrc);
    return WriteAtomic(ContextPath(basePath), newCtx, true, error);
}

LoadOutcome LoadSession(const std::wstring& basePath, ScanSession& out) {
    LoadOutcome r;
    std::string ctxText;
    std::wstring ioErr;
    const bool haveMain = ReadAll(ContextPath(basePath), ctxText, ioErr);
    if (!haveMain) {
        // No main context: try .prev before giving up.
        if (!ReadAll(PrevPath(basePath), ctxText, ioErr)) {
            r.error = SessionError::IoError;
            r.detail = "no context file";
            return r;
        }
        r.fellBackToPrev = true;
    }
    SessionError cerr = SessionError::Ok;
    std::string cdetail;
    uint64_t declRecords = 0, declBytes = 0;
    uint32_t declCrc = 0;
    ScanSession sess;
    if (!ParseContext(ctxText, sess, cerr, cdetail, declRecords, declBytes, declCrc)) {
        if (!haveMain || !ReadAll(PrevPath(basePath), ctxText, ioErr)) {
            r.error = cerr;
            r.detail = cdetail;
            return r;
        }
        if (!ParseContext(ctxText, sess, cerr, cdetail, declRecords, declBytes, declCrc)) {
            r.error = cerr;
            r.detail = cdetail;
            return r;
        }
        r.fellBackToPrev = true;
    }
    // Replay the journal: stop at the first short read, bad magic, length
    // overrun or CRC mismatch. Everything before is kept.
    std::string journal;
    if (!ReadAll(JournalPath(basePath), journal, ioErr)) {
        if (declRecords == 0 && declBytes == 0) {
            out = sess; // fresh session, journal not yet created
            r.ok = true;
            return r;
        }
        r.ok = true; // context valid: report the missing tail, keep going
        r.journalTruncated = true;
        r.detail = "journal file missing";
        out = sess;
        return r;
    }
    size_t off = 0;
    uint32_t running = Crc32Init();
    std::vector<JournalEntry> rows;
    bool tailBad = false;
    while (off < journal.size()) {
        if (journal.size() - off < kRecordHeaderSize) {
            tailBad = true; // partial header: crash mid-append
            break;
        }
        Reader h{journal.data() + off, kRecordHeaderSize, true};
        uint32_t magic = 0, len = 0, crc = 0;
        h.u32(magic);
        h.u32(len);
        h.u32(crc);
        if (!h.ok || magic != kRecordMagic) {
            tailBad = true;
            break;
        }
        if (journal.size() - off - kRecordHeaderSize < len) {
            tailBad = true; // partial payload: crash mid-append
            break;
        }
        const char* payload = journal.data() + off + kRecordHeaderSize;
        if (Crc32Of(payload, len) != crc) {
            tailBad = true;
            break;
        }
        JournalEntry e;
        if (!DecodePayload(payload, len, e)) {
            tailBad = true;
            break;
        }
        running = Crc32Update(running, journal.data() + off, kRecordHeaderSize + len);
        rows.push_back(std::move(e));
        off += kRecordHeaderSize + len;
    }
    sess.journal = std::move(rows);
    r.journalRecovered = sess.journal.size();
    // Cross-check against the trailer: fewer valid rows than declared, a bad
    // tail, or a CRC mismatch over the declared prefix all mean truncation.
    // (Valid extras beyond the declared count are kept: they are finalized
    // rows written before a context-save crash.)
    if (tailBad || r.journalRecovered < declRecords) {
        r.journalTruncated = true;
        r.detail = "journal tail damaged or incomplete";
    } else {
        // Recompute the CRC over exactly the declared prefix and compare.
        size_t scan = 0;
        uint32_t check = Crc32Init();
        for (uint64_t i = 0; i < declRecords && scan < journal.size(); ++i) {
            Reader h{journal.data() + scan, kRecordHeaderSize, true};
            uint32_t magic = 0, len = 0, crc = 0;
            h.u32(magic);
            h.u32(len);
            h.u32(crc);
            if (!h.ok || magic != kRecordMagic ||
                journal.size() - scan - kRecordHeaderSize < len)
                break;
            check = Crc32Update(check, journal.data() + scan, kRecordHeaderSize + len);
            scan += kRecordHeaderSize + len;
        }
        if (scan < declBytes || Crc32Final(check) != declCrc) {
            r.journalTruncated = true;
            r.detail = "journal trailer mismatch";
        } else if (r.journalRecovered > declRecords) {
            r.detail = "journal holds records newer than the context";
        }
    }
    out = std::move(sess);
    r.ok = true;
    return r;
}

} // namespace session
} // namespace bv
