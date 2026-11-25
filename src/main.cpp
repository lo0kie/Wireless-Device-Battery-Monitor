// BatteryHub - tray app showing the battery level of wireless mice, headsets and Bluetooth devices.
#include <windows.h>
#include <shellapi.h>
#include <dbt.h>
#include <initguid.h>
#include <hidclass.h>
#include <bthdef.h>
#include <bluetoothapis.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "device_aliases.h"
#include "known_devices.h"
#include "alert_rules.h"
#include "devices.h"
#include "battery_history.h"
#include "i18n.h"
#include "json_output.h"
#include "labels.h"
#include "refresh_policy.h"
#include "memory_trim.h"
#include "report.h"
#include "rename_dialog.h"
#include "offset_dialog.h"
#include "text_style_dialog.h"
#include "popup_menu.h"
#include "icon_font.h"
#include "widget.h"
#include "update_win.h"
#include "update_apply.h"

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_RESULTS = WM_APP + 2;
constexpr UINT WM_WIDGET_CLICK = WM_APP + 3;
constexpr UINT_PTR TIMER_DEBOUNCE = 1;
constexpr UINT_PTR TIMER_WIDGET = 2;
constexpr UINT_PTR TIMER_ALERT_ICON = 3;
constexpr UINT_PTR TIMER_RESUME = 4;
constexpr GUID kConsoleDisplayStateGuid = {
    0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 }
};
// "Check battery every" choices in seconds; 60 is the default. A poll resets an awake device's sleep timer,
// so a device only sleeps if its timer is shorter than the interval.
constexpr DWORD POLL_SECONDS[] = { 30, 60, 120, 300, 600, 900, 1800, 3600 };
constexpr int LOW_BATTERY = 15;
constexpr UINT PLACEHOLDER_ID = 1;
constexpr UINT ALERT_ICON_ID = 2;

enum MenuId : UINT {
    ID_REFRESH = 100, ID_AUTOSTART, ID_HIDE_LAST_KNOWN, ID_WIDGET_BACKGROUND, ID_EXIT, ID_MODE_WIDGET, ID_MODE_TRAY, ID_SUPPORT, ID_REPORT,
    ID_BRAND_COLORS, ID_STACKED, ID_DEVICE_DIVIDER, ID_SHOW_DISCONNECTED, ID_RECORD_HISTORY, ID_DIM_SLEEPING,
    ID_TRAY_WHEN_EMPTY, ID_MAKE_ROOM, ID_FAR_LEFT, ID_PERCENT_SIGN, ID_RING_STYLE, ID_BEFORE_APPS, ID_CONNECTION_ICONS, ID_KEEP_MOUSE_LAST_KNOWN,
    ID_PLACE_AUTO, ID_PLACE_TOP, ID_PLACE_TRAY, ID_PLACE_BOTTOM, ID_TEXT_STYLE,  // "Position on a vertical taskbar"
    ID_POSITION_FIRST = 700,  // "Position on the taskbar": ID_POSITION_FIRST + 0..5, then the custom value
    ID_POLL_FIRST = 500,  // "Check battery every": ID_POLL_FIRST + 0..7
    ID_LIMIT_OFF = 600, ID_LIMIT_FIRST,  // "Notify when charged to": off, then alerts::kChargeLimits
    ID_UPDATE = 200, ID_AUTO_UPDATE,
    ID_LANG_AUTO = 400, ID_LANG_ADD, ID_LANG_FIRST,  // "Language": ID_LANG_FIRST + 0..63
    ID_MONITOR_ALL = 300, ID_MONITOR_MAIN, ID_MONITOR_FIRST,  // "Show the widget on": ID_MONITOR_FIRST + 0..31
    ID_DEVICE_FIRST = 1000
};
enum DeviceCmd : UINT {
    DEV_TOGGLE, DEV_LEFT, DEV_RIGHT, DEV_RENAME, DEV_RESET_NAME, DEV_HISTORY,
    DEV_ALERT_FIRST = 6, DEV_ALERT_DEFAULT = DEV_ALERT_FIRST + 8, DEV_ALERT_OFF, DEV_STRIDE = 16
};

const wchar_t* kAppName = L"BatteryHub";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"WirelessBattery";

const wchar_t* kSettingsKey = L"Software\\WirelessBattery";

// `--demo`: one sample device of every brand instead of real ones, for screenshots and for checking the
// brand colors without owning the hardware. Keys, vendor ids and names are mixed on purpose, so every way
// of recognising a brand gets used.
bool g_demo = false;

HINSTANCE g_inst;
HWND g_hwnd;
HANDLE g_refreshEvt, g_stopEvt, g_worker;
UINT g_taskbarCreatedMsg;
bool g_lightTaskbar;
bool g_wantWidget = true;      // user's choice: taskbar widget (default) or tray icons
bool g_useWidget = false;      // what is actually shown right now
bool g_hideLastKnown = false;  // hide (last known) / asleep devices from taskbar and tray
bool g_trayWhenEmpty = false;  // taskbar mode: swap the strip for the tray placeholder while no device is shown
bool g_emptyToTray = false;    // that swap is in effect right now
bool g_showDisconnectedDevices = false;  // opt-in, explicit driver-reported offline devices only
bool g_dimSleeping = true;  // dim a sleeping device's last known value (menu option, on by default)
bool g_recordHistory = true;  // save readings for "Battery history..." (menu option, on by default)
int g_chargeLimit = 0;        // "Notify when charged to" 80/85/90%, 0 = off (the default)
std::wstring g_widgetMonitor = L"all";  // which taskbars show the widget: "all", "main" or one monitor id
bool g_widgetBackground = false;  // draw a plate behind the taskbar text (transparent taskbar themes)
bool g_stacked = false;      // icon above the percentage (menu option)
bool g_showPercentSign = true;  // include '%' in taskbar battery labels (menu option)
widget::TextStyle g_textStyle;  // font, size and spacing of the taskbar strip (menu option)
bool g_ringStyle = false;  // device icon inside a battery ring (menu option, PR #110)
bool g_connectionIcons = false;  // wireless mark for 2.4 GHz devices (menu option, PR #2277)
bool g_deviceDivider = false;    // thin line between neighbouring devices on the taskbar (menu option)
bool g_makeRoom = true;      // step past other apps' taskbar widgets (menu option)
// "Position on the taskbar": distance from the notification area in DPI-independent pixels. Stored as
// the TaskbarOffset value the widget has always read, so a value set by hand keeps working.
const int POSITION_OFFSETS[] = { 0, 40, 80, 120, 160, 200 };
widget::VerticalPlace g_verticalPlace = widget::VerticalPlace::Auto;  // column position on a left / right taskbar
bool g_farLeft = false;      // widget at the left end of the taskbar instead of next to the clock (menu option)
bool g_beforeApps = false;   // widget just left of the taskbar apps (menu option, PR #760)
bool g_keepMouseLastKnown = false;  // with "Hide last known", still show mice (menu option, PR #1422)
bool g_brandColors = false;  // off by default; known brands get their color (Razer green, Logitech blue, ...)
std::atomic<DWORD> g_pollIntervalMs{ 60 * 1000 };  // set from the menu, read by the worker thread

struct TrayEntry {
    UINT uid = 0;
    DeviceStatus st;
    int lastPct = -1;        // last known percentage, kept while the device sleeps
    ULONGLONG lastSeen = 0;
    bool added = false;
    bool lowWarned = false;
    bool limitWarned = false;  // "unplug at 80%" already shown during this charge
    std::wstring iconSig;    // what the current icon shows, to avoid needless redraws
    HICON icon = nullptr;
};
std::map<std::wstring, TrayEntry> g_entries;
// Devices seen in an earlier session. Restored as offline placeholders at startup and updated as
// polls answer, so an unplugged device stays in the menu after a restart (see "Show disconnected").
known_devices::Map g_knownDevices;
std::map<std::wstring, bool> g_alertLatches;  // survive transient removal of a device
bool g_alertIconAdded = false;  // short-lived tray icon for widget-mode notifications
bool g_placeholder = false;
HICON g_placeholderIcon = nullptr;

// User-supplied names are presentation-only, keyed by stable DeviceStatus.key.
// Driver identity, HID polling, brand detection and device diagnostics are unchanged.
aliases::AliasMap g_aliases;

std::wstring DisplayName(const TrayEntry& e) {
    return aliases::DisplayName(g_aliases, e.st.key, e.st.name);
}

void LoadAliases() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceAliases", RRF_RT_REG_MULTI_SZ,
                     nullptr, nullptr, &size) != ERROR_SUCCESS) return;
    if (size < 2 * sizeof(wchar_t) || size > 128 * 1024 || size % sizeof(wchar_t)) return;
    std::vector<wchar_t> data(size / sizeof(wchar_t), 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceAliases", RRF_RT_REG_MULTI_SZ,
                     nullptr, data.data(), &size) == ERROR_SUCCESS)
        g_aliases = aliases::Decode(data.data(), size / sizeof(wchar_t));
}

void SaveAliases() {
    auto data = aliases::Encode(g_aliases);
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceAliases", REG_MULTI_SZ,
                    data.data(), (DWORD)(data.size() * sizeof(wchar_t)));
}

// ---- helpers ----------------------------------------------------------------------------------

UINT StableId(const std::wstring& key) {
    UINT h = 2166136261u;
    for (wchar_t c : key) { h ^= c; h *= 16777619u; }
    return (h & 0x7FFFFFFF) | 0x100;
}

bool ReadLightTaskbar() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v != 0;
    return false;
}

// Tells uxtheme to draw the remaining native bits (dialog scrollbars, tooltips) light or dark.
void ApplyMenuTheme(bool light) {
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto setMode = ux ? (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135)) : nullptr;
    static auto flush = ux ? (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136)) : nullptr;
    if (setMode) setMode(light ? 3 : 2);
    if (flush) flush();
}

std::wstring ExePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

// Devices the user chose to hide (by key), stored as a REG_MULTI_SZ.
std::set<std::wstring> g_hidden;

void LoadHidden() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) g_hidden.insert(p);
}

void SaveHidden() {
    std::wstring multi;
    for (const auto& k : g_hidden) { multi += k; multi.push_back(L'\0'); }
    multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", REG_MULTI_SZ, multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

bool IsHidden(const std::wstring& key) { return g_hidden.count(key) != 0; }

// The user's left-to-right order (device keys), stored as a REG_MULTI_SZ. Devices not in it follow
// in the default order (by type, then name).
std::vector<std::wstring> g_order;

void LoadOrder() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) g_order.push_back(p);
}

void SaveOrder() {
    if (g_order.size() > 64) g_order.resize(64);  // forget long-gone devices
    std::wstring multi;
    for (const auto& k : g_order) { multi += k; multi.push_back(L'\0'); }
    multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", REG_MULTI_SZ, multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

// Restore last session's devices as offline placeholders. Without a fresh poll there is no battery
// level, so they only appear when "Show disconnected devices" is on -- but their names and kinds are
// back immediately, and a later poll that answers simply overwrites them.
void RestoreKnownDevices() {
    if (g_demo) return;  // demo data must never enter the real device archive
    g_knownDevices = known_devices::Load(kSettingsKey);
    for (const auto& [key, d] : g_knownDevices) {
        TrayEntry& e = g_entries[key];
        e.uid = StableId(key);
        e.st.key = key;
        e.st.name = d.name;
        e.st.via = d.via;
        e.st.kind = d.kind;
        e.st.connected = false;
        e.st.percent = -1;
        e.lastPct = -1;
    }
}

bool IsEntryHidden(const TrayEntry& e) {
    if (IsHidden(e.st.key)) return true;
    // An explicitly offline device is not just a stale battery reading.
    if (g_showDisconnectedDevices && !e.st.connected) return false;
    // A mouse sleeps between uses; its last level stays useful (PR #1422, opt-in).
    if (g_hideLastKnown && g_keepMouseLastKnown && e.st.kind == DeviceKind::Mouse && e.lastPct >= 0) return false;
    return g_hideLastKnown && (e.st.asleep || (e.st.percent < 0 && e.lastPct >= 0));
}

bool ShouldDim(const DeviceStatus& s) {
    // An explicitly offline device keeps its dimmed "x", whatever the sleeping-device choice is.
    if (!s.connected) return true;
    // A sleeping device has no fresh reading: keep the last one, optionally at full brightness.
    if (s.asleep && !g_dimSleeping) return false;
    return s.percent < 0;
}

bool LoadBoolSetting(const wchar_t* name, bool defaultVal) {
    DWORD v = defaultVal ? 1 : 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v != 0;
}

void SaveBoolSetting(const wchar_t* name, bool on) {
    DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &v, sizeof(v));
}

