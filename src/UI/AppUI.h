#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "Comparison/ComparisonResult.h"
#include "Profiling/DirTiming.h"
#include "ScanOrchestrator.h"

struct TTF_Font;

namespace bv::ui {

// SDL3 GUI for BackupVerifier (Phase 2). This class is ONLY the view layer: it
// renders the orchestrator state and forwards input events to it. All scan
// logic (inputs, worker thread, cancellation, results, export) lives in
// bv::ScanOrchestrator; nothing here runs a scan or touches business state.
//
// Threading model: the UI lives on the main thread (SDL event + render loop);
// the comparison runs on a separate worker thread owned by the orchestrator.
// The render loop reads a single lock-guarded snapshot per frame and repaints
// when dirty.
class AppUI {
public:
    AppUI() = default;
    ~AppUI() = default;

    // Blocks until the window is closed. Returns 0 on success, non-zero on
    // failure (e.g. SDL could not initialise).
    int run();

private:
    static constexpr Uint8 kFilterAll = 0;
    static constexpr Uint8 kFilterIdentical = 1;
    static constexpr Uint8 kFilterMissing = 2;
    static constexpr Uint8 kFilterExtra = 3;
    static constexpr Uint8 kFilterSize = 4;
    static constexpr Uint8 kFilterContent = 5;
    static constexpr Uint8 kFilterErrors = 6;
    static constexpr Uint8 kFilterTimings = 7;

    bool init();
    void shutdown();
    void processEvents();
    void render(const bv::ScanOrchestrator::UiSnapshot& st);

    void OnMouseDown(int mx, int my);
    void OnRightClick(int mx, int my);
    // Left-click while the context menu is open: activates the hit item or
    // dismisses the menu. Always consumes the click.
    void OnContextMenuClick(int mx, int my);
    void DrawContextMenu();
    // Opens Explorer on the folder containing `path`, with `path` selected.
    static void OpenInExplorer(const std::wstring& path);
    void CloseContextMenu() {
        ctxOpen_ = false;
        ctxItems_.clear();
    }
    void OnKeyDown(unsigned int key, bool repeat);
    void OnTextInput(const char* text);
    bool isPointerOverList(float wx, float wy);
    void startScanFromUi();
    void onLoadSnapshot();
    // Copies the finished results out of the orchestrator once per run, so the
    // render loop never re-copies a large problem list on every repaint.
    void syncResultsCache(const bv::ScanOrchestrator::UiSnapshot& st);

    std::vector<const FileResult*> FilteredRows() const;
    // Rebuilds filteredCache_ from uiResults_ + filter_. Called only when the
    // results arrive or the filter changes, so per-frame rendering and mouse
    // handlers never rescan the whole problem list.
    void rebuildFilteredCache();
    void DrawResultsList(int yList, int listBottom);
    // "Tempistiche" view: slowest directories of the cached run, read-only
    // from uiDirTiming_ (never recomputed here). Dedicated drawing, separate
    // from DrawResultsList: different dataset, no scrolling model shared.
    void DrawTimings(int yList, int listBottom,
                     const bv::ScanOrchestrator::UiSnapshot& st);
    void DrawSummary(int summaryY, uint64_t hashingErrors);

    // hits ------------------------------------------------------------------
    bool hit(int mx, int my, const SDL_FRect& r) const;

    // UI state (main thread) ------------------------------------------------
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    TTF_Font* fontBody_ = nullptr;
    TTF_Font* fontBold_ = nullptr;

    int winW_ = 1000;
    int winH_ = 700;

    // The worker thread sets dirty_ through the orchestrator's progress
    // callback, so it is atomic (benign by nature, but tidy).
    std::atomic<bool> dirty_{true};
    bool quit_ = false;
    uint8_t filter_ = kFilterAll;
    int scroll_ = 0;

    // Fractional mouse-wheel delta accumulator: high-resolution wheel/trackpads
    // report deltas smaller than one tick, so the integer scroll steps are only
    // applied once a whole tick accumulates.
    float wheelAccum_ = 0.0f;

    // Live elapsed-time clock for the running operation: the tick is captured
    // on the first frame that observes the run, then counts up while running.
    Uint64 runStartTicks_ = 0;
    bool runStarted_ = false;
    // Last live byte count seen during the run (kept across the Hashing phase,
    // which reports candidates but no bytes), used for the live byte-rate.
    uint64_t lastLiveBytes_ = 0;

