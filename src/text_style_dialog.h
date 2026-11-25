#pragma once
#include "widget.h"
#include <windows.h>

#include <functional>

// Ranges shared by the dialog and the settings loaded from the registry.
constexpr int kMinTextSize = 9, kMaxTextSize = 24;
constexpr int kMinTextWeight = 100, kMaxTextWeight = 900;
constexpr int kMinIconSize = 10, kMaxIconSize = 28;
// Gaps are fractional (one decimal is enough), so they are floats rather than ints.
constexpr float kMinIconGap = 0.0f, kMaxIconGap = 24.0f;
constexpr float kMinItemGap = 2.0f, kMaxItemGap = 60.0f;
constexpr float kMinLetterSpacing = -2.0f, kMaxLetterSpacing = 12.0f;

// Native, modal Win32 editor for the taskbar strip's font, size and spacing. Returns false if cancelled.
// `onApply`, when set, is called by the dialog's Apply button with the values currently entered, so
// the change takes effect without closing the window. Returning from the dialog with Save still
// reports the same values through `result`.
bool PromptTextStyle(HWND owner, const widget::TextStyle& current, widget::TextStyle& result,
                     const std::function<void(const widget::TextStyle&)>& onApply = {});