// A REG_MULTI_SZ with one "device-key<TAB>threshold" entry per override.
// Threshold -1 disables alerts; a missing key uses the default 15%.
std::map<std::wstring, int> g_alertOverrides;

void LoadAlertThresholds() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"AlertThresholds", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size < 2 * sizeof(wchar_t) || size > 128 * 1024) return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"AlertThresholds", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    size_t limit = size / sizeof(wchar_t);
    for (size_t i = 0; i < limit && buf[i];) {
        size_t end = i;
        while (end < limit && buf[end]) ++end;
        if (end == limit) break;
        std::wstring item(buf.data() + i, end - i);
        size_t sep = item.rfind(L'\t');
        if (sep != std::wstring::npos && sep != 0) {
            std::wstring value = item.substr(sep + 1);
            wchar_t* tail = nullptr;
            long n = std::wcstol(value.c_str(), &tail, 10);
            if (tail != value.c_str() && *tail == 0 && n >= -1 && n <= 40 && alerts::ValidOverride((int)n))
                g_alertOverrides[item.substr(0, sep)] = (int)n;
        }
        i = end + 1;
    }
}

void SaveAlertThresholds() {
    std::wstring multi;
    for (const auto& [key, threshold] : g_alertOverrides) {
        multi += key + L"\t" + std::to_wstring(threshold);
        multi.push_back(L'\0');
    }
    multi.push_back(L'\0');
    if (multi.size() == 1) multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"AlertThresholds", REG_MULTI_SZ,
                    multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

bool LoadWidgetMode() { return LoadBoolSetting(L"TaskbarWidget", true); }
void SaveWidgetMode(bool on) { SaveBoolSetting(L"TaskbarWidget", on); }
bool LoadHideLastKnown() { return LoadBoolSetting(L"HideLastKnownDevices", false); }
void SaveHideLastKnown(bool on) { SaveBoolSetting(L"HideLastKnownDevices", on); }
bool LoadTrayWhenEmpty() { return LoadBoolSetting(L"MoveToTrayWhenEmpty", false); }
void SaveTrayWhenEmpty(bool on) { SaveBoolSetting(L"MoveToTrayWhenEmpty", on); }
bool LoadShowDisconnectedDevices() { return LoadBoolSetting(L"ShowDisconnectedDevices", false); }
void SaveShowDisconnectedDevices(bool on) { SaveBoolSetting(L"ShowDisconnectedDevices", on); }
bool LoadDimSleeping() { return LoadBoolSetting(L"DimSleepingDevices", true); }
void SaveDimSleeping(bool on) { SaveBoolSetting(L"DimSleepingDevices", on); }
bool LoadRecordHistory() { return LoadBoolSetting(L"RecordBatteryHistory", true); }
bool LoadMakeRoom() { return LoadBoolSetting(L"MakeRoomForOtherWidgets", true); }
void SaveMakeRoom(bool on) { SaveBoolSetting(L"MakeRoomForOtherWidgets", on); }
// Signed: stored as a DWORD, read back as int, so negative values move the widget towards the tray.
int LoadTaskbarOffset() {
    DWORD v = 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"TaskbarOffset", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return (int)v;
}
void SaveTaskbarOffset(int px) {
    DWORD v = (DWORD)px;
    if (px) RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"TaskbarOffset", REG_DWORD, &v, sizeof(v));
    else RegDeleteKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"TaskbarOffset");
}
void SaveRecordHistory(bool on) { SaveBoolSetting(L"RecordBatteryHistory", on); }
bool LoadWidgetBackground() { return LoadBoolSetting(L"WidgetBackground", false); }
void SaveWidgetBackground(bool on) { SaveBoolSetting(L"WidgetBackground", on); }
std::wstring LoadWidgetMonitor() {
    wchar_t buf[512] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"WidgetMonitor", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS && buf[0])
        return buf;
    return LoadBoolSetting(L"OtherTaskbars", true) ? L"all" : L"main";  // the earlier on/off toggle
}
void SaveWidgetMonitor(const std::wstring& v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"WidgetMonitor", REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}
void SetWidgetMonitor(const std::wstring& v) {
    g_widgetMonitor = v;
    SaveWidgetMonitor(v);
    widget::SetMonitorChoice(v);
}
bool LoadStacked() { return LoadBoolSetting(L"StackedLayout", false); }
void SaveStacked(bool on) { SaveBoolSetting(L"StackedLayout", on); }
bool LoadShowPercentSign() { return LoadBoolSetting(L"ShowPercentSign", true); }
void SaveShowPercentSign(bool on) { SaveBoolSetting(L"ShowPercentSign", on); }
int LoadIntSetting(const wchar_t* name, int def, int min, int max) {
    DWORD v = (DWORD)def, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS) return def;
    return std::clamp((int)v, min, max);
}
// Fractional pixel settings live in a separate key that stores tenths of a pixel ("...Px10"). The
// whole-pixel key an older build wrote is still read when the new one is absent, and rewritten on
// save so downgrading keeps a sensible value.
float LoadPxSetting(const wchar_t* tenths, const wchar_t* whole, float def, float min, float max) {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, tenths, RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return std::clamp((float)v / 10.0f, min, max);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, whole, RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return std::clamp((float)v, min, max);
    return def;
}
void SavePxSetting(const wchar_t* tenths, const wchar_t* whole, float v) {
    DWORD t = (DWORD)std::lround(v * 10.0f);
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, tenths, REG_DWORD, &t, sizeof(t));
    DWORD w = (DWORD)std::lround(v);
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, whole, REG_DWORD, &w, sizeof(w));
}

void SaveIntSetting(const wchar_t* name, int v) {
    DWORD d = (DWORD)v;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &d, sizeof(d));
}
std::wstring LoadTextFont() {
    wchar_t buf[256] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"TextFont", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return buf;
    return L"";  // Windows UI font
}
void SaveTextFont(const std::wstring& face) {
    if (face.empty()) RegDeleteKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"TextFont");
    else RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"TextFont", REG_SZ, face.c_str(),
                         (DWORD)((face.size() + 1) * sizeof(wchar_t)));
}
widget::TextStyle LoadTextStyle() {
    widget::TextStyle s;
    s.face = LoadTextFont();
    s.size = LoadIntSetting(L"TextSizePx", s.size, kMinTextSize, kMaxTextSize);
    s.weight = LoadIntSetting(L"TextWeight", s.weight, kMinTextWeight, kMaxTextWeight);
    s.iconSize = LoadIntSetting(L"TextIconSizePx", s.iconSize, kMinIconSize, kMaxIconSize);
    s.iconGap = LoadPxSetting(L"TextIconGapPx10", L"TextIconGapPx", s.iconGap, kMinIconGap, kMaxIconGap);
    s.itemGap = LoadPxSetting(L"TextItemGapPx10", L"TextItemGapPx", s.itemGap, kMinItemGap, kMaxItemGap);
    s.dividerGap = LoadPxSetting(L"TextDividerGapPx10", L"TextDividerGapPx", s.dividerGap,
                                 kMinDividerGap, kMaxDividerGap);
    s.letterSpacing = LoadPxSetting(L"TextLetterSpacingPx10", L"TextLetterSpacingPx", s.letterSpacing,
                                    kMinLetterSpacing, kMaxLetterSpacing);
    return s;
}
bool LoadRingStyle() { return LoadBoolSetting(L"RingStyle", false); }
void SaveRingStyle(bool on) { SaveBoolSetting(L"RingStyle", on); }
bool LoadConnectionIcons() { return LoadBoolSetting(L"ConnectionIcons", false); }
bool LoadDeviceDivider() { return LoadBoolSetting(L"DeviceDivider", false); }
void SaveConnectionIcons(bool on) { SaveBoolSetting(L"ConnectionIcons", on); }
widget::VerticalPlace LoadVerticalPlace() {
    DWORD v = 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"VerticalPlace", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 1 ? widget::VerticalPlace::Top : v == 2 ? widget::VerticalPlace::AboveTray
         : v == 3 ? widget::VerticalPlace::Bottom : widget::VerticalPlace::Auto;
}
void SaveVerticalPlace(widget::VerticalPlace p) {
    DWORD v = p == widget::VerticalPlace::Top ? 1 : p == widget::VerticalPlace::AboveTray ? 2
              : p == widget::VerticalPlace::Bottom ? 3 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"VerticalPlace", REG_DWORD, &v, sizeof(v));
}
bool LoadFarLeft() { return LoadBoolSetting(L"WidgetFarLeft", false); }
void SaveFarLeft(bool on) { SaveBoolSetting(L"WidgetFarLeft", on); }
bool LoadBeforeApps() { return LoadBoolSetting(L"WidgetBeforeApps", false); }
void SaveBeforeApps(bool on) { SaveBoolSetting(L"WidgetBeforeApps", on); }
bool LoadBrandColors() { return LoadBoolSetting(L"BrandColors", false); }
void SaveBrandColors(bool on) { SaveBoolSetting(L"BrandColors", on); }
DWORD LoadPollSeconds() {
    DWORD v = 60, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"PollIntervalSeconds", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return std::clamp<DWORD>(v, 30, 24 * 60 * 60);
}
void SavePollSeconds(DWORD seconds) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"PollIntervalSeconds", REG_DWORD, &seconds, sizeof(seconds));
}

bool AutostartEnabled() {
    wchar_t buf[MAX_PATH * 2];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return false;
    return _wcsicmp(buf, (L"\"" + ExePath() + L"\"").c_str()) == 0;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

// ---- icon rendering ---------------------------------------------------------------------------
// Tray icons are Material Icons battery glyphs, filled left-to-right by charge level. The glyph is
// drawn white-on-black into a scratch bitmap and its brightness becomes the alpha channel, which
// gives smooth edges on the transparent icon. A tiny Segoe fallback keeps icons present if the
// embedded font ever fails to register.

struct Rgb { BYTE r, g, b; };
Rgb Foreground();

// Segoe fallback for the rare case the embedded font cannot be registered.
static HFONT MakeTrayFallbackFont(int px) {
    return CreateFontW(-px, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");
}

// Renders one icon-font glyph, auto-fitted into the tray icon square.
static HICON RenderGlyph(wchar_t glyph, Rgb tint, bool dim, int fillPct = -1) {
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    UINT dpi = taskbar ? GetDpiForWindow(taskbar) : 0;
    const int S = GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : GetDpiForSystem());

    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    const int W = S * 3, H = S * 2;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    void* sbits = nullptr;
    HBITMAP scratch = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &sbits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, scratch);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    auto* sp = static_cast<DWORD*>(sbits);

    HFONT faceFont = icons::Ready() ? icons::Font(S)
                                    : MakeTrayFallbackFont(S);
    HGDIOBJ oldF = SelectObject(dc, faceFont);
    memset(sbits, 0, (size_t)W * H * 4);
    wchar_t one[2] = { glyph, 0 };
    TextOutW(dc, 2, 0, one, 1);
    GdiFlush();
    SelectObject(dc, oldF);
    if (faceFont) DeleteObject(faceFont);
    int bx0 = W, by0 = H, bx1 = -1, by1 = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (sp[y * W + x] & 0xFF) { bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); by0 = std::min(by0, y); by1 = std::max(by1, y); }

    bi.bmiHeader.biWidth = S;
    bi.bmiHeader.biHeight = -S;
    void* ibits = nullptr;
    HBITMAP colorBmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &ibits, nullptr, 0);
    auto* ip = static_cast<DWORD*>(ibits);
    memset(ip, 0, (size_t)S * S * 4);
    const int alphaScale = dim ? 140 : 255;
    auto put = [&](int x, int y, Rgb c, int a) {
        if (x < 0 || y < 0 || x >= S || y >= S || a <= 0) return;
        a = a * alphaScale / 255;
        ip[y * S + x] = ((DWORD)a << 24) | ((DWORD)c.r << 16) | ((DWORD)c.g << 8) | c.b;
    };
    if (bx1 >= 0) {
        int gw = bx1 - bx0 + 1, gh = by1 - by0 + 1;
        int ox = (S - gw) / 2, oy = (S - gh) / 2;
        int fill = fillPct < 0 ? gw : std::max(fillPct > 0 ? 1 : 0, (fillPct * gw + 50) / 100);
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++) {
                int a = sp[(by0 + y) * W + (bx0 + x)] & 0xFF;
                if (!a) continue;
                if (x < fill) put(ox + x, oy + y, tint, a);
                else put(ox + x, oy + y, g_lightTaskbar ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }, a * 60 / 100);
            }
    }

    SelectObject(dc, oldBmp);
    DeleteObject(scratch);
    DeleteDC(dc);

    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    {
        HDC mdc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(mdc, mask);
        PatBlt(mdc, 0, 0, S, S, BLACKNESS);
        SelectObject(mdc, o);
        DeleteDC(mdc);
    }
    ICONINFO ii{ TRUE, 0, 0, mask, colorBmp };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(colorBmp);
    return icon;
}