    // Draggable scrollbar state
    int scrollbarTrackX = 0;
    int scrollbarTrackY = 0;
    int scrollbarTrackH = 0;
    int scrollbarThumbY = 0;
    int scrollbarThumbH = 0;
    bool scrollbarDragging_ = false;
    bool sliderDragging_ = false; // verify-percent slider thumb drag
    int scrollbarDragStartY_ = 0;
    float scrollbarDragRatio_ = 0.0f;

    // Cursor position (in UTF-16 code units) inside the focused path field.
    // Lives here because the field text is stored in the orchestrator; this is
    // the only UI-side bit of the editing state.
    size_t caret_ = 0;
    // Classic text-field selection: the selected range is
    // [min(selAnchor_, caret_), max(selAnchor_, caret_)) in UTF-16 code units.
    // Collapsed (anchor == caret) means "no selection". Collapse it on focus
    // change, blur and Escape; both ends always sit on codepoint boundaries.
    size_t selAnchor_ = 0;
    bool HasSelection() const { return selAnchor_ != caret_; }
    void ClearSelection() { selAnchor_ = caret_; }
    // Erases the selected range from buf (ends clamped); caret collapses to
    // its start. Returns true when something was erased.
    bool EraseSelection(std::wstring& buf);
    // Left button held after a press inside a path field: motion extends.
    bool fieldDrag_ = false;
    // Last left-press inside a field, for double-click word selection.
    Uint64 lastClickTicks_ = 0;
    int lastClickField_ = 0; // 0 = none/outside, 1 = source, 2 = dest

    // Cached copy of the last completed results (updated by syncResultsCache).
    ResultSet uiResults_;
    bool resultsReadySeen_ = false;
    // Slowest-directories tops of the displayed run, cached once with the
    // results above (same hook, so a new run always replaces them and they
    // can never go stale). Plus the run-global hash-cache hit count.
    profiling::DirTimingReport uiDirTiming_;
    uint64_t uiHashCacheHits_ = 0;
    // Tempistiche view state (GUI-only): selected side (0 = A, 1 = B) and the
    // first visible content line of the panels (wheel scroll, own model).
    int timingSide_ = 0;
    int timingScroll_ = 0;
    // A/B roots used by the currently displayed results, captured when the
    // results arrive so the context-menu targets never follow later field
    // edits. A = source field, B = destination field.
    std::wstring resultsSourceRoot_;
    std::wstring resultsDestRoot_;
    // Whether the displayed results come from an offline/snapshot comparison
    // (captured from UiSnapshot::lastUsedSnapshot, which describes the run
    // that produced the results). The A side is then an index, not a live
    // filesystem, so "Apri A" is never offered.
    bool resultsOffline_ = false;

    // Right-click context menu over a result row ("Apri A/B in Esplora
    // risorse", "Riscansiona"). Only the items whose side path exists are
    // shown, so the menu stores resolved target paths (or the row's relative
    // path for a rescan), never row indices.
    struct CtxMenuItem {
        std::string labelUtf8;
        std::wstring targetPath; // explorer target; empty for a rescan item
        bool isRescan = false;
        std::wstring relPath; // row identity for a rescan item
    };
    // Forwards one single-file re-verification to the orchestrator with the
    // settings frozen at click time. Silently ignored when busy/offline.
    void RequestSingleVerify(const std::wstring& relPath);
    // Picks up a finished single-verify outcome (if any) and folds it into
    // uiResults_ (row replace/remove, stats fix, cache rebuild).
    void PollSingleVerify();
    bool ctxOpen_ = false;
    int ctxX_ = 0, ctxY_ = 0, ctxW_ = 0, ctxH_ = 0;
    std::vector<CtxMenuItem> ctxItems_;
    // Filtered view of uiResults_.problems, rebuilt only by rebuildFilteredCache
    // (new results or filter change). Render + mouse handlers read this instead
    // of rescanning problems every frame/event.
    std::vector<const FileResult*> filteredCache_;

    // All business state lives here; the view reads a per-frame snapshot.
    bv::ScanOrchestrator orch_;
};

} // namespace bv::ui
