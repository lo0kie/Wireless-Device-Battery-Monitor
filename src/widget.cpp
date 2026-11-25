#include "widget.h"

#include "i18n.h"
#include "icon_font.h"

#include <commctrl.h>
#include <uiautomation.h>
#include <algorithm>
#include <climits>
#include <cmath>

namespace {

HWND g_wnd, g_taskbar, g_tip, g_notify, g_rebar;
UINT g_clickMsg;
bool g_light, g_hover;
bool g_background;  // opaque rounded plate behind the text, for transparent / restyled taskbars
bool g_stacked;     // icon above the percentage (menu option, PR #525)
bool g_showPercentSign = true;  // '%' after numeric levels (menu option, PR #1386)
bool g_ringStyle;   // device icon inside a ring that fills with the charge (menu option, PR #110)
bool g_connectionIcons;  // wireless mark after 2.4 GHz devices' levels (menu option, PR #2277)
bool g_deviceDivider;    // thin line between neighbouring devices on the taskbar (menu option)
bool g_vertical;    // the primary taskbar is docked left or right: one block per device, stacked (PR #321)
bool g_makeRoom = true;  // step past other apps' widgets in the taskbar instead of covering them
widget::VerticalPlace g_place = widget::VerticalPlace::Auto;  // where the column sits on such a taskbar (menu option)
bool g_farLeft;     // at the left end of the taskbar instead of next to the clock (menu option)
bool g_beforeApps;  // just left of Start / the first task button, centred taskbar (menu option, PR #760)
widget::TextStyle g_textStyle;  // user-selected text font, size, letter spacing and device spacing
std::vector<WidgetItem> g_items;
std::wstring g_tipText;
RECT g_lastTray{}, g_lastBar{}, g_lastWidgets{};
int g_lastW = -1, g_lastDpi = 0, g_lastLeft = -1;
SIZE g_size{};
SIZE g_paintedBarClient{};
bool g_painted = false;
HINSTANCE g_inst;

// Copies of the strip on secondary-monitor taskbars (Shell_SecondaryTrayWnd), so the levels stay visible
// while a game runs fullscreen on the main screen. Same drawing, same click and tooltip; each one sits
// left of its taskbar's clock. The primary strip above is unchanged, and with one monitor none exist.
struct Mirror {
    HWND bar = nullptr, wnd = nullptr;
    bool hover = false;
    bool painted = false;
    SIZE size{};
    int dpi = 0;
    int lastLeft = INT_MIN;  // client x it was last placed at; INT_MIN = not placed (hidden)
};
std::vector<Mirror> g_mirrors;
// Which taskbars show the strip: "all", "main", or one monitor's id (MonitorId), e.g. only monitor 3.
// With one monitor chosen, the primary strip is hidden; if that monitor is not connected, the primary
// strip is shown instead so the levels never disappear.
std::wstring g_choice = L"all";
bool g_primaryHidden = false;
HWND g_dpiBar;  // while a mirror is painted or placed, Px() uses its taskbar's DPI

const wchar_t* kClass = L"WirelessBatteryWidget";
const wchar_t* kBridgeClass = L"Windows.UI.Composition.DesktopWindowContentBridge";

int Dpi() {
    HWND bar = g_dpiBar ? g_dpiBar : g_taskbar;
    UINT d = bar ? GetDpiForWindow(bar) : 0;
    return d ? (int)d : 96;
}
int Px(int v) { return MulDiv(v, Dpi(), 96); }
// Same, for the fractional gaps: scale first, round afterwards, or 0.5 and 1 px would both land on 1.
int Pxf(float v) { return (int)std::lround(v * Dpi() / 96.0); }

// Some taskbar customizations report tray geometry that places the strip off-screen (#2993).
// A plain clamp to x=0 is insufficient: the Start/task buttons may cover a child widget there.
// Preserve an earlier valid position; otherwise estimate the normal slot left of the tray.
constexpr int RecoverHorizontalWidgetX(int desired, int widgetWidth, int barWidth,
                                       int trayWidth, int lastGoodX, int gap) {
    const int rightmost = std::max(0, barWidth - widgetWidth);
    if (desired >= 0 && desired <= rightmost) return desired;  // don't touch healthy positioning
    if (lastGoodX >= 0 && lastGoodX <= rightmost) return lastGoodX;
    if (trayWidth > 0 && trayWidth < barWidth) {
        // With invalid tray coordinates, assume the tray is anchored to the taskbar's outer edge.
        return std::clamp(barWidth - trayWidth - widgetWidth - gap, 0, rightmost);
    }
    return std::clamp(desired, 0, rightmost);  // final fallback when tray metrics are unusable
}
static_assert(RecoverHorizontalWidgetX(-61, 59, 1707, 220, 1400, 2) == 1400); // keep valid position
static_assert(RecoverHorizontalWidgetX(-61, 59, 1707, 220, -1, 2) == 1426);   // fresh startup
static_assert(RecoverHorizontalWidgetX(1240, 80, 1707, 220, 900, 2) == 1240);// normal unchanged
static_assert(RecoverHorizontalWidgetX(1720, 80, 1707, 220, -1, 2) == 1405); // right overflow
static_assert(RecoverHorizontalWidgetX(-50, 120, 100, 0, -1, 2) == 0);        // too wide

// Windows 11, left / right taskbar, "Bottom" place: the taskbar's XAML content (task buttons and tray) lives
// in one child window that fills the bar. Making that window `reserve` pixels shorter moves all of its
// content up by as much and leaves the bottom of the bar free for the column, so nothing is covered.
// Explorer gives the window its full height back on its own layout passes, so Reposition() re-applies
// this; 0 restores it. Only a window we shortened ourselves is ever restored.
bool g_reserved;
// The freed strip shows the bare taskbar window, which is not the colour the taskbar's content is drawn
// in (acrylic, or whatever a taskbar theme sets), so the column would sit on a visible band. While the
// bottom place is used, the column is filled with the colour found just above it instead.
bool g_fill;
COLORREF g_fillColor;

// Reads the taskbar's colour from three points on the row just above `screenTop`. Fails when they differ
// (text or an icon is there, e.g. before the content has moved up), so a glyph's colour is never taken.
bool SampleBarColor(const RECT& bar, int screenTop, COLORREF& out) {
    HDC dc = GetDC(nullptr);
    int w = bar.right - bar.left, y = screenTop - Px(4);
    COLORREF c[3];
    for (int i = 0; i < 3; i++) c[i] = GetPixel(dc, bar.left + w * (i + 1) / 4, y);
    ReleaseDC(nullptr, dc);
    auto differs = [](COLORREF a, COLORREF b, int tol) {
        return abs(GetRValue(a) - GetRValue(b)) > tol || abs(GetGValue(a) - GetGValue(b)) > tol ||
               abs(GetBValue(a) - GetBValue(b)) > tol;
    };
    for (COLORREF v : c)
        if (v == CLR_INVALID || differs(v, c[1], 10)) return false;
    out = c[1];
    return true;
}

void ReserveBottom(int reserve) {
    HWND bridge = g_taskbar && IsWindow(g_taskbar) ? FindWindowExW(g_taskbar, nullptr, kBridgeClass, nullptr) : nullptr;
    if (!bridge || (!reserve && !g_reserved)) { g_reserved = false; return; }
    RECT client{}, br{};
    GetClientRect(g_taskbar, &client);
    GetWindowRect(bridge, &br);
    int want = client.bottom - reserve;
    if (want > 0 && br.bottom - br.top != want)
        SetWindowPos(bridge, nullptr, 0, 0, br.right - br.left, want, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    g_reserved = reserve > 0;
}

// Windows 11 puts the Widgets (weather) button right next to the tray when the taskbar is
// left-aligned (start-aligned on right-to-left Windows). It is XAML, so only UI Automation knows
// where it is. The element is looked up once per taskbar; its rectangle is re-read every few
// seconds because the weather text changes its width.
//
// UI Automation calls go to Explorer and wait for its UI thread, while our widget is a child window
// of the taskbar, so Explorer in turn waits for our UI thread whenever it lays out or sends input to
// the widget. Making those calls from our UI thread deadlocks both: Explorer is reported hung
// ("Cross-process", waiting on BatteryHub.exe) and restarted, again and again. So the lookup
// runs on its own thread, which owns no windows, and the UI thread only reads the last rectangle.
SRWLOCK g_wbLock = SRWLOCK_INIT;
RECT g_wbRect{};             // last Widgets button rectangle (screen), empty when unknown
HWND g_wbRectBar;            // taskbar g_wbRect was found in (guarded by g_wbLock)
HWND g_wbTaskbar;            // taskbar to search, null while there is no widget (guarded by g_wbLock)
HANDLE g_wbWake;             // auto-reset: wakes the lookup thread early

// Where a Windows 11 secondary taskbar's clock, widgets button and task buttons are. Its XAML has no child
// windows for them, so the same lookup thread reads them with UI Automation (the clock has AutomationId
// "SystemTrayIcon", widgets button "WidgetsButton", task buttons class "Taskbar.TaskListButtonAutomationPeer";
// found by PR #107).
// On a vertical (left / right) taskbar "left/right" below read as "top/bottom": positions run along the bar.
struct Span {
    HWND bar = nullptr;
    bool measured = false;   // UI Automation answered for this taskbar
    bool hasClock = false;   // the clock can be turned off on secondary taskbars
    int clockLeft = 0;       // screen x (y on a vertical bar)
    bool hasWidgets = false; // Widgets (weather) button on secondary taskbar
    RECT widgets{};          // bounding rectangle of WidgetsButton (screen coords)
    bool hasTasks = false;
    int tasksRight = 0;      // right (bottom) edge of the last task button before the clock (screen x / y)
    bool hasFirst = false;
    int firstLeft = 0;       // left edge of the leftmost Start or task button (screen x), for the far-left spot
    // Right-to-left taskbars (Arabic, Hebrew; from PR #2853): the clock sits at the left end, so the strip
    // goes right of it. clockRight is the clock area's right edge, rtlTasksLeft the left edge of the
    // nearest task button to its right (horizontal bars only).
    bool rtl = false;
    int clockRight = 0;
    bool hasRtlTasks = false;
    int rtlTasksLeft = 0;
};

bool IsVertical(const RECT& r) { return r.bottom - r.top > r.right - r.left; }
// Start and end of a rectangle along a taskbar's length: x for a horizontal bar, y for a vertical one.
int Lo(const RECT& r, bool vertical) { return vertical ? r.top : r.left; }
int Hi(const RECT& r, bool vertical) { return vertical ? r.bottom : r.right; }
std::vector<Span> g_spans;   // guarded by g_wbLock
bool g_wbMirrors;            // guarded by g_wbLock: measure secondary taskbars too
bool g_wbFarLeft;            // guarded by g_wbLock: measure the primary taskbar too (far-left spot)

IUIAutomationCondition* StringCondition(IUIAutomation* uia, PROPERTYID prop, const wchar_t* value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(value);
    IUIAutomationCondition* c = nullptr;
    if (v.bstrVal) uia->CreatePropertyCondition(prop, v, &c);
    VariantClear(&v);
    return c;
}

Span MeasureSpan(IUIAutomation* uia, HWND bar) {
    Span s;
    s.bar = bar;
    RECT br{};
    IUIAutomationElement* root = nullptr;
    if (!GetWindowRect(bar, &br) || FAILED(uia->ElementFromHandle(bar, &root)) || !root) return s;
    s.measured = true;
    s.rtl = (GetWindowLongW(bar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) != 0;
    bool v = IsVertical(br);
    auto inBar = [&](const RECT& r) {
        return Hi(r, v) > Lo(r, v) && Lo(r, v) >= Lo(br, v) && Hi(r, v) <= Hi(br, v);
    };
    if (IUIAutomationCondition* clock = StringCondition(uia, UIA_AutomationIdPropertyId, L"SystemTrayIcon")) {
        IUIAutomationElementArray* all = nullptr;
        int n = 0;
        if (SUCCEEDED(root->FindAll(TreeScope_Descendants, clock, &all)) && all && SUCCEEDED(all->get_Length(&n)))
            for (int i = 0; i < n; i++) {  // leftmost of the clock area's buttons
                IUIAutomationElement* el = nullptr;
                RECT r{};
                if (SUCCEEDED(all->GetElement(i, &el)) && el && SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) && inBar(r)) {
                    if (!s.hasClock || Lo(r, v) < s.clockLeft) s.clockLeft = Lo(r, v);
                    if (!s.hasClock || r.right > s.clockRight) s.clockRight = r.right;
                    s.hasClock = true;
                }
                if (el) el->Release();
            }
        if (all) all->Release();
        clock->Release();
    }
    if (IUIAutomationCondition* wbCond = StringCondition(uia, UIA_AutomationIdPropertyId, L"WidgetsButton")) {
        IUIAutomationElement* wbEl = nullptr;
        RECT r{};
        if (SUCCEEDED(root->FindFirst(TreeScope_Descendants, wbCond, &wbEl)) && wbEl &&
            SUCCEEDED(wbEl->get_CurrentBoundingRectangle(&r)) && inBar(r)) {
            s.widgets = r;
            s.hasWidgets = true;
        }
        if (wbEl) wbEl->Release();
        wbCond->Release();
    }
    if (IUIAutomationCondition* task = StringCondition(uia, UIA_ClassNamePropertyId, L"Taskbar.TaskListButtonAutomationPeer")) {
        IUIAutomationElementArray* all = nullptr;
        int n = 0;
        if (SUCCEEDED(root->FindAll(TreeScope_Descendants, task, &all)) && all && SUCCEEDED(all->get_Length(&n)))
            for (int i = 0; i < n; i++) {
                IUIAutomationElement* el = nullptr;
                RECT r{};
                if (SUCCEEDED(all->GetElement(i, &el)) && el && SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) && inBar(r)) {
                    if (!s.hasFirst || r.left < s.firstLeft) {
                        s.firstLeft = r.left;
                        s.hasFirst = true;
                    }
                    if ((!s.hasClock || Hi(r, v) <= s.clockLeft) && (!s.hasWidgets || Hi(r, v) <= Lo(s.widgets, v)) &&
                        (!s.hasTasks || Hi(r, v) > s.tasksRight)) {
                        s.tasksRight = Hi(r, v);
                        s.hasTasks = true;
                    }
                    if (s.rtl && !v && (!s.hasClock || r.left >= s.clockRight) &&
                        (!s.hasWidgets || r.left >= s.widgets.right) && (!s.hasRtlTasks || r.left < s.rtlTasksLeft)) {
                        s.rtlTasksLeft = r.left;
                        s.hasRtlTasks = true;
                    }
                }
                if (el) el->Release();
            }
        if (all) all->Release();
        task->Release();
    }
    if (IUIAutomationCondition* start = StringCondition(uia, UIA_AutomationIdPropertyId, L"StartButton")) {
        IUIAutomationElement* el = nullptr;
        RECT r{};
        if (SUCCEEDED(root->FindFirst(TreeScope_Descendants, start, &el)) && el &&
            SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) && inBar(r) && (!s.hasFirst || r.left < s.firstLeft)) {
            s.firstLeft = r.left;
            s.hasFirst = true;
        }
        if (el) el->Release();
        start->Release();
    }
    root->Release();
    return s;
}