HICON RenderIcon(const std::wstring& text, int barPct, Rgb textColor, Rgb barColor, bool dim) {
    if (text == L"-" || text == L"?") return RenderGlyph(icons::G().batteryUnknown, textColor, dim);
    if (text == L"\u00D7") return RenderGlyph(icons::G().offline, textColor, dim);
    // battery_0_bar .. battery_6_bar are glyphs from the icon set: seven charge levels, no hand drawing.
    int level = barPct < 0 ? 0 : std::min(6, std::max(0, (barPct * 6 + 50) / 100));
    return RenderGlyph(icons::G().batteryBars[level], barColor, dim);
}

Rgb Foreground() { return g_lightTaskbar ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }; }

Accent AccentFor(const DeviceStatus& s) { return g_brandColors ? BrandOf(s) : Accent::None; }

HICON IconFor(int pct, bool charging, bool dim, Accent accent) {
    Rgb fg = Foreground();
    BrandColor bc = AccentColor(accent, g_lightTaskbar);
    Rgb bar = charging        ? Rgb{ 0, 170, 255 }
            : pct <= LOW_BATTERY ? Rgb{ 232, 32, 48 }
            : pct <= 30       ? Rgb{ 255, 150, 0 }
            : accent != Accent::None ? Rgb{ bc.r, bc.g, bc.b }
                              : Rgb{ 30, 190, 90 };
    Rgb text = (!charging && pct >= 0 && pct <= LOW_BATTERY) ? Rgb{ 240, 50, 60 } : fg;
    std::wstring t = pct < 0 ? L"?" : std::to_wstring(pct);
    return RenderIcon(t, pct, text, bar, dim);
}

// ---- tray management --------------------------------------------------------------------------

void CopyTip(wchar_t* dst, size_t cap, const std::wstring& s) {
    wcsncpy(dst, s.c_str(), cap - 1);
    dst[cap - 1] = 0;
}

std::wstring StatusText(const TrayEntry& e) {
    const auto& s = e.st;
    if (!s.connected) return T(L"off / not connected");
    std::wstring t;
    if (!s.levelText.empty() && s.percent >= 0) t = T(s.levelText);
    else if (s.percent >= 0) t = std::to_wstring(s.percent) + L"%";
    else if (e.lastPct >= 0) t = TF(L"{percent}% (last known)", { { L"percent", std::to_wstring(e.lastPct) } });
    else t = T(L"unknown");
    if (!s.detail.empty() && s.percent >= 0) t += L" (" + s.detail + L")";
    if (s.full || (s.charging && s.percent >= 100)) t += std::wstring(L", ") + T(L"fully charged");
    else if (s.charging) t += std::wstring(L", ") + T(L"charging");
    if (s.asleep) t += std::wstring(L", ") + T(L"asleep");
    if (!s.note.empty() && s.percent < 0) t += std::wstring(L", ") + T(s.note);
    return t;
}

void TraySetVersion(UINT uid) {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

bool TrayCall(DWORD msg, UINT uid, HICON icon, const std::wstring& tip, const wchar_t* infoTitle = nullptr,
              const std::wstring& info = L"") {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = icon;
    CopyTip(nid.szTip, ARRAYSIZE(nid.szTip), tip);
    if (infoTitle) {
        nid.uFlags |= NIF_INFO;
        CopyTip(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), infoTitle);
        CopyTip(nid.szInfo, ARRAYSIZE(nid.szInfo), info);
        nid.dwInfoFlags = NIIF_WARNING;
    }
    bool ok = Shell_NotifyIconW(msg, &nid) != FALSE;
    if (ok && msg == NIM_ADD) TraySetVersion(uid);
    return ok;
}

// Adds or updates a tray icon; returns whether the shell has it. While Explorer is busy (e.g. at sign-in)
// NIM_ADD can time out and report failure although the icon was added, and NIM_MODIFY fails for an icon
// the shell no longer has, so each one falls back to the other.
bool TrayShow(bool added, UINT uid, HICON icon, const std::wstring& tip, const wchar_t* infoTitle = nullptr,
              const std::wstring& info = L"") {
    if (TrayCall(added ? NIM_MODIFY : NIM_ADD, uid, icon, tip, infoTitle, info)) return true;
    if (!TrayCall(added ? NIM_ADD : NIM_MODIFY, uid, icon, tip, infoTitle, info)) return false;
    if (!added) TraySetVersion(uid);
    return true;
}

void TrayDelete(UINT uid) {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void ShowNotification(TrayEntry& e, const std::wstring& title, const std::wstring& message) {
    if (!g_useWidget && e.added) {
        TrayCall(NIM_MODIFY, e.uid, e.icon, DisplayName(e) + L"\n" + StatusText(e), title.c_str(), message);
        return;
    }
    // Windows notification balloons need a tray icon. Keep a temporary one in widget mode
    // long enough for the shell to display the notification, then remove it.
    if (!g_placeholderIcon) g_placeholderIcon = RenderIcon(L"-", -1, Foreground(), Foreground(), true);
    g_alertIconAdded =
        TrayShow(g_alertIconAdded, ALERT_ICON_ID, g_placeholderIcon, L"BatteryHub", title.c_str(), message);
    if (g_alertIconAdded) SetTimer(g_hwnd, TIMER_ALERT_ICON, 60 * 1000, nullptr);
}

void ShowBatteryAlert(TrayEntry& e) {
    ShowNotification(e, T(L"Low battery"), TF(L"{name} is at {percent}%",
                                              { { L"name", DisplayName(e) }, { L"percent", std::to_wstring(e.st.percent) } }));
}

void ShowChargeLimitAlert(TrayEntry& e) {
    ShowNotification(e, T(L"You can unplug it"),
                     TF(L"{name} is charged to {percent}%. Stopping around here helps the battery last longer.",
                        { { L"name", DisplayName(e) }, { L"percent", std::to_wstring(e.st.percent) } }));
}

void UpdatePlaceholder() {
    if (g_useWidget && !g_emptyToTray) {
        if (g_placeholder) { TrayDelete(PLACEHOLDER_ID); g_placeholder = false; }
        return;
    }
    const bool any = std::any_of(g_entries.begin(), g_entries.end(),
        [](const auto& entry) { return entry.second.added; });
    if (!any && !g_placeholder) {
        if (!g_placeholderIcon) g_placeholderIcon = RenderIcon(L"-", -1, Foreground(), Foreground(), true);
        g_placeholder = TrayShow(false, PLACEHOLDER_ID, g_placeholderIcon,
                                 std::wstring(L"BatteryHub\n") + T(L"No devices with battery info found"));
    } else if (any && g_placeholder) {
        TrayDelete(PLACEHOLDER_ID);
        g_placeholder = false;
    }
}

void ApplyEntry(TrayEntry& e) {
    const auto& s = e.st;
    bool visible = !g_useWidget && !IsEntryHidden(e) && (s.connected || g_showDisconnectedDevices);
    if (!visible) {
        if (e.added) { TrayDelete(e.uid); e.added = false; }
        return;
    }
    // Never display an old percentage as a live reading for an explicitly offline device.
    int shown = !s.connected ? -1 : (s.percent >= 0 ? s.percent : e.lastPct);
    bool dim = ShouldDim(s);
    // Fully charged on power: the normal green bar instead of the blue charging one.
    bool charging = s.connected && !s.asleep && s.charging && !s.full && shown < 100;
    Accent accent = AccentFor(s);
    std::wstring sig = std::to_wstring(shown) + (charging ? L"c" : L"") + (dim ? L"d" : L"") + (g_lightTaskbar ? L"l" : L"k") +
                       L"a" + std::to_wstring((int)accent) + (s.connected ? L"|on" : L"|off");
    if (sig != e.iconSig || !e.icon) {
        if (e.icon) DestroyIcon(e.icon);
        e.icon = s.connected ? IconFor(shown, charging, dim, accent)
                             : RenderIcon(L"\u00D7", -1, Foreground(), Foreground(), true);
        e.iconSig = sig;
    }
    std::wstring tip = DisplayName(e) + L"\n" + StatusText(e);

    e.added = TrayShow(e.added, e.uid, e.icon, tip);
}

int KindOrder(DeviceKind k) {  // default order: headset first, then mouse, ...
    switch (k) {
        case DeviceKind::Headset:  return 0;
        case DeviceKind::Mouse:    return 1;
        case DeviceKind::Keyboard: return 2;
        case DeviceKind::Gamepad:  return 3;
        case DeviceKind::Phone:    return 4;
        default:                   return 5;
    }
}

bool IsShown(const TrayEntry& e) { return (e.st.connected || g_showDisconnectedDevices) && !IsEntryHidden(e); }

// Entries in left-to-right order: the user's order first, then the rest by type and name. Works the
// same for every kind of device (receivers, Bluetooth, controllers, HeadsetControl) as it only uses
// the device key.
std::vector<TrayEntry*> OrderedEntries(bool shownOnly) {
    std::vector<TrayEntry*> list;
    for (auto& [k, e] : g_entries)
        if (!shownOnly || IsShown(e)) list.push_back(&e);
    auto rank = [](const TrayEntry* e) {
        auto it = std::find(g_order.begin(), g_order.end(), e->st.key);
        return it == g_order.end() ? INT_MAX : (int)(it - g_order.begin());
    };
    std::stable_sort(list.begin(), list.end(), [&](const TrayEntry* a, const TrayEntry* b) {
        int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        int ka = KindOrder(a->st.kind), kb = KindOrder(b->st.kind);
        if (ka != kb) return ka < kb;
        return DisplayName(*a) != DisplayName(*b) ? DisplayName(*a) < DisplayName(*b) : a->st.key < b->st.key;  // same name: keep a fixed order
    });
    return list;
}

// The `via` string is driver-supplied; classify its common forms so the small mark can show how a
// device is connected. Unknown kinds stay unmarked instead of guessing.
LinkKind LinkFor(const std::wstring& via) {
    if (via.rfind(L"2.4 GHz", 0) == 0 || via.find(L"receiver") != std::wstring::npos ||
        via.find(L"dongle") != std::wstring::npos || via.find(L"base station") != std::wstring::npos ||
        via.find(L"Xbox Wireless") != std::wstring::npos ||
        via.find(L"PlayStation Link") != std::wstring::npos ||
        via.find(L"Phone Link") != std::wstring::npos)
        return LinkKind::Wireless;
    if (via.find(L"Bluetooth") != std::wstring::npos) return LinkKind::Bluetooth;
    if (via.find(L"USB") != std::wstring::npos || via == L"HID") return LinkKind::Wired;
    return LinkKind::None;
}

void UpdateWidget() {
    if (!g_useWidget || !widget::Alive()) return;  // not alive while the strip is swapped for the tray
    std::vector<TrayEntry*> list = OrderedEntries(true);
    // Two devices of one kind share an icon: give each a tiny caption so they can be told apart.
    std::vector<LabelInput> labelIn;
    for (auto* e : list) labelIn.push_back({ e->st.kind, DisplayName(*e) });
    std::vector<std::wstring> labels = ShortLabels(labelIn, 3);
    std::vector<WidgetItem> items;
    std::wstring tip;
    for (auto* e : list) {
        WidgetItem it;
        it.kind = e->st.kind;
        bool offline = !e->st.connected;
        it.percent = offline ? -1 : (e->st.percent >= 0 ? e->st.percent : e->lastPct);
        if (offline) it.text = L"--%";  // clear status, not a stale percentage
        else if (e->st.percent >= 0) it.text = T(e->st.levelText);
        it.charging = !offline && !e->st.asleep && e->st.charging;
        it.full = !offline && (e->st.full || (e->st.charging && it.percent >= 100));
        it.dim = ShouldDim(e->st);
        it.low = !offline && !it.charging && it.percent >= 0 && it.percent <= LOW_BATTERY;
        it.bluetooth = e->st.via == L"Bluetooth";
        it.link = LinkFor(e->st.via);  // keep the mark even when offline, so a dropped device still says how it was connected
        it.label = labels[items.size()];
        it.accent = AccentFor(e->st);
        items.push_back(it);
        if (!tip.empty()) tip += L"\n";
        tip += DisplayName(*e) + L": " + StatusText(*e);
    }
    if (tip.empty()) tip = std::wstring(L"BatteryHub: ") + T(L"No devices with battery info found");
    widget::SetItems(items, tip);
}

void Reconcile();

// Last percentage read while NOT charging, per device name (the name, not the key: a device moved
// from its receiver to a cable gets a new key). Some devices restart their charge estimate when a
// cable goes in -- a Logitech G502 X reports ~50% at first and only reaches its real 70-something
// after a while -- but a battery does not drop while charging, so until the reading catches up the
// last value from before charging is shown.
struct IdleReading { int pct; ULONGLONG at; };
std::map<std::wstring, IdleReading> g_idlePct;
constexpr ULONGLONG kIdlePctMaxAgeMs = 12ull * 60 * 60 * 1000;

void SmoothCharging(DeviceStatus& s) {
    if (s.percent < 0 || s.name.empty() || s.name.find(L"wireless device") != std::wstring::npos) return;
    ULONGLONG now = GetTickCount64();
    if (!s.charging) {
        g_idlePct[s.name] = { s.percent, now };
        return;
    }
    auto it = g_idlePct.find(s.name);
    if (it == g_idlePct.end() || now - it->second.at > kIdlePctMaxAgeMs) return;
    if (s.percent < it->second.pct) {
        s.percent = it->second.pct;
        s.levelText.clear();
    } else {
        g_idlePct.erase(it);  // caught up: the device's own reading is trusted again
    }
}

void OnResults(std::vector<DeviceStatus>& results) {
    for (auto& s : results) SmoothCharging(s);
    // Remember each answering device's identity for the next session. Write once per poll and only
    // if something changed, so the archive does not churn the registry every minute.
    bool archiveChanged = false;
    for (const auto& s : results)
        if (!g_demo && known_devices::Remember(g_knownDevices, s)) archiveChanged = true;
    if (archiveChanged) known_devices::Save(kSettingsKey, g_knownDevices);

    std::map<std::wstring, bool> seen;
    for (auto& s : results) {
        auto& e = g_entries[s.key];
        if (!e.uid) e.uid = StableId(s.key);
        const int previousPercent = e.st.connected && !e.st.asleep ? e.st.percent : -1;  // the last fresh reading
        e.st = s;
        if (s.percent >= 0) e.lastPct = s.percent;
        e.lastSeen = GetTickCount64();
        seen[s.key] = true;
        // Evaluate even in widget mode: ApplyEntry only manages tray icons.
        // Alert latches are kept by stable device key even if the driver briefly stops reporting it.
        if (const auto it = g_alertLatches.find(s.key); it != g_alertLatches.end())
            e.lowWarned = it->second;
        bool notify = !IsHidden(s.key) &&
                      alerts::ShouldNotify(s, alerts::EffectiveThreshold(g_alertOverrides, s.key), e.lowWarned);
        g_alertLatches[s.key] = e.lowWarned;
        if (g_alertLatches.size() > 256) g_alertLatches.erase(g_alertLatches.begin());
        if (g_recordHistory && s.connected && !s.asleep && s.percent >= 0)
            battery_history::Observe(s.key, s.percent, s.charging);
        bool limit = !IsHidden(s.key) && alerts::ShouldNotifyChargeLimit(s, g_chargeLimit, previousPercent, e.limitWarned);
        ApplyEntry(e);
        if (notify) ShowBatteryAlert(e);
        else if (limit) ShowChargeLimitAlert(e);
    }
    // Devices answering right now, by name: a device plugged in by cable (or on Bluetooth) also
    // has an entry from its receiver, which stops answering then. Generic names are not matched.
    std::set<std::wstring> liveNames;
    for (auto& s : results)
        if (s.connected && !s.asleep && s.percent >= 0 && s.name.find(L"wireless device") == std::wstring::npos)
            liveNames.insert(s.name);
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        auto& e = it->second;
        // Some receivers drop sleeping devices from the link; keep showing the last value for a while
        // (at least two poll intervals), unless the same device is answering another way (then that one
        // is the current value).
        bool inGrace = !seen.count(it->first) && e.st.graceSec > 0 && e.st.connected &&
                       GetTickCount64() - e.lastSeen <
                           std::max<ULONGLONG>((ULONGLONG)e.st.graceSec * 1000, 2ULL * g_pollIntervalMs) &&
                       !liveNames.count(e.st.name);
        if (inGrace) {
            e.st.percent = -1;
            e.st.asleep = true;
            ApplyEntry(e);
        }
        if (!seen.count(it->first) && !inGrace) {
            // A device that stops answering drops out of every driver's results, which used to erase
            // its entry. Keep it instead, marked explicitly offline: "Show disconnected devices" can
            // then bring it back as a dimmed cross, and a device that answers again just overwrites
            // this state on the next poll. Entries are still bounded by the keys ever seen on this PC.
            TrayEntry& e = it->second;
            if (e.st.connected) {
                e.st.connected = false;   // drives both the dimmed icon and the "off" tooltip
                e.st.asleep = false;      // it is gone, not sleeping
                e.st.percent = -1;        // never show the last reading as a live one
                ApplyEntry(e);            // remove its tray icon unless the opt-in is on
            }
            ++it;
        } else {
            ++it;
        }
    }
    Reconcile();  // react to the new device list now, not at the next timer tick
    UpdatePlaceholder();
    UpdateWidget();
    memory_trim::Soon();  // the poll (HID, Bluetooth, drivers) is done: give its pages back until the next one
}

