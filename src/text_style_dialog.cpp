#include "text_style_dialog.h"
#include "dialog_shell.h"
#include "i18n.h"

#include <cmath>
#include <cwchar>
#include <string>

// A plain Win32 dialog window: system caption, system message font, stock edit/combobox/button
// controls and the standard window background. Nothing is owner-drawn, so it follows the user's
// theme, DPI and font settings exactly like the dialogs that ship with Windows.
namespace {
constexpr wchar_t kWindowClass[] = L"WirelessBatteryTextStyleDialog";

// Control ids. The two buttons use IDOK/IDCANCEL so IsDialogMessage maps Enter and Esc onto them.
enum : int {
    kIdFont = 100, kIdSize, kIdWeight, kIdIconSize, kIdSpacing, kIdGap,
    kIdError = 200,
};

// Layout in pixels at 96 dpi; every value goes through S() so the dialog scales with the monitor.
constexpr int kClientW = 468, kClientH = 268;
constexpr int kPad = dlg::kPad, kCol2X = 246, kFieldW = 210;

struct StyleState {
    widget::TextStyle initial;
    widget::TextStyle result;
    HWND font = nullptr, size = nullptr, weight = nullptr, iconSize = nullptr, spacing = nullptr, gap = nullptr;
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

bool Accept(HWND hwnd) {
    StyleState* s = State(hwnd);
    SetWindowTextW(s->error, L"");
    wchar_t face[256] = {};
    GetWindowTextW(s->font, face, ARRAYSIZE(face));
    const int weightSel = (int)SendMessageW(s->weight, CB_GETCURSEL, 0, 0);
    int size = 0, weight = 0, iconSize = 0, spacing = 0, gap = 0;
    weight = weightSel >= 0 ? (int)SendMessageW(s->weight, CB_GETITEMDATA, weightSel, 0) : 400;
    if (!CheckNumber(hwnd, s->size, kMinTextSize, kMaxTextSize, size) ||
        !CheckNumber(hwnd, s->iconSize, kMinIconSize, kMaxIconSize, iconSize) ||
        !CheckNumber(hwnd, s->spacing, kMinIconGap, kMaxIconGap, spacing) ||
        !CheckNumber(hwnd, s->gap, kMinItemGap, kMaxItemGap, gap))
        return false;
    widget::TextStyle chosen;
    chosen.face = face;
    chosen.size = size;
    chosen.weight = weight;
    chosen.iconSize = iconSize;
    chosen.iconGap = spacing;
    chosen.itemGap = gap;
    s->result = chosen;
    s->accepted = true;
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
    s.spacing = EditBox(hwnd, std::to_wstring(s.initial.iconGap).c_str(), dpi, kCol2X, 126, kFieldW, kIdSpacing, false);

    Label(hwnd, T(L"Device spacing (px)"), dpi, kPad, 158, kFieldW);
    s.gap = EditBox(hwnd, std::to_wstring(s.initial.itemGap).c_str(), dpi, kPad, 176, kFieldW, kIdGap, true);

    s.error = Label(hwnd, L"", dpi, kPad, 206, kClientW - kPad * 2);

    const int btnW = dlg::kButtonW, btnH = dlg::kButtonH, btnY = 228;
    CreateWindowExW(0, L"BUTTON", T(L"Cancel"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    S(dpi, kClientW - kPad - btnW * 2 - dlg::kButtonGap), S(dpi, btnY), S(dpi, btnW), S(dpi, btnH),
                    hwnd, (HMENU)(INT_PTR)IDCANCEL, inst, nullptr);
    CreateWindowExW(0, L"BUTTON", T(L"Save"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                    S(dpi, kClientW - kPad - btnW), S(dpi, btnY), S(dpi, btnW), S(dpi, btnH),
                    hwnd, (HMENU)(INT_PTR)IDOK, inst, nullptr);

    s.uiFont = dlg::SystemFont(dpi);
    if (s.uiFont) EnumChildWindows(hwnd, SetFontProc, (LPARAM)s.uiFont);
    SendMessageW(s.font, CB_SETCURSEL,
                 SendMessageW(s.font, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)s.initial.face.c_str()), 0);
}

LRESULT CALLBACK StyleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
        return TRUE;
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
                if (Accept(hwnd)) DestroyWindow(hwnd);
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

bool PromptTextStyle(HWND owner, const widget::TextStyle& current, widget::TextStyle& result) {
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
    shell.title = T(L"Text style");
    shell.clientW = kClientW;
    shell.clientH = kClientH;
    shell.state = &state;
    shell.focusId = kIdFont;
    if (!dlg::Run(owner, shell)) return false;
    if (state.accepted) result = state.result;
    return state.accepted;
}
