#pragma once

// BVSS v1 persistent session store (Phase 0).
//
// A session lives in a sidecar PAIR next to the scan inputs:
//
//   <base>.bvss       context: JSON header (schema, ids, roots, settings,
//                      state, progress, stats, checkpoint, journal trailer)
//   <base>.bvj        journal: append-only binary records (finalized rows)
//   <base>.bvss.prev  last-good context (rotated on every atomic save)
//
// Rationale for the pair (instead of a single file): the journal is
// append-only and is NEVER rewritten in place, so a crash mid-append can only
// damage its tail (detected per-record via length+CRC32 and truncated); the
// context is rewritten atomically (tmp + flush + rename) on every checkpoint
// with the previous good copy kept as .prev. A crash therefore never destroys
// the last valid checkpoint: at worst the tail record is dropped.
//
// BVSS v1 context (JSON, UTF-8):
//   { "magic":"BVSS", "schema":1, "sessionId":"…", "createdAt":uint,
//     "sourceA":"…", "sourceB":"…",
//     "settings":{ "mode":"presence|size|content", "caseSensitive":bool,
//                  "backend":"auto|win32|mft",
//                  "verify":{ "requested":int, "effective":int,
//                             "pattern":"edges|center|random", "random":bool },
//                  "hashThreads":uint },
//     "state":"in_progress|checkpointed|interrupted|completed",
//     "progressA":{ "files":uint, "dirs":uint, "bytes":uint },
//     "progressB":{…}, "stats":{…19 counters…},
//     "checkpoint":{ "seq":uint, "at":uint },
//     "runMillis":uint,   // cumulative wall time, optional on load (default 0)
//     "journal":{ "file":"<name>.bvj", "records":uint, "bytes":uint,
//                 "crc32":uint } }
//
// BVSS v1 journal record (all integers little-endian):
//   u32 magic 0x4A535642 ("BVSJ"), u32 payloadLen, u32 crc32(payload),
//   payload: u32 pathLen + UTF-8 path,
//            u8 verdict, u8 flags(bit0 hasHashA, bit1 hasHashB, bit2 isDir),
//            u8 pattern, s32 verifiedPercent,
//            u64 sizeA, u64 mtimeA, [32 hashA if flag],
//            u64 sizeB, u64 mtimeB, [32 hashB if flag]
//
// Loader policy: unknown JSON fields are ignored (additive growth); unknown
// schema > kSchemaVersion is rejected (VersionMismatch); a truncated or
// CRC-bad journal tail stops the replay and is reported via
// LoadOutcome::journalTruncated (recovered prefix is kept, never discarded).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Session/ScanSession.h"

namespace bv {
namespace session {

// File suffixes appended to the user-chosen base path.
inline constexpr wchar_t kContextSuffix[] = L".bvss";
inline constexpr wchar_t kJournalSuffix[] = L".bvj";
inline constexpr wchar_t kPrevSuffix[] = L".prev";

enum class SessionError {
    Ok = 0,
    IoError,        // file read/write/rename failed (detail carries the OS error)
    CorruptHeader,  // context is not valid JSON or misses required fields
    MagicMismatch,  // context magic != "BVSS" (not a session file)
    VersionMismatch, // schema newer than kSchemaVersion (or < 1)
};

struct LoadOutcome {
    bool ok = false;
    SessionError error = SessionError::Ok;
    std::string detail; // narrow/UTF-8 diagnostic
    // Journal recovery flags: a damaged TAIL is never fatal. The recovered
    // prefix is returned in session.journal; only a bad CONTEXT fails.
    bool journalTruncated = false; // tail record incomplete or CRC-bad, or
                                   // fewer valid records than declared
    size_t journalRecovered = 0;   // valid records replayed
    bool fellBackToPrev = false;   // main context unusable, .prev loaded
};

const char* SessionErrorName(SessionError e);

// Full save: writes <base>.bvj (journal bytes, atomic tmp+rename) then
// <base>.bvss (context incl. journal trailer, rotating the old context to
// .prev). Order matters: on crash the context is older-or-equal, so the
// loader may see journal extras (valid rows, reported) but never trusts a
// context newer than the journal without flagging truncation.
bool SaveSession(const std::wstring& basePath, const ScanSession& session,
                 std::wstring& error);

// Append `entries` to <base>.bvj (pure append + flush) and atomically refresh
// <base>.bvss from `contextSession` (same rows + new entries, updated
// progress/stats/checkpoint as passed in). The journal file is never
// re-read here (only its size is stat'ed); CRC continuation needs no re-read
// thanks to the trailer.
//
// CONTRACT: call AppendJournal only after bootstrapping the session files
// with SaveSession in the SAME run, and always pass every still-unflushed row
// in `entries` (the checkpoint pump keeps them queued across retries). Every
// byte past the declared journal size is then necessarily an orphan of this
// run's own earlier append, still present in `entries`: a longer-or-equal
// prefix match truncates and re-appends, anything else is an explicit error
// and the files are left untouched. Appending to a journal written by another
// run breaks this contract: the truncation above would become destructive.
bool AppendJournal(const std::wstring& basePath, const ScanSession& contextSession,
                   const std::vector<JournalEntry>& entries, std::wstring& error);

// Load: context (.bvss, else .bvss.prev with fellBackToPrev) + journal replay.
// Missing journal file with zero declared records loads cleanly (fresh
// session); otherwise the missing tail is reported as truncated.
LoadOutcome LoadSession(const std::wstring& basePath, ScanSession& out);

// Header-only load: reads and validates just the context (with .prev fallback
// like LoadSession) without touching the journal file. out.journal is always
// empty; use it to validate a session or read its roots/settings without
// paying for a full journal replay. Error and detail match LoadSession's for
// the same damaged context.
LoadOutcome PeekSessionHeader(const std::wstring& basePath, ScanSession& out);

// Path helpers (exposed for tests and diagnostics).
std::wstring ContextPath(const std::wstring& base);
std::wstring JournalPath(const std::wstring& base);
std::wstring PrevPath(const std::wstring& base);

// Strips a trailing ".bvss" picked up from a file dialog: the store appends
// its own suffixes to the base path, so both "sess" and "sess.bvss" resolve
// to the same session. Single implementation shared by CLI and GUI.
std::wstring StripSessionSuffix(std::wstring path);

// CRC32 (zlib polynomial 0xEDB88320): init/update/finalize split so journal
// appends can continue a stored digest without re-reading the file.
uint32_t Crc32Init();
uint32_t Crc32Update(uint32_t crc, const void* data, size_t n);
uint32_t Crc32Final(uint32_t crc);

} // namespace session
} // namespace bv
