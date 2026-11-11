#include "platform.h"
#include "theme.h"
#include "scrollbar.h"
#include "key_popup.h"
#include "key_channel.h"
#include "bootstrap.h"
#include "engine_release.h"
#include <commctrl.h>
#include <shellapi.h>
#include <objbase.h>
#include <iostream>
#include <algorithm>
#include <stdexcept>
using namespace manager;
namespace {
int paints = 0, sizes = 0, headerPaints = 0;
bool fitColumns = false;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::wstring text(HWND window) {
    std::wstring value(GetWindowTextLengthW(window) + 1, L'\0');
    value.resize(GetWindowTextW(window, value.data(), static_cast<int>(value.size())));
    return value;
}
void pump(DWORD milliseconds) {
    auto start = GetTickCount64();
    do {
        MSG message{};
        for (int count = 0; count < 200 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        Sleep(1);
    } while (GetTickCount64() - start < milliseconds);
}
LRESULT CALLBACK countPaint(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == WM_PAINT) ++paints;
    if (message == WM_SIZE) ++sizes;
    auto result = DefSubclassProc(window, message, wp, lp);
    if (fitColumns && message == WM_SIZE) {
        RECT client{}; GetClientRect(window, &client);
        int width = (std::max)(1L, client.right - 1);
        if (ListView_GetColumnWidth(window, 0) != width) ListView_SetColumnWidth(window, 0, width);
    }
    return result;
}
LRESULT CALLBACK countHeader(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == WM_PAINT) ++headerPaints;
    return DefSubclassProc(window, message, wp, lp);
}
void headerChecks(HWND list) {
    HWND header = capsule::header(list);
    check(header && header != ListView_GetHeader(list), "header is still coupled to list painting");
    check(!IsWindowVisible(ListView_GetHeader(list)), "built-in list header is still visible");
    check((GetWindowLongPtrW(list, GWL_STYLE) & WS_CLIPCHILDREN) != 0, "list painting can overwrite its header");
    RECT rect{}; GetClientRect(header, &rect);
    HDC dc = GetDC(header), memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, rect.right, rect.bottom);
    check(memory && bitmap, "could not allocate header paint probe");
    auto old = SelectObject(memory, bitmap);
    HBRUSH sentinel = CreateSolidBrush(RGB(255,0,255)); FillRect(memory, &rect, sentinel); DeleteObject(sentinel);
    auto erased = SendMessageW(header, WM_ERASEBKGND, reinterpret_cast<WPARAM>(memory), 0);
    bool retained = GetPixel(memory, 1, 1) == RGB(255,0,255);
    SendMessageW(header, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT | PRF_ERASEBKGND);
    bool rendered = GetPixel(memory, 1, 1) != RGB(255,0,255);
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(header, dc);
    check(erased && retained, "header erases the screen before buffered painting");
    check(rendered, "native header cannot render to an offscreen DC");

    pump(100); headerPaints = 0;
    GetWindowRect(header, &rect); MapWindowPoints(nullptr, list, reinterpret_cast<POINT*>(&rect), 2);
    for (int i = 0; i < 20; ++i)
        SetWindowPos(header, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    pump(150); headerPaints = 0; pump(150); check(headerPaints <= 2, "independent header does not become idle");
    DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int i = 0; i < 50; ++i) RedrawWindow(header, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= before + 2, "header buffers leak GDI objects");
    int width = ListView_GetColumnWidth(list, 0);
    ListView_SetColumnWidth(list, 0, width + 30); pump(100);
    RECT column{}; Header_GetItemRect(header, 0, &column);
    check(column.right - column.left == width + 30, "real column resizing was suppressed");
    ListView_SetColumnWidth(list, 0, width); pump(100);
    HDITEMW changed{}; changed.mask = HDI_WIDTH; changed.cxy = width + 20;
    Header_SetItem(header, 0, &changed); pump(100);
    check(ListView_GetColumnWidth(list, 0) == width + 20, "visible header divider did not update list column");
    ListView_SetColumnWidth(list, 0, width); pump(100);
}
LRESULT CALLBACK ownerProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_MEASUREITEM) { reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight = 28; return TRUE; }
    if (message == WM_DRAWITEM && reinterpret_cast<DRAWITEMSTRUCT*>(lp)->CtlType == ODT_COMBOBOX) {
        theme::comboItem(*reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE;
    }
    if (message == WM_NOTIFY) {
        auto* header = reinterpret_cast<NMHDR*>(lp);
        if (header->idFrom == 10 && header->code == NM_CUSTOMDRAW) {
            auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                // This read during drawing used to enqueue another scrollbar sync/repaint.
                ListView_GetItemState(header->hwndFrom, static_cast<int>(draw->nmcd.dwItemSpec), LVIS_SELECTED);
                theme::listRow(header->hwndFrom, draw->nmcd.hdc, static_cast<int>(draw->nmcd.dwItemSpec),
                    static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)), 96, false);
                return CDRF_SKIPDEFAULT;
            }
        }
    }
    return DefWindowProcW(window, message, wp, lp);
}
HWND descendant(HWND parent, int id) {
    struct Search { int id; HWND found = nullptr; } search{id};
    EnumChildWindows(parent, [](HWND child, LPARAM context) -> BOOL {
        auto& s = *reinterpret_cast<Search*>(context);
        if (GetDlgCtrlID(child) == s.id) { s.found = child; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}
void rowThemeChecks(HWND list) {
    int width = ListView_GetColumnWidth(list, 0);
    ListView_SetColumnWidth(list, 0, 250);
    LVCOLUMNW action{}; action.mask = LVCF_TEXT | LVCF_WIDTH; action.cx = 100;
    action.pszText = const_cast<LPWSTR>(L"Action"); ListView_InsertColumn(list, 1, &action);
    RECT client{}, row{}; GetClientRect(list, &client); ListView_GetItemRect(list, 0, &row, LVIR_BOUNDS);
    HDC dc = GetDC(list), memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
    check(memory && bitmap, "could not allocate row paint probe");
    auto old = SelectObject(memory, bitmap); auto palette = theme::colors;
    auto font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    DWORD resources = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (bool contrast : {false, true}) {
        theme::colors = palette;
        if (contrast) {
            theme::colors.highContrast = true;
            theme::colors.selected = theme::colors.hover = RGB(0,0,128);
            theme::colors.onAccent = RGB(255,255,255);
        }
        for (bool selected : {false, true}) for (bool hot : {false, true}) {
            ListView_SetItemState(list, 0, selected ? LVIS_SELECTED : 0, LVIS_SELECTED);
            theme::listRow(list, memory, 0, font, 96, hot, 1);
            auto face = selected ? theme::colors.selected : hot ? theme::colors.hover : theme::colors.surface;
            check(GetPixel(memory, 5, row.top + 1) == face && GetPixel(memory, 255, row.top + 1) == face &&
                GetPixel(memory, 400, row.top + 1) == face, "row, action cell and trailing space have different backgrounds");
        }
    }
    theme::colors = palette;
    for (int i = 0; i < 100; ++i) theme::listRow(list, memory, 0, font, 96, false, 1);
    check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= resources + 2, "row renderer leaked GDI objects");
    wchar_t text[32]{}; ListView_GetItemText(list, 0, 0, text, 32);
    check(wcscmp(text, L"Record") == 0, "custom paint changed native item text");
    SetFocus(list); ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    SendMessageW(list, WM_KEYDOWN, VK_DOWN, 0);
    check(ListView_GetNextItem(list, -1, LVNI_FOCUSED) == 1, "native keyboard navigation was lost");
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(list, dc);
    ListView_DeleteColumn(list, 1); ListView_SetColumnWidth(list, 0, width);
}
void stableList(HWND host, HWND list) {
    RECT outer{}, content{}; GetClientRect(host, &outer); GetWindowRect(list, &content);
    MapWindowPoints(nullptr, host, reinterpret_cast<POINT*>(&content), 2);
    RECT heading{}; GetClientRect(GetParent(capsule::header(list)), &heading);
    check(content.left == 0 && content.top == heading.bottom && content.right == outer.right && content.bottom == outer.bottom,
        "list and independent header no longer fit their host");
    sizes = paints = headerPaints = 0; pump(150);
    check(sizes == 0, "idle list keeps resizing itself");
    check(paints <= 2, "idle list keeps repainting itself");
    check(headerPaints <= 2, "idle header keeps repainting itself");
}
LRESULT CALLBACK fitDefaultColumns(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    auto result = DefSubclassProc(window, message, wp, lp);
    if (message == WM_SIZE && !capsule::hasCustomColumns(window)) {
        RECT client{}; GetClientRect(window, &client);
        ListView_SetColumnWidth(window, 0, (std::max)(100L, client.right - 261));
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, fitDefaultColumns, id);
    return result;
}
void headerDragChecks(HWND owner) {
    HWND host = capsule::host(owner);
    SetWindowPos(host, nullptr, 10, 300, 600, 120, SWP_NOZORDER);
    HWND list = CreateWindowExW(0, WC_LISTVIEWW, L"Drag probe", WS_CHILD | WS_VISIBLE | LVS_REPORT,
        0, 0, 600, 120, host, nullptr, GetModuleHandleW(nullptr), nullptr);
    capsule::attach(host, list, true, false);
    int column = 0;
    for (int width : {260, 180, 80}) {
        LVCOLUMNW item{}; item.mask = LVCF_TEXT | LVCF_WIDTH;
        item.pszText = const_cast<LPWSTR>(L"Column"); item.cx = width;
        ListView_InsertColumn(list, column++, &item);
    }
    SetWindowSubclass(list, fitDefaultColumns, 10, 0);
    capsule::refresh(list); pump(100);
    check(!capsule::hasCustomColumns(list), "automatic header synchronization was marked as manual");
    HWND header = capsule::header(list);
    RECT cell{}, client{}; Header_GetItemRect(header, 0, &cell); GetClientRect(header, &client);
    int original = ListView_GetColumnWidth(list, 0), divider = cell.right - 1, y = client.bottom / 2;
    HDC dc = GetDC(header), memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
    check(memory && bitmap, "could not allocate divider paint probe");
    auto old = SelectObject(memory, bitmap);
    SendMessageW(header, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT);
    check(GetPixel(memory, divider, y) == theme::colors.border, "header column separator is missing");
    SendMessageW(header, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(divider, y));
    SendMessageW(header, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(divider + 100, y)); pump(30);
    SendMessageW(header, WM_LBUTTONUP, 0, MAKELPARAM(divider + 100, y)); pump(100);
    int resized = ListView_GetColumnWidth(list, 0);
    check(capsule::hasCustomColumns(list) && resized >= original + 90, "dragged column snapped to its automatic width");
    Header_GetItemRect(header, 0, &cell);
    SendMessageW(header, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT);
    check(GetPixel(memory, divider, y) == theme::colors.header &&
        GetPixel(memory, cell.right - 1, y) == theme::colors.border, "header cache retained the old divider after dragging");
    SetWindowPos(host, nullptr, 10, 300, 540, 120, SWP_NOZORDER);
    ShowWindow(host, SW_HIDE); ShowWindow(host, SW_SHOW); capsule::refresh(list); pump(100);
    check(ListView_GetColumnWidth(list, 0) == resized, "resize or page visibility reset the dragged width");
    Header_GetItemRect(header, 0, &cell);
    check(cell.right - cell.left == resized, "header and list columns diverged after layout");
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(header, dc);
    DestroyWindow(host);
}
void wrappedEditChecks(HWND owner) {
    HWND host = capsule::host(owner);
    SetWindowPos(host, nullptr, 10, 300, 550, 64, SWP_NOZORDER);
    HWND edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        0, 0, 550, 64, host, nullptr, GetModuleHandleW(nullptr), nullptr);
    capsule::attach(host, edit, false, true);
    HWND horizontal = descendant(host, capsule::horizontalId);
    HWND vertical = descendant(host, capsule::verticalId);
    check(horizontal && vertical, "wrapped edit capsules missing");
    for (int width : {550, 180, 750}) {
        SetWindowPos(host, nullptr, 10, 300, width, 64, SWP_NOZORDER);
        for (const auto& text : {std::wstring(L"Short note"), std::wstring(1000, L'W'), std::wstring()}) {
            SetWindowTextW(edit, text.c_str()); capsule::refresh(edit); pump(50);
            check(!IsWindowVisible(horizontal) && !capsule::range(horizontal).scrollable(), "wrapped note displays horizontal scrollbar");
            if (text.size() == 1000)
                check(IsWindowVisible(vertical) && capsule::range(vertical).scrollable(), "wrapped note lost vertical scrolling");
        }
    }

    // Regression: scrollbar metadata must not enable an undeclared axis.
    SCROLLINFO stale{sizeof(stale), SIF_RANGE | SIF_PAGE | SIF_POS, 0, 100, 0, 0, 0};
    SetScrollInfo(edit, SB_HORZ, &stale, FALSE); capsule::refresh(edit); pump(50);
    check(!IsWindowVisible(horizontal) && !capsule::range(horizontal).scrollable(), "stale horizontal range enabled a wrapped note's scrollbar");
    DestroyWindow(host);
}
void themeChecks(HWND owner) {
    auto originalPalette = theme::colors;
    auto light = theme::paletteFor(false, false), dark = theme::paletteFor(true, false), contrast = theme::paletteFor(true, true);
    check(!light.dark && dark.dark && !contrast.dark && contrast.highContrast, "theme/high-contrast precedence is wrong");
    check(light.surface != dark.surface && light.text != dark.text && theme::paletteFor(false, false) == light, "theme round trip changed colors");
    HWND checkBox = CreateWindowExW(0, L"BUTTON", L"Theme checkbox", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        20, 440, 250, 30, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_HASSTRINGS | CBS_OWNERDRAWFIXED,
        20, 480, 250, 180, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND button = CreateWindowExW(0, L"BUTTON", L"Theme button", WS_CHILD | BS_OWNERDRAW,
        20, 520, 180, 38, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND editable = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN |
        CBS_DROPDOWN | CBS_HASSTRINGS | CBS_OWNERDRAWFIXED | CBS_AUTOHSCROLL,
        300, 480, 250, 180, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(checkBox && combo && button && editable, "theme controls could not be created");
    for (HWND control : {checkBox, combo, button, editable}) theme::styleControl(control);
    COMBOBOXINFO editableInfo{}; editableInfo.cbSize = sizeof(editableInfo);
    check(GetComboBoxInfo(editable, &editableInfo) && IsChild(editable, editableInfo.hwndItem), "editable combo lost native text input");
    SendMessageW(editable, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Ethernet — 192.0.2.1"));
    SendMessageW(editable, CB_SETCURSEL, 0, 0);
    check(text(editable) == L"Ethernet — 192.0.2.1", "editable combo lost selected interface caption");
    SendMessageW(editable, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    SetWindowTextW(editable, L"fe80::1%");
    SendMessageW(editableInfo.hwndItem, EM_SETSEL, 8, 8);
    SendMessageW(editableInfo.hwndItem, WM_CHAR, L'7', 0);
    check(text(editable) == L"fe80::1%7", "styled combo lost manual IP editing");
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"First"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Second"));
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
    SendMessageW(checkBox, BM_CLICK, 0, 0);
    check(SendMessageW(checkBox, BM_GETCHECK, 0, 0) == BST_CHECKED, "styled checkbox lost native toggling");
    SetFocus(combo); SendMessageW(combo, WM_KEYDOWN, VK_DOWN, 0);
    check(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1, "styled combo lost native keyboard selection");
    HDC dc = GetDC(owner), memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, 300, 80);
    check(memory && bitmap, "could not allocate theme probe"); auto old = SelectObject(memory, bitmap);
    auto font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    for (const auto& palette : {light, dark, contrast, light}) {
        theme::colors = palette;
        RECT all{0,0,300,80}; theme::fill(memory, all, RGB(255,0,255));
        for (HWND control : {checkBox, combo, button, editable})
            check(SendMessageW(control, WM_ERASEBKGND, reinterpret_cast<WPARAM>(memory), 0) == 1 &&
                GetPixel(memory, 1, 1) == RGB(255,0,255), "control erases before its buffered paint");
        RECT offset{10,12,190,50};
        theme::button(memory, offset, L"Buffered", font, theme::Button::Secondary, true, false, false, false, false, 96, palette.surface);
        check(GetPixel(memory, 10, 12) == palette.surface && GetPixel(memory, 1, 1) == RGB(255,0,255), "button buffer painted outside its item rectangle");
        for (HWND control : {checkBox, combo, editable}) {
            SendMessageW(control, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT);
            check(GetPixel(memory, 0, 0) == palette.surface, "styled control retained its previous theme background");
        }
        check(SendMessageW(checkBox, BM_GETCHECK, 0, 0) == BST_CHECKED && SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1,
            "theme change reset native control state");
        check(text(editable) == L"fe80::1%7", "theme change reset manual listen address");
    }
    DWORD resources = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int i = 0; i < 100; ++i) {
        RECT area{0,0,180,38};
        theme::button(memory, area, L"Buffered", font, theme::Button::Secondary, true, i % 2, false, false, false, 96, light.surface);
        SendMessageW(combo, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT);
    }
    check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= resources + 2, "control buffering leaked GDI objects");
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(owner, dc);
    for (HWND control : {checkBox, combo, button, editable}) DestroyWindow(control);
    theme::colors = originalPalette;
    auto brush = theme::surfaceBrush();
    check(!theme::refresh() && brush == theme::surfaceBrush(), "unchanged system settings recreate the palette");
}
void controls() {
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = ownerProc; cls.lpszClassName = L"Manager.Controls.Test";
    RegisterClassW(&cls);
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"Native controls regression", WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
        20, 20, 800, 650, nullptr, nullptr, cls.hInstance, nullptr);
    check(window != nullptr, "owner creation failed");
    HWND host = capsule::host(window);
    SetWindowPos(host, nullptr, 10, 10, 600, 200, SWP_NOZORDER);
    HWND list = CreateWindowExW(0, WC_LISTVIEWW, L"Test list", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | LVS_REPORT,
        0, 0, 600, 200, host, reinterpret_cast<HMENU>(10), cls.hInstance, nullptr);
    capsule::attach(host, list, true, false);
    SetWindowSubclass(list, countPaint, 9, 0);
    SetWindowSubclass(capsule::header(list), countHeader, 9, 0);
    ListView_SetExtendedListViewStyle(list, LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH; column.pszText = const_cast<LPWSTR>(L"Entry"); column.cx = 1000;
    ListView_InsertColumn(list, 0, &column);
    for (int i = 0; i < 60; ++i) { LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = i; item.pszText = const_cast<LPWSTR>(L"Record"); ListView_InsertItem(list, &item); }
    pump(250); rowThemeChecks(list); headerChecks(list); paints = 0; pump(350);
    check(paints <= 2, "idle list continuously repaints after custom-draw queries");
    for (int i = 0; i < 50; ++i) { ListView_GetItemState(list, i, LVIS_SELECTED); ListView_GetColumnWidth(list, 0); }
    paints = 0; pump(250); check(paints <= 2, "read-only list queries trigger repaint loop");
    auto vertical = descendant(host, capsule::verticalId);
    check(vertical != nullptr, "list capsule missing");
    check(capsule::range(vertical).scrollable(), "list scrollbar did not synchronize after insertions");
    SendMessageW(host, WM_VSCROLL, SB_BOTTOM, reinterpret_cast<LPARAM>(vertical)); pump(100);
    check(capsule::range(vertical).position > 0, "list capsule could not scroll");
    HWND horizontal = descendant(host, capsule::horizontalId);
    SendMessageW(host, WM_HSCROLL, SB_BOTTOM, reinterpret_cast<LPARAM>(horizontal)); pump(100);
    RECT heading{}; GetWindowRect(capsule::header(list), &heading);
    MapWindowPoints(nullptr, GetParent(capsule::header(list)), reinterpret_cast<POINT*>(&heading), 2);
    check(heading.left == -capsule::range(horizontal).position, "header is misaligned after horizontal scrolling");
    stableList(host, list);
    ListView_DeleteAllItems(list); pump(150);
    check(!capsule::range(vertical).scrollable(), "list scrollbar did not synchronize after deletion");

    // Cover both the events table (wide columns) and auto-fitting authorized keys,
    // including row counts/heights at native scrollbar appearance thresholds.
    for (bool fit : {false, true}) {
        fitColumns = fit;
        for (int rows : {0, 1, 4, 6, 60}) {
            { capsule::ListUpdate update(list);
            ListView_DeleteAllItems(list);
            ListView_SetColumnWidth(list, 0, fit ? 599 : 1000);
            for (int i = 0; i < rows; ++i) {
                LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = i; item.pszText = const_cast<LPWSTR>(L"Key / event");
                ListView_InsertItem(list, &item);
            }
            }
            for (int height : {141, 142, 200}) {
                SetWindowPos(host, nullptr, 10, 10, 600, height, SWP_NOZORDER | SWP_NOACTIVATE);
                pump(100); stableList(host, list);
                ShowWindow(host, SW_HIDE); ShowWindow(host, SW_SHOW); pump(50); stableList(host, list);
            }
        }
    }
    fitColumns = false;

    themeChecks(window);
    headerDragChecks(window);
    wrappedEditChecks(window);
    HWND anchor = CreateWindowExW(0, L"BUTTON", L"Import", WS_CHILD | WS_VISIBLE, 450, 260, 120, 38, window, nullptr, cls.hInstance, nullptr);
    int submits = 0;
    auto submit = [&](const std::string&) { ++submits; return false; };
    showKeyPopup(window, anchor, L"Test target", submit); pump(100);
    HWND popup = keyPopupWindow(); check(popup != nullptr, "input popover missing");
    HWND input = GetDlgItem(popup, 10); check(input != nullptr, "popover input missing");
    SetWindowTextW(input, L"-----BEGIN OPENSSH PRIVATE KEY-----");
    SendMessageW(popup, WM_COMMAND, IDOK, 0);
    check(submits == 0 && keyPopupWindow() == popup, "invalid key submitted or input discarded");
    SetWindowTextW(input, L"ssh-ed25519 AAAABBBBBBBBBBBBBBBB test");
    SendMessageW(popup, WM_COMMAND, IDOK, 0);
    check(submits == 1 && keyPopupWindow() == popup && GetWindowTextLengthW(input) > 0, "cancelled submission lost input");
    SendMessageW(popup, WM_COMMAND, IDCANCEL, 0); check(!keyPopupWindow(), "popover cancel did not close");
    showKeyPopup(window, anchor, L"Test target", [](const std::string&) { return true; });
    popup = keyPopupWindow(); SetWindowTextW(GetDlgItem(popup, 10), L"ssh-ed25519 AAAABBBBBBBBBBBBBBBB test");
    SendMessageW(popup, WM_COMMAND, IDOK, 0); check(!keyPopupWindow(), "successful submission did not close");
    DestroyWindow(window);
}
void channel() {
    KeyChannel channel; channel.open();
    auto command = quoteArgument(modulePath()) + L" --channel-test " + quoteArgument(channel.token);
    STARTUPINFOW start{}; start.cb = sizeof(start); PROCESS_INFORMATION child{};
    check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &start, &child), "channel peer did not start");
    CloseHandle(child.hThread);
    auto deadline = GetTickCount64() + 10000;
    try {
        while (true) {
            channel.poll(child.hProcess);
            if (WaitForSingleObject(child.hProcess, 0) == WAIT_OBJECT_0) break;
            check(GetTickCount64() < deadline, "response channel stalled"); Sleep(5);
        }
        DWORD code = 1; GetExitCodeProcess(child.hProcess, &code);
        check(code == 0, "response channel peer failed");
        check(channel.result() == std::string(256 * 1024, 'A'), "streamed response truncated or corrupted");
    } catch (...) { channel.close(); WaitForSingleObject(child.hProcess, 2000); CloseHandle(child.hProcess); throw; }
    CloseHandle(child.hProcess);
}
void bootstrapChecks() {
    check(setupResource(301).find("function Install-EngineService") != std::string::npos, "embedded installer missing");
    check(setupResource(302).find("Add-EngineToMachinePath") != std::string::npos, "embedded PATH helper missing");
    check(setupResource(303).find("Expand-VerifiedEngineArchive") != std::string::npos, "embedded archive helper missing");
    check(setupResource(304).find("OwnsService") != std::string::npos, "embedded uninstaller missing");
    check(setupResource(305).find(engineRelease::sha256) != std::string::npos, "downloader and embedded installer digests differ");
    check(setupResource(306).find("Select-SetupLanguage") != std::string::npos, "embedded installer translations missing");
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
    check(GetTempPathW(MAX_PATH, directory) && GetTempFileNameW(directory, L"ssh", 0, path), "cannot create ZIP validation fixture");
    struct Cleanup { const wchar_t* path; ~Cleanup() { DeleteFileW(path); } } cleanup{path};
    auto rejected = [&] {
        bool failed = false;
        try { verifiedEngineArchive(path); } catch (const std::exception&) { failed = true; }
        check(failed, "unverified ZIP accepted by elevated snapshot reader");
    };
    rejected(); // empty
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    check(file != INVALID_HANDLE_VALUE, "cannot write ZIP fixture");
    DWORD written = 0; bool ok = WriteFile(file, "PKxx", 4, &written, nullptr) && written == 4;
    CloseHandle(file); check(ok, "cannot write ZIP fixture"); rejected(); // nonmatching digest
    file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(file != INVALID_HANDLE_VALUE, "cannot hold ZIP fixture open");
    try { rejected(); } catch (...) { CloseHandle(file); throw; }
    CloseHandle(file); // concurrent writer is refused, even before hashing
}

}
int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        int count = 0; auto argv = CommandLineToArgvW(GetCommandLineW(), &count);
        bool peer = count == 3 && std::wstring(argv[1]) == L"--channel-test";
        std::wstring token = peer ? argv[2] : L""; LocalFree(argv);
        if (peer) { KeyChannelWriter writer(token); writer.send(std::string(256 * 1024, 'A')); return 0; }
        wchar_t executable[32768]{};
        check(GetModuleFileNameW(nullptr, executable, 32768) != 0, "cannot locate version resource fixture");
        auto version = executableVersion(executable);
        check(!version.empty() && executableVersion(executable) == version, "cannot read/cache executable file version");
        check(executableVersion(std::wstring(executable) + L".missing").empty(), "missing executable inherited a cached version");
        INITCOMMONCONTROLSEX init{sizeof(init), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES}; InitCommonControlsEx(&init);
        bootstrapChecks(); theme::initialize(); controls(); channel(); theme::shutdown(); CoUninitialize();
        std::cout << "Buffered header erase/render/resize/GDI lifetime, stable list geometry, idle paints, key popup validation/cancel and local response streaming passed.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
