#pragma once
#include "devices.h"
#include <windows.h>

// Material Icons (Apache-2.0), embedded as a private font so the taskbar, tray and menu share one
// glyph set without depending on any font installed on the machine. If the embedded font cannot be
// registered, the original Segoe Fluent Icons / MDL2 glyphs are used instead.
namespace icons {

// Registers the embedded font. Call once at startup, before any icon is drawn.
bool Init();
bool Ready();

// Creates a font sized in pixels for the active icon set. `weight` is ignored by Material Icons.
HFONT Font(int px, int weight = FW_NORMAL);

// Glyph table, switched by Init(): Material Icons when available, otherwise the original Segoe codes.
struct Glyphs {
    wchar_t headset, mouse, keyboard, gamepad, phone, microphone, speaker, bluetooth, devices;
    wchar_t bolt, check, wifi, batteryUnknown, batteryFull, battery, batteryCharging, batteryAlert, offline, usb;
    wchar_t rename, alerts, history, monitor, globe, stopwatch, refresh, help, support, update, exit;
    wchar_t chevronRight;  // menu submenu arrow, from the same icon set
    wchar_t batteryBars[7];              // battery_0_bar .. battery_6_bar (charge level)
};
const Glyphs& G();

// Device kind -> glyph, used by both the taskbar and the menu.
wchar_t Device(DeviceKind kind, bool bluetooth);

}  // namespace icons
