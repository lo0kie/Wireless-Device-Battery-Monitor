#pragma once
// Windows 11 style popup menu: a self-drawn layered window instead of the classic Win32 menu —
// rounded corners, a translucent panel, 32 px rows, a rounded hover pill and a check column.
// The menu keeps its own modal loop, so it behaves like TrackPopupMenu: it returns the chosen
// command id, or 0 when dismissed.
//
// If the window cannot be created (no layered-window support), Show() falls back to a native
// TrackPopupMenu built from the same items, so the menu never disappears.
#include <windows.h>
#include <string>
#include <vector>

namespace popup_menu {

struct Item {
    enum class Kind { Normal, Header, Separator };
    Kind kind = Kind::Normal;
    std::wstring text;
    std::wstring rightText;    // secondary value shown at the far right (e.g. a battery level)
    UINT_PTR id = 0;           // command returned by Show() when this item is chosen
    bool checked = false;      // check mark at the right
    bool radio = false;        // dot at the right (one of a group)
    bool disabled = false;     // greyed out: no hover, no click
    std::vector<Item> children;  // non-empty: this item opens a submenu
};

struct Options {
    bool light = true;        // panel and text follow the taskbar theme
    bool rtl = false;         // lay the menu out from the right (Hebrew, Arabic)
    bool hasExclude = false;  // keep the menu out of `exclude` (the taskbar it was opened from)
    RECT exclude{};
    int maxHeight = 400;      // cap the panel at this many 96-dpi pixels; taller menus scroll.
                              // 0 disables the cap (only the screen work area limits the menu).
};

// Shows the menu at a screen point. Returns false only when nothing could be shown at all;
// `chosen` is the id of the clicked item, or 0 when the menu was dismissed.
bool Show(HWND owner, POINT at, const std::vector<Item>& items, const Options& opt, UINT_PTR& chosen);

}  // namespace popup_menu
