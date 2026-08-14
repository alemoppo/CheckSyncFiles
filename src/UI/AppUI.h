#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "Comparison/ComparisonResult.h"
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

    bool init();
    void shutdown();
    void processEvents();
    void render(const bv::ScanOrchestrator::UiSnapshot& st);

    void OnMouseDown(int mx, int my);
    void OnKeyDown(unsigned int key, bool repeat);
    void OnTextInput(const char* text);
    bool isPointerOverList(float wx, float wy);
    void startScanFromUi();
    void onLoadSnapshot();
    // Copies the finished results out of the orchestrator once per run, so the
    // render loop never re-copies a large problem list on every repaint.
    void syncResultsCache(const bv::ScanOrchestrator::UiSnapshot& st);

    std::vector<const FileResult*> FilteredRows() const;
    void DrawResultsList(int yList, int listBottom);
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

    // Cached copy of the last completed results (updated by syncResultsCache).
    ResultSet uiResults_;
    bool resultsReadySeen_ = false;

    // All business state lives here; the view reads a per-frame snapshot.
    bv::ScanOrchestrator orch_;
};

} // namespace bv::ui