bool IsXamlTaskbar(HWND bar) {
    return FindWindowExW(bar, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr) != nullptr;
}

// A left / right Windows 11 taskbar: the screen-y range its XAML buttons (Start, search, task buttons)
// take above the notification area. The task list is centred and grows towards the tray, and being XAML
// it cannot be shortened like Windows 10's ReBarWindow32, so the column has to step out of its way.
struct Column {
    HWND bar = nullptr;
    bool measured = false;  // UI Automation answered for this taskbar
    bool any = false;       // at least one button found
    int top = 0, bottom = 0;
};
Column g_column;  // guarded by g_wbLock

bool IsVerticalBar(HWND bar) {
    RECT r{};
    return GetWindowRect(bar, &r) && r.bottom - r.top > r.right - r.left;
}

Column MeasureColumn(IUIAutomation* uia, HWND bar) {
    Column c;
    c.bar = bar;
    RECT br{}, tr{};
    HWND tray = FindWindowExW(bar, nullptr, L"TrayNotifyWnd", nullptr);
    IUIAutomationElement* root = nullptr;
    if (!tray || !GetWindowRect(tray, &tr) || !GetWindowRect(bar, &br)) return c;
    if (FAILED(uia->ElementFromHandle(bar, &root)) || !root) return c;
    // XAML elements have no window of their own; this leaves out our strip and the taskbar's legacy
    // child windows (ReBarWindow32 and friends still exist on Windows 11, with stale rectangles).
    VARIANT none;
    VariantInit(&none);
    none.vt = VT_I4;
    none.lVal = 0;
    IUIAutomationCondition* xaml = nullptr;
    IUIAutomationCacheRequest* req = nullptr;
    IUIAutomationElementArray* all = nullptr;
    int n = 0;
    // One cross-process call for all rectangles.
    if (SUCCEEDED(uia->CreatePropertyCondition(UIA_NativeWindowHandlePropertyId, none, &xaml)) && xaml &&
        SUCCEEDED(uia->CreateCacheRequest(&req)) && req &&
        SUCCEEDED(req->AddProperty(UIA_BoundingRectanglePropertyId)) &&
        SUCCEEDED(root->FindAllBuildCache(TreeScope_Descendants, xaml, req, &all)) && all &&
        SUCCEEDED(all->get_Length(&n))) {
        c.measured = true;
        for (int i = 0; i < n; i++) {
            IUIAutomationElement* el = nullptr;
            RECT r{};
            // Elements reaching below the tray's top are the tray icons and the frame around everything.
            if (SUCCEEDED(all->GetElement(i, &el)) && el && SUCCEEDED(el->get_CachedBoundingRectangle(&r)) &&
                r.bottom > r.top && r.right > r.left && r.left >= br.left && r.right <= br.right && r.top >= br.top && r.bottom <= tr.top) {
                c.top = c.any ? std::min(c.top, (int)r.top) : (int)r.top;
                c.bottom = c.any ? std::max(c.bottom, (int)r.bottom) : (int)r.bottom;
                c.any = true;
            }
            if (el) el->Release();
        }
    }
    if (all) all->Release();
    if (req) req->Release();
    if (xaml) xaml->Release();
    root->Release();
    return c;
}

DWORD WINAPI WidgetsButtonThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    IUIAutomationElement* btn = nullptr;
    HWND btnBar = nullptr;
    bool column = false;  // vertical XAML taskbar: its task buttons come and go, so look more often
    for (;;) {
        // No widget (tray-icon mode): sleep until one.
        WaitForSingleObject(g_wbWake, btnBar ? (column ? 1500 : 5000) : INFINITE);
        AcquireSRWLockShared(&g_wbLock);
        HWND bar = g_wbTaskbar;
        ReleaseSRWLockShared(&g_wbLock);
        if (btn && bar != btnBar) { btn->Release(); btn = nullptr; }
        btnBar = bar;
        RECT r{};
        if (bar && IsWindow(bar)) {
            if (!uia)
                CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&uia);
            if (uia && !btn) {
                IUIAutomationElement* root = nullptr;
                IUIAutomationCondition* cond = nullptr;
                VARIANT id;
                VariantInit(&id);
                id.vt = VT_BSTR;
                id.bstrVal = SysAllocString(L"WidgetsButton");
                if (SUCCEEDED(uia->ElementFromHandle(bar, &root)) && root &&
                    SUCCEEDED(uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, id, &cond)) && cond)
                    root->FindFirst(TreeScope_Descendants, cond, &btn);
                VariantClear(&id);
                if (cond) cond->Release();
                if (root) root->Release();
            }
            if (btn && FAILED(btn->get_CurrentBoundingRectangle(&r))) {
                btn->Release();  // stale (Widgets turned off, Explorer restarted): look it up again next time
                btn = nullptr;
                r = {};
            }
        }
        AcquireSRWLockShared(&g_wbLock);
        bool mirrors = g_wbMirrors, farLeft = g_wbFarLeft;
        ReleaseSRWLockShared(&g_wbLock);
        std::vector<Span> spans;
        if (bar && farLeft && uia && IsWindow(bar) && IsXamlTaskbar(bar)) spans.push_back(MeasureSpan(uia, bar));
        if (bar && mirrors && uia)
            for (HWND t = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr); t;
                 t = FindWindowExW(nullptr, t, L"Shell_SecondaryTrayWnd", nullptr))
                if (IsXamlTaskbar(t)) spans.push_back(MeasureSpan(uia, t));  // Windows 10 bars have real child windows
        Column col;
        column = bar && uia && IsWindow(bar) && IsVerticalBar(bar) && IsXamlTaskbar(bar);
        if (column) col = MeasureColumn(uia, bar);
        AcquireSRWLockExclusive(&g_wbLock);
        g_column = col;
        if (bar && g_wbTaskbar == bar) {  // drop a result for a taskbar we no longer sit on
            g_wbRect = r;
            g_wbRectBar = bar;
        }
        g_spans = std::move(spans);
        ReleaseSRWLockExclusive(&g_wbLock);
    }
}

