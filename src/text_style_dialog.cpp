#include "text_style_dialog.h"
#include "dialog_shell.h"
#include "i18n.h"

#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>

// A plain Win32 dialog window: system caption, system message font, stock edit/combobox/button
// controls and the standard window background. Nothing is owner-drawn, so it follows the user's
// theme, DPI and font settings exactly like the dialogs that ship with Windows.
namespace {
constexpr wchar_t kWindowClass[] = L"WirelessBatteryTextStyleDialog";

// Control ids. The two buttons use IDOK/IDCANCEL so IsDialogMessage maps Enter and Esc onto them.
enum : int {
    kIdFont = 100, kIdSize, kIdWeight, kIdIconSize, kIdSpacing, kIdGap, kIdLetterSpacing,
    kIdDividerGap,
    kIdError = 200,
};

// Layout in pixels at 96 dpi; every value goes through S() so the dialog scales with the monitor.
constexpr int kClientW = 468, kClientH = 282;
constexpr int kPad = dlg::kPad, kCol2X = 246, kFieldW = 210;

struct StyleState {
    widget::TextStyle initial;
    widget::TextStyle result;
    HWND font = nullptr, size = nullptr, weight = nullptr, iconSize = nullptr, spacing = nullptr, gap = nullptr;
    HWND letterSpacing = nullptr, dividerGap = nullptr;
    HWND error = nullptr;
    HFONT uiFont = nullptr;
    bool accepted = false;
};

StyleState* State(HWND hwnd) {
    return reinterpret_cast<StyleState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

int S(int dpi, int px) { return MulDiv(px, dpi, 96); }

HWND Label(HWND parent, const wchar_t* text, int dpi, int x, int y, int w) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                           S(dpi, x), S(dpi, y), S(dpi, w), S(dpi, dlg::kLabelH), parent, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
}

HWND EditBox(HWND parent, const wchar_t* text, int dpi, int x, int y, int w, int id, bool numeric) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | (numeric ? ES_NUMBER : 0),
                           S(dpi, x), S(dpi, y), S(dpi, w), S(dpi, dlg::kFieldH), parent, (HMENU)(INT_PTR)id,
                           GetModuleHandleW(nullptr), nullptr);
}

// EnumFontFamiliesEx reports one entry per face *and script*, so a Chinese face that covers several
// scripts appears many times with the same name. Add each name only once (case-insensitively).
int CALLBACK EnumFontProc(const LOGFONTW* lf, const TEXTMETRICW*, DWORD, LPARAM lp) {
    // Faces starting with '@' are vertical variants of an existing font (CJK); they cannot be used here.
    if (!lf->lfFaceName[0] || lf->lfFaceName[0] == L'@') return TRUE;
    HWND combo = (HWND)lp;
    if (SendMessageW(combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)lf->lfFaceName) == CB_ERR)
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)lf->lfFaceName);
    return TRUE;
}

bool ParseNumber(HWND edit, int min, int max, int& out) {
    wchar_t buffer[32] = {};
    GetWindowTextW(edit, buffer, ARRAYSIZE(buffer));
    const wchar_t* p = buffer;
    while (*p == L' ' || *p == L'\t') p++;
    if (!*p) return false;
    wchar_t* end = nullptr;
    long v = wcstol(p, &end, 10);
    while (end && (*end == L' ' || *end == L'\t')) end++;
    if (!end || *end || v < min || v > max) return false;
    out = (int)v;
    return true;
}

// One decimal at most, and no trailing ".0": the boxes read "5" and "5.5", never "5.000000".
std::wstring FmtNum(float v) {
    wchar_t buffer[32];
    swprintf(buffer, ARRAYSIZE(buffer), L"%.1f", v);
    std::wstring text = buffer;
    if (text.size() > 2 && text.compare(text.size() - 2, 2, L".0") == 0) text.resize(text.size() - 2);
    return text;
}

// Same as ParseNumber, but one decimal point is allowed: "0.5", "-1.5", "12".
bool ParseDecimal(HWND edit, float min, float max, float& out) {
    wchar_t buffer[32] = {};
    GetWindowTextW(edit, buffer, ARRAYSIZE(buffer));
    const wchar_t* p = buffer;
    while (*p == L' ' || *p == L'	') p++;
    if (!*p) return false;
    wchar_t* end = nullptr;
    const double v = wcstod(p, &end);
    while (end && (*end == L' ' || *end == L'	')) end++;
    if (!end || *end || v < min || v > max) return false;
    out = (float)v;
    return true;
}

bool CheckDecimal(HWND hwnd, HWND edit, float min, float max, float& out) {
    if (ParseDecimal(edit, min, max, out)) return true;
    StyleState* s = State(hwnd);
    std::wstring msg = TF(L"Enter a number from {min} to {max}.", { { L"min", FmtNum(min) }, { L"max", FmtNum(max) } });
    SetWindowTextW(s->error, msg.c_str());
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, 0, -1);
    return false;
}

