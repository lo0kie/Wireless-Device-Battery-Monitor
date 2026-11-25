#pragma once
#include "widget.h"
#include <windows.h>

// Ranges shared by the dialog and the settings loaded from the registry.
constexpr int kMinTextSize = 9, kMaxTextSize = 24;
constexpr int kMinTextWeight = 100, kMaxTextWeight = 900;
constexpr int kMinIconSize = 10, kMaxIconSize = 28;
constexpr int kMinIconGap = 0, kMaxIconGap = 24;
constexpr int kMinItemGap = 2, kMaxItemGap = 60;

// Native, modal Win32 editor for the taskbar strip's font, size and spacing. Returns false if cancelled.
bool PromptTextStyle(HWND owner, const widget::TextStyle& current, widget::TextStyle& result);
