#include "UI/AppUI.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#ifdef WIN32_LEAN_AND_MEAN
#undef WIN32_LEAN_AND_MEAN
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "ScanController.h"
#include "UI/Utf.h"

namespace bv::ui {

namespace {

constexpr int kMargin = 12;
constexpr int kLabelW = 100;
constexpr int kFieldH = 28;
constexpr int kGap = 8;
constexpr int kRowH = 20;
constexpr int kBrowseW = 76;

// Geometry for the whole window, shared by render() (drawing) and
// OnMouseDown() (hit-testing) so the two can never drift apart.
struct Layout {
    int labelX = kMargin;
    int fieldX = 0;      // field left edge
    int fieldW = 0;      // field width
    int browseX = 0;     // browse button left edge
    int browseW = kBrowseW;

    int y1 = 0, y2 = 0, y3 = 0, y3b = 0, y4 = 0, y5 = 0;
    int y6 = 0, y7 = 0, y8 = 0, yList = 0, listBottom = 0, summaryY = 0;

    SDL_FRect sourceField, destField;
    SDL_FRect sourceBrowse, destBrowse;

    SDL_FRect startBtn, stopBtn, snapBtn, exportBtn, caricaBtn;
};

Layout ComputeLayout(int W, int H) {
    Layout L;
    const int titleH = 34;
    L.fieldX = kMargin + kLabelW;
    L.browseX = W - kMargin - kBrowseW;
    L.fieldW = L.browseX - L.fieldX - kGap;

    const SDL_FRect topBand{0, 0, static_cast<float>(W), static_cast<float>(titleH)};
    (void)topBand;

    L.y1 = kMargin + titleH;
    L.y2 = L.y1 + kFieldH + kGap;
    L.y3 = L.y2 + kFieldH + kGap + 8;
    L.y3b = L.y3 + 30; // back-end selection row
    L.y4 = L.y3b + 30;
    L.y5 = L.y4 + 30;
    L.y6 = L.y5 + 40;
    L.y7 = L.y6 + 30;
    L.y8 = L.y7 + 32;
    L.yList = L.y8 + 34;
    L.listBottom = H - 30;
    L.summaryY = H - 26;

    L.sourceField = {static_cast<float>(L.fieldX), static_cast<float>(L.y1),
                     static_cast<float>(L.fieldW), static_cast<float>(kFieldH)};
    L.destField = {static_cast<float>(L.fieldX), static_cast<float>(L.y2),
                   static_cast<float>(L.fieldW), static_cast<float>(kFieldH)};
    L.sourceBrowse = {static_cast<float>(L.browseX), static_cast<float>(L.y1),
                      static_cast<float>(kBrowseW), static_cast<float>(kFieldH)};
    L.destBrowse = {static_cast<float>(L.browseX), static_cast<float>(L.y2),
                    static_cast<float>(kBrowseW), static_cast<float>(kFieldH)};

    L.startBtn = {static_cast<float>(kMargin), static_cast<float>(L.y5), 120.0f, 30.0f};
    L.stopBtn = {static_cast<float>(kMargin + 130), static_cast<float>(L.y5), 120.0f, 30.0f};
    L.snapBtn = {static_cast<float>(kMargin + 260), static_cast<float>(L.y5), 120.0f, 30.0f};
    L.exportBtn = {static_cast<float>(kMargin + 390), static_cast<float>(L.y5), 120.0f, 30.0f};
    L.caricaBtn = {static_cast<float>(kMargin + 520), static_cast<float>(L.y5), 150.0f, 30.0f};
    return L;
}

// Off-white / dark theme colors.
struct RGBA {
    Uint8 r, g, b, a;
};
constexpr RGBA kBg{30, 30, 34, 255};
constexpr RGBA kPanel{46, 46, 52, 255};
constexpr RGBA kField{20, 20, 24, 255};
constexpr RGBA kBorder{90, 90, 100, 255};
constexpr RGBA kTextHi{235, 235, 240, 255};
constexpr RGBA kTextLo{170, 170, 180, 255};
constexpr RGBA kAccent{70, 120, 220, 255};
constexpr RGBA kAccentHover{90, 140, 235, 255};
constexpr RGBA kOk{110, 200, 120, 255};
constexpr RGBA kBad{230, 90, 90, 255};
constexpr RGBA kWarn{240, 170, 60, 255};

void FillRect(SDL_Renderer* ren, int x, int y, int w, int h, RGBA c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    const SDL_FRect r{static_cast<float>(x), static_cast<float>(y),
                      static_cast<float>(w), static_cast<float>(h)};
    SDL_RenderFillRect(ren, &r);
}

void DrawRect(SDL_Renderer* ren, int x, int y, int w, int h, RGBA c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    const SDL_FRect r{static_cast<float>(x), static_cast<float>(y),
                      static_cast<float>(w), static_cast<float>(h)};
    SDL_RenderRect(ren, &r);
}

TTF_Font* OpenFont(const char* path, float size) {
    return TTF_OpenFont(path, size);
}

std::string FindFont() {
    const char* cands[] = {
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/tahoma.ttf",
    };
    for (const char* c : cands) {
        if (GetFileAttributesA(c) != INVALID_FILE_ATTRIBUTES) return c;
    }
    return cands[1];
}

// Cached text textures. Text is rendered once per (font, color, string) and
// reused across frames instead of being re-uploaded to the GPU every repaint.
// This is the main way to cut GPU usage of the always-on renderer.
struct CachedText {
    TTF_Font* font;
    uint32_t color;
    std::string s;
    SDL_Texture* tex;
    int w, h;
};
std::vector<CachedText> g_textCache;

void FlushTextCache() {
    for (auto& e : g_textCache) {
        if (e.tex) SDL_DestroyTexture(e.tex);
    }
    g_textCache.clear();
}

// Returns a cached texture for (font, text, color); the caller must NOT destroy
// it. w/h are filled with the pixel size.
SDL_Texture* TextTextureCached(SDL_Renderer* ren, TTF_Font* font, const std::string& s,
                               SDL_Color col, int& w, int& h) {
    if (s.empty() || !font) {
        w = h = 0;
        return nullptr;
    }
    const uint32_t crgba = (static_cast<uint32_t>(col.r) << 24) |
                           (static_cast<uint32_t>(col.g) << 16) |
                           (static_cast<uint32_t>(col.b) << 8) | col.a;
    for (const auto& e : g_textCache) {
        if (e.font == font && e.color == crgba && e.s == s) {
            w = e.w;
            h = e.h;
            return e.tex;
        }
    }

    SDL_Surface* surf = TTF_RenderText_Blended(font, s.c_str(), s.size(), col);
    if (!surf) {
        w = h = 0;
        return nullptr;
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, surf);
    w = surf->w;
    h = surf->h;
    SDL_DestroySurface(surf);
    if (!tex) {
        w = h = 0;
        return nullptr;
    }

    if (g_textCache.size() > 512) FlushTextCache(); // crude cap, rare in practice
    g_textCache.push_back({font, crgba, s, tex, w, h});
    return tex;
}

// Top-left text (status/summary/title lines).
int DrawText(SDL_Renderer* ren, TTF_Font* font, const std::string& s, int x, int y, RGBA c) {
    SDL_Color col{c.r, c.g, c.b, c.a};
    int tw = 0, th = 0;
    SDL_Texture* t = TextTextureCached(ren, font, s, col, tw, th);
    if (!t) return 0;
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    const SDL_FRect d{static_cast<float>(x), static_cast<float>(y),
                      static_cast<float>(tw), static_cast<float>(th)};
    SDL_RenderTexture(ren, t, nullptr, &d);
    return tw;
}

// Draws text vertically centred inside a box [yBox, yBox+boxH], left aligned at x.
void DrawTextVCenter(SDL_Renderer* ren, TTF_Font* font, const std::string& s,
                     int x, int yBox, int boxH, RGBA c) {
    SDL_Color col{c.r, c.g, c.b, c.a};
    int tw = 0, th = 0;
    SDL_Texture* t = TextTextureCached(ren, font, s, col, tw, th);
    if (!t) return;
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    const int ty = yBox + (boxH - th) / 2;
    const SDL_FRect d{static_cast<float>(x), static_cast<float>(ty),
                      static_cast<float>(tw), static_cast<float>(th)};
    SDL_RenderTexture(ren, t, nullptr, &d);
}

// Draws text centred both horizontally and vertically inside a box.
void DrawTextCenterIn(SDL_Renderer* ren, TTF_Font* font, const std::string& s,
                      int xBox, int yBox, int boxW, int boxH, RGBA c) {
    SDL_Color col{c.r, c.g, c.b, c.a};
    int tw = 0, th = 0;
    SDL_Texture* t = TextTextureCached(ren, font, s, col, tw, th);
    if (!t) return;
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    const int tx = xBox + (boxW - tw) / 2;
    const int ty = yBox + (boxH - th) / 2;
    const SDL_FRect d{static_cast<float>(tx), static_cast<float>(ty),
                      static_cast<float>(tw), static_cast<float>(th)};
    SDL_RenderTexture(ren, t, nullptr, &d);
}

void DrawPickerButton(SDL_Renderer* ren, TTF_Font* font, const SDL_FRect& r, bool hover) {
    FillRect(ren, static_cast<int>(r.x), static_cast<int>(r.y),
             static_cast<int>(r.w), static_cast<int>(r.h), hover ? kAccentHover : kPanel);
    DrawRect(ren, static_cast<int>(r.x), static_cast<int>(r.y),
             static_cast<int>(r.w), static_cast<int>(r.h), kBorder);
    DrawTextCenterIn(ren, font, "Sfoglia", static_cast<int>(r.x), static_cast<int>(r.y),
                     static_cast<int>(r.w), static_cast<int>(r.h), kTextHi);
}

void DrawToggle(SDL_Renderer* ren, TTF_Font* font, const std::string& label,
                int x, int y, int w, int h, bool active) {
    FillRect(ren, x, y, w, h, active ? kAccent : kPanel);
    DrawRect(ren, x, y, w, h, kBorder);
    DrawTextCenterIn(ren, font, label, x, y, w, h, kTextHi);
}

void RemoveLastCodepoint(std::wstring& s) {
    if (s.empty()) return;
    if (s.size() >= 2 && s.back() >= 0xDC00 && s.back() <= 0xDFFF &&
        s[s.size() - 2] >= 0xD800 && s[s.size() - 2] <= 0xDBFF) {
        s.pop_back();
        s.pop_back();
    } else {
        s.pop_back();
    }
}

std::string Group(std::uint64_t v) {
    std::string s = std::to_string(v);
    std::string out;
    int count = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (count == 3) {
            out.push_back('.');
            count = 0;
        }
        out.push_back(*it);
        ++count;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::wstring FormatRateW(uint64_t bytes, double seconds) {
    if (seconds <= 0.0) return L"n/d";
    double v = static_cast<double>(bytes) / seconds;
    const wchar_t* units[] = {L"B/s", L"KB/s", L"MB/s", L"GB/s", L"TB/s"};
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    wchar_t buf[64];
    swprintf(buf, 64, L"%.2f %ls", v, units[u]);
    return buf;
}

const wchar_t* StatusName(bv::Status st) {
    switch (st) {
        case bv::Status::Identical: return L"IDENTICO";
        case bv::Status::Missing: return L"MANCANTE";
        case bv::Status::Extra: return L"EXTRA";
        case bv::Status::SizeMismatch: return L"DIM_DIVERSA";
        case bv::Status::ContentMismatch: return L"CONTENUTO_DIVERSO";
        case bv::Status::ReadError: return L"ERRORE_LETTURA";
        case bv::Status::AccessDenied: return L"ACCESSO_NEGATO";
        case bv::Status::ChangedDuringScan: return L"MODIFICATO_DURANTE_SCAN";
    }
    return L"?";
}

RGBA StatusColor(bv::Status st) {
    switch (st) {
        case bv::Status::Identical: return kOk;
        case bv::Status::Missing:
        case bv::Status::ContentMismatch:
        case bv::Status::AccessDenied: return kBad;
        case bv::Status::Extra:
        case bv::Status::SizeMismatch:
        case bv::Status::ChangedDuringScan: return kWarn;
        default: return kTextLo;
    }
}

// Modal folder-picker using the modern Windows IFileOpenDialog (COM).
bool BrowseFolder(std::wstring& out) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;

    bool ok = false;
    IFileOpenDialog* pfd = nullptr;
    const CLSID clsid = CLSID_FileOpenDialog;
    hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                          reinterpret_cast<void**>(&pfd));
    if (SUCCEEDED(hr)) {
        DWORD opts = 0;
        pfd->GetOptions(&opts);
        pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(pfd->Show(nullptr))) {
            IShellItem* psi = nullptr;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    out = path;
                    CoTaskMemFree(path);
                    ok = true;
                }
                psi->Release();
            }
        }
        pfd->Release();
    }
    CoUninitialize();
    return ok;
}