bool CheckNumber(HWND hwnd, HWND edit, int min, int max, int& out) {
    if (ParseNumber(edit, min, max, out)) return true;
    StyleState* s = State(hwnd);
    std::wstring msg = TF(L"Enter a whole number from {min} to {max}.",
                          { { L"min", std::to_wstring(min) }, { L"max", std::to_wstring(max) } });
    SetWindowTextW(s->error, msg.c_str());
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, 0, -1);
    return false;
}

// WNDENUMPROC has a fixed calling convention, which a lambda does not match on 32-bit builds.
BOOL CALLBACK SetFontProc(HWND child, LPARAM font) {
    SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
    return TRUE;
}

// Reads and validates the fields. Returns false with the error line filled in when a value is bad.
bool Collect(HWND hwnd, widget::TextStyle& chosen) {
    StyleState* s = State(hwnd);
    SetWindowTextW(s->error, L"");
    wchar_t face[256] = {};
    GetWindowTextW(s->font, face, ARRAYSIZE(face));
    const int weightSel = (int)SendMessageW(s->weight, CB_GETCURSEL, 0, 0);
    int size = 0, weight = 0, iconSize = 0;
    float spacing = 0, gap = 0, letterSpacing = 0, dividerGap = 0;
    weight = weightSel >= 0 ? (int)SendMessageW(s->weight, CB_GETITEMDATA, weightSel, 0) : 400;
    if (!CheckNumber(hwnd, s->size, kMinTextSize, kMaxTextSize, size) ||
        !CheckNumber(hwnd, s->iconSize, kMinIconSize, kMaxIconSize, iconSize) ||
        !CheckDecimal(hwnd, s->spacing, kMinIconGap, kMaxIconGap, spacing) ||
        !CheckDecimal(hwnd, s->gap, kMinItemGap, kMaxItemGap, gap) ||
        !CheckDecimal(hwnd, s->letterSpacing, kMinLetterSpacing, kMaxLetterSpacing, letterSpacing) ||
        !CheckDecimal(hwnd, s->dividerGap, kMinDividerGap, kMaxDividerGap, dividerGap))
        return false;
    chosen.face = face;
    chosen.size = size;
    chosen.weight = weight;
    chosen.iconSize = iconSize;
    chosen.iconGap = spacing;
    chosen.itemGap = gap;
    chosen.letterSpacing = letterSpacing;
    chosen.dividerGap = dividerGap;
    return true;
}

