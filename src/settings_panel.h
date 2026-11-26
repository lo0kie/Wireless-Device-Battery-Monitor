// The settings window: an HTML page rendered by WebView2 in a borderless native window.
//
// The panel is decoupled from main.cpp's file-local state: main fills a SettingsData snapshot and
// supplies callbacks that apply each change. This keeps main's globals in their anonymous namespace
// and lets the panel simply reflect + report values.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "widget.h"

struct SettingsData {
    // General
    bool recordHistory = true;
    int chargeLimit = 0;                 // 0 = off
    std::vector<int> chargeLimits;       // selectable percentages
    int pollSeconds = 60;
    std::vector<int> pollSecondsList;    // selectable intervals

    // Display style
    bool wantWidget = true;
    bool trayWhenEmpty = false;
    bool stacked = false, widgetBackground = false, ringStyle = false, showPercentSign = true;
    bool brandColors = false, connectionIcons = false, deviceDivider = false, showTooltip = true;

    // Position
    bool beforeApps = false;
    bool makeRoom = true;
    int taskbarOffset = 0;
    std::vector<int> offsetList;         // preset offsets (the custom value is prepended if absent)
    int verticalPlace = 0;               // widget::VerticalPlace
    int monitorChoice = 0;               // 0 all, 1 main, 2 custom

    // Devices
    bool hideLastKnown = false;
    bool showInactive = false, dimInactive = true;

    // Text style (the taskbar strip's font, size and spacing), edited on its own page.
    widget::TextStyle textStyle;
};

struct SettingsCallbacks {
    std::function<void(bool)> recordHistory;
    std::function<void(int)> chargeLimit;
    std::function<void(int)> pollSeconds;
    std::function<void(bool)> wantWidget;
    std::function<void(bool)> trayWhenEmpty;
    std::function<void(bool)> stacked;
    std::function<void(bool)> widgetBackground;
    std::function<void(bool)> ringStyle;
    std::function<void(bool)> showPercentSign;
    std::function<void(bool)> brandColors;
    std::function<void(bool)> connectionIcons;
    std::function<void(bool)> deviceDivider;
    std::function<void(bool)> showTooltip;
    std::function<void(bool)> beforeApps;
    std::function<void(bool)> makeRoom;
    std::function<void(int)> taskbarOffset;
    std::function<void(int)> verticalPlace;
    std::function<void(int)> monitorChoice;
    std::function<void(bool)> hideLastKnown;
    std::function<void(bool)> showInactive;
    std::function<void(bool)> dimInactive;
    std::function<void(const widget::TextStyle&)> textStyle;
};

// Opens the settings window modally (runs its own message loop until it closes).
void ShowSettingsPanel(const SettingsData& data, const SettingsCallbacks& cb);