// Modal save-file dialog using the modern Windows IFileSaveDialog (COM).
bool BrowseSaveFile(std::wstring& out, const wchar_t* defName) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;

    bool ok = false;
    IFileSaveDialog* pfd = nullptr;
    hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IFileSaveDialog, reinterpret_cast<void**>(&pfd));
    if (SUCCEEDED(hr)) {
        pfd->SetFileName(defName);
        if (SUCCEEDED(pfd->Show(nullptr))) {
            IShellItem* psi = nullptr;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    out = path;
                    CoTaskMemFree(path);
                    ok = true;
                }
                psi->Release();
            }
        }
        pfd->Release();
    }
    CoUninitialize();
    return ok;
}

// Modal open-file dialog (modern Windows IFileOpenDialog, COM).
bool BrowseOpenFile(std::wstring& out, const wchar_t* filterName, const wchar_t* filterSpec) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;

    bool ok = false;
    IFileOpenDialog* pfd = nullptr;
    hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IFileOpenDialog, reinterpret_cast<void**>(&pfd));
    if (SUCCEEDED(hr)) {
        DWORD opts = 0;
        pfd->GetOptions(&opts);
        pfd->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
        COMDLG_FILTERSPEC filter{filterName, filterSpec};
        pfd->SetFileTypes(1, &filter);
        if (SUCCEEDED(pfd->Show(nullptr))) {
            IShellItem* psi = nullptr;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    out = path;
                    CoTaskMemFree(path);
                    ok = true;
                }
                psi->Release();
            }
        }
        pfd->Release();
    }
    CoUninitialize();
    return ok;
}

