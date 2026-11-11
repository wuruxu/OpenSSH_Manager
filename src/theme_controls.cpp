#include "theme.h"
#include <commctrl.h>
#include <algorithm>
#include <string>

namespace theme {
namespace {
enum class Kind { Button, Check, Combo };
struct Control { Kind kind; bool hot = false; };
UINT controlDpi(HWND window) {
    LOGFONTW font{};
    auto handle = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
    return handle && GetObjectW(handle, sizeof(font), &font) && font.lfHeight < 0 ?
        (std::max)(1, MulDiv(-font.lfHeight, 96, 14)) : GetDpiForWindow(window);
}
void paintControl(HWND window, HDC target, const Control& data) {
    RECT rect{}; GetClientRect(window, &rect);
    if (rect.right <= 0 || rect.bottom <= 0) return;
    HDC memory = CreateCompatibleDC(target);
    HBITMAP bitmap = memory ? CreateCompatibleBitmap(target, rect.right, rect.bottom) : nullptr;
    HDC dc = bitmap ? memory : target;
    auto original = bitmap ? SelectObject(memory, bitmap) : nullptr;
    int saved = SaveDC(dc);
    UINT dpi = controlDpi(window);
    auto scale = [dpi](int value) { return MulDiv(value, dpi, 96); };
    bool enabled = IsWindowEnabled(window) != FALSE;
    bool focus = (GetFocus() == window || IsChild(window, GetFocus())) && !(SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
    COLORREF ink = enabled ? colors.text : colors.muted;
    fill(dc, rect, colors.surface);
    RECT textRect = rect;
    if (data.kind == Kind::Check) {
        int side = scale(18), top = (rect.bottom - side) / 2;
        RECT box{0, top, side, top + side};
        bool checked = SendMessageW(window, BM_GETCHECK, 0, 0) != BST_UNCHECKED;
        COLORREF face = checked ? (enabled ? colors.accent : colors.disabled) : data.hot ? colors.hover : colors.surface;
        rounded(dc, box, face, enabled && (checked || data.hot) ? colors.accent : colors.border, scale(4), (std::max)(1, scale(1)));
        if (checked) {
            auto pen = CreatePen(PS_SOLID, (std::max)(1, scale(2)), enabled ? colors.onAccent : colors.muted);
            auto old = SelectObject(dc, pen);
            MoveToEx(dc, scale(4), top + scale(9), nullptr); LineTo(dc, scale(8), top + scale(13)); LineTo(dc, scale(14), top + scale(5));
            SelectObject(dc, old); DeleteObject(pen);
        }
        textRect.left = side + scale(10);
    } else {
        rounded(dc, rect, enabled && data.hot ? colors.hover : colors.surface,
            focus ? colors.accent : colors.border, scale(6), (std::max)(1, scale(1)));
        textRect.left += scale(10); textRect.right -= scale(30);
        int x = rect.right - scale(16), y = rect.bottom / 2;
        auto pen = CreatePen(PS_SOLID, (std::max)(1, scale(2)), ink); auto old = SelectObject(dc, pen);
        MoveToEx(dc, x - scale(4), y - scale(2), nullptr); LineTo(dc, x, y + scale(2)); LineTo(dc, x + scale(4), y - scale(2));
        SelectObject(dc, old); DeleteObject(pen);
    }
    std::wstring caption(GetWindowTextLengthW(window) + 1, 0);
    GetWindowTextW(window, caption.data(), static_cast<int>(caption.size()));
    auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
    SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, ink);
    // An editable combo's native EDIT child owns its text/caret/selection.
    // Paint only the surrounding frame and arrow, without duplicating the text.
    bool editable = data.kind == Kind::Combo && (GetWindowLongPtrW(window, GWL_STYLE) & 3) == CBS_DROPDOWN;
    if (!editable) DrawTextW(dc, caption.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (focus && data.kind == Kind::Check) {
        RECT mark{textRect.left, rect.bottom - scale(2), textRect.left + scale(24), rect.bottom}; fill(dc, mark, colors.accent);
    }
    RestoreDC(dc, saved);
    if (bitmap) {
        BitBlt(target, 0, 0, rect.right, rect.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, original); DeleteObject(bitmap);
    }
    if (memory) DeleteDC(memory);
}
LRESULT CALLBACK controlProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR context) {
    auto& data = *reinterpret_cast<Control*>(context);
    if (message == WM_ERASEBKGND) return 1;
    if (data.kind != Kind::Button) {
        if (message == WM_PAINT) {
            PAINTSTRUCT ps{}; auto dc = BeginPaint(window, &ps); paintControl(window, dc, data); EndPaint(window, &ps); return 0;
        }
        if (message == WM_PRINTCLIENT) { paintControl(window, reinterpret_cast<HDC>(wp), data); return 0; }
        if (message == WM_MOUSEMOVE && !data.hot) {
            data.hot = true; TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0}; TrackMouseEvent(&track);
            InvalidateRect(window, nullptr, FALSE);
        }
        if (message == WM_MOUSELEAVE) { data.hot = false; InvalidateRect(window, nullptr, FALSE); }
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, controlProc, id); delete &data;
        return DefSubclassProc(window, message, wp, lp);
    }
    bool customPaint = data.kind != Kind::Button;
    auto result = DefSubclassProc(window, message, wp, lp);
    if (customPaint && IsWindow(window) && (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE ||
        message == WM_THEMECHANGED || message == WM_UPDATEUISTATE || message == BM_SETCHECK ||
        message == CB_SETCURSEL || message == CB_SHOWDROPDOWN || message == WM_KEYDOWN || message == WM_LBUTTONUP ||
        (message == WM_COMMAND && (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS))))
        InvalidateRect(window, nullptr, FALSE);
    return result;
}
}
void styleControl(HWND window) {
    DWORD_PTR existing = 0;
    if (GetWindowSubclass(window, controlProc, 70, &existing)) return;
    wchar_t name[32]{}; GetClassNameW(window, name, 32);
    auto style = GetWindowLongPtrW(window, GWL_STYLE);
    Kind kind;
    if (_wcsicmp(name, L"ComboBox") == 0) kind = Kind::Combo;
    else if (_wcsicmp(name, L"Button") != 0) return;
    else if ((style & BS_TYPEMASK) == BS_OWNERDRAW) kind = Kind::Button;
    else if ((style & BS_TYPEMASK) == BS_AUTOCHECKBOX) kind = Kind::Check;
    else return;
    auto* data = new Control{kind};
    if (!SetWindowSubclass(window, controlProc, 70, reinterpret_cast<DWORD_PTR>(data))) delete data;
}
void comboItem(const DRAWITEMSTRUCT& draw) {
    bool selected = (draw.itemState & ODS_SELECTED) != 0;
    fill(draw.hDC, draw.rcItem, selected ? colors.selected : colors.surface);
    COLORREF ink = (draw.itemState & ODS_DISABLED) ? colors.muted : selected && colors.highContrast ? colors.onAccent : colors.text;
    if (draw.itemID == static_cast<UINT>(-1)) return;
    int length = static_cast<int>(SendMessageW(draw.hwndItem, CB_GETLBTEXTLEN, draw.itemID, 0));
    if (length < 0) return;
    std::wstring text(length + 1, 0);
    SendMessageW(draw.hwndItem, CB_GETLBTEXT, draw.itemID, reinterpret_cast<LPARAM>(text.data()));
    int saved = SaveDC(draw.hDC);
    auto font = reinterpret_cast<HFONT>(SendMessageW(draw.hwndItem, WM_GETFONT, 0, 0));
    SelectObject(draw.hDC, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(draw.hDC, ink); SetBkMode(draw.hDC, TRANSPARENT);
    RECT rect = draw.rcItem; InflateRect(&rect, -MulDiv(10, controlDpi(draw.hwndItem), 96), 0);
    DrawTextW(draw.hDC, text.c_str(), -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    RestoreDC(draw.hDC, saved);
}
}