// Points the lookup thread at `taskbar` (null to idle it), starting the thread on first use.
// The last rectangle is kept for its taskbar, so switching from tray-icon back to taskbar mode on the
// same taskbar steps past the Widgets button straight away instead of after the next lookup.
void WatchWidgetsButton(HWND taskbar) {
    AcquireSRWLockExclusive(&g_wbLock);
    g_wbTaskbar = taskbar;
    ReleaseSRWLockExclusive(&g_wbLock);
    if (!g_wbWake) {
        g_wbWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE t = g_wbWake ? CreateThread(nullptr, 0, WidgetsButtonThread, nullptr, 0, nullptr) : nullptr;
        if (t) CloseHandle(t);
    }
    if (g_wbWake) SetEvent(g_wbWake);
}

// Never blocks: returns what the lookup thread last found on the current taskbar.
RECT WidgetsButtonRect() {
    AcquireSRWLockShared(&g_wbLock);
    RECT r = g_wbRectBar && g_wbRectBar == g_wbTaskbar ? g_wbRect : RECT{};
    ReleaseSRWLockShared(&g_wbLock);
    return r;
}

// Screen x for a strip `width` wide at the left end of a Windows 11 taskbar, right of the Widgets button
// when that sits there (centred taskbar). False while the taskbar is not measured yet, or when the Start or
// task buttons reach into the spot (left-aligned taskbar, many open windows): the strip then stays next to
// the clock rather than cover a button.
bool FarLeftX(HWND bar, int width, int& x) {
    RECT br{};
    if (width <= 0 || !GetWindowRect(bar, &br)) return false;
    bool ok = false;
    AcquireSRWLockShared(&g_wbLock);
    for (const auto& s : g_spans)
        if (s.bar == bar && s.measured && s.hasFirst && g_beforeApps) {
            // Right next to the taskbar apps (PR #760): just left of Start, or left of the Widgets button
            // when that sits between the free space and Start. Needs room between the bar's edge and them.
            int edge = s.firstLeft;
            if (s.hasWidgets && s.widgets.right <= s.firstLeft && s.widgets.right + Px(4) >= s.firstLeft - width - Px(4))
                edge = s.widgets.left;
            x = edge - width - Px(4);
            ok = x >= br.left + Px(4);
        } else if (s.bar == bar && s.measured && s.hasFirst) {
            x = br.left + Px(4);
            if (s.hasWidgets && s.widgets.left < x + width + Px(4) && s.widgets.right > x) x = s.widgets.right + Px(4);
            ok = x + width + Px(4) <= s.firstLeft;
            if (!ok && s.hasTasks) {
                // No room before Start (left-aligned taskbar, from PR #2519): take the first free spot from the
                // left instead, right after the task buttons, as long as it stays clear of the Widgets button
                // and the clock. Only when that does not fit either does the strip go back next to the clock.
                x = s.tasksRight + Px(4);
                int limit = s.hasClock ? s.clockLeft : (int)br.right;
                if (s.hasWidgets && s.widgets.left >= x) limit = std::min(limit, (int)s.widgets.left);
                ok = x + width + Px(4) <= limit;
            }
        }
    ReleaseSRWLockShared(&g_wbLock);
    return ok;
}

void ApplyTipTheme() {
    if (!g_tip) return;
    using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto setTheme = ux ? (SetWindowThemeFn)GetProcAddress(ux, "SetWindowTheme") : nullptr;
    if (setTheme) {
        setTheme(g_tip, g_light ? L"Explorer" : L"DarkMode_Explorer", nullptr);
        SendMessageW(g_tip, WM_THEMECHANGED, 0, 0);
    }
}

wchar_t Glyph(DeviceKind k, bool bluetooth) {
    return icons::Device(k, bluetooth);  // Material Icons, with Segoe fallback
}

// Creates a font, falling back when the preferred face is not installed (GDI would silently substitute).
HFONT MakeFont(int px, int weight, const wchar_t* face, const wchar_t* fallback, int fallbackPx = 0) {
    for (const wchar_t* f : { face, fallback }) {
        int h_px = (f == fallback && fallbackPx > 0) ? fallbackPx : px;
        HFONT h = CreateFontW(-h_px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, f);
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(dc, h);
        wchar_t got[LF_FACESIZE] = {};
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, o);
        DeleteDC(dc);
        if (!_wcsicmp(got, f) || f == fallback) return h;
        DeleteObject(h);
    }
    return nullptr;
}

// Text captions follow the text size; icons follow their own size setting. The defaults (13 / 16 px)
// keep the original proportions.
int TextPx(int base) { return MulDiv(base, g_textStyle.size, 13); }
int IconPx(int base) { return MulDiv(base, g_textStyle.iconSize, 16); }
// Device gap for the three layouts: the normal row uses the full value, stacked and vertical use less.
int GapPx(int base = 14) {
    return Pxf(std::max(2.0f, g_textStyle.itemGap * base / 14.0f));
}
// Distance between a device icon and its percentage text (the "text spacing" setting).
int TextGapPx() { return Pxf(g_textStyle.iconGap); }
// Separator line thickness, and the gap between devices when the separator is on. One padding value
// is used on both sides of the line, so together with the line it sets the device spacing.
int DividerLineW() { return std::max(2, Px(2)); }
int DividerPadPx() { return Pxf(g_textStyle.dividerGap); }
int DividerGapPx() { return DividerPadPx() * 2 + DividerLineW(); }
// Gap between neighbouring devices: with the separator on it is made of the two custom paddings
// around the line, otherwise the ordinary device-spacing value is used.
int DeviceGapPx(int base = 14) { return g_deviceDivider ? DividerGapPx() : GapPx(base); }

// Grayscale coverage mask of a string, cropped to its ink bounds.
struct Mask {
    int w = 0, h = 0;
    std::vector<BYTE> a;
};

Mask RenderMask(const std::wstring& s, HFONT font, bool darkOnLight, float extra = 0.0f) {
    Mask m;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ of = SelectObject(dc, font);
    // GDI's character extra is an integer, so a fractional value is applied by placing the characters
    // one by one at accumulated (sub-pixel) positions: 0.5 px then really is half of 1 px instead of
    // being rounded away. Whole values keep the single TextOut path.
    const bool fractional = std::fabs(extra - std::round(extra)) > 0.01f;
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    int W = sz.cx + 8;
    if (fractional || extra > 0.0f) W = sz.cx + (int)std::ceil(extra * (float)s.size()) + 8;
    const int H = sz.cy + 8;
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ ob = SelectObject(dc, bmp);
    memset(bits, darkOnLight ? 0xFF : 0x00, (size_t)W * H * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, darkOnLight ? RGB(0, 0, 0) : RGB(255, 255, 255));
    if (fractional) {
        // Accumulate the pen in floating point and round only the draw position, so the extra space
        // is spread evenly instead of being truncated after the first character.
        const int fallback = s.empty() ? 0 : sz.cx / (int)s.size();
        double pen = 4.0;
        for (wchar_t c : s) {
            TextOutW(dc, (int)std::lround(pen), 4, &c, 1);
            int advance = 0;
            if (!GetCharWidth32W(dc, c, c, &advance)) advance = fallback;
            pen += advance + extra;
        }
    } else {
        SetTextCharacterExtra(dc, (int)std::lround(extra));
        TextOutW(dc, 4, 4, s.c_str(), (int)s.size());
    }
    GdiFlush();
    auto* p = static_cast<DWORD*>(bits);
    auto cover = [&](DWORD v) -> BYTE {
        int c = (int)((v & 0xFF) + ((v >> 8) & 0xFF) + ((v >> 16) & 0xFF)) / 3;
        return (BYTE)(darkOnLight ? 255 - c : c);
    };
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (cover(p[y * W + x])) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    if (x1 >= 0) {
        m.w = x1 - x0 + 1;
        m.h = y1 - y0 + 1;
        m.a.resize((size_t)m.w * m.h);
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) m.a[y * m.w + x] = cover(p[(y0 + y) * W + x0 + x]);
    }
    SelectObject(dc, ob);
    SelectObject(dc, of);
    DeleteObject(bmp);
    DeleteDC(dc);
    return m;
}

struct Rgb { int r, g, b; };

struct Canvas {
    int w, h;
    std::vector<DWORD> px;  // premultiplied BGRA
    Canvas(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_, 0) {}
    void Blend(int x, int y, Rgb c, int a) {  // source-over, a = 0..255
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        DWORD d = px[y * w + x];
        int inv = 255 - a;
        int da = d >> 24, dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
        int oa = a + da * inv / 255;
        int orr = c.r * a / 255 + dr * inv / 255;
        int og = c.g * a / 255 + dg * inv / 255;
        int ob = c.b * a / 255 + db * inv / 255;
        px[y * w + x] = ((DWORD)oa << 24) | ((DWORD)orr << 16) | ((DWORD)og << 8) | (DWORD)ob;
    }
    void Draw(const Mask& m, int x, int y, Rgb c, int alpha) {
        for (int j = 0; j < m.h; j++)
            for (int i = 0; i < m.w; i++) Blend(x + i, y + j, c, m.a[j * m.w + i] * alpha / 255);
    }
};

// Soft glow around a glyph, like a lit RGB device: the coverage mask padded by r pixels, box-blurred
// twice and brightened so a thin icon outline still gives a visible glow.
Mask Halo(const Mask& m, int r) {
    Mask g;
    g.w = m.w + 2 * r;
    g.h = m.h + 2 * r;
    std::vector<int> a((size_t)g.w * g.h, 0), tmp(a.size(), 0);
    for (int y = 0; y < m.h; y++)
        for (int x = 0; x < m.w; x++) a[(y + r) * g.w + x + r] = m.a[y * m.w + x];
    int n = 2 * r + 1;
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < g.h; y++)
            for (int x = 0; x < g.w; x++) {
                int s = 0;
                for (int k = -r; k <= r; k++) if (x + k >= 0 && x + k < g.w) s += a[y * g.w + x + k];
                tmp[y * g.w + x] = s / n;
            }
        for (int y = 0; y < g.h; y++)
            for (int x = 0; x < g.w; x++) {
                int s = 0;
                for (int k = -r; k <= r; k++) if (y + k >= 0 && y + k < g.h) s += tmp[(y + k) * g.w + x];
                a[y * g.w + x] = s / n;
            }
    }
    g.a.resize(a.size());
    for (size_t i = 0; i < a.size(); i++) g.a[i] = (BYTE)std::min(255, a[i] * 3);
    return g;
}

