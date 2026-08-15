#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Comparison/ComparisonResult.h"
#include "Comparison/ScanMode.h"
#include "ScanController.h"

namespace bv {

// Owns the non-UI business state of the GUI: the scan inputs, the cancel flag,
// the worker thread, progress and the final results. AppUI renders this state
// and forwards events to it; nothing in here touches SDL (or any presentation
// layer), so the class can be exercised without a window.
//
// Threading model: the UI thread calls the command/input methods; a worker
// thread runs ScanController and publishes the results. All shared state is
// guarded by an internal mutex. The optional progress callback is invoked from
// the worker thread so a view can request a repaint.
class ScanOrchestrator {
public:
    // Everything the UI needs to draw one frame, read under a single lock.
    struct UiSnapshot {
        std::wstring source;
        std::wstring dest;
        bool sourceFocus = false;
        bool destFocus = false;
        ScanMode mode = ScanMode::Presence;
        bool caseSensitive = false;
        EnumeratorBackend backend = EnumeratorBackend::Auto;
        int threadSel = 0; // 0=Auto,1,2,4,8,16
        bool useSnapshot = false;
        std::wstring snapshotFile;

        bool running = false;
        bool resultsReady = false;
        bool cancelled = false; // last run was interrupted by the user
        // Last run outcome: whether each side was enumerated cleanly (mirrors
        // ScanReport::sourceOk / destinationOk). False when the side failed or
        // was cancelled; the UI must then never present the run as completed.
        bool sourceOk = true;
        bool destinationOk = true;
        // User-facing notes from the last run (e.g. back-end fallbacks, or an
        // incomplete scan explanation). Empty when there is nothing to say.
        std::vector<std::wstring> notes;
        ScanProgress progress;
        unsigned int threadCountUsed = 0; // hash workers actually launched
        double lastSecondsTotal = 0.0;    // duration of the last completed scan
        uint64_t hashingErrors = 0;       // worker exceptions during hashing
        bool lastSnapshotWritten = false;
        bool lastUsedSnapshot = false;
        bool lastDegraded = false;
        std::wstring statusNote;
    };

    ScanOrchestrator() = default;
    ~ScanOrchestrator();

    ScanOrchestrator(const ScanOrchestrator&) = delete;
    ScanOrchestrator& operator=(const ScanOrchestrator&) = delete;

    // Called from the worker thread whenever progress advances or a run ends;
    // the UI uses it to mark itself dirty. Optional.
    void setProgressCallback(std::function<void()> cb);

    // -- Inputs (UI edits) ---------------------------------------------------
    // `setSource` updates the text only; `useLiveSource` switches the source
    // side back to a live enumeration (used when the user browses a folder).
    void setSource(std::wstring s);
    void setDest(std::wstring s);
    void setSourceFocus(bool f);
    void setDestFocus(bool f);
    void setMode(ScanMode m);
    void setCaseSensitive(bool c);
    void setBackend(EnumeratorBackend b);
    void setThreadSel(int sel);
    void useLiveSource();

    // Offline mode: the source index is loaded from `file` (the source device
    // is not touched). `clearSnapshot` switches back to a live source.
    void loadSnapshot(std::wstring file);
    void clearSnapshot();

    // -- Commands -------------------------------------------------------------
    // startLiveScan handles both the live case (source + destination) and the
    // offline case (snapshot + destination), mirroring the previous UI logic.
    bool startLiveScan();
    // Captures the source index to `outFile` (the caller already picked it with
    // a save dialog). Returns false when no source is set.
    bool startSnapshotScan(const std::wstring& outFile);
    // Requests cancellation; the worker winds down on its own.
    void stop();
    // Exports the last results as CSV. Fails when no completed scan is present;
    // the status note carries the outcome in either case.
    bool exportCsv(const std::wstring& path);
    // Cancels and joins the worker thread. Safe to call at any time.
    void shutdown();

    // -- Test seams (null by default; never used by production code) ----------
    // Invoked by workerThread after the final state update (mtx_ released) and
    // immediately before notify(); lets a test park the worker inside the window
    // between "running_ is observable false" and "the thread physically exits".
    void setBeforeNotifyHook(std::function<void()> hook);
    // Invoked by startLiveScan()/startSnapshotScan() while mtx_ is held, after
    // input validation, just before the previous worker is reaped; lets a test
    // observe that a start acquired the lock. The hook must not acquire mtx_
    // (it is already held) and must not block indefinitely.
    void setStartLockedHook(std::function<void()> hook);

    // -- Read-only state ------------------------------------------------------
    UiSnapshot snapshot() const;
    // The completed results (moved into the snapshot's caller once, when a run
    // finishes; cheap to copy, kept out of UiSnapshot to bound per-frame cost).
    ResultSet results() const;

private:
    void workerThread(ScanOptions options);
    unsigned int threadToCount() const;
    // Copies the callback out of the lock and invokes it (the callback may run
    // on the worker thread; it must be cheap and never take this mutex).
    void notify();
    void resetForRunLocked();

    mutable std::mutex mtx_;
    std::function<void()> progressCb_;

    // Inputs.
    std::wstring source_;
    std::wstring dest_;
    bool sourceFocus_ = false;
    bool destFocus_ = false;
    ScanMode mode_ = ScanMode::Presence;
    bool caseSensitive_ = false;
    EnumeratorBackend backend_ = EnumeratorBackend::Auto;
    int threadSel_ = 0;
    bool useSnapshot_ = false;
    std::wstring snapshotFile_;

    // Run state.
    bool running_ = false;
    std::atomic_bool cancel_{false};
    std::thread worker_;
    ScanProgress progress_;
    bool resultsReady_ = false;
    ResultSet results_;
    unsigned int threadCountUsed_ = 0;
    double lastSecondsTotal_ = 0.0;
    bool lastSnapshotWritten_ = false;
    bool lastUsedSnapshot_ = false;
    bool lastDegraded_ = false;
    bool sourceOk_ = true;
    bool destinationOk_ = true;
    std::vector<std::wstring> notes_;
    uint64_t hashingErrors_ = 0;
    std::wstring lastSnapshotPath_;
    std::wstring statusNote_;

    std::function<void()> beforeNotifyHook_;
    std::function<void()> startLockedHook_;
};

} // namespace bv
