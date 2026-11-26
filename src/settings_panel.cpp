// WebView2-backed settings panel.
//
// The four groups that used to live in the tray menu (battery history, charge alert, polling and
// display style) plus position and device options are rendered as a small HTML page in a borderless
// WebView2 window. The page is embedded in the exe as RCDATA, so there is no file to ship next to it.
//
// The native side owns the data: main.cpp fills a SettingsData snapshot and supplies callbacks, and
// this file turns every change the page reports into exactly one callback. Nothing here reaches into
// main's globals.
//
// WebView2 needs its loader DLL. It is only ~190 KB and has no CRT dependency, so it is embedded as a
// resource too and unpacked to %LOCALAPPDATA%\BatteryHub on first use; the app still ships as one exe.
#include "settings_panel.h"

#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "WebView2.h"
#include "i18n.h"

namespace {


constexpr wchar_t kClassName[] = L"BatteryHubSettings";
// Only one settings window at a time: a second tray-menu command raises the open one instead of
// stacking another modal loop on top of it.
HWND g_openPanel = nullptr;
constexpr int kDesignW = 820, kDesignH = 560;
constexpr UINT_PTR kLoadingTimer = 1;   // dot animation while the page loads
constexpr UINT_PTR kWatchdogTimer = 2;  // never sit on "Loading..." for longer than this

// ---- tiny COM helpers -------------------------------------------------------------------------

template <class T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T* operator->() const { return p_; }
    T* get() const { return p_; }
    T** put() { reset(); return &p_; }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

#define IMPL_UNKNOWN(Iface, IID_NAME)                                                       \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {             \
        if (!ppv) return E_POINTER;                                                          \
        if (riid == IID_IUnknown || riid == IID_NAME) {                                      \
            *ppv = static_cast<Iface*>(this);                                                \
            AddRef();                                                                        \
            return S_OK;                                                                     \
        }                                                                                    \
        *ppv = nullptr;                                                                      \
        return E_NOINTERFACE;                                                                \
    }                                                                                        \
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }                            \
    ULONG STDMETHODCALLTYPE Release() override {                                             \
        ULONG n = --refs_;                                                                   \
        if (!n) delete this;                                                                 \
        return n;                                                                            \
    }

class EnvHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
public:
    using Fn = std::function<void(HRESULT, ICoreWebView2Environment*)>;
    explicit EnvHandler(Fn fn) : fn_(std::move(fn)) {}
    virtual ~EnvHandler() = default;
    IMPL_UNKNOWN(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                 IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Environment* env) override {
        if (fn_) fn_(hr, env);
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    Fn fn_;
};

class ControllerHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
public:
    using Fn = std::function<void(HRESULT, ICoreWebView2Controller*)>;
    explicit ControllerHandler(Fn fn) : fn_(std::move(fn)) {}
    virtual ~ControllerHandler() = default;
    IMPL_UNKNOWN(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                 IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* c) override {
        if (fn_) fn_(hr, c);
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    Fn fn_;
};

class NavHandler : public ICoreWebView2NavigationCompletedEventHandler {
public:
    using Fn = std::function<void(ICoreWebView2NavigationCompletedEventArgs*)>;
    explicit NavHandler(Fn fn) : fn_(std::move(fn)) {}
    virtual ~NavHandler() = default;
    IMPL_UNKNOWN(ICoreWebView2NavigationCompletedEventHandler, IID_ICoreWebView2NavigationCompletedEventHandler)
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) override {
        if (fn_) fn_(args);
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    Fn fn_;
};

class MessageHandler : public ICoreWebView2WebMessageReceivedEventHandler {
public:
    using Fn = std::function<void(ICoreWebView2WebMessageReceivedEventArgs*)>;
    explicit MessageHandler(Fn fn) : fn_(std::move(fn)) {}
    virtual ~MessageHandler() = default;
    IMPL_UNKNOWN(ICoreWebView2WebMessageReceivedEventHandler, IID_ICoreWebView2WebMessageReceivedEventHandler)
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) override {
        if (fn_) fn_(args);
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    Fn fn_;
};

