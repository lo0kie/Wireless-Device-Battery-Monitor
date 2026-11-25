#pragma once
#include <windows.h>

// The frame shared by the small modal dialogs: a standard caption, the system message font, the
// standard window background and one set of metrics. Keeping it in one place is what makes the
// input boxes look like Windows dialogs instead of themed one-offs, and keeps them identical to
// each other.
namespace dlg {

constexpr int kPad = 12;       // margin around the content
constexpr int kFieldH = 24;    // height of an edit or combo box
constexpr int kLabelH = 16;    // height of one line of label text
constexpr int kButtonW = 88;
constexpr int kButtonH = 26;
constexpr int kButtonGap = 8;

// The shell font at `dpi`, i.e. the same face and size Explorer uses.
HFONT SystemFont(int dpi);

struct Shell {
    const wchar_t* className = nullptr;  // must already be registered by the caller
    const wchar_t* title = nullptr;
    int clientW = 0, clientH = 0;        // client size in pixels at 96 dpi
    void* state = nullptr;               // passed through GWLP_USERDATA
    int focusId = 0;                     // control focused (and selected) once shown
};

// Creates the window at the monitor the pointer is on, disables the owner, pumps messages until the
// window closes, then restores the owner. Returns false only if the window could not be created;
// whether the user accepted is up to the caller's state.
bool Run(HWND owner, const Shell& shell);

}  // namespace dlg