bool NothingShown() {
    for (const auto& [k, e] : g_entries)
        if (IsShown(e)) return false;
    return true;
}

// Makes the actual display match the chosen mode; falls back to tray icons if the taskbar
// widget cannot be created (e.g. Explorer not ready yet). Called once a second.
// With "Move to tray when no devices" the strip gives way to the tray placeholder while nothing is shown.
void Reconcile() {
    // Taskbar mode, option on, nothing to show: the strip is replaced by the tray placeholder.
    bool emptyToTray = g_wantWidget && g_trayWhenEmpty && NothingShown();
    bool wasAlive = widget::Alive();
    // Keep, create or remove the strip.
    bool alive = g_wantWidget && !emptyToTray && wasAlive;
    if (g_wantWidget && !emptyToTray && !alive) alive = widget::Create(g_inst, g_hwnd, WM_WIDGET_CLICK, g_lightTaskbar);
    if (!g_wantWidget || emptyToTray) widget::Destroy();
    // Stays in widget mode while swapped, so device icons are not added to the tray in between.
    bool useWidget = alive || emptyToTray;
    bool changed = useWidget != g_useWidget || emptyToTray != g_emptyToTray;
    g_useWidget = useWidget;
    g_emptyToTray = emptyToTray;
    // Refresh after a change, or when the strip was just re-created.
    if (changed || (alive && !wasAlive)) {
        for (auto& [k, e] : g_entries) ApplyEntry(e);
        UpdatePlaceholder();
        UpdateWidget();
    } else if (g_useWidget) {
        widget::Reposition();
    } else {
        // Retry tray icons whose NIM_ADD failed while the shell notification area was starting.
        bool anyAdded = false;
        for (auto& [k, e] : g_entries) {
            if (!e.added && IsShown(e)) ApplyEntry(e);
            anyAdded |= e.added;
        }
        if (anyAdded == g_placeholder) UpdatePlaceholder();
    }
}

void SetMode(bool useWidget) {
    g_wantWidget = useWidget;
    SaveWidgetMode(useWidget);
    Reconcile();
}

void SetWidgetBackground(bool on) {
    g_widgetBackground = on;
    SaveWidgetBackground(on);
    widget::SetBackground(on);
}

void SetKeepMouseLastKnown(bool on) {
    g_keepMouseLastKnown = on;
    SaveBoolSetting(L"KeepMouseLastKnown", on);
    for (auto& [k, e] : g_entries) ApplyEntry(e);
    UpdatePlaceholder();
    UpdateWidget();
}

void SetHideLastKnown(bool on) {
    g_hideLastKnown = on;
    SaveHideLastKnown(on);
    for (auto& [k, e] : g_entries) ApplyEntry(e);
    UpdatePlaceholder();
    UpdateWidget();
}

void SetTrayWhenEmpty(bool on) {
    g_trayWhenEmpty = on;
    SaveTrayWhenEmpty(on);
    Reconcile();
}

void SetShowDisconnectedDevices(bool on) {
    g_showDisconnectedDevices = on;
    SaveShowDisconnectedDevices(on);
    for (auto& [k, e] : g_entries) ApplyEntry(e);
    UpdatePlaceholder();
    UpdateWidget();
}

void SetDimSleeping(bool on) {
    g_dimSleeping = on;
    SaveDimSleeping(on);
    for (auto& [k, e] : g_entries) ApplyEntry(e);
    UpdateWidget();
}

void ReAddAll() {
    g_placeholder = false;
    for (auto& [k, e] : g_entries) { e.added = false; ApplyEntry(e); }
    UpdatePlaceholder();
}

// Switches the UI language ("" = follow Windows) and redraws everything that shows text.
void SetLanguage(const std::wstring& code);

void RedrawAll() {
    if (g_alertIconAdded) {  // alert icon shares the placeholder's HICON
        KillTimer(g_hwnd, TIMER_ALERT_ICON);
        TrayDelete(ALERT_ICON_ID);
        g_alertIconAdded = false;
    }
    if (g_placeholderIcon) {
        DestroyIcon(g_placeholderIcon);
        g_placeholderIcon = nullptr;
        if (g_placeholder) { TrayDelete(PLACEHOLDER_ID); g_placeholder = false; }
    }
    for (auto& [k, e] : g_entries) { e.iconSig.clear(); ApplyEntry(e); }
    UpdatePlaceholder();
}

void SetLanguage(const std::wstring& code) {
    i18n::SetChoice(code);
    RedrawAll();  // tray tooltips and the placeholder text
    UpdateWidget();
}

void SetConnectionIcons(bool on) {
    g_connectionIcons = on;
    SaveConnectionIcons(on);
    widget::SetConnectionIcons(on);
}

void SetDeviceDivider(bool on) {
    g_deviceDivider = on;
    SaveBoolSetting(L"DeviceDivider", on);
    widget::SetDeviceDivider(on);
}

void SetRingStyle(bool on) {
    g_ringStyle = on;
    SaveRingStyle(on);
    widget::SetRingStyle(on);
}

void SetShowPercentSign(bool on) {
    g_showPercentSign = on;
    SaveShowPercentSign(on);
    widget::SetShowPercentSign(on);
}

void ApplyTextStyle(const widget::TextStyle& style) {
    g_textStyle = style;
    SaveTextFont(style.face);
    SaveIntSetting(L"TextSizePx", style.size);
    SaveIntSetting(L"TextWeight", style.weight);
    SaveIntSetting(L"TextIconSizePx", style.iconSize);
    SavePxSetting(L"TextIconGapPx10", L"TextIconGapPx", style.iconGap);
    SavePxSetting(L"TextItemGapPx10", L"TextItemGapPx", style.itemGap);
    SavePxSetting(L"TextDividerGapPx10", L"TextDividerGapPx", style.dividerGap);
    SavePxSetting(L"TextLetterSpacingPx10", L"TextLetterSpacingPx", style.letterSpacing);
    widget::SetTextStyle(style);
}

void SetStacked(bool on) {
    g_stacked = on;
    SaveStacked(on);
    widget::SetStacked(on);
}

void SetMakeRoom(bool on) {
    g_makeRoom = on;
    SaveMakeRoom(on);
    widget::SetMakeRoom(on);
}

void SetTaskbarOffset(int px) {
    SaveTaskbarOffset(px);
    if (g_useWidget) widget::Reposition();
}

void SetWidgetPlace(widget::VerticalPlace p) {
    g_verticalPlace = p;
    SaveVerticalPlace(p);
    widget::SetVerticalPlace(p);
}

void SetBeforeApps(bool on) {
    g_beforeApps = on;
    SaveBeforeApps(on);
    widget::SetBeforeApps(on);
}

void SetFarLeft(bool on) {
    g_farLeft = on;
    SaveFarLeft(on);
    widget::SetFarLeft(on);
}

void SetBrandColors(bool on) {
    g_brandColors = on;
    SaveBrandColors(on);
    RedrawAll();
    UpdateWidget();
}

void SetChargeLimit(int limit) {
    g_chargeLimit = limit;
    DWORD v = (DWORD)limit;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"ChargeLimitAlert", REG_DWORD, &v, sizeof(v));
    for (auto& [k, e] : g_entries) e.limitWarned = false;
}

int LoadChargeLimit() {
    DWORD v = 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"ChargeLimitAlert", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    for (int l : alerts::kChargeLimits)
        if ((int)v == l) return l;
    return 0;
}

void SetPollSeconds(DWORD seconds) {
    g_pollIntervalMs = seconds * 1000;
    SavePollSeconds(seconds);
    SetEvent(g_refreshEvt);  // poll now, so the new interval starts right away
}

void ToggleHidden(const std::wstring& key) {
    if (!g_hidden.erase(key)) g_hidden.insert(key);
    SaveHidden();
    auto it = g_entries.find(key);
    if (it != g_entries.end()) ApplyEntry(it->second);
    UpdatePlaceholder();
    UpdateWidget();
}

