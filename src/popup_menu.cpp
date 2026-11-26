#include "popup_menu.h"
#include "icon_font.h"

// A menu drawn by us instead of by the menu manager: one layered window per level, an opaque rounded
// panel, and every pixel composited in software. The reason for all of this is the material
// and the row metrics of the Windows 11 menus, which a classic HMENU cannot be told to use.
//
// Input is taken on the root window with a mouse capture, so hovering, clicking and dismissing work
// whether or not the window manager gave us the foreground window; the keyboard needs the focus and
// is simply unavailable when it did not.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <mmsystem.h>  // timeBeginPeriod: raise the timer resolution while a menu is open

namespace popup_menu {
namespace {

// ---- small helpers -----------------------------------------------------------------------------

// Minimal COM smart pointer: this project builds with MinGW and does not pull in WRL.
template <class T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& o) noexcept : p(o.p) { o.p = nullptr; }
    Com& operator=(Com&& o) noexcept {
        if (this != &o) {
            Reset();
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    ~Com() { Reset(); }
    T* get() const { return p; }
    T* operator->() const { return p; }
    T** put() {
        Reset();
        return &p;
    }
    void Reset() {
        if (p) {
            p->Release();
            p = nullptr;
        }
    }
    explicit operator bool() const { return p != nullptr; }
};

struct Rgba {
    int r = 0, g = 0, b = 0;
    int a = 255;
};

inline int MinI(int a, int b) { return a < b ? a : b; }
inline int MaxI(int a, int b) { return a > b ? a : b; }
inline int ClampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Bezier { float x1, y1, x2, y2; };

// High-resolution monotonic clock in milliseconds. GetTickCount/Sleep tick at ~15.6 ms even with
// timeBeginPeriod(1), which would collapse a 150 ms animation to ~10 distinct frames. QPC has
// sub-microsecond resolution, so the eased offset actually advances every loop iteration.
inline double NowMs() {
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

// Cubic-bezier easing, evaluated like CSS: given a time x in [0,1], solve for the bezier parameter
// whose X coordinate is x, then return its Y. A few Newton steps converge for the M3 curves below.
float Ease(const Bezier& b, float x) {
    x = ClampF(x, 0.0f, 1.0f);
    if (x <= 0.0f || x >= 1.0f) return x;
    auto bez = [](float t, float p1, float p2) {
        const float u = 1.0f - t;
        return 3.0f * u * u * t * p1 + 3.0f * u * t * t * p2 + t * t * t;
    };
    auto slope = [](float t, float p1, float p2) {
        const float u = 1.0f - t;
        return 3.0f * u * u * p1 + 6.0f * u * t * (p2 - p1) + 3.0f * t * t * (1.0f - p2);
    };
    float t = x;  // good initial guess for these gentle curves
    for (int i = 0; i < 6; ++i) {
        float d = slope(t, b.x1, b.x2);
        if (std::fabs(d) < 1e-5f) break;
        t = ClampF(t - (bez(t, b.x1, b.x2) - x) / d, 0.0f, 1.0f);
    }
    return bez(t, b.y1, b.y2);
}

// ---- direct2d ----------------------------------------------------------------------------------

// Both factories are process-wide and created once: a DC render target per popup is cheap, a factory
// is not, and DirectWrite spends real time enumerating the font collection.
ID2D1Factory* D2dFactory() {
    static Com<ID2D1Factory> f;
    static bool tried = false;
    if (!tried) {
        tried = true;
        D2D1_FACTORY_OPTIONS opt{};
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &opt, (void**)f.put());
    }
    return f.get();
}

IDWriteFactory* DwFactory() {
    static Com<IDWriteFactory> f;
    static bool tried = false;
    if (!tried) {
        tried = true;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)f.put());
    }
    return f.get();
}

D2D1_COLOR_F Col(const Rgba& c) {
    D2D1_COLOR_F f{ c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f };
    return f;
}

bool FontExists(const wchar_t* face) {
    IDWriteFactory* f = DwFactory();
    if (!f) return false;
    Com<IDWriteFontCollection> col;
    if (FAILED(f->GetSystemFontCollection(col.put(), FALSE))) return false;
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(col->FindFamilyName(face, &index, &exists))) return false;
    return exists != FALSE;
}

// ---- surface -----------------------------------------------------------------------------------

// A 32bpp premultiplied BGRA bitmap with its own DC. Everything the menu draws goes through these.
struct Surface {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HBITMAP old = nullptr;
    void* bits = nullptr;
    DWORD* px = nullptr;
    int w = 0, h = 0;
    Com<ID2D1DCRenderTarget> rt;  // bound to `dc` at the start of every paint

    Surface() = default;
    ~Surface() { Destroy(); }
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    Surface(Surface&& o) noexcept { Move(o); }
    Surface& operator=(Surface&& o) noexcept {
        if (this != &o) {
            Destroy();
            Move(o);
        }
        return *this;
    }

    void Move(Surface& o) {
        dc = o.dc;
        bmp = o.bmp;
        old = o.old;
        bits = o.bits;
        px = o.px;
        w = o.w;
        h = o.h;
        rt = std::move(o.rt);
        o.dc = nullptr;
        o.bmp = nullptr;
        o.old = nullptr;
        o.bits = nullptr;
        o.px = nullptr;
        o.w = o.h = 0;
    }

    bool Create(int width, int height) {
        Destroy();
        if (width <= 0 || height <= 0) return false;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(nullptr);
        dc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (!dc) return false;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) {
            Destroy();
            return false;
        }
        old = (HBITMAP)SelectObject(dc, bmp);
        w = width;
        h = height;
        px = (DWORD*)bits;
        memset(bits, 0, (size_t)w * (size_t)h * 4);
        return true;
    }

    void Destroy() {
        rt.Reset();
        if (dc) {
            if (old) SelectObject(dc, old);
            DeleteDC(dc);
        }
        if (bmp) DeleteObject(bmp);
        dc = nullptr;
        bmp = nullptr;
        old = nullptr;
        bits = nullptr;
        px = nullptr;
        w = h = 0;
    }

    void Clear() {
        if (bits) memset(bits, 0, (size_t)w * (size_t)h * 4);
    }
};

// ---- theme -------------------------------------------------------------------------------------

// Every dimension comes from a Material 3 token (see MakeTheme for the mapping). Kept as plain ints
// M3 motion tokens: duration + easing pairs, kept as data so every transition uses the same spec.
// Durations are in ms (not DPI-scaled); geometry tokens are in 96-dpi logical px and scaled in MakeTheme.
struct Motion {
    int enterMs = 120, exitMs = 100;                 // short2 (quick utility transition in/out)
    Bezier enterEase{ 0.05f, 0.7f, 0.1f, 1.0f };     // Emphasized decelerate: snappy start, soft landing
    Bezier exitEase{ 0.3f, 0.0f, 0.8f, 0.15f };      // Emphasized accelerate: starts soft, leaves at speed
    float enterScaleFrom = 0.94f, exitScaleTo = 0.96f;
    int cornerFrom = 16, cornerTo = 8;               // shape corner-large -> corner-small (the radius animates)
    int slide = 6;                                   // spacing 1.5: subtle rise as the panel appears
    // Scroll: one wheel notch eases the offset to its target instead of jumping, so a long menu
    // glides. short3 (150ms) with standard-decelerate gives a soft landing without feeling laggy.
    int scrollMs = 150;                              // short3: enough travel, not sluggish
    Bezier scrollEase{ 0.2f, 0.0f, 0.0f, 1.0f };     // Standard: even pacing, soft stop (no back-loaded crawl)
};

// in device pixels so the layout code stays simple; the token names live in the comments.
struct Theme {
    int scale = 100;
    int rowH = 40, headerH = 32, gapH = 8;                    // compact menu item 40dp / header 32dp / spacing 8dp
    // `pad` is the margin between the panel border and the whole block of rows, the same on all four
    // sides; `rowInset` is the text inset inside a row, so the hover highlight keeps its own padding.
    int pad = 8, rowInset = 12, checkW = 18, chevW = 12, colGap = 8;  // spacing 8dp / item padding 12dp / icons 18dp 12dp
    int radius = 8, pillRadius = 4, minWidth = 180, maxWidth = 320;   // shape corner-small 8dp / extra-small 4dp
    int textH = 14, headerTextH = 12, iconSize = 40;          // Label Large 14sp / Label Medium 12sp
    int shadow = 12;                                          // soft drop shadow reach, 96-dpi px (scaled in MakeTheme)
    Motion motion;                                            // M3 motion tokens (durations/easing/scale/corner)
    Rgba panel, border, text, muted, disabled, hover, mark, scrollBar, sep, accent, shadowColor;
    Com<IDWriteTextFormat> textFmt, textFmtRtl, headerFmt, headerFmtRtl;
    HFONT iconFont = nullptr;  // GDI on purpose: see the note in MakeTheme
    bool light = true;

    ~Theme() {
        if (iconFont) DeleteObject(iconFont);
    }
};