// One drawn piece of the strip. Normal pieces follow each other from left to right. A piece with an
// `anchor` (index of an earlier piece) is a caption: drawn over that piece, `bx`/`by` from its top-left
// corner, and taking no room of its own. Branded devices add a glow behind the glyph and a level bar.
struct Piece {
    Mask m;
    Rgb color;
    int alpha;
    int gapAfter;
    int anchor = -1;
    int bx = 0, by = 0;
    int barPct = -1;  // >= 0: thin level bar under the text (branded devices)
    Rgb barColor{};
    Mask halo;        // glow drawn behind the glyph (branded devices)
    int haloR = 0, haloAlpha = 0;
    int px = INT_MIN, py = 0;  // fixed top-left position (stacked and vertical layouts); INT_MIN = flow left to right
    bool deviceEnd = false;  // last piece of a device that has a following device (gapAfter holds the gap)
};

// Device separators, filled by Layout() and drawn by PaintStrip(): filled rects in strip pixels.
struct Divider { int x, y, w, h; };
std::vector<Divider> g_divs;

// Coverage masks for a ring of diameter d (PR #110): the full circle (track) and the arc that starts at
// 12 o'clock and runs clockwise over `fraction` of it. 3x3 supersampled for smooth edges.
void RingMasks(int d, double stroke, double fraction, Mask& track, Mask& arc) {
    const double kTwoPi = 6.283185307179586;
    track.w = track.h = arc.w = arc.h = d;
    track.a.assign((size_t)d * d, 0);
    arc.a.assign((size_t)d * d, 0);
    double R = d / 2.0, inner = R - stroke;
    for (int y = 0; y < d; y++)
        for (int x = 0; x < d; x++) {
            int t = 0, a = 0;
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 3; sx++) {
                    double px = x + (sx + 0.5) / 3 - R, py = y + (sy + 0.5) / 3 - R;
                    double r = std::sqrt(px * px + py * py);
                    if (r > R || r < inner) continue;
                    t++;
                    double ang = std::atan2(px, -py);
                    if (ang < 0) ang += kTwoPi;
                    if (ang <= fraction * kTwoPi) a++;
                }
            track.a[(size_t)y * d + x] = (BYTE)(t * 255 / 9);
            arc.a[(size_t)y * d + x] = (BYTE)(a * 255 / 9);
        }
}

// Three arrangements of the same pieces:
//  * normal: "[icon] 30%  [icon] 93%" in one row (unchanged default)
//  * stacked (menu option, from PR #525): each device's icon above its percentage, to save width;
//    used only when the taskbar is tall enough, otherwise the normal row
//  * vertical (left / right taskbar, from PR #321): one device per row block, stacked top to bottom
// `width` and `height` return the strip's size; in the vertical layout the width is the bar's.
std::vector<Piece> Layout(int barW, int barH, int& width, int& height) {
    g_divs.clear();
    Rgb fg = g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 };
    Rgb red = g_light ? Rgb{ 196, 30, 40 } : Rgb{ 255, 90, 90 };
    Rgb charge = g_light ? Rgb{ 0, 120, 212 } : Rgb{ 96, 205, 255 };
    Rgb full = g_light ? Rgb{ 16, 124, 16 } : Rgb{ 108, 203, 95 };  // Windows "success" green
    HFONT iconFont = icons::Ready() ? icons::Font(Px(IconPx(16)))
                                    : MakeFont(Px(IconPx(16)), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
    HFONT boltFont = icons::Ready() ? icons::Font(Px(IconPx(11)))
                                    : MakeFont(Px(IconPx(11)), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
    // A user-chosen face replaces the Windows UI font for both the percentage and its tiny caption.
    // GDI substitutes unknown faces silently, so MakeFont checks the face name and falls back to Segoe UI.
    auto uiFont = [&](int px, int weight) {
        return g_textStyle.face.empty() ? MakeFont(px, weight, L"Segoe UI Variable Text", L"Segoe UI", px - Px(1))
                                        : MakeFont(px, weight, g_textStyle.face.c_str(), L"Segoe UI", px);
    };
    HFONT text = uiFont(Px(g_textStyle.size), g_textStyle.weight);
    HFONT caption = uiFont(Px(std::max(8, TextPx(7))), g_textStyle.weight);  // never under 8 px
    // Ring style (PR #110) is drawn in the normal one-row layout only; stacked and vertical keep the text.
    const bool ring = g_ringStyle && !g_stacked && barH <= barW;
    HFONT ringIconFont = ring ? (icons::Ready() ? icons::Font(Px(IconPx(11)))
                                                     : MakeFont(Px(IconPx(11)), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets"))
                               : nullptr;

    std::vector<Piece> out;
    struct Slot { int icon, label, bolt; };  // indices of one device's pieces
    std::vector<Slot> slots;
    if (g_items.empty()) out.push_back({ RenderMask(std::wstring(1, icons::G().batteryUnknown), iconFont, g_light), fg, 110, 0 });
    for (size_t i = 0; i < g_items.size(); i++) {
        const auto& it = g_items[i];
        int a = it.dim ? 120 : 255;
        if (ring) {
            // The arc is the charge: red when low, blue while charging; the faint full circle is the track.
            int d = Px(IconPx(28));
            d += d & 1;  // even, so the ring and the icon share an exact centre
            Mask track, arc;
            RingMasks(d, std::max(2.0, d / 13.0), it.percent >= 0 ? it.percent / 100.0 : 0.0, track, arc);
            int base = (int)out.size();
            int ringGap = i + 1 < g_items.size() ? DeviceGapPx(8) : 0;
            out.push_back({ std::move(track), fg, a * (g_light ? 45 : 60) / 255, ringGap });
            out.back().deviceEnd = ringGap > 0;
            Piece arcPiece{ std::move(arc), it.charging ? charge : it.low ? red : fg, a, 0 };
            arcPiece.anchor = base;
            out.push_back(std::move(arcPiece));
            Piece glyph{ RenderMask(std::wstring(1, Glyph(it.kind, it.bluetooth)), ringIconFont, g_light), fg, a, 0 };
            glyph.anchor = base;
            glyph.bx = (d - glyph.m.w) / 2;
            glyph.by = (d - glyph.m.h) / 2;
            out.push_back(std::move(glyph));
            continue;
        }
        Rgb tc = it.low ? red : fg;
        bool mark = it.charging || it.full;  // bolt while charging, green check mark once full
        // The connection mark is shown on every device, including offline ones -- for those it is
        // the only hint left about how the device used to be connected. Charging wins over it.
        bool link = !mark && g_connectionIcons && it.link != LinkKind::None;
        size_t glyph = out.size();
        slots.push_back({ (int)glyph, (int)glyph + 1, mark || link ? (int)glyph + 2 : -1 });
        bool branded = it.accent != Accent::None;
        BrandColor bc = AccentColor(it.accent, g_light);
        Rgb accent{ bc.r, bc.g, bc.b };
        Piece icon{ RenderMask(std::wstring(1, Glyph(it.kind, it.bluetooth)), iconFont, g_light), branded ? accent : fg, a, TextGapPx() };
        if (branded && !it.dim) {
            icon.haloR = Px(3);
            icon.halo = Halo(icon.m, icon.haloR);
            icon.haloAlpha = g_light ? 60 : 140;
        }
        out.push_back(std::move(icon));
        std::wstring t = !it.text.empty() ? it.text : it.percent >= 0 ? std::to_wstring(it.percent) + (g_showPercentSign ? L"%" : L"") : L"?";
        Piece label{ RenderMask(t, text, g_light, g_textStyle.letterSpacing * Dpi() / 96.0f), tc, a,
                      mark || link ? TextGapPx() : 0 };
        if (branded && it.percent >= 0) {
            label.barPct = it.percent;
            label.barColor = it.low ? red : accent;
        }
        out.push_back(std::move(label));
        if (mark) out.push_back({ RenderMask(std::wstring(1, it.full ? icons::G().check : icons::G().bolt), boltFont, g_light), it.full ? full : charge, a, 0 });
        else if (link) {
            const wchar_t g = it.link == LinkKind::Bluetooth ? icons::G().bluetooth
                            : it.link == LinkKind::Wired      ? icons::G().usb
                                                              : icons::G().wifi;
            out.push_back({ RenderMask(std::wstring(1, g), boltFont, g_light), fg, a * 160 / 255, 0 });
        }
        out.back().gapAfter = i + 1 < g_items.size() ? DeviceGapPx() : 0;
        out.back().deviceEnd = out.back().gapAfter > 0;
        if (!it.label.empty()) {  // tiny caption in the bottom-right corner of the icon
            Piece c{ RenderMask(it.label, caption, g_light), fg, a, 0 };
            c.anchor = (int)glyph;
            c.bx = out[glyph].m.w - c.m.w + Px(3);
            c.by = out[glyph].m.h - c.m.h + Px(3);
            out.push_back(c);
        }
    }
    DeleteObject(iconFont);
    DeleteObject(boltFont);
    DeleteObject(text);
    DeleteObject(caption);
    if (ringIconFont) DeleteObject(ringIconFont);

    height = barH;
    const int rowGap = TextGapPx(), barRoom = Px(5);  // barRoom: space for a brand level bar under the text
    int iconH = 0, textH = 0;
    for (const Slot& s : slots) {
        iconH = std::max(iconH, out[s.icon].m.h);
        textH = std::max(textH, out[s.label].m.h);
    }
    bool vertical = barH > barW;  // per bar: a secondary taskbar can be docked differently from the main one
    bool stacked = !vertical && g_stacked && !slots.empty() && barH >= iconH + rowGap + textH + barRoom + Px(6);
    if (vertical) {
        // One block per device, centred in the bar's width: icon, then the percentage (and bolt) under it.
        // Spaced like the notification area's own icons, so the column reads as part of it.
        width = barW;
        const int pad = Px(10), blockGap = DeviceGapPx(12), iconGap = TextGapPx();
        int y = pad;
        if (slots.empty()) {
            // No devices yet (startup, or the last one disconnected): only the placeholder icon. Without this
            // branch the row layout below would keep height = barH, and on a left / right taskbar barH is the
            // whole screen height, so the strip covered the entire taskbar.
            Piece& ic = out[0];
            ic.px = (barW - ic.m.w) / 2;
            ic.py = y;
            height = y + ic.m.h + pad;
            return out;
        }
        for (size_t si = 0; si < slots.size(); si++) {
            const Slot& s = slots[si];
            Piece& ic = out[s.icon];
            Piece& lb = out[s.label];
            int boltW = s.bolt >= 0 ? Px(2) + out[s.bolt].m.w : 0;
            ic.px = (barW - ic.m.w) / 2;
            ic.py = y;
            y += ic.m.h + iconGap;
            lb.px = (barW - lb.m.w - boltW) / 2;
            lb.py = y;
            if (s.bolt >= 0) {
                out[s.bolt].px = lb.px + lb.m.w + TextGapPx();
                out[s.bolt].py = y + (lb.m.h - out[s.bolt].m.h) / 2;
            }
            y += lb.m.h + (lb.barPct >= 0 ? barRoom : 0) + blockGap;
            // Horizontal rule in the gap between two stacked device blocks, after the left (top) padding.
            if (g_deviceDivider && si + 1 < slots.size()) {
                int ty = y - blockGap + DividerPadPx();
                int dw = DividerLineW();
                g_divs.push_back({ Px(10), ty, barW - Px(20), dw });
            }
        }
        height = y - blockGap + pad;
        return out;
    }
    if (stacked) {
        // Icon on top (bolt to its right), percentage centred under it; the pair centred in the bar's height.
        int top = (barH - (iconH + rowGap + textH + barRoom)) / 2;
        int x = Px(8);
        for (size_t si = 0; si < slots.size(); si++) {
            const Slot& s = slots[si];
            Piece& ic = out[s.icon];
            Piece& lb = out[s.label];
            int boltW = s.bolt >= 0 ? Px(2) + out[s.bolt].m.w : 0;
            int slotW = std::max(ic.m.w + boltW, lb.m.w);
            ic.px = x + (slotW - ic.m.w - boltW) / 2;
            ic.py = top + (iconH - ic.m.h) / 2;
            if (s.bolt >= 0) {
                out[s.bolt].px = ic.px + ic.m.w + Px(2);
                out[s.bolt].py = top + (iconH - out[s.bolt].m.h) / 2;
            }
            lb.px = x + (slotW - lb.m.w) / 2;
            lb.py = top + iconH + rowGap;
            int deviceGap = DeviceGapPx(10);
            x += slotW + deviceGap;
            // Vertical rule between two side-by-side device blocks: the gap is left padding + line +
            // right padding, so place the line after the left padding rather than dead centre.
            if (g_deviceDivider && si + 1 < slots.size()) {
                int lx = x - deviceGap + DividerPadPx();
                int dh = iconH + rowGap + textH;
                int dw = DividerLineW();
                g_divs.push_back({ lx, top, dw, dh });
            }
        }
        width = x - DeviceGapPx(10) + Px(8);
        return out;
    }
    width = Px(8) * 2;
    int flowX = Px(8);
    for (auto& p : out) {
        if (p.anchor < 0) {
            if (g_deviceDivider && p.deviceEnd && p.gapAfter > 0) {
                int lx = flowX + p.m.w + DividerPadPx();
                int dh = Px(12);  // short rule centred in the strip
                g_divs.push_back({ lx, (height - dh) / 2, DividerLineW(), dh });
            }
            flowX += p.m.w + p.gapAfter;
            width += p.m.w + p.gapAfter;
        }
    }
    return out;
}

// Draws the strip for one window: the primary one or a mirror on a secondary taskbar.
// Returns true only when the layered surface was updated with a valid non-empty size.
bool PaintStrip(HWND wnd, HWND taskbar, bool hover, SIZE& size) {
    if (!wnd || !taskbar || !IsWindow(wnd) || !IsWindow(taskbar)) return false;
    RECT bar{};
    if (!GetClientRect(taskbar, &bar)) return false;
    int W = 0, H = 0;
    auto pieces = Layout(bar.right - bar.left, bar.bottom - bar.top, W, H);
    if (W <= 0 || H <= 0) return false;
    Canvas cv(W, H);

    // Nearly transparent background so the whole strip receives mouse input,
    // plus a rounded hover highlight like the system tray icons have.
    DWORD clear = 0x01000000;
    if (wnd == g_wnd && g_fill)  // bottom place on a vertical taskbar: continue the taskbar's colour
        clear = 0xFF000000 | ((DWORD)GetRValue(g_fillColor) << 16) | ((DWORD)GetGValue(g_fillColor) << 8) |
                GetBValue(g_fillColor);
    for (auto& p : cv.px) p = clear;
    auto roundRect = [&](int inset, int r, Rgb c, int a) {
        for (int y = inset; y < H - inset; y++)
            for (int x = 0; x < W; x++) {
                int dx = x < r ? r - x : (x >= W - r ? x - (W - r - 1) : 0);
                int dy = y < inset + r ? inset + r - y : (y >= H - inset - r ? y - (H - inset - r - 1) : 0);
                if (dx * dx + dy * dy <= r * r) cv.Blend(x, y, c, a);
            }
    };
    // Optional plate in the taskbar's own colours: keeps the text readable when a taskbar theme
    // (e.g. Windhawk's Taskbar Styler) makes the bar transparent over the wallpaper.
    Rgb barColor = g_light ? Rgb{ 243, 243, 243 } : Rgb{ 32, 32, 32 };
    if (g_background) roundRect(Px(4), Px(6), barColor, 240);
    if (hover) roundRect(Px(4), Px(4), g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }, g_light ? 20 : 26);

    int x = Px(8);
    Rgb track = g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 };
    std::vector<int> xs(pieces.size()), ys(pieces.size());
    for (size_t i = 0; i < pieces.size(); i++) {
        const Piece& p = pieces[i];
        if (p.anchor < 0) {
            bool fixed = p.px != INT_MIN;  // stacked / vertical layout: Layout() placed it
            xs[i] = fixed ? p.px : x;
            ys[i] = fixed ? p.py : (H - p.m.h) / 2;
            if (p.haloR) cv.Draw(p.halo, xs[i] - p.haloR, ys[i] - p.haloR, p.color, p.haloAlpha * p.alpha / 255);
            if (p.barPct >= 0) {
                // The bar spans the text's width, like Synapse's battery bar; in the row layout text and bar
                // are centred together.
                int bh = std::max(2, Px(2)), gap = Px(3);
                if (!fixed) ys[i] = (H - (p.m.h + gap + bh)) / 2;
                int by = ys[i] + p.m.h + gap;
                int fill = std::max(p.barPct > 0 ? 1 : 0, (p.barPct * p.m.w + 50) / 100);
                for (int yy = by; yy < by + bh; yy++)
                    for (int xx = 0; xx < p.m.w; xx++)
                        cv.Blend(xs[i] + xx, yy, xx < fill ? p.barColor : track, xx < fill ? p.alpha : 45 * p.alpha / 255);
            }
            if (!fixed) x += p.m.w + p.gapAfter;
        } else {  // caption: one-pixel outline in the taskbar's colour so it reads over the icon's lines
            xs[i] = xs[p.anchor] + p.bx;
            ys[i] = ys[p.anchor] + p.by;
            for (int oy = -1; oy <= 1; oy++)
                for (int ox = -1; ox <= 1; ox++)
                    if (ox || oy) cv.Draw(p.m, xs[i] + ox, ys[i] + oy, barColor, 255);
        }
        cv.Draw(p.m, xs[i], ys[i], p.color, p.alpha);
    }

    // Device separators on top of everything: a soft rule in the same tone as the text.
    if (g_deviceDivider) {
        Rgb c = g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 };
        int a = g_light ? 55 : 64;
        for (const Divider& d : g_divs)
            for (int yy = d.y; yy < d.y + d.h; yy++)
                for (int xx = d.x; xx < d.x + d.w; xx++)
                    cv.Blend(xx, yy, c, a);
    }

    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC mem = CreateCompatibleDC(screen);
    if (!mem) {
        ReleaseDC(nullptr, screen);
        return false;
    }
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return false;
    }
    memcpy(bits, cv.px.data(), (size_t)W * H * 4);
    HGDIOBJ ob = SelectObject(mem, bmp);
    SIZE sz{ W, H };
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    bool updated = UpdateLayeredWindow(wnd, screen, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA) != FALSE;
    if (updated) size = { W, H };
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return updated;
}

