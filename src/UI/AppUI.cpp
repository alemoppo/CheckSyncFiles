#include "UI/AppUI.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#ifdef WIN32_LEAN_AND_MEAN
#undef WIN32_LEAN_AND_MEAN
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "Filesystem/PathUtil.h"
#include "Profiling/HashProfile.h"
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

// Right-click context menu metrics.
constexpr int kCtxRowH = 26;
constexpr int kCtxPadX = 12;
constexpr int kCtxPadY = 6;

// Geometry for the whole window, shared by render() (drawing) and
// OnMouseDown() (hit-testing) so the two can never drift apart.
struct Layout {
    int labelX = kMargin;
    int fieldX = 0;      // field left edge
    int fieldW = 0;      // field width
    int browseX = 0;     // browse button left edge
    int browseW = kBrowseW;

    int y1 = 0, y2 = 0, y3 = 0, y3b = 0, y3c = 0, y4 = 0, y5 = 0;
    int y6 = 0, y7 = 0, y8 = 0, yList = 0, listBottom = 0, summaryY = 0;
    int metricsY = 0; // dedicated footer row for Tempo / Velocita

    SDL_FRect sourceField, destField;
    SDL_FRect sourceBrowse, destBrowse;

    SDL_FRect startBtn, snapBtn, exportBtn, caricaBtn;
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
    L.y3c = L.y3b + 30; // partial-verify pattern/percent row
    L.y4 = L.y3c + 30;
    L.y5 = L.y4 + 30;
    L.y6 = L.y5 + 40;
    L.y7 = L.y6 + 30;
    L.y8 = L.y7 + 32;
    L.yList = L.y8 + 34;
    L.listBottom = H - 50;
    L.summaryY = H - 26;
    L.metricsY = H - 48;

    L.sourceField = {static_cast<float>(L.fieldX), static_cast<float>(L.y1),
                     static_cast<float>(L.fieldW), static_cast<float>(kFieldH)};
    L.destField = {static_cast<float>(L.fieldX), static_cast<float>(L.y2),
                   static_cast<float>(L.fieldW), static_cast<float>(kFieldH)};
    L.sourceBrowse = {static_cast<float>(L.browseX), static_cast<float>(L.y1),
                      static_cast<float>(kBrowseW), static_cast<float>(kFieldH)};
    L.destBrowse = {static_cast<float>(L.browseX), static_cast<float>(L.y2),
                    static_cast<float>(kBrowseW), static_cast<float>(kFieldH)};