void RenameDevice(const std::wstring& key) {
    auto it = g_entries.find(key);
    if (it == g_entries.end()) return;
    const std::wstring originalName = it->second.st.name;
    // Shared by Save and the dialog's Apply button: write the alias and refresh the display.
    auto applyName = [&](const std::wstring& chosen) {
        if (chosen == originalName) g_aliases.erase(key);
        else if (g_aliases.count(key) || g_aliases.size() < aliases::kMaxAliases) g_aliases[key] = chosen;
        else return;  // bounded registry value; don't silently drop other users' names
        SaveAliases();
        // The modal editor pumps the message queue, so polling may remove the entry.
        // Resolve the iterator again rather than dereferencing a potentially stale one.
        auto cur = g_entries.find(key);
        if (cur != g_entries.end()) ApplyEntry(cur->second);
        UpdateWidget();
    };
    std::wstring chosen;
    if (!PromptDeviceName(g_hwnd, DisplayName(it->second), chosen,
                          [&](const std::wstring& n) { applyName(n); }))
        return;
    applyName(chosen);
}

void ResetDeviceName(const std::wstring& key) {
    if (!g_aliases.erase(key)) return;
    SaveAliases();
    auto it = g_entries.find(key);
    if (it != g_entries.end()) ApplyEntry(it->second);
    UpdateWidget();
}

void SetAlertOverride(const std::wstring& key, int threshold) {
    if (threshold == alerts::kUseDefault) g_alertOverrides.erase(key);
    else if (alerts::ValidOverride(threshold)) g_alertOverrides[key] = threshold;
    else return;
    SaveAlertThresholds();
    g_alertLatches.erase(key);
    auto it = g_entries.find(key);
    if (it != g_entries.end()) it->second.lowWarned = false;
    SetEvent(g_refreshEvt);  // reconsider with a fresh reading; never alert from stale data
}

// Swaps a shown device with its neighbour (dir -1 = left, +1 = right) and remembers the new order.
void MoveEntry(const std::wstring& key, int dir) {
    std::vector<TrayEntry*> list = OrderedEntries(true);
    auto pos = std::find_if(list.begin(), list.end(), [&](const TrayEntry* e) { return e->st.key == key; });
    if (pos == list.end()) return;
    ptrdiff_t i = pos - list.begin(), j = i + dir;
    if (j < 0 || j >= (ptrdiff_t)list.size()) return;
    std::swap(list[i], list[j]);
    std::vector<std::wstring> order;
    for (auto* e : list) order.push_back(e->st.key);
    for (auto& k : g_order)  // keep the places of devices that are not shown right now
        if (std::find(order.begin(), order.end(), k) == order.end()) order.push_back(k);
    g_order = order;
    SaveOrder();
    UpdateWidget();
}

