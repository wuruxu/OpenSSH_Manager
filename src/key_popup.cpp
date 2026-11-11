#include "i18n.h"
#include "key_popup.h"
#include "platform.h"
#include "theme.h"
#include <commctrl.h>
#include <algorithm>
#include <stdexcept>
namespace manager {
namespace {
constexpr wchar_t popupClass[] = L"OpenSSHManager.KeyPopup";
HWND popup = nullptr;
struct Popup {
    HWND owner = nullptr, edit = nullptr, error = nullptr, hot = nullptr;
    HFONT font = nullptr;
    UINT dpi = 96;
    int tip = 40;
    bool above = false, submitting = false;
    std::wstring target;
    std::function<bool(const std::string&)> submit;
    int scale(int v) const { return MulDiv(v, dpi, 96); }
};
std::wstring contents(HWND window) {
    std::wstring value(GetWindowTextLengthW(window) + 1, 0);
    value.resize(GetWindowTextW(window, value.data(), static_cast<int>(value.size()))); return value;
}
LRESULT CALLBACK buttonProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR context) {
    auto& data = *reinterpret_cast<Popup*>(context);
    if (message == WM_MOUSEMOVE && data.hot != window) {
        HWND old = data.hot; data.hot = window;
        if (old) InvalidateRect(old, nullptr, FALSE);
        InvalidateRect(window, nullptr, FALSE);
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking);
    }
    if (message == WM_MOUSELEAVE && data.hot == window) {
        data.hot = nullptr; InvalidateRect(window, nullptr, FALSE);
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, buttonProc, id);
    return DefSubclassProc(window, message, wp, lp);
}
LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<Popup*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        data = static_cast<Popup*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
    }
    if (!data) return DefWindowProcW(window, message, wp, lp);
    switch (message) {
    case WM_CREATE: {
        data->font = CreateFontW(-data->scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, i18n::fontFamily());
        const int top = data->above ? 0 : 12;
        auto child = [&](const wchar_t* cls, const wchar_t* caption, int id, int x, int y, int w, int h, DWORD style) {
            HWND c = CreateWindowExW(0, cls, caption, WS_CHILD | WS_VISIBLE | style, data->scale(x), data->scale(top + y),
                data->scale(w), data->scale(h), window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(data->font), FALSE); theme::styleControl(c); return c;
        };
        child(L"STATIC", i18n::text(i18n::Text::PasteKey), 0, 20, 16, 470, 26, SS_NOPREFIX);
        child(L"STATIC", data->target.c_str(), 0, 20, 47, 470, 24, SS_NOPREFIX | SS_ENDELLIPSIS);
        data->edit = child(L"EDIT", L"", 10, 20, 82, 470, 92, WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL);
        SendMessageW(data->edit, EM_SETLIMITTEXT, 12000, 0);
        SendMessageW(data->edit, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"ssh-ed25519 AAAA… user@device"));
        data->error = child(L"STATIC", i18n::text(i18n::Text::PasteKeyHint), 0, 20, 186, 470, 48, SS_NOPREFIX);
        child(L"BUTTON", i18n::text(i18n::Text::Cancel), IDCANCEL, 278, 240, 100, 38, WS_TABSTOP | BS_OWNERDRAW);
        child(L"BUTTON", i18n::text(i18n::Text::ConfirmImport), IDOK, 390, 240, 100, 38, WS_TABSTOP | BS_OWNERDRAW);
        for (int id : {IDCANCEL, IDOK})
            SetWindowSubclass(GetDlgItem(window, id), buttonProc, 1, reinterpret_cast<DWORD_PTR>(data));
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) { if (!data->submitting) DestroyWindow(window); return 0; }
        if (LOWORD(wp) == IDOK && !data->submitting) {
            data->submitting = true;
            try {
                auto key = validatePublicKey(utf8(contents(data->edit)));
                if (wide(key).size() > 12000) throw std::runtime_error(i18n::utf8Text(i18n::Text::KeyTooLong));
                if (data->submit(key)) { data->submitting = false; DestroyWindow(window); return 0; }
                SetWindowTextW(data->error, i18n::text(i18n::Text::KeyAuthCanceled));
            } catch (const std::exception& e) { SetWindowTextW(data->error, wide(e.what()).c_str()); }
            data->submitting = false; SetForegroundWindow(window); SetFocus(data->edit); return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && !data->submitting) PostMessageW(window, WM_CLOSE, 0, 0);
        return 0;
    case WM_CLOSE: if (!data->submitting) DestroyWindow(window); return 0;
    case WM_DRAWITEM: {
        auto& draw = *reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        theme::button(draw.hDC, draw.rcItem, contents(draw.hwndItem).c_str(), data->font,
            draw.CtlID == IDOK ? theme::Button::Primary : theme::Button::Secondary,
            !(draw.itemState & ODS_DISABLED), data->hot == draw.hwndItem,
            (draw.itemState & ODS_SELECTED) != 0, (draw.itemState & ODS_FOCUS) != 0 && !(draw.itemState & ODS_NOFOCUSRECT),
            false, data->dpi, theme::colors.surface);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: {
        auto dc = reinterpret_cast<HDC>(wp); SetBkColor(dc, theme::colors.surface); SetTextColor(dc, theme::colors.text);
        return reinterpret_cast<LRESULT>(theme::surfaceBrush());
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{}; auto dc = BeginPaint(window, &ps); RECT rect{}; GetClientRect(window, &rect);
        theme::fill(dc, rect, theme::colors.surface);
        RECT body = rect; if (data->above) body.bottom -= data->scale(12); else body.top += data->scale(12);
        theme::rounded(dc, body, theme::colors.surface, theme::colors.border, data->scale(12), 1);
        POINT triangle[3] = {{data->tip - data->scale(12), data->above ? body.bottom - 1 : body.top + 1},
            {data->tip, data->above ? rect.bottom - 1 : 0}, {data->tip + data->scale(12), data->above ? body.bottom - 1 : body.top + 1}};
        HPEN pen = CreatePen(PS_SOLID, 1, theme::colors.border); auto oldPen = SelectObject(dc, pen); auto oldBrush = SelectObject(dc, theme::surfaceBrush());
        Polygon(dc, triangle, 3); SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
        RECT input{}; GetWindowRect(data->edit, &input); MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&input), 2); InflateRect(&input, data->scale(6), data->scale(6));
        theme::rounded(dc, input, theme::colors.surface, theme::colors.accent, data->scale(6), 1);
        EndPaint(window, &ps); return 0;
    }
    case WM_NCDESTROY:
        if (popup == window) popup = nullptr;
        DeleteObject(data->font); delete data; SetWindowLongPtrW(window, GWLP_USERDATA, 0); break;
    }
    return DefWindowProcW(window, message, wp, lp);
}
}
HWND keyPopupWindow() { return popup; }
void closeKeyPopup() { if (popup) SendMessageW(popup, WM_CLOSE, 0, 0); }
void showKeyPopup(HWND owner, HWND anchor, const std::wstring& target, std::function<bool(const std::string&)> submit) {
    closeKeyPopup();
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = popupClass; cls.lpfnWndProc = proc;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.style = CS_DROPSHADOW; RegisterClassW(&cls);
    auto* data = new Popup; data->owner = owner; data->dpi = GetDpiForWindow(owner); data->target = target; data->submit = std::move(submit);
    RECT button{}; GetWindowRect(anchor, &button);
    MONITORINFO monitor{sizeof(monitor), {}, {}, 0}; GetMonitorInfoW(MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST), &monitor);
    int width = data->scale(510), height = data->scale(302);
    int x = (std::max)(monitor.rcWork.left, (std::min)(button.right - width, monitor.rcWork.right - width));
    data->above = button.bottom + height > monitor.rcWork.bottom;
    int y = data->above ? button.top - height : button.bottom;
    y = (std::max)(static_cast<int>(monitor.rcWork.top), (std::min)(y, static_cast<int>(monitor.rcWork.bottom) - height));
    data->tip = (std::max)(data->scale(24), (std::min)(static_cast<int>((button.left + button.right) / 2) - x, width - data->scale(24)));
    popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, popupClass, i18n::text(i18n::Text::ImportKey), WS_POPUP | WS_CLIPCHILDREN,
        x, y, width, height, owner, nullptr, cls.hInstance, data);
    if (!popup) throw std::runtime_error(i18n::utf8Text(i18n::Text::KeyPopupFailed));
    HRGN region = CreateRoundRectRgn(0, data->above ? 0 : data->scale(12), width + 1, height - (data->above ? data->scale(12) : 0) + 1, data->scale(24), data->scale(24));
    POINT points[3] = {{data->tip - data->scale(12), data->above ? height - data->scale(13) : data->scale(13)},
        {data->tip, data->above ? height : 0}, {data->tip + data->scale(12), data->above ? height - data->scale(13) : data->scale(13)}};
    HRGN tip = CreatePolygonRgn(points, 3, WINDING); CombineRgn(region, region, tip, RGN_OR); DeleteObject(tip);
    if (!SetWindowRgn(popup, region, FALSE)) DeleteObject(region);
    ShowWindow(popup, SW_SHOW); SetForegroundWindow(popup); SetFocus(data->edit);
}
}
