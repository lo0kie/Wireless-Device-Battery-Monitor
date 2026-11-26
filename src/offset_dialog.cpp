#include "offset_dialog.h"
#include "dialog_shell.h"
#include "i18n.h"

#include <cwchar>
#include <string>

// One label, one edit box, Save and Cancel — on the shared dialog shell, so it matches the text
// style dialog and the rest of the system.
namespace {
constexpr wchar_t kWindowClass[] = L"WirelessBatteryOffsetDialog";

constexpr int kClientW = 468, kClientH = 126;
constexpr int kFieldW = kClientW - dlg::kPad * 2;

enum : int { kIdEdit = 100, kIdError = 200 };

struct OffsetState {
    int initial = 0;
    int result = 0;
    HWND edit = nullptr;
    HWND error = nullptr;
    HFONT uiFont = nullptr;
    bool accepted = false;
};

OffsetState* State(HWND hwnd) {
    return reinterpret_cast<OffsetState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// Whole number in range, optional leading minus; anything else is rejected.
bool ParseOffset(const wchar_t* text, int& out) {
    while (*text == L' ') text++;
    if (!*text) return false;
    wchar_t* end = nullptr;
    long v = wcstol(text, &end, 10);
    while (end && *end == L' ') end++;
    if (!end || *end || v < kMinTaskbarOffset || v > kMaxTaskbarOffset) return false;
    out = (int)v;
    return true;
}

LRESULT CALLBACK OffsetProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
        // Let DefWindowProc finish non-client setup (including the window caption); returning TRUE
        // here directly leaves the title bar text empty even though CreateWindowExW was given one.
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    OffsetState* state = State(hwnd);
    if (!state) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_CREATE: {
            HINSTANCE inst = GetModuleHandleW(nullptr);
            const int dpi = (int)GetDpiForWindow(hwnd);
            auto s = [dpi](int v) { return MulDiv(v, dpi, 96); };
            HWND label = CreateWindowExW(0, L"STATIC",
                                         T(L"Horizontal offset in pixels (0 = left end):"),
                                         WS_CHILD | WS_VISIBLE, s(dlg::kPad), s(10), s(kFieldW), s(dlg::kLabelH),
                                         hwnd, nullptr, inst, nullptr);
            state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(state->initial).c_str(),
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                          s(dlg::kPad), s(30), s(kFieldW), s(dlg::kFieldH),
                                          hwnd, (HMENU)(INT_PTR)kIdEdit, inst, nullptr);
            state->error = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                           s(dlg::kPad), s(60), s(kFieldW), s(dlg::kLabelH),
                                           hwnd, (HMENU)(INT_PTR)kIdError, inst, nullptr);
            // The button row comes from dlg::Run, so every dialog's buttons sit in the same place.
            state->uiFont = dlg::SystemFont(dpi);
            if (state->uiFont) {
                for (HWND c : { label, state->edit, state->error })
                    SendMessageW(c, WM_SETFONT, (WPARAM)state->uiFont, TRUE);
            }
            SendMessageW(state->edit, EM_SETLIMITTEXT, 6, 0);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK) {
                wchar_t buffer[16] = {};
                GetWindowTextW(state->edit, buffer, ARRAYSIZE(buffer));
                int value = 0;
                if (!ParseOffset(buffer, value)) {
                    std::wstring text = TF(L"Enter a whole number from {min} to {max}.",
                                           { { L"min", std::to_wstring(kMinTaskbarOffset) },
                                             { L"max", std::to_wstring(kMaxTaskbarOffset) } });
                    SetWindowTextW(state->error, text.c_str());
                    SetFocus(state->edit);
                    SendMessageW(state->edit, EM_SETSEL, 0, -1);
                    return 0;
                }
                state->result = value;
                state->accepted = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wp) == IDCANCEL) { DestroyWindow(hwnd); return 0; }
            break;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_NCDESTROY:
            if (state->uiFont) DeleteObject(state->uiFont);
            state->uiFont = nullptr;
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

bool PromptTaskbarOffset(HWND owner, const wchar_t* title, int current, int& result,
                         const std::function<void(int)>& onApply) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = inst;
    wc.lpfnWndProc = OffsetProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    OffsetState state;
    state.initial = current;
    dlg::Shell shell;
    shell.className = kWindowClass;
    shell.title = T(title);
    shell.clientW = kClientW;
    shell.clientH = kClientH;
    shell.state = &state;
    shell.focusId = kIdEdit;
    shell.onApply = [&state, &onApply](HWND hwnd) {
        wchar_t buffer[16] = {};
        GetWindowTextW(state.edit, buffer, ARRAYSIZE(buffer));
        int value = 0;
        if (!ParseOffset(buffer, value)) {
            std::wstring text = TF(L"Enter a whole number from {min} to {max}.",
                                   { { L"min", std::to_wstring(kMinTaskbarOffset) },
                                     { L"max", std::to_wstring(kMaxTaskbarOffset) } });
            SetWindowTextW(state.error, text.c_str());
            SetFocus(state.edit);
            SendMessageW(state.edit, EM_SETSEL, 0, -1);
            return false;
        }
        state.result = value;
        state.accepted = true;
        if (onApply) onApply(value);
        return true;
    };
    if (!dlg::Run(owner, shell)) return false;
    if (state.accepted) result = state.result;
    return state.accepted;
}
