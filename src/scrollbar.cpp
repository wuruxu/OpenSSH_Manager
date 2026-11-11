#include "scrollbar.h"
#include "theme.h"
#include <commctrl.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <vector>
#include <string>
#include <algorithm>
#include <stdexcept>

namespace capsule {
namespace {
constexpr UINT syncMessage = WM_APP + 43;
constexpr wchar_t gutterClass[] = L"OpenSSHManager.ListGutter";
constexpr wchar_t hostClass[] = L"OpenSSHManager.ScrollHost";
constexpr wchar_t hostProperty[] = L"OpenSSHManager.ScrollHost.Owner";
struct Bar {
    bool vertical = false, canvas = false, hot = false, dragging = false;
    int grab = 0, repeatDirection = 0;
};
struct Host {
    HWND owner = nullptr, window = nullptr, child = nullptr, horizontal = nullptr, vertical = nullptr, corner = nullptr;
    HWND horizontalGutter = nullptr, verticalGutter = nullptr, headerClip = nullptr, header = nullptr;
    bool listView = false, canvas = false, syncing = false, pending = false, routingWheel = false;
    bool editHorizontal = false, editVertical = false;
    bool customColumns = false;
};
int dimension(HWND window, bool vertical) { RECT r{}; GetClientRect(window, &r); return vertical ? r.bottom : r.right; }
void notify(HWND bar, Bar& data, int value) {
    SetScrollPos(bar, SB_CTL, range(bar).clamp(value), FALSE);
    InvalidateRect(bar, nullptr, FALSE);
    SendMessageW(GetParent(bar), positionChanged, data.vertical ? SB_VERT : SB_HORZ, reinterpret_cast<LPARAM>(bar));
}
void paintBar(HWND window, HDC dc, Bar& data) {
    RECT rect{}; GetClientRect(window, &rect);
    theme::fill(dc, rect, data.canvas ? theme::colors.canvas : theme::colors.surface);
    auto slider = scrollmath::thumb(range(window), data.vertical ? rect.bottom : rect.right, MulDiv(24, GetDpiForWindow(window), 96));
    if (!slider.length) return;
    if (data.vertical) { rect.top = slider.start; rect.bottom = slider.start + slider.length; }
    else { rect.left = slider.start; rect.right = slider.start + slider.length; }
    COLORREF ink = theme::colors.highContrast ? theme::colors.text : data.dragging ? RGB(95,99,104) : data.hot ? RGB(128,134,139) : RGB(173,179,187);
    theme::rounded(dc, rect, ink, ink, (data.vertical ? rect.right : rect.bottom) / 2, 0);
}
LRESULT CALLBACK barProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR context) {
    auto& data = *reinterpret_cast<Bar*>(context);
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT paint{}; auto dc = BeginPaint(window, &paint); paintBar(window, dc, data); EndPaint(window, &paint); return 0; }
    case WM_PRINTCLIENT: paintBar(window, reinterpret_cast<HDC>(wp), data); return 0;
    case WM_LBUTTONDOWN: {
        auto r = range(window); if (!r.scrollable()) return 0;
        auto thumb = scrollmath::thumb(r, dimension(window, data.vertical), MulDiv(24, GetDpiForWindow(window), 96));
        int point = data.vertical ? GET_Y_LPARAM(lp) : GET_X_LPARAM(lp);
        SetCapture(window); data.hot = true;
        if (point >= thumb.start && point < thumb.start + thumb.length) { data.dragging = true; data.grab = point - thumb.start; }
        else {
            data.repeatDirection = point < thumb.start ? -1 : 1;
            notify(window, data, r.clamp(static_cast<int64_t>(r.position) + data.repeatDirection * static_cast<int64_t>((std::max)(1u, r.page))));
            SetTimer(window, 1, 400, nullptr);
        }
        InvalidateRect(window, nullptr, FALSE); return 0;
    }
    case WM_MOUSEMOVE:
        if (!data.hot) { data.hot = true; TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, window, 0}; TrackMouseEvent(&t); InvalidateRect(window, nullptr, FALSE); }
        if (data.dragging) {
            auto r = range(window); auto thumb = scrollmath::thumb(r, dimension(window, data.vertical), MulDiv(24, GetDpiForWindow(window), 96));
            notify(window, data, scrollmath::drag(r, thumb, (data.vertical ? GET_Y_LPARAM(lp) : GET_X_LPARAM(lp)) - data.grab));
        }
        return 0;
    case WM_MOUSELEAVE: data.hot = false; InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_TIMER:
        if (wp == 1 && data.repeatDirection) {
            POINT cursor{}; GetCursorPos(&cursor); ScreenToClient(window, &cursor);
            auto r = range(window); auto thumb = scrollmath::thumb(r, dimension(window, data.vertical), MulDiv(24, GetDpiForWindow(window), 96));
            int point = data.vertical ? cursor.y : cursor.x;
            if ((data.repeatDirection < 0 && point < thumb.start) || (data.repeatDirection > 0 && point >= thumb.start + thumb.length))
                notify(window, data, r.clamp(static_cast<int64_t>(r.position) + data.repeatDirection * static_cast<int64_t>((std::max)(1u, r.page))));
            SetTimer(window, 1, 70, nullptr);
        }
        return 0;
    case WM_LBUTTONUP: if (GetCapture() == window) ReleaseCapture(); return 0;
    case WM_CANCELMODE: if (GetCapture() == window) ReleaseCapture(); return 0;
    case WM_CAPTURECHANGED:
        data.dragging = false; data.repeatDirection = 0; KillTimer(window, 1); InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_NCDESTROY:
        KillTimer(window, 1); RemoveWindowSubclass(window, barProc, id); delete &data;
        return DefSubclassProc(window, message, wp, lp);
    }
    auto result = DefSubclassProc(window, message, wp, lp);
    if (message == SBM_SETSCROLLINFO || message == SBM_SETPOS || message == WM_ENABLE || message == WM_THEMECHANGED)
        InvalidateRect(window, nullptr, FALSE);
    return result;
}
scrollmath::Range nativeRange(HWND child, int bar) {
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, 0, 0, 0, 0};
    if (!GetScrollInfo(child, bar, &info)) return {};
    return {info.nMin, info.nMax, info.nPage, info.nPos};
}
scrollmath::Range scrollRange(const Host& data, int axis) {
    // A native range alone does not enable an axis: wrapped EDITs can expose
    // default/stale horizontal metadata even though they must only wrap.
    if (!data.listView && !(axis == SB_HORZ ? data.editHorizontal : data.editVertical)) return {};
    return nativeRange(data.child, axis);
}
void placeBar(HWND bar, int x, int y, int width, int height, bool visible) {
    RECT previous{}; GetWindowRect(bar, &previous);
    MapWindowPoints(nullptr, GetParent(bar), reinterpret_cast<POINT*>(&previous), 2);
    bool shown = (GetWindowLongPtrW(bar, GWL_STYLE) & WS_VISIBLE) != 0;
    if (previous.left == x && previous.top == y && previous.right - previous.left == width &&
        previous.bottom - previous.top == height && shown == visible) return;
    SetWindowPos(bar, HWND_TOP, x, y, width, height, SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}
int headerHeight(HWND list) {
    HWND source = ListView_GetHeader(list);
    auto font = reinterpret_cast<HFONT>(SendMessageW(source, WM_GETFONT, 0, 0));
    HDC dc = GetDC(list); auto old = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    TEXTMETRICW metrics{}; GetTextMetricsW(dc, &metrics); SelectObject(dc, old); ReleaseDC(list, dc);
    return metrics.tmHeight + (std::max)(12L, metrics.tmHeight);
}
void syncHeader(Host& data, int width, int height) {
    HWND source = ListView_GetHeader(data.child);
    if (!source) return;
    auto font = SendMessageW(source, WM_GETFONT, 0, 0);
    if (SendMessageW(data.header, WM_GETFONT, 0, 0) != font) SendMessageW(data.header, WM_SETFONT, font, TRUE);
    int count = Header_GetItemCount(source), total = 0;
    if (count < 0) return;
    while (Header_GetItemCount(data.header) > count)
        if (!Header_DeleteItem(data.header, Header_GetItemCount(data.header) - 1)) break;
    for (int i = 0; i < count; ++i) {
        wchar_t caption[1024]{}, previous[1024]{};
        HDITEMW item{}; item.mask = HDI_TEXT | HDI_WIDTH | HDI_FORMAT; item.pszText = caption; item.cchTextMax = 1024;
        Header_GetItem(source, i, &item); total += item.cxy;
        HDITEMW old{}; old.mask = item.mask; old.pszText = previous; old.cchTextMax = 1024;
        if (!Header_GetItem(data.header, i, &old)) Header_InsertItem(data.header, i, &item);
        else if (old.cxy != item.cxy || old.fmt != item.fmt || wcscmp(caption, previous) != 0) Header_SetItem(data.header, i, &item);
    }
    placeBar(data.headerClip, 0, 0, (std::max)(1, width), height, true);
    placeBar(data.header, -scrollRange(data, SB_HORZ).position, 0, (std::max)(width, total), height, true);
}
// The independent header occupies the top of the host. The list fills the
// remaining rectangle; never enlarge it based on WS_[HV]SCROLL. Native gutters
// are covered by the thin capsules without changing the list viewport.
void syncList(Host& data, const RECT& area, UINT dpi) {
    RECT previous{}; GetWindowRect(data.child, &previous);
    int heading = headerHeight(data.child);
    int width = (std::max)(1L, area.right), height = (std::max)(1L, area.bottom - heading);
    MapWindowPoints(nullptr, data.window, reinterpret_cast<POINT*>(&previous), 2);
    if (previous.left != 0 || previous.top != heading || previous.right - previous.left != width || previous.bottom - previous.top != height)
        SetWindowPos(data.child, nullptr, 0, heading, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    RECT client{}; GetClientRect(data.child, &client);
    MapWindowPoints(data.child, data.window, reinterpret_cast<POINT*>(&client), 2);
    auto style = GetWindowLongPtrW(data.child, GWL_STYLE);
    int gutterV = (std::max)(0L, area.right - client.right), gutterH = (std::max)(0L, area.bottom - client.bottom);
    bool vertical = (style & WS_VSCROLL) && gutterV > 0, horizontal = (style & WS_HSCROLL) && gutterH > 0;
    int thinV = (std::min)(thickness(dpi, true), gutterV), thinH = (std::min)(thickness(dpi, false), gutterH);
    configure(data.vertical, vertical ? scrollRange(data, SB_VERT) : scrollmath::Range{});
    configure(data.horizontal, horizontal ? scrollRange(data, SB_HORZ) : scrollmath::Range{});
    placeBar(data.verticalGutter, client.right, client.top, (std::max)(1, gutterV), (std::max)(1L, client.bottom - client.top), vertical);
    placeBar(data.horizontalGutter, client.left, client.bottom, (std::max)(1L, client.right - client.left), (std::max)(1, gutterH), horizontal);
    placeBar(data.vertical, (gutterV - thinV) / 2, 0, (std::max)(1, thinV), (std::max)(1L, client.bottom - client.top), vertical);
    placeBar(data.horizontal, 0, (gutterH - thinH) / 2, (std::max)(1L, client.right - client.left), (std::max)(1, thinH), horizontal);
    placeBar(data.corner, client.right, client.bottom, (std::max)(1, gutterV), (std::max)(1, gutterH), vertical && horizontal);
    syncHeader(data, client.right, heading);
}
void sync(Host& data) {
    if (data.syncing || !data.child) return;
    data.syncing = true; data.pending = false;
    RECT area{}; GetClientRect(data.window, &area);
    UINT dpi = GetDpiForWindow(data.window);
    if (data.listView) { syncList(data, area, dpi); data.syncing = false; return; }
    int thinV = thickness(dpi, true), thinH = thickness(dpi, false);
    bool vertical = scrollRange(data, SB_VERT).scrollable(), horizontal = scrollRange(data, SB_HORZ).scrollable();
    for (int pass = 0; pass < 3; ++pass) {
        auto style = GetWindowLongPtrW(data.child, GWL_STYLE);
        int width = (std::max)(1L, area.right - (vertical ? thinV : 0));
        int height = (std::max)(1L, area.bottom - (horizontal ? thinH : 0));
        // Native scrollbar pixels extend beyond the host or underneath the opaque capsule controls.
        int childWidth = width + ((style & WS_VSCROLL) ? GetSystemMetricsForDpi(SM_CXVSCROLL, dpi) : 0);
        int childHeight = height + ((style & WS_HSCROLL) ? GetSystemMetricsForDpi(SM_CYHSCROLL, dpi) : 0);
        RECT previous{}; GetWindowRect(data.child, &previous);
        if (previous.right - previous.left != childWidth || previous.bottom - previous.top != childHeight)
            SetWindowPos(data.child, HWND_BOTTOM, 0, 0, childWidth, childHeight, SWP_NOACTIVATE | SWP_NOCOPYBITS);
        bool nextV = scrollRange(data, SB_VERT).scrollable(), nextH = scrollRange(data, SB_HORZ).scrollable();
        auto nextStyle = GetWindowLongPtrW(data.child, GWL_STYLE);
        if (vertical == nextV && horizontal == nextH && ((style ^ nextStyle) & (WS_HSCROLL | WS_VSCROLL)) == 0) break;
        vertical = nextV; horizontal = nextH;
    }
    configure(data.vertical, scrollRange(data, SB_VERT)); configure(data.horizontal, scrollRange(data, SB_HORZ));
    placeBar(data.vertical, (std::max)(0L, area.right - thinV), 0, thinV, (std::max)(1L, area.bottom - (horizontal ? thinH : 0)), vertical);
    placeBar(data.horizontal, 0, (std::max)(0L, area.bottom - thinH), (std::max)(1L, area.right - (vertical ? thinV : 0)), thinH, horizontal);
    placeBar(data.corner, (std::max)(0L, area.right - thinV), (std::max)(0L, area.bottom - thinH), thinV, thinH, horizontal && vertical);
    data.syncing = false;
}
void queueSync(Host& data) {
    if (!data.syncing && !data.pending) { data.pending = true; PostMessageW(data.window, syncMessage, 0, 0); }
}
void scrollChild(Host& data, int axis, int target) {
    auto before = scrollRange(data, axis); target = before.clamp(target);
    int delta = target - before.position;
    if (data.listView) {
        int row = MulDiv(32, GetDpiForWindow(data.child), 96); RECT item{};
        if (ListView_GetItemRect(data.child, 0, &item, LVIR_BOUNDS)) row = item.bottom - item.top;
        ListView_Scroll(data.child, axis == SB_HORZ ? delta : 0, axis == SB_VERT ? delta * row : 0);
    } else if (axis == SB_VERT) {
        SendMessageW(data.child, EM_LINESCROLL, 0, target - SendMessageW(data.child, EM_GETFIRSTVISIBLELINE, 0, 0));
    } else if (target <= 65535) {
        SendMessageW(data.child, WM_HSCROLL, MAKEWPARAM(SB_THUMBPOSITION, target), 0);
    } else {
        // Edit controls use pixel offsets for their horizontal scrollbar, while EM_LINESCROLL uses character cells.
        HDC dc = GetDC(data.child); auto font = reinterpret_cast<HFONT>(SendMessageW(data.child, WM_GETFONT, 0, 0));
        auto old = SelectObject(dc, font); TEXTMETRICW metrics{}; GetTextMetricsW(dc, &metrics); SelectObject(dc, old); ReleaseDC(data.child, dc);
        int cell = (std::max)(1L, metrics.tmAveCharWidth);
        int chars = delta >= 0 ? (delta + cell / 2) / cell : (delta - cell / 2) / cell;
        SendMessageW(data.child, EM_LINESCROLL, static_cast<WPARAM>(chars), 0);
    }
    sync(data);
}
struct HeaderBuffer {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr, original = nullptr;
    int width = 0, height = 0;
    bool dirty = true;
    COLORREF face = 0, ink = 0, border = 0;
    void reset() {
        if (dc) { SelectObject(dc, original); DeleteObject(bitmap); DeleteDC(dc); }
        dc = nullptr; bitmap = original = nullptr;
    }
    ~HeaderBuffer() { reset(); }
};
void drawHeader(HWND window, HDC dc) {
    RECT client{}; GetClientRect(window, &client);
    theme::fill(dc, client, theme::colors.header);
    auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
    auto old = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    TEXTMETRICW metrics{}; GetTextMetricsW(dc, &metrics);
    const int padding = (std::max)(6L, metrics.tmHeight * 2 / 3);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, theme::colors.highContrast ? theme::colors.text : theme::colors.muted);
    for (int i = 0; i < Header_GetItemCount(window); ++i) {
        wchar_t caption[1024]{}; HDITEMW item{};
        item.mask = HDI_TEXT | HDI_FORMAT; item.pszText = caption; item.cchTextMax = 1024;
        if (!Header_GetItem(window, i, &item)) continue;
        RECT cell{}; Header_GetItemRect(window, i, &cell);
        if (i + 1 < Header_GetItemCount(window) && cell.right > cell.left) {
            int stroke = (std::max)(1L, metrics.tmHeight / 14);
            RECT divider{cell.right - stroke, client.bottom / 4, cell.right, client.bottom * 3 / 4};
            theme::fill(dc, divider, theme::colors.border);
        }
        cell.left += padding; cell.right -= padding;
        UINT align = (item.fmt & HDF_JUSTIFYMASK) == HDF_RIGHT ? DT_RIGHT : (item.fmt & HDF_JUSTIFYMASK) == HDF_CENTER ? DT_CENTER : DT_LEFT;
        DrawTextW(dc, caption, -1, &cell, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | align);
    }
    RECT bottom{0, client.bottom - 1, client.right, client.bottom}; theme::fill(dc, bottom, theme::colors.border);
    SelectObject(dc, old);
}
void paintHeader(HWND window, HDC target, HeaderBuffer& data) {
    RECT client{}; GetClientRect(window, &client);
    if (client.right <= 0 || client.bottom <= 0) return;
    if (!data.dc || data.width != client.right || data.height != client.bottom) {
        data.reset(); data.dc = CreateCompatibleDC(target);
        data.bitmap = data.dc ? CreateCompatibleBitmap(target, client.right, client.bottom) : nullptr;
        if (!data.bitmap) {
            if (data.dc) DeleteDC(data.dc);
            data.dc = nullptr; drawHeader(window, target); return;
        }
        data.original = static_cast<HBITMAP>(SelectObject(data.dc, data.bitmap));
        data.width = client.right; data.height = client.bottom; data.dirty = true;
    }
    COLORREF ink = theme::colors.highContrast ? theme::colors.text : theme::colors.muted;
    if (data.dirty || data.face != theme::colors.header || data.ink != ink || data.border != theme::colors.border) {
        drawHeader(window, data.dc); data.dirty = false;
        data.face = theme::colors.header; data.ink = ink; data.border = theme::colors.border;
    }
    BitBlt(target, 0, 0, client.right, client.bottom, data.dc, 0, 0, SRCCOPY);
}
LRESULT CALLBACK headerProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR context) {
    auto& data = *reinterpret_cast<HeaderBuffer*>(context);
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_NCPAINT: return 0;
    case WM_PRINT: case WM_PRINTCLIENT: paintHeader(window, reinterpret_cast<HDC>(wp), data); return 0;
    case WM_PAINT: {
        if (wp) { paintHeader(window, reinterpret_cast<HDC>(wp), data); return 0; }
        PAINTSTRUCT paint{}; auto dc = BeginPaint(window, &paint); paintHeader(window, dc, data); EndPaint(window, &paint); return 0;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, headerProc, id); delete &data;
        return DefSubclassProc(window, message, wp, lp);
    }
    // The native header still handles divider hit testing and dragging, but never
    // paints. In particular, do not call its WM_PRINTCLIENT from our WM_PAINT.
    bool changed = message == WM_SETFONT || message == WM_SIZE || message == WM_THEMECHANGED ||
        message == HDM_SETITEMW || message == HDM_INSERTITEMW || message == HDM_DELETEITEM;
    if (changed) data.dirty = true;
    auto result = DefSubclassProc(window, message, wp, lp);
    if (changed) InvalidateRect(window, nullptr, FALSE);
    return result;
}
LRESULT CALLBACK childProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR context) {
    auto* data = reinterpret_cast<Host*>(context);
    if (message == WM_NCDESTROY) {
        RemovePropW(window, hostProperty); RemoveWindowSubclass(window, childProc, id); data->child = nullptr;
        return DefSubclassProc(window, message, wp, lp);
    }
    auto result = DefSubclassProc(window, message, wp, lp);
    // Never schedule layout for read-only LVM/EM queries or custom-draw notifications:
    // a query from paint -> sync -> scrollbar position -> paint used to run indefinitely.
    bool headerChanged = false;
    if (data->listView && message == WM_NOTIFY && lp) {
        auto code = reinterpret_cast<NMHDR*>(lp)->code;
        if (code == HDN_ENDTRACKW || code == HDN_ENDTRACKA) headerChanged = true;
        else if (code == HDN_ITEMCHANGEDW || code == HDN_ITEMCHANGEDA) {
            const auto* header = reinterpret_cast<NMHEADERW*>(lp);
            headerChanged = header->pitem && (header->pitem->mask & (HDI_WIDTH | HDI_ORDER | HDI_TEXT | HDI_FORMAT));
        }
    }
    if (headerChanged || message == WM_STYLECHANGED || message == WM_SIZE || message == WM_SETTEXT || message == WM_SETFONT || message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL ||
        message == WM_VSCROLL || message == WM_HSCROLL || message == WM_KEYDOWN || message == WM_LBUTTONUP ||
        message == LVM_INSERTITEMW || message == LVM_DELETEITEM || message == LVM_DELETEALLITEMS ||
        message == LVM_SETITEMCOUNT || message == LVM_SETITEMTEXTW || message == LVM_SETCOLUMNWIDTH ||
        message == LVM_INSERTCOLUMNW || message == LVM_DELETECOLUMN || message == LVM_SETIMAGELIST ||
        message == LVM_SETCOLUMNW || message == LVM_SCROLL || message == LVM_ENSUREVISIBLE || message == EM_LINESCROLL || message == EM_REPLACESEL)
        queueSync(*data);
    return result;
}
LRESULT CALLBACK gutterProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; auto dc = BeginPaint(window, &paint); RECT rect{}; GetClientRect(window, &rect);
        theme::fill(dc, rect, theme::colors.surface); EndPaint(window, &paint); return 0;
    }
    case WM_NOTIFY: case positionChanged: case WM_VSCROLL: case WM_HSCROLL: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        return SendMessageW(GetParent(window), message, wp, lp);
    case WM_LBUTTONDOWN: {
        // The padding around the capsule remains part of its clickable track.
        HWND bar = GetWindow(window, GW_CHILD);
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; MapWindowPoints(window, bar, &point, 1);
        return SendMessageW(bar, message, wp, MAKELPARAM(point.x, point.y));
    }
    }
    return DefWindowProcW(window, message, wp, lp);
}
HWND gutter(HWND parent) {
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = gutterClass;
    cls.lpfnWndProc = gutterProc; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); RegisterClassW(&cls);
    auto window = CreateWindowExW(0, gutterClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 1, 1, parent, nullptr, cls.hInstance, nullptr);
    if (!window) throw std::runtime_error("Could not create list scrollbar gutter.");
    return window;
}
LRESULT CALLBACK hostProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<Host*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        data = static_cast<Host*>(create->lpCreateParams); data->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
    }
    if (!data) return DefWindowProcW(window, message, wp, lp);
    switch (message) {
    case WM_SIZE: sync(*data); return 0;
    case syncMessage: sync(*data); return 0;
    case positionChanged: scrollChild(*data, static_cast<int>(wp), range(reinterpret_cast<HWND>(lp)).position); return 0;
    case WM_VSCROLL: case WM_HSCROLL:
        if (lp) { auto bar = reinterpret_cast<HWND>(lp); scrollChild(*data, message == WM_VSCROLL ? SB_VERT : SB_HORZ, action(bar, LOWORD(wp), 1)); }
        return 0;
    case WM_NOTIFY:
        if (data->listView && reinterpret_cast<NMHDR*>(lp)->hwndFrom == data->header) {
            auto* notification = reinterpret_cast<NMHEADERW*>(lp);
            auto code = notification->hdr.code;
            // Mark before updating ListView: adding its horizontal scrollbar can
            // synchronously send WM_SIZE and invoke the owner's automatic fit.
            if (!data->syncing && (code == HDN_BEGINTRACKW || code == HDN_BEGINTRACKA)) data->customColumns = true;
            bool resized = code == HDN_ITEMCHANGEDW || code == HDN_ITEMCHANGEDA || code == HDN_ENDTRACKW || code == HDN_ENDTRACKA;
            if (!data->syncing && resized && notification->pitem && (notification->pitem->mask & HDI_WIDTH)) {
                data->customColumns = true;
                int width = notification->pitem->cxy;
                if (ListView_GetColumnWidth(data->child, notification->iItem) != width) ListView_SetColumnWidth(data->child, notification->iItem, width);
                // Native divider dragging changes header items internally, without
                // sending the HDM_SETITEM message that normally dirties our cache.
                DWORD_PTR context = 0;
                if (GetWindowSubclass(data->header, headerProc, 4, &context)) {
                    reinterpret_cast<HeaderBuffer*>(context)->dirty = true;
                    InvalidateRect(data->header, nullptr, FALSE);
                }
                queueSync(*data);
            }
            return 0;
        }
        return SendMessageW(data->owner, message, wp, lp);
    case WM_COMMAND: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN: case WM_CTLCOLORLISTBOX:
        return SendMessageW(data->owner, message, wp, lp);
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        if (!data->routingWheel && data->child && scrollRange(*data, message == WM_MOUSEWHEEL ? SB_VERT : SB_HORZ).scrollable()) {
            data->routingWheel = true; auto result = SendMessageW(data->child, message, wp, lp); data->routingWheel = false; return result;
        }
        return SendMessageW(data->owner, message, wp, lp);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(window, &ps); RECT rect{}; GetClientRect(window, &rect);
        theme::fill(dc, rect, data->canvas ? theme::colors.canvas : theme::colors.surface); EndPaint(window, &ps); return 0;
    }
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0); delete data; break;
    }
    return DefWindowProcW(window, message, wp, lp);
}
}
int thickness(UINT dpi, bool vertical) { return (std::max)(4, (GetSystemMetricsForDpi(vertical ? SM_CXVSCROLL : SM_CYHSCROLL, dpi) + 1) / 2); }
scrollmath::Range range(HWND bar) {
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, 0, 0, 0, 0};
    GetScrollInfo(bar, SB_CTL, &info); return {info.nMin, info.nMax, info.nPage, info.nPos};
}
void configure(HWND bar, const scrollmath::Range& value) {
    auto old = range(bar);
    if (old.minimum == value.minimum && old.maximum == value.maximum && old.page == value.page && old.position == value.clamp(value.position)) return;
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, value.minimum, value.maximum, value.page, value.clamp(value.position), 0};
    SetScrollInfo(bar, SB_CTL, &info, FALSE); InvalidateRect(bar, nullptr, FALSE);
}
int action(HWND bar, UINT code, int line) {
    auto r = range(bar); int64_t result = r.position;
    switch (code) {
    case SB_LINEUP: result -= line; break;
    case SB_LINEDOWN: result += line; break;
    case SB_PAGEUP: result -= r.page; break;
    case SB_PAGEDOWN: result += r.page; break;
    case SB_TOP: result = r.minimum; break;
    case SB_BOTTOM: result = r.last(); break;
    case SB_THUMBTRACK: case SB_THUMBPOSITION: {
        SCROLLINFO info{sizeof(info), SIF_TRACKPOS, 0, 0, 0, 0, 0}; GetScrollInfo(bar, SB_CTL, &info); result = info.nTrackPos; break;
    }
    default: break;
    }
    return r.clamp(result);
}
HWND create(HWND parent, bool vertical, int id, bool canvas) {
    HWND window = CreateWindowExW(0, L"SCROLLBAR", vertical ? L"垂直滚动" : L"水平滚动", WS_CHILD | WS_CLIPSIBLINGS | (vertical ? SBS_VERT : SBS_HORZ),
        0, 0, 1, 1, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (!window) throw std::runtime_error("Could not create capsule scrollbar.");
    auto* data = new Bar; data->vertical = vertical; data->canvas = canvas;
    if (!SetWindowSubclass(window, barProc, 1, reinterpret_cast<DWORD_PTR>(data))) { delete data; DestroyWindow(window); throw std::runtime_error("Could not initialize capsule scrollbar."); }
    return window;
}
HWND host(HWND owner) {
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = hostClass;
    cls.lpfnWndProc = hostProc; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); RegisterClassW(&cls);
    auto* data = new Host; data->owner = owner;
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, hostClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 1, 1, owner, nullptr, cls.hInstance, data);
    if (!window) { delete data; throw std::runtime_error("Could not create scrolling host."); }
    return window;
}
void attach(HWND window, HWND child, bool listView, bool canvas) {
    auto* data = reinterpret_cast<Host*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    data->child = child; data->listView = listView; data->canvas = canvas;
    if (!listView) {
        // Capture the declared behavior before Windows hides unused native bars.
        auto style = GetWindowLongPtrW(child, GWL_STYLE);
        data->editHorizontal = (style & (WS_HSCROLL | ES_AUTOHSCROLL)) != 0;
        data->editVertical = (style & (WS_VSCROLL | ES_AUTOVSCROLL)) != 0;
    }
    if (listView) {
        // Decouple the visible header from ListView's internal header layout and
        // paint cycle. The hidden built-in header remains the column model.
        SetWindowLongPtrW(child, GWL_STYLE, GetWindowLongPtrW(child, GWL_STYLE) | WS_CLIPCHILDREN | LVS_NOCOLUMNHEADER);
        data->headerClip = gutter(window);
        data->header = CreateWindowExW(0, WC_HEADERW, L"表头", WS_CHILD | WS_VISIBLE | HDS_HORZ | HDS_FULLDRAG,
            0, 0, 1, 1, data->headerClip, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!data->header) throw std::runtime_error("Could not create independent table header.");
        auto* buffer = new HeaderBuffer;
        if (!SetWindowSubclass(data->header, headerProc, 4, reinterpret_cast<DWORD_PTR>(buffer))) {
            delete buffer; throw std::runtime_error("Could not buffer table header.");
        }
        SendMessageW(data->header, CCM_SETUNICODEFORMAT, TRUE, 0);
        SetWindowTheme(data->header, L"", L"");
        ShowWindow(ListView_GetHeader(child), SW_HIDE);
        data->horizontalGutter = gutter(window); data->verticalGutter = gutter(window);
    }
    data->horizontal = create(listView ? data->horizontalGutter : window, false, horizontalId, canvas);
    data->vertical = create(listView ? data->verticalGutter : window, true, verticalId, canvas);
    data->corner = create(window, false, 0, canvas); configure(data->corner, {});
    SetPropW(child, hostProperty, window);
    SetWindowSubclass(child, childProc, 2, reinterpret_cast<DWORD_PTR>(data)); queueSync(*data);
}
HWND header(HWND child) {
    auto hostWindow = reinterpret_cast<HWND>(GetPropW(child, hostProperty));
    auto* data = hostWindow ? reinterpret_cast<Host*>(GetWindowLongPtrW(hostWindow, GWLP_USERDATA)) : nullptr;
    return data ? data->header : nullptr;
}
bool hasCustomColumns(HWND child) {
    auto window = reinterpret_cast<HWND>(GetPropW(child, hostProperty));
    auto* data = window ? reinterpret_cast<Host*>(GetWindowLongPtrW(window, GWLP_USERDATA)) : nullptr;
    return data && data->customColumns;
}
ListUpdate::ListUpdate(HWND list) : list_(list), header_(header(list)) {
    if (header_) SendMessageW(header_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
}
ListUpdate::~ListUpdate() {
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    if (header_) SendMessageW(header_, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(list_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
HWND outer(HWND child) { auto window = reinterpret_cast<HWND>(GetPropW(child, hostProperty)); return window ? window : child; }
void refresh(HWND child) {
    auto window = reinterpret_cast<HWND>(GetPropW(child, hostProperty));
    if (window) { auto* data = reinterpret_cast<Host*>(GetWindowLongPtrW(window, GWLP_USERDATA)); if (data) sync(*data); }
}
}
