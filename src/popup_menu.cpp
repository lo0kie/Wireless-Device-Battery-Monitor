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

struct Theme {
    int scale = 100;
    int rowH = 32, headerH = 26, gapH = 8;
    // `pad` is the margin between the panel border and the whole block of rows, the same on all four
    // sides; `rowInset` is the text inset inside a row, so the hover highlight keeps its own padding.
    int pad = 6, rowInset = 6, checkW = 14, chevW = 10, colGap = 5;
    int radius = 8, pillRadius = 4, minWidth = 180, maxWidth = 320;
    int textH = 15, headerTextH = 13, iconSize = 40;
    Rgba panel, border, text, muted, disabled, hover, mark, scrollBar, sep;
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

void MakeFormat(Com<IDWriteTextFormat>& out, const wchar_t* face, float size, DWRITE_TEXT_ALIGNMENT align) {
    IDWriteFactory* dw = DwFactory();
    if (!dw) return;
    if (FAILED(dw->CreateTextFormat(face, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
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
    auto px = [dpi](int dips) { return MulDiv(dips, (int)dpi, 96); };
    t.rowH = px(32);
    t.headerH = px(26);
    t.gapH = px(8);
    t.pad = px(6);
    t.rowInset = px(6);
    t.checkW = px(14);
    t.chevW = px(10);
    t.colGap = px(5);
    t.radius = px(8);
    t.pillRadius = px(4);
    t.minWidth = px(180);
    t.maxWidth = px(320);
    t.textH = px(15);
    t.headerTextH = px(13);
    t.iconSize = px(40);  // marks (check, radio dot, chevron) are laid out this large, then scaled down

    const wchar_t* ui = FontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    MakeFormat(t.textFmt, ui, (float)t.textH, DWRITE_TEXT_ALIGNMENT_LEADING);
    MakeFormat(t.textFmtRtl, ui, (float)t.textH, DWRITE_TEXT_ALIGNMENT_TRAILING);
    MakeFormat(t.headerFmt, ui, (float)t.headerTextH, DWRITE_TEXT_ALIGNMENT_LEADING);
    MakeFormat(t.headerFmtRtl, ui, (float)t.headerTextH, DWRITE_TEXT_ALIGNMENT_TRAILING);

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

    if (light) {
        t.panel = { 249, 249, 249, 255 };
        t.border = { 0, 0, 0, 26 };
        t.text = { 26, 26, 26, 255 };
        t.muted = { 108, 108, 108, 255 };
        t.disabled = { 160, 160, 160, 255 };
        t.hover = { 0, 0, 0, 16 };
        t.mark = { 26, 26, 26, 255 };
        t.scrollBar = { 0, 0, 0, 64 };
        t.sep = { 0, 0, 0, 26 };
    } else {
        t.panel = { 43, 43, 43, 255 };
        t.border = { 255, 255, 255, 22 };
        t.text = { 242, 242, 242, 255 };
        t.muted = { 168, 168, 168, 255 };
        t.disabled = { 118, 118, 118, 255 };
        t.hover = { 255, 255, 255, 18 };
        t.mark = { 242, 242, 242, 255 };
        t.scrollBar = { 255, 255, 255, 70 };
        t.sep = { 255, 255, 255, 28 };
    }
}

// ---- painter -----------------------------------------------------------------------------------

// A D2D render target bound to the popup's DIB plus the brushes the theme needs. Everything is drawn
// straight into the buffer UpdateLayeredWindow uploads, so there is no intermediate bitmap.
struct Painter {
    ID2D1RenderTarget* rt = nullptr;
    Com<ID2D1SolidColorBrush> panel, border, text, muted, disabled, hover, mark, scroll, sep;

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
            { &sep, t.sep },
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
    }

    void FillRound(const D2D1_RECT_F& r, float radius, ID2D1Brush* b) {
        D2D1_ROUNDED_RECT rr{ r, radius, radius };
        rt->FillRoundedRectangle(&rr, b);
    }

    void StrokeRound(const D2D1_RECT_F& r, float radius, float width, ID2D1Brush* b) {
        D2D1_ROUNDED_RECT rr{ r, radius, radius };
        rt->DrawRoundedRectangle(&rr, b, width);
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
    Com<IDWriteTextLayout> text;
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
    int scroll = 0, maxScroll = 0;
    int hot = -1;
    bool hasCheck = false, hasChev = false;
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
};

Session* g_session = nullptr;
const wchar_t* kClassName = L"BatteryHubMenuPopup";
constexpr UINT_PTR kHoverTimer = 1;
constexpr UINT kHoverDelayMs = 220;
constexpr UINT WM_MENU_DONE = WM_APP + 77;

Popup* PopupAtPoint(Session& s, POINT p);

// ---- layout ------------------------------------------------------------------------------------

// Width of a single-line run, or -1 when DirectWrite could not lay it out.
int TextWidth(IDWriteFactory* dw, IDWriteTextFormat* fmt, const std::wstring& text, int limit, int rowH) {
    Com<IDWriteTextLayout> layout;
    if (FAILED(dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, (FLOAT)limit, (FLOAT)rowH, layout.put())))
        return -1;
    DWRITE_TEXT_METRICS m{};
    if (FAILED(layout->GetMetrics(&m))) return -1;
    return (int)ceilf(m.widthIncludingTrailingWhitespace);
}

IDWriteTextFormat* FormatFor(const Theme& t, const Item& it, bool rtl) {
    if (it.kind == Item::Kind::Header) return t.headerFmt.get();
    return rtl ? t.textFmtRtl.get() : t.textFmt.get();
}

int MeasureText(const Theme& t, const Item& it, bool rtl) {
    if (it.text.empty()) return 0;
    IDWriteFactory* dw = DwFactory();
    IDWriteTextFormat* fmt = FormatFor(t, it, rtl);
    if (!dw || !fmt) return 0;
    return MaxI(0, TextWidth(dw, fmt, it.text, 8192, t.rowH));
}

void LayoutPopup(Session& s, Popup& p) {
    const Theme& t = s.theme;
    int textW = 0;
    p.hasCheck = p.hasChev = false;
    for (const Item& it : *p.items) {
        if (it.kind == Item::Kind::Separator) continue;
        textW = MaxI(textW, MeasureText(t, it, s.opt.rtl));
        p.hasCheck = p.hasCheck || it.checked || it.radio;
        p.hasChev = p.hasChev || !it.children.empty();
    }
    // One slot on the right holds either the check or the submenu arrow, so the labels line up.
    int rightW = (p.hasCheck || p.hasChev) ? MaxI(t.checkW, t.chevW) + t.colGap : 0;
    if (textW) textW += MaxI(2, t.colGap);  // slack: a measured width can come out a pixel short
    int rowW = (t.pad + t.rowInset) * 2 + textW + rightW;
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
};

RowBox BoxFor(const Theme& t, const Popup& p, bool rtl) {
    int rightW = (p.hasCheck || p.hasChev) ? MaxI(t.checkW, t.chevW) + t.colGap : 0;
    RowBox b;
    b.textL = t.pad + t.rowInset + (rtl ? rightW : 0);
    b.textR = p.width - t.pad - t.rowInset - (rtl ? 0 : rightW);
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

void BuildText(const Theme& t, RowDraw& d, const Item& it, int availW, int rowH, bool rtl) {
    if (it.text.empty()) return;
    IDWriteFactory* dw = DwFactory();
    IDWriteTextFormat* fmt = FormatFor(t, it, rtl);
    if (!dw || !fmt) return;
    const int limit = MaxI(8, availW);

    // DirectWrite's own trimming let long labels run past the layout box, so the label is cut here:
    // the longest prefix that still fits the column, plus an ellipsis.
    std::wstring text = it.text;
    int w0 = TextWidth(dw, fmt, text, 8192, rowH);
    if (w0 > limit) {
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
    dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, (FLOAT)limit, (FLOAT)rowH, d.text.put());
}

void BuildDraw(Session& s, Popup& p) {
    const Theme& t = s.theme;
    p.draw.clear();
    p.draw.resize(p.rows.size());
    RowBox box = BoxFor(t, p, s.opt.rtl);
    int availW = MaxI(8, box.textR - box.textL);
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
        BuildText(t, p.draw[i], it, availW, p.rows[i].h, s.opt.rtl);
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

    // Check and chevron share the one slot at the right edge and are drawn at the same size, so a
    // checked row keeps its label where an unchecked one has it.
    const int markW = MaxI(t.checkW, t.chevW);
    const float markCx = rtl ? (float)(t.pad + t.rowInset) + (float)markW * 0.5f
                             : (float)(p.width - t.pad - t.rowInset) - (float)markW * 0.5f;
    const float markSize = (float)markW * 0.8f;
    ID2D1SolidColorBrush* markBrush = it.disabled ? g.disabled.get() : g.muted.get();  // same as the chevron
    if (it.checked && !d.check.empty())
        DrawMask(g.rt, d.check, d.checkW, d.checkH, markCx, cy, markSize, markBrush);
    if (p.hasChev && !it.children.empty() && !d.chevron.empty())
        DrawMask(g.rt, d.chevron, d.chevronW, d.chevronH, markCx, cy, markSize,
                 it.disabled ? g.disabled.get() : g.muted.get());
}

void PaintPopup(Session& s, Popup& p) {
    const Theme& t = s.theme;
    if (p.surface.w != p.width || p.surface.h != p.height) {
        if (!p.surface.Create(p.width, p.height)) return;
    }
    Painter g;
    if (!g.Begin(p.surface, t)) {
        g.End();
        return;
    }
    const D2D1_RECT_F panel{ 0.5f, 0.5f, (float)p.width - 0.5f, (float)p.height - 0.5f };
    g.FillRound(panel, (float)t.radius, g.panel.get());
    g.StrokeRound(panel, (float)t.radius, 1.0f, g.border.get());

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
        float track = (float)(p.height - t.pad * 2);
        float span = (float)p.contentH;
        float barH = std::max(24.0f, track * (float)p.height / span);
        float barY = (float)t.pad + (track - barH) * ((float)p.scroll / (float)p.maxScroll);
        float barW = (float)MaxI(2, MulDiv(3, t.scale, 96));
        float x = s.opt.rtl ? (float)t.pad : (float)(p.width - t.pad) - barW - 1.0f;
        g.FillRound(D2D1_RECT_F{ x, barY, x + barW, barY + barH }, barW * 0.5f, g.scroll.get());
    }
    g.End();
}

// `alpha` is a global opacity on top of the per-pixel one: the fade needs no repaint, only a new
// upload of the same surface.
void Flush(Popup& p, int alpha = 255) {
    if (!p.hwnd || !p.surface.dc) return;
    HDC screen = GetDC(nullptr);
    POINT src{ 0, 0 };
    SIZE size{ p.width, p.height };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, (BYTE)ClampI(alpha, 0, 255), AC_SRC_ALPHA };
    UpdateLayeredWindow(p.hwnd, screen, nullptr, &size, p.surface.dc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

// Windows 11 menus appear instantly; a short slide plus fade keeps that weight while making the panel
// feel attached to the point that opened it. It runs before the message loop, so nothing can be
// clicked mid-animation.
void AnimateOpen(Popup& p, int scale) {
    RECT r{};
    if (!p.hwnd || !GetWindowRect(p.hwnd, &r)) return;
    const int dist = MulDiv(6, scale, 96);
    const int frames = 8;
    for (int i = 1; i <= frames; i++) {
        const float t = (float)i / (float)frames;
        const float e = 1.0f - (1.0f - t) * (1.0f - t);  // ease out
        const int dy = (int)((1.0f - e) * (float)dist + 0.5f);
        SetWindowPos(p.hwnd, nullptr, r.left, r.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        Flush(p, (int)(110.0f + 145.0f * e));
        Sleep(11);
    }
    SetWindowPos(p.hwnd, nullptr, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    Flush(p, 255);
}

// ---- session helpers ---------------------------------------------------------------------------

RECT WindowRect(Popup& p) {
    RECT r{};
    if (p.hwnd) GetWindowRect(p.hwnd, &r);
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

void OpenSubmenu(Session& s, Popup& parent, int rowIdx);

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
        s.pendingPopup = &p;
        s.pendingIndex = rowIdx;
        SetTimer(s.root->hwnd, kHoverTimer, kHoverDelayMs, nullptr);
    } else {
        OpenSubmenu(s, p, rowIdx);
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
    SetWindowPos(child.hwnd, HWND_TOP, x, y, child.width, child.height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

Popup* NewPopup(Session& s, const std::vector<Item>& items, Popup* parent, int parentIndex);

void OpenSubmenu(Session& s, Popup& parent, int rowIdx) {
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
    Repaint(*child);
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
    int maxH = mi.rcWork.bottom - mi.rcWork.top - MulDiv(12, s.theme.scale, 96);
    p->height = MinI(p->contentH, maxH);
    p->maxScroll = MaxI(0, p->contentH - p->height);

    DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW;
    if (parent) ex |= WS_EX_NOACTIVATE;
    p->hwnd = CreateWindowExW(ex, kClassName, L"", WS_POPUP, 0, 0, p->width, p->height, nullptr, nullptr,
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
    SetWindowPos(p.hwnd, HWND_TOPMOST, x, y, p.width, p.height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void OnMouseMove(Session& s, POINT screen) {
    Popup* target = PopupAtPoint(s, screen);
    if (!target) return;
    int row = RowAt(*target, screen);
    SetHot(s, *target, row, true);
}

void OnWheel(Session& s, POINT screen, int delta) {
    Popup* target = PopupAtPoint(s, screen);
    if (!target || target->maxScroll <= 0) return;
    int step = s.theme.rowH * 3;
    target->scroll -= (delta / WHEEL_DELTA) * step;
    if (target->scroll < 0) target->scroll = 0;
    if (target->scroll > target->maxScroll) target->scroll = target->maxScroll;
    Repaint(*target);
}

void OnKey(Session& s, WPARAM key) {
    Popup* p = s.open.empty() ? nullptr : s.open.back();
    if (!p) return;
    switch (key) {
        case VK_ESCAPE: Finish(s, 0); break;
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
                if (!it.children.empty()) OpenSubmenu(s, *p, p->hot);
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
        case WM_MOUSEMOVE:
            if (isRoot && s->running) {
                POINT pt;
                GetCursorPos(&pt);
                OnMouseMove(*s, pt);
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
            if (isRoot && s->running) {
                POINT pt;
                GetCursorPos(&pt);
                Popup* target = PopupAtPoint(*s, pt);
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
            if (isRoot && s->running) {
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
    AnimateOpen(*root, s.theme.scale);

    Session* prev = g_session;
    g_session = &s;
    s.running = true;

    SetForegroundWindow(root->hwnd);
    SetFocus(root->hwnd);
    if (GetCapture() != root->hwnd) {
        SetCapture(root->hwnd);
        s.captured = (GetCapture() == root->hwnd);
    }

    MSG msg;
    while (s.running) {
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage((int)msg.wParam);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    while (!s.open.empty()) DestroyPopup(s, s.open.back());
    if (s.captured) {
        ReleaseCapture();
        s.captured = false;
    }
    g_session = prev;
    chosen = s.chosen;
    return true;
}

}  // namespace popup_menu
