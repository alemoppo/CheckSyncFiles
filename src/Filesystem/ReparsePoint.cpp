#include "ReparsePoint.h"

#include <cstddef>
#include <cstring>
#include <vector>

#include "PathUtil.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

namespace bv {
namespace {

// Local copy of REPARSE_DATA_BUFFER: MinGW's winioctl.h only provides
// REPARSE_GUID_DATA_BUFFER. The layout below is the stable documented ABI.
// Reads of a GET payload NEVER dereference this struct directly: every field
// access goes through ParseReparsePayload, which validates each offset and
// length against the real `bytes` returned by DeviceIoControl (a truncated or
// non-conformant payload yields Unknown, never an overread). The struct is
// still used to CONSTRUCT the junction SET buffer (fully caller-controlled).
struct ReparseBuffer {
    uint32_t tag;
    uint16_t dataLen;
    uint16_t reserved;
    union {
        struct {
            uint16_t subOff;
            uint16_t subLen;
            uint16_t printOff;
            uint16_t printLen;
            uint32_t flags;
            wchar_t path[1];
        } symlink;
        struct {
            uint16_t subOff;
            uint16_t subLen;
            uint16_t printOff;
            uint16_t printLen;
            wchar_t path[1];
        } mount;
    };
};

// \\?\-prefixed absolute path for every Win32 boundary (see PathUtil).
std::wstring Prefixed(const std::wstring& abs) {
    return pathutil::AddLongPathPrefix(pathutil::NormalizeRoot(abs));
}

void SetError(std::wstring* out, const std::wstring& msg) {
    if (out) *out = msg;
}

// Handle to a link itself, never to its target.
HANDLE OpenLinkItself(const std::wstring& abs, DWORD access) {
    return CreateFileW(Prefixed(abs).c_str(), access,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING,
                       FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

bool ReadReparseBuffer(const std::wstring& abs, std::vector<uint8_t>& buf, DWORD& bytesOut,
                       std::wstring* error) {
    HANDLE h = OpenLinkItself(abs, 0);
    if (h == INVALID_HANDLE_VALUE) {
        SetError(error, L"apertura link fallita: " + abs);
        return false;
    }
    buf.resize(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    const BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buf.data(),
                                    static_cast<DWORD>(buf.size()), &bytesOut, nullptr);
    CloseHandle(h);
    if (!ok) {
        SetError(error, L"lettura reparse point fallita: " + abs);
        return false;
    }
    return true;
}

const wchar_t kNtPrefix[] = L"\\??\\";

// A [pathBase + off, pathBase + off + len) name slice must sit inside the
// `bytes` actually returned, on wchar boundaries. Lengths are Naming
// lengths in bytes (always even for UTF-16); odd or escaping values mean a
// truncated/non-conformant payload.
bool SliceInRange(size_t pathBase, uint16_t off, uint16_t len, size_t bytes) {
    if ((off & 1) != 0 || (len & 1) != 0) return false;
    const size_t o = off, l = len;
    return o + l >= o && pathBase + o + l <= bytes;
}

bool RangesValid(size_t pathBase, size_t bytes, uint16_t subOff, uint16_t subLen,
                 uint16_t printOff, uint16_t printLen) {
    return SliceInRange(pathBase, subOff, subLen, bytes) &&
           SliceInRange(pathBase, printOff, printLen, bytes);
}

// SubstituteName with any \??\ prefix stripped; PrintName fallback.
// Precondition: RangesValid for the same arguments (checked by
// ParseReparsePayload); never called on unverified offsets.
std::wstring ExtractTarget(const wchar_t* base, uint16_t subOff, uint16_t subLen,
                           uint16_t printOff, uint16_t printLen) {
    auto slice = [&](uint16_t off, uint16_t len) {
        return std::wstring(base + off / sizeof(wchar_t), len / sizeof(wchar_t));
    };
    std::wstring target = subLen > 0 ? slice(subOff, subLen) : slice(printOff, printLen);
    if (target.compare(0, 4, kNtPrefix) == 0) target.erase(0, 4);
    return target;
}

void ReadU16(const uint8_t* data, size_t bytes, size_t at, uint16_t& out) {
    out = 0;
    if (at + sizeof(uint16_t) <= bytes) memcpy(&out, data + at, sizeof(uint16_t));
}

} // namespace

bool ParseReparsePayload(const uint8_t* data, size_t bytes, bool isDirectory,
                         ReparseKind& kindOut, std::wstring& targetOut,
                         std::wstring* error) {
    kindOut = ReparseKind::Unknown;
    targetOut.clear();
    if (!data || bytes < sizeof(uint32_t)) {
        SetError(error, L"reparse payload troncato: tag illeggibile");
        return false;
    }
    uint32_t tag = 0;
    memcpy(&tag, data, sizeof(tag)); // alignment-safe: no struct dereference
    if (tag == IO_REPARSE_TAG_SYMLINK) {
        // tag(4) + dataLen(2) + reserved(2) + subOff/subLen/printOff/
        // printLen/flags(12): nothing below is readable with fewer bytes.
        constexpr size_t kFixed = 8 + 12;
        static_assert(offsetof(ReparseBuffer, symlink) + 12 == kFixed,
                      "symlink fixed header size");
        if (bytes < kFixed) {
            SetError(error, L"reparse payload troncato: header symlink incompleto");
            return false;
        }
        uint16_t subOff, subLen, printOff, printLen;
        ReadU16(data, bytes, 8, subOff);
        ReadU16(data, bytes, 10, subLen);
        ReadU16(data, bytes, 12, printOff);
        ReadU16(data, bytes, 14, printLen);
        if (!RangesValid(kFixed, bytes, subOff, subLen, printOff, printLen)) {
            SetError(error, L"reparse payload non conforme: nomi fuori range");
            return false;
        }
        kindOut = isDirectory ? ReparseKind::SymlinkDir : ReparseKind::SymlinkFile;
        targetOut = ExtractTarget(reinterpret_cast<const wchar_t*>(data + kFixed), subOff,
                                  subLen, printOff, printLen);
        return true;
    }
    if (tag == IO_REPARSE_TAG_MOUNT_POINT) {
        // tag(4) + dataLen(2) + reserved(2) + subOff/subLen/printOff/
        // printLen(8).
        constexpr size_t kFixed = 8 + 8;
        static_assert(offsetof(ReparseBuffer, mount) + 8 == kFixed,
                      "mount fixed header size");
        if (bytes < kFixed) {
            SetError(error, L"reparse payload troncato: header mount incompleto");
            return false;
        }
        uint16_t subOff, subLen, printOff, printLen;
        ReadU16(data, bytes, 8, subOff);
        ReadU16(data, bytes, 10, subLen);
        ReadU16(data, bytes, 12, printOff);
        ReadU16(data, bytes, 14, printLen);
        if (!RangesValid(kFixed, bytes, subOff, subLen, printOff, printLen)) {
            SetError(error, L"reparse payload non conforme: nomi fuori range");
            return false;
        }
        kindOut = ReparseKind::Junction;
        targetOut = ExtractTarget(reinterpret_cast<const wchar_t*>(data + kFixed), subOff,
                                  subLen, printOff, printLen);
        return true;
    }
    kindOut = ReparseKind::Other; // the tag alone classifies: no field reads
    return false;
}

ReparseKind GetReparseKind(const std::wstring& absPath, bool isDirectory,
                           std::wstring* error) {
    std::vector<uint8_t> buf;
    DWORD bytes = 0;
    if (!ReadReparseBuffer(absPath, buf, bytes, error)) {
        // ERROR_NOT_A_REPARSE_POINT means surely-plain; any other failure
        // (access, races, ...) leaves a possibly-unreadable reparse point.
        return GetLastError() == ERROR_NOT_A_REPARSE_POINT ? ReparseKind::None
                                                           : ReparseKind::Unknown;
    }
    // Every field read is validated against `bytes` inside
    // ParseReparsePayload: truncated payloads yield Unknown, never an
    // overread, and short-but-tagged payloads never reach the name fields.
    ReparseKind kind = ReparseKind::Unknown;
    std::wstring target;
    if (bytes > buf.size()) bytes = static_cast<DWORD>(buf.size());
    ParseReparsePayload(buf.data(), bytes, isDirectory, kind, target, error);
    return kind;
}

std::wstring ReadLinkTarget(const std::wstring& absPath, ReparseKind* kindOut,
                            std::wstring* error) {
    std::vector<uint8_t> buf;
    DWORD bytes = 0;
    if (!ReadReparseBuffer(absPath, buf, bytes, error)) {
        if (kindOut) *kindOut = ReparseKind::None;
        return {};
    }
    // Validated parse (same guarantees as GetReparseKind above). File vs dir
    // stays the caller's enumeration knowledge: without it, default by tag
    // only, exactly as before.
    ReparseKind kind = ReparseKind::Unknown;
    std::wstring target;
    if (bytes > buf.size()) bytes = static_cast<DWORD>(buf.size());
    if (!ParseReparsePayload(buf.data(), bytes, /*isDirectory=*/false, kind, target,
                             nullptr)) {
        if (kindOut) *kindOut = kind;
        if (kind == ReparseKind::Other) {
            SetError(error, L"reparse tag non supportato: " + absPath);
        } else {
            SetError(error, L"reparse point illeggibile: " + absPath);
        }
        return {};
    }
    if (kindOut) *kindOut = kind;
    return target;
}

bool CreateLink(const std::wstring& linkPath, const std::wstring& target, ReparseKind kind,
                std::wstring& error) {
    const std::wstring link = Prefixed(linkPath);
    if (kind == ReparseKind::SymlinkFile || kind == ReparseKind::SymlinkDir) {
        DWORD flags = (kind == ReparseKind::SymlinkDir) ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
        flags |= SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE; // no-op without Dev Mode
        if (CreateSymbolicLinkW(link.c_str(), target.c_str(), flags)) {
            error.clear();
            return true;
        }
        error = L"creazione symlink fallita (servono privilegi o Modalita sviluppatore): " +
                linkPath;
        return false;
    }
    if (kind == ReparseKind::Junction) {
        // No Win32 CreateJunction API: empty dir + FSCTL_SET_REPARSE_POINT.
        if (!CreateDirectoryW(link.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            error = L"creazione cartella per junction fallita: " + linkPath;
            return false;
        }
        // Byte-identical layout to mklink: raw names (lengths EXCLUDE the
        // terminator), a 2-byte gap between them and a trailing NUL, all
        // provided by the zeroed buffer; the filter rejects any other shape
        // (ERROR_INVALID_REPARSE_DATA).
        const std::wstring sub = std::wstring(kNtPrefix) + target;
        const std::wstring print = target;
        const DWORD subBytes = static_cast<DWORD>(sub.size() * sizeof(wchar_t));
        const DWORD printBytes = static_cast<DWORD>(print.size() * sizeof(wchar_t));
        const DWORD dataLen = 8 + subBytes + 2 + printBytes + 2;
        std::vector<uint8_t> buf(8 + dataLen, 0); // tag(4) + len(2) + reserved(2)
        auto* rb = reinterpret_cast<ReparseBuffer*>(buf.data());
        rb->tag = IO_REPARSE_TAG_MOUNT_POINT;
        rb->dataLen = static_cast<uint16_t>(dataLen);
        rb->reserved = 0;
        auto& m = rb->mount;
        m.subOff = 0;
        m.subLen = static_cast<uint16_t>(subBytes);
        m.printOff = static_cast<uint16_t>(subBytes + 2);
        m.printLen = static_cast<uint16_t>(printBytes);
        wchar_t* dst = m.path;
        memcpy(dst, sub.data(), subBytes);
        memcpy(reinterpret_cast<uint8_t*>(dst) + subBytes + 2, print.data(), printBytes);
        HANDLE h = OpenLinkItself(linkPath, GENERIC_WRITE);
        if (h == INVALID_HANDLE_VALUE) {
            error = L"apertura junction fallita: " + linkPath;
            RemoveDirectoryW(link.c_str());
            return false;
        }
        DWORD ignored = 0;
        const BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(),
                                        static_cast<DWORD>(buf.size()), nullptr, 0, &ignored,
                                        nullptr);
        const DWORD code = ok ? 0 : GetLastError();
        CloseHandle(h);
        if (!ok) {
            RemoveDirectoryW(link.c_str());
            error = L"scrittura junction fallita (codice " + std::to_wstring(code) + L"): " +
                    linkPath;
            return false;
        }
        error.clear();
        return true;
    }
    error = L"tipo di link non supportato per la creazione.";
    return false;
}

// Root-relative split of an absolute target inside `root` (both normalized,
// case-insensitive): "C:\A\real" under "C:\A" -> "real". Empty when outside.
std::wstring SplitInsideRoot(const std::wstring& target, const std::wstring& root) {
    const std::wstring nRoot = pathutil::NormalizeRoot(root);
    const std::wstring nTarget = pathutil::NormalizeRoot(target);
    if (nRoot.empty() || nTarget.empty()) return {};
    const std::wstring fRoot = pathutil::FoldForCompare(nRoot);
    const std::wstring fTarget = pathutil::FoldForCompare(nTarget);
    if (fTarget.size() <= fRoot.size()) return {};
    if (fTarget.compare(0, fRoot.size(), fRoot) != 0) return {};
    if (fTarget[fRoot.size()] != L'\\') return {};
    return nTarget.substr(nRoot.size() + 1);
}

std::wstring NormalizeLinkTargetForCompare(const std::wstring& target,
                                           const std::wstring& ownRoot) {
    const std::wstring rel = SplitInsideRoot(target, ownRoot);
    if (!rel.empty()) return std::wstring(L"<root>\\") + rel;
    return target;
}

std::wstring RebaseLinkTargetForWrite(const std::wstring& target,
                                      const std::wstring& fromRoot,
                                      const std::wstring& toRoot) {
    const std::wstring rel = SplitInsideRoot(target, fromRoot);
    if (rel.empty()) return target;
    const std::wstring nTo = pathutil::NormalizeRoot(toRoot);
    if (nTo.empty()) return target;
    return nTo + L"\\" + rel;
}

uint64_t StatLinkItself(const std::wstring& absPath) {
    HANDLE h = OpenLinkItself(absPath, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FILETIME wt{};
    const bool ok = GetFileTime(h, nullptr, nullptr, &wt);
    CloseHandle(h);
    if (!ok) return 0;
    return (static_cast<uint64_t>(wt.dwHighDateTime) << 32) | wt.dwLowDateTime;
}

bool ResolveLinkEntry(const std::wstring& absPath, bool isDirectory, ReparseKind& kind,
                      std::wstring& target, uint64_t& linkSize, uint64_t& linkMtime) {
    kind = GetReparseKind(absPath, isDirectory);
    if (kind != ReparseKind::SymlinkFile && kind != ReparseKind::SymlinkDir &&
        kind != ReparseKind::Junction)
        return false;
    target = ReadLinkTarget(absPath);
    if (target.empty()) return false;
    const std::string u8 = pathutil::ToUtf8(target);
    linkSize = isDirectory ? 0 : static_cast<uint64_t>(u8.size());
    linkMtime = StatLinkItself(absPath);
    return true;
}

bool DeleteLink(const std::wstring& linkPath, bool isDirectory, std::wstring& error) {
    const std::wstring link = Prefixed(linkPath);
    const BOOL ok = isDirectory ? RemoveDirectoryW(link.c_str()) : DeleteFileW(link.c_str());
    if (ok) {
        error.clear();
        return true;
    }
    error = L"eliminazione link fallita: " + linkPath;
    return false;
}

} // namespace bv