// `settings=true` opens the right-click menu (application settings); false is the left-click menu
// (devices and their actions). Both are built from the same item list so command ids and the keys /
// languages / monitors tables stay in sync; only the top-level rows offered differ.
void ShowMenu(bool settings) {
    using Item = popup_menu::Item;
    auto raw = [](std::wstring text, UINT_PTR id) {
        // Menu items open a dialog or submenu right away, so a trailing "..." adds no information.
        // Strip it from the shown text; the i18n key (with "...") stays intact so translations work.
        while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) text.pop_back();
        if (text.size() >= 3 && text.compare(text.size() - 3, 3, L"...") == 0)
            text.resize(text.size() - 3);
        Item it;
        it.text = std::move(text);
        it.id = id;
        return it;
    };
    auto sep = [] {
        Item it;
        it.kind = Item::Kind::Separator;
        return it;
    };
    auto dot = [](Item& it) {  // one of an exclusive group
        it.radio = true;
        return it;
    };
    std::vector<Item> top;
    std::vector<std::wstring> keys;  // device index -> device key; menu id ID_DEVICE_FIRST + index * DEV_STRIDE + command
    std::vector<TrayEntry*> all = OrderedEntries(false), shown = OrderedEntries(true);
    if (all.empty()) {
        Item none = raw(T(L"No devices with battery info found"), 0);
        none.disabled = true;
        top.push_back(none);
        top.push_back(sep());
    }
    for (TrayEntry* e : all) {
        const std::wstring& k = e->st.key;
        UINT base = ID_DEVICE_FIRST + (UINT)keys.size() * DEV_STRIDE;
        auto pos = std::find(shown.begin(), shown.end(), e);
        bool canLeft = pos != shown.end() && pos != shown.begin();
        bool canRight = pos != shown.end() && pos + 1 != shown.end();
        std::vector<Item> sub;
        Item toggle = raw(g_wantWidget ? T(L"Show on the taskbar") : T(L"Show in the tray"), base + DEV_TOGGLE);
        toggle.checked = !IsHidden(k);
        sub.push_back(toggle);
        Item left = raw(g_wantWidget ? T(L"Move left") : T(L"Move up"), base + DEV_LEFT);
        left.disabled = !canLeft;
        sub.push_back(left);
        Item right = raw(g_wantWidget ? T(L"Move right") : T(L"Move down"), base + DEV_RIGHT);
        right.disabled = !canRight;
        sub.push_back(right);
        sub.push_back(sep());
        Item rename = raw(T(L"Rename device..."), base + DEV_RENAME);

        sub.push_back(rename);
        Item reset = raw(T(L"Reset to original name"), base + DEV_RESET_NAME);
        reset.disabled = !g_aliases.count(k);
        sub.push_back(reset);
        sub.push_back(sep());

        int selectedThreshold = alerts::OverrideFor(g_alertOverrides, k);
        std::vector<Item> alertItems;
        for (int i = 0; i < alerts::kThresholdCount; ++i) {
            const int level = alerts::kThresholds[i];
            // The default level is one of the choices: its label carries "(default)" and picking it
            // clears the per-device override, exactly like the old separate "Use default" row did.
            const bool isDefault = level == alerts::kDefaultThreshold;
            const std::wstring label = isDefault
                ? TF(L"{level}% (default)", { { L"level", std::to_wstring(level) } })
                : std::to_wstring(level) + L"%";
            Item th = raw(label, base + DEV_ALERT_FIRST + i);
            th.checked = selectedThreshold == level || (isDefault && selectedThreshold == alerts::kUseDefault);
            alertItems.push_back(dot(th));
        }
        alertItems.push_back(sep());
        Item alertsOff = raw(T(L"Disable notifications"), base + DEV_ALERT_OFF);
        alertsOff.checked = selectedThreshold == alerts::kDisabled;
        alertItems.push_back(dot(alertsOff));
        Item alertsItem = raw(T(L"Battery alerts"), 0);

        alertsItem.children = std::move(alertItems);
        sub.push_back(alertsItem);
        Item history = raw(T(L"Battery history..."), base + DEV_HISTORY);

        sub.push_back(history);

        Item entry = raw(DisplayName(*e), 0);
        entry.rightText = StatusText(*e);

        entry.children = std::move(sub);
        top.push_back(std::move(entry));
        keys.push_back(k);
    }
    // Categories, top to bottom: the devices, the action on that list, battery behaviour, the
    // interface, then about/help, and Exit alone at the bottom.
    if (!all.empty()) top.push_back(sep());
    Item refresh = raw(T(L"Refresh now"), ID_REFRESH);
    top.push_back(refresh);
    top.push_back(sep());
    Item history = raw(T(L"Record battery history"), ID_RECORD_HISTORY);
    history.checked = g_recordHistory;
    top.push_back(history);
    {
        std::vector<Item> sub;
        UINT checked = ID_LIMIT_OFF;
        for (UINT i = 0; i < (UINT)alerts::kChargeLimitCount; i++)
            if (g_chargeLimit == alerts::kChargeLimits[i]) checked = ID_LIMIT_FIRST + i;
        for (UINT i = 0; i < (UINT)alerts::kChargeLimitCount; i++) {
            Item it = raw(std::to_wstring(alerts::kChargeLimits[i]) + L"%", ID_LIMIT_FIRST + i);
            it.checked = checked == ID_LIMIT_FIRST + i;
            sub.push_back(dot(it));
        }
        sub.push_back(sep());
        Item off = raw(T(L"Off"), ID_LIMIT_OFF);
        off.checked = checked == ID_LIMIT_OFF;
        sub.push_back(dot(off));
        Item item = raw(T(L"Notify when charged to"), 0);
        item.children = std::move(sub);
        top.push_back(item);
    }
    {
        std::vector<Item> sub;
        const UINT count = ARRAYSIZE(POLL_SECONDS);
        for (UINT i = 0; i < count; i++) {
            DWORD secs = POLL_SECONDS[i];
            std::wstring label = secs < 60 ? TF(L"{seconds} s", { { L"seconds", std::to_wstring(secs) } })
                                          : TF(L"{minutes} min", { { L"minutes", std::to_wstring(secs / 60) } });
            Item it = raw(label, ID_POLL_FIRST + i);
            it.checked = secs * 1000 == g_pollIntervalMs;
            sub.push_back(dot(it));
        }
        Item item = raw(T(L"Check battery every"), 0);

        item.children = std::move(sub);
        top.push_back(item);
    }
    top.push_back(sep());
    // Display settings (PR #3292): how and where the devices are shown, in one submenu.
    std::vector<Item> display;
    {
        // Taskbar widget or tray icons: one choice, so it lives one level down like the other groups.
        std::vector<Item> modeSub;
        Item widgetMode = raw(T(L"Show on the taskbar"), ID_MODE_WIDGET);
        widgetMode.checked = g_wantWidget;
        modeSub.push_back(dot(widgetMode));
        Item trayMode = raw(T(L"Show as tray icons"), ID_MODE_TRAY);
        trayMode.checked = !g_wantWidget;
        modeSub.push_back(dot(trayMode));
        Item empty = raw(T(L"Move to tray when no devices"), ID_TRAY_WHEN_EMPTY);
        empty.checked = g_trayWhenEmpty;
        empty.disabled = !g_wantWidget;
        modeSub.push_back(empty);
        // How that widget looks: colour, shape, background and unit, in the same submenu.
        modeSub.push_back(sep());
        Item stacked = raw(T(L"Icons above percentages"), ID_STACKED);
        stacked.checked = g_stacked;
        stacked.disabled = !g_wantWidget;
        modeSub.push_back(stacked);
        Item bg = raw(T(L"Background behind taskbar text"), ID_WIDGET_BACKGROUND);
        bg.checked = g_widgetBackground;
        bg.disabled = !g_wantWidget;
        modeSub.push_back(bg);
        Item ring = raw(T(L"Ring style"), ID_RING_STYLE);
        ring.checked = g_ringStyle;
        ring.disabled = !g_wantWidget;
        modeSub.push_back(ring);
        Item sign = raw(T(L"Show percent sign"), ID_PERCENT_SIGN);
        sign.checked = g_showPercentSign;
        sign.disabled = !g_wantWidget;
        modeSub.push_back(sign);
        Item brand = raw(T(L"Brand colors"), ID_BRAND_COLORS);
        brand.checked = g_brandColors;
        modeSub.push_back(brand);
        Item link = raw(T(L"Mark connection type"), ID_CONNECTION_ICONS);
        link.checked = g_connectionIcons;
        link.disabled = !g_wantWidget;
        modeSub.push_back(link);
        Item divider = raw(T(L"Separator between devices"), ID_DEVICE_DIVIDER);
        divider.checked = g_deviceDivider;
        divider.disabled = !g_wantWidget;
        modeSub.push_back(divider);
        modeSub.push_back(sep());
        modeSub.push_back(raw(T(L"Text style..."), ID_TEXT_STYLE));  // also styles the tray digits
        Item mode = raw(T(L"Display style"), 0);
        mode.children = std::move(modeSub);
        display.push_back(mode);
        display.push_back(sep());
    }
    {
        std::vector<Item> sub;
        const UINT count = ARRAYSIZE(POSITION_OFFSETS);
        const int current = LoadTaskbarOffset();
        UINT checked = ID_POSITION_FIRST + count;
        for (UINT i = 0; i < count; i++) {
            int px = POSITION_OFFSETS[i];
            Item it = raw(px ? TF(L"{px} px from the notification area", { { L"px", std::to_wstring(px) } })
                             : T(L"Next to the notification area"),
                          ID_POSITION_FIRST + i);
            it.checked = px == current;
            it.disabled = !g_wantWidget;
            sub.push_back(dot(it));
            if (px == current) checked = ID_POSITION_FIRST + i;
        }
        // Any other value (typed in, or set by hand in the registry) shows as the custom entry.
        std::wstring custom = checked == ID_POSITION_FIRST + count
                                  ? TF(L"Custom ({px} px)...", { { L"px", std::to_wstring(current) } })
                                  : std::wstring(T(L"Custom..."));
        Item it = raw(custom, ID_POSITION_FIRST + count);
        it.checked = checked == ID_POSITION_FIRST + count;
        it.disabled = !g_wantWidget;
        sub.push_back(dot(it));
        sub.push_back(sep());
        Item room = raw(T(L"Make room for other taskbar widgets"), ID_MAKE_ROOM);
        room.checked = g_makeRoom;
        room.disabled = !g_wantWidget;
        sub.push_back(room);
        sub.push_back(sep());
        Item farLeft = raw(T(L"At the left end of the taskbar"), ID_FAR_LEFT);
        farLeft.checked = g_farLeft;
        farLeft.disabled = !g_wantWidget;
        sub.push_back(farLeft);
        Item before = raw(T(L"Just before the taskbar apps"), ID_BEFORE_APPS);
        before.checked = g_beforeApps;
        before.disabled = !g_wantWidget;
        sub.push_back(before);
        Item item = raw(T(L"Position on the taskbar"), 0);
        item.children = std::move(sub);
        item.disabled = !g_wantWidget;
        display.push_back(item);
    }
    // Left / right taskbar: choose where the column sits.
    if (widget::Vertical()) {
        std::vector<Item> sub;
        const UINT checked = g_verticalPlace == widget::VerticalPlace::Top        ? ID_PLACE_TOP
                             : g_verticalPlace == widget::VerticalPlace::AboveTray ? ID_PLACE_TRAY
                             : g_verticalPlace == widget::VerticalPlace::Bottom    ? ID_PLACE_BOTTOM
                                                                                   : ID_PLACE_AUTO;
        for (UINT id : { (UINT)ID_PLACE_AUTO, (UINT)ID_PLACE_TOP, (UINT)ID_PLACE_TRAY, (UINT)ID_PLACE_BOTTOM }) {
            const wchar_t* label = id == ID_PLACE_AUTO    ? T(L"Automatic (keeps clear of app icons)")
                                   : id == ID_PLACE_TOP   ? T(L"Top of the taskbar")
                                   : id == ID_PLACE_TRAY  ? T(L"Above the notification area")
                                                          : T(L"Bottom of the taskbar (moves the taskbar icons up, Windows 11)");
            Item it = raw(label, id);
            it.checked = id == checked;
            sub.push_back(dot(it));
        }
        Item item = raw(T(L"Position on a vertical taskbar"), 0);
        item.children = std::move(sub);
        display.push_back(item);
    }
    // With more than one monitor: choose which taskbars show the widget (all, main only, or one monitor).
    auto monitors = widget::OtherMonitors();
    if (monitors.size() > 31) monitors.resize(31);
    if (!monitors.empty()) {
        std::vector<Item> sub;
        UINT checked = g_widgetMonitor == L"main" ? ID_MONITOR_MAIN : ID_MONITOR_ALL;
        bool found = g_widgetMonitor == L"all" || g_widgetMonitor == L"main";
        std::vector<std::wstring> ids;
        for (const auto& mo : monitors) ids.push_back(mo.id);
        int chosen = found ? -1 : widget::FindMonitor(ids, g_widgetMonitor);
        Item allItem = raw(T(L"All monitors"), ID_MONITOR_ALL);
        allItem.checked = checked == ID_MONITOR_ALL;
        allItem.disabled = !g_wantWidget;
        sub.push_back(dot(allItem));
        Item mainItem = raw(T(L"Main monitor only"), ID_MONITOR_MAIN);
        mainItem.checked = checked == ID_MONITOR_MAIN;
        mainItem.disabled = !g_wantWidget;
        sub.push_back(dot(mainItem));
        for (UINT i = 0; i < monitors.size(); i++) {
            Item it = raw(TF(L"{monitor} only", { { L"monitor", monitors[i].label } }), ID_MONITOR_FIRST + i);
            if ((int)i == chosen) {
                checked = ID_MONITOR_FIRST + i;
                found = true;
            }
            it.checked = (int)i == chosen;
            it.disabled = !g_wantWidget;
            sub.push_back(dot(it));
        }
        if (!found) {  // the chosen monitor is unplugged: the main taskbar shows the widget meanwhile
            Item it = raw(T(L"Chosen monitor (not connected, shown on main)"), ID_MONITOR_FIRST + 31);
            it.disabled = true;
            it.checked = true;
            sub.push_back(dot(it));
            checked = ID_MONITOR_FIRST + 31;
        }
        Item item = raw(T(L"Show the widget on"), 0);

        item.children = std::move(sub);
        item.disabled = !g_wantWidget;
        display.push_back(item);
    }
    {
        display.push_back(sep());
        Item hide = raw(T(L"Hide last known / asleep devices"), ID_HIDE_LAST_KNOWN);
        hide.checked = g_hideLastKnown;
        display.push_back(hide);
        Item keep = raw(T(L"Keep last known battery for mice"), ID_KEEP_MOUSE_LAST_KNOWN);
        keep.checked = g_keepMouseLastKnown;
        keep.disabled = !g_hideLastKnown;
        display.push_back(keep);
        display.push_back(sep());
        Item disconnected = raw(T(L"Show disconnected devices"), ID_SHOW_DISCONNECTED);
        disconnected.checked = g_showDisconnectedDevices;
        display.push_back(disconnected);
        Item dim = raw(T(L"Dim sleeping devices"), ID_DIM_SLEEPING);
        dim.checked = g_dimSleeping;
        display.push_back(dim);
    }
    {
        Item item = raw(T(L"Display settings"), 0);
        item.children = std::move(display);
        top.push_back(item);
    }
    top.push_back(sep());
    // Language: automatic, every built-in or user-added language (in its own name), and a way to add one.
    i18n::Init();  // picks up language files added since the last time
    auto languages = i18n::Available();
    if (languages.size() > 64) languages.resize(64);
    {
        std::vector<Item> sub;
        Item autoItem = raw(T(L"Automatic (Windows language)"), ID_LANG_AUTO);
        autoItem.checked = i18n::Choice().empty();
        sub.push_back(dot(autoItem));
        sub.push_back(sep());
        for (UINT i = 0; i < languages.size(); i++) {
            Item it = raw(languages[i].name, ID_LANG_FIRST + i);
            bool chosen = !i18n::Choice().empty() && _wcsicmp(languages[i].code.c_str(), i18n::Choice().c_str()) == 0;
            it.checked = chosen;
            sub.push_back(dot(it));
        }
        sub.push_back(sep());
        sub.push_back(raw(T(L"Add your own language..."), ID_LANG_ADD));
        Item item = raw(T(L"Language"), 0);

        item.children = std::move(sub);
        top.push_back(item);
    }
    {
        // Still the interface group: starting with Windows belongs next to the language choice.
        Item autostart = raw(T(L"Start with Windows"), ID_AUTOSTART);
        autostart.checked = AutostartEnabled();
        top.push_back(autostart);
        top.push_back(sep());
        // About and help: updates first, then the two ways to help the project.
        Item update = raw(updates::MenuLabel(), ID_UPDATE);
        update.disabled = updates::Busy();
        top.push_back(update);
        Item autoUpdate = raw(T(L"Automatically check for updates"), ID_AUTO_UPDATE);
        autoUpdate.checked = updates::AutomaticEnabled();
        autoUpdate.disabled = !updates::Supported();
        top.push_back(autoUpdate);
        Item report = raw(T(L"Request support for a device..."), ID_REPORT);
        top.push_back(report);
        Item support = raw(T(L"Support the project..."), ID_SUPPORT);
        top.push_back(support);
        top.push_back(sep());
        Item exit = raw(T(L"Exit"), ID_EXIT);
        top.push_back(exit);
    }

    // Split into the two menus. Device rows and Refresh belong to the left-click menu; everything
    // that configures the application belongs to the right-click menu. Exit and the empty-list
    // placeholder are useful in both. Item ids are chosen so a row's group can be recognised without
    // duplicating the build: device rows are the only parent rows with a rightText (their status).
    {
        std::vector<Item> filtered;
        for (const Item& it : top) {
            const bool isSep = it.kind == Item::Kind::Separator;
            const bool isDevice = it.id == 0 && !it.rightText.empty() && !it.children.empty();
            const bool isBoth = isSep || it.id == ID_EXIT || (it.id == 0 && it.disabled && it.children.empty() && it.rightText.empty());
            const bool isLeft = isDevice || it.id == ID_REFRESH;
            bool keep = isBoth || (settings ? !isLeft : isLeft);
            if (keep) {
                // Drop a separator that opens the list or follows another one.
                bool prevSep = !filtered.empty() && filtered.back().kind == Item::Kind::Separator;
                if (!(isSep && (filtered.empty() || prevSep))) filtered.push_back(it);
            }
        }
        while (!filtered.empty() && filtered.back().kind == Item::Kind::Separator) filtered.pop_back();
        top = std::move(filtered);
    }

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    // Opened from the taskbar, the menu keeps clear of it: the last items (Exit) would otherwise end up
    // under the taskbar.
    HWND under = GetAncestor(WindowFromPoint(pt), GA_ROOT);
    wchar_t cls[32] = {};
    if (under) GetClassNameW(under, cls, ARRAYSIZE(cls));
    bool fromTaskbar = !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd");
    popup_menu::Options opt;
    opt.light = g_lightTaskbar;
    opt.rtl = i18n::Rtl();
    if (fromTaskbar) {
        opt.hasExclude = true;
        GetWindowRect(under, &opt.exclude);
    }
    UINT_PTR cmd = 0;
    bool shownMenu = popup_menu::Show(g_hwnd, pt, top, opt, cmd);
    if (!shownMenu) return;
    memory_trim::Soon();
    switch (cmd) {
        case ID_REFRESH: SetEvent(g_refreshEvt); break;
        case ID_AUTOSTART: SetAutostart(!AutostartEnabled()); break;
        case ID_HIDE_LAST_KNOWN: SetHideLastKnown(!g_hideLastKnown); break;
        case ID_KEEP_MOUSE_LAST_KNOWN: SetKeepMouseLastKnown(!g_keepMouseLastKnown); break;
        case ID_TRAY_WHEN_EMPTY: SetTrayWhenEmpty(!g_trayWhenEmpty); break;
        case ID_SHOW_DISCONNECTED: SetShowDisconnectedDevices(!g_showDisconnectedDevices); break;
        case ID_DIM_SLEEPING: SetDimSleeping(!g_dimSleeping); break;
        case ID_RECORD_HISTORY: g_recordHistory = !g_recordHistory; SaveRecordHistory(g_recordHistory); break;
        case ID_LIMIT_OFF: SetChargeLimit(0); break;
        case ID_WIDGET_BACKGROUND: SetWidgetBackground(!g_widgetBackground); break;
        case ID_MONITOR_ALL: SetWidgetMonitor(L"all"); break;
        case ID_LANG_AUTO: SetLanguage(L""); break;
        case ID_LANG_ADD: i18n::OpenUserFolder(); break;
        case ID_MONITOR_MAIN: SetWidgetMonitor(L"main"); break;
        case ID_BRAND_COLORS: SetBrandColors(!g_brandColors); break;
        case ID_STACKED: SetStacked(!g_stacked); break;
        case ID_PERCENT_SIGN: SetShowPercentSign(!g_showPercentSign); break;
        case ID_TEXT_STYLE: {
            widget::TextStyle style = g_textStyle;
            if (PromptTextStyle(g_hwnd, style, style,
                                [](const widget::TextStyle& s) { ApplyTextStyle(s); }))
                ApplyTextStyle(style);
            break;
        }
        case ID_RING_STYLE: SetRingStyle(!g_ringStyle); break;
        case ID_CONNECTION_ICONS: SetConnectionIcons(!g_connectionIcons); break;
        case ID_DEVICE_DIVIDER: SetDeviceDivider(!g_deviceDivider); break;
        case ID_MAKE_ROOM: SetMakeRoom(!g_makeRoom); break;
        case ID_PLACE_AUTO: SetWidgetPlace(widget::VerticalPlace::Auto); break;
        case ID_PLACE_TOP: SetWidgetPlace(widget::VerticalPlace::Top); break;
        case ID_PLACE_TRAY: SetWidgetPlace(widget::VerticalPlace::AboveTray); break;
        case ID_PLACE_BOTTOM: SetWidgetPlace(widget::VerticalPlace::Bottom); break;
        case ID_FAR_LEFT:  // the two left spots exclude each other
            if (!g_farLeft && g_beforeApps) SetBeforeApps(false);
            SetFarLeft(!g_farLeft);
            break;
        case ID_BEFORE_APPS:
            if (!g_beforeApps && g_farLeft) SetFarLeft(false);
            SetBeforeApps(!g_beforeApps);
            break;
        case ID_MODE_WIDGET: SetMode(true); break;
        case ID_MODE_TRAY: SetMode(false); break;
        case ID_SUPPORT: ShellExecuteW(nullptr, L"open", L"https://github.com/sponsors/krisgfx9", nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_REPORT: report::Show(g_inst, g_hwnd); break;
        case ID_UPDATE: updates::Activate(g_hwnd); break;
        case ID_AUTO_UPDATE: updates::ToggleAutomatic(); break;
        case ID_EXIT: DestroyWindow(g_hwnd); break;
        default:
            if (cmd >= ID_POSITION_FIRST && cmd < ID_POSITION_FIRST + ARRAYSIZE(POSITION_OFFSETS)) {
                SetTaskbarOffset(POSITION_OFFSETS[cmd - ID_POSITION_FIRST]);
                break;
            }
            if (cmd == ID_POSITION_FIRST + ARRAYSIZE(POSITION_OFFSETS)) {
                int px = 0;
                if (PromptTaskbarOffset(g_hwnd, LoadTaskbarOffset(), px,
                                        [](int v) { SetTaskbarOffset(v); }))
                    SetTaskbarOffset(px);
                break;
            }
            if (cmd >= ID_LIMIT_FIRST && cmd < ID_LIMIT_FIRST + (UINT)alerts::kChargeLimitCount) {
                SetChargeLimit(alerts::kChargeLimits[cmd - ID_LIMIT_FIRST]);
            } else if (cmd >= ID_POLL_FIRST && cmd < ID_POLL_FIRST + ARRAYSIZE(POLL_SECONDS)) {
                SetPollSeconds(POLL_SECONDS[cmd - ID_POLL_FIRST]);
            } else if (cmd >= ID_LANG_FIRST && cmd < ID_LANG_FIRST + languages.size()) {
                SetLanguage(languages[cmd - ID_LANG_FIRST].code);
            } else if (cmd >= ID_MONITOR_FIRST && cmd < ID_MONITOR_FIRST + monitors.size()) {
                SetWidgetMonitor(monitors[cmd - ID_MONITOR_FIRST].id);
            } else if (cmd >= ID_DEVICE_FIRST && cmd < ID_DEVICE_FIRST + keys.size() * DEV_STRIDE) {
                std::wstring key = keys[(cmd - ID_DEVICE_FIRST) / DEV_STRIDE];
                switch ((cmd - ID_DEVICE_FIRST) % DEV_STRIDE) {
                    case DEV_TOGGLE: ToggleHidden(key); break;
                    case DEV_LEFT:   MoveEntry(key, -1); break;
                    case DEV_RIGHT:  MoveEntry(key, +1); break;
                    case DEV_RENAME: RenameDevice(key); break;
                    case DEV_RESET_NAME: ResetDeviceName(key); break;
                    case DEV_ALERT_OFF: SetAlertOverride(key, alerts::kDisabled); break;
                    case DEV_HISTORY: {
                        auto it = g_entries.find(key);
                        if (it != g_entries.end())
                            battery_history::Show(g_hwnd, key, DisplayName(it->second));
                        break;
                    }
                    default: {
                        UINT slot = (cmd - ID_DEVICE_FIRST) % DEV_STRIDE;
                        if (slot >= DEV_ALERT_FIRST && slot < DEV_ALERT_FIRST + alerts::kThresholdCount) {
                            const int level = alerts::kThresholds[slot - DEV_ALERT_FIRST];
                            // Selecting the default level removes the override, so a future change of
                            // the app-wide default is still picked up for this device.
                            SetAlertOverride(key, level == alerts::kDefaultThreshold ? alerts::kUseDefault : level);
                        }
                        break;
                    }
                }
            }
    }
}