// Last path component (for showing a selected snapshot compactly).
std::wstring BaseName(const std::wstring& p) {
    const size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? p : p.substr(pos + 1);
}

} // namespace

int AppUI::run() {
    if (!init()) return 1;

    // The orchestrator notifies us on progress; the flag triggers a repaint.
    orch_.setProgressCallback([this] { dirty_.store(true); });

    while (!quit_) {
        processEvents();

        // The indeterminate progress bar animates continuously while running,
        // so keep repainting during a scan. Otherwise only repaint on demand
        // (state changes set dirty_) so an idle window costs ~0% GPU.
        bv::ScanOrchestrator::UiSnapshot st = orch_.snapshot();
        if (st.running) dirty_.store(true);
        if (dirty_.load()) {
            render(st);
        }
        SDL_Delay(10);
    }

    orch_.shutdown();
    shutdown();
    return 0;
}

bool AppUI::init() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return false;
    }
    if (!TTF_Init()) {
        SDL_Quit();
        return false;
    }

    window_ = SDL_CreateWindow("BackupVerifier — Verifica backup", 1000, 700,
                               SDL_WINDOW_RESIZABLE);
    if (!window_) {
        TTF_Quit();
        SDL_Quit();
        return false;
    }
    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        TTF_Quit();
        SDL_Quit();
        return false;
    }

    const std::string fontPath = FindFont();
    fontBody_ = OpenFont(fontPath.c_str(), 15.0f);
    fontBold_ = OpenFont(fontPath.c_str(), 17.0f);
    return true;
}

