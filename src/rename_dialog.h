#pragma once
#include <windows.h>
#include <functional>
#include <string>

// Native, modal Win32 name editor on the shared dialog shell. Returns false if the user cancels.
// `onApply`, when set, is called by the Apply button with the new name so it takes effect without
// closing the window. Save still reports the same name through `result`.
bool PromptDeviceName(HWND owner, const std::wstring& currentName, std::wstring& result,
                      const std::function<void(const std::wstring&)>& onApply = {});