// ---- worker thread ----------------------------------------------------------------------------

std::vector<DeviceStatus> DemoDevices() {
    auto make = [](const wchar_t* key, const wchar_t* name, DeviceKind kind, int pct, unsigned short vid, bool charging) {
        DeviceStatus s;
        s.key = key;
        s.name = name;
        s.kind = kind;
        s.percent = pct;
        s.vid = vid;
        s.charging = charging;
        s.via = L"demo";
        return s;
    };
    std::vector<DeviceStatus> devices = {
        make(L"razer:demo", L"Razer DeathAdder V3 Pro", DeviceKind::Mouse, 78, 0x1532, false),
        make(L"demo:logi", L"Logitech G PRO X 2 LIGHTSPEED", DeviceKind::Headset, 64, 0x046D, false),
        make(L"steelseries:demo", L"SteelSeries Arctis Nova 7", DeviceKind::Headset, 55, 0, false),
        make(L"hsc:1b1c:0a6b", L"Corsair HS80 RGB Wireless", DeviceKind::Headset, 81, 0x1B1C, false),
        make(L"hyperx:demo", L"HyperX Cloud III Wireless", DeviceKind::Headset, 47, 0, false),
        make(L"rog:demo", L"ROG Harpe Ace", DeviceKind::Mouse, 90, 0, false),
        make(L"zowie:demo", L"ZOWIE EC2-CW", DeviceKind::Mouse, 66, 0x04A5, false),
        make(L"bt:demo-ps", L"Wireless Controller", DeviceKind::Gamepad, 38, 0x054C, false),
        make(L"xbox:demo", L"Xbox Wireless Controller", DeviceKind::Gamepad, 72, 0x045E, false),
        make(L"bt:demo-buds", L"Galaxy Buds3 Pro", DeviceKind::Headset, 59, 0, false),
        make(L"airpods:demo", L"AirPods Pro", DeviceKind::Headset, 85, 0, false),
        make(L"jabra:demo", L"Jabra Evolve2 65", DeviceKind::Headset, 23, 0x0B0E, false),
        make(L"jbl:demo", L"JBL Quantum 910", DeviceKind::Headset, 70, 0x0ECB, false),
        make(L"coolermaster:demo", L"Cooler Master MM712", DeviceKind::Mouse, 12, 0x2516, false),
        make(L"steam:demo", L"Steam Controller", DeviceKind::Gamepad, 50, 0x28DE, true),
        make(L"bt:demo-plain", L"Bluetooth Headphones", DeviceKind::Headset, 100, 0, true),  // charging at 100%: fully charged
    };
    DeviceStatus offline = make(L"demo:offline-receiver", L"Offline wireless headset", DeviceKind::Headset, -1, 0, false);
    offline.via = L"2.4 GHz receiver";
    offline.connected = false;
    devices.push_back(offline);  // hidden by default, shown when the opt-in setting is enabled
    return devices;
}

// A closer check of the next polls (see refresh_policy.h), handed to the worker for its next poll.
SRWLOCK g_monitorLock = SRWLOCK_INIT;
refresh_policy::Mode g_monitorRequest = refresh_policy::Mode::Idle;
std::set<std::wstring> g_monitorKeys;
std::set<std::wstring> g_awakeBeforeSleep;
DWORD g_displayState = 1;  // GUID_CONSOLE_DISPLAY_STATE: 0 off, 1 on, 2 dimmed

// Bluetooth connections (GUID_BLUETOOTH_HCI_EVENT) are reported only to windows that registered a
// handle to the radio.
struct BluetoothRadioWatch {
    HANDLE radio = nullptr;
    HDEVNOTIFY notify = nullptr;
};
std::vector<BluetoothRadioWatch> g_btRadioWatches;

void ClearBluetoothRadioWatches() {
    for (auto& w : g_btRadioWatches) {
        if (w.notify) UnregisterDeviceNotification(w.notify);
        if (w.radio) CloseHandle(w.radio);
    }
    g_btRadioWatches.clear();
}

void DropBluetoothRadioWatch(HANDLE radio) {
    for (auto it = g_btRadioWatches.begin(); it != g_btRadioWatches.end(); ++it) {
        if (it->radio != radio) continue;
        if (it->notify) UnregisterDeviceNotification(it->notify);
        if (it->radio) CloseHandle(it->radio);
        g_btRadioWatches.erase(it);
        return;
    }
}

void RefreshBluetoothRadioWatches() {
    ClearBluetoothRadioWatches();
    if (!g_hwnd) return;
    BLUETOOTH_FIND_RADIO_PARAMS params{ sizeof(params) };
    HANDLE radio = nullptr;
    HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&params, &radio);
    if (!find) return;
    do {
        DEV_BROADCAST_HANDLE filter{ sizeof(filter) };
        filter.dbch_devicetype = DBT_DEVTYP_HANDLE;
        filter.dbch_handle = radio;
        HDEVNOTIFY notify = RegisterDeviceNotificationW(g_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
        if (notify) {
            g_btRadioWatches.push_back({ radio, notify });
        } else {
            CloseHandle(radio);
        }
        radio = nullptr;
    } while (BluetoothFindNextRadio(find, &radio));
    BluetoothFindRadioClose(find);
}

std::set<std::wstring> AwakeDevices() {
    std::set<std::wstring> keys;
    for (const auto& [k, e] : g_entries)
        if (e.st.connected && !e.st.asleep) keys.insert(k);
    return keys;
}

std::set<std::wstring> DevicesWithLevel() {
    std::set<std::wstring> keys;
    for (const auto& [k, e] : g_entries)
        if (e.st.connected && e.st.percent >= 0) keys.insert(k);
    return keys;
}

void RequestMonitor(refresh_policy::Mode mode, std::set<std::wstring> keys) {
    AcquireSRWLockExclusive(&g_monitorLock);
    if (mode != refresh_policy::Mode::Arrival || g_monitorRequest != refresh_policy::Mode::Resume) {
        g_monitorRequest = mode;
        g_monitorKeys = std::move(keys);
    }
    ReleaseSRWLockExclusive(&g_monitorLock);
}

DWORD WINAPI Worker(LPVOID) {
    refresh_policy::Monitor monitor;
    monitor.BeginStartup();
    HANDLE events[2] = { g_stopEvt, g_refreshEvt };
    for (;;) {
        AcquireSRWLockExclusive(&g_monitorLock);
        refresh_policy::Mode request = std::exchange(g_monitorRequest, refresh_policy::Mode::Idle);
        std::set<std::wstring> keys = std::move(g_monitorKeys);
        g_monitorKeys.clear();
        ReleaseSRWLockExclusive(&g_monitorLock);
        if (request == refresh_policy::Mode::Resume) monitor.BeginResume(std::move(keys));
        else if (request == refresh_policy::Mode::Arrival) monitor.BeginArrival(std::move(keys));

        auto* v = new std::vector<DeviceStatus>(g_demo ? DemoDevices() : PollAllDevices());
        DWORD delay = monitor.DelayAfterPoll(*v, g_pollIntervalMs);
        if (!PostMessageW(g_hwnd, WM_RESULTS, 0, (LPARAM)v)) delete v;
        DWORD wait = WaitForMultipleObjects(2, events, FALSE, delay);
        if (wait == WAIT_OBJECT_0) return 0;
        monitor.OnWaitFinished(wait == WAIT_TIMEOUT);
    }
}