void AppUI::shutdown() {
    FlushTextCache();
    if (fontBold_) TTF_CloseFont(fontBold_);
    if (fontBody_) TTF_CloseFont(fontBody_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    TTF_Quit();
    SDL_Quit();
    fontBold_ = fontBody_ = nullptr;
    renderer_ = nullptr;
    window_ = nullptr;
}

bool AppUI::hit(int mx, int my, const SDL_FRect& r) const {
    return mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h;
}

void AppUI::processEvents() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_EVENT_QUIT:
                quit_ = true;
                orch_.stop(); // let the worker wind down before the join
                break;
            case SDL_EVENT_WINDOW_RESIZED:
                winW_ = ev.window.data1;
                winH_ = ev.window.data2;
                dirty_ = true;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                dirty_ = true; // refresh hover highlights
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    OnMouseDown(static_cast<int>(ev.button.x), static_cast<int>(ev.button.y));
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (ev.wheel.y != 0.0f && isPointerOverList(ev.wheel.x, ev.wheel.y)) {
                    scroll_ -= static_cast<int>(ev.wheel.y);
                    dirty_ = true;
                }
                break;
            case SDL_EVENT_KEY_DOWN:
                OnKeyDown(static_cast<unsigned int>(ev.key.key), ev.key.repeat);
                break;
            case SDL_EVENT_TEXT_INPUT:
                OnTextInput(ev.text.text);
                break;
            default:
                break;
        }
    }
}

void AppUI::OnMouseDown(int mx, int my) {
    const Layout L = ComputeLayout(winW_, winH_);

    const bv::ScanOrchestrator::UiSnapshot st = orch_.snapshot();
    const bool running = st.running;
    const bool offline = st.useSnapshot;

    // Browse buttons first (they should not steal focus).
    if (hit(mx, my, L.sourceBrowse)) {
        std::wstring picked;
        if (BrowseFolder(picked)) {
            orch_.setSource(picked);
            orch_.useLiveSource(); // manual source: back to live enumeration
            dirty_.store(true);
        }
        return;
    }
    if (hit(mx, my, L.destBrowse)) {
        std::wstring picked;
        if (BrowseFolder(picked)) {
            orch_.setDest(picked);
            dirty_.store(true);
        }
        return;
    }

    // Path fields: focus.
    if (hit(mx, my, L.sourceField)) {
        if (offline) return; // source field is disabled while offline
        orch_.setSourceFocus(true);
        orch_.setDestFocus(false);
        SDL_StartTextInput(window_);
        dirty_.store(true);
        return;
    }
    if (hit(mx, my, L.destField)) {
        orch_.setDestFocus(true);
        orch_.setSourceFocus(false);
        SDL_StartTextInput(window_);
        dirty_.store(true);
        return;
    }
    if (st.sourceFocus || st.destFocus) {
        orch_.setSourceFocus(false);
        orch_.setDestFocus(false);
        SDL_StopTextInput(window_);
        dirty_.store(true);
    }

    // Mode radios.
    const int mr = 110;
    for (int i = 0; i < 3; ++i) {
        const int x = kMargin + 100 + i * mr;
        const SDL_FRect r{static_cast<float>(x), static_cast<float>(L.y3 + 2),
                          static_cast<float>(mr - 12), 22.0f};
        if (hit(mx, my, r)) {
            orch_.setMode(static_cast<ScanMode>(i));
            dirty_.store(true);
        }
    }

    // Case sensitivity toggle (after the mode radios).
    {
        const int x = kMargin + 100 + 3 * mr;
        const SDL_FRect r{static_cast<float>(x), static_cast<float>(L.y3 + 2), 104.0f, 22.0f};
        if (hit(mx, my, r)) {
            orch_.setCaseSensitive(!st.caseSensitive);
            dirty_.store(true);
        }
    }

    // Back-end selection row (Auto / Win32 / MFT).
    {
        const int br = 90;
        struct BE { EnumeratorBackend v; const wchar_t* label; };
        const BE bes[] = {{EnumeratorBackend::Auto, L"Auto"},
                          {EnumeratorBackend::Win32, L"Win32"},
                          {EnumeratorBackend::Mft, L"MFT"}};
        for (int i = 0; i < 3; ++i) {
            const int x = kMargin + 100 + i * br;
            const SDL_FRect r{static_cast<float>(x), static_cast<float>(L.y3b + 2),
                              static_cast<float>(br - 12), 22.0f};
            if (hit(mx, my, r)) {
                orch_.setBackend(bes[i].v);
                dirty_.store(true);
            }
        }
    }

    // Thread selection.
    const int tr = 66;
    for (int i = 0; i < 6; ++i) {
        const int x = kMargin + 60 + i * tr;
        const SDL_FRect r{static_cast<float>(x), static_cast<float>(L.y4 + 2),
                          static_cast<float>(tr - 10), 22.0f};
        if (hit(mx, my, r)) {
            orch_.setThreadSel(i);
            dirty_.store(true);
        }
    }

    if (hit(mx, my, L.startBtn) && !running) {
        startScanFromUi();
        dirty_.store(true);
    }
    if (hit(mx, my, L.stopBtn) && running) {
        orch_.stop();
        dirty_.store(true);
    }
    if (hit(mx, my, L.snapBtn) && !running) {
        std::wstring file;
        if (BrowseSaveFile(file, L"backup_index.bin")) {
            orch_.startSnapshotScan(file); // sets the note if no source is set
        }
        dirty_.store(true);
    }
    if (hit(mx, my, L.exportBtn) && !running) {
        std::wstring file;
        if (BrowseSaveFile(file, L"risultati.csv")) {
            orch_.exportCsv(file);
        }
        dirty_.store(true);
    }
    if (hit(mx, my, L.caricaBtn) && !running) {
        onLoadSnapshot();
        dirty_.store(true);
    }

    // Filters.
    const int fr = 92;
    for (int i = 0; i < 7; ++i) {
        const int x = kMargin + i * fr;
        const SDL_FRect r{static_cast<float>(x), static_cast<float>(L.y8),
                          static_cast<float>(fr - 8), 26.0f};
        if (hit(mx, my, r)) {
            filter_ = static_cast<uint8_t>(i);
            scroll_ = 0;
            dirty_.store(true);
        }
    }

    dirty_.store(true);
}