void Paint() {
    if (!g_wnd) return;
    RECT client{};
    g_painted = PaintStrip(g_wnd, g_taskbar, g_hover, g_size);
    if (g_painted && GetClientRect(g_taskbar, &client)) {
        g_paintedBarClient = { client.right - client.left, client.bottom - client.top };
    } else {
        g_size = {};
        g_paintedBarClient = {};
    }
    g_lastW = -1;  // force a re-position with the new size
}

void PaintMirror(Mirror& m) {
    g_dpiBar = m.bar;
    m.dpi = Dpi();
    m.painted = PaintStrip(m.wnd, m.bar, m.hover, m.size);
    if (!m.painted) m.size = {};
    g_dpiBar = nullptr;
    m.lastLeft = INT_MIN;  // force a re-position with the new size
}

void HideMirror(Mirror& m) {
    if (m.lastLeft != INT_MIN || IsWindowVisible(m.wnd)) ShowWindow(m.wnd, SW_HIDE);
    m.lastLeft = INT_MIN;
}

// The window's own visible flag (IsWindowVisible() is also false while the taskbar itself is hidden).
bool Shown(HWND w) { return (GetWindowLongW(w, GWL_STYLE) & WS_VISIBLE) != 0; }

// Places a mirror left of its taskbar's clock (and left of the Widgets button if present), or hides it
// when that spot is unknown or taken by task buttons: never guess and cover something.
// On a vertical (left / right) taskbar the same happens along the bar's length: the mirror sits above
// the clock, centred in the bar's width, like the primary strip does.
void PositionMirror(Mirror& m) {
    RECT bar{};
    if (!GetWindowRect(m.bar, &bar)) { HideMirror(m); return; }
    bool v = IsVertical(bar);
    const bool rtl = (GetWindowLongW(m.bar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) != 0;
    if (rtl && v) { HideMirror(m); return; }  // a vertical right-to-left bar is not handled
    g_dpiBar = m.bar;
    // The strip's cross-bar size equals the bar's; a mismatch means the bar was resized or re-docked.
    if (!m.painted || Dpi() != m.dpi || m.size.cx <= 0 ||
        (v ? m.size.cx != bar.right - bar.left : m.size.cy != bar.bottom - bar.top)) {
        PaintMirror(m);
        g_dpiBar = m.bar;
    }
    if (!m.painted || m.size.cx <= 0 || m.size.cy <= 0) {
        g_dpiBar = nullptr;
        HideMirror(m);
        return;
    }
    int length = v ? m.size.cy : m.size.cx;  // the strip's size along the bar
    int right = 0, minLeft = Lo(bar, v);      // read "right"/"left" as "bottom"/"top" on a vertical bar
    bool known = false;
    if (rtl) {
        // Right-to-left (PR #2853): the notification area is at the left end; sit just right of it (or of the
        // Widgets button there), and only while the task buttons leave room. Mirrors the left-to-right rules.
        int edge = 0, limit = bar.right;
        HWND rtray = FindWindowExW(m.bar, nullptr, L"TrayNotifyWnd", nullptr);
        RECT rt{};
        if (rtray && IsWindowVisible(rtray) && GetWindowRect(rtray, &rt) && rt.right > rt.left && rt.left >= bar.left &&
            rt.right < bar.right && rt.bottom > bar.top && rt.top < bar.bottom) {
            edge = rt.right;
            known = true;
        } else if (!IsXamlTaskbar(m.bar)) {
            HWND clock = FindWindowExW(m.bar, nullptr, L"ClockButton", nullptr);
            RECT cr{};
            edge = clock && IsWindowVisible(clock) && GetWindowRect(clock, &cr) ? cr.right : bar.left + Px(8);
            for (const wchar_t* cls : { L"ShellFeedsTaskbarViewWindow", L"DynamicContent2" }) {
                HWND feeds = FindWindowExW(m.bar, nullptr, cls, nullptr);
                RECT fr{};
                if (feeds && IsWindowVisible(feeds) && GetWindowRect(feeds, &fr) && fr.right > edge && fr.right > fr.left)
                    edge = fr.right;
            }
            known = true;
        } else {
            AcquireSRWLockShared(&g_wbLock);
            for (const auto& s : g_spans)
                if (s.bar == m.bar && s.measured && s.rtl) {
                    edge = s.hasClock ? s.clockRight : bar.left + Px(8);
                    if (s.hasWidgets && s.widgets.right > edge && s.widgets.left < edge + length + Px(2))
                        edge = s.widgets.right;
                    if (s.hasRtlTasks) limit = s.rtlTasksLeft - Px(4);
                    known = true;
                }
            ReleaseSRWLockShared(&g_wbLock);
        }
        DWORD off = 0, osz = sizeof(off);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"TaskbarOffset", RRF_RT_REG_DWORD, nullptr, &off,
                         &osz) == ERROR_SUCCESS)
            edge += Px((int)off);
        int left = edge + Px(2);
        g_dpiBar = nullptr;
        if (!known || length <= 0 || left + length > limit) { HideMirror(m); return; }
        // In a WS_EX_LAYOUTRTL parent, client x runs from the right edge: convert the strip's screen RIGHT edge.
        POINT p{ left + length, bar.top };
        ScreenToClient(m.bar, &p);
        int along = p.x;
        bool covered = GetWindow(m.wnd, GW_HWNDPREV) != nullptr;
        if (!covered && Shown(m.wnd) && along == m.lastLeft) return;
        m.lastLeft = along;
        SetWindowPos(m.wnd, HWND_TOP, p.x, 0, m.size.cx, m.size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return;
    }
    // StartAllBack can put a full notification area on each secondary taskbar.
    // Use its left edge, as on the primary taskbar, to clear all of the tray icons.
    HWND tray = FindWindowExW(m.bar, nullptr, L"TrayNotifyWnd", nullptr);
    RECT tr{};
    if (tray && IsWindowVisible(tray) && GetWindowRect(tray, &tr) &&
        Lo(tr, v) > Lo(bar, v) && Hi(tr, v) <= Hi(bar, v) && Hi(tr, v) > Lo(tr, v) &&
        Hi(tr, !v) > Lo(bar, !v) && Lo(tr, !v) < Hi(bar, !v)) {
        right = Lo(tr, v);
        known = true;
    } else if (!IsXamlTaskbar(m.bar)) {  // Windows 10: the clock is a real child window (absent when it is turned off)
        HWND clock = FindWindowExW(m.bar, nullptr, L"ClockButton", nullptr);
        RECT cr{};
        right = clock && IsWindowVisible(clock) && GetWindowRect(clock, &cr) ? Lo(cr, v) : Hi(bar, v) - Px(8);
        for (const wchar_t* cls : { L"ShellFeedsTaskbarViewWindow", L"DynamicContent2" }) {
            HWND feeds = FindWindowExW(m.bar, nullptr, cls, nullptr);
            RECT fr{};
            if (feeds && IsWindowVisible(feeds) && GetWindowRect(feeds, &fr) && Lo(fr, v) < right && Hi(fr, v) > Lo(fr, v))
                right = Lo(fr, v);
        }
        known = true;
    } else {
        AcquireSRWLockShared(&g_wbLock);
        for (const auto& s : g_spans)
            if (s.bar == m.bar && s.measured) {
                right = s.hasClock ? s.clockLeft : Hi(bar, v) - Px(8);
                if (s.hasWidgets && Lo(s.widgets, v) < right && Hi(s.widgets, v) > right - length - Px(2))
                    right = Lo(s.widgets, v);
                if (s.hasTasks) minLeft = s.tasksRight + Px(4);
                known = true;
            }
        ReleaseSRWLockShared(&g_wbLock);
    }
    DWORD offset = 0, sz = sizeof(offset);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"TaskbarOffset", RRF_RT_REG_DWORD, nullptr, &offset, &sz) ==
        ERROR_SUCCESS)
        right -= Px((int)offset);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"SecondaryTaskbarOffset", RRF_RT_REG_DWORD, nullptr, &offset, &sz) ==
        ERROR_SUCCESS)
        right -= Px((int)offset);
    int left = right - length - Px(2), farX = 0;
    bool atFarLeft = !v && (g_farLeft || g_beforeApps) && FarLeftX(m.bar, m.size.cx, farX);  // horizontal bars only
    if (atFarLeft) left = farX;
    g_dpiBar = nullptr;
    if (!atFarLeft && (!known || length <= 0 || left < minLeft)) { HideMirror(m); return; }
    POINT p = v ? POINT{ bar.left, left } : POINT{ left, bar.top };
    ScreenToClient(m.bar, &p);
    int along = v ? p.y : p.x;
    bool covered = GetWindow(m.wnd, GW_HWNDPREV) != nullptr;
    if (!covered && Shown(m.wnd) && along == m.lastLeft) return;
    m.lastLeft = along;
    RECT client{};
    GetClientRect(m.bar, &client);
    int x = v ? (int)std::max<LONG>(0, (client.right - m.size.cx) / 2) : p.x;
    SetWindowPos(m.wnd, HWND_TOP, x, v ? p.y : 0, m.size.cx, m.size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void AddTool(HWND w) {
    if (!g_tip) return;
    TTTOOLINFOW ti{ sizeof(ti) };
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_notify;
    ti.uId = (UINT_PTR)w;
    ti.lpszText = const_cast<wchar_t*>(g_tipText.empty() ? L"BatteryHub" : g_tipText.c_str());
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

void DestroyMirror(Mirror& m) {
    if (g_tip) {
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.hwnd = g_notify;
        ti.uId = (UINT_PTR)m.wnd;
        SendMessageW(g_tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
    }
    if (m.wnd && IsWindow(m.wnd)) DestroyWindow(m.wnd);
    m.wnd = nullptr;
}

void SetMeasureMirrors(bool on) {
    AcquireSRWLockExclusive(&g_wbLock);
    bool changed = g_wbMirrors != on;
    g_wbMirrors = on;
    ReleaseSRWLockExclusive(&g_wbLock);
    if (changed && on && g_wbWake) SetEvent(g_wbWake);  // measure the new taskbar now, not in 5 s
}

std::vector<HWND> SecondaryTaskbars() {
    std::vector<HWND> out;
    for (HWND t = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr); t;
         t = FindWindowExW(nullptr, t, L"Shell_SecondaryTrayWnd", nullptr)) {
        if (IsWindowVisible(t)) out.push_back(t);  // horizontal or vertical: PositionMirror handles both
    }
    return out;
}

// Keeps one mirror per secondary taskbar: adds them for new monitors, removes them for gone ones.
// Called from Reposition() about once a second; with one monitor it is a single FindWindowEx call.
// A monitor's lasting identity: its device interface name, which survives reboots, Explorer restarts and
// Windows renumbering the displays (falls back to "\\.\DISPLAYn").
std::wstring MonitorId(HWND bar) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(bar, MONITOR_DEFAULTTONULL);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return L"";
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME) && dd.DeviceID[0]) return dd.DeviceID;
    return mi.szDevice;
}

