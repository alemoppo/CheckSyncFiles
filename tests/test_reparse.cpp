// Tests for Filesystem/ReparsePoint (tag resolution, target read,
// link creation/deletion) and for link classification (FileEntry ->
// FileResult propagation, snapshot round-trip). Symlink creation needs
// privileges or Developer Mode: those cases accept the documented privilege
// error and skip.

#include <filesystem>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Comparison/ClassifyUtil.h"
#include "Comparison/ScanMode.h"
#include "Filesystem/FileIndex.h"
#include "Filesystem/FileIndexSerializer.h"
#include "Filesystem/ReparsePoint.h"
#include "TestHarness.h"
#include "TestHelpers.h"

using namespace bv;
using namespace bv::testutil;
namespace fs = std::filesystem;

TEST("reparse: ordinary file and dir report None", [] {
    const std::wstring root = MakeTempDir();
    const std::wstring f = root + L"\\a.txt";
    const std::wstring d = root + L"\\sub";
    CHECK(WriteFileBytes(f, "x", 1));
    CHECK(fs::create_directory(d));
    CHECK(GetReparseKind(f, false) == ReparseKind::None);
    CHECK(GetReparseKind(d, true) == ReparseKind::None);
});

TEST("reparse: mklink junction resolves kind and target", [] {
    const std::wstring root = MakeTempDir();
    const std::wstring target = root + L"\\target";
    const std::wstring link = root + L"\\link";
    CHECK(fs::create_directory(target));
    CHECK(WriteFileBytes(target + L"\\t.txt", "data", 4));
    if (!CreateJunction(link, target)) return; // unsupported host: skip
    CHECK(GetReparseKind(link, true) == ReparseKind::Junction);
    std::wstring err;
    const std::wstring readBack = ReadLinkTarget(link, nullptr, &err);
    CHECK_MSG(readBack == target, "junction target round-trip");
});

TEST("reparse: CreateLink/DeleteLink junction round-trip", [] {
    const std::wstring root = MakeTempDir();
    const std::wstring target = root + L"\\target";
    const std::wstring link = root + L"\\j";
    CHECK(fs::create_directory(target));
    std::wstring err;
    CHECK_MSG(CreateLink(link, target, ReparseKind::Junction, err), "create junction");
    CHECK(GetReparseKind(link, true) == ReparseKind::Junction);
    CHECK(ReadLinkTarget(link) == target);
    CHECK_MSG(DeleteLink(link, true, err), "delete junction");
    CHECK(!fs::exists(link));
    CHECK(fs::exists(target)); // target untouched
});

TEST("reparse: file symlink round-trip (or privilege skip)", [] {
    const std::wstring root = MakeTempDir();
    const std::wstring target = root + L"\\real.txt";
    const std::wstring link = root + L"\\alias.txt";
    CHECK(WriteFileBytes(target, "hello", 5));
    std::wstring err;
    if (!CreateLink(link, target, ReparseKind::SymlinkFile, err)) {
        CHECK_MSG(err.find(L"privilegi") != std::wstring::npos, "privilege error text");
        return; // no rights on this host: skip
    }
    CHECK(GetReparseKind(link, false) == ReparseKind::SymlinkFile);
    CHECK(ReadLinkTarget(link) == target);
    CHECK_MSG(DeleteLink(link, false, err), "delete symlink");
    CHECK(!fs::exists(link));
    CHECK(ReadFileBytes(target) == "hello"); // target untouched
});

TEST("reparse: dir symlink kind (or privilege skip)", [] {
    const std::wstring root = MakeTempDir();
    const std::wstring target = root + L"\\realdir";
    const std::wstring link = root + L"\\aliasdir";
    CHECK(fs::create_directory(target));
    std::wstring err;
    if (!CreateLink(link, target, ReparseKind::SymlinkDir, err)) {
        CHECK_MSG(err.find(L"privilegi") != std::wstring::npos, "privilege error text");
        return;
    }
    CHECK(GetReparseKind(link, true) == ReparseKind::SymlinkDir);
    CHECK(ReadLinkTarget(link) == target);
    CHECK_MSG(DeleteLink(link, true, err), "delete dir symlink");
    CHECK(!fs::exists(link));
    CHECK(fs::exists(target));
});

namespace {

FileEntry LinkEntry(const std::wstring& rel, ReparseKind kind, const std::wstring& target,
                    bool isDir) {
    FileEntry e;
    e.relativePath = rel;
    e.isDirectory = isDir;
    e.attributes = (isDir ? FILE_ATTRIBUTE_DIRECTORY : 0) | FILE_ATTRIBUTE_REPARSE_POINT;
    e.reparseKind = kind;
    e.linkTarget = target;
    e.size = isDir ? 0 : target.size();
    return e;
}

FileEntry PlainFile(const std::wstring& rel, uint64_t size = 10) {
    FileEntry e;
    e.relativePath = rel;
    e.size = size;
    return e;
}

} // namespace

TEST("links: TryClassifyLinks ignores plain pairs", [] {
    const LinkDecision d =
        TryClassifyLinks(PlainFile(L"a.txt"), PlainFile(L"a.txt"), L"C:\\A", L"D:\\B");
    CHECK(!d.handled);
});

TEST("links: same target is identical (file and dir)", [] {
    LinkDecision d =
        TryClassifyLinks(LinkEntry(L"l", ReparseKind::SymlinkFile, L"C:\\t", false),
                         LinkEntry(L"l", ReparseKind::SymlinkFile, L"C:\\t", false),
                         L"C:\\A", L"D:\\B");
    CHECK(d.handled && d.identical && !d.identicalIsDir);
    d = TryClassifyLinks(LinkEntry(L"j", ReparseKind::Junction, L"C:\\t", true),
                         LinkEntry(L"j", ReparseKind::Junction, L"C:\\t", true),
                         L"C:\\A", L"D:\\B");
    CHECK(d.handled && d.identical && d.identicalIsDir);
});

TEST("links: mirrored absolute targets compare equal", [] {
    const LinkDecision d =
        TryClassifyLinks(LinkEntry(L"j", ReparseKind::Junction, L"C:\\A\\real", true),
                         LinkEntry(L"j", ReparseKind::Junction, L"D:\\B\\real", true),
                         L"C:\\A", L"D:\\B");
    CHECK(d.handled && d.identical && d.identicalIsDir);
});

TEST("links: different targets are ContentMismatch with digests", [] {
    const LinkDecision d =
        TryClassifyLinks(LinkEntry(L"j", ReparseKind::Junction, L"C:\\one", true),
                         LinkEntry(L"j", ReparseKind::Junction, L"C:\\two", true),
                         L"C:\\A", L"D:\\B");
    CHECK(d.handled && !d.identical);
    CHECK(d.row.status == Status::ContentMismatch);
    CHECK(d.row.hasHashSource && d.row.hasHashDest);
    CHECK(d.row.hashSource != d.row.hashDest);
    CHECK_EQ(d.row.sizeSource, 6ull);
    CHECK_EQ(d.row.sizeDest, 6ull);
});

TEST("links: unknown or mixed reparse is ReadError, never SizeMismatch", [] {
    // Plain file vs unreadable reparse: must not become a plain replace.
    LinkDecision d =
        TryClassifyLinks(LinkEntry(L"x", ReparseKind::None, L"", false),
                         LinkEntry(L"x", ReparseKind::Unknown, L"", false), L"C:\\A",
                         L"D:\\B");
    CHECK(d.handled && !d.identical);
    CHECK(d.row.status == Status::ReadError);
    // Supported link vs Other: unreadable pair, not a type mismatch.
    d = TryClassifyLinks(LinkEntry(L"y", ReparseKind::Junction, L"C:\\t", true),
                         LinkEntry(L"y", ReparseKind::Other, L"", true), L"C:\\A",
                         L"D:\\B");
    CHECK(d.handled && !d.identical);
    CHECK(d.row.status == Status::ReadError);
});

TEST("links: kind change is SizeMismatch, unsupported is ReadError", [] {
    LinkDecision d =
        TryClassifyLinks(LinkEntry(L"x", ReparseKind::Junction, L"C:\\t", true),
                         PlainFile(L"x", 5), L"C:\\A", L"D:\\B");
    CHECK(d.handled && !d.identical);
    CHECK(d.row.status == Status::SizeMismatch);
    d = TryClassifyLinks(LinkEntry(L"u", ReparseKind::Other, L"", false),
                         LinkEntry(L"u", ReparseKind::Other, L"", false), L"C:\\A", L"D:\\B");
    CHECK(d.handled && !d.identical);
    CHECK(d.row.status == Status::ReadError);
});

TEST("links: rebase maps inside-root targets, keeps the rest", [] {
    CHECK(RebaseLinkTargetForWrite(L"C:\\A\\real", L"C:\\A", L"D:\\B") == L"D:\\B\\real");
    CHECK(RebaseLinkTargetForWrite(L"..\\rel", L"C:\\A", L"D:\\B") == L"..\\rel");
    CHECK(RebaseLinkTargetForWrite(L"C:\\elsewhere", L"C:\\A", L"D:\\B") == L"C:\\elsewhere");
});