void RefreshSoon(UINT ms) { SetTimer(g_hwnd, TIMER_DEBOUNCE, ms, nullptr); }
void RefreshAfterWake(UINT ms) { SetTimer(g_hwnd, TIMER_RESUME, ms, nullptr); }

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreatedMsg && msg != 0) {  // Explorer restarted
        widget::Destroy();
        g_useWidget = false;
        g_alertIconAdded = false;
        KillTimer(g_hwnd, TIMER_ALERT_ICON);
        ReAddAll();
        Reconcile();
        return 0;
    }
    switch (msg) {
        case WM_RESULTS: {
            auto* v = reinterpret_cast<std::vector<DeviceStatus>*>(lp);
            OnResults(*v);
            delete v;
            return 0;
        }
        case WM_TRAY:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU: ShowMenu(true); break;   // right click: settings
                case NIN_SELECT:                               // left click: devices
                case NIN_KEYSELECT: ShowMenu(false); break;    // keyboard: same as left
            }
            return 0;
        case WM_WIDGET_CLICK:
            ShowMenu(wp != 0);  // widget posts 1 for right click, 0 for left
            return 0;
        case WM_ENDSESSION:  // sign-out / shutdown / installer restart manager
            if (wp) DestroyWindow(h);
            return 0;
        case WM_DEVICECHANGE: {
            auto* hdr = reinterpret_cast<const DEV_BROADCAST_HDR*>(lp);
            bool handle = hdr && hdr->dbch_devicetype == DBT_DEVTYP_HANDLE;
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
                if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                    auto* iface = reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(lp);
                    if (iface->dbcc_classguid == GUID_BTHPORT_DEVICE_INTERFACE) RefreshBluetoothRadioWatches();
                    else if (wp == DBT_DEVICEARRIVAL) RequestMonitor(refresh_policy::Mode::Arrival, DevicesWithLevel());
                }
                if (handle && wp == DBT_DEVICEREMOVECOMPLETE)
                    DropBluetoothRadioWatch(reinterpret_cast<const DEV_BROADCAST_HANDLE*>(lp)->dbch_handle);
                RefreshSoon(2500);
            } else if (handle && (wp == DBT_DEVICEQUERYREMOVE || wp == DBT_DEVICEREMOVEPENDING)) {
                // Let the radio go: an open handle would block its removal.
                DropBluetoothRadioWatch(reinterpret_cast<const DEV_BROADCAST_HANDLE*>(lp)->dbch_handle);
            } else if (handle && wp == DBT_DEVICEQUERYREMOVEFAILED) {
                RefreshBluetoothRadioWatches();
            } else if (handle && wp == DBT_CUSTOMEVENT) {
                auto* bh = reinterpret_cast<const DEV_BROADCAST_HANDLE*>(lp);
                // HCI_CONNECTION_TYPE_*: 1 classic (ACL), 2 the voice link of a call (SCO), 3 Bluetooth LE.
                constexpr UCHAR kHciAcl = 1, kHciSco = 2;
                BTH_HCI_EVENT_INFO info{};
                if (bh->dbch_eventguid == GUID_BLUETOOTH_HCI_EVENT &&
                    bh->dbch_size >= offsetof(DEV_BROADCAST_HANDLE, dbch_data) + sizeof(info)) {
                    memcpy(&info, bh->dbch_data, sizeof(info));
                    if (info.connectionType != kHciSco) {
                        // A Bluetooth headset reports its level a few seconds after it connects.
                        if (info.connected && info.connectionType == kHciAcl)
                            RequestMonitor(refresh_policy::Mode::Arrival, DevicesWithLevel());
                        RefreshSoon(2500);
                    }
                }
            }
            return TRUE;
        }
        case WM_POWERBROADCAST:
            if (wp == PBT_APMSUSPEND) {
                g_awakeBeforeSleep = AwakeDevices();
            } else if (wp == PBT_APMRESUMEAUTOMATIC) {
                RefreshAfterWake(5000);
            } else if (wp == PBT_POWERSETTINGCHANGE && lp) {
                // Modern Standby may not report a suspend / resume: the display turning off and on stands in.
                auto* ps = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
                if (ps->PowerSetting == kConsoleDisplayStateGuid && ps->DataLength >= sizeof(DWORD)) {
                    DWORD state = *reinterpret_cast<const DWORD*>(ps->Data);
                    if (g_displayState != 0 && state == 0) g_awakeBeforeSleep = AwakeDevices();
                    else if (g_displayState == 0 && state != 0) RefreshAfterWake(3000);
                    g_displayState = state;
                }
            }
            return TRUE;
        case WM_TIMER:
            if (wp == TIMER_DEBOUNCE) { KillTimer(h, TIMER_DEBOUNCE); SetEvent(g_refreshEvt); }
            if (wp == TIMER_RESUME) {
                KillTimer(h, TIMER_RESUME);
                RequestMonitor(refresh_policy::Mode::Resume, std::move(g_awakeBeforeSleep));
                g_awakeBeforeSleep.clear();
                SetEvent(g_refreshEvt);
            }
            if (wp == TIMER_WIDGET) { Reconcile(); updates::Tick(h); }
            if (wp == TIMER_ALERT_ICON) {
                KillTimer(h, TIMER_ALERT_ICON);
                if (g_alertIconAdded) { TrayDelete(ALERT_ICON_ID); g_alertIconAdded = false; }
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && !lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet")) {
                bool light = ReadLightTaskbar();
                if (light != g_lightTaskbar) { g_lightTaskbar = light; ApplyMenuTheme(light); RedrawAll(); widget::SetLightTheme(light); }
            }
            return 0;
        case WM_DESTROY:
            updates::Shutdown();
            KillTimer(h, TIMER_DEBOUNCE);
            KillTimer(h, TIMER_RESUME);
            KillTimer(h, TIMER_WIDGET);
            KillTimer(h, TIMER_ALERT_ICON);
            if (g_alertIconAdded) { TrayDelete(ALERT_ICON_ID); g_alertIconAdded = false; }
            widget::Destroy();
            SetEvent(g_stopEvt);
            if (g_worker) WaitForSingleObject(g_worker, 5000);
            for (auto& [k, e] : g_entries) {
                if (e.added) TrayDelete(e.uid);
                if (e.icon) DestroyIcon(e.icon);
            }
            if (g_placeholder) TrayDelete(PLACEHOLDER_ID);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// `BatteryHub.exe --list` prints what it finds, `--dump` every HID collection.
int ListToConsole(bool dump) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    auto write = [&](const std::wstring& s) {
        int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string u(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), u.data(), n, nullptr, nullptr);
        DWORD w;
        WriteFile(out, u.data(), (DWORD)u.size(), &w, nullptr);
    };
    if (dump) {
        // Also saved to a file: PowerShell does not wait for this windowed exe, so console output can end up
        // after the prompt or get lost, and a double-click has no console at all.
        std::wstring text = DumpHidCollections();
        wchar_t path[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, path);
        wcscat_s(path, L"BatteryHub-dump.txt");
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            std::wstring crlf;
            for (wchar_t ch : text) crlf += ch == L'\n' ? std::wstring(L"\r\n") : std::wstring(1, ch);
            int n = WideCharToMultiByte(CP_UTF8, 0, crlf.c_str(), (int)crlf.size(), nullptr, 0, nullptr, nullptr);
            std::string u = "\xEF\xBB\xBF" + std::string(n, 0);  // UTF-8 byte order mark, so Notepad shows it right
            WideCharToMultiByte(CP_UTF8, 0, crlf.c_str(), (int)crlf.size(), u.data() + 3, n, nullptr, nullptr);
            DWORD w;
            WriteFile(f, u.data(), (DWORD)u.size(), &w, nullptr);
            CloseHandle(f);
        }
        if (out && out != INVALID_HANDLE_VALUE) {
            write(text);
            write(std::wstring(L"\nAlso saved to ") + path + L"\n");
        } else {
            ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL);  // no console: show the file
        }
        return 0;
    }
    auto list = PollAllDevices();
    if (list.empty()) write(L"No devices with battery info found.\n");
    for (auto& s : list) {
        Accent accent = BrandOf(s);
        std::wstring line = s.name + L"  [" + s.via + L"]  " +
                            (accent != Accent::None ? L"{" + std::wstring(StyleOf(accent).name) + L"}  " : L"");
        if (!s.connected) line += L"off / not connected";
        else if (s.asleep) line += L"asleep";
        else if (!s.levelText.empty()) line += s.levelText;
        else if (s.percent >= 0) line += std::to_wstring(s.percent) + L"%";
        else line += L"unknown";
        if (!s.detail.empty()) line += L" (" + s.detail + L")";
        if (s.full || (s.charging && s.percent >= 100)) line += L", fully charged";
        else if (s.charging) line += L", charging";
        if (!s.note.empty() && s.percent < 0) line += L", " + s.note;
        if (s.millivolts) line += L", " + std::to_wstring(s.millivolts) + L" mV";
        write(line + L"   (" + s.key + L")\n");
    }
    return 0;
}

// Machine-readable device list, shared with the Linux entry point (src/json_output.h).
int ListJson() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS))
            out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    std::string text = DevicesToJson(PollAllDevices());  // UTF-8
    if (out && out != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(out, text.data(), (DWORD)text.size(), &written, nullptr);
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    if (cmdLine && wcsstr(cmdLine, L"--json")) return ListJson();
    if (cmdLine && wcsstr(cmdLine, L"--help")) {
        MessageBoxW(nullptr,
                    L"BatteryHub options:\n\n"
                    L"--list   Print devices and battery levels\n"
                    L"--json   Print a JSON array of devices and battery levels\n"
                    L"--dump   Print every HID collection and device\n"
                    L"--report Open the \"Request support for a device\" window\n"
                    L"--demo   Show sample devices\n"
                    L"--quit   Quit a running instance",
                    L"BatteryHub", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int applyResult = argv ? updates::RunApplyMode(argc, argv) : -1;
    if (argv) LocalFree(argv);
    if (applyResult >= 0) return applyResult;
    if (cmdLine && wcsstr(cmdLine, L"--list")) return ListToConsole(false);
    if (cmdLine && wcsstr(cmdLine, L"--dump")) return ListToConsole(true);
    if (cmdLine && wcsstr(cmdLine, L"--report")) {  // the "Request support for a device" window on its own
        i18n::Init();
        report::Show(inst, nullptr);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (report::PreTranslate(&msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--quit")) {  // used by the installer before upgrading/uninstalling
        if (HWND w = FindWindowW(L"WirelessBatteryWnd", nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            HANDLE running = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
            if (!running) return 1;
            PostMessageW(w, WM_CLOSE, 0, 0);
            DWORD exited = WaitForSingleObject(running, 30000);
            CloseHandle(running);
            if (exited != WAIT_OBJECT_0) return 1;
        }
        return 0;
    }
    g_inst = inst;
    g_demo = cmdLine && wcsstr(cmdLine, L"--demo");
    icons::Init();  // register the embedded Material Icons font before drawing any icon

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\WirelessBatteryTray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    updates::Initialize();

    g_lightTaskbar = ReadLightTaskbar();
    ApplyMenuTheme(g_lightTaskbar);
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"WirelessBatteryWnd";
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so it receives the TaskbarCreated broadcast.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;

    DEV_BROADCAST_DEVICEINTERFACE_W filter{ sizeof(filter) };
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = GUID_DEVINTERFACE_HID;
    HDEVNOTIFY notify = RegisterDeviceNotificationW(g_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    // Bluetooth radios coming and going, and the connections on each one.
    filter.dbcc_classguid = GUID_BTHPORT_DEVICE_INTERFACE;
    HDEVNOTIFY radioNotify = RegisterDeviceNotificationW(g_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    RefreshBluetoothRadioWatches();
    HPOWERNOTIFY displayNotify =
        RegisterPowerSettingNotification(g_hwnd, &kConsoleDisplayStateGuid, DEVICE_NOTIFY_WINDOW_HANDLE);

    g_refreshEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stopEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wantWidget = LoadWidgetMode();
    g_hideLastKnown = LoadHideLastKnown();
    g_keepMouseLastKnown = LoadBoolSetting(L"KeepMouseLastKnown", false);
    g_trayWhenEmpty = LoadTrayWhenEmpty();
    g_showDisconnectedDevices = LoadShowDisconnectedDevices();
    g_dimSleeping = LoadDimSleeping();
    g_recordHistory = LoadRecordHistory();
    g_chargeLimit = LoadChargeLimit();
    g_widgetBackground = LoadWidgetBackground();
    g_brandColors = LoadBrandColors();
    g_stacked = LoadStacked();
    g_makeRoom = LoadMakeRoom();
    widget::SetMakeRoom(g_makeRoom);
    g_farLeft = LoadFarLeft();
    g_pollIntervalMs = LoadPollSeconds() * 1000;
    widget::SetStacked(g_stacked);
    g_showPercentSign = LoadShowPercentSign();
    widget::SetShowPercentSign(g_showPercentSign);
    g_textStyle = LoadTextStyle();
    widget::SetTextStyle(g_textStyle);
    g_ringStyle = LoadRingStyle();
    widget::SetRingStyle(g_ringStyle);
    g_connectionIcons = LoadConnectionIcons();
    widget::SetConnectionIcons(g_connectionIcons);
    g_deviceDivider = LoadDeviceDivider();
    widget::SetDeviceDivider(g_deviceDivider);
    g_verticalPlace = LoadVerticalPlace();
    widget::SetVerticalPlace(g_verticalPlace);
    widget::SetFarLeft(g_farLeft);
    g_beforeApps = LoadBeforeApps() && !g_farLeft;
    widget::SetBeforeApps(g_beforeApps);
    widget::SetBackground(g_widgetBackground);  // remembered by the widget across re-creation
    i18n::Init();
    g_widgetMonitor = LoadWidgetMonitor();
    widget::SetMonitorChoice(g_widgetMonitor);
    LoadHidden();
    LoadOrder();
    LoadAliases();
    RestoreKnownDevices();
    LoadAlertThresholds();
    battery_history::Initialize(g_demo);
    Reconcile();
    UpdatePlaceholder();  // something to click on until the first poll finishes
    SetTimer(g_hwnd, TIMER_WIDGET, 1000, nullptr);
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (report::PreTranslate(&m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    // drain results posted after shutdown started
    while (PeekMessageW(&m, nullptr, WM_RESULTS, WM_RESULTS, PM_REMOVE)) delete reinterpret_cast<std::vector<DeviceStatus>*>(m.lParam);
    if (notify) UnregisterDeviceNotification(notify);
    if (radioNotify) UnregisterDeviceNotification(radioNotify);
    ClearBluetoothRadioWatches();
    if (displayNotify) UnregisterPowerSettingNotification(displayNotify);
    if (g_worker) CloseHandle(g_worker);
    ReleaseMutex(mutex);
    return 0;
}