bool AppUI::isPointerOverList(float wx, float wy) {
    (void)wx;
    const Layout L = ComputeLayout(winW_, winH_);
    return wy >= L.yList && wy < L.listBottom;
}

void AppUI::OnKeyDown(unsigned int key, bool repeat) {
    (void)repeat;
    if (key == SDLK_BACKSPACE) {
        bv::ScanOrchestrator::UiSnapshot st = orch_.snapshot();
        if (st.sourceFocus) {
            RemoveLastCodepoint(st.source);
            orch_.setSource(st.source);
            dirty_.store(true);
        } else if (st.destFocus) {
            RemoveLastCodepoint(st.dest);
            orch_.setDest(st.dest);
            dirty_.store(true);
        }
    } else if (key == SDLK_RETURN) {
        startScanFromUi();
    } else if (key == SDLK_ESCAPE) {
        if (orch_.snapshot().sourceFocus || orch_.snapshot().destFocus) {
            orch_.setSourceFocus(false);
            orch_.setDestFocus(false);
            SDL_StopTextInput(window_);
            dirty_.store(true);
        }
    }
}

void AppUI::OnTextInput(const char* text) {
    bv::ScanOrchestrator::UiSnapshot st = orch_.snapshot();
    if (!st.sourceFocus && !st.destFocus) return;
    const std::wstring ws = FromUtf8(text);
    if (st.sourceFocus) {
        orch_.setSource(st.source + ws);
    } else {
        orch_.setDest(st.dest + ws);
    }
    dirty_.store(true);
}

void AppUI::startScanFromUi() {
    // startLiveScan ignores invalid inputs (no source/destination), mirroring
    // the previous behaviour of silently doing nothing on an empty AVVIA click.
    orch_.startLiveScan();
}

// Toggle the offline comparison mode: load a snapshot (the source field is
// then disabled and the scan uses "compareFrom"), or switch back to a live
// source when a snapshot is already selected.
void AppUI::onLoadSnapshot() {
    if (orch_.snapshot().running) return;

    if (orch_.snapshot().useSnapshot) {
        // Second click: back to a live source enumeration.
        orch_.clearSnapshot();
        dirty_.store(true);
        return;
    }

    if (orch_.snapshot().sourceFocus || orch_.snapshot().destFocus) {
        orch_.setSourceFocus(false);
        orch_.setDestFocus(false);
        SDL_StopTextInput(window_);
    }

    std::wstring file;
    if (!BrowseOpenFile(file, L"Snapshot Backup Verifier (*.bin;*.bvs)", L"*.bin;*.bvs")) {
        return; // user cancelled the dialog
    }

    orch_.loadSnapshot(file);
    dirty_.store(true);
}

void AppUI::syncResultsCache(const bv::ScanOrchestrator::UiSnapshot& st) {
    if (st.resultsReady && !resultsReadySeen_) {
        uiResults_ = orch_.results();
        resultsReadySeen_ = true;
    }
    if (!st.resultsReady) resultsReadySeen_ = false;
}

std::vector<const bv::FileResult*> AppUI::FilteredRows() const {
    std::vector<const FileResult*> rows;
    const auto& ps = uiResults_.problems;
    rows.reserve(ps.size());
    for (const FileResult& p : ps) {
        switch (filter_) {
            case kFilterAll: rows.push_back(&p); break;
            case kFilterMissing: if (p.status == Status::Missing) rows.push_back(&p); break;
            case kFilterExtra: if (p.status == Status::Extra) rows.push_back(&p); break;
            case kFilterSize: if (p.status == Status::SizeMismatch) rows.push_back(&p); break;
            case kFilterContent:
                if (p.status == Status::ContentMismatch) rows.push_back(&p);
                break;
            case kFilterErrors:
                if (p.status == Status::ReadError || p.status == Status::AccessDenied ||
                    p.status == Status::ChangedDuringScan)
                    rows.push_back(&p);
                break;
            case kFilterIdentical: break; // count only
            default: break;
        }
    }
    return rows;
}