#undef IMPL_UNKNOWN

// ---- small helpers ----------------------------------------------------------------------------

std::wstring LocalAppDataFolder() {
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

std::wstring AppDataDir() {
    std::wstring base = LocalAppDataFolder();
    if (base.empty()) return L".";
    std::wstring dir = base + L"\\BatteryHub";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool AppsLightTheme() {
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v != 0;
}

// Writes an RCDATA blob to `path` once; returns false when the resource is missing or unwritable.
bool ExtractResourceOnce(HMODULE mod, LPCWSTR name, const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) && fad.nFileSizeHigh == 0 &&
        fad.nFileSizeLow > 0)
        return true;
    HRSRC res = FindResourceW(mod, name, RT_RCDATA);
    if (!res) return false;
    const DWORD size = SizeofResource(mod, res);
    HGLOBAL glob = LoadResource(mod, res);
    if (!glob || !size) return false;
    const void* data = LockResource(glob);
    if (!data) return false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, data, size, &written, nullptr) && written == size;
    CloseHandle(f);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

std::wstring LoadHtmlResource() {
    HRSRC res = FindResourceW(GetModuleHandleW(nullptr), L"SETTINGS_HTML", RT_RCDATA);
    if (!res) return {};
    const DWORD size = SizeofResource(GetModuleHandleW(nullptr), res);
    HGLOBAL glob = LoadResource(GetModuleHandleW(nullptr), res);
    if (!glob || !size) return {};
    const char* utf8 = static_cast<const char*>(LockResource(glob));
    if (!utf8) return {};
    int wide = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)size, nullptr, 0);
    if (wide <= 0) return {};
    std::wstring out((size_t)wide, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, (int)size, out.data(), wide);
    return out;
}