void CreateControls(HWND hwnd, StyleState& s) {
    const int dpi = (int)GetDpiForWindow(hwnd);
    HINSTANCE inst = GetModuleHandleW(nullptr);

    Label(hwnd, T(L"Font (blank for default)"), dpi, kPad, 10, kClientW - kPad * 2);
    s.font = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", s.initial.face.c_str(),
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_SORT | CBS_AUTOHSCROLL,
                             S(dpi, kPad), S(dpi, 28), S(dpi, kClientW - kPad * 2), S(dpi, 200),
                             hwnd, (HMENU)(INT_PTR)kIdFont, inst, nullptr);
    SendMessageW(s.font, CB_LIMITTEXT, 128, 0);
    HDC dc = GetDC(nullptr);
    if (dc) {
        LOGFONTW lf{};
        lf.lfCharSet = DEFAULT_CHARSET;
        EnumFontFamiliesExW(dc, &lf, EnumFontProc, (LPARAM)s.font, 0);
        ReleaseDC(nullptr, dc);
    }
    if (!s.initial.face.empty() &&
        SendMessageW(s.font, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)s.initial.face.c_str()) == CB_ERR)
        SendMessageW(s.font, CB_ADDSTRING, 0, (LPARAM)s.initial.face.c_str());

    Label(hwnd, T(L"Text size (px)"), dpi, kPad, 58, kFieldW);
    Label(hwnd, T(L"Font weight (100-900)"), dpi, kCol2X, 58, kFieldW);
    s.size = EditBox(hwnd, std::to_wstring(s.initial.size).c_str(), dpi, kPad, 76, kFieldW, kIdSize, true);
    // A fixed list: only these values map onto real font weights, and typing "650" only ever fell
    // back to something else. Labels are the numbers themselves, so nothing needs translating.
    s.weight = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                               S(dpi, kCol2X), S(dpi, 76), S(dpi, kFieldW), S(dpi, 200),
                               hwnd, (HMENU)(INT_PTR)kIdWeight, inst, nullptr);
    int nearest = 0, bestDiff = 0x7FFFFFFF;
    for (int i = 0; i < 9; i++) {
        const int w = (i + 1) * 100;
        const int item = (int)SendMessageW(s.weight, CB_ADDSTRING, 0, (LPARAM)std::to_wstring(w).c_str());
        if (item >= 0) SendMessageW(s.weight, CB_SETITEMDATA, item, (LPARAM)w);
        const int diff = std::abs(w - s.initial.weight);
        if (diff < bestDiff) {
            bestDiff = diff;
            nearest = i;
        }
    }
    SendMessageW(s.weight, CB_SETCURSEL, nearest, 0);

    Label(hwnd, T(L"Icon size (px)"), dpi, kPad, 108, kFieldW);
    Label(hwnd, T(L"Text-icon spacing (px)"), dpi, kCol2X, 108, kFieldW);
    s.iconSize = EditBox(hwnd, std::to_wstring(s.initial.iconSize).c_str(), dpi, kPad, 126, kFieldW, kIdIconSize, true);
    s.spacing = EditBox(hwnd, FmtNum(s.initial.iconGap).c_str(), dpi, kCol2X, 126, kFieldW, kIdSpacing, false);

    Label(hwnd, T(L"Device spacing (px)"), dpi, kPad, 158, kFieldW);
    Label(hwnd, T(L"Letter spacing (px)"), dpi, kCol2X, 158, kFieldW);
    s.gap = EditBox(hwnd, FmtNum(s.initial.itemGap).c_str(), dpi, kPad, 176, kFieldW, kIdGap, false);
    // Decimals are allowed in the gaps, and the letter spacing can be negative, so none of these
    // three can use ES_NUMBER; the range check covers what may be typed.
    s.letterSpacing = EditBox(hwnd, FmtNum(s.initial.letterSpacing).c_str(), dpi, kCol2X, 176, kFieldW,
                              kIdLetterSpacing, false);

    Label(hwnd, T(L"Divider gap (px)"), dpi, kPad, 208, kFieldW);
    s.dividerGap = EditBox(hwnd, FmtNum(s.initial.dividerGap).c_str(), dpi, kPad, 226, kFieldW,
                           kIdDividerGap, false);

    s.error = Label(hwnd, L"", dpi, kPad, 256, kClientW - kPad * 2);

    s.uiFont = dlg::SystemFont(dpi);
    if (s.uiFont) EnumChildWindows(hwnd, SetFontProc, (LPARAM)s.uiFont);
    SendMessageW(s.font, CB_SETCURSEL,
                 SendMessageW(s.font, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)s.initial.face.c_str()), 0);
}

LRESULT CALLBACK StyleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
        // Let DefWindowProc finish non-client setup (including the window caption); returning TRUE
        // here directly leaves the title bar text empty even though CreateWindowExW was given one.
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    StyleState* state = State(hwnd);
    if (!state) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_CREATE:
            CreateControls(hwnd, *state);
            return 0;
        case WM_COMMAND: {
            WORD id = LOWORD(wp), notice = HIWORD(wp);
            if (id == IDOK && (notice == BN_CLICKED || notice == 0)) {
                widget::TextStyle chosen;
                if (Collect(hwnd, chosen)) {
                    state->result = chosen;
                    state->accepted = true;
                    DestroyWindow(hwnd);
                }
                return 0;
            }
            if (id == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_KEYDOWN: {
            // Enter accepts from anywhere but an open combo list, Esc always cancels.
            if (wp == VK_RETURN) {
                if (GetFocus() == state->font) {
                    if (SendMessageW(state->font, CB_GETDROPPEDSTATE, 0, 0)) break;  // let the list take it
                }
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            // Children are gone by now, so no control still references the font.
            if (state->uiFont) DeleteObject(state->uiFont);
            state->uiFont = nullptr;
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

bool PromptTextStyle(HWND owner, const wchar_t* title, const widget::TextStyle& current, widget::TextStyle& result,
                     const std::function<void(const widget::TextStyle&)>& onApply) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = inst;
    wc.lpfnWndProc = StyleProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);  // standard dialog background
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    StyleState state;
    state.initial = current;
    dlg::Shell shell;
    shell.className = kWindowClass;
    shell.title = T(title);
    shell.clientW = kClientW;
    shell.clientH = kClientH;
    shell.state = &state;
    shell.focusId = kIdFont;
    shell.onApply = [&state, &onApply](HWND hwnd) {
        widget::TextStyle chosen;
        if (!Collect(hwnd, chosen)) return false;  // the error line already says what is wrong
        state.result = chosen;
        state.accepted = true;  // Save after Apply keeps the same values
        if (onApply) onApply(chosen);
        return true;
    };
    if (!dlg::Run(owner, shell)) return false;
    if (state.accepted) result = state.result;
    return state.accepted;
}