void AppUI::render(const bv::ScanOrchestrator::UiSnapshot& st) {
    SDL_SetRenderDrawColor(renderer_, kBg.r, kBg.g, kBg.b, kBg.a);
    SDL_RenderClear(renderer_);

    syncResultsCache(st);

    const Layout L = ComputeLayout(winW_, winH_);

    const bool running = st.running;
    const ScanProgress& progress = st.progress;
    const unsigned int threadCountUsed = st.threadCountUsed;
    const std::wstring& statusNote = st.statusNote;

    // Title
    DrawText(renderer_, fontBold_, "Backup Verifier — Verifica backup (sola lettura)",
             kMargin, 10, kTextHi);

    // ---- Sorgente row ----
    DrawTextVCenter(renderer_, fontBody_, "Sorgente:", L.labelX, L.y1, kFieldH, kTextLo);
    FillRect(renderer_, L.fieldX, L.y1, L.fieldW, kFieldH, kField);
    std::wstring srcText = st.source;
    RGBA srcCol = kTextHi;
    if (st.useSnapshot) {
        srcText = L"[snap] " + BaseName(st.snapshotFile);
        srcCol = kTextLo;
    }
    DrawRect(renderer_, L.fieldX, L.y1, L.fieldW, kFieldH,
             st.useSnapshot ? kBorder : (st.sourceFocus ? kAccent : kBorder));
    DrawTextVCenter(renderer_, fontBody_, ToUtf8(srcText), L.fieldX + 6, L.y1, kFieldH, srcCol);
    float mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    const bool overSrcBrowse = hit(static_cast<int>(mx), static_cast<int>(my), L.sourceBrowse);
    DrawPickerButton(renderer_, fontBody_, L.sourceBrowse, overSrcBrowse);

    // ---- Destinazione row ----
    DrawTextVCenter(renderer_, fontBody_, "Destinaz.:", L.labelX, L.y2, kFieldH, kTextLo);
    FillRect(renderer_, L.fieldX, L.y2, L.fieldW, kFieldH, kField);
    DrawRect(renderer_, L.fieldX, L.y2, L.fieldW, kFieldH, st.destFocus ? kAccent : kBorder);
    DrawTextVCenter(renderer_, fontBody_, ToUtf8(st.dest), L.fieldX + 6, L.y2, kFieldH, kTextHi);
    const bool overDstBrowse = hit(static_cast<int>(mx), static_cast<int>(my), L.destBrowse);
    DrawPickerButton(renderer_, fontBody_, L.destBrowse, overDstBrowse);

    // ---- Modalita ----
    DrawTextVCenter(renderer_, fontBody_, "Modalita:", L.labelX, L.y3, 26, kTextLo);
    const char* modeNames[3] = {"Presenza", "Dimensione", "Contenuto"};
    const int mr = 110;
    for (int i = 0; i < 3; ++i) {
        DrawToggle(renderer_, fontBody_, modeNames[i], kMargin + 100 + i * mr, L.y3 + 2,
                   mr - 12, 22, static_cast<int>(st.mode) == i);
    }
    DrawToggle(renderer_, fontBody_, "Case-sens.", kMargin + 100 + 3 * mr, L.y3 + 2,
               104, 22, st.caseSensitive);

    // ---- Back-end ----
    DrawTextVCenter(renderer_, fontBody_, "Back-end:", L.labelX, L.y3b, 26, kTextLo);
    const char* beNames[3] = {"Auto", "Win32", "MFT"};
    const int br = 90;
    for (int i = 0; i < 3; ++i) {
        DrawToggle(renderer_, fontBody_, beNames[i], kMargin + 100 + i * br, L.y3b + 2,
                   br - 12, 22, static_cast<int>(st.backend) == i);
    }

    // ---- Thread ----
    DrawTextVCenter(renderer_, fontBody_, "Thread:", L.labelX, L.y4, 26, kTextLo);
    const char* threadNames[6] = {"Auto", "1", "2", "4", "8", "16"};
    const int tr = 66;
    for (int i = 0; i < 6; ++i) {
        DrawToggle(renderer_, fontBody_, threadNames[i], kMargin + 60 + i * tr, L.y4 + 2,
                   tr - 10, 22, st.threadSel == i);
    }
    // Number of hash workers actually launched (resolved "auto" too), live
    // while the Hashing phase is running.
    {
        std::wstring thrInfo;
        if (st.mode == ScanMode::Content) {
            if (running) {
                if (progress.phase == ScanPhase::Hashing) {
                    if (progress.threads > 0) {
                        thrInfo = L"→ " + std::to_wstring(progress.threads) + L" thread hash";
                    } else {
                        thrInfo = L"(hashing in avvio...)";
                    }
                }
            } else if (st.resultsReady) {
                thrInfo = L"→ " + std::to_wstring(threadCountUsed) + L" thread hash";
            } else {
                thrInfo = L"(auto: stimato al lancio)";
            }
        } else {
            thrInfo = L"(hash non usato in questa modalita)";
        }
        DrawText(renderer_, fontBody_, ToUtf8(thrInfo), kMargin + 60 + 6 * tr + 8, L.y4 + 4,
                 kTextLo);
    }

    // ---- Buttons AVVIA / INTERROMPI ----
    const bool overStart = !running && hit(static_cast<int>(mx), static_cast<int>(my), L.startBtn);
    const bool overStop = running && hit(static_cast<int>(mx), static_cast<int>(my), L.stopBtn);
    FillRect(renderer_, static_cast<int>(L.startBtn.x), static_cast<int>(L.startBtn.y),
             static_cast<int>(L.startBtn.w), static_cast<int>(L.startBtn.h),
             overStart ? kAccentHover : kAccent);
    DrawRect(renderer_, static_cast<int>(L.startBtn.x), static_cast<int>(L.startBtn.y),
             static_cast<int>(L.startBtn.w), static_cast<int>(L.startBtn.h), kBorder);
    DrawTextCenterIn(renderer_, fontBold_, running ? "In corso..." : "AVVIA",
                     static_cast<int>(L.startBtn.x), static_cast<int>(L.startBtn.y),
                     static_cast<int>(L.startBtn.w), static_cast<int>(L.startBtn.h), kTextHi);
    FillRect(renderer_, static_cast<int>(L.stopBtn.x), static_cast<int>(L.stopBtn.y),
             static_cast<int>(L.stopBtn.w), static_cast<int>(L.stopBtn.h),
             overStop ? kAccentHover : kPanel);
    DrawRect(renderer_, static_cast<int>(L.stopBtn.x), static_cast<int>(L.stopBtn.y),
             static_cast<int>(L.stopBtn.w), static_cast<int>(L.stopBtn.h), kBorder);
    DrawTextCenterIn(renderer_, fontBold_, "INTERROMPI",
                     static_cast<int>(L.stopBtn.x), static_cast<int>(L.stopBtn.y),
                     static_cast<int>(L.stopBtn.w), static_cast<int>(L.stopBtn.h), kTextHi);

    // ---- Buttons SNAPSHOT / ESPORTA ----
    const bool overSnap = !running && hit(static_cast<int>(mx), static_cast<int>(my), L.snapBtn);
    const bool overExport =
        !running && hit(static_cast<int>(mx), static_cast<int>(my), L.exportBtn);
    FillRect(renderer_, static_cast<int>(L.snapBtn.x), static_cast<int>(L.snapBtn.y),
             static_cast<int>(L.snapBtn.w), static_cast<int>(L.snapBtn.h),
             overSnap ? kAccentHover : kPanel);
    DrawRect(renderer_, static_cast<int>(L.snapBtn.x), static_cast<int>(L.snapBtn.y),
             static_cast<int>(L.snapBtn.w), static_cast<int>(L.snapBtn.h), kBorder);
    DrawTextCenterIn(renderer_, fontBold_, "SNAPSHOT",
                     static_cast<int>(L.snapBtn.x), static_cast<int>(L.snapBtn.y),
                     static_cast<int>(L.snapBtn.w), static_cast<int>(L.snapBtn.h), kTextHi);
    FillRect(renderer_, static_cast<int>(L.exportBtn.x), static_cast<int>(L.exportBtn.y),
             static_cast<int>(L.exportBtn.w), static_cast<int>(L.exportBtn.h),
             overExport ? kAccentHover : kPanel);
    DrawRect(renderer_, static_cast<int>(L.exportBtn.x), static_cast<int>(L.exportBtn.y),
             static_cast<int>(L.exportBtn.w), static_cast<int>(L.exportBtn.h), kBorder);
    DrawTextCenterIn(renderer_, fontBold_, "ESPORTA CSV",
                     static_cast<int>(L.exportBtn.x), static_cast<int>(L.exportBtn.y),
                     static_cast<int>(L.exportBtn.w), static_cast<int>(L.exportBtn.h), kTextHi);

    // ---- Button CARICA SNAP. (offline comparison) ----
    const bool overCarica = !running && hit(static_cast<int>(mx), static_cast<int>(my), L.caricaBtn);
    FillRect(renderer_, static_cast<int>(L.caricaBtn.x), static_cast<int>(L.caricaBtn.y),
             static_cast<int>(L.caricaBtn.w), static_cast<int>(L.caricaBtn.h),
             overCarica ? kAccentHover : (st.useSnapshot ? kAccent : kPanel));
    DrawRect(renderer_, static_cast<int>(L.caricaBtn.x), static_cast<int>(L.caricaBtn.y),
             static_cast<int>(L.caricaBtn.w), static_cast<int>(L.caricaBtn.h), kBorder);
    {
        std::wstring carLab = st.useSnapshot ? BaseName(st.snapshotFile) : L"CARICA SNAP.";
        if (carLab.size() > 14) carLab = L"..." + carLab.substr(carLab.size() - 11);
        DrawTextCenterIn(renderer_, fontBold_, ToUtf8(carLab),
                         static_cast<int>(L.caricaBtn.x), static_cast<int>(L.caricaBtn.y),
                         static_cast<int>(L.caricaBtn.w), static_cast<int>(L.caricaBtn.h),
                         kTextHi);
    }

    // ---- Status ----
    std::wstring status = L"Pronto. Specificare sorgente e destinazione.";
    if (running) {
        switch (progress.phase) {
            case ScanPhase::EnumerateSource:
                status = L"Enumerazione sorgente...  (file: " + std::to_wstring(progress.files) +
                         L", dir: " + std::to_wstring(progress.dirs) + L")";
                break;
            case ScanPhase::CompareDestination:
                status = L"Enumerazione destinazione e confronto...  (voci: " +
                         std::to_wstring(progress.files) + L")";
                break;
            case ScanPhase::Hashing:
                status = L"Verifica contenuti...  (file: " +
                         std::to_wstring(progress.files) + L"/" +
                         std::to_wstring(progress.dirs) + L")";
                break;
            default: break;
        }
        if (!progress.currentPath.empty()) {
            std::wstring shortP = progress.currentPath;
            if (shortP.size() > 60) shortP = L"..." + shortP.substr(shortP.size() - 57);
            status += L"  [" + shortP + L"]";
        }
    } else if (st.resultsReady && st.cancelled) {
        status = L"Scansione interrotta dall'utente.";
    } else if (st.resultsReady) {
        status = L"Scansione completata.  Velocita: " +
                 FormatRateW(uiResults_.stats.bytesSource, st.lastSecondsTotal);
    }
    if (!statusNote.empty()) {
        status += (status == L"Pronto. Specificare sorgente e destinazione." ? L"" : L"   —  ") +
                  statusNote;
    }
    DrawText(renderer_, fontBody_, ToUtf8(status), kMargin, L.y6, kTextHi);

    // ---- Progress bar ----
    const int barW = winW_ - 2 * kMargin;
    FillRect(renderer_, kMargin, L.y7, barW, 14, kPanel);
    DrawRect(renderer_, kMargin, L.y7, barW, 14, kBorder);
    if (running) {
        const Uint64 ticks = SDL_GetTicks();
        const float frac = (ticks % 2000) / 2000.0f;
        const int seg = static_cast<int>(barW * 0.25f);
        const int x = kMargin + static_cast<int>(frac * (barW - seg));
        FillRect(renderer_, x, L.y7 + 1, seg, 12, kAccent);
    } else if (st.resultsReady) {
        FillRect(renderer_, kMargin, L.y7 + 1, barW, 12, kOk);
    }

    // ---- Filters ----
    const char* filterNames[7] = {"Tutti", "Identici", "Mancanti", "Extra",
                                  "Dimensione", "Contenuto", "Errori"};
    const int fr = 92;
    for (int i = 0; i < 7; ++i) {
        DrawToggle(renderer_, fontBody_, filterNames[i], kMargin + i * fr, L.y8,
                   fr - 8, 26, filter_ == static_cast<uint8_t>(i));
    }

    // ---- Results list ----
    DrawResultsList(L.yList, L.listBottom);
    DrawSummary(L.summaryY, st.hashingErrors);

    SDL_RenderPresent(renderer_);
    dirty_.store(false);
}