std::wstring JsonEscape(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size() + 8);
    for (wchar_t c : in) {
        switch (c) {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (c < 0x20 || c == L'<' || c == L'>' || c == L'&') {
                    wchar_t buf[8];
                    swprintf_s(buf, L"\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::wstring FmtNum(float v) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f", v);
    std::wstring out(buf);
    if (out.size() > 2 && out.compare(out.size() - 2, 2, L".0") == 0) out.resize(out.size() - 2);
    return out;
}

std::wstring ItemList(const std::vector<int>& values, const std::function<std::wstring(int)>& label) {
    std::wstring out = L"[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += L",";
        out += L"{\"v\":" + std::to_wstring(values[i]) + L",\"l\":\"" + JsonEscape(label(values[i])) + L"\"}";
    }
    return out + L"]";
}

std::wstring PollLabel(int s) {
    if (s < 60) return TF(L"{seconds} s", { { L"seconds", std::to_wstring(s) } });
    return TF(L"{minutes} min", { { L"minutes", std::to_wstring(s / 60) } });
}

int CALLBACK CollectFont(const LOGFONTW* lf, const TEXTMETRICW*, DWORD, LPARAM lp) {
    // '@' faces are the vertical variants of a CJK family and cannot be used for the strip.
    if (!lf->lfFaceName[0] || lf->lfFaceName[0] == L'@') return TRUE;
    auto* out = reinterpret_cast<std::vector<std::wstring>*>(lp);
    for (const auto& f : *out)
        if (!_wcsicmp(f.c_str(), lf->lfFaceName)) return TRUE;
    out->push_back(lf->lfFaceName);
    return TRUE;
}

// The installed families do not change while the app runs, so the enumeration (which walks every
// face on the system) runs once instead of on each open of the window.
const std::vector<std::wstring>& InstalledFonts() {
    static const std::vector<std::wstring> cached = [] {
        std::vector<std::wstring> out;
        HDC dc = GetDC(nullptr);
        if (dc) {
            LOGFONTW lf{};
            lf.lfCharSet = DEFAULT_CHARSET;
            EnumFontFamiliesExW(dc, &lf, CollectFont, reinterpret_cast<LPARAM>(&out), 0);
            ReleaseDC(nullptr, dc);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }();
    return cached;
}

// Every phrase the page shows has its English text here once; T() translates it.
const wchar_t* const kPhrases[] = {
    L"Settings", L"Close", L"General", L"Display style", L"Position", L"Devices",
    L"Battery alerts and settings", L"Record battery history", L"Notify when charged to", L"Off",
    L"Check battery every", L"Taskbar widget", L"Show on the taskbar", L"Move to tray when no devices",
    L"Icons above percentages", L"Background behind taskbar text", L"Appearance", L"Ring style",
    L"Show percent sign", L"Brand colors", L"Mark connection type", L"Separator between devices",
    L"Show tooltip", L"Text style", L"Font", L"Font family", L"Text size", L"Font weight", L"Layout",
    L"Icon size", L"Icon gap", L"Item gap", L"Divider gap", L"Letter spacing", L"Default",
    L"Position on the taskbar", L"At the left end of the taskbar",
    L"Just before the taskbar apps", L"Custom offset", L"Make room for other taskbar widgets",
    L"Position on a vertical taskbar", L"Vertical placement", L"Show the widget on", L"Monitor",
    L"Automatic", L"Top of the taskbar", L"Above the notification area",
    L"Bottom of the taskbar", L"All monitors",
    L"Main monitor only", L"Custom monitor", L"Inactive devices",
    L"Hide last known / asleep devices",
    L"Show sleeping / disconnected devices", L"Dim sleeping / disconnected devices",
};

std::wstring BuildBootJson(const SettingsData& d) {
    const bool light = AppsLightTheme();
    std::wstring j = L"{\"light\":";
    j += light ? L"true" : L"false";
    j += L",\"t\":{";
    bool first = true;
    for (const wchar_t* p : kPhrases) {
        if (!first) j += L",";
        first = false;
        j += L"\"" + JsonEscape(p) + L"\":\"" + JsonEscape(T(p)) + L"\"";
    }
    j += L"},\"v\":{";
    auto b = [&](const wchar_t* k, bool val) {
        j += std::wstring(L"\"") + k + L"\":" + (val ? L"true" : L"false") + L",";
    };
    b(L"recordHistory", d.recordHistory);
    b(L"wantWidget", d.wantWidget);
    b(L"trayWhenEmpty", d.trayWhenEmpty);
    b(L"stacked", d.stacked);
    b(L"widgetBackground", d.widgetBackground);
    b(L"ringStyle", d.ringStyle);
    b(L"showPercentSign", d.showPercentSign);
    b(L"brandColors", d.brandColors);
    b(L"connectionIcons", d.connectionIcons);
    b(L"deviceDivider", d.deviceDivider);
    b(L"showTooltip", d.showTooltip);
    b(L"beforeApps", d.beforeApps);
    b(L"makeRoom", d.makeRoom);
    b(L"hideLastKnown", d.hideLastKnown);
    b(L"showInactive", d.showInactive);
    j += L"\"dimInactive\":" + std::wstring(d.dimInactive ? L"true" : L"false") + L",";
    j += L"\"chargeLimit\":" + std::to_wstring(d.chargeLimit) + L",";
    j += L"\"pollSeconds\":" + std::to_wstring(d.pollSeconds) + L",";
    j += L"\"taskbarOffset\":" + std::to_wstring(d.taskbarOffset) + L",";
    j += L"\"verticalPlace\":" + std::to_wstring(d.verticalPlace) + L",";
    j += L"\"monitorChoice\":" + std::to_wstring(d.monitorChoice) + L",";
    j += L"\"tsFace\":\"" + JsonEscape(d.textStyle.face) + L"\",";
    j += L"\"tsSize\":" + std::to_wstring(d.textStyle.size) + L",";
    j += L"\"tsWeight\":" + std::to_wstring(d.textStyle.weight) + L",";
    j += L"\"tsIconSize\":" + std::to_wstring(d.textStyle.iconSize) + L",";
    j += L"\"tsIconGap\":" + FmtNum(d.textStyle.iconGap) + L",";
    j += L"\"tsItemGap\":" + FmtNum(d.textStyle.itemGap) + L",";
    j += L"\"tsDividerGap\":" + FmtNum(d.textStyle.dividerGap) + L",";
    j += L"\"tsLetterSpacing\":" + FmtNum(d.textStyle.letterSpacing);
    j += L"},\"chargeLimitItems\":";
    std::vector<int> limits{0};
    for (int l : d.chargeLimits) limits.push_back(l);
    j += ItemList(limits, [](int v) { return v == 0 ? std::wstring(T(L"Off")) : std::to_wstring(v) + L"%"; });
    j += L",\"pollItems\":";
    j += ItemList(d.pollSecondsList, PollLabel);
    j += L",\"offsetItems\":";
    j += ItemList(d.offsetList, [](int v) { return std::to_wstring(v) + L" px"; });
    j += L",\"verticalItems\":";
    j += ItemList({0, 1, 2, 3}, [](int v) {
        static const wchar_t* const kLabels[] = {
            L"Automatic", L"Top of the taskbar", L"Above the notification area",
            L"Bottom of the taskbar",
        };
        return std::wstring(T(kLabels[v < 0 || v > 3 ? 0 : v]));
    });
    j += L",\"monitorItems\":";
    j += ItemList({0, 1, 2}, [](int v) {
        static const wchar_t* const kLabels[] = { L"All monitors", L"Main monitor only", L"Custom monitor" };
        return std::wstring(T(kLabels[v < 0 || v > 2 ? 0 : v]));
    });
    j += L",\"fontItems\":[{\"v\":\"\",\"l\":\"" + JsonEscape(T(L"Default")) + L"\"}";
    for (const auto& f : InstalledFonts())
        j += L",{\"v\":\"" + JsonEscape(f) + L"\",\"l\":\"" + JsonEscape(f) + L"\"}";
    j += L"]";
    j += L",\"weightItems\":";
    j += ItemList({100, 200, 300, 400, 500, 600, 700, 800, 900}, [](int w) { return std::to_wstring(w); });
    return j + L"}";
}

struct Context {
    const SettingsCallbacks* cb = nullptr;
    widget::TextStyle style;
    std::wstring bootJson;
    std::wstring html;
    bool light = false;
    bool closed = false;
    bool webviewReady = false;
    bool loading = true;   // native "Loading..." until the page has painted
    int dots = 0;          // animation step for that text
    int attempts = 0;      // controller creations tried in this window (see the retry below)
    EventRegistrationToken messageToken{};
    Com<ICoreWebView2Environment> env;  // one per window: a released controller cannot be replaced
    Com<ICoreWebView2Controller> controller;
    Com<ICoreWebView2> webview;
};

Context* Ctx(HWND hwnd) {
    return reinterpret_cast<Context*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

COLORREF WindowBackground(bool light) { return light ? RGB(0xf3, 0xf3, 0xf3) : RGB(0x20, 0x20, 0x20); }

void ResizeWebView(HWND hwnd, Context* ctx) {
    if (!ctx || !ctx->controller) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    ctx->controller->put_Bounds(rc);
    UINT dpi = GetDpiForWindow(hwnd);
    Com<ICoreWebView2Controller3> c3;
    if (dpi && SUCCEEDED(ctx->controller->QueryInterface(IID_ICoreWebView2Controller3,
                                                         reinterpret_cast<void**>(c3.put()))))
        c3->put_RasterizationScale(dpi / 96.0);
}

// The text style page reports one field at a time; keep the running style and apply it live.
void ApplyTextChange(Context* ctx, const std::wstring& k, const std::wstring& value) {
    if (!ctx) return;
    widget::TextStyle& st = ctx->style;
    if (k == L"tsFace") st.face = value;
    else if (k == L"tsSize") st.size = _wtoi(value.c_str());
    else if (k == L"tsWeight") st.weight = _wtoi(value.c_str());
    else if (k == L"tsIconSize") st.iconSize = _wtoi(value.c_str());
    else if (k == L"tsIconGap") st.iconGap = static_cast<float>(_wtof(value.c_str()));
    else if (k == L"tsItemGap") st.itemGap = static_cast<float>(_wtof(value.c_str()));
    else if (k == L"tsDividerGap") st.dividerGap = static_cast<float>(_wtof(value.c_str()));
    else if (k == L"tsLetterSpacing") st.letterSpacing = static_cast<float>(_wtof(value.c_str()));
    else return;
    if (ctx->cb && ctx->cb->textStyle) ctx->cb->textStyle(st);
}

// Applies the page reported change to the callback set.
void ApplyChange(Context* ctx, const wchar_t* key, const std::wstring& value) {
    if (!ctx || !ctx->cb) return;
    const bool on = value == L"1";
    const int n = _wtoi(value.c_str());
    const SettingsCallbacks& cb = *ctx->cb;
    const std::wstring k = key;
    if (k.rfind(L"ts", 0) == 0) {
        ApplyTextChange(ctx, k, value);
        return;
    }
    if (k == L"recordHistory" && cb.recordHistory) cb.recordHistory(on);
    else if (k == L"chargeLimit" && cb.chargeLimit) cb.chargeLimit(n);
    else if (k == L"pollSeconds" && cb.pollSeconds) cb.pollSeconds(n);
    else if (k == L"wantWidget" && cb.wantWidget) cb.wantWidget(on);
    else if (k == L"trayWhenEmpty" && cb.trayWhenEmpty) cb.trayWhenEmpty(on);
    else if (k == L"stacked" && cb.stacked) cb.stacked(on);
    else if (k == L"widgetBackground" && cb.widgetBackground) cb.widgetBackground(on);
    else if (k == L"ringStyle" && cb.ringStyle) cb.ringStyle(on);
    else if (k == L"showPercentSign" && cb.showPercentSign) cb.showPercentSign(on);
    else if (k == L"brandColors" && cb.brandColors) cb.brandColors(on);
    else if (k == L"connectionIcons" && cb.connectionIcons) cb.connectionIcons(on);
    else if (k == L"deviceDivider" && cb.deviceDivider) cb.deviceDivider(on);
    else if (k == L"showTooltip" && cb.showTooltip) cb.showTooltip(on);
    else if (k == L"beforeApps" && cb.beforeApps) cb.beforeApps(on);
    else if (k == L"makeRoom" && cb.makeRoom) cb.makeRoom(on);
    else if (k == L"taskbarOffset" && cb.taskbarOffset) cb.taskbarOffset(n);
    else if (k == L"verticalPlace" && cb.verticalPlace) cb.verticalPlace(n);
    else if (k == L"monitorChoice" && cb.monitorChoice) cb.monitorChoice(n);
    else if (k == L"hideLastKnown" && cb.hideLastKnown) cb.hideLastKnown(on);
    else if (k == L"showInactive" && cb.showInactive) cb.showInactive(on);
    else if (k == L"dimInactive" && cb.dimInactive) cb.dimInactive(on);
}

void OnPageMessage(HWND hwnd, ICoreWebView2WebMessageReceivedEventArgs* args) {
    LPWSTR raw = nullptr;
    if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw) return;
    std::wstring msg(raw);
    CoTaskMemFree(raw);

    Context* ctx = Ctx(hwnd);
    if (msg == L"cmd:close") {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    } else if (msg == L"cmd:drag") {
        ReleaseCapture();
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    } else if (msg.rfind(L"set:", 0) == 0) {
        std::wstring body = msg.substr(4);
        const size_t eq = body.find(L'=');
        if (eq != std::wstring::npos) ApplyChange(ctx, body.substr(0, eq).c_str(), body.substr(eq + 1));
    }
}

void CreateWebView(HWND hwnd);        // forward: a failed controller is retried through it
void ShowWebViewRuntimeError(HWND hwnd);

// Puts the page into the window. Shared by the cold path (environment just created) and the warm one.
void CreateController(HWND hwnd, ICoreWebView2Environment* env) {
    env->CreateCoreWebView2Controller(
        hwnd, new ControllerHandler([hwnd](HRESULT cHr, ICoreWebView2Controller* c) {
            Context* ctx = Ctx(hwnd);
            if (!ctx) return;
            if (FAILED(cHr) || !c) {
                // Build the whole thing again once before giving up; never close silently.
                if (ctx->attempts++ < 1) {
                    CreateWebView(hwnd);
                    return;
                }
                ShowWebViewRuntimeError(hwnd);
                return;
            }
            ctx->controller.reset();
            *ctx->controller.put() = c;
            c->AddRef();
            c->get_CoreWebView2(ctx->webview.put());
            if (!ctx->webview) return;
            // Stay invisible until the page has painted, otherwise the empty webview surface shows
            // up as a white (or blank) pane for the length of the start-up.
            c->put_IsVisible(FALSE);

            Com<ICoreWebView2Controller2> c2;
            if (SUCCEEDED(c->QueryInterface(IID_ICoreWebView2Controller2,
                                            reinterpret_cast<void**>(c2.put())))) {
                const COLORREF bg = WindowBackground(ctx->light);
                COREWEBVIEW2_COLOR color{ 255, GetRValue(bg), GetGValue(bg), GetBValue(bg) };
                c2->put_DefaultBackgroundColor(color);
            }

            Com<ICoreWebView2Settings> st;
            if (SUCCEEDED(ctx->webview->get_Settings(st.put())) && st) {
                st->put_AreDefaultContextMenusEnabled(FALSE);
                st->put_AreDevToolsEnabled(FALSE);
                st->put_IsStatusBarEnabled(FALSE);
                st->put_IsZoomControlEnabled(FALSE);
                st->put_AreDefaultScriptDialogsEnabled(FALSE);
                st->put_IsBuiltInErrorPageEnabled(FALSE);
            }

            ResizeWebView(hwnd, ctx);
            ctx->webview->add_WebMessageReceived(
                new MessageHandler([hwnd](ICoreWebView2WebMessageReceivedEventArgs* a) {
                    OnPageMessage(hwnd, a);
                }),
                &ctx->messageToken);
            ctx->webview->add_NavigationCompleted(
                new NavHandler([hwnd](ICoreWebView2NavigationCompletedEventArgs*) {
                    Context* c2 = Ctx(hwnd);
                    if (!c2) return;
                    c2->loading = false;
                    KillTimer(hwnd, kLoadingTimer);
            KillTimer(hwnd, kWatchdogTimer);
                    KillTimer(hwnd, kWatchdogTimer);
                    if (c2->controller) c2->controller->put_IsVisible(TRUE);
                    InvalidateRect(hwnd, nullptr, TRUE);
                }),
                nullptr);
            ctx->webviewReady = true;
            ctx->webview->NavigateToString(ctx->html.c_str());
        }));
}

void ShowWebViewRuntimeError(HWND hwnd) {
    MessageBoxW(hwnd,
                L"BatteryHub needs the Microsoft Edge WebView2 Runtime to show this window.\n\n"
                L"Install it from Microsoft and try again.",
                L"BatteryHub", MB_OK | MB_ICONINFORMATION);
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

void CreateWebView(HWND hwnd) {
    Context* ctx = Ctx(hwnd);
    if (!ctx) return;
    std::wstring loaderPath = AppDataDir() + L"\\WebView2Loader-1.0.4370.26.dll";
    if (!ExtractResourceOnce(GetModuleHandleW(nullptr), L"WEBVIEW2_LOADER", loaderPath)) return;
    HMODULE loader = LoadLibraryW(loaderPath.c_str());
    if (!loader) return;
    using CreateEnvFn = HRESULT(WINAPI*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
                                         ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    auto createEnv = reinterpret_cast<CreateEnvFn>(
        reinterpret_cast<void*>(GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions")));
    if (!createEnv) return;

    std::wstring userData = AppDataDir() + L"\\WebView2";
    HRESULT hr = createEnv(nullptr, userData.c_str(), nullptr,
                           new EnvHandler([hwnd](HRESULT envHr, ICoreWebView2Environment* env) {
                               Context* c = Ctx(hwnd);
                               if (!c) return;
                               if (FAILED(envHr) || !env) {
                                   ShowWebViewRuntimeError(hwnd);
                                   return;
                               }
                               c->env.reset();
                               *c->env.put() = env;  // take over the reference handed to us
                               CreateController(hwnd, env);
                           }));
    if (FAILED(hr)) ShowWebViewRuntimeError(hwnd);
}

// Painted on the native window while the page loads: the window background plus a centred
// "Loading..." whose dots tick, so the click gets an immediate answer instead of a blank pane.
void PaintLoading(HWND hwnd, HDC dc, const Context& ctx) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    HBRUSH br = CreateSolidBrush(WindowBackground(ctx.light));
    FillRect(dc, &rc, br);
    DeleteObject(br);

    const UINT dpi = GetDpiForWindow(hwnd);
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(13, dpi ? static_cast<int>(dpi) : 96, 96);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW(lf.lfFaceName, L"Segoe UI", LF_FACESIZE);
    HFONT font = CreateFontIndirectW(&lf);

    HGDIOBJ oldFont = SelectObject(dc, font ? static_cast<HGDIOBJ>(font) : GetStockObject(DEFAULT_GUI_FONT));
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(dc, ctx.light ? RGB(0x5a, 0x5a, 0x5a) : RGB(0xb4, 0xb4, 0xb4));

    std::wstring base = T(L"Loading");
    std::wstring full = base + L"...";
    SIZE size{};
    GetTextExtentPoint32W(dc, full.c_str(), static_cast<int>(full.size()), &size);
    std::wstring text = base;
    for (int i = 0; i < ctx.dots; i++) text += L".";
    const int x = rc.left + (rc.right - rc.left - size.cx) / 2;
    const int y = rc.top + (rc.bottom - rc.top - size.cy) / 2;
    TextOutW(dc, x, y, text.c_str(), static_cast<int>(text.size()));

    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
    if (font) DeleteObject(font);
}

// Tears the page down in the documented order. The controller is bound to the HWND, so it has to be
// closed before the window goes away; otherwise the environment keeps a half-dead controller and the
// next window created on that environment never gets its controller callback (the panel then sat on
// the loading placeholder until the watchdog closed it).
void CloseWebView(Context* ctx) {
    if (!ctx) return;
    if (ctx->webview && ctx->webviewReady) ctx->webview->remove_WebMessageReceived(ctx->messageToken);
    ctx->webviewReady = false;
    ctx->webview.reset();
    ctx->controller.reset();
    ctx->env.reset();  // last: the environment is what keeps the browser process alive
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        case WM_NCCALCSIZE:
            // The whole window is client area; DWM still draws the rounded frame and shadow.
            if (wp) return 0;
            break;
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_SIZE:
            ResizeWebView(hwnd, Ctx(hwnd));
            return 0;
        case WM_DPICHANGED: {
            auto* rc = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            ResizeWebView(hwnd, Ctx(hwnd));
            return 0;
        }
        case WM_ERASEBKGND: {
            Context* ctx = Ctx(hwnd);
            HDC dc = reinterpret_cast<HDC>(wp);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            HBRUSH br = CreateSolidBrush(WindowBackground(ctx && ctx->light));
            FillRect(dc, &rc, br);
            DeleteObject(br);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            Context* ctx = Ctx(hwnd);
            if (ctx && ctx->loading) PaintLoading(hwnd, dc, *ctx);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wp == kLoadingTimer) {
                Context* ctx = Ctx(hwnd);
                if (!ctx || !ctx->loading) {
                    KillTimer(hwnd, kLoadingTimer);
                    return 0;
                }
                ctx->dots = (ctx->dots + 1) & 3;
                InvalidateRect(hwnd, nullptr, FALSE);  // the background is already painted
                return 0;
            }
            if (wp == kWatchdogTimer) {
                KillTimer(hwnd, kWatchdogTimer);
                Context* ctx = Ctx(hwnd);
                if (!ctx || !ctx->loading) return 0;
                if (ctx->controller) {
                    // Page never reported back but the controller exists: show it rather than keep
                    // the user staring at the placeholder.
                    ctx->loading = false;
                    KillTimer(hwnd, kLoadingTimer);
                    ctx->controller->put_IsVisible(TRUE);
                    InvalidateRect(hwnd, nullptr, TRUE);
                } else {
                    // Nothing came back at all: close rather than sit here forever.
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                }
                return 0;
            }
            break;
        case WM_CLOSE: {
            Context* ctx = Ctx(hwnd);
            if (ctx) {
                ctx->closed = true;
                CloseWebView(ctx);
            }
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, kLoadingTimer);
            // Wake the modal loop below. PostQuitMessage would put a WM_QUIT on the thread queue that
            // this loop never reads (it stops on ctx.closed first), and the app's own message loop
            // would then see it and end the process the moment the window closes.
            PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ApplyWindowFrame(HWND hwnd, bool light) {
    HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dwm) return;
    using Fn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto set = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")));
    if (set) {
        const BOOL dark = light ? FALSE : TRUE;
        if (FAILED(set(hwnd, 20, &dark, sizeof(dark)))) set(hwnd, 19, &dark, sizeof(dark));
        const int round = 2;  // DWMWCP_ROUND
        set(hwnd, 33, &round, sizeof(round));
        const COLORREF border = light ? RGB(0xe2, 0xe2, 0xe2) : RGB(0x3d, 0x3d, 0x3d);
        set(hwnd, 34, &border, sizeof(border));
    }
    FreeLibrary(dwm);
}

}  // namespace

void ShowSettingsPanel(const SettingsData& d, const SettingsCallbacks& cb) {
    if (g_openPanel && IsWindow(g_openPanel)) {
        ShowWindow(g_openPanel, SW_SHOW);
        SetForegroundWindow(g_openPanel);
        return;
    }
    const bool comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

    Context ctx;
    ctx.cb = &cb;
    ctx.style = d.textStyle;
    ctx.light = AppsLightTheme();
    ctx.html = LoadHtmlResource();
    if (ctx.html.empty()) {
        if (comInit) CoUninitialize();
        return;
    }
    const std::wstring boot = BuildBootJson(d);
    const std::wstring token = L"__BOOT_JSON__";
    const size_t at = ctx.html.find(token);
    if (at != std::wstring::npos) ctx.html.replace(at, token.size(), boot);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    const bool registered = RegisterClassExW(&wc) != 0;

    const UINT dpi = GetDpiForSystem();
    const int w = MulDiv(kDesignW, dpi ? (int)dpi : 96, 96);
    const int h = MulDiv(kDesignH, dpi ? (int)dpi : 96, 96);
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    int x = 0, y = 0;
    if (GetMonitorInfoW(mon, &mi)) {
        x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
        y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    }

    HWND hwnd = CreateWindowExW(0, kClassName, T(L"Settings"), WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN, x, y, w, h,
                                nullptr, nullptr, GetModuleHandleW(nullptr), &ctx);
    if (!hwnd) {
        if (registered) UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
        if (comInit) CoUninitialize();
        return;
    }
    g_openPanel = hwnd;
    ApplyWindowFrame(hwnd, ctx.light);
    SetTimer(hwnd, kLoadingTimer, 280, nullptr);
    SetTimer(hwnd, kWatchdogTimer, 3000, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    CreateWebView(hwnd);

    MSG msg;
    while (!ctx.closed) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) break;  // WM_QUIT (system shutdown) or an error
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // Never leave a stray WM_QUIT behind: the caller's own loop would take it as "quit the app".
    PeekMessageW(&msg, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE);

    g_openPanel = nullptr;
    CloseWebView(&ctx);  // no-op when the window closed normally; covers a loop that ended early
    if (registered) UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
    if (comInit) CoUninitialize();
}
