#include "dialog_shell.h"

#include <algorithm>

namespace dlg {

HFONT SystemFont(int dpi) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    // SPI_GETNONCLIENTMETRICS already returns a height scaled for the system DPI; asking the
    // ForDpi variant for `dpi` directly avoids scaling the same font twice.
    using SpiForDpiFn = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    auto spiForDpi = (SpiForDpiFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi");
    if (spiForDpi) {
        if (!spiForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, (UINT)dpi)) return nullptr;
    } else {
        if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) return nullptr;
        ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, dpi, (int)GetDpiForSystem());
    }
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

int TextLine(int dpi) { return MulDiv(kLabelH, dpi, 96); }

bool Run(HWND owner, const Shell& shell) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    const int dpi = owner ? (int)GetDpiForWindow(owner) : 96;
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    const DWORD exStyle = WS_EX_DLGMODALFRAME;

    RECT rc{ 0, 0, MulDiv(shell.clientW, dpi, 96), MulDiv(shell.clientH, dpi, 96) };
    using AdjustFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    auto adjust = (AdjustFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    if (adjust) adjust(&rc, style, FALSE, exStyle, (UINT)dpi);
    else AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    const int width = rc.right - rc.left, height = rc.bottom - rc.top;

    POINT p{};
    GetCursorPos(&p);
    MONITORINFO mi{ sizeof(mi) };
    RECT area{};
    if (GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi)) area = mi.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    int x = std::max(area.left + ((area.right - area.left - width) / 2), area.left);
    int y = std::max(area.top + ((area.bottom - area.top - height) / 2), area.top);

    HWND dialog = CreateWindowExW(exStyle, shell.className, shell.title, style, x, y, width, height, owner,
                                  nullptr, inst, shell.state);
    if (!dialog) return false;

    const bool ownerEnabled = owner && IsWindowEnabled(owner);
    if (ownerEnabled) EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    SetForegroundWindow(dialog);
    if (HWND focus = shell.focusId ? GetDlgItem(dialog, shell.focusId) : nullptr) {
        SetFocus(focus);
        SendMessageW(focus, EM_SETSEL, 0, -1);  // start with the current value selected, like Windows
    }

    MSG msg{};
    while (IsWindow(dialog)) {
        BOOL code = GetMessageW(&msg, nullptr, 0, 0);
        if (code <= 0) {
            if (code == 0) PostQuitMessage((int)msg.wParam);
            break;
        }
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (IsWindow(dialog)) DestroyWindow(dialog);
    if (ownerEnabled) EnableWindow(owner, TRUE);
    return true;
}

}  // namespace dlg