// The DPI of the monitor the menu opens on: a tray menu can appear on a monitor other than the app's
// window, and the row metrics and the font have to match that monitor.
UINT DpiForPoint(POINT p) {
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto fn = shcore ? (GetDpiForMonitorFn)GetProcAddress(shcore, "GetDpiForMonitor") : nullptr;
    if (fn) {
        UINT x = 96, y = 96;
        if (SUCCEEDED(fn(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) && x) return x;
    }
    return 96;
}

void MakeFormat(Com<IDWriteTextFormat>& out, const wchar_t* face, float size, DWRITE_TEXT_ALIGNMENT align,
                DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL) {
    IDWriteFactory* dw = DwFactory();
    if (!dw) return;
    if (FAILED(dw->CreateTextFormat(face, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, size, L"", out.put())))
        return;
    out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    out->SetTextAlignment(align);
}

void MakeTheme(Theme& t, UINT dpi, bool light) {
    if (!dpi) dpi = 96;
    t.scale = (int)dpi;
    t.light = light;
    auto px = [dpi](int dp) { return MulDiv(dp, (int)dpi, 96); };  // dp -> device px
    // Layout tokens (Material 3): the 4dp spacing grid, shape corner scale, and the compact menu
    // item. Label Large / Label Medium supply the type sizes; the glyphs are laid out in a 40dp box
    // and scaled down to the check/chevron token sizes when drawn.
    t.rowH = px(40);        // md menu item, compact: 40dp
    t.headerH = px(32);     // group header: 2 spacing steps
    t.gapH = px(8);         // spacing 2 (4dp grid)
    t.pad = px(8);          // spacing 2
    t.rowInset = px(12);    // item horizontal padding: 12dp
    t.checkW = px(18);      // leading icon 18dp
    t.chevW = px(12);       // trailing icon 12dp
    t.colGap = px(8);       // spacing 2
    t.radius = px(8);       // shape corner-small: menus 8dp
    t.pillRadius = px(4);   // shape corner-extra-small: 4dp
    t.shadow = px(12);      // soft drop-shadow reach: 12dp
    t.minWidth = px(180);
    t.maxWidth = px(320);
    t.textH = px(14);       // Label Large: 14sp / 20sp line
    t.headerTextH = px(12); // Label Medium: 12sp / 16sp line
    t.iconSize = px(40);    // glyph layout box
    t.motion.cornerFrom = px(t.motion.cornerFrom);  // shape corner-large (animated start/end radius)
    t.motion.cornerTo = px(t.motion.cornerTo);      // shape corner-small (resting radius == t.radius)
    t.motion.slide = px(t.motion.slide);            // spacing step: entry rise

    const wchar_t* ui = FontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    // M3 Label Large / Label Medium are weight 500 (Medium).
    constexpr auto kLabelWeight = DWRITE_FONT_WEIGHT_MEDIUM;
    MakeFormat(t.textFmt, ui, (float)t.textH, DWRITE_TEXT_ALIGNMENT_LEADING, kLabelWeight);
    MakeFormat(t.textFmtRtl, ui, (float)t.textH, DWRITE_TEXT_ALIGNMENT_TRAILING, kLabelWeight);
    MakeFormat(t.headerFmt, ui, (float)t.headerTextH, DWRITE_TEXT_ALIGNMENT_LEADING, kLabelWeight);
    MakeFormat(t.headerFmtRtl, ui, (float)t.headerTextH, DWRITE_TEXT_ALIGNMENT_TRAILING, kLabelWeight);

    // The icons are rasterised with GDI rather than DirectWrite: Material Icons is registered with
    // AddFontMemResourceEx, which makes it visible to GDI only. DirectWrite's system collection cannot
    // see it and silently falls back to a font without those code points, which draws empty boxes.
    if (icons::Ready()) {
        t.iconFont = icons::Font(t.iconSize);
    } else {
        const wchar_t* face = FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
        t.iconFont = CreateFontW(-t.iconSize, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
    }

    // Semantic colour tokens, following the system-palette approach of Apple HIG and Material 3:
    //   surface          panel (secondary system background / surface container)
    //   on-surface       primary label (label / on-surface)
    //   on-surface-var   secondary label + value column (secondary label / on-surface-variant)
    //   outline          border + separators (opaque separator / outline-variant)
    //   primary          check + radio accent (system blue / primary)
    //   state layer      hover, a translucent on-surface wash (Material state layer)
    // Neutral greys are taken from Apple's system-gr grey ramp; the accent is Apple System Blue and
    // the dark scheme matches Material 3's dark on-surface/outline tones. Alpha values are kept as a
    // state layer rather than solid greys so the panel colour shows through, like the two systems.
    if (light) {
        const Rgba surface{ 250, 250, 252, 255 };        // ~ grey-6 / surface-container-low
        const Rgba onSurface{ 28, 28, 30, 255 };          // label / on-surface  #1C1C1E
        const Rgba onSurfaceVar{ 92, 92, 98, 255 };       // secondary label / on-surface-variant
        const Rgba disabled{ 174, 174, 180, 255 };        // tertiary label / grey-3
        const Rgba outline{ 198, 198, 204, 255 };         // opaque separator #C6C6C8
        const Rgba primary{ 0, 122, 255, 255 };           // system blue #007AFF
        t.panel = surface;
        t.border = Rgba{ outline.r, outline.g, outline.b, 110 };  // outline hairline
        t.text = onSurface;
        t.muted = onSurfaceVar;
        t.disabled = disabled;
        t.hover = Rgba{ onSurface.r, onSurface.g, onSurface.b, 14 };  // on-surface state layer, ~6%
        t.mark = primary;                                 // check / radio dot in the accent
        t.accent = primary;
        t.scrollBar = Rgba{ onSurfaceVar.r, onSurfaceVar.g, onSurfaceVar.b, 90 };
        t.sep = Rgba{ outline.r, outline.g, outline.b, 90 };
    } else {
        const Rgba surface{ 32, 32, 34, 255 };            // ~ grey-6 dark / surface-container
        const Rgba onSurface{ 244, 244, 246, 255 };       // label / on-surface  #F4F4F6
        const Rgba onSurfaceVar{ 174, 174, 182, 255 };    // secondary label / on-surface-variant
        const Rgba disabled{ 104, 104, 110, 255 };        // tertiary label / grey-3 dark
        const Rgba outline{ 84, 84, 90, 255 };            // separator dark / outline #545458
        const Rgba primary{ 10, 132, 255, 255 };          // system blue dark #0A84FF
        t.panel = surface;
        t.border = Rgba{ outline.r, outline.g, outline.b, 110 };
        t.text = onSurface;
        t.muted = onSurfaceVar;
        t.disabled = disabled;
        t.hover = Rgba{ onSurface.r, onSurface.g, onSurface.b, 20 };  // on-surface state layer, ~8%
        t.mark = primary;
        t.accent = primary;
        t.scrollBar = Rgba{ onSurfaceVar.r, onSurfaceVar.g, onSurfaceVar.b, 96 };
        t.sep = Rgba{ outline.r, outline.g, outline.b, 96 };
    }
    t.shadowColor = Rgba{ 0, 0, 0, 255 };  // base colour; DropShadow scales alpha per layer
}

// ---- painter -----------------------------------------------------------------------------------

// A D2D render target bound to the popup's DIB plus the brushes the theme needs. Everything is drawn
// straight into the buffer UpdateLayeredWindow uploads, so there is no intermediate bitmap.
struct Painter {
    ID2D1RenderTarget* rt = nullptr;
    Com<ID2D1SolidColorBrush> panel, border, text, muted, disabled, hover, mark, scroll, sep, accent;

    bool Begin(Surface& s, const Theme& t) {
        if (!s.dc || !s.w || !s.h) return false;
        if (!s.rt) {
            ID2D1Factory* f = D2dFactory();
            if (!f) return false;
            D2D1_RENDER_TARGET_PROPERTIES props{};
            props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
            props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
            props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
            props.dpiX = props.dpiY = 96.0f;
            props.usage = D2D1_RENDER_TARGET_USAGE_NONE;
            props.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;
            if (FAILED(f->CreateDCRenderTarget(&props, s.rt.put()))) return false;
        }
        RECT rc{ 0, 0, s.w, s.h };
        if (FAILED(s.rt->BindDC(s.dc, &rc))) return false;
        // Work in pixels: the metrics are already DPI-scaled, so one unit has to be one pixel.
        s.rt->SetDpi(96.0f, 96.0f);
        s.rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        s.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // ClearType would tint the alpha
        s.rt->BeginDraw();
        s.rt->Clear(D2D1_COLOR_F{ 0.0f, 0.0f, 0.0f, 0.0f });
        rt = s.rt.get();

        struct Brush {
            Com<ID2D1SolidColorBrush>* out;
            Rgba color;
        };
        Brush brushes[] = {
            { &panel, t.panel },       { &border, t.border }, { &text, t.text },  { &muted, t.muted },
            { &disabled, t.disabled }, { &hover, t.hover },   { &mark, t.mark },  { &scroll, t.scrollBar },
            { &sep, t.sep },           { &accent, t.accent },
        };
        for (Brush& b : brushes) {
            if (FAILED(rt->CreateSolidColorBrush(Col(b.color), b.out->put()))) return false;
        }
        return true;
    }

    void End() {
        if (rt) rt->EndDraw();
        rt = nullptr;
        panel.Reset();
        border.Reset();
        text.Reset();
        muted.Reset();
        disabled.Reset();
        hover.Reset();
        mark.Reset();
        scroll.Reset();
        sep.Reset();
        accent.Reset();
    }

    void FillRound(const D2D1_RECT_F& r, float radius, ID2D1Brush* b) {
        D2D1_ROUNDED_RECT rr{ r, radius, radius };
        rt->FillRoundedRectangle(&rr, b);
    }

    void StrokeRound(const D2D1_RECT_F& r, float radius, float width, ID2D1Brush* b) {
        D2D1_ROUNDED_RECT rr{ r, radius, radius };
        rt->DrawRoundedRectangle(&rr, b, width);
    }

    // Soft drop shadow as concentric translucent rounded rects, widest and faintest at the outside.
    // Drawn behind the panel; no effect API needed, and it composes correctly in a layered window.
    void DropShadow(const D2D1_RECT_F& panel, float radius, int reach, Rgba color) {
        if (reach <= 0) return;
        Com<ID2D1SolidColorBrush> b;
        if (FAILED(rt->CreateSolidColorBrush(Col(color), b.put()))) return;
        const int layers = 4;
        for (int i = layers; i >= 1; --i) {
            const float spread = (float)reach * i / layers;
            const float fade = (float)i / layers;
            const D2D1_RECT_F r{ panel.left - spread, panel.top - spread,
                                 panel.right + spread, panel.bottom + spread };
            D2D1_ROUNDED_RECT rr{ r, radius + spread, radius + spread };
            // Outermost is faintest; opacity grows toward the panel. Kept deliberately light for a
            // subtle M3 elevation hint rather than a heavy drop.
            D2D1_COLOR_F c = Col(color);
            c.a *= 0.03f * (1.0f - fade) + 0.008f;
            b->SetColor(c);
            rt->FillRoundedRectangle(&rr, b.get());
        }
    }

    void Line(float x0, float y0, float x1, float y1, float width, ID2D1Brush* b) {
        rt->DrawLine(D2D1_POINT_2F{ x0, y0 }, D2D1_POINT_2F{ x1, y1 }, b, width);
    }

    void Dot(float cx, float cy, float radius, ID2D1Brush* b) {
        D2D1_ELLIPSE e{ D2D1_POINT_2F{ cx, cy }, radius, radius };
        rt->FillEllipse(&e, b);
    }
};

// ---- popup -------------------------------------------------------------------------------------

struct Session;

struct Row {
    int index = 0;  // into the item list
    int y = 0;      // from the content top, including the outer padding
    int h = 0;
};

// Everything a row needs in order to paint, built once per popup: the label layout and the marks.
struct RowDraw {
    Com<IDWriteTextLayout> text, rightText;
    std::vector<unsigned char> check, chevron;  // icon-set marks, rasterised once per popup
    int checkW = 0, checkH = 0, chevronW = 0, chevronH = 0;
};

struct Popup {
    Session* s = nullptr;
    const std::vector<Item>* items = nullptr;
    Popup* parent = nullptr;
    int parentIndex = -1;
    int level = 0;
    HWND hwnd = nullptr;
    std::vector<Row> rows;
    std::vector<RowDraw> draw;  // one per row: layouts for the icon and the label
    std::vector<int> seps;      // y of every separator, so the hairline can be drawn
    int width = 0, height = 0, contentH = 0;
    int shadow = 0;                     // extra pixels around the panel for the drop shadow
    int totalW = 0, totalH = 0;         // window / surface size = panel + 2 * shadow
    int scroll = 0, maxScroll = 0;
    // Smooth scroll: `scroll` is the painted offset, `scrollTarget` is where wheel/drag wants it;
    // a timer eases scroll toward the target with the M3 standard-decelerate curve.
    int scrollTarget = 0;
    int scrollFrom = 0;      // offset at the start of the current easing step
    double scrollT0 = 0;     // QPC timestamp (ms) of that step
    bool scrollHot = false;  // pointer is over this popup's scrollbar
    int hot = -1;
    bool hasCheck = false, hasChev = false;
    int rightTextW = 0;  // widest secondary value in this popup, shared column for every row
    Surface surface;

    Popup() = default;
    Popup(const Popup&) = delete;
    Popup& operator=(const Popup&) = delete;
};

struct Session {
    HWND owner = nullptr;
    const std::vector<Item>* items = nullptr;
    Options opt;
    Theme theme;
    std::vector<Popup*> open;  // root first, deepest last
    Popup* root = nullptr;
    UINT_PTR chosen = 0;
    bool running = false;
    bool captured = false;
    bool painted = false;
    POINT anchor{};
    // Hover-open timer for submenus.
    Popup* pendingPopup = nullptr;
    int pendingIndex = -1;
    // The item the mouse went down on, so a drag off it does not activate.
    Popup* pressPopup = nullptr;
    int pressIndex = -1;
    // Active scrollbar drag (mouse-down on the thumb or track).
    Popup* dragPopup = nullptr;
    int dragGrab = 0;  // pointer's offset inside the thumb, in window pixels
};

Session* g_session = nullptr;
const wchar_t* kClassName = L"BatteryHubMenuPopup";
constexpr UINT_PTR kHoverTimer = 1;
constexpr UINT kHoverDelayMs = 220;
constexpr UINT WM_MENU_DONE = WM_APP + 77;

Popup* PopupAtPoint(Session& s, POINT p);
void ScrollBarGeom(const Popup& p, float& x, float& y, float& h, float& w);
bool ScrollBarHit(Popup& p, POINT screen, int& grab);

// ---- layout ------------------------------------------------------------------------------------

// 0.2 px of tracking on every menu label, applied at layout time so measurement and painting agree.
// DirectWrite tracks via character spacing on IDWriteTextLayout1: half a pixel before and after each
// glyph yields 0.2 px between neighbours; the minimum advance keeps very narrow glyphs from collapsing.
constexpr float kLetterSpacing = 0.2f;
void ApplyLetterSpacing(IDWriteTextLayout* layout, size_t length) {
    Com<IDWriteTextLayout1> l1;
    if (FAILED(layout->QueryInterface(IID_PPV_ARGS(l1.put())))) return;
    const DWRITE_TEXT_RANGE all{ 0, (UINT32)length };
    l1->SetCharacterSpacing(kLetterSpacing / 2, kLetterSpacing / 2, kLetterSpacing / 2, all);
}

// Width of a single-line run, or -1 when DirectWrite could not lay it out.
int TextWidth(IDWriteFactory* dw, IDWriteTextFormat* fmt, const std::wstring& text, int limit, int rowH) {
    Com<IDWriteTextLayout> layout;
    if (FAILED(dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, (FLOAT)limit, (FLOAT)rowH, layout.put())))
        return -1;
    ApplyLetterSpacing(layout.get(), text.size());
    DWRITE_TEXT_METRICS m{};
    if (FAILED(layout->GetMetrics(&m))) return -1;
    return (int)ceilf(m.widthIncludingTrailingWhitespace);
}

IDWriteTextFormat* FormatFor(const Theme& t, const Item& it, bool rtl) {
    if (it.kind == Item::Kind::Header) return t.headerFmt.get();
    return rtl ? t.textFmtRtl.get() : t.textFmt.get();
}

int MeasureText(const Theme& t, const std::wstring& text, bool rtl, bool header) {
    if (text.empty()) return 0;
    IDWriteFactory* dw = DwFactory();
    IDWriteTextFormat* fmt = header ? t.headerFmt.get() : (rtl ? t.textFmtRtl.get() : t.textFmt.get());
    if (!dw || !fmt) return 0;
    return MaxI(0, TextWidth(dw, fmt, text, 8192, t.rowH));
}

int MeasureText(const Theme& t, const Item& it, bool rtl) {
    return MeasureText(t, it.text, rtl, it.kind == Item::Kind::Header);
}

void LayoutPopup(Session& s, Popup& p) {
    const Theme& t = s.theme;
    int textW = 0;
    p.hasCheck = p.hasChev = false;
    p.rightTextW = 0;
    for (const Item& it : *p.items) {
        if (it.kind == Item::Kind::Separator) continue;
        textW = MaxI(textW, MeasureText(t, it, s.opt.rtl));
        p.rightTextW = MaxI(p.rightTextW, MeasureText(t, it.rightText, s.opt.rtl, false));
        p.hasCheck = p.hasCheck || it.checked || it.radio;
        p.hasChev = p.hasChev || !it.children.empty();
    }
    // One slot on the right holds either the check or the submenu arrow. Secondary values sit in
    // their own column just inside that slot, so labels and values both line up across rows.
    int rightW = (p.hasCheck || p.hasChev) ? MaxI(t.checkW, t.chevW) + t.colGap : 0;
    if (p.rightTextW) p.rightTextW += MaxI(2, t.colGap);  // slack for fractional glyph widths
    if (textW) textW += MaxI(2, t.colGap);  // slack: a measured width can come out a pixel short
    int rowW = (t.pad + t.rowInset) * 2 + textW + p.rightTextW + rightW;
    // A long label must not stretch the menu across the screen: past `maxWidth` it ellipsizes.
    p.width = MinI(MaxI(t.minWidth, rowW), t.maxWidth);

    p.rows.clear();
    p.seps.clear();
    int y = t.pad;
    bool prevSep = true;  // a separator at the very top has nothing to separate
    for (int i = 0; i < (int)p.items->size(); i++) {
        const Item& it = (*p.items)[i];
        if (it.kind == Item::Kind::Separator) {
            if (!prevSep) {
                p.seps.push_back(y + t.gapH / 2);
                y += t.gapH;
            }
            prevSep = true;
            continue;
        }
        Row r;
        r.index = i;
        r.y = y;
        r.h = it.kind == Item::Kind::Header ? t.headerH : t.rowH;
        y += r.h;
        prevSep = false;
        p.rows.push_back(r);
    }
    p.contentH = y + t.pad;
}

// ---- drawing -----------------------------------------------------------------------------------

// The label span of a row, identical for every row of a popup, computed once per layout.
struct RowBox {
    int textL = 0, textR = 0;
    int rightL = 0, rightR = 0;  // the secondary-value column, next to the mark slot
};

RowBox BoxFor(const Theme& t, const Popup& p, bool rtl) {
    int rightW = (p.hasCheck || p.hasChev) ? MaxI(t.checkW, t.chevW) + t.colGap : 0;
    const int innerL = t.pad + t.rowInset, innerR = p.width - t.pad - t.rowInset;
    RowBox b;
    // In LTR the value sits just inside the mark at the right; in RTL, mirror it to the left.
    b.rightL = rtl ? innerL + rightW : innerR - rightW - p.rightTextW;
    b.rightR = b.rightL + p.rightTextW;
    b.textL = rtl ? b.rightR + (p.rightTextW ? t.colGap : 0) : innerL;
    b.textR = rtl ? innerR : b.rightL - (p.rightTextW ? t.colGap : 0);
    return b;
}

// Icons come from an icon font, and a glyph's ink is not the em box: an X sits in a much smaller box
// than a monitor at the same font size. Each glyph is therefore rasterised once, its ink cropped out,
// and drawn scaled to the icon column, so every icon in the menu ends up the same optical size.
struct GlyphMask {
    std::vector<unsigned char> pixels;
    int w = 0, h = 0;
};

void BuildGlyph(const Theme& t, GlyphMask& mask, wchar_t glyph) {
    if (!t.iconFont) return;
    const int box = MaxI(16, t.iconSize * 2);
    Surface scratch;
    if (!scratch.Create(box, box)) return;

    HGDIOBJ old = SelectObject(scratch.dc, t.iconFont);
    SetBkMode(scratch.dc, TRANSPARENT);
    SetTextColor(scratch.dc, RGB(255, 255, 255));
    wchar_t utf16[2] = { glyph, 0 };
    RECT area{ 0, 0, box, box };
    DrawTextW(scratch.dc, utf16, 1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(scratch.dc, old);
    GdiFlush();

    // GDI leaves the alpha channel at zero, so the coverage is in the colour channels: white text on
    // a zeroed bitmap means blue is the coverage.
    int x0 = box, y0 = box, x1 = -1, y1 = -1;
    for (int y = 0; y < box; y++) {
        const DWORD* row = scratch.px + (size_t)y * box;
        for (int x = 0; x < box; x++) {
            if ((row[x] & 0xFF) < 8) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    if (x1 < x0 || y1 < y0) return;

    mask.w = x1 - x0 + 1;
    mask.h = y1 - y0 + 1;
    mask.pixels.resize((size_t)mask.w * (size_t)mask.h);
    for (int y = 0; y < mask.h; y++) {
        const DWORD* row = scratch.px + (size_t)(y0 + y) * box;
        for (int x = 0; x < mask.w; x++) mask.pixels[(size_t)y * mask.w + x] = (unsigned char)(row[x0 + x] & 0xFF);
    }
}

// Lays `text` out into `out`, truncating with an ellipsis when it is wider than `availW`.
// DirectWrite's own trimming let long labels run past the layout box, so the cut is done here.
void BuildTextInto(const Theme& t, IDWriteTextLayout** out, const std::wstring& value, IDWriteTextFormat* fmt,
                   int availW, int rowH) {
    if (value.empty() || !fmt) return;
    IDWriteFactory* dw = DwFactory();
    if (!dw) return;
    const int limit = MaxI(8, availW);
    std::wstring text = value;
    if (TextWidth(dw, fmt, text, 8192, rowH) > limit) {
        const std::wstring tail = L"…";
        size_t lo = 0, hi = text.size();
        while (lo < hi) {
            const size_t mid = (lo + hi + 1) / 2;
            if (TextWidth(dw, fmt, text.substr(0, mid) + tail, 8192, rowH) <= limit) lo = mid;
            else hi = mid - 1;
        }
        // Never split a surrogate pair.
        if (lo > 0 && lo < text.size() && text[lo - 1] >= 0xD800 && text[lo - 1] <= 0xDBFF) lo--;
        text = text.substr(0, lo) + tail;
    }
    if (FAILED(dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, (FLOAT)limit, (FLOAT)rowH, out))) return;
    ApplyLetterSpacing(*out, text.size());
}

void BuildText(const Theme& t, Com<IDWriteTextLayout>& out, const std::wstring& value, const Item& it,
               int availW, int rowH, bool rtl) {
    if (value.empty()) return;
    BuildTextInto(t, out.put(), value, FormatFor(t, it, rtl), availW, rowH);
}

void BuildRightText(const Theme& t, Com<IDWriteTextLayout>& out, const std::wstring& value,
                    int availW, int rowH, bool rtl) {
    if (value.empty()) return;
    IDWriteTextFormat* fmt = rtl ? t.textFmtRtl.get() : t.textFmt.get();  // never the smaller header font
    BuildTextInto(t, out.put(), value, fmt, availW, rowH);
}

void BuildDraw(Session& s, Popup& p) {
    const Theme& t = s.theme;
    p.draw.clear();
    p.draw.resize(p.rows.size());
    RowBox box = BoxFor(t, p, s.opt.rtl);
    const int availW = MaxI(8, box.textR - box.textL);
    const int rightAvailW = MaxI(8, box.rightR - box.rightL);
    for (size_t i = 0; i < p.rows.size(); i++) {
        const Item& it = (*p.items)[p.rows[i].index];
        // Marks come from the same icon set as the row icons, rasterised the same way.
        GlyphMask mask;
        if (it.checked) {
            BuildGlyph(t, mask, icons::G().check);  // radio and checkbox share the check mark
            p.draw[i].check = std::move(mask.pixels);
            p.draw[i].checkW = mask.w;
            p.draw[i].checkH = mask.h;
        }
        if (p.hasChev && !it.children.empty()) {
            BuildGlyph(t, mask, icons::G().chevronRight);
            p.draw[i].chevron = std::move(mask.pixels);
            p.draw[i].chevronW = mask.w;
            p.draw[i].chevronH = mask.h;
        }
        BuildText(t, p.draw[i].text, it.text, it, availW, p.rows[i].h, s.opt.rtl);
        BuildRightText(t, p.draw[i].rightText, it.rightText, rightAvailW, p.rows[i].h, s.opt.rtl);
    }
}

// Draws a pre-rasterised icon glyph (alpha mask) at a centre point, scaled to `size`.
static void DrawMask(ID2D1RenderTarget* rt, const std::vector<unsigned char>& mask, int mw, int mh,
                     float cx, float cy, float size, ID2D1SolidColorBrush* brush) {
    if (mask.empty() || mw <= 0 || mh <= 0) return;
    const float scale = size / (float)MaxI(mw, mh);
    const float w = mw * scale, h = mh * scale;
    std::vector<DWORD> px((size_t)mw * (size_t)mh);
    const D2D1_COLOR_F c = brush->GetColor();
    for (size_t i = 0; i < px.size(); i++) {
        const unsigned a = mask[i];
        px[i] = (a << 24) | ((unsigned)(c.r * 255) * a / 255 << 16) |
                ((unsigned)(c.g * 255) * a / 255 << 8) | ((unsigned)(c.b * 255) * a / 255);
    }
    D2D1_BITMAP_PROPERTIES bp{};
    bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bp.dpiX = bp.dpiY = 96.0f;
    Com<ID2D1Bitmap> bmp;
    if (FAILED(rt->CreateBitmap(D2D1_SIZE_U{ (UINT32)mw, (UINT32)mh }, px.data(), (UINT32)mw * 4, &bp, bmp.put()))) return;
    rt->DrawBitmap(bmp.get(), D2D1_RECT_F{ cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f },
                   1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
}

void DrawRow(Session& s, Popup& p, int rowIdx, int y, Painter& g) {
    const Theme& t = s.theme;
    const Row& r = p.rows[rowIdx];
    const Item& it = (*p.items)[r.index];
    const RowDraw& d = p.draw[rowIdx];
    const bool rtl = s.opt.rtl;
    const RowBox box = BoxFor(t, p, rtl);
    ID2D1Brush* fg = it.kind == Item::Kind::Header ? g.muted.get() : (it.disabled ? g.disabled.get() : g.text.get());
    const float cy = (float)y + r.h * 0.5f;

    if (rowIdx == p.hot) {
        const float inX = (float)t.pad, inY = (float)(t.pad * 0.25f);
        g.FillRound(D2D1_RECT_F{ inX, (float)y + inY, (float)p.width - inX, (float)y + r.h - inY },
                    (float)t.pillRadius, g.hover.get());
    }

    if (d.text) g.rt->DrawTextLayout(D2D1_POINT_2F{ (float)box.textL, (float)y }, d.text.get(), fg);
    if (d.rightText) {
        // Right-align inside the value column so every level ends at the same x (starts at the same
        // x in RTL), the way a secondary value is set in a Win11 Flyout.
        DWRITE_TEXT_METRICS tm{};
        const float tw = SUCCEEDED(d.rightText->GetMetrics(&tm)) ? tm.width : 0.0f;
        const float x = rtl ? (float)box.rightL : (float)box.rightR - tw;
        g.rt->DrawTextLayout(D2D1_POINT_2F{ x, (float)y }, d.rightText.get(),
                             it.disabled ? g.disabled.get() : g.muted.get());
    }

    // Check and chevron share the one slot at the right edge and are drawn at the same size, so a
    // checked row keeps its label where an unchecked one has it.
    const int markW = MaxI(t.checkW, t.chevW);
    const float markCx = rtl ? (float)(t.pad + t.rowInset) + (float)markW * 0.5f
                             : (float)(p.width - t.pad - t.rowInset) - (float)markW * 0.5f;
    const float markSize = (float)markW * 0.8f;
    // Selected state uses the primary accent (Apple/Material menus); the chevron is a neutral
    // navigation affordance, so it stays muted and never competes with a check mark.
    if (it.checked && !d.check.empty())
        DrawMask(g.rt, d.check, d.checkW, d.checkH, markCx, cy, markSize,
                 it.disabled ? g.disabled.get() : g.mark.get());
    if (p.hasChev && !it.children.empty() && !d.chevron.empty())
        DrawMask(g.rt, d.chevron, d.chevronW, d.chevronH, markCx, cy, markSize,
                 it.disabled ? g.disabled.get() : g.muted.get());
}

// `scale`/`anchor` implement the M3 enter/exit scale; `corner` overrides the resting radius so the
// corner shape can animate between shape tokens. Defaults keep every non-animated repaint unchanged.
void PaintPopup(Session& s, Popup& p, float scale = 1.0f,
                D2D1_POINT_2F anchor = { 0, 0 }, int corner = -1) {
    const Theme& t = s.theme;
    if (p.surface.w != p.totalW || p.surface.h != p.totalH) {
        if (!p.surface.Create(p.totalW, p.totalH)) return;
    }
    Painter g;
    if (!g.Begin(p.surface, t)) {
        g.End();
        return;
    }
    // Draw in panel coordinates: shift by the shadow margin so all row/scrollbar/anchor math stays
    // in the panel space the hit-tests use. The shadow itself is painted in the surrounding margin.
    g.rt->SetTransform(D2D1::Matrix3x2F::Translation((float)p.shadow, (float)p.shadow));
    if (scale != 1.0f)
        g.rt->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, D2D1_POINT_2F{ anchor.x, anchor.y }) *
                           D2D1::Matrix3x2F::Translation((float)p.shadow, (float)p.shadow));
    const float r = (float)(corner >= 0 ? corner : t.radius);
    const D2D1_RECT_F panel{ 0.5f, 0.5f, (float)p.width - 0.5f, (float)p.height - 0.5f };
    g.DropShadow(panel, r, p.shadow, t.shadowColor);
    g.FillRound(panel, r, g.panel.get());
    g.StrokeRound(panel, r, 1.0f, g.border.get());

    // Rows are clipped to the panel, so a scrolled popup cannot paint over its own border.
    g.rt->PushAxisAlignedClip(D2D1_RECT_F{ 1.0f, (float)t.pad, (float)p.width - 1.0f, (float)(p.height - t.pad) },
                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (size_t i = 0; i < p.rows.size(); i++) {
        int y = p.rows[i].y - p.scroll;
        if (y + p.rows[i].h <= 0 || y >= p.height) continue;
        DrawRow(s, p, (int)i, y, g);
    }
    // Separators are a hairline inside the gap, inset to the label column the way Explorer draws them.
    const float sepL = (float)(t.pad + t.rowInset);
    const float sepR = (float)(p.width - t.pad - t.rowInset);
    for (int sy : p.seps) {
        float fy = (float)(sy - p.scroll) + 0.5f;  // the half pixel keeps the 1 px line crisp
        if (fy < 0.0f || fy > (float)p.height) continue;
        g.rt->DrawLine(D2D1_POINT_2F{ sepL, fy }, D2D1_POINT_2F{ sepR, fy }, g.sep.get(), 1.0f);
    }
    g.rt->PopAxisAlignedClip();

    if (p.maxScroll > 0) {
        float bx, by, bh, bw;
        ScrollBarGeom(p, bx, by, bh, bw);
        // Hover paints the thumb in the accent (state-layer on the scroll token), also making it wider.
        g.FillRound(D2D1_RECT_F{ bx, by, bx + bw, by + bh }, bw * 0.5f,
                    p.scrollHot ? g.accent.get() : g.scroll.get());
    }
    g.End();
}

// `alpha` is a global opacity on top of the per-pixel one: the fade needs no repaint, only a new
// upload of the same surface.
void Flush(Popup& p, int alpha = 255) {
    if (!p.hwnd || !p.surface.dc) return;
    HDC screen = GetDC(nullptr);
    POINT src{ 0, 0 };
    SIZE size{ p.totalW, p.totalH };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, (BYTE)ClampI(alpha, 0, 255), AC_SRC_ALPHA };
    UpdateLayeredWindow(p.hwnd, screen, nullptr, &size, p.surface.dc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

// M3 menu motion: a short scale + corner-radius morph + fade, anchored at the edge the panel grows
// from. Enter uses short4/Emphasized-decelerate, exit uses short3/Emphasized-accelerate. Every frame
// repaints so the corner token animates too (a plain alpha Flush cannot do that). It runs before the
// message loop on open, and right before DestroyWindow on close, so nothing is clicked mid-animation.
enum class AnimKind { Enter, Exit };

void AnimatePopup(Popup& p, AnimKind kind, D2D1_POINT_2F anchor, int slideX = 0, int slideY = 0) {
    if (!p.hwnd) return;
    RECT r{};
    if (!GetWindowRect(p.hwnd, &r)) return;
    const Motion& m = p.s->theme.motion;
    const bool enter = kind == AnimKind::Enter;
    const int ms = enter ? m.enterMs : m.exitMs;
    const Bezier& ease = enter ? m.enterEase : m.exitEase;
    const int fromR = enter ? m.cornerFrom : p.s->theme.radius;
    const int toR = enter ? p.s->theme.radius : m.cornerFrom;
    const float fromS = enter ? m.enterScaleFrom : 1.0f;
    const float toS = enter ? 1.0f : m.exitScaleTo;

    // Advance by real elapsed time (QPC), not a fixed Sleep per frame: Sleep ticks at ~15.6 ms,
    // which would make a short animation visibly step. ~120 fps via 1 ms yields.
    const double t0 = NowMs();
    for (;;) {
        float t = (float)((NowMs() - t0) / ms);
        bool last = t >= 1.0f;
        if (last) t = 1.0f;
        const float e = Ease(ease, t);
        const float scale = fromS + (toS - fromS) * e;
        const int corner = (int)(fromR + (toR - fromR) * e + 0.5f);
        const int alpha = (int)(255.0f * (enter ? e : 1.0f - e) + 0.5f);
        const int dx = (int)((1.0f - (enter ? e : 1.0f - e)) * (float)slideX + 0.5f);
        const int dy = (int)((1.0f - (enter ? e : 1.0f - e)) * (float)slideY + 0.5f);
        SetWindowPos(p.hwnd, nullptr, r.left + dx, r.top + dy, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        PaintPopup(*p.s, p, scale, anchor, corner);
        Flush(p, alpha);
        if (last) break;
        Sleep(1);
    }
    // Settle exactly on the resting frame (no scale, theme radius, full alpha), then let callers
    // destroy or keep the window in its final, known state.
    SetWindowPos(p.hwnd, nullptr, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (enter) {
        PaintPopup(*p.s, p);
        Flush(p, 255);
    }
}

// Anchor for a root menu: the top edge, so it unfolds downward toward the cursor.
D2D1_POINT_2F RootAnchor(const Popup& p) {
    return D2D1_POINT_2F{ (float)p.width * 0.5f, 0.0f };
}

// Anchor for a submenu: the vertical edge facing its parent, i.e. where the new panel is attached.
D2D1_POINT_2F SubAnchor(const Session& s, const Popup& child) {
    const float edge = s.opt.rtl ? (float)child.width : 0.0f;
    return D2D1_POINT_2F{ edge, (float)child.height * 0.5f };
}

// Whole-menu exit: every open level fades/scales out together (short3), so dismissing is as quick
// as one transition no matter how many submenus are stacked.
void AnimateExit(Session& s) {
    if (s.open.empty()) return;
    const Motion& m = s.theme.motion;
    struct Layer { Popup* p; D2D1_POINT_2F anchor; };
    std::vector<Layer> layers;
    for (Popup* p : s.open) {
        RECT r{};
        if (!p->hwnd || !GetWindowRect(p->hwnd, &r)) continue;
        layers.push_back({ p, p->parent ? SubAnchor(s, *p) : RootAnchor(*p) });
    }
    if (layers.empty()) return;
    const double t0 = NowMs();
    for (;;) {
        float t = (float)((NowMs() - t0) / m.exitMs);
        bool last = t >= 1.0f;
        if (last) t = 1.0f;
        const float e = Ease(m.exitEase, t);
        const float scale = 1.0f + (m.exitScaleTo - 1.0f) * e;
        const int corner = (int)((float)s.theme.radius + (float)(m.cornerFrom - s.theme.radius) * e + 0.5f);
        const int alpha = (int)(255.0f * (1.0f - e) + 0.5f);
        for (const Layer& l : layers) {
            PaintPopup(*l.p->s, *l.p, scale, l.anchor, corner);
            Flush(*l.p, alpha);
        }
        if (last) break;
        Sleep(1);
    }
}

// ---- session helpers ---------------------------------------------------------------------------

RECT WindowRect(Popup& p) {
    RECT r{};
    if (p.hwnd) GetWindowRect(p.hwnd, &r);
    // The window carries an invisible shadow margin; every hit-test and row placement works in the
    // panel's coordinate space, so report the panel rectangle (the window inset by the margin).
    r.left += p.shadow;
    r.top += p.shadow;
    r.right -= p.shadow;
    r.bottom -= p.shadow;
    return r;
}

int RowAt(Popup& p, POINT screen) {
    RECT r = WindowRect(p);
    if (screen.x < r.left || screen.x >= r.right || screen.y < r.top || screen.y >= r.bottom) return -1;
    int y = screen.y - r.top + p.scroll;
    for (size_t i = 0; i < p.rows.size(); i++) {
        const Row& row = p.rows[i];
        if (y >= row.y && y < row.y + row.h) {
            const Item& it = (*p.items)[row.index];
            if (it.kind != Item::Kind::Normal || it.disabled) return -1;
            return (int)i;
        }
    }
    return -1;
}

Popup* PopupAtPoint(Session& s, POINT p) {
    for (auto it = s.open.rbegin(); it != s.open.rend(); ++it) {
        RECT r = WindowRect(**it);
        if (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom) return *it;
    }
    return nullptr;
}

void DestroyPopup(Session& s, Popup* p) {
    auto it = std::find(s.open.begin(), s.open.end(), p);
    if (it == s.open.end()) return;
    s.open.erase(it);
    if (s.pendingPopup == p) {
        s.pendingPopup = nullptr;
        s.pendingIndex = -1;
        KillTimer(s.root ? s.root->hwnd : nullptr, kHoverTimer);
    }
    if (s.pressPopup == p) {
        s.pressPopup = nullptr;
        s.pressIndex = -1;
    }
    if (p->hwnd) {
        SetWindowLongPtrW(p->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(p->hwnd);
    }
    delete p;
}

void CloseDeeper(Session& s, Popup* p) {
    while (!s.open.empty() && s.open.back() != p) DestroyPopup(s, s.open.back());
}

void Finish(Session& s, UINT_PTR id) {
    if (!s.running) return;
    s.chosen = id;
    AnimateExit(s);  // M3 short3: fade/scale/corner all levels out together before destroying them
    while (!s.open.empty()) DestroyPopup(s, s.open.back());
    s.root = nullptr;
    if (s.captured) {
        ReleaseCapture();
        s.captured = false;
    }
    s.running = false;
}

void Repaint(Popup& p) {
    if (!p.hwnd) return;
    PaintPopup(*p.s, p);
    Flush(p);
}

void EnsureVisible(Popup& p, int rowIdx) {
    if (rowIdx < 0 || rowIdx >= (int)p.rows.size() || p.maxScroll <= 0) return;
    const Row& r = p.rows[rowIdx];
    int top = r.y - p.scroll;
    int bottom = top + r.h;
    int limit = p.height;
    if (top < p.s->theme.pad) p.scroll = MaxI(0, r.y - p.s->theme.pad);
    else if (bottom > limit - p.s->theme.pad) p.scroll = MinI(p.maxScroll, r.y + r.h - limit + p.s->theme.pad);
}

void OpenSubmenu(Session& s, Popup& parent, int rowIdx, bool byKeyboard = false);

void SetHot(Session& s, Popup& p, int rowIdx, bool byMouse) {
    if (p.hot == rowIdx) {
        if (byMouse && rowIdx >= 0) {
            const Item& it = (*p.items)[p.rows[rowIdx].index];
            if (!it.children.empty()) {
                bool already = false;
                for (Popup* q : s.open)
                    if (q->parent == &p && q->parentIndex == rowIdx) already = true;
                if (!already && (s.pendingPopup != &p || s.pendingIndex != rowIdx)) {
                    s.pendingPopup = &p;
                    s.pendingIndex = rowIdx;
                    SetTimer(s.root->hwnd, kHoverTimer, kHoverDelayMs, nullptr);
                }
            }
        }
        return;
    }
    p.hot = rowIdx;
    Repaint(p);
    if (rowIdx < 0) return;
    const Item& it = (*p.items)[p.rows[rowIdx].index];
    if (it.disabled) return;
    if (it.children.empty()) {
        if (s.pendingPopup) {
            s.pendingPopup = nullptr;
            s.pendingIndex = -1;
            KillTimer(s.root ? s.root->hwnd : nullptr, kHoverTimer);
        }
        CloseDeeper(s, &p);
    } else if (byMouse) {
        // Close the previous row's submenu now instead of leaving two open until the timer fires.
        if (p.parent || !s.open.empty()) CloseDeeper(s, &p);
        s.pendingPopup = &p;
        s.pendingIndex = rowIdx;
        SetTimer(s.root->hwnd, kHoverTimer, kHoverDelayMs, nullptr);
    } else {
        OpenSubmenu(s, p, rowIdx, true);  // keyboard navigation: preselect the first item
    }
}

void MoveHot(Session& s, Popup& p, int dir) {
    int n = (int)p.rows.size();
    if (!n) return;
    int start = p.hot;
    int i = start;
    for (int step = 0; step < n; step++) {
        i += dir;
        if (i < 0) i = n - 1;
        if (i >= n) i = 0;
        const Item& it = (*p.items)[p.rows[i].index];
        if (it.kind == Item::Kind::Normal && !it.disabled) break;
    }
    if (i == start || i < 0) return;
    p.hot = i;
    EnsureVisible(p, i);
    Repaint(p);
    // Native menus do not open a submenu while the keyboard walks past it; VK_RIGHT does.
    if ((*p.items)[p.rows[i].index].children.empty()) CloseDeeper(s, &p);
}

void Activate(Session& s, Popup& p, int rowIdx) {
    if (rowIdx < 0 || rowIdx >= (int)p.rows.size()) return;
    const Item& it = (*p.items)[p.rows[rowIdx].index];
    if (it.disabled) return;
    if (!it.children.empty()) {
        OpenSubmenu(s, p, rowIdx);
        return;
    }
    Finish(s, it.id);
}

// ---- window ------------------------------------------------------------------------------------

void PlaceSubmenu(Session& s, Popup& parent, Popup& child, int rowIdx) {
    RECT pr = WindowRect(parent);
    const Row& r = parent.rows[rowIdx];
    int y = pr.top + r.y - parent.scroll;
    MONITORINFO mi{ sizeof(mi) };
    HMONITOR mon = MonitorFromPoint(POINT{ pr.left, pr.top }, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(mon, &mi);
    RECT work = mi.rcWork;
    int x = s.opt.rtl ? pr.left - child.width : pr.right;
    if (!s.opt.rtl && x + child.width > work.right) x = pr.left - child.width;
    if (s.opt.rtl && x < work.left) x = pr.right;
    if (y + child.height > work.bottom) y = work.bottom - child.height;
    if (y < work.top) y = work.top;
    if (x < work.left) x = work.left;
    if (x + child.width > work.right) x = work.right - child.width;
    // TOPMOST like the root, not HWND_TOP: a HWND_TOP child would sit below the topmost parent and
    // be hidden where they overlap. Later-placed submenus land above earlier ones, so level 2 < 3.
    SetWindowPos(child.hwnd, HWND_TOPMOST, x - child.shadow, y - child.shadow,
                 child.totalW, child.totalH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

Popup* NewPopup(Session& s, const std::vector<Item>& items, Popup* parent, int parentIndex);

void OpenSubmenu(Session& s, Popup& parent, int rowIdx, bool byKeyboard) {
    if (rowIdx < 0 || rowIdx >= (int)parent.rows.size()) return;
    const Item& it = (*parent.items)[parent.rows[rowIdx].index];
    if (it.children.empty() || it.disabled) return;
    for (Popup* q : s.open) {
        if (q->parent == &parent && q->parentIndex == rowIdx) {
            CloseDeeper(s, q);
            return;  // already open
        }
    }
    CloseDeeper(s, &parent);
    Popup* child = NewPopup(s, it.children, &parent, rowIdx);
    if (!child) return;
    PlaceSubmenu(s, parent, *child, rowIdx);
    if (byKeyboard) {
        // Native menus select the first enabled row when a submenu opens from the keyboard, so a
        // plain Enter activates something instead of doing nothing.
        for (size_t i = 0; i < child->rows.size(); i++) {
            const Item& sub = (*child->items)[child->rows[i].index];
            if (sub.kind == Item::Kind::Normal && !sub.disabled) {
                child->hot = (int)i;
                EnsureVisible(*child, (int)i);
                break;
            }
        }
    }
    Repaint(*child);
    // Grow from the parent's edge. Detect which side PlaceSubmenu used by comparing rectangles.
    RECT cr = WindowRect(*child), pr = WindowRect(parent);
    int slideX = (cr.left >= pr.right) ? -s.theme.motion.slide
                                       : (cr.right <= pr.left ? s.theme.motion.slide : 0);
    AnimatePopup(*child, AnimKind::Enter, SubAnchor(s, *child), slideX, 0);
    if (s.pendingPopup) {
        s.pendingPopup = nullptr;
        s.pendingIndex = -1;
        KillTimer(s.root ? s.root->hwnd : nullptr, kHoverTimer);
    }
}

Popup* NewPopup(Session& s, const std::vector<Item>& items, Popup* parent, int parentIndex) {
    Popup* p = new Popup();
    p->s = &s;
    p->items = &items;
    p->parent = parent;
    p->parentIndex = parentIndex;
    p->level = parent ? parent->level + 1 : 0;
    LayoutPopup(s, *p);

    MONITORINFO mi{ sizeof(mi) };
    HMONITOR mon = MonitorFromPoint(s.anchor, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(mon, &mi);
    // Two ceilings: the screen work area, and the caller's maxHeight (a fixed cap so a long menu
    // scrolls instead of covering the desktop). maxHeight is in 96-dpi logical pixels.
    int maxH = mi.rcWork.bottom - mi.rcWork.top - MulDiv(12, s.theme.scale, 96);
    if (s.opt.maxHeight > 0)
        maxH = MinI(maxH, MulDiv(s.opt.maxHeight, s.theme.scale, 96));
    p->height = MinI(p->contentH, maxH);
    p->maxScroll = MaxI(0, p->contentH - p->height);
    p->shadow = s.theme.shadow;
    p->totalW = p->width + 2 * p->shadow;
    p->totalH = p->height + 2 * p->shadow;

    DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW;
    if (parent) ex |= WS_EX_NOACTIVATE;
    p->hwnd = CreateWindowExW(ex, kClassName, L"", WS_POPUP, 0, 0, p->totalW, p->totalH, nullptr, nullptr,
                              GetModuleHandleW(nullptr), p);
    if (!p->hwnd) {
        delete p;
        return nullptr;
    }
    BuildDraw(s, *p);
    if (!parent) {
        s.root = p;
        s.open.push_back(p);
        PaintPopup(s, *p);
    } else {
        s.open.push_back(p);
        PaintPopup(s, *p);
    }
    return p;
}

void PlaceRoot(Session& s, Popup& p) {
    MONITORINFO mi{ sizeof(mi) };
    HMONITOR mon = MonitorFromPoint(s.anchor, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(mon, &mi);
    RECT work = mi.rcWork;
    // The taskbar the menu was opened from: stay clear of it, the same way TPMPARAMS does.
    if (s.opt.hasExclude) {
        if (s.opt.exclude.top > work.top && s.anchor.y >= s.opt.exclude.top) work.bottom = MinI(work.bottom, s.opt.exclude.top);
        else if (s.opt.exclude.bottom <= work.top + 1) work.top = MaxI(work.top, s.opt.exclude.bottom);
    }
    int margin = MulDiv(8, s.theme.scale, 96);
    int x = s.anchor.x;
    int y = s.anchor.y;
    if (x + p.width > work.right - margin) x = work.right - margin - p.width;
    if (y + p.height > work.bottom - margin) y = s.anchor.y - p.height;
    if (x < work.left + margin) x = work.left + margin;
    if (y < work.top + margin) y = work.top + margin;
    if (y + p.height > work.bottom - margin) y = work.bottom - margin - p.height;
    SetWindowPos(p.hwnd, HWND_TOPMOST, x - p.shadow, y - p.shadow,
                 p.totalW, p.totalH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// A row gets the hand cursor only when clicking it runs a command. Rows that merely expand a
// submenu on hover, plus headers/separators/disabled rows, keep the arrow. RowAt already filters
// out the non-Normal and disabled rows; `children` distinguishes action rows from submenu entries.
void ApplyCursor(Popup* over, POINT screen) {
    bool clickable = false;
    if (over) {
        int row = RowAt(*over, screen);
        if (row >= 0) {
            const Item& it = (*over->items)[over->rows[row].index];
            clickable = it.children.empty();
        }
    }
    static HCURSOR hand = LoadCursorW(nullptr, IDC_HAND);
    static HCURSOR arrow = LoadCursorW(nullptr, IDC_ARROW);
    SetCursor(clickable ? hand : arrow);
}

void OnMouseMove(Session& s, POINT screen) {
    Popup* target = PopupAtPoint(s, screen);
    ApplyCursor(target, screen);
    // Clear the scrollbar hover state everywhere it no longer applies (and repaint to un-highlight).
    for (Popup* q : s.open) {
        if (q->scrollHot && q != target) { q->scrollHot = false; Repaint(*q); }
    }
    if (!target) {
        // Pointer left every popup (e.g. moved off a submenu into the rest of the screen): collapse
        // all open submenus back to the root, not just cancel a pending timer. Native menus retract
        // a submenu as soon as the cursor leaves it, without waiting for another row to be hovered.
        if (s.root) CloseDeeper(s, s.root);
        for (Popup* q : s.open) {
            if (q->hot >= 0) { q->hot = -1; Repaint(*q); }
        }
        if (s.pendingPopup) {
            s.pendingPopup = nullptr;
            s.pendingIndex = -1;
            KillTimer(s.root ? s.root->hwnd : nullptr, kHoverTimer);
        }
        return;
    }
    // Over the scrollbar: highlight it, but do not select a row or open a submenu (no event pass-through).
    int grab = 0;
    if (ScrollBarHit(*target, screen, grab)) {
        if (!target->scrollHot) { target->scrollHot = true; Repaint(*target); }
        if (target->hot >= 0) { target->hot = -1; Repaint(*target); }
        if (s.pendingPopup) {
            s.pendingPopup = nullptr; s.pendingIndex = -1;
            KillTimer(s.root ? s.root->hwnd : nullptr, kHoverTimer);
        }
        return;
    }
    if (target->scrollHot) { target->scrollHot = false; Repaint(*target); }
    int row = RowAt(*target, screen);
    SetHot(s, *target, row, true);
}

// Scrollbar geometry is shared by painting and hit-testing, so the thumb the user grabs is exactly
// the one that was drawn. Coordinates are popup-window pixels, not content pixels. The bar sits close
// to the panel edge (only 2 dp inset), and widens on hover as a state-layer affordance.
void ScrollBarGeom(const Popup& p, float& x, float& y, float& h, float& w) {
    const Theme& t = p.s->theme;
    float track = (float)(p.height - t.pad * 2);
    h = std::max(24.0f, track * (float)p.height / (float)p.contentH);
    y = (float)t.pad + (track - h) * ((float)p.scroll / (float)p.maxScroll);
    int baseW = MaxI(2, MulDiv(3, t.scale, 96));
    w = (float)(p.scrollHot ? MaxI(4, MulDiv(6, t.scale, 96)) : baseW);
    float edge = (float)MulDiv(2, t.scale, 96);  // hug the border instead of the text column
    x = p.s->opt.rtl ? edge : (float)p.width - edge - w;
}

// Returns true when the point hits the scrollbar. `grab` is the pointer offset inside the thumb.
bool ScrollBarHit(Popup& p, POINT screen, int& grab) {
    if (p.maxScroll <= 0) return false;
    RECT r = WindowRect(p);
    float x, y, h, w;
    ScrollBarGeom(p, x, y, h, w);
    int px = screen.x - r.left, py = screen.y - r.top;
    int x0 = std::max(0, (int)x - 5), x1 = std::min(p.width, (int)(x + w) + 5);
    if (px < x0 || px > x1 || py < p.s->theme.pad || py > p.height - p.s->theme.pad) return false;
    grab = py - (int)y;
    if (grab < 0 || grab > (int)h) grab = (int)h / 2;  // clicked the track: centre the thumb here
    return true;
}

// Aim a popup at a new scroll offset. When `animated`, the timer eases `scroll` toward the target
// using the M3 standard-decelerate token; otherwise (thumb drag) it tracks the pointer immediately.
void SetScrollTarget(Session& s, Popup& p, int target, bool animated) {
    target = ClampI(target, 0, p.maxScroll);
    p.scrollTarget = target;
    if (!animated) {
        p.scroll = target;
        p.scrollTarget = target;
        Repaint(p);
        return;
    }
    if (p.scroll == target) return;
    p.scrollFrom = p.scroll;
    p.scrollT0 = NowMs();
    (void)s;  // the message loop drives the animation now; no WM_TIMER needed
}

// Advance every in-flight scroll easing by the real elapsed time. Called each turn of the message
// loop (not from a low-priority WM_TIMER), so the frame rate follows the loop, not the timer queue.
// Returns true if any popup is still animating, so the loop can yield when idle.
bool AdvanceAnimations(Session& s) {
    bool anyActive = false;
    POINT cursor;
    GetCursorPos(&cursor);
    for (Popup* p : s.open) {
        if (p->scroll == p->scrollTarget) continue;
        const float elapsed = (float)(NowMs() - p->scrollT0);
        float t = elapsed / (float)s.theme.motion.scrollMs;
        if (t >= 1.0f) {
            p->scroll = p->scrollTarget;
        } else {
            float e = Ease(s.theme.motion.scrollEase, t);
            p->scroll = p->scrollFrom + (int)((float)(p->scrollTarget - p->scrollFrom) * e + 0.5f);
            anyActive = true;
        }
        // Rows move under a stationary cursor while scrolling: refresh the highlight silently
        // (without SetHot, so a wheel gesture never arms a submenu-open timer).
        if (!p->scrollHot) {
            int row = RowAt(*p, cursor);
            if (row != p->hot) { p->hot = row; }
        }
        Repaint(*p);
    }
    return anyActive;
}

void OnScrollDrag(Session& s, POINT screen) {
    Popup* p = s.dragPopup;
    if (!p || !p->hwnd || p->maxScroll <= 0) return;
    // Same rule as wheel: dragging a level's scrollbar dismisses the submenu it had opened.
    CloseDeeper(s, p);
    RECT r = WindowRect(*p);
    float x, y, h, w;
    ScrollBarGeom(*p, x, y, h, w);
    (void)x; (void)w;
    float track = (float)(p->height - p->s->theme.pad * 2);
    float span = std::max(1.0f, track - h);
    float thumbY = (float)(screen.y - r.top - s.dragGrab);
    int next = (int)((thumbY - p->s->theme.pad) / span * p->maxScroll + 0.5f);
    SetScrollTarget(s, *p, next, false);  // dragging follows the pointer exactly
}

void OnWheel(Session& s, POINT screen, int delta) {
    Popup* target = PopupAtPoint(s, screen);
    if (!target || target->maxScroll <= 0) return;
    // Scrolling a level dismisses the submenu it had opened: e.g. wheeling in a level-2 menu closes
    // the level-3 menu. Rows shift under the pointer, so a stale submenu would point at the wrong item.
    CloseDeeper(s, target);
    // If the pointer is over the scrollbar, wheel scrolls that popup and must not select a row.
    int grab = 0;
    bool onBar = ScrollBarHit(*target, screen, grab);
    int step = std::max(1, s.theme.rowH * 2);
    SetScrollTarget(s, *target, target->scrollTarget - (delta / WHEEL_DELTA) * step, true);
    // Refresh the highlight for the row under the pointer, but do NOT arm the submenu timer
    // (byMouse=false) — a wheel gesture should scroll, not re-open the submenu just dismissed.
    SetHot(s, *target, onBar ? -1 : RowAt(*target, screen), false);
    if (onBar) Repaint(*target);
}

void OnKey(Session& s, WPARAM key) {
    Popup* p = s.open.empty() ? nullptr : s.open.back();
    if (!p) return;
    switch (key) {
        case VK_ESCAPE:
            // Like a native menu: Esc closes the deepest submenu first; only the top level exits.
            if (p->parent) {
                Popup* parent = p->parent;
                int idx = p->parentIndex;
                CloseDeeper(s, parent);
                parent->hot = idx;
                Repaint(*parent);
            } else {
                Finish(s, 0);
            }
            break;
        case VK_UP: MoveHot(s, *p, -1); break;
        case VK_DOWN: MoveHot(s, *p, +1); break;
        case VK_HOME:
            p->hot = -1;
            MoveHot(s, *p, +1);
            break;
        case VK_END:
            p->hot = 0;
            MoveHot(s, *p, -1);
            break;
        case VK_RETURN:
        case VK_SPACE: Activate(s, *p, p->hot); break;
        case VK_RIGHT:
            if (p->hot >= 0) {
                const Item& it = (*p->items)[p->rows[p->hot].index];
                if (!it.children.empty()) OpenSubmenu(s, *p, p->hot, true);
            }
            break;
        case VK_LEFT:
            if (p->parent) {
                Popup* parent = p->parent;
                int idx = p->parentIndex;
                CloseDeeper(s, parent);
                parent->hot = idx;
                Repaint(*parent);
            }
            break;
        case VK_TAB: Finish(s, 0); break;
        default: break;
    }
}

LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        // CreateWindowEx hands us the Popup* as lpCreateParams; keep it on the window.
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        Popup* created = (Popup*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)created);
        if (created) created->hwnd = hwnd;
        return 0;
    }
    Popup* p = (Popup*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    Session* s = p ? p->s : g_session;
    if (!s) return DefWindowProcW(hwnd, msg, wp, lp);
    const bool isRoot = (p == s->root);

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            if (p) Flush(*p);
            return 0;
        }
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        case WM_NCHITTEST: {
            // A layered window reports HTTRANSPARENT over its transparent pixels, which suppresses
            // WM_SETCURSOR. Report the whole panel as client area so the cursor can be updated.
            POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
            RECT r{};
            if (p && GetWindowRect(p->hwnd, &r) && pt.x >= r.left && pt.x < r.right &&
                pt.y >= r.top && pt.y < r.bottom)
                return HTCLIENT;
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        case WM_SETCURSOR: {
            // Use the popup under the pointer (not `p`) because the root holds mouse capture and
            // receives these messages even when the pointer is over a submenu. ApplyCursor applies
            // the same action-vs-submenu rule as the mouse-move path.
            if (s->running) {
                POINT pt;
                GetCursorPos(&pt);
                ApplyCursor(PopupAtPoint(*s, pt), pt);
            }
            return TRUE;
        }
        case WM_MOUSEMOVE:
            if (isRoot && s->running) {
                POINT pt;
                GetCursorPos(&pt);
                if (s->dragPopup) {
                    OnScrollDrag(*s, pt);
                    return 0;
                }
                OnMouseMove(*s, pt);
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
            if (isRoot && s->running) {
                POINT pt;
                GetCursorPos(&pt);
                Popup* target = PopupAtPoint(*s, pt);
                int grab = 0;
                if (target && ScrollBarHit(*target, pt, grab)) {
                    s->dragPopup = target;
                    s->dragGrab = grab;
                    return 0;
                }
                s->pressPopup = target;
                s->pressIndex = target ? RowAt(*target, pt) : -1;
                if (!target) {
                    Finish(*s, 0);
                    return 0;
                }
            }
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            if (isRoot && s->running) {
                if (s->dragPopup) {
                    s->dragPopup = nullptr;
                    return 0;
                }
                POINT pt;
                GetCursorPos(&pt);
                Popup* target = PopupAtPoint(*s, pt);
                int row = target ? RowAt(*target, pt) : -1;
                bool same = target && target == s->pressPopup && row == s->pressIndex;
                s->pressPopup = nullptr;
                s->pressIndex = -1;
                if (target && (same || msg == WM_RBUTTONUP)) Activate(*s, *target, row);
            }
            return 0;
        case WM_MOUSEWHEEL:
            // Every level gets wheel messages: a submenu window can receive the wheel when Windows
            // scrolls the window under the cursor, even though only the root holds focus/capture.
            if (s->running) {
                POINT pt;
                GetCursorPos(&pt);
                OnWheel(*s, pt, GET_WHEEL_DELTA_WPARAM(wp));
            }
            return 0;
        case WM_KEYDOWN:
            if (isRoot && s->running) OnKey(*s, wp);
            return 0;
        case WM_TIMER:
            if (wp == kHoverTimer && s->running) {
                KillTimer(hwnd, kHoverTimer);
                Popup* target = s->pendingPopup;
                int idx = s->pendingIndex;
                s->pendingPopup = nullptr;
                s->pendingIndex = -1;
                if (target && target->hot == idx) OpenSubmenu(*s, *target, idx);
            }
            return 0;
        case WM_ACTIVATE:
            if (isRoot && s->running && LOWORD(wp) == WA_INACTIVE) {
                Finish(*s, 0);
                return 0;
            }
            break;
        case WM_ACTIVATEAPP:
            if (isRoot && s->running && !wp) {
                Finish(*s, 0);
                return 0;
            }
            break;
        case WM_MENU_DONE:
            if (s->running) Finish(*s, 0);
            return 0;
        case WM_DESTROY:
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool RegisterClassOnce() {
    static bool done = false;
    if (done) return true;
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_SAVEBITS;
    wc.lpfnWndProc = PopupProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = kClassName;
    done = RegisterClassExW(&wc) != 0;
    return done;
}

// ---- native fallback ---------------------------------------------------------------------------

UINT_PTR NativeMenu(HWND owner, POINT at, const std::vector<Item>& items, const Options& opt) {
    std::function<void(HMENU, const std::vector<Item>&)> add = [&](HMENU menu, const std::vector<Item>& list) {
        for (const Item& it : list) {
            if (it.kind == Item::Kind::Separator) {
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                continue;
            }
            UINT flags = MF_STRING;
            if (it.disabled) flags |= MF_GRAYED;
            if (it.checked || it.radio) flags |= MF_CHECKED;
            if (!it.children.empty()) {
                HMENU sub = CreatePopupMenu();
                add(sub, it.children);
                AppendMenuW(menu, flags | MF_POPUP, (UINT_PTR)sub, it.text.c_str());
            } else {
                AppendMenuW(menu, flags, (UINT_PTR)it.id, it.text.c_str());
            }
        }
    };
    HMENU menu = CreatePopupMenu();
    if (!menu) return 0;
    add(menu, items);
    TPMPARAMS tpm{ sizeof(tpm) };
    if (opt.hasExclude) tpm.rcExclude = opt.exclude;
    UINT_PTR cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | (opt.rtl ? TPM_LAYOUTRTL : 0),
                                    at.x, at.y, owner, opt.hasExclude ? &tpm : nullptr);
    DestroyMenu(menu);
    return cmd;
}

}  // namespace

bool Show(HWND owner, POINT at, const std::vector<Item>& items, const Options& opt, UINT_PTR& chosen) {
    chosen = 0;
    if (items.empty()) return false;
    // Without Direct2D or DirectWrite the popup would come up as an empty panel, so in that case the
    // classic menu is the better answer (it also covers a Windows without those components).
    if (!RegisterClassOnce() || !D2dFactory() || !DwFactory()) {
        chosen = NativeMenu(owner, at, items, opt);
        return true;
    }

    // The default 15.6 ms timer tick makes an 8 ms scroll step run at ~30 fps. Raise the system
    // timer resolution for the lifetime of this modal menu so the easing and hover animations run
    // at their intended frame rate, then restore it on exit.
    timeBeginPeriod(1);

    Session s;
    s.owner = owner;
    s.items = &items;
    s.opt = opt;
    s.anchor = at;
    MakeTheme(s.theme, DpiForPoint(at), opt.light);

    Popup* root = NewPopup(s, items, nullptr, -1);
    if (!root) {
        chosen = NativeMenu(owner, at, items, opt);
        return true;
    }
    PlaceRoot(s, *root);
    Repaint(*root);
    AnimatePopup(*root, AnimKind::Enter, RootAnchor(*root), 0, s.theme.motion.slide);

    Session* prev = g_session;
    g_session = &s;
    s.running = true;

    SetForegroundWindow(root->hwnd);
    SetFocus(root->hwnd);
    if (GetCapture() != root->hwnd) {
        SetCapture(root->hwnd);
        s.captured = (GetCapture() == root->hwnd);
    }
    // The pointer may already sit over a row when the menu opens (no WM_MOUSEMOVE will fire until it
    // moves). Seed the hover highlight and cursor from its current position.
    {
        POINT pt;
        GetCursorPos(&pt);
        OnMouseMove(s, pt);
    }

    // Peek-driven loop: process pending messages, then advance scroll easing once per turn. A
    // blocking GetMessage + WM_TIMER would cap animation at the timer queue's low-priority tick;
    // driving it here makes the frame rate follow the loop (a frame costs ~1 ms). When nothing is
    // animating, block on MsgWaitForMultipleObjects so an idle menu burns no CPU.
    MSG msg;
    while (s.running) {
        bool animating = AdvanceAnimations(s);
        BOOL got = PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE);
        if (got) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage((int)msg.wParam);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        if (s.running) {
            if (animating)
                Sleep(1);  // yield between animation frames; 1 ms resolution via timeBeginPeriod
            else
                MsgWaitForMultipleObjects(0, nullptr, FALSE, INFINITE, QS_ALLINPUT);
        }
    }

    while (!s.open.empty()) DestroyPopup(s, s.open.back());
    if (s.captured) {
        ReleaseCapture();
        s.captured = false;
    }
    g_session = prev;
    chosen = s.chosen;
    timeEndPeriod(1);
    return true;
}

}  // namespace popup_menu