// The model part of a monitor id: "AUO7EAD" in \\?\DISPLAY#AUO7EAD#4&300ab7d&0&UID8388688#{...}.
// A laptop panel gets a different instance path when a MUX / Advanced Optimus switch moves it between the
// integrated and the NVIDIA GPU, so its full id changes from one boot to the next; this part does not.
std::wstring MonitorModel(const std::wstring& id) {
    size_t a = id.find(L"DISPLAY#");
    if (a == std::wstring::npos) return L"";
    a += 8;
    size_t b = id.find(L'#', a);
    return b == std::wstring::npos ? L"" : id.substr(a, b - a);
}

// "Display 3 (right)": the number Windows gives the display and where it sits next to the main one.
std::wstring MonitorLabel(HWND bar) {
    MONITORINFOEXW mi{}, pm{};
    mi.cbSize = pm.cbSize = sizeof(MONITORINFOEXW);
    HMONITOR mon = MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(mon, &mi)) return T(L"Other monitor");
    std::wstring dev = mi.szDevice;
    size_t d = dev.find(L"DISPLAY");
    std::wstring label = d != std::wstring::npos ? TF(L"Display {number}", { { L"number", dev.substr(d + 7) } }) : T(L"Other monitor");
    POINT origin{ 0, 0 };
    if (GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &pm)) {
        int dx = (mi.rcMonitor.left + mi.rcMonitor.right - pm.rcMonitor.left - pm.rcMonitor.right) / 2;
        int dy = (mi.rcMonitor.top + mi.rcMonitor.bottom - pm.rcMonitor.top - pm.rcMonitor.bottom) / 2;
        label += std::wstring(L" (") + T(abs(dx) >= abs(dy) ? (dx < 0 ? L"left" : L"right") : (dy < 0 ? L"above" : L"below")) + L")";
    }
    return label;
}

// Hides or shows the strip on the main taskbar (it keeps running, just out of sight).
void SetPrimaryHidden(bool hide) {
    if (hide == g_primaryHidden || !g_wnd) return;
    g_primaryHidden = hide;
    if (hide) {
        ShowWindow(g_wnd, SW_HIDE);
        ReserveBottom(0);
        if (g_rebar && IsWindow(g_rebar)) {  // Windows 10: give the task buttons their full width back
            RECT r;
            GetClientRect(g_taskbar, &r);
            PostMessageW(g_taskbar, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right, r.bottom));
        }
    }
    g_lastW = -1;  // re-place (and show) on the next pass
}

