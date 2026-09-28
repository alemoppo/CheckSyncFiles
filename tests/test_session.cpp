// ScanSession / BVSS session-store unit tests + 100k benchmark (Phase 0).
//
// Covers: model round-trip of every significant field, NeedsReverify matrix,
// resume-compatibility split (correctness vs performance settings), journal
// append, crash-safety (truncated record, bad CRC, corrupt header, bad magic,
// future schema, .prev fallback), CRC32 known vector, and a 100k-entry
// save/load benchmark baseline for future checkpoint work.
//
// JOURNAL FINALITY (see ScanSession.h): the store is verdict-agnostic; the
// discipline "journal only finalized rows, Missing/Extra only after a
// completed enumeration" belongs to the future engine (Phase 1), not to
// these tests.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Session/SessionJson.h"
#include "Session/SessionLogic.h"
#include "Session/SessionStore.h"
#include "TestHarness.h"

namespace fs = std::filesystem;
using namespace bv;
using namespace bv::session;

namespace {

// Self-contained temp-dir + raw-byte helpers (test_main.cpp has its own
// anonymous-namespace copies; TestHelpers.cpp is not linked into bv_tests).

struct TempDir {
    std::wstring path;
    TempDir() {
        static std::atomic<int> counter{0};
        const fs::path dir = fs::temp_directory_path() /
                             (L"bvsess_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                              std::to_wstring(counter.fetch_add(1)));
        fs::create_directories(dir);
        path = dir.wstring();
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(fs::path(path), ec);
    }
};

bool WriteRawBytes(const std::wstring& path, const char* data, size_t n) {
    std::ofstream f(fs::path(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data, static_cast<std::streamsize>(n));
    f.flush();
    return static_cast<bool>(f);
}

std::string ReadRawBytes(const std::wstring& path) {
    std::ifstream f(fs::path(path), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool RemoveOne(const std::wstring& path) {
    std::error_code ec;
    return fs::remove(fs::path(path), ec);
}

bool HasDuplicatePaths(const std::vector<JournalEntry>& v) {
    std::set<std::wstring> seen;
    for (const auto& e : v) {
        if (!seen.insert(e.relativePath).second) return true;
    }
    return false;
}

JournalEntry MakeEntry(const wchar_t* rel, Status verdict, bool withHashes,
                       bool isDir = false) {
    JournalEntry e;
    e.relativePath = rel;
    e.sizeA = 1000;
    e.mtimeA = 132000000000000000ULL;
    e.sizeB = 1000;
    e.mtimeB = 132000000000000000ULL;
    e.hasHashA = withHashes;
    e.hasHashB = withHashes;
    if (withHashes) {
        e.hashA.fill(0xAB);
        e.hashB.fill(0xCD);
    }
    e.verdict = verdict;
    e.verifiedPercent = 100;
    e.verifiedPattern = PartialPattern::Edges;
    e.isDirectory = isDir;
    return e;
}

JournalEntry MakeSeqEntry(int i) {
    wchar_t name[64];
    swprintf(name, 64, L"seq\\f%06d.dat", i);
    return MakeEntry(name, Status::Identical, (i % 2) == 0);
}

// Mutable DOM navigation for header-surgery tests (missing/wrong-typed fields,
// out-of-range values).
json::Value* FindMutable(json::Value& root, const char* key) {
    if (root.type != json::Value::Type::Object || !root.obj) return nullptr;
    auto it = root.obj->find(key);
    return it == root.obj->end() ? nullptr : &it->second;
}

ScanSession MakeRichSession() {
    ScanSession s;
    s.sessionId = GenerateSessionId();
    s.createdAtUnix = NowUnixSeconds();
    s.sourceA = L"D:\\Backup";
    s.sourceB = L"\\\\NAS\\Backup";
    s.settings.mode = ScanMode::Content;
    s.settings.caseSensitive = false;
    s.settings.backend = "auto";
    s.settings.verify.percentRequested = 50;
    s.settings.verify.percentEffective = 50;
    s.settings.verify.pattern = PartialPattern::Center;
    s.settings.verify.patternRandom = true;
    s.settings.hashThreads = 8;
    s.state = SessionState::Checkpointed;
    s.progressA = {100, 10, 100000};
    s.progressB = {90, 9, 90000};
    s.stats.sourceFiles = 100;
    s.stats.destFiles = 90;
    s.stats.identicalFiles = 80;
    s.stats.contentMismatch = 1;
    s.stats.missingFiles = 2;
    s.stats.extraFiles = 1;
    s.stats.readErrors = 1;
    s.stats.changedDuringScan = 1;
    s.stats.bytesSource = 100000;
    s.stats.bytesDest = 90000;
    s.checkpoint = {7, NowUnixSeconds()};
    s.runMillis = 123456;
    s.journal.push_back(MakeEntry(L"Foto\\img001.jpg", Status::Identical, true));
    JournalEntry cm = MakeEntry(L"Docs\\relazione.docx", Status::ContentMismatchPartial, true);
    cm.verifiedPercent = 50;
    cm.verifiedPattern = PartialPattern::Center;
    cm.sizeB = 1001; // same path, different content batch
    s.journal.push_back(cm);
    JournalEntry miss = MakeEntry(L"Vecchi\\sparito.txt", Status::Missing, false);
    miss.sizeB = 0;
    miss.mtimeB = 0;
    s.journal.push_back(miss);
    JournalEntry uni = MakeEntry(L"Foto\\日本語\\🎉.txt", Status::Identical, true);
    s.journal.push_back(uni);
    JournalEntry dir = MakeEntry(L"Foto\\2025", Status::Identical, false, true);
    dir.sizeA = dir.sizeB = 0;
    s.journal.push_back(dir);
    return s;
}

bool StatsEqual(const Stats& a, const Stats& b) {
    return a.sourceFiles == b.sourceFiles && a.sourceDirs == b.sourceDirs &&
           a.destFiles == b.destFiles && a.destDirs == b.destDirs &&
           a.identicalFiles == b.identicalFiles && a.identicalDirs == b.identicalDirs &&
           a.missingFiles == b.missingFiles && a.missingDirs == b.missingDirs &&
           a.extraFiles == b.extraFiles && a.extraDirs == b.extraDirs &&
           a.sizeMismatch == b.sizeMismatch && a.contentMismatch == b.contentMismatch &&
           a.identicalPartialFiles == b.identicalPartialFiles &&
           a.contentMismatchPartial == b.contentMismatchPartial &&
           a.readErrors == b.readErrors && a.accessDenied == b.accessDenied &&
           a.changedDuringScan == b.changedDuringScan && a.bytesSource == b.bytesSource &&
           a.bytesDest == b.bytesDest;
}

void CheckSessionEqual(const ScanSession& a, const ScanSession& b) {
    CHECK(a.schemaVersion == b.schemaVersion);
    CHECK(a.sessionId == b.sessionId);
    CHECK(a.createdAtUnix == b.createdAtUnix);
    CHECK(a.sourceA == b.sourceA);
    CHECK(a.sourceB == b.sourceB);
    CHECK(a.settings.mode == b.settings.mode);
    CHECK(a.settings.caseSensitive == b.settings.caseSensitive);
    CHECK(a.settings.backend == b.settings.backend);
    CHECK(a.settings.verify.percentRequested == b.settings.verify.percentRequested);
    CHECK(a.settings.verify.percentEffective == b.settings.verify.percentEffective);
    CHECK(a.settings.verify.pattern == b.settings.verify.pattern);
    CHECK(a.settings.verify.patternRandom == b.settings.verify.patternRandom);
    CHECK(a.settings.hashThreads == b.settings.hashThreads);
    CHECK(a.state == b.state);
    CHECK(a.progressA.files == b.progressA.files);
    CHECK(a.progressA.dirs == b.progressA.dirs);
    CHECK(a.progressA.bytes == b.progressA.bytes);
    CHECK(a.progressB.files == b.progressB.files);
    CHECK(a.progressB.dirs == b.progressB.dirs);
    CHECK(a.progressB.bytes == b.progressB.bytes);
    CHECK(StatsEqual(a.stats, b.stats));
    CHECK(a.checkpoint.seq == b.checkpoint.seq);
    CHECK(a.checkpoint.atUnix == b.checkpoint.atUnix);
    CHECK(a.runMillis == b.runMillis);
    CHECK(a.journal.size() == b.journal.size());
    for (size_t i = 0; i < a.journal.size() && i < b.journal.size(); ++i)
        CHECK(a.journal[i] == b.journal[i]);
}

} // namespace

TEST("session: crc32 known vector", [] {
    const char* v = "123456789";
    uint32_t crc = Crc32Update(Crc32Init(), v, 9);
    CHECK(Crc32Final(crc) == 0xCBF43926u);
    // Continuation equals one-shot.
    uint32_t half = Crc32Update(Crc32Init(), v, 4);
    half = Crc32Update(half, v + 4, 5);
    CHECK(Crc32Final(half) == 0xCBF43926u);
});

TEST("session: round-trip preserves every significant field", [] {
    TempDir tmp;
    const std::wstring dir = tmp.path;
    const std::wstring base = dir + L"\\sess";
    const ScanSession src = MakeRichSession();
    std::wstring err;
    CHECK(SaveSession(base, src, err));
    ScanSession loaded;
    LoadOutcome o = LoadSession(base, loaded);
    CHECK(o.ok);
    CHECK(!o.journalTruncated);
    CHECK(!o.fellBackToPrev);
    CHECK(o.journalRecovered == src.journal.size());
    CheckSessionEqual(src, loaded);
});

TEST("session: NeedsReverify staleness matrix", [] {
    const JournalEntry e = MakeEntry(L"f.txt", Status::Identical, true);
    // Same size + same mtime on both sides: reusable.
    CHECK(!NeedsReverify(e, 1000, 132000000000000000ULL, 1000, 132000000000000000ULL));
    // Size differs (A side): stale.
    CHECK(NeedsReverify(e, 1001, 132000000000000000ULL, 1000, 132000000000000000ULL));
    // Mtime differs (A side, same size): stale. A saved digest alone must
    // never validate the row: the (size,mtime) version it was computed from
    // is gone.
    CHECK(NeedsReverify(e, 1000, 132000000000000001ULL, 1000, 132000000000000000ULL));
    // Both differ: stale.
    CHECK(NeedsReverify(e, 500, 131000000000000000ULL, 700, 131000000000000000ULL));
    // B side changed only: stale as well (verdict covers the pair).
    CHECK(NeedsReverify(e, 1000, 132000000000000000ULL, 1000, 132000000000000001ULL));
});

TEST("session: resume-compatibility splits correctness from performance", [] {
    ScanSettings saved;
    saved.mode = ScanMode::Content;
    saved.caseSensitive = false;
    saved.backend = "auto";
    saved.verify.percentEffective = 100;
    saved.hashThreads = 4;
    // Identical: compatible.
    CHECK(CheckResumeCompatible(saved, saved).compatible);
    // Thread count is performance-only: never invalidates.
    ScanSettings retuned = saved;
    retuned.hashThreads = 32;
    CHECK(CheckResumeCompatible(saved, retuned).compatible);
    // Correctness-relevant changes invalidate, each with a reason.
    ScanSettings m = saved;
    m.mode = ScanMode::Size;
    CompatResult r = CheckResumeCompatible(saved, m);
    CHECK(!r.compatible);
    CHECK(!r.reason.empty());
    m = saved;
    m.caseSensitive = true;
    CHECK(!CheckResumeCompatible(saved, m).compatible);
    m = saved;
    m.backend = "win32";
    CHECK(!CheckResumeCompatible(saved, m).compatible);
    m = saved;
    m.verify.percentEffective = 50;
    CHECK(!CheckResumeCompatible(saved, m).compatible);
    // Pattern matters only for partial verification (effective < 100):
    // full-content rows stay compatible across patterns.
    m = saved;
    m.verify.pattern = PartialPattern::Center;
    CHECK(CheckResumeCompatible(saved, m).compatible);
    ScanSettings partial = saved;
    partial.verify.percentEffective = 50;
    partial.verify.pattern = PartialPattern::Edges;
    ScanSettings partialOther = partial;
    partialOther.verify.pattern = PartialPattern::Center;
    CHECK(!CheckResumeCompatible(partial, partialOther).compatible);
});

TEST("session: journal append concatenates", [] {
    TempDir tmp;
    const std::wstring dir = tmp.path;
    const std::wstring base = dir + L"\\sess";
    ScanSession s = MakeRichSession();
    const size_t first = 2;
    std::vector<JournalEntry> head(s.journal.begin(), s.journal.begin() + first);
    std::vector<JournalEntry> tail(s.journal.begin() + first, s.journal.end());
    s.journal = head;
    std::wstring err;
    CHECK(SaveSession(base, s, err));
    s.journal.insert(s.journal.end(), tail.begin(), tail.end());
    s.checkpoint.seq = 8;
    CHECK(AppendJournal(base, s, tail, err));
    ScanSession loaded;
    LoadOutcome o = LoadSession(base, loaded);
    CHECK(o.ok);
    CHECK(!o.journalTruncated);
    CHECK(o.journalRecovered == s.journal.size());
    CHECK(loaded.checkpoint.seq == 8u);
    CheckSessionEqual(s, loaded);
});

TEST("session: truncated tail record is dropped, prefix kept", [] {
    TempDir tmp;
    const std::wstring dir = tmp.path;
    const std::wstring base = dir + L"\\sess";
    const ScanSession src = MakeRichSession();
    std::wstring err;
    CHECK(SaveSession(base, src, err));
    // Simulate a crash mid-append: cut the journal 10 bytes short.
    std::string bytes = ReadRawBytes(JournalPath(base));
    CHECK(!bytes.empty());
    CHECK(WriteRawBytes(JournalPath(base), bytes.data(), bytes.size() - 10));
    ScanSession loaded;
    LoadOutcome o = LoadSession(base, loaded);
    CHECK(o.ok); // tail damage is never fatal
    CHECK(o.journalTruncated);
    CHECK(o.journalRecovered == src.journal.size() - 1);
    for (size_t i = 0; i < loaded.journal.size(); ++i) CHECK(loaded.journal[i] == src.journal[i]);
});

TEST("session: bad CRC stops the replay at the damaged record", [] {
    TempDir tmp;
    const std::wstring dir = tmp.path;
    const std::wstring base = dir + L"\\sess";
    const ScanSession src = MakeRichSession();
    std::wstring err;
    CHECK(SaveSession(base, src, err));
    // Corrupt one payload byte in the middle of the journal.
    std::string bytes = ReadRawBytes(JournalPath(base));
    CHECK(bytes.size() > 64);
    bytes[bytes.size() / 2] ^= 0xFF;
    CHECK(WriteRawBytes(JournalPath(base), bytes.data(), bytes.size()));
    ScanSession loaded;
    LoadOutcome o = LoadSession(base, loaded);
    CHECK(o.ok);
    CHECK(o.journalTruncated);
    CHECK(o.journalRecovered < src.journal.size());
    for (size_t i = 0; i < loaded.journal.size(); ++i) CHECK(loaded.journal[i] == src.journal[i]);
});

TEST("session: corrupt header fails, .prev fallback recovers", [] {
    TempDir tmp;
    const std::wstring dir = tmp.path;
    const std::wstring base = dir + L"\\sess";
    ScanSession src = MakeRichSession();
    std::wstring err;
    CHECK(SaveSession(base, src, err));
    src.checkpoint.seq = 99;
    CHECK(SaveSession(base, src, err)); // rotates a good .prev
    // Destroy the main context only.
    const char junk[] = "{not valid json";
    CHECK(WriteRawBytes(ContextPath(base), junk, sizeof(junk) - 1));
    ScanSession loaded;
    LoadOutcome o = LoadSession(base, loaded);
    CHECK(o.ok);
    CHECK(o.fellBackToPrev);
    CHECK(loaded.checkpoint.seq != 99u); // previous good generation
    CHECK(o.journalRecovered == loaded.journal.size());
    // Destroy the backup too: now it must fail as corrupt header.
    CHECK(WriteRawBytes(PrevPath(base), junk, sizeof(junk) - 1));
    LoadOutcome o2 = LoadSession(base, loaded);
    CHECK(!o2.ok);
    CHECK(o2.error == SessionError::CorruptHeader);
});

TEST("session: bad magic and future schema are rejected explicitly", [] {
    TempDir tmp;
    const std::wstring dir = tmp.path;
    const std::wstring base = dir + L"\\sess";
    const ScanSession src = MakeRichSession();
    std::wstring err;
    CHECK(SaveSession(base, src, err));
    std::string ctx = ReadRawBytes(ContextPath(base));
    CHECK(!ctx.empty());
    // Bad magic.
    std::string badMagic = ctx;
    const size_t mp = badMagic.find("\"BVSS\"");
    CHECK(mp != std::string::npos);
    badMagic.replace(mp, 6, "\"XXXX\"");
    CHECK(WriteRawBytes(ContextPath(base), badMagic.data(), badMagic.size()));
    // Best-effort .prev removal so no fallback can mask the error (a single
    // save may not have created one yet).
    RemoveOne(PrevPath(base));
    ScanSession loaded;
    LoadOutcome o = LoadSession(base, loaded);
    CHECK(!o.ok);
    CHECK(o.error == SessionError::MagicMismatch);
    // Future schema.
    std::string future = ctx;
    const size_t sp = future.find("\"schema\":1");
    CHECK(sp != std::string::npos);
    future.replace(sp, 10, "\"schema\":999"); // `"schema":1` is 10 chars
    CHECK(WriteRawBytes(ContextPath(base), future.data(), future.size()));
    LoadOutcome o2 = LoadSession(base, loaded);
    CHECK(!o2.ok);
    CHECK(o2.error == SessionError::VersionMismatch);
});

TEST("session: missing journal with zero records loads cleanly", [] {
    TempDir tmp;
    const std::wstring dir = tmp.path;
    const std::wstring base = dir + L"\\sess";
    ScanSession src = MakeRichSession();
    src.journal.clear();
    std::wstring err;
    CHECK(SaveSession(base, src, err));
    CHECK(RemoveOne(JournalPath(base)));
    ScanSession loaded;
    LoadOutcome o = LoadSession(base, loaded);
    CHECK(o.ok);
    CHECK(!o.journalTruncated);
    CHECK(loaded.journal.empty());
});

TEST("session: session ids are unique", [] {
    CHECK(GenerateSessionId() != GenerateSessionId());
});

TEST("session: required header fields are strict, unknown ones ignored", [] {
    TempDir tmp;
    const std::wstring base = tmp.path + L"\\sess";
    ScanSession src = MakeRichSession();
    std::wstring err;
    CHECK(SaveSession(base, src, err));
    const std::string ctx = ReadRawBytes(ContextPath(base));
    CHECK(!ctx.empty());
    RemoveOne(PrevPath(base));
    auto tryMutant = [&](auto mutate, bool expectOk) {
        json::Value v;
        std::string perr;
        CHECK(json::Parse(ctx, v, perr));
        mutate(v);
        const std::string text = json::Write(v);
        CHECK(WriteRawBytes(ContextPath(base), text.data(), text.size()));
        ScanSession loaded;
        LoadOutcome o = LoadSession(base, loaded);
        CHECK(o.ok == expectOk);
        if (!expectOk) CHECK(o.error == SessionError::CorruptHeader);
        return o.ok;
    };
    // Missing or wrong-typed required fields are corruption...
    CHECK(!tryMutant([](json::Value& v) { CHECK(v.obj->erase("sourceA") == 1u); }, false));
    CHECK(!tryMutant(
        [](json::Value& v) { *FindMutable(v, "sourceB") = json::Value::Int(7); }, false));
    CHECK(!tryMutant([](json::Value& v) { CHECK(v.obj->erase("settings") == 1u); }, false));
    CHECK(!tryMutant(
        [](json::Value& v) {
            CHECK(FindMutable(*FindMutable(v, "settings"), "verify")->obj->erase("pattern") ==
                  1u);
        },
        false));
    CHECK(!tryMutant([](json::Value& v) { CHECK(v.obj->erase("state") == 1u); }, false));
    CHECK(!tryMutant([](json::Value& v) { CHECK(v.obj->erase("progressA") == 1u); }, false));
    CHECK(!tryMutant([](json::Value& v) { CHECK(v.obj->erase("checkpoint") == 1u); }, false));
    // ...while unknown future fields are ignored for extensibility.
    CHECK(tryMutant(
        [](json::Value& v) {
            (*v.obj)["futureField"] = json::Value::String("whatever");
            (*v.obj)["anotherOne"] = json::Value::Int(42);
        },
        true));
    // Restore the valid header: the last mutant left a corrupt one behind.
    CHECK(WriteRawBytes(ContextPath(base), ctx.data(), ctx.size()));
    ScanSession loaded;
    CHECK(LoadSession(base, loaded).ok);
    CHECK(loaded.sessionId == src.sessionId);
});

TEST("session: out-of-range settings are corrupt, not wrapped", [] {
    TempDir tmp;
    const std::wstring base = tmp.path + L"\\sess";
    ScanSession src = MakeRichSession(); // requested=50, effective=50, threads=8
    std::wstring err;
    CHECK(SaveSession(base, src, err));
    const std::string ctx = ReadRawBytes(ContextPath(base));
    CHECK(!ctx.empty());
    RemoveOne(PrevPath(base));
    // Each variant mutates a fresh parse; all must fail as CorruptHeader with
    // an "out of range" detail (narrowing casts used to wrap these silently).
    auto tryMutant = [&](auto mutate) {
        json::Value v;
        std::string perr;
        CHECK(json::Parse(ctx, v, perr));
        mutate(v);
        const std::string bad = json::Write(v);
        CHECK(WriteRawBytes(ContextPath(base), bad.data(), bad.size()));
        ScanSession loaded;
        LoadOutcome o = LoadSession(base, loaded);
        CHECK(!o.ok);
        CHECK(o.error == SessionError::CorruptHeader);
        CHECK(o.detail.compare(0, 12, "out of range") == 0);
    };
    tryMutant([](json::Value& v) { // effective=101
        *FindMutable(*FindMutable(*FindMutable(v, "settings"), "verify"), "effective") =
            json::Value::Int(101);
    });
    tryMutant([](json::Value& v) { // requested wraps to 100 as int32
        *FindMutable(*FindMutable(*FindMutable(v, "settings"), "verify"), "requested") =
            json::Value::Int(4294967396LL);
    });
    tryMutant([](json::Value& v) { // hashThreads=1e9
        *FindMutable(*FindMutable(v, "settings"), "hashThreads") =
            json::Value::Int(1000000000LL);
    });
    tryMutant([](json::Value& v) { // runMillis ~ 31600 years
        *FindMutable(v, "runMillis") = json::Value::Int(1000000000000000000LL);
    });
    // The valid header still loads (no over-strictness).
    CHECK(WriteRawBytes(ContextPath(base), ctx.data(), ctx.size()));
    ScanSession loaded;
    CHECK(LoadSession(base, loaded).ok);
});

TEST("session: header without runMillis loads with zero (old sessions)", [] {
    TempDir tmp;
    const std::wstring base = tmp.path + L"\\sess";
    ScanSession src = MakeRichSession();
    CHECK(src.runMillis != 0u);
    std::wstring err;
    CHECK(SaveSession(base, src, err));
    std::string ctx = ReadRawBytes(ContextPath(base));
    CHECK(!ctx.empty());
    json::Value v;
    std::string perr;
    CHECK(json::Parse(ctx, v, perr));
    CHECK(v.obj->erase("runMillis") == 1u);
    const std::string slim = json::Write(v);
    CHECK(WriteRawBytes(ContextPath(base), slim.data(), slim.size()));
    RemoveOne(PrevPath(base));
    ScanSession loaded;
    LoadOutcome o = LoadSession(base, loaded);
    CHECK(o.ok);
    CHECK(loaded.runMillis == 0u);
    CHECK(loaded.sessionId == src.sessionId);
});

TEST("session: valid JSON with a missing required field is corrupt, not a crash", [] {
    TempDir tmp;
    const std::wstring base = tmp.path + L"\\sess";
    ScanSession src = MakeRichSession();
    std::wstring err;
    CHECK(SaveSession(base, src, err));
    const std::string ctx = ReadRawBytes(ContextPath(base));
    CHECK(!ctx.empty());
    // Sanity: a parse+write round-trip without changes still loads.
    {
        json::Value v;
        std::string perr;
        CHECK(json::Parse(ctx, v, perr));
        CHECK(WriteRawBytes(ContextPath(base), json::Write(v).data(),
                            json::Write(v).size()));
        RemoveOne(PrevPath(base)); // no fallback masking below
        ScanSession reloaded;
        CHECK(LoadSession(base, reloaded).ok);
        CHECK(SaveSession(base, src, err)); // restore the valid header
    }
    // Each required field dropped individually must fail as CorruptHeader
    // (previously: null-pointer dereference on several of these).
    const std::vector<std::vector<std::string>> dropPaths = {
        {"createdAt"},
        {"settings", "caseSensitive"},
        {"settings", "verify", "requested"},
        {"settings", "verify", "effective"},
        {"settings", "verify", "random"},
        {"settings", "hashThreads"},
        {"checkpoint", "seq"},
        {"checkpoint", "at"},
    };
    for (const auto& path : dropPaths) {
        json::Value v;
        std::string perr;
        CHECK(json::Parse(ctx, v, perr));
        json::Value* parent = &v;
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            parent = FindMutable(*parent, path[i].c_str());
            CHECK(parent != nullptr);
        }
        CHECK(parent->obj->erase(path.back()) == 1u);
        const std::string slim = json::Write(v);
        CHECK(WriteRawBytes(ContextPath(base), slim.data(), slim.size()));
        RemoveOne(PrevPath(base)); // the .prev backup is still good: drop it
        ScanSession loaded;
        LoadOutcome o = LoadSession(base, loaded);
        CHECK(!o.ok);
        CHECK(o.error == SessionError::CorruptHeader);
    }
    // Wrong-typed values are corruption too, not silent defaults.
    {
        json::Value v;
        std::string perr;
        CHECK(json::Parse(ctx, v, perr));
        *FindMutable(v, "createdAt") = json::Value::String("yesterday");
        json::Value* cp = FindMutable(v, "checkpoint");
        CHECK(cp != nullptr);
        *FindMutable(*cp, "seq") = json::Value::Int(-5);
        json::Value* set = FindMutable(v, "settings");
        CHECK(set != nullptr);
        *FindMutable(*set, "caseSensitive") = json::Value::Int(1);
        const std::string bad = json::Write(v);
        CHECK(WriteRawBytes(ContextPath(base), bad.data(), bad.size()));
        RemoveOne(PrevPath(base));
        ScanSession loaded;
        LoadOutcome o = LoadSession(base, loaded);
        CHECK(!o.ok);
        CHECK(o.error == SessionError::CorruptHeader);
    }
});

TEST("session: checkpoint trigger predicate", [] {
    // Row-count trigger (0 disables).
    CHECK(!ShouldCheckpoint(9, 10, 1000, 0, 0));
    CHECK(ShouldCheckpoint(10, 10, 1000, 0, 0));
    CHECK(ShouldCheckpoint(25, 10, 1000, 0, 0));
    CHECK(!ShouldCheckpoint(1000000, 0, 1000, 0, 0));
    // Time trigger (0 disables; boundary is inclusive).
    CHECK(!ShouldCheckpoint(0, 0, 29, 0, 30));
    CHECK(ShouldCheckpoint(0, 0, 30, 0, 30));
    CHECK(ShouldCheckpoint(0, 0, 61, 31, 30));
    CHECK(!ShouldCheckpoint(0, 0, 1000, 0, 0));
    // Clock skew (now < last) never triggers.
    CHECK(!ShouldCheckpoint(0, 0, 50, 100, 30));
    // Either trigger suffices.
    CHECK(ShouldCheckpoint(10, 10, 0, 0, 3600));
    CHECK(ShouldCheckpoint(0, 100, 3600, 0, 3600));
});

TEST("session: benchmark 100k journal round-trip", [] {
    TempDir tmp;
    const std::wstring dir = tmp.path;
    const std::wstring base = dir + L"\\bench";
    ScanSession s;
    s.sessionId = GenerateSessionId();
    s.createdAtUnix = NowUnixSeconds();
    s.sourceA = L"D:\\Data";
    s.sourceB = L"E:\\Data";
    s.state = SessionState::InProgress;
    s.journal.reserve(100000);
    for (int i = 0; i < 100000; ++i) {
        JournalEntry e;
        wchar_t name[64];
        swprintf(name, 64, L"dir%05d\\file%05d.dat", i / 1000, i);
        e.relativePath = name;
        e.sizeA = e.sizeB = 1000 + (i % 4096);
        e.mtimeA = e.mtimeB = 132000000000000000ULL + i;
        e.hasHashA = e.hasHashB = true;
        e.hashA.fill(static_cast<uint8_t>(i & 0xFF));
        e.hashB.fill(static_cast<uint8_t>((i >> 8) & 0xFF));
        e.verdict = (i % 50 == 0) ? Status::ContentMismatch : Status::Identical;
        s.journal.push_back(e);
    }
    std::wstring err;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(SaveSession(base, s, err));
    const auto t1 = std::chrono::steady_clock::now();
    ScanSession loaded;
    LoadOutcome o = LoadSession(base, loaded);
    const auto t2 = std::chrono::steady_clock::now();
    CHECK(o.ok);
    CHECK(!o.journalTruncated);
    CHECK(o.journalRecovered == 100000u);
    CHECK(loaded.journal.size() == 100000u);
    CHECK(loaded.journal[0] == s.journal[0]);
    CHECK(loaded.journal[99999] == s.journal[99999]);
    CHECK(loaded.journal[42424] == s.journal[42424]);
    const auto saveMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    const auto loadMs = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    std::cout << "  [bench] 100k journal: save " << saveMs << " ms, load " << loadMs << " ms\n";
    CHECK(saveMs < 120000 && loadMs < 120000);
});

// ---------------------------------------------------------------------------
// AppendJournal crash-recovery (Section 1): an orphaned tail (journal bytes
// written by a previous append whose context save never landed) must be
// detected and reconciled against the still-pending rows, never duplicated.
// ---------------------------------------------------------------------------

TEST("session: append retry after failed context save", [] {
    TempDir tmp;
    const std::wstring base = tmp.path + L"\\sess";
    ScanSession s = MakeRichSession(); // 5 rows
    std::wstring err;
    CHECK(SaveSession(base, s, err));
    const std::string ctx0 = ReadRawBytes(ContextPath(base));
    CHECK(!ctx0.empty());
    RemoveOne(PrevPath(base));

    // First append lands on disk; then the context save "never happens".
    const std::vector<JournalEntry> X = {MakeSeqEntry(1000), MakeSeqEntry(1001)};
    ScanSession s2 = s;
    s2.checkpoint.seq = 8;
    CHECK(AppendJournal(base, s2, X, err));
    CHECK(WriteRawBytes(ContextPath(base), ctx0.data(), ctx0.size()));
    RemoveOne(PrevPath(base));

    // Retry with the still-pending X plus new Z (as doCheckpoint would):
    // the orphan must be recognized, not duplicated.
    const std::vector<JournalEntry> Z = {MakeSeqEntry(1002)};
    std::vector<JournalEntry> XZ = X;
    XZ.insert(XZ.end(), Z.begin(), Z.end());
    ScanSession s3 = s;
    s3.checkpoint.seq = 9;
    CHECK(AppendJournal(base, s3, XZ, err));

    ScanSession loaded;
    LoadOutcome o = LoadSession(base, loaded);
    CHECK(o.ok);
    CHECK(!o.journalTruncated);
    CHECK(o.detail.empty());
    const size_t expect = s.journal.size() + XZ.size();
    CHECK(o.journalRecovered == expect);
    CHECK(loaded.journal.size() == expect);
    for (size_t i = 0; i < s.journal.size(); ++i) CHECK(loaded.journal[i] == s.journal[i]);
    for (size_t i = 0; i < XZ.size(); ++i) CHECK(loaded.journal[s.journal.size() + i] == XZ[i]);
    CHECK(!HasDuplicatePaths(loaded.journal));
    CHECK(loaded.checkpoint.seq == 9u);
});

TEST("session: torn orphan tail", [] {
    TempDir tmp;
    const std::wstring base = tmp.path + L"\\sess";
    ScanSession s = MakeRichSession();
    std::wstring err;
    CHECK(SaveSession(base, s, err));
    const std::string ctx0 = ReadRawBytes(ContextPath(base));
    const std::string j0 = ReadRawBytes(JournalPath(base));
    CHECK(!j0.empty());
    RemoveOne(PrevPath(base));

    const std::vector<JournalEntry> X = {MakeSeqEntry(1000), MakeSeqEntry(1001)};
    ScanSession s2 = s;
    CHECK(AppendJournal(base, s2, X, err));
    const std::string j1 = ReadRawBytes(JournalPath(base));
    CHECK(j1.size() > j0.size());
    // Crash between context save and retry, plus a torn tail: keep only the
    // first half of the orphaned bytes.
    CHECK(WriteRawBytes(ContextPath(base), ctx0.data(), ctx0.size()));
    RemoveOne(PrevPath(base));
    const size_t tail = j1.size() - j0.size();
    CHECK(tail > 1u);
    std::error_code ec;
    fs::resize_file(fs::path(JournalPath(base)), j0.size() + tail / 2, ec);
    CHECK(!ec);

    // Same retry as T1.1 must converge to the same journal.
    const std::vector<JournalEntry> Z = {MakeSeqEntry(1002)};
    std::vector<JournalEntry> XZ = X;
    XZ.insert(XZ.end(), Z.begin(), Z.end());
    ScanSession s3 = s;
    s3.checkpoint.seq = 9;
    CHECK(AppendJournal(base, s3, XZ, err));

    ScanSession loaded;
    LoadOutcome o = LoadSession(base, loaded);
    CHECK(o.ok);
    CHECK(!o.journalTruncated);
    CHECK(o.detail.empty());
    const size_t expect = s.journal.size() + XZ.size();
    CHECK(o.journalRecovered == expect);
    CHECK(loaded.journal.size() == expect);
    for (size_t i = 0; i < s.journal.size(); ++i) CHECK(loaded.journal[i] == s.journal[i]);
    for (size_t i = 0; i < XZ.size(); ++i) CHECK(loaded.journal[s.journal.size() + i] == XZ[i]);
    CHECK(!HasDuplicatePaths(loaded.journal));
});

TEST("session: orphan tail that is not a prefix", [] {
    TempDir tmp;
    const std::wstring base = tmp.path + L"\\sess";
    ScanSession s = MakeRichSession();
    std::wstring err;
    CHECK(SaveSession(base, s, err));

    // Foreign bytes past the declared journal size (not rows from this run).
    const std::string before = ReadRawBytes(JournalPath(base));
    const std::string garbage(64, '\xAA');
    {
        std::ofstream f(fs::path(JournalPath(base)), std::ios::binary | std::ios::app);
        CHECK(static_cast<bool>(f));
        f.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
        f.flush();
        CHECK(static_cast<bool>(f));
    }
    const std::string withGarbage = ReadRawBytes(JournalPath(base));
    CHECK(withGarbage.size() == before.size() + garbage.size());

    const std::vector<JournalEntry> XZ = {MakeSeqEntry(1000), MakeSeqEntry(1001)};
    ScanSession s3 = s;
    std::wstring err2;
    CHECK(!AppendJournal(base, s3, XZ, err2));
    CHECK(!err2.empty());
    // The journal file must be byte-identical to before the call.
    CHECK(ReadRawBytes(JournalPath(base)) == withGarbage);
});

TEST("session: orphan tail with empty entries", [] {
    TempDir tmp;
    const std::wstring base = tmp.path + L"\\sess";
    ScanSession s = MakeRichSession();
    std::wstring err;
    CHECK(SaveSession(base, s, err));
    const std::string ctx0 = ReadRawBytes(ContextPath(base));
    RemoveOne(PrevPath(base));

    const std::vector<JournalEntry> X = {MakeSeqEntry(1000)};
    ScanSession s2 = s;
    CHECK(AppendJournal(base, s2, X, err));
    CHECK(WriteRawBytes(ContextPath(base), ctx0.data(), ctx0.size()));
    RemoveOne(PrevPath(base));

    // Orphaned rows exist but nothing is pending: refusing is the only safe
    // answer (those rows belong to nobody).
    const std::string journalBefore = ReadRawBytes(JournalPath(base));
    const std::string ctxBefore = ReadRawBytes(ContextPath(base));
    ScanSession s3 = s;
    std::wstring err2;
    CHECK(!AppendJournal(base, s3, {}, err2));
    CHECK(!err2.empty());
    CHECK(ReadRawBytes(JournalPath(base)) == journalBefore);
    CHECK(ReadRawBytes(ContextPath(base)) == ctxBefore);
});

TEST("session: append does not scale with journal size", [] {
    TempDir tmp;
    const std::wstring base = tmp.path + L"\\sess";
    ScanSession s = MakeRichSession();
    s.journal.clear();
    std::wstring err;
    CHECK(SaveSession(base, s, err));
    std::vector<double> ms;
    ms.reserve(200);
    int seq = 0;
    for (int a = 0; a < 200; ++a) {
        std::vector<JournalEntry> batch;
        for (int i = 0; i < 500; ++i) batch.push_back(MakeSeqEntry(seq++));
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(AppendJournal(base, s, batch, err));
        const auto t1 = std::chrono::steady_clock::now();
        ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    double first = 0.0, last = 0.0;
    for (int i = 0; i < 10; ++i) first += ms[i];
    for (int i = 190; i < 200; ++i) last += ms[i];
    first /= 10.0;
    last /= 10.0;
    std::cout << "  [bench] append avg ms: first10=" << first << " last10=" << last << "\n";
    // A correct append costs O(batch) + fixed file I/O, independent of the
    // journal size; re-reading/re-decoding the whole journal per append shows
    // up as a multi-x slowdown on the tail appends.
    CHECK(last < 3.0 * first + 20.0);
    ScanSession loaded;
    LoadOutcome o = LoadSession(base, loaded);
    CHECK(o.ok && !o.journalTruncated && o.journalRecovered == 100000u);
});

TEST("session: peek reads the header without the journal file", [] {
    TempDir tmp;
    const std::wstring base = tmp.path + L"\\sess";
    ScanSession src = MakeRichSession();
    std::wstring err;
    CHECK(SaveSession(base, src, err));
    // Delete the journal outright: Peek needs only the context...
    CHECK(RemoveOne(JournalPath(base)));
    ScanSession peeked;
    LoadOutcome po = PeekSessionHeader(base, peeked);
    CHECK(po.ok);
    CHECK(peeked.journal.empty());
    CHECK(peeked.sessionId == src.sessionId);
    CHECK(peeked.sourceA == src.sourceA);
    CHECK(peeked.sourceB == src.sourceB);
    // ...while a full load on the same base reports the missing tail.
    ScanSession loaded;
    LoadOutcome o = LoadSession(base, loaded);
    CHECK(o.ok);
    CHECK(o.journalTruncated);
});

TEST("session: peek and load agree on a corrupt header", [] {
    TempDir tmp;
    const std::wstring base = tmp.path + L"\\sess";
    ScanSession src = MakeRichSession();
    std::wstring err;
    CHECK(SaveSession(base, src, err));
    CHECK(SaveSession(base, src, err)); // second save rotates a good .prev
    const char junk[] = "{not valid json";
    CHECK(WriteRawBytes(ContextPath(base), junk, sizeof(junk) - 1));
    CHECK(WriteRawBytes(PrevPath(base), junk, sizeof(junk) - 1));
    ScanSession a, b;
    LoadOutcome po = PeekSessionHeader(base, a);
    LoadOutcome o = LoadSession(base, b);
    CHECK(!po.ok);
    CHECK(!o.ok);
    CHECK(po.error == o.error);
    CHECK(po.detail == o.detail);
});

TEST("session: empty append without orphans keeps a coherent trailer", [] {
    TempDir tmp;
    const std::wstring base = tmp.path + L"\\sess";
    ScanSession s = MakeRichSession();
    std::wstring err;
    CHECK(SaveSession(base, s, err));
    ScanSession s2 = s;
    s2.state = SessionState::Completed;
    s2.checkpoint.seq = 3;
    CHECK(AppendJournal(base, s2, {}, err));
    ScanSession loaded;
    LoadOutcome o = LoadSession(base, loaded);
    CHECK(o.ok);
    CHECK(!o.journalTruncated);
    CHECK(o.detail.empty());
    CHECK(o.journalRecovered == s.journal.size());
    CHECK(loaded.journal.size() == s.journal.size());
    for (size_t i = 0; i < s.journal.size(); ++i) CHECK(loaded.journal[i] == s.journal[i]);
    CHECK(loaded.state == SessionState::Completed);
    CHECK(loaded.checkpoint.seq == 3u);
});
