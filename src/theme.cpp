#include "i18n.h"
namespace i18n = manager::i18n;
#include "theme.h"
#include <objidl.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <algorithm>
#include <stdexcept>

namespace theme {
Palette colors{};
namespace {
ULONG_PTR token = 0;
HBRUSH surface = nullptr, canvas = nullptr;
Gdiplus::Color color(COLORREF value) { return {255, GetRValue(value), GetGValue(value), GetBValue(value)}; }
int scaled(int value, UINT dpi) { return MulDiv(value, dpi, 96); }
COLORREF blend(COLORREF background, COLORREF foreground, int percent) {
    auto mix = [percent](int a, int b) { return (a * (100 - percent) + b * percent + 50) / 100; };
    return RGB(mix(GetRValue(background), GetRValue(foreground)),
        mix(GetGValue(background), GetGValue(foreground)), mix(GetBValue(background), GetBValue(foreground)));
}
void icon(HDC dc, int index, int x, int y, int size, COLORREF ink) {
    Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.TranslateTransform(static_cast<float>(x), static_cast<float>(y));
    g.ScaleTransform(size / 24.0f, size / 24.0f);
    Gdiplus::Pen pen(color(ink), 1.6f);
    pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound);
    if (index == 0) {
        g.DrawRectangle(&pen, 3, 3, 18, 7); g.DrawRectangle(&pen, 3, 14, 18, 7);
        g.DrawLine(&pen, 7, 6, 7, 7); g.DrawLine(&pen, 7, 17, 7, 18);
        g.DrawLine(&pen, 12, 7, 17, 7); g.DrawLine(&pen, 12, 18, 17, 18);
    } else if (index == 1) {
        g.DrawLine(&pen, 3, 6, 21, 6); g.DrawLine(&pen, 3, 12, 21, 12); g.DrawLine(&pen, 3, 18, 21, 18);
        Gdiplus::SolidBrush brush(color(ink));
        g.FillEllipse(&brush, 6, 3, 6, 6); g.FillEllipse(&brush, 14, 9, 6, 6); g.FillEllipse(&brush, 6, 15, 6, 6);
    } else if (index == 2) {
        g.DrawRectangle(&pen, 2, 4, 20, 16);
        g.DrawLine(&pen, 6, 8, 10, 12); g.DrawLine(&pen, 10, 12, 6, 16); g.DrawLine(&pen, 13, 16, 18, 16);
    } else {
        g.DrawRectangle(&pen, 4, 2, 16, 20);
        for (int row : {7, 12, 17}) g.DrawLine(&pen, 8, row, 16, row);
    }
}
}
Palette paletteFor(bool dark, bool high) {
    Palette result = {RGB(246,248,252), RGB(255,255,255), RGB(32,33,36), RGB(95,99,104), RGB(218,224,232),
        RGB(26,115,232), RGB(255,255,255), RGB(232,240,254), RGB(232,234,237), RGB(241,243,244),
        RGB(24,128,56), RGB(230,244,234), high,
        RGB(248,249,250), RGB(179,38,30), RGB(222,230,241), RGB(23,101,204), RGB(20,80,170), dark && !high};
    if (dark && !high) {
        result = {RGB(24,26,29), RGB(32,33,36), RGB(232,234,237), RGB(174,180,188), RGB(70,75,82),
            RGB(138,180,248), RGB(24,32,44), RGB(45,64,90), RGB(49,54,61), RGB(48,51,56),
            RGB(129,201,149), RGB(32,58,42), false,
            RGB(38,41,46), RGB(242,139,130), RGB(61,68,79), RGB(164,198,250), RGB(118,161,232), true};
    }
    if (high) {
        result.canvas = result.surface = result.disabled = GetSysColor(COLOR_WINDOW);
        result.text = result.success = GetSysColor(COLOR_WINDOWTEXT);
        result.muted = GetSysColor(COLOR_GRAYTEXT); result.border = GetSysColor(COLOR_WINDOWTEXT);
        result.accent = result.selected = GetSysColor(COLOR_HIGHLIGHT);
        result.onAccent = GetSysColor(COLOR_HIGHLIGHTTEXT);
        result.successBg = result.header = result.surface; result.danger = result.text;
        result.pressed = result.hover = result.selected;
        result.primaryHover = result.primaryPressed = result.accent;
    }
    return result;
}
bool refresh() {
    HIGHCONTRASTW contrast{sizeof(contrast), 0, nullptr};
    bool high = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON);
    DWORD light = 1, bytes = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes) != ERROR_SUCCESS) light = 1;
    auto next = paletteFor(light == 0, high);
    if (surface && canvas && next == colors) return false;
    HBRUSH nextSurface = CreateSolidBrush(next.surface), nextCanvas = CreateSolidBrush(next.canvas);
    if (!nextSurface || !nextCanvas) {
        if (nextSurface) DeleteObject(nextSurface);
        if (nextCanvas) DeleteObject(nextCanvas);
        throw std::runtime_error(i18n::utf8Text(i18n::Text::ThemeBrushFailed));
    }
    if (surface) DeleteObject(surface);
    if (canvas) DeleteObject(canvas);
    colors = next; surface = nextSurface; canvas = nextCanvas;
    return true;
}
void titleBar(HWND window) {
    BOOL dark = colors.dark && !colors.highContrast;
    // Older Windows versions may reject this attribute; the client palette still works.
    DwmSetWindowAttribute(window, static_cast<DWMWINDOWATTRIBUTE>(20), &dark, sizeof(dark));
}
void initialize() {
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) throw std::runtime_error(i18n::utf8Text(i18n::Text::DrawingInitFailed));
    refresh();
}
void shutdown() {
    if (surface) DeleteObject(surface);
    if (canvas) DeleteObject(canvas);
    surface = canvas = nullptr;
    if (token) Gdiplus::GdiplusShutdown(token);
    token = 0;
}
HBRUSH surfaceBrush() { return surface; }
HBRUSH canvasBrush() { return canvas; }
void fill(HDC dc, const RECT& rect, COLORREF value) {
    HBRUSH brush = CreateSolidBrush(value); FillRect(dc, &rect, brush); DeleteObject(brush);
}
void rounded(HDC dc, const RECT& rect, COLORREF background, COLORREF edge, int radius, int stroke) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float inset = stroke / 2.0f;
    float x = rect.left + inset, y = rect.top + inset;
    float w = rect.right - rect.left - static_cast<float>(stroke) - 1, h = rect.bottom - rect.top - static_cast<float>(stroke) - 1;
    float d = static_cast<float>((std::min)(radius * 2, (std::min)(static_cast<int>(w), static_cast<int>(h))));
    Gdiplus::GraphicsPath path;
    if (d > 0) {
        path.AddArc(x, y, d, d, 180, 90); path.AddArc(x + w - d, y, d, d, 270, 90);
        path.AddArc(x + w - d, y + h - d, d, d, 0, 90); path.AddArc(x, y + h - d, d, d, 90, 90); path.CloseFigure();
    } else path.AddRectangle(Gdiplus::RectF(x, y, w, h));
    Gdiplus::SolidBrush brush(color(background)); g.FillPath(&brush, &path);
    if (stroke > 0) { Gdiplus::Pen pen(color(edge), static_cast<float>(stroke)); g.DrawPath(&pen, &path); }
}
static void drawButton(HDC dc, RECT rect, const wchar_t* caption, HFONT font, Button style,
    bool enabled, bool hot, bool pressed, bool focused, bool selected, UINT dpi, COLORREF background, int glyph) {
    fill(dc, rect, background);
    COLORREF face = colors.surface, ink = colors.accent, edge = colors.border;
    if (style == Button::Navigation) {
        face = selected ? colors.selected : hot ? colors.hover : background;
        ink = colors.highContrast && (selected || hot) ? colors.onAccent : selected ? colors.accent : colors.text;
        edge = face;
    } else if (!enabled) {
        face = style == Button::Primary ? colors.disabled : background;
        ink = colors.highContrast ? colors.muted : blend(background, colors.muted, 55);
        edge = colors.highContrast ? colors.muted : blend(background, colors.border, 55);
    } else if (style == Button::Primary) {
        face = colors.accent; ink = colors.onAccent; edge = face;
        if (hot || pressed) face = edge = pressed ? colors.primaryPressed : colors.primaryHover;
    } else {
        face = pressed ? colors.pressed : hot ? colors.hover : background;
        ink = style == Button::Danger ? colors.danger : colors.text;
        edge = style == Button::Danger ? blend(background, colors.danger, hot || pressed ? 65 : 35) :
            hot || pressed ? blend(colors.border, colors.text, 25) : colors.border;
        if (colors.highContrast) {
            ink = hot || pressed ? colors.onAccent : colors.text;
            edge = ink;
        } else if (style == Button::Secondary) {
            // Use a tonal surface and a quiet hairline, not a dark outline on
            // white. Hover feedback comes mainly from the fill and blue label.
            face = blend(background, pressed ? colors.pressed : colors.hover, pressed ? 75 : hot ? 60 : 18);
            edge = blend(background, colors.border, pressed ? 100 : hot ? 80 : 60);
            ink = colors.accent;
        }
    }
    InflateRect(&rect, -scaled(1, dpi), -scaled(1, dpi));
    // Secondary/danger actions keep a quiet outline at rest; primary actions
    // and navigation keep their filled shapes without an extra border.
    int radius = style == Button::Navigation ? scaled(20, dpi) : (rect.bottom - rect.top) / 2;
    int stroke = style == Button::Secondary || style == Button::Danger ? (std::max)(1, scaled(1, dpi)) : 0;
    // Keep secondary outlines visually light when the window/fonts grow; retain
    // the stronger DPI-scaled stroke for system high-contrast mode.
    if (style == Button::Secondary && !colors.highContrast) stroke = 1;
    if (style == Button::Navigation) {
        // Extend the rounded shape beyond a clip at its left edge, leaving
        // square left corners and the existing rounded right end. No outline.
        int saved = SaveDC(dc);
        IntersectClipRect(dc, rect.left, rect.top, rect.right, rect.bottom);
        RECT shape = rect; shape.left -= radius * 2;
        rounded(dc, shape, face, edge, radius, 0);
        RestoreDC(dc, saved);
    } else rounded(dc, rect, face, edge, radius, stroke);
    RECT label = rect;
    UINT flags = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
    if (style == Button::Navigation) {
        if (glyph >= 0) icon(dc, glyph, rect.left + scaled(14, dpi), rect.top + (rect.bottom - rect.top - scaled(22, dpi)) / 2, scaled(22, dpi), ink);
        label.left += scaled(48, dpi); label.right -= scaled(10, dpi); flags |= DT_LEFT;
    } else { InflateRect(&label, -scaled(16, dpi), 0); flags |= DT_CENTER; }
    auto old = SelectObject(dc, font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, ink);
    DrawTextW(dc, caption, -1, &label, flags); SelectObject(dc, old);
    if (focused && style == Button::Navigation) {
        RECT marker{label.left, rect.bottom - scaled(7, dpi), label.left + scaled(24, dpi), rect.bottom - scaled(5, dpi)};
        rounded(dc, marker, ink, ink, scaled(1, dpi), 0);
    } else if (focused && enabled) {
        // Focus remains an underline, separate from the normal button outline.
        int width = scaled(24, dpi), center = (rect.left + rect.right) / 2;
        RECT marker{center - width / 2, rect.bottom - scaled(5, dpi), center + width / 2, rect.bottom - scaled(3, dpi)};
        fill(dc, marker, ink);
    }
}
void button(HDC target, RECT rect, const wchar_t* caption, HFONT font, Button style,
    bool enabled, bool hot, bool pressed, bool focused, bool selected, UINT dpi, COLORREF background, int glyph) {
    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = dc ? CreateCompatibleBitmap(target, width, height) : nullptr;
    if (!bitmap) {
        if (dc) DeleteDC(dc);
        drawButton(target, rect, caption, font, style, enabled, hot, pressed, focused, selected, dpi, background, glyph);
        return;
    }
    auto old = SelectObject(dc, bitmap);
    SetWindowOrgEx(dc, rect.left, rect.top, nullptr);
    drawButton(dc, rect, caption, font, style, enabled, hot, pressed, focused, selected, dpi, background, glyph);
    BitBlt(target, rect.left, rect.top, width, height, dc, rect.left, rect.top, SRCCOPY);
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
}
void listRow(HWND list, HDC dc, int row, HFONT font, UINT dpi, bool hot,
    int actionColumn, int semanticColumn, COLORREF semanticInk) {
    RECT bounds{}, client{};
    if (!ListView_GetItemRect(list, row, &bounds, LVIR_BOUNDS)) return;
    GetClientRect(list, &client);
    UINT state = ListView_GetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED);
    bool selected = (state & LVIS_SELECTED) != 0;
    bool highlighted = selected || hot;
    COLORREF face = selected ? colors.selected : hot ? colors.hover : colors.surface;
    COLORREF ink = colors.highContrast && highlighted ? colors.onAccent : colors.text;
    int saved = SaveDC(dc);
    IntersectClipRect(dc, client.left, bounds.top, client.right, bounds.bottom);
    RECT background{client.left, bounds.top, client.right, bounds.bottom};
    fill(dc, background, face);
    SelectObject(dc, font); SetBkMode(dc, TRANSPARENT);
    int count = Header_GetItemCount(ListView_GetHeader(list));
    for (int column = 0; column < count; ++column) {
        if (column == actionColumn) continue;
        RECT cell{}; ListView_GetSubItemRect(list, row, column, LVIR_BOUNDS, &cell);
        // Subitem zero's LVIR_BOUNDS covers the whole row.
        if (column == 0) cell.right = cell.left + ListView_GetColumnWidth(list, 0);
        cell.left += scaled(12, dpi); cell.right -= scaled(12, dpi);
        if (cell.right <= cell.left) continue;
        wchar_t text[4096]{}; ListView_GetItemText(list, row, column, text, 4096);
        LVCOLUMNW info{}; info.mask = LVCF_FMT; ListView_GetColumn(list, column, &info);
        UINT align = (info.fmt & LVCFMT_JUSTIFYMASK) == LVCFMT_RIGHT ? DT_RIGHT :
            (info.fmt & LVCFMT_JUSTIFYMASK) == LVCFMT_CENTER ? DT_CENTER : DT_LEFT;
        SetTextColor(dc, column == semanticColumn && semanticInk != CLR_INVALID && !colors.highContrast ? semanticInk : ink);
        DrawTextW(dc, text, -1, &cell, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | align);
    }
    if ((state & LVIS_FOCUSED) && GetFocus() == list &&
        !(SendMessageW(list, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        RECT marker{client.left, bounds.top + scaled(8, dpi), client.left + (std::max)(1, scaled(2, dpi)), bounds.bottom - scaled(8, dpi)};
        fill(dc, marker, colors.highContrast && highlighted ? colors.onAccent : colors.accent);
    }
    RestoreDC(dc, saved);
}
}