    L.startBtn = {static_cast<float>(kMargin), static_cast<float>(L.y5), 120.0f, 30.0f};
    L.snapBtn = {static_cast<float>(kMargin + 130), static_cast<float>(L.y5), 120.0f, 30.0f};
    L.exportBtn = {static_cast<float>(kMargin + 260), static_cast<float>(L.y5), 120.0f, 30.0f};
    L.caricaBtn = {static_cast<float>(kMargin + 390), static_cast<float>(L.y5), 150.0f, 30.0f};
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
constexpr RGBA kBadHover{250, 120, 120, 255};
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

// Draws text like DrawTextVCenter but returns the pixel width, so consecutive
// segments (e.g. directory prefix + bold file name) can be chained.
int DrawTextVCenterW(SDL_Renderer* ren, TTF_Font* font, const std::string& s,
                     int x, int yBox, int boxH, RGBA c) {
    SDL_Color col{c.r, c.g, c.b, c.a};
    int tw = 0, th = 0;
    SDL_Texture* t = TextTextureCached(ren, font, s, col, tw, th);
    if (!t) return 0;
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    const int ty = yBox + (boxH - th) / 2;
    const SDL_FRect d{static_cast<float>(x), static_cast<float>(ty),
                      static_cast<float>(tw), static_cast<float>(th)};
    SDL_RenderTexture(ren, t, nullptr, &d);
    return tw;
}

// Faux-bold variant of DrawTextVCenterW: same font/size (row metrics
// unchanged), double strike with a 1px horizontal offset. Returns the width.
int DrawTextVCenterBold(SDL_Renderer* ren, TTF_Font* font, const std::string& s,
                        int x, int yBox, int boxH, RGBA c) {
    SDL_Color col{c.r, c.g, c.b, c.a};
    int tw = 0, th = 0;
    SDL_Texture* t = TextTextureCached(ren, font, s, col, tw, th);
    if (!t) return 0;
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    const int ty = yBox + (boxH - th) / 2;
    for (int dx = 0; dx <= 1; ++dx) {
        const SDL_FRect d{static_cast<float>(x + dx), static_cast<float>(ty),
                          static_cast<float>(tw), static_cast<float>(th)};
        SDL_RenderTexture(ren, t, nullptr, &d);
    }
    return tw;
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

// Draws text aligned to the right edge (rightX is the right bound of the text).
void DrawTextRight(SDL_Renderer* ren, TTF_Font* font, const std::string& s, int rightX,
                   int y, RGBA c) {
    SDL_Color col{c.r, c.g, c.b, c.a};
    int tw = 0, th = 0;
    SDL_Texture* t = TextTextureCached(ren, font, s, col, tw, th);
    if (!t) return;
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    const SDL_FRect d{static_cast<float>(rightX - tw), static_cast<float>(y),
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

// Removes the single UTF-16 codepoint immediately before `caret` (handling
// surrogate pairs), and moves the caret to just after the removed text.
void RemoveCodepointBefore(std::wstring& s, size_t& caret) {
    if (caret == 0) return;
    const size_t p = caret - 1;
    if (p > 0 && s[p] >= 0xDC00 && s[p] <= 0xDFFF &&
        s[p - 1] >= 0xD800 && s[p - 1] <= 0xDBFF) {
        s.erase(p - 1, 2);
        caret -= 2;
    } else {
        s.erase(p, 1);
        caret -= 1;
    }
}

// Removes the single UTF-16 codepoint at `caret` (handling surrogate pairs).
void RemoveCodepointAt(std::wstring& s, size_t caret) {
    if (caret >= s.size()) return;
    if (caret + 1 < s.size() && s[caret] >= 0xD800 && s[caret] <= 0xDBFF &&
        s[caret + 1] >= 0xDC00 && s[caret + 1] <= 0xDFFF) {
        s.erase(caret, 2);
    } else {
        s.erase(caret, 1);
    }
}

// Caret position stepped one codepoint left/right (never lands inside a
// surrogate pair).
size_t PrevCodepoint(const std::wstring& s, size_t caret) {
    if (caret == 0) return 0;
    const size_t p = caret - 1;
    if (p > 0 && s[p] >= 0xDC00 && s[p] <= 0xDFFF &&
        s[p - 1] >= 0xD800 && s[p - 1] <= 0xDBFF) return p - 1;
    return p;
}

size_t NextCodepoint(const std::wstring& s, size_t caret) {
    if (caret >= s.size()) return s.size();
    if (caret + 1 < s.size() && s[caret] >= 0xD800 && s[caret] <= 0xDBFF &&
        s[caret + 1] >= 0xDC00 && s[caret + 1] <= 0xDFFF) return caret + 2;
    return caret + 1;
}

// Horizontal pixel offset of a caret at `caret` inside `text`, relative to the
// left edge of the field's text area (textLeft is where the glyphs begin).
int CaretPixelX(TTF_Font* font, const std::wstring& text, size_t caret,
                int textLeft) {
    int w = 0, h = 0;
    const std::string utf = ToUtf8(text.substr(0, caret));
    TTF_GetStringSize(font, utf.c_str(), utf.size(), &w, &h);
    return textLeft + w;
}

// Inverse of CaretPixelX: turns a mouse x into the nearest caret index (in
// UTF-16 code units). Positions inside a surrogate pair snap to its start.
size_t CaretFromPixelX(TTF_Font* font, const std::wstring& text, int x,
                       int textLeft) {
    if (x <= textLeft) return 0;
    size_t best = text.size();
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i > 0 && i < text.size() && text[i] >= 0xDC00 && text[i] <= 0xDFFF &&
            text[i - 1] >= 0xD800 && text[i - 1] <= 0xDBFF) continue; // mid-pair
        int w = 0, h = 0;
        const std::string utf = ToUtf8(text.substr(0, i));
        TTF_GetStringSize(font, utf.c_str(), utf.size(), &w, &h);
        if (textLeft + w >= x) {
            best = i;
            break;
        }
    }
    return best;
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

std::wstring FormatHms(double seconds) {
    long long total = static_cast<long long>(seconds < 0.0 ? 0.0 : seconds);
    const long long h = total / 3600;
    total %= 3600;
    const long long m = total / 60;
    total %= 60;
    wchar_t buf[32];
    swprintf(buf, 32, L"%02lld:%02lld:%02lld", h, m, total);
    return buf;
}

// Per-directory seconds with millisecond resolution ("1.234 s"), for the
// Tempistiche view. Matches the CLI FmtSec granularity.
std::wstring FormatSecW(double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    wchar_t buf[32];
    swprintf(buf, 32, L"%.3f s", seconds);
    return buf;
}

// Tempistiche view: A/B side selector geometry (shared by draw + hit-test).
constexpr int kTimingSideW = 64;
constexpr int kTimingSideH = 26;
constexpr int kTimingSideGap = 8;
constexpr int kTimingLineH = 20;

// Dynamic bar scale: smallest "nice" value >= v on the 1/2/2.5/5/10 ladder
// (37->50, 480->500, 1000->1000, 1200->2000, 23000->25000, 870000->1000000),
// so small counts stay readable without tracking every fluctuation. The GUI
// feeds it the run peaks (monotonic), hence the scale only ever ratchets up.
uint64_t NiceCeil(uint64_t v) {
    if (v == 0) return 10;
    uint64_t d = 1;
    while (d <= v / 10) d *= 10; // largest 10^k <= v (v realistic: no overflow)
    if (d > (std::numeric_limits<uint64_t>::max)() / 100) {
        return (std::numeric_limits<uint64_t>::max)();
    }
    for (uint64_t m : {10ull, 20ull, 25ull, 50ull, 100ull}) {
        const uint64_t c = d * m / 10;
        if (c >= v) return c;
    }
    return d * 10; // unreachable (m = 100 always covers v < 10*d)
}

std::wstring FormatRateCountW(uint64_t count, double seconds) {
    if (seconds <= 0.0 || count == 0) return L"n/d";
    double v = static_cast<double>(count) / seconds;
    const wchar_t* units[] = {L"voci/s", L"k voci/s", L"M voci/s", L"G voci/s"};
    int u = 0;
    while (v >= 1000.0 && u < 3) {
        v /= 1000.0;
        ++u;
    }
    wchar_t buf[64];
    swprintf(buf, 64, L"%.1f %ls", v, units[u]);
    return buf;
}

const wchar_t* VerifyPatternName(bv::PartialPattern p) {
    switch (p) {
        case bv::PartialPattern::Edges: return L"Edges";
        case bv::PartialPattern::Center: return L"Center";
        case bv::PartialPattern::Random: return L"Random";
    }
    return L"?";
}

const wchar_t* StatusName(bv::Status st) {
    switch (st) {
        case bv::Status::Identical: return L"IDENTICO";
        case bv::Status::Missing: return L"MANCANTE";
        case bv::Status::Extra: return L"EXTRA";
        case bv::Status::SizeMismatch: return L"DIM_DIVERSA";
        case bv::Status::ContentMismatch: return L"CONTENUTO_DIVERSO";
        case bv::Status::IdenticalPartial: return L"IDENTICO_PARZIALE";
        case bv::Status::ContentMismatchPartial: return L"CONTENUTO_DIVERSO_PARZIALE";
        case bv::Status::ReadError: return L"ERRORE_LETTURA";
        case bv::Status::AccessDenied: return L"ACCESSO_NEGATO";
        case bv::Status::ChangedDuringScan: return L"MODIFICATO_DURANTE_SCAN";
    }
    return L"?";
}

RGBA StatusColor(bv::Status st) {
    switch (st) {
        case bv::Status::Identical:
        case bv::Status::IdenticalPartial: return kOk; // no difference found
        case bv::Status::Missing:
        case bv::Status::ContentMismatch:
        case bv::Status::ContentMismatchPartial:
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

// A target usable as `explorer.exe /select,"<path>"`. An embedded `"` would
// break out of the argument quoting, and such a path can arrive from a
// hand-edited snapshot (`relativePath` is deserialized without character
// validation; live filesystems never contain `"` in names). Refuse it here:
// never offer the action and never execute it, without transforming the path.
bool IsExplorerSafePath(const std::wstring& path) {
    return !path.empty() && path.find(L'"') == std::wstring::npos;
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
                CloseContextMenu();
                dirty_ = true;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                dirty_ = true; // refresh hover highlights
                if (scrollbarDragging_) {
                    const float dy = static_cast<float>(ev.motion.y - scrollbarDragStartY_);
                    const float trackRange = static_cast<float>(scrollbarTrackH - scrollbarThumbH);
                    if (trackRange > 0) {
                        const float newRatio = std::clamp(scrollbarDragRatio_ + dy * (1.0f / trackRange), 0.0f, 1.0f);
                        const int visibleRows = scrollbarTrackH / kRowH;
                        const int maxScroll = std::max(0, static_cast<int>(filteredCache_.size()) - visibleRows);
                        scroll_ = std::clamp(static_cast<int>(newRatio * static_cast<float>(maxScroll)), 0, maxScroll);
                    }
                    dirty_ = true;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (ev.button.button == SDL_BUTTON_RIGHT) {
                    OnRightClick(static_cast<int>(ev.button.x), static_cast<int>(ev.button.y));
                } else if (ev.button.button == SDL_BUTTON_LEFT) {
                    if (ctxOpen_) {
                        OnContextMenuClick(static_cast<int>(ev.button.x),
                                           static_cast<int>(ev.button.y));
                    } else {
                        OnMouseDown(static_cast<int>(ev.button.x), static_cast<int>(ev.button.y));
                    }
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                scrollbarDragging_ = false;
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                // `mouse_x/mouse_y` are the cursor position; `x/y` are only the
                // scroll deltas, so the list-hit test must use the cursor.
                if (ev.wheel.y != 0.0f &&
                    isPointerOverList(ev.wheel.mouse_x, ev.wheel.mouse_y)) {
                    float dy = ev.wheel.y;
                    if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) dy = -dy;
                    wheelAccum_ += dy;
                    const int ticks = static_cast<int>(wheelAccum_);
                    if (ticks != 0) {
                        wheelAccum_ -= static_cast<float>(ticks);
                        // The Tempistiche view has its own line model.
                        if (filter_ == kFilterTimings) {
                            timingScroll_ = std::max(0, timingScroll_ - ticks);
                        } else {
                            scroll_ -= ticks;
                        }
                        dirty_ = true;
                    }
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
            if (st.sourceFocus) caret_ = picked.size(); // replacement, not edit
            dirty_.store(true);
        }
        return;
    }
    if (hit(mx, my, L.destBrowse)) {
        std::wstring picked;
        if (BrowseFolder(picked)) {
            orch_.setDest(picked);
            if (st.destFocus) caret_ = picked.size();
            dirty_.store(true);
        }
        return;
    }

    // Path fields: focus (a click also moves the caret to the click position).
    if (hit(mx, my, L.sourceField)) {
        if (offline) return; // source field is disabled while offline
        orch_.setSourceFocus(true);
        orch_.setDestFocus(false);
        caret_ = CaretFromPixelX(fontBody_, st.source, mx,
                                 static_cast<int>(L.fieldX) + 6);
        SDL_StartTextInput(window_);
        dirty_.store(true);
        return;
    }
    if (hit(mx, my, L.destField)) {
        orch_.setDestFocus(true);
        orch_.setSourceFocus(false);
        caret_ = CaretFromPixelX(fontBody_, st.dest, mx,
                                 static_cast<int>(L.fieldX) + 6);
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

    // Partial-verify row: three peer pattern toggles (not a dropdown) plus a
    // percent stepper with explicit end labels. Applies to Content mode only;
    // the controller ignores it otherwise. Percent 0 means size comparison
    // (mapped to Size at scan start); the row geometry below must match the
    // draw code exactly.
    {
        const int pr = 90;
        for (int i = 0; i < 3; ++i) {
            const int x = kMargin + 100 + i * pr;
            const SDL_FRect r{static_cast<float>(x), static_cast<float>(L.y3c + 2),
                              static_cast<float>(pr - 12), 22.0f};
            if (hit(mx, my, r)) {
                orch_.setVerifyPattern(static_cast<PartialPattern>(i));
                dirty_.store(true);
            }
        }
        const int px0 = kMargin + 100 + 3 * pr + 16;
        const SDL_FRect rMinus{static_cast<float>(px0 + 118), static_cast<float>(L.y3c + 2),
                               30.0f, 22.0f};
        const SDL_FRect rPlus{static_cast<float>(px0 + 118 + 38 + 64 + 8),
                              static_cast<float>(L.y3c + 2), 30.0f, 22.0f};
        if (hit(mx, my, rMinus)) {
            orch_.setVerifyPercent(st.verifyPercent - 5);
            dirty_.store(true);
        }
        if (hit(mx, my, rPlus)) {
            orch_.setVerifyPercent(st.verifyPercent + 5);
            dirty_.store(true);
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

    if (hit(mx, my, L.startBtn) && running) {
        orch_.stop();
        dirty_.store(true);
    }
    if (hit(mx, my, L.startBtn) && !running) {
        startScanFromUi();
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

    // Filters (7 problem views + the Tempistiche timings view).
    const int fr = 92;
    for (int i = 0; i < 8; ++i) {
        const int x = kMargin + i * fr;
        const SDL_FRect r{static_cast<float>(x), static_cast<float>(L.y8),
                          static_cast<float>(fr - 8), 26.0f};
        if (hit(mx, my, r)) {
            filter_ = static_cast<uint8_t>(i);
            scroll_ = 0;
            timingScroll_ = 0;
            CloseContextMenu();
            // The Tempistiche view never reads the filtered problems cache.
            if (i != kFilterTimings) rebuildFilteredCache();
            dirty_.store(true);
        }
    }

    // Tempistiche view: A/B side selector at the top of the list area.
    if (filter_ == kFilterTimings) {
        const SDL_FRect ra{static_cast<float>(kMargin), static_cast<float>(L.yList),
                           static_cast<float>(kTimingSideW), static_cast<float>(kTimingSideH)};
        const SDL_FRect rb{static_cast<float>(kMargin + kTimingSideW + kTimingSideGap),
                           static_cast<float>(L.yList),
                           static_cast<float>(kTimingSideW), static_cast<float>(kTimingSideH)};
        if (hit(mx, my, ra) && timingSide_ != 0) {
            timingSide_ = 0;
            timingScroll_ = 0;
            dirty_.store(true);
            return;
        }
        if (hit(mx, my, rb) && timingSide_ != 1) {
            timingSide_ = 1;
            timingScroll_ = 0;
            dirty_.store(true);
            return;
        }
    }

    // Scrollbar click/drag start. Geometry is written to the members (no
    // shadowing locals) so the motion-drag handler reads the same values.
    constexpr int scrollbarW = 12;
    scrollbarTrackX = winW_ - kMargin - scrollbarW;
    if (mx >= scrollbarTrackX && mx < winW_ - kMargin &&
        my >= L.yList && my < L.listBottom) {
        scrollbarTrackY = L.yList;
        scrollbarTrackH = L.listBottom - L.yList;
        const auto& rows = filteredCache_;
        const int visibleRows = scrollbarTrackH / kRowH;
        const int maxScroll = std::max(0, static_cast<int>(rows.size()) - visibleRows);
        scroll_ = std::clamp(scroll_, 0, maxScroll);
        const float ratio = rows.empty() ? 1.0f
            : static_cast<float>(visibleRows) / static_cast<float>(rows.size());
        const int thumbH = std::max(20, static_cast<int>(scrollbarTrackH * ratio));
        const int trackRange = scrollbarTrackH - thumbH;
        const float scrollRatio = maxScroll > 0
            ? static_cast<float>(scroll_) / static_cast<float>(maxScroll)
            : 0.0f;
        const int thumbY = scrollbarTrackY + (trackRange > 0
            ? static_cast<int>(scrollRatio * static_cast<float>(trackRange))
            : 0);

        if (my >= thumbY && my < thumbY + thumbH) {
            scrollbarDragging_ = true;
            scrollbarDragStartY_ = my;
            scrollbarDragRatio_ = scrollRatio;
        } else {
            // Click on track: center the thumb on the click position.
            const float clickRatio = trackRange > 0
                ? static_cast<float>(my - scrollbarTrackY - thumbH / 2) /
                  static_cast<float>(trackRange)
                : 0.0f;
            scroll_ = std::clamp(static_cast<int>(clickRatio * static_cast<float>(maxScroll)),
                                 0, maxScroll);
        }
        dirty_.store(true);
    }

    dirty_.store(true);
}

void AppUI::OnRightClick(int mx, int my) {
    CloseContextMenu();
    const Layout L = ComputeLayout(winW_, winH_);
    // Same list area as DrawResultsList, excluding the scrollbar strip.
    constexpr int scrollbarW = 12;
    const int listRight = winW_ - kMargin - scrollbarW;
    if (mx < kMargin || mx >= listRight || my < L.yList || my >= L.listBottom) {
        dirty_ = true;
        return;
    }
    if (filter_ == kFilterIdentical || filteredCache_.empty()) {
        dirty_ = true;
        return;
    }
    const int areaH = L.listBottom - L.yList;
    const int visible = areaH / kRowH;
    if (visible <= 0) {
        dirty_ = true;
        return;
    }
    const int maxScroll = std::max(0, static_cast<int>(filteredCache_.size()) - visible);
    const int idx = std::clamp(scroll_, 0, maxScroll) + (my - L.yList) / kRowH;
    if (idx < 0 || idx >= static_cast<int>(filteredCache_.size())) {
        dirty_ = true;
        return;
    }
    const FileResult& p = *filteredCache_[idx];
    if (p.relativePath.empty()) {
        dirty_ = true; // root-level error row: nothing to select in Explorer
        return;
    }

    // One item per side whose path exists right now; a missing side simply
    // contributes no item (e.g. Missing shows only A, Extra only B). The A
    // side is never offered for offline/snapshot results (see resultsOffline_).
    const auto existsSide = [](const std::wstring& root, const std::wstring& rel,
                               std::wstring& out) {
        if (root.empty()) return false;
        out = pathutil::MakeAbsolute(pathutil::NormalizeRoot(root), rel);
        std::error_code ec;
        return std::filesystem::exists(out, ec) && !ec;
    };
    std::wstring cand;
    if (!resultsOffline_ && existsSide(resultsSourceRoot_, p.relativePath, cand) &&
        IsExplorerSafePath(cand)) {
        ctxItems_.push_back({"Apri A in Esplora risorse", cand});
    }
    if (existsSide(resultsDestRoot_, p.relativePath, cand) && IsExplorerSafePath(cand)) {
        ctxItems_.push_back({"Apri B in Esplora risorse", cand});
    }
    if (ctxItems_.empty()) {
        dirty_ = true;
        return;
    }

    // Size the menu from the longest label, then clamp it inside the window.
    int maxTw = 0;
    const SDL_Color col{kTextHi.r, kTextHi.g, kTextHi.b, kTextHi.a};
    for (const auto& it : ctxItems_) {
        int tw = 0, th = 0;
        if (TextTextureCached(renderer_, fontBody_, it.labelUtf8, col, tw, th)) {
            maxTw = std::max(maxTw, tw);
        }
    }
    ctxW_ = maxTw + 2 * kCtxPadX;
    ctxH_ = static_cast<int>(ctxItems_.size()) * kCtxRowH + 2 * kCtxPadY;
    ctxX_ = std::clamp(mx, 4, std::max(4, winW_ - ctxW_ - 4));
    ctxY_ = std::clamp(my, 4, std::max(4, winH_ - ctxH_ - 4));
    ctxOpen_ = true;
    dirty_ = true;
}

void AppUI::OnContextMenuClick(int mx, int my) {
    size_t hitIdx = ctxItems_.size();
    for (size_t i = 0; i < ctxItems_.size(); ++i) {
        const int iy = ctxY_ + kCtxPadY + static_cast<int>(i) * kCtxRowH;
        if (mx >= ctxX_ && mx < ctxX_ + ctxW_ && my >= iy && my < iy + kCtxRowH) {
            hitIdx = i;
            break;
        }
    }
    std::wstring target;
    if (hitIdx < ctxItems_.size()) target = ctxItems_[hitIdx].targetPath;
    CloseContextMenu();
    if (!target.empty()) {
        // Re-check: the file may have vanished between menu and click.
        std::error_code ec;
        if (std::filesystem::exists(target, ec) && !ec) OpenInExplorer(target);
    }
    dirty_ = true;
}

void AppUI::DrawContextMenu() {
    if (!ctxOpen_ || ctxItems_.empty()) return;
    float fmx = 0.0f, fmy = 0.0f;
    SDL_GetMouseState(&fmx, &fmy);
    FillRect(renderer_, ctxX_, ctxY_, ctxW_, ctxH_, kField);
    DrawRect(renderer_, ctxX_, ctxY_, ctxW_, ctxH_, kBorder);
    for (size_t i = 0; i < ctxItems_.size(); ++i) {
        const int iy = ctxY_ + kCtxPadY + static_cast<int>(i) * kCtxRowH;
        const bool hover = fmx >= ctxX_ && fmx < ctxX_ + ctxW_ &&
                           fmy >= iy && fmy < iy + kCtxRowH;
        if (hover) FillRect(renderer_, ctxX_ + 1, iy, ctxW_ - 2, kCtxRowH, kAccent);
        DrawTextVCenter(renderer_, fontBody_, ctxItems_[i].labelUtf8,
                        ctxX_ + kCtxPadX, iy, kCtxRowH, kTextHi);
    }
}

void AppUI::OpenInExplorer(const std::wstring& path) {
    // /select opens the containing folder with the item highlighted; for a
    // directory row this selects the directory inside its parent. The guard
    // stays here (not only at menu-build time) so no future caller can pass
    // an unvalidated argument to ShellExecuteW.
    if (!IsExplorerSafePath(path)) return;
    const std::wstring params = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
}

bool AppUI::isPointerOverList(float wx, float wy) {
    (void)wx;
    const Layout L = ComputeLayout(winW_, winH_);
    return wy >= L.yList && wy < L.listBottom;
}

void AppUI::OnKeyDown(unsigned int key, bool repeat) {
    (void)repeat;
    bv::ScanOrchestrator::UiSnapshot st = orch_.snapshot();
    const bool inField = st.sourceFocus || st.destFocus;

    if (key == SDLK_RETURN) {
        startScanFromUi();
        return;
    }
    if (key == SDLK_ESCAPE) {
        if (ctxOpen_) {
            CloseContextMenu();
            dirty_.store(true);
            return;
        }
        if (inField) {
            orch_.setSourceFocus(false);
            orch_.setDestFocus(false);
            SDL_StopTextInput(window_);
            dirty_.store(true);
        }
        return;
    }
    if (!inField) return;

    const bool ctrl = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
    std::wstring buf = st.sourceFocus ? st.source : st.dest;
    caret_ = std::min(caret_, buf.size());

    // Edits write the whole field back through the orchestrator.
    const auto commit = [&] {
        if (st.sourceFocus) orch_.setSource(buf);
        else orch_.setDest(buf);
        dirty_.store(true);
    };

    if (ctrl && key == SDLK_V) { // paste
        if (SDL_HasClipboardText()) {
            char* clip = SDL_GetClipboardText();
            if (clip) {
                const std::wstring ws = FromUtf8(clip);
                SDL_free(clip);
                buf.insert(caret_, ws);
                caret_ += ws.size();
                commit();
            }
        }
        return;
    }
    if (ctrl && key == SDLK_C) { // copy the whole field
        if (!buf.empty()) SDL_SetClipboardText(ToUtf8(buf).c_str());
        return;
    }
    if (ctrl && key == SDLK_X) { // cut: copy, then clear
        if (!buf.empty()) SDL_SetClipboardText(ToUtf8(buf).c_str());
        buf.clear();
        caret_ = 0;
        commit();
        return;
    }
    if (ctrl && key == SDLK_A) return; // no selection model yet: ignore

    switch (key) {
        case SDLK_BACKSPACE:
            RemoveCodepointBefore(buf, caret_);
            commit();
            break;
        case SDLK_DELETE:
            if (caret_ < buf.size()) {
                RemoveCodepointAt(buf, caret_);
                commit();
            }
            break;
        case SDLK_LEFT:
            caret_ = PrevCodepoint(buf, caret_);
            dirty_.store(true);
            break;
        case SDLK_RIGHT:
            caret_ = NextCodepoint(buf, caret_);
            dirty_.store(true);
            break;
        case SDLK_HOME:
            caret_ = 0;
            dirty_.store(true);
            break;
        case SDLK_END:
            caret_ = buf.size();
            dirty_.store(true);
            break;
        default:
            break;
    }
}

void AppUI::OnTextInput(const char* text) {
    // Ctrl+letter combos (e.g. Ctrl+V) can also arrive as TEXT_INPUT; they are
    // handled in OnKeyDown, so never let them type into the field.
    if (SDL_GetModState() & SDL_KMOD_CTRL) return;
    bv::ScanOrchestrator::UiSnapshot st = orch_.snapshot();
    if (!st.sourceFocus && !st.destFocus) return;
    const std::wstring ws = FromUtf8(text);
    if (ws.empty()) return;
    std::wstring buf = st.sourceFocus ? st.source : st.dest;
    caret_ = std::min(caret_, buf.size());
    buf.insert(caret_, ws);
    caret_ += ws.size();
    if (st.sourceFocus) {
        orch_.setSource(buf);
    } else {
        orch_.setDest(buf);
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
        resultsSourceRoot_ = st.source;
        resultsDestRoot_ = st.dest;
        resultsOffline_ = st.lastUsedSnapshot;
        // Same hook: the timings belong to this run, so a new run always
        // replaces them (never stale, empty when the run timed nothing).
        uiDirTiming_ = orch_.dirTiming();
        uiHashCacheHits_ = st.hashCacheHits;
        resultsReadySeen_ = true;
        scroll_ = 0;
        timingScroll_ = 0;
        CloseContextMenu();
        rebuildFilteredCache();
    }
    if (!st.resultsReady) resultsReadySeen_ = false;
}

void AppUI::rebuildFilteredCache() {
    filteredCache_ = FilteredRows();
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
                if (p.status == Status::ContentMismatch ||
                    p.status == Status::ContentMismatchPartial)
                    rows.push_back(&p);
                break;
            case kFilterErrors:
                if (p.status == Status::ReadError || p.status == Status::AccessDenied ||
                    p.status == Status::ChangedDuringScan)
                    rows.push_back(&p);
                break;
            case kFilterIdentical: break; // count only
            case kFilterTimings: break;  // dedicated view, no problem rows
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

    // Live clock / byte rate for the running operation, drawn on the dedicated
    // footer row (metricsY) so it never collides with the status line.
    std::wstring footerMetrics;

    // Arm the live clock on the first frame that observes the run, and reset it
    // as soon as the run ends so a later run starts from zero again.
    if (running) {
        if (!runStarted_) {
            runStarted_ = true;
            runStartTicks_ = SDL_GetTicks();
            lastLiveBytes_ = 0;
        }
    } else {
        runStarted_ = false;
    }

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
    if (st.sourceFocus && !st.useSnapshot) {
        caret_ = std::min(caret_, st.source.size());
        const int cx = CaretPixelX(fontBody_, st.source, caret_,
                                   static_cast<int>(L.fieldX) + 6);
        FillRect(renderer_, cx, L.y1 + 4, 1, kFieldH - 8, kTextHi);
    }
    float mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    const bool overSrcBrowse = hit(static_cast<int>(mx), static_cast<int>(my), L.sourceBrowse);
    DrawPickerButton(renderer_, fontBody_, L.sourceBrowse, overSrcBrowse);

    // ---- Destinazione row ----
    DrawTextVCenter(renderer_, fontBody_, "Destinaz.:", L.labelX, L.y2, kFieldH, kTextLo);
    FillRect(renderer_, L.fieldX, L.y2, L.fieldW, kFieldH, kField);
    DrawRect(renderer_, L.fieldX, L.y2, L.fieldW, kFieldH, st.destFocus ? kAccent : kBorder);
    DrawTextVCenter(renderer_, fontBody_, ToUtf8(st.dest), L.fieldX + 6, L.y2, kFieldH, kTextHi);
    if (st.destFocus) {
        caret_ = std::min(caret_, st.dest.size());
        const int cx = CaretPixelX(fontBody_, st.dest, caret_,
                                   static_cast<int>(L.fieldX) + 6);
        FillRect(renderer_, cx, L.y2 + 4, 1, kFieldH - 8, kTextHi);
    }
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

    // ---- Verifica parziale (Content mode) ----
    DrawTextVCenter(renderer_, fontBody_, "Verifica:", L.labelX, L.y3c, 26, kTextLo);
    const char* patNames[3] = {"Edges", "Center", "Random"};
    const int pr = 90;
    for (int i = 0; i < 3; ++i) {
        DrawToggle(renderer_, fontBody_, patNames[i], kMargin + 100 + i * pr, L.y3c + 2,
                   pr - 8, 22, static_cast<int>(st.verifyPattern) == i);
    }
    {
        const int px0 = kMargin + 100 + 3 * pr + 16;
        DrawTextVCenter(renderer_, fontBody_, "Solo dimensione", px0, L.y3c, 26, kTextLo);
        DrawToggle(renderer_, fontBody_, "-", px0 + 118, L.y3c + 2, 30, 22, false);
        wchar_t pctBuf[16];
        swprintf(pctBuf, 16, L"%d%%", st.verifyPercent);
        FillRect(renderer_, px0 + 118 + 38, L.y3c + 2, 64, 22, kPanel);
        DrawRect(renderer_, px0 + 118 + 38, L.y3c + 2, 64, 22, kBorder);
        DrawTextCenterIn(renderer_, fontBody_, ToUtf8(pctBuf), px0 + 118 + 38, L.y3c + 2,
                         64, 22, kTextHi);
        DrawToggle(renderer_, fontBody_, "+", px0 + 118 + 38 + 64 + 8, L.y3c + 2, 30, 22,
                   false);
        DrawTextVCenter(renderer_, fontBody_, "Contenuto completo",
                        px0 + 118 + 38 + 64 + 8 + 30 + 8, L.y3c, 26, kTextLo);
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

    // ---- Button AVVIA / INTERROMPI (single toggle) ----
    const bool overStart = hit(static_cast<int>(mx), static_cast<int>(my), L.startBtn);
    const RGBA startFill = running ? (overStart ? kBadHover : kBad)
                                   : (overStart ? kAccentHover : kAccent);
    FillRect(renderer_, static_cast<int>(L.startBtn.x), static_cast<int>(L.startBtn.y),
             static_cast<int>(L.startBtn.w), static_cast<int>(L.startBtn.h), startFill);
    DrawRect(renderer_, static_cast<int>(L.startBtn.x), static_cast<int>(L.startBtn.y),
             static_cast<int>(L.startBtn.w), static_cast<int>(L.startBtn.h), kBorder);
    DrawTextCenterIn(renderer_, fontBold_, running ? "INTERROMPI" : "AVVIA",
                     static_cast<int>(L.startBtn.x), static_cast<int>(L.startBtn.y),
                     static_cast<int>(L.startBtn.w), static_cast<int>(L.startBtn.h), kTextHi);
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
        if (runStarted_) {
            if (progress.bytes > 0) lastLiveBytes_ = progress.bytes;
            const double elapsed = (SDL_GetTicks() - runStartTicks_) / 1000.0;
            footerMetrics = L"Tempo: " + FormatHms(elapsed);
            const uint64_t items = progress.files + progress.dirs;
            if (items > 0) {
                footerMetrics += L"   Velocita: " + FormatRateCountW(items, elapsed);
            }
            if (lastLiveBytes_ > 0) {
                footerMetrics += L"   " + FormatRateW(lastLiveBytes_, elapsed);
            }
        }
    } else if (st.resultsReady && st.cancelled) {
        status = L"Scansione interrotta dall'utente.";
    } else if (st.resultsReady && (!st.sourceOk || !st.destinationOk)) {
        // A side failed or was only partially scanned: the run is NOT complete,
        // so it must never be presented as a successful comparison.
        status = L"Scansione incompleta: una o entrambe le radici non sono state "
                 L"scandite completamente.";
    } else if (st.resultsReady) {
        status = L"Scansione completata.";
        if (st.lastSecondsTotal > 0.0) {
            footerMetrics = L"Tempo totale: " + FormatHms(st.lastSecondsTotal);
            footerMetrics += L"   Velocita: " +
                             FormatRateW(uiResults_.stats.bytesSource, st.lastSecondsTotal);
            footerMetrics += L"   " +
                             FormatRateCountW(uiResults_.stats.sourceFiles,
                                              st.lastSecondsTotal);
        }
    }
    // Run-level partial banner (like the offline note above): percent and
    // pattern EFFECTIVELY used, shown only when the read was really partial.
    if (st.resultsReady && st.verify.percentEffective < 100) {
        status += L"   —  Verifica parziale " +
                  std::to_wstring(st.verify.percentEffective) + L"% (" +
                  VerifyPatternName(st.verify.pattern) + L")" +
                  (st.verify.patternRandom ? L" [casuale]" : L"") +
                  L": IDENTICO_PARZIALE puo differire nelle parti non lette.";
    }
    if (!statusNote.empty()) {
        status += (status == L"Pronto. Specificare sorgente e destinazione." ? L"" : L"   —  ") +
                  statusNote;
    }
    DrawText(renderer_, fontBody_, ToUtf8(status), kMargin, L.y6, kTextHi);
    if (!footerMetrics.empty()) {
        DrawTextRight(renderer_, fontBody_, ToUtf8(footerMetrics), winW_ - kMargin, L.metricsY,
                      kTextLo);
    }

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
    const char* filterNames[8] = {"Tutti", "Identici", "Mancanti", "Extra",
                                  "Dimensione", "Contenuto", "Errori", "Tempistiche"};
    const int fr = 92;
    for (int i = 0; i < 8; ++i) {
        DrawToggle(renderer_, fontBody_, filterNames[i], kMargin + i * fr, L.y8,
                   fr - 8, 26, filter_ == static_cast<uint8_t>(i));
    }

    // ---- Results list / Tempistiche view ----
    if (filter_ == kFilterTimings) {
        DrawTimings(L.yList, L.listBottom, st);
    } else {
        DrawResultsList(L.yList, L.listBottom);
    }
    DrawSummary(L.summaryY, st.hashingErrors);
    DrawContextMenu();

    SDL_RenderPresent(renderer_);
    dirty_.store(false);
}

void AppUI::DrawTimings(int yList, int listBottom, const bv::ScanOrchestrator::UiSnapshot& st) {
    const bool running = st.running;
    // A/B side selector (same geometry as the click handler in OnMouseDown).
    DrawToggle(renderer_, fontBody_, "A", kMargin, yList,
               kTimingSideW, kTimingSideH, timingSide_ == 0);
    DrawToggle(renderer_, fontBody_, "B", kMargin + kTimingSideW + kTimingSideGap, yList,
               kTimingSideW, kTimingSideH, timingSide_ == 1);
    int y = yList + kTimingSideH + 8;
    const int areaW = winW_ - 2 * kMargin;

    // Right-aligned text, vertically centred in a kTimingLineH box.
    const auto drawRightVCenter = [&](const std::string& s, int rightX, int yy, RGBA c) {
        SDL_Color col{c.r, c.g, c.b, c.a};
        int tw = 0, th = 0;
        SDL_Texture* t = TextTextureCached(renderer_, fontBody_, s, col, tw, th);
        if (!t) return;
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        const SDL_FRect d{static_cast<float>(rightX - tw),
                          static_cast<float>(yy + (kTimingLineH - th) / 2),
                          static_cast<float>(tw), static_cast<float>(th)};
        SDL_RenderTexture(renderer_, t, nullptr, &d);
    };

    if (!running && !resultsReadySeen_) {
        DrawText(renderer_, fontBody_, "Avvia una scansione per vedere le tempistiche.",
                 kMargin, y, kTextLo);
        return;
    }

    // ---- Live MatchTable gauges (per-frame progress, both sides) ----
    // Currently-unmatched entries per side, observed peaks, and the throttle
    // threshold as a tick when it fits the dynamic scale. The scale ratchets
    // on the run peaks, so it never oscillates while values fluctuate. The
    // threshold stays conceptually separate: it is only a tick, never the
    // scale itself.
    const ScanProgress& mp = st.progress;
    DrawText(renderer_, fontBold_, "MatchTable live", kMargin, y + 1, kTextHi);
    const uint64_t peakMax = std::max(
        {mp.matchPeakA, mp.matchPeakB, mp.matchPendingA, mp.matchPendingB});
    const uint64_t scale = NiceCeil(peakMax);
    drawRightVCenter("scala 0-" + Group(scale), kMargin + areaW - 8, y, kTextLo);
    y += kTimingLineH;
    const auto drawBar = [&](const char* label, uint64_t val, uint64_t peak, int yy) {
        DrawTextVCenter(renderer_, fontBody_, label, kMargin, yy, kTimingLineH, kTextHi);
        const int barX = kMargin + 24;
        const int numW = 230;
        const int barW = std::max(40, areaW - 24 - numW - 16);
        const int barH = 12;
        const int barY = yy + (kTimingLineH - barH) / 2;
        FillRect(renderer_, barX, barY, barW, barH, kPanel);
        DrawRect(renderer_, barX, barY, barW, barH, kBorder);
        const double frac = scale > 0 ? std::min(1.0, static_cast<double>(val) /
                                                       static_cast<double>(scale))
                                      : 0.0;
        if (frac > 0.0) {
            FillRect(renderer_, barX + 1, barY + 1,
                     std::max(1, static_cast<int>(frac * static_cast<double>(barW - 2))),
                     barH - 2, kAccent);
        }
        if (mp.matchHighWater > 0 && mp.matchHighWater <= scale) {
            const int tx = barX + static_cast<int>(static_cast<double>(barW) *
                                                   static_cast<double>(mp.matchHighWater) /
                                                   static_cast<double>(scale));
            FillRect(renderer_, tx - 1, barY - 2, 2, barH + 4, kBad);
        }
        drawRightVCenter(Group(val) + " (picco " + Group(peak) + ")", kMargin + areaW - 8,
                         yy, kTextHi);
    };
    drawBar("A", mp.matchPendingA, mp.matchPeakA, y);
    y += kTimingLineH;
    drawBar("B", mp.matchPendingB, mp.matchPeakB, y);
    y += kTimingLineH;
    DrawText(renderer_, fontBody_,
             "Totale A+B: " + Group(mp.matchPendingA + mp.matchPendingB) + " (picco " +
                 Group(mp.matchPeakTotal) + ")",
             kMargin, y + 1, kTextHi);
    y += kTimingLineH;
    std::string bp = mp.throttleParked > 0 ? "Backpressure: attivo, "
                                           : "Backpressure: non attivo, ";
    bp += "interventi " + Group(mp.throttleEngagements);
    bp += ", attesa totale " + ToUtf8(FormatSecW(profiling::QpcToSeconds(mp.throttleWaitTicks)));
    bp += ", max " + ToUtf8(FormatSecW(profiling::QpcToSeconds(mp.throttleMaxWaitTicks)));
    bp += ", soglia ";
    if (mp.matchHighWater == 0) {
        bp += "n/d";
    } else {
        bp += Group(mp.matchHighWater);
        if (mp.matchHighWater > scale) bp += " (oltre scala)";
    }
    DrawText(renderer_, fontBody_, bp, kMargin, y + 1, kTextLo);
    y += kTimingLineH + 6;

    if (running && !resultsReadySeen_) {
        DrawText(renderer_, fontBody_, "Scansione in corso...", kMargin, y, kTextLo);
        return;
    }
    DrawText(renderer_, fontBody_, "Hash cache hit: " + Group(uiHashCacheHits_),
             kMargin, y, kTextLo);
    y += kTimingLineH + 6;

    // Offline side A comes from the snapshot index, never from a live
    // filesystem: explain instead of showing empty tables. Side B is normal.
    if (timingSide_ == 0 && resultsOffline_) {
        DrawText(renderer_, fontBody_,
                 "Nessun dato di tempistica disponibile: il lato A proviene dallo snapshot.",
                 kMargin, y, kTextLo);
        return;
    }

    const std::vector<profiling::DirEntry>& list =
        timingSide_ == 0 ? uiDirTiming_.listA : uiDirTiming_.listB;
    const std::vector<profiling::DirEntry>& walk =
        timingSide_ == 0 ? uiDirTiming_.walkA : uiDirTiming_.walkB;
    const std::vector<profiling::DirEntry>& hash =
        timingSide_ == 0 ? uiDirTiming_.hashA : uiDirTiming_.hashB;
    if (list.empty() && walk.empty() && hash.empty()) {
        DrawText(renderer_, fontBody_, "Nessun dato di tempistica per questo run.",
                 kMargin, y, kTextLo);
        return;
    }

    // Display-only line model: titles, headers, notes and rows as uniform
    // lines. Built per frame from the already-ordered report vectors (no
    // re-sorting, no aggregation here); at most a few dozen lines.
    struct TLine {
        int kind = 0; // 0 title, 1 header, 2 row, 3 note
        const char* text = "";   // title/header/note (UTF-8 literal)
        const char* right = "";  // header right label
        const profiling::DirEntry* entry = nullptr; // rows only
    };
    std::vector<TLine> left;
    std::vector<TLine> right;
    if (!list.empty()) {
        left.push_back({0, "Enumerazione directory (Listato)"});
        left.push_back({1, "Directory", "Tempo"});
        for (const auto& e : list) left.push_back({2, "", "", &e});
    }
    if (!walk.empty()) {
        left.push_back({0, "Enumerazione directory (Walk MFT)"});
        left.push_back({1, "Directory", "Tempo"});
        for (const auto& e : walk) left.push_back({2, "", "", &e});
    }
    if (left.empty()) {
        left.push_back({3, "(nessuna enumerazione cronometrata)"});
    }
    right.push_back({0, "Hash directory"});
    // The hash column is estimate-ordered (bounded aggregation), never
    // presented as an exact top-N: same wording as CLI/README.
    right.push_back({3, "Top-N stimata (aggregazione bounded)"});
    if (!hash.empty()) {
        right.push_back({1, "Directory", "Tempo hash"});
        for (const auto& e : hash) right.push_back({2, "", "", &e});
    } else {
        right.push_back({3, "(nessun dato hash: run senza verifica contenuti)"});
    }

    const bool sideBySide = winW_ >= 720;
    const int panelsTop = y;
    const int panelsH = std::max(kTimingLineH, listBottom - y);
    const int visibleLines = std::max(1, panelsH / kTimingLineH);
    const size_t maxLines = sideBySide ? std::max(left.size(), right.size())
                                       : left.size() + right.size();
    timingScroll_ = std::clamp(timingScroll_, 0,
                               std::max(0, static_cast<int>(maxLines) - visibleLines));

    // Display-only path fit: truncate (never alter the stored data). The walk
    // root itself is keyed "" like ScanError::path; show it as "(radice)".
    const auto fitDir = [&](const std::wstring& d, int colW) {
        std::wstring s = d.empty() ? L"(radice)" : d;
        const size_t budget = static_cast<size_t>(std::max(10, (colW - 116) / 7));
        if (s.size() > budget) s = s.substr(0, budget - 3) + L"...";
        return ToUtf8(s);
    };
    const auto drawLine = [&](int x, int w, const TLine& ln, int yy) {
        if (ln.kind == 0) {
            DrawText(renderer_, fontBold_, ln.text, x + 8, yy + 1, kTextHi);
        } else if (ln.kind == 1) {
            DrawTextVCenter(renderer_, fontBody_, ln.text, x + 8, yy, kTimingLineH, kTextLo);
            drawRightVCenter(ln.right, x + w - 8, yy, kTextLo);
        } else if (ln.kind == 3) {
            DrawTextVCenter(renderer_, fontBody_, ln.text, x + 8, yy, kTimingLineH, kTextLo);
        } else {
            DrawTextVCenter(renderer_, fontBody_, fitDir(ln.entry->dir, w), x + 8, yy,
                            kTimingLineH, kTextHi);
            drawRightVCenter(ToUtf8(FormatSecW(ln.entry->seconds)), x + w - 8, yy, kTextHi);
        }
    };

    if (sideBySide) {
        const int lw = (areaW - 8) / 2;
        const int lx = kMargin;
        const int rx = lx + lw + 8;
        const int rw = areaW - lw - 8;
        const SDL_Rect clip{lx, panelsTop, lw + 8 + rw, panelsH};
        SDL_SetRenderClipRect(renderer_, &clip);
        FillRect(renderer_, lx, panelsTop, lw, panelsH, kField);
        DrawRect(renderer_, lx, panelsTop, lw, panelsH, kBorder);
        FillRect(renderer_, rx, panelsTop, rw, panelsH, kField);
        DrawRect(renderer_, rx, panelsTop, rw, panelsH, kBorder);
        for (int i = 0; i < visibleLines; ++i) {
            const size_t li = static_cast<size_t>(timingScroll_) + static_cast<size_t>(i);
            const int yy = panelsTop + i * kTimingLineH;
            if (li < left.size()) drawLine(lx, lw, left[li], yy);
            if (li < right.size()) drawLine(rx, rw, right[li], yy);
        }
        SDL_SetRenderClipRect(renderer_, nullptr);
    } else {
        const SDL_Rect clip{kMargin, panelsTop, areaW, panelsH};
        SDL_SetRenderClipRect(renderer_, &clip);
        FillRect(renderer_, kMargin, panelsTop, areaW, panelsH, kField);
        DrawRect(renderer_, kMargin, panelsTop, areaW, panelsH, kBorder);
        for (int i = 0; i < visibleLines; ++i) {
            const size_t li = static_cast<size_t>(timingScroll_) + static_cast<size_t>(i);
            const int yy = panelsTop + i * kTimingLineH;
            if (li < left.size()) {
                drawLine(kMargin, areaW, left[li], yy);
            } else if (li - left.size() < right.size()) {
                drawLine(kMargin, areaW, right[li - left.size()], yy);
            } else {
                break;
            }
        }
        SDL_SetRenderClipRect(renderer_, nullptr);
    }
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

    const auto& rows = filteredCache_;
    const int maxScroll = std::max(0, static_cast<int>(rows.size()) - visible);
    scroll_ = std::clamp(scroll_, 0, maxScroll);

    const int cw = winW_ - 2 * kMargin - 12;
    const int ch = listBottom - listTop;
    const SDL_Rect clip{kMargin, listTop, cw, ch};
    SDL_SetRenderClipRect(renderer_, &clip);

    // Compute scrollbar geometry (row-based ratio, not pixels).
    scrollbarTrackX = winW_ - kMargin - 12;
    scrollbarTrackY = listTop;
    scrollbarTrackH = ch;
    const float ratio = rows.empty() ? 1.0f
        : static_cast<float>(visible) / static_cast<float>(rows.size());
    scrollbarThumbH = std::max(20, static_cast<int>(ch * ratio));
    const int trackRange = ch - scrollbarThumbH;
    const float scrollRatio = maxScroll > 0
        ? static_cast<float>(scroll_) / static_cast<float>(maxScroll)
        : 0.0f;
    scrollbarThumbY = scrollbarTrackY + (trackRange > 0
        ? static_cast<int>(scrollRatio * static_cast<float>(trackRange))
        : 0);

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
        std::wstring head = (p.isDirectory ? L"[dir] " : L"") +
                              (p.fullPath.empty() ? p.relativePath : p.fullPath);
        std::wstring tail;
        if (!p.errorMessage.empty()) {
            std::wstring msg = p.errorMessage;
            if (msg.size() > 96) msg = msg.substr(0, 93) + L"...";
            tail = L"  (" + msg + L")";
        }
        if (head.size() > 180) head = head.substr(0, 177) + L"...";
        // Bold only the file name (last component with extension), not the path.
        const size_t sep = head.find_last_of(L"\\/");
        const std::string dirU8 =
            ToUtf8(sep == std::wstring::npos ? std::wstring() : head.substr(0, sep + 1));
        const std::string nameU8 =
            ToUtf8(sep == std::wstring::npos ? head : head.substr(sep + 1));
        int tx = kMargin + 120;
        tx += DrawTextVCenterW(renderer_, fontBody_, dirU8, tx, y, kRowH, kTextHi);
        if (!nameU8.empty()) {
            tx += DrawTextVCenterBold(renderer_, fontBody_, nameU8, tx, y, kRowH, kTextHi);
        }
        if (!tail.empty()) {
            DrawTextVCenter(renderer_, fontBody_, ToUtf8(tail), tx, y, kRowH, kTextHi);
        }
    }

    SDL_SetRenderClipRect(renderer_, nullptr);

    // Scrollbar track and thumb (drawn outside the list clip so it is visible).
    FillRect(renderer_, scrollbarTrackX, scrollbarTrackY, 12, ch, kPanel);
    DrawRect(renderer_, scrollbarTrackX, scrollbarTrackY, 12, ch, kBorder);
    if (rows.size() > static_cast<size_t>(visible)) {
        FillRect(renderer_, scrollbarTrackX + 1, scrollbarThumbY, 10, scrollbarThumbH, kAccent);
    } else {
        FillRect(renderer_, scrollbarTrackX + 1, scrollbarTrackY + 1, 10, ch - 2, kBorder);
    }
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
    if (st.identicalPartialFiles > 0) {
        s += "   Ident.parz. " + Group(st.identicalPartialFiles);
    }
    if (st.contentMismatchPartial > 0) {
        s += "   Cont.parz. " + Group(st.contentMismatchPartial);
    }
    DrawText(renderer_, fontBody_, s, kMargin, summaryY, kTextLo);
}

} // namespace bv::ui