void SyncMirrors() {
    std::vector<HWND> all = g_wnd ? SecondaryTaskbars() : std::vector<HWND>{}, bars;
    bool chosenFound = false;
    if (g_choice == L"all") {
        bars = all;
    } else if (g_choice != L"main") {
        std::vector<std::wstring> ids;
        for (HWND b : all) ids.push_back(MonitorId(b));
        int i = widget::FindMonitor(ids, g_choice);
        if (i >= 0) {
            bars.push_back(all[i]);
            chosenFound = true;
        }
    }
    SetPrimaryHidden(chosenFound);
    for (auto it = g_mirrors.begin(); it != g_mirrors.end();) {
        if (std::find(bars.begin(), bars.end(), it->bar) == bars.end() || !IsWindow(it->wnd)) {
            DestroyMirror(*it);
            it = g_mirrors.erase(it);
        } else {
            ++it;
        }
    }
    for (HWND b : bars) {
        if (std::any_of(g_mirrors.begin(), g_mirrors.end(), [b](const Mirror& m) { return m.bar == b; })) continue;
        Mirror m;
        m.bar = b;
        m.wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, b, nullptr,
                                g_inst, nullptr);
        if (!m.wnd) continue;
        AddTool(m.wnd);
        g_mirrors.push_back(m);
        PaintMirror(g_mirrors.back());
    }
    SetMeasureMirrors(!g_mirrors.empty());
    for (auto& m : g_mirrors) PositionMirror(m);
}

void PaintAll() {
    Paint();
    for (auto& m : g_mirrors) PaintMirror(m);
}

// Hover highlight for the strip under the mouse: the primary one or a mirror.
void SetHover(HWND h, bool on) {
    if (h == g_wnd) {
        if (g_hover == on) return;
        g_hover = on;
        Paint();
        widget::Reposition();
        return;
    }
    for (auto& m : g_mirrors)
        if (m.wnd == h && m.hover != on) {
            m.hover = on;
            PaintMirror(m);
            PositionMirror(m);
        }
}

LRESULT CALLBACK WidgetProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            SetHover(h, true);
            return 0;
        }
        case WM_MOUSELEAVE:
            SetHover(h, false);
            return 0;
        case WM_LBUTTONUP:
            PostMessageW(g_notify, g_clickMsg, 0, 0);  // left: device menu
            return 0;
        case WM_RBUTTONUP:
            PostMessageW(g_notify, g_clickMsg, 1, 0);  // right: settings menu
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

namespace widget {

wchar_t DeviceGlyph(DeviceKind kind, bool bluetooth) { return Glyph(kind, bluetooth); }

bool Create(HINSTANCE inst, HWND notifyWnd, UINT clickMsg, bool lightTheme) {
    Destroy();
    g_taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!g_taskbar || !FindWindowExW(g_taskbar, nullptr, L"TrayNotifyWnd", nullptr)) { g_taskbar = nullptr; return false; }
    RECT tb;
    GetWindowRect(g_taskbar, &tb);
    g_vertical = tb.bottom - tb.top > tb.right - tb.left;  // left / right taskbar: devices stacked in a column
    // Windows 11's taskbar is a XAML island; on Windows 10 the task buttons live in ReBarWindow32
    // and have to be narrowed so they never slide under the widget.
    g_rebar = FindWindowExW(g_taskbar, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr)
                  ? nullptr
                  : FindWindowExW(g_taskbar, nullptr, L"ReBarWindow32", nullptr);
    g_notify = notifyWnd;
    g_clickMsg = clickMsg;
    g_light = lightTheme;
    g_inst = inst;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WidgetProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    g_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1,
                            g_taskbar, nullptr, inst, nullptr);
    if (!g_wnd) { g_taskbar = nullptr; return false; }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
                            notifyWnd, nullptr, inst, nullptr);
    if (g_tip) {
        ApplyTipTheme();
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = notifyWnd;
        ti.uId = (UINT_PTR)g_wnd;
        ti.lpszText = const_cast<wchar_t*>(L"BatteryHub");
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 600);
    }
    WatchWidgetsButton(g_taskbar);
    g_lastDpi = Dpi();
    Paint();
    g_lastW = -1;
    Reposition();  // shows the strip once it is drawn and placed; it stays hidden until then
    return true;
}

void Destroy() {
    if (g_rebar && IsWindow(g_rebar) && g_taskbar && IsWindow(g_taskbar)) {
        // Let Explorer lay the task buttons out again over the full width.
        RECT r;
        GetClientRect(g_taskbar, &r);
        PostMessageW(g_taskbar, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right, r.bottom));
    }
    g_rebar = nullptr;
    ReserveBottom(0);  // give the taskbar its full height back
    g_fill = false;
    for (auto& m : g_mirrors) DestroyMirror(m);
    g_mirrors.clear();
    SetMeasureMirrors(false);
    g_primaryHidden = false;  // the next Create() decides again
    if (g_wbWake) WatchWidgetsButton(nullptr);
    if (g_tip) { DestroyWindow(g_tip); g_tip = nullptr; }
    if (g_wnd) { DestroyWindow(g_wnd); g_wnd = nullptr; }
    g_taskbar = nullptr;
    g_hover = false;
    g_painted = false;
    g_size = {};
    g_paintedBarClient = {};
}

bool Alive() { return g_wnd && IsWindow(g_wnd) && g_taskbar && IsWindow(g_taskbar); }

void SetItems(const std::vector<WidgetItem>& items, const std::wstring& tooltip) {
    g_items = items;
    if (tooltip != g_tipText && g_tip) {
        g_tipText = tooltip;
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.hwnd = g_notify;
        ti.lpszText = const_cast<wchar_t*>(g_tipText.c_str());
        ti.uId = (UINT_PTR)g_wnd;
        SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
        for (const auto& m : g_mirrors) {
            ti.uId = (UINT_PTR)m.wnd;
            SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
        }
    }
    PaintAll();
    Reposition();
}

void SetLightTheme(bool light) {
    g_light = light;
    ApplyTipTheme();
    PaintAll();
}

void SetBackground(bool on) {
    g_background = on;
    PaintAll();
}

void SetTextStyle(const TextStyle& style) {
    g_textStyle = style;
    PaintAll();
    if (g_wnd) Reposition();
}

void SetConnectionIcons(bool on) {
    g_connectionIcons = on;
    PaintAll();
    if (g_wnd) Reposition();
}

void SetDeviceDivider(bool on) {
    g_deviceDivider = on;
    PaintAll();
    if (g_wnd) Reposition();
}

void SetRingStyle(bool on) {
    g_ringStyle = on;
    PaintAll();
    if (g_wnd) Reposition();
}

void SetShowPercentSign(bool on) {
    g_showPercentSign = on;
    PaintAll();
    if (g_wnd) Reposition();
}

void SetStacked(bool on) {
    g_stacked = on;
    PaintAll();
    if (g_wnd) Reposition();
}

void SetMakeRoom(bool on) {
    g_makeRoom = on;
    g_lastW = -1;  // force a re-position
    if (g_wnd) Reposition();
}

void SetVerticalPlace(VerticalPlace place) {
    g_place = place;
    if (g_wnd) Reposition();
}

bool Vertical() { return g_wnd && g_vertical; }

void ApplyLeftSpot() {
    bool on = g_farLeft || g_beforeApps;
    AcquireSRWLockExclusive(&g_wbLock);
    g_wbFarLeft = on;  // measure the primary taskbar too
    ReleaseSRWLockExclusive(&g_wbLock);
    if (on && g_wbWake) SetEvent(g_wbWake);  // measure the taskbar now, not in 5 s
    if (g_wnd) Reposition();
}

void SetFarLeft(bool on) {
    g_farLeft = on;
    ApplyLeftSpot();
}

void SetBeforeApps(bool on) {
    g_beforeApps = on;
    ApplyLeftSpot();
}

void SetMonitorChoice(const std::wstring& choice) {
    g_choice = choice.empty() ? L"all" : choice;
    SyncMirrors();  // adds, removes or hides strips right away
    if (g_wnd) Reposition();
}

int FindMonitor(const std::vector<std::wstring>& ids, const std::wstring& choice) {
    for (size_t i = 0; i < ids.size(); i++)
        if (ids[i] == choice) return (int)i;
    std::wstring model = MonitorModel(choice);
    int found = -1;
    if (!model.empty())
        for (size_t i = 0; i < ids.size(); i++)
            if (MonitorModel(ids[i]) == model) {
                if (found >= 0) return -1;  // two monitors of that model: can't tell which one was chosen
                found = (int)i;
            }
    return found;
}

std::vector<MonitorOption> OtherMonitors() {
    std::vector<MonitorOption> out;
    for (HWND b : SecondaryTaskbars()) out.push_back({ MonitorId(b), MonitorLabel(b) });
    std::sort(out.begin(), out.end(), [](const MonitorOption& a, const MonitorOption& b) { return a.label < b.label; });
    return out;
}

// Other apps can put their own small windows into the taskbar next to the notification area (network
// graphs, countdowns, ...). They are visible child windows of the taskbar owned by neither Explorer nor
// us. Starting at `edge` (where the strip's trailing side would go), steps past every such window packed
// against it, so the strips sit side by side. A window already sitting next to the edge keeps its spot
// and the strip goes beside it. If another app does the same, the first of the two to re-position takes
// the outer spot and the other one then stays put, so they settle within a second.
int StepPastOtherWidgets(int edge, const RECT& bar, bool vertical, bool rtl) {
    if (!g_makeRoom || !g_wnd) return edge;
    DWORD explorer = 0, pid = 0;
    GetWindowThreadProcessId(g_taskbar, &explorer);
    const DWORD self = GetCurrentProcessId();
    const LONG span = vertical ? bar.bottom - bar.top : bar.right - bar.left;
    // Candidates: the taskbar's own child windows, and (from PR #1970) top-level windows of other apps that
    // float over the taskbar, lying entirely inside it (e.g. a monitor gadget docked onto the taskbar).
    std::vector<RECT> others;
    auto consider = [&](HWND c, bool topLevel) {
        if (c == g_wnd || !IsWindowVisible(c)) return;
        GetWindowThreadProcessId(c, &pid);
        RECT r;
        if (pid == explorer || pid == self || !GetWindowRect(c, &r) || r.right <= r.left || r.bottom <= r.top) return;
        // A window docked over the taskbar has to be topmost to show there; anything else is just behind it.
        if (topLevel && (!(GetWindowLongW(c, GWL_EXSTYLE) & WS_EX_TOPMOST) || r.left < bar.left || r.right > bar.right ||
                         r.top < bar.top || r.bottom > bar.bottom))
            return;
        if ((vertical ? r.bottom - r.top : r.right - r.left) > span / 3) return;  // not a small widget
        others.push_back(r);
    };
    for (HWND c = GetWindow(g_taskbar, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) consider(c, false);
    for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) consider(w, true);
    for (int pass = 0; pass < 8; pass++) {
        bool moved = false;
        for (const RECT& r : others) {
            if (vertical) {
                if (r.right > bar.left && r.left < bar.right && r.bottom >= edge - Px(16) && r.bottom <= edge + Px(4) &&
                    r.top < edge)
                    edge = r.top, moved = true;
            } else if (r.bottom > bar.top && r.top < bar.bottom) {
                if (!rtl && r.right >= edge - Px(16) && r.right <= edge + Px(4) && r.left < edge)
                    edge = r.left, moved = true;
                else if (rtl && r.left <= edge + Px(16) && r.left >= edge - Px(4) && r.right > edge)
                    edge = r.right, moved = true;
            }
        }
        if (!moved) break;
    }
    return edge;
}