void AppUI::DrawResultsList(int yList, int listBottom) {
    const int listTop = yList;
    const int areaH = listBottom - listTop;
    const int visible = areaH / kRowH;
    if (visible <= 0) return;

    if (filter_ == kFilterIdentical) {
        const Stats& st = uiResults_.stats;
        const std::string msg =
            "File identici: " + Group(st.identicalFiles) +
            "  (conteggio; le voci identiche non vengono elencate singolarmente)";
        DrawText(renderer_, fontBody_, msg, kMargin, listTop + kRowH / 2, kOk);
        return;
    }

    auto rows = FilteredRows();
    const int maxScroll = std::max(0, static_cast<int>(rows.size()) - visible);
    scroll_ = std::clamp(scroll_, 0, maxScroll);

    const int cw = winW_ - 2 * kMargin;
    const int ch = listBottom - listTop;
    const SDL_Rect clip{kMargin, listTop, cw, ch};
    SDL_SetRenderClipRect(renderer_, &clip);

    for (int i = 0; i < visible; ++i) {
        const int idx = scroll_ + i;
        if (idx >= static_cast<int>(rows.size())) break;
        const FileResult& p = *rows[idx];
        const int y = listTop + i * kRowH;
        if (i % 2 == 0) {
            FillRect(renderer_, kMargin, y, cw, kRowH, kPanel);
        }
        DrawTextVCenter(renderer_, fontBody_, ToUtf8(StatusName(p.status)),
                        kMargin + 2, y, kRowH, StatusColor(p.status));
        const std::wstring full = (p.isDirectory ? L"[dir] " : L"") + p.relativePath;
        DrawTextVCenter(renderer_, fontBody_, ToUtf8(full), kMargin + 120, y, kRowH, kTextHi);
    }
    SDL_SetRenderClipRect(renderer_, nullptr);
}

void AppUI::DrawSummary(int summaryY, uint64_t hashingErrors) {
    const Stats& st = uiResults_.stats;
    std::string s =
        "File: src " + Group(st.sourceFiles) + " / dst " + Group(st.destFiles) +
        "   Identici " + Group(st.identicalFiles) +
        "   Mancanti " + Group(st.missingFiles) +
        "   Extra " + Group(st.extraFiles) +
        "   Dim.diversa " + Group(st.sizeMismatch) +
        "   Errori " + Group(st.readErrors + st.accessDenied + st.changedDuringScan);
    if (hashingErrors > 0) {
        s += "   Err.hash " + Group(hashingErrors);
    }
    DrawText(renderer_, fontBody_, s, kMargin, summaryY, kTextLo);
}

} // namespace bv::ui
