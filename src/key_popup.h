#pragma once
#include <windows.h>
#include <functional>
#include <string>
namespace manager {
HWND keyPopupWindow();
void closeKeyPopup();
void showKeyPopup(HWND owner, HWND anchor, const std::wstring& target, std::function<bool(const std::string&)> submit);
}
