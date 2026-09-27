#pragma once

// Observer hook for finalized comparison rows (Phase 1: resumable sessions).
//
// A ClassifiedRow carries everything a verdict was computed from: the side
// observations (entries with size+mtime), the verdict itself, and the digests
// when content was actually read. The session layer converts rows into journal
// entries; the comparison engine never knows about sessions.
//
// Threading: rows are emitted from enumeration workers and hash-pool threads
// concurrently. IRowSink implementations must be thread-safe. BufferedRowSink
// below is the mutex-protected implementation used by ScanController; the
// hook is a nullable raw pointer so the hot path pays a single branch when
// nobody listens (session capture disabled).

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "Comparison/ComparisonResult.h"
#include "Comparison/ScanMode.h"
#include "Filesystem/FileEntry.h"

namespace bv {

// One finalized row: a verdict together with the exact input versions it was
// computed from. `relativePath` follows the FileResult convention (the
// destination-side spelling for matched pairs). Error rows (ReadError /
// AccessDenied / ChangedDuringScan) are captured too, with whatever
// fingerprints were available; the session layer never reuses them (they are
// always re-verified), they only serve a future complete re-save.
struct ClassifiedRow {
    std::wstring relativePath;
    bool hasA = false;
    bool hasB = false;
    FileEntry entryA; // valid iff hasA (size+mtime of the observed version)
    FileEntry entryB; // valid iff hasB
    Status verdict = Status::Identical;
    bool hasHashA = false;
    bool hasHashB = false;
    std::array<uint8_t, 32> hashA{};
    std::array<uint8_t, 32> hashB{};
    // Effective verify level applied to this row (Content mode, partial).
    int verifiedPercent = 100;
    PartialPattern verifiedPattern = PartialPattern::Edges;
    // Mirrors FileResult::isDirectory for the verdict (false for file/dir
    // type mismatches, which are reported as SizeMismatch).
    bool isDirectory = false;
};

// Row observer. Lifetime: must outlive the comparer run that uses it (the
// hash pool is drained before runImpl returns, so a stack-owned sink in the
// caller is sufficient).
class IRowSink {
public:
    virtual ~IRowSink() = default;
    virtual void onRow(ClassifiedRow row) = 0;
};

// Mutex-protected buffering sink. take() moves the rows out; call once after
// the run (same contract as ConcurrentSink::take).
class BufferedRowSink : public IRowSink {
public:
    void onRow(ClassifiedRow row) override {
        std::lock_guard<std::mutex> lock(mutex_);
        rows_.push_back(std::move(row));
    }
    std::vector<ClassifiedRow> take() {
        std::vector<ClassifiedRow> out;
        std::lock_guard<std::mutex> lock(mutex_);
        out.swap(rows_);
        return out;
    }
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return rows_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<ClassifiedRow> rows_;
};

} // namespace bv
