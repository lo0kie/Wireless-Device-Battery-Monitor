#include "rename_dialog.h"
#include "dialog_shell.h"
#include "device_aliases.h"
#include "i18n.h"
#include "memory_trim.h"

#include <string>
#include <vector>

namespace {
constexpr wchar_t kWindowClass[] = L"WirelessBatteryRenameDialog";

enum : int {
    kIdName = 100,
    kIdError = 200,
};

// Layout in pixels at 96 dpi; the button row is placed by dlg::Run at the bottom of this client area.
constexpr int kClientW = 420, kClientH = 126;
constexpr int kPad = dlg::kPad;

struct EditorState {
    std::wstring initial;
    std::wstring result;
    HWND edit = nullptr;
    HWND error = nullptr;
    HFONT uiFont = nullptr;
    bool accepted = false;
};

EditorState* State(HWND hwnd) {
    return reinterpret_cast<EditorState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

int S(int dpi, int px) { return MulDiv(px, dpi, 96); }

HWND Label(HWND parent, const wchar_t* text, int dpi, int x, int y) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                           S(dpi, x), S(dpi, y), S(dpi, kClientW - kPad * 2), S(dpi, dlg::kLabelH),
                           parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}

// WNDENUMPROC has a fixed calling convention, which a lambda does not match on 32-bit builds.
BOOL CALLBACK SetFontProc(HWND child, LPARAM font) {
    SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
    return TRUE;
}

// Reads and validates the name. Returns false with the error line filled in when it is empty or bad.
bool Collect(HWND hwnd, std::wstring& name) {
    EditorState* s = State(hwnd);
    SetWindowTextW(s->error, L"");
    const int length = GetWindowTextLengthW(s->edit);
    std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, 0);
    GetWindowTextW(s->edit, buffer.data(), (int)buffer.size());
    std::wstring chosen = aliases::Trim(buffer.data());
    if (!aliases::ValidName(chosen)) {
        SetWindowTextW(s->error, T(L"Enter a name (1-64 characters)."));
        SetFocus(s->edit);
        SendMessageW(s->edit, EM_SETSEL, 0, -1);
        return false;
    }
    name = std::move(chosen);
    return true;
}

void CreateControls(HWND hwnd, EditorState& s) {
    const int dpi = (int)GetDpiForWindow(hwnd);
    HINSTANCE inst = GetModuleHandleW(nullptr);

    Label(hwnd, T(L"Device display name:"), dpi, kPad, 10);
    s.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", s.initial.c_str(),
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                             S(dpi, kPad), S(dpi, 30), S(dpi, kClientW - kPad * 2), S(dpi, dlg::kFieldH),
                             hwnd, (HMENU)(INT_PTR)kIdName, inst, nullptr);
    SendMessageW(s.edit, EM_SETLIMITTEXT, aliases::kMaxNameLength, 0);
    s.error = Label(hwnd, L"", dpi, kPad, 60);

    s.uiFont = dlg::SystemFont(dpi);
    if (s.uiFont) EnumChildWindows(hwnd, SetFontProc, (LPARAM)s.uiFont);
}

LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
        return TRUE;
    }
    EditorState* state = State(hwnd);
    if (!state) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_CREATE:
            CreateControls(hwnd, *state);
            return 0;
        case WM_COMMAND: {
            const WORD id = LOWORD(wp), notice = HIWORD(wp);
            if (id == IDOK && (notice == BN_CLICKED || notice == 0)) {
                std::wstring name;
                if (Collect(hwnd, name)) {
                    state->result = std::move(name);
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
        case WM_KEYDOWN:
            // Enter accepts from the edit box, Esc always cancels.
            if (wp == VK_RETURN) {
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
                return 0;
            }
            break;
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

bool PromptDeviceName(HWND owner, const std::wstring& currentName, std::wstring& result,
                      const std::function<void(const std::wstring&)>& onApply) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = inst;
    wc.lpfnWndProc = EditorProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    EditorState state;
    state.initial = currentName;
    dlg::Shell shell;
    shell.className = kWindowClass;
    shell.title = T(L"Rename device");
    shell.clientW = kClientW;
    shell.clientH = kClientH;
    shell.state = &state;
    shell.focusId = kIdName;
    shell.onApply = [&state, &onApply](HWND hwnd) {
        std::wstring name;
        if (!Collect(hwnd, name)) return false;  // the error line already says what is wrong
        state.result = std::move(name);
        state.accepted = true;  // Save after Apply keeps the same name
        if (onApply) onApply(state.result);
        return true;
    };
    if (!dlg::Run(owner, shell)) return false;
    if (state.accepted) result = state.result;
    memory_trim::Soon();
    return state.accepted;
}