TEST("links: scan sees junction-only-in-A as Missing with kind", [] {
    const std::wstring a = MakeTempDir();
    const std::wstring b = MakeTempDir();
    const std::wstring target = a + L"\\real";
    CHECK(fs::create_directory(target));
    CHECK(WriteFileBytes(target + L"\\f.txt", "data", 4));
    std::wstring err;
    CHECK(CreateLink(a + L"\\j", target, ReparseKind::Junction, err));
    const auto r = RunScan(a, b, ScanMode::Presence);
    // Two rows: the junction itself plus the file inside its target.
    CHECK_EQ(r.results.problems.size(), 2ull);
    const FileResult* row = nullptr;
    for (const FileResult& p : r.results.problems) {
        if (p.relativePath == L"j") row = &p;
    }
    CHECK(row != nullptr);
    CHECK(row->status == Status::Missing);
    CHECK(row->isDirectory);
    CHECK(row->reparseKind == ReparseKind::Junction);
});

TEST("links: same junction both sides is identical, not descended", [] {
    const std::wstring a = MakeTempDir();
    const std::wstring b = MakeTempDir();
    CHECK(fs::create_directory(a + L"\\real"));
    CHECK(fs::create_directory(b + L"\\real"));
    CHECK(WriteFileBytes(a + L"\\real\\f.txt", "data", 4));
    CHECK(WriteFileBytes(b + L"\\real\\f.txt", "data", 4));
    std::wstring err;
    CHECK(CreateLink(a + L"\\j", a + L"\\real", ReparseKind::Junction, err));
    CHECK(CreateLink(b + L"\\j", b + L"\\real", ReparseKind::Junction, err));
    const auto r = RunScan(a, b, ScanMode::Presence);
    CHECK(r.results.problems.empty());
});

TEST("links: different junction targets are ContentMismatch without hashing", [] {
    const std::wstring a = MakeTempDir();
    const std::wstring b = MakeTempDir();
    CHECK(fs::create_directory(a + L"\\one"));
    CHECK(fs::create_directory(b + L"\\two"));
    CHECK(WriteFileBytes(a + L"\\one\\f.txt", "data", 4));
    CHECK(WriteFileBytes(b + L"\\two\\f.txt", "data", 4));
    std::wstring err;
    CHECK(CreateLink(a + L"\\j", a + L"\\one", ReparseKind::Junction, err));
    CHECK(CreateLink(b + L"\\j", b + L"\\two", ReparseKind::Junction, err));
    const auto r = RunScan(a, b, ScanMode::Presence);
    // The junction row plus the two differently-pathed inner files.
    CHECK_EQ(r.results.problems.size(), 3ull);
    const FileResult* row = nullptr;
    for (const FileResult& p : r.results.problems) {
        if (p.relativePath == L"j") row = &p;
    }
    CHECK(row != nullptr);
    CHECK(row->status == Status::ContentMismatch);
    CHECK(row->reparseKind == ReparseKind::Junction);
});

TEST("links: junction-only-in-B is Extra, junction-vs-dir is SizeMismatch", [] {
    const std::wstring a = MakeTempDir();
    const std::wstring b = MakeTempDir();
    CHECK(fs::create_directory(b + L"\\real"));
    CHECK(WriteFileBytes(b + L"\\real\\f.txt", "data", 4));
    std::wstring err;
    CHECK(CreateLink(b + L"\\j", b + L"\\real", ReparseKind::Junction, err));
    const auto r1 = RunScan(a, b, ScanMode::Presence);
    // The junction row plus the file inside its target.
    CHECK_EQ(r1.results.problems.size(), 2ull);
    const FileResult* extra = nullptr;
    for (const FileResult& p : r1.results.problems) {
        if (p.relativePath == L"j") extra = &p;
    }
    CHECK(extra != nullptr);
    CHECK(extra->status == Status::Extra);
    CHECK(extra->reparseKind == ReparseKind::Junction);
    // Same path, plain dir in A vs junction in B.
    CHECK(fs::create_directory(a + L"\\j"));
    const auto r2 = RunScan(a, b, ScanMode::Presence);
    const FileResult* mismatch = nullptr;
    for (const FileResult& p : r2.results.problems) {
        if (p.relativePath == L"j") mismatch = &p;
    }
    CHECK(mismatch != nullptr);
    CHECK(mismatch->status == Status::SizeMismatch);
});

TEST("links: snapshot round-trip preserves kind and target", [] {
    FileIndex idx(false);
    FileEntry e = LinkEntry(L"j", ReparseKind::Junction, L"C:\\target", true);
    idx.addEntry(std::move(e));
    const std::wstring snap = MakeTempDir() + L"\\s.bvsi";
    std::wstring err;
    CHECK(indexio::WriteSnapshot(snap, idx, L"C:\\root", err));
    FileIndex loaded(false);
    std::wstring rootOut;
    CHECK(indexio::ReadSnapshot(snap, loaded, rootOut, err));
    CHECK(rootOut == L"C:\\root");
    FileEntry got;
    CHECK(loaded.find(L"j", got));
    CHECK(got.reparseKind == ReparseKind::Junction);
    CHECK(got.linkTarget == L"C:\\target");
    CHECK(got.isDirectory);
});
