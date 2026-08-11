#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "Comparison/ComparisonResult.h"
#include "Comparison/ScanMode.h"
#include "ScanController.h"

struct TTF_Font;

namespace bv::ui {

// SDL3 GUI for BackupVerifier (Phase 2).
//
// Threading model: the UI lives on the main thread (SDL event + render loop);
// a comparison runs on a separate worker thread. Progress and the final result
// are copied into UI state under a mutex; the render loop repaints when dirty.
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
    void render();
    void layoutAll();

    void OnMouseDown(int mx, int my);
    void OnKeyDown(unsigned int key, bool repeat);
    void OnTextInput(const char* text);
    bool isPointerOverList(float wx, float wy);
    void startScan();
    void stopScan();
    void workerThread(ScanOptions options);
    unsigned int threadToCount() const; // threadSel_ -> pool size (0 = auto)

    std::vector<const FileResult*> FilteredRows() const;
    void DrawResultsList(int yList, int listBottom);
    void DrawSummary(int summaryY);

    // hits ------------------------------------------------------------------
    bool hit(int mx, int my, const SDL_FRect& r) const;

    // UI state (main thread) ------------------------------------------------
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    TTF_Font* fontBody_ = nullptr;
    TTF_Font* fontBold_ = nullptr;

    int winW_ = 1000;
    int winH_ = 700;

    std::wstring source_;
    std::wstring dest_;
    bool sourceFocus_ = false;
    bool destFocus_ = false;

    ScanMode mode_ = ScanMode::Presence;
    int threadSel_ = 0; // 0=Auto,1,2,4,8,16
    bool caseSensitive_ = false; // path matching case policy (default: insensitive)
    EnumeratorBackend backend_ = EnumeratorBackend::Auto; // enumeration back-end

    // private, locked by mtx_ -----------------------------------------------
    std::mutex mtx_;
    bool running_ = false;
    std::atomic_bool cancel_{false};
    std::thread worker_;
    ScanProgress progress_;
    bool resultsReady_ = false;
    ResultSet results_;
    unsigned int threadCountUsed_ = 0; // hash workers actually launched
    double lastSecondsTotal_ = 0.0;    // duration of the last completed scan

    // render-state ----------------------------------------------------------
    bool dirty_ = true;
    bool quit_ = false;
    uint8_t filter_ = kFilterAll;
    int scroll_ = 0;

    // layout (recomputed on resize)
    int listTop_ = 0;
    int listBottom_ = 0;
    int listAreaW_ = 0;
    int rowH_ = 20;
};

} // namespace bv::ui
