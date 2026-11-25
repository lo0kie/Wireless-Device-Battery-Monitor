#include "icon_font.h"

#include <algorithm>

namespace icons {
namespace {

// Material Icons codepoints (from font/MaterialIconsOutlined-Regular.codepoints, Apache-2.0).
constexpr Glyphs kMaterial = {
    0xE310, 0xE323, 0xE312, 0xE30F, 0xE32C, 0xE029, 0xE32D, 0xE1A7, 0xE1B1,  // devices
    0xEA0B, 0xE5CA, 0xE63E, 0xE1A6, 0xE1A4, 0xE1A5, 0xE1A3, 0xE19C, 0xE5CD, 0xE1E0,  // status / battery / offline / usb
    0xE3C9, 0xE7F4, 0xE889, 0xE333, 0xE80B, 0xE425, 0xE5D5, 0xE887, 0xE87D, 0xE62A, 0xE5CD,  // menu
    0xE5CC,  // chevron right
    { 0xEBDC, 0xEBD9, 0xEBE0, 0xEBDD, 0xEBE2, 0xEBD4, 0xEBD2 }  // battery_0..6_bar
};

// Original Segoe Fluent Icons / MDL2 Assets codes, kept as a fallback when the private font fails.
constexpr Glyphs kSegoe = {
    0xE7F6, 0xE962, 0xE765, 0xE7FC, 0xE8EA, 0xE720, 0xE7F5, 0xE702, 0xE772,
    0xE945, 0xE73E, 0xE701, 0xE996, 0xE996, 0xE996, 0xE945, 0xE996, 0xE711, 0xE772,
    0xE8AC, 0xEA8F, 0xE81C, 0xE7F4, 0xE774, 0xE916, 0xE72C, 0xE897, 0xEB51, 0xE777, 0xE711,
    0xE76C,  // Segoe: ChevronDown
    { 0xE996, 0xE996, 0xE996, 0xE996, 0xE996, 0xE996, 0xE996 }  // no Segoe battery bars; use unknown
};

bool g_ready = false;
HANDLE g_fontResource = nullptr;

}  // namespace

bool Init() {
    if (g_fontResource) return g_ready;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(inst, L"MATERIAL_FONT", RT_RCDATA);
    if (!res) return false;
    HGLOBAL data = LoadResource(inst, res);
    void* bytes = data ? LockResource(data) : nullptr;
    DWORD size = SizeofResource(inst, res);
    if (!bytes || !size) return false;
    DWORD count = 0;
    g_fontResource = AddFontMemResourceEx(bytes, size, nullptr, &count);
    g_ready = g_fontResource != nullptr && count > 0;
    return g_ready;
}

bool Ready() { return g_ready; }

HFONT Font(int px, int weight) {
    if (!g_ready) return nullptr;  // caller keeps its own Segoe font
    return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Material Icons Outlined");
}

const Glyphs& G() { return g_ready ? kMaterial : kSegoe; }

wchar_t Device(DeviceKind kind, bool bluetooth) {
    const Glyphs& g = G();
    switch (kind) {
        case DeviceKind::Headset:    return g.headset;
        case DeviceKind::Mouse:      return g.mouse;
        case DeviceKind::Keyboard:   return g.keyboard;
        case DeviceKind::Gamepad:    return g.gamepad;
        case DeviceKind::Phone:      return g.phone;
        case DeviceKind::Microphone: return g.microphone;
        case DeviceKind::Speaker:    return g.speaker;
        default:                             return bluetooth ? g.bluetooth : g.devices;
    }
}

}  // namespace icons