bool Reposition() {
    if (!Alive()) return false;
    SyncMirrors();
    if (g_primaryHidden) return true;  // only another monitor's taskbar shows the strip
    RECT bar{}, client{};
    if (!GetWindowRect(g_taskbar, &bar) || !GetClientRect(g_taskbar, &client)) return true;
    int clientW = client.right - client.left, clientH = client.bottom - client.top;
    if (clientW <= 0 || clientH <= 0) return true;  // taskbar not laid out yet
    // The taskbar can be moved between a horizontal and a vertical edge while the app runs.
    bool vertical = bar.bottom - bar.top > bar.right - bar.left;
    int dpi = Dpi();
    if (!g_painted || g_size.cx <= 0 || g_size.cy <= 0 || dpi != g_lastDpi || vertical != g_vertical ||
        g_paintedBarClient.cx != clientW || g_paintedBarClient.cy != clientH) {
        g_lastDpi = dpi;
        g_vertical = vertical;
        Paint();
    }
    if (!g_painted || g_size.cx <= 0 || g_size.cy <= 0) return true;
    HWND tray = FindWindowExW(g_taskbar, nullptr, L"TrayNotifyWnd", nullptr);
    RECT tr{};
    if (!tray || !GetWindowRect(tray, &tr)) return true;
    if (tr.right <= tr.left || tr.bottom <= tr.top) return true;
    if (tr.right <= bar.left || tr.left >= bar.right || tr.bottom <= bar.top || tr.top >= bar.bottom)
        return true;
    if (g_vertical) {
        // Left or right taskbar (PR #321): the column sits just above the notification area, centred in
        // the bar's width.
        POINT anchor{ bar.left, StepPastOtherWidgets(tr.top, bar, true, false) };
        ScreenToClient(g_taskbar, &anchor);
        int top = anchor.y - g_size.cy - Px(2);
        DWORD offset = 0, osz = sizeof(offset);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"TaskbarOffset", RRF_RT_REG_DWORD, nullptr, &offset,
                         &osz) == ERROR_SUCCESS)
            top -= Px((int)offset);
        top = std::clamp(top, 0, (int)std::max<LONG>(0, client.bottom - g_size.cy));
        // Windows 11: the centred task list grows down to the tray and would end up under the column.
        // When that happens, move to the free end of the bar above the first button, if the column fits.
        // The menu can also pin the column to the top, keep it above the tray, or give it the bottom of the bar.
        AcquireSRWLockShared(&g_wbLock);
        Column col = g_column;
        ReleaseSRWLockShared(&g_wbLock);
        bool bottom = g_place == widget::VerticalPlace::Bottom && !g_rebar && g_size.cy > 0 &&
                      g_size.cy < client.bottom / 2;
        bool wasReserved = g_reserved;  // the content has had a pass to move up
        ReserveBottom(bottom ? g_size.cy : 0);
        COLORREF fill = 0;
        bool sampled = bottom && wasReserved && SampleBarColor(bar, bar.bottom - g_size.cy, fill);
        bool changed = sampled && (!g_fill || abs(GetRValue(fill) - GetRValue(g_fillColor)) > 4 ||
                                   abs(GetGValue(fill) - GetGValue(g_fillColor)) > 4 ||
                                   abs(GetBValue(fill) - GetBValue(g_fillColor)) > 4);
        if (changed || (!bottom && g_fill)) {
            g_fill = bottom;
            g_fillColor = fill;
            Paint();
        }
        if (bottom) {
            top = client.bottom - g_size.cy;
        } else if (g_place == widget::VerticalPlace::Top) {
            top = 0;
        } else if (g_place == widget::VerticalPlace::Auto && !g_rebar && col.bar == g_taskbar && col.measured &&
                   col.any) {
            POINT first{ bar.left, col.top }, last{ bar.left, col.bottom };
            ScreenToClient(g_taskbar, &first);
            ScreenToClient(g_taskbar, &last);
            if (last.y > top && first.y < top + g_size.cy && g_size.cy + Px(2) <= first.y) top = 0;
        }
        int left = (int)std::max<LONG>(0, (client.right - g_size.cx) / 2);
        // Windows 10: shorten the task-button area when it would reach behind the column.
        if (g_rebar && IsWindow(g_rebar)) {
            RECT rr{};
            GetWindowRect(g_rebar, &rr);
            POINT tl{ rr.left, rr.top };
            ScreenToClient(g_taskbar, &tl);
            if (tl.y < top && tl.y + (rr.bottom - rr.top) > top)
                MoveWindow(g_rebar, tl.x, tl.y, rr.right - rr.left, top - tl.y, TRUE);
        }
        bool covered = GetWindow(g_wnd, GW_HWNDPREV) != nullptr;
        if (!covered && Shown(g_wnd) && EqualRect(&tr, &g_lastTray) && EqualRect(&bar, &g_lastBar) &&
            g_lastW == g_size.cy && g_lastLeft == top)
            return true;
        g_lastTray = tr;
        g_lastBar = bar;
        g_lastW = g_size.cy;
        g_lastLeft = top;
        SetWindowPos(g_wnd, HWND_TOP, left, top, g_size.cx, g_size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return true;
    }
    ReserveBottom(0);  // the taskbar was moved from a vertical edge to a horizontal one
    if (g_fill) { g_fill = false; Paint(); }
    // Right-to-left Windows (Hebrew, Arabic) mirrors the taskbar: client x runs from the screen's right
    // edge, so a window's client-space left edge is its screen-space right edge.
    bool rtl = GetWindowLongW(g_taskbar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL;
    int edge = rtl ? tr.right : tr.left;  // screen x we sit next to
    // Windows 10: News and Interests / Weather sits immediately left of the tray. The window that hosts it
    // depends on the build: ShellFeedsTaskbarViewWindow on some, DynamicContent2 on others (e.g. 19045,
    // where the search highlights button is DynamicContent1). Both are direct children of the taskbar and
    // only a visible one counts. Its rectangle is read on every pass because the weather text changes its
    // width, so the widget follows it.
    if (!rtl)
        for (const wchar_t* cls : { L"ShellFeedsTaskbarViewWindow", L"DynamicContent2" }) {
            HWND feeds = FindWindowExW(g_taskbar, nullptr, cls, nullptr);
            if (!feeds || !IsWindowVisible(feeds)) continue;
            RECT fr;
            GetWindowRect(feeds, &fr);
            if (fr.left < edge && fr.right > fr.left) edge = fr.left;
        }
    // Step past the Widgets button when it sits where we would go.
    RECT wb = WidgetsButtonRect();
    if (wb.right > wb.left && wb.top < bar.bottom && wb.bottom > bar.top &&
        (rtl ? wb.left < edge + g_size.cx + Px(2) && wb.right > edge : wb.right > edge - g_size.cx - Px(2) && wb.left < edge))
        edge = rtl ? wb.right : wb.left;
    edge = StepPastOtherWidgets(edge, bar, false, rtl);
    POINT p{ edge, bar.top };
    ScreenToClient(g_taskbar, &p);
    int left = p.x - g_size.cx - Px(2);


    // Optional user override in DPI-independent pixels (e.g. for custom padding).
    DWORD customOffset = 0, sz = sizeof(customOffset);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"TaskbarOffset",
                     RRF_RT_REG_DWORD, nullptr, &customOffset, &sz) == ERROR_SUCCESS) {
        left -= Px((int)customOffset);
    }

    // Clamp only out-of-range positions. At x=0 the widget can be obscured by Start/XAML,
    // so prefer its previous valid position or the space beside a normally anchored tray (#2993).
    RECT taskbarClient{};
    if (GetClientRect(g_taskbar, &taskbarClient)) {
        left = RecoverHorizontalWidgetX(left, g_size.cx, taskbarClient.right - taskbarClient.left,
                                        tr.right - tr.left, g_lastLeft, Px(2));
    } else if (left < 0 && g_lastLeft >= 0) {
        left = g_lastLeft;
    }

    int farX = 0;
    bool atFarLeft = !rtl && (g_farLeft || g_beforeApps) && FarLeftX(g_taskbar, g_size.cx, farX);
    if (atFarLeft) {
        POINT fp{ farX, bar.top };
        ScreenToClient(g_taskbar, &fp);
        left = fp.x;
    }

    // Windows 10: Explorer re-expands the task buttons on its own layout passes; narrow them again.
    if (!atFarLeft && g_rebar && IsWindow(g_rebar)) {
        RECT rr;
        GetWindowRect(g_rebar, &rr);
        POINT tl{ rtl ? rr.right : rr.left, rr.top };
        ScreenToClient(g_taskbar, &tl);
        if (tl.x + (rr.right - rr.left) > left && left > tl.x)
            MoveWindow(g_rebar, tl.x, tl.y, left - tl.x, rr.bottom - rr.top, TRUE);
    }

    // Keep us above the taskbar's own content (our sibling windows).
    bool covered = GetWindow(g_wnd, GW_HWNDPREV) != nullptr;
    if (!covered && Shown(g_wnd) && EqualRect(&tr, &g_lastTray) && EqualRect(&bar, &g_lastBar) &&
        EqualRect(&wb, &g_lastWidgets) && g_lastW == g_size.cx && g_lastLeft == left)
        return true;
    g_lastTray = tr;
    g_lastWidgets = wb;
    g_lastBar = bar;
    g_lastW = g_size.cx;
    g_lastLeft = left;
    SetWindowPos(g_wnd, HWND_TOP, left, 0, g_size.cx, g_size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

}  // namespace widget