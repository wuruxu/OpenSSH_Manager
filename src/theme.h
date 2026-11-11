#pragma once
#include <windows.h>
#include <tuple>

namespace theme {
enum class Button { Secondary, Primary, Danger, Navigation };
struct Palette {
    COLORREF canvas, surface, text, muted, border, accent, onAccent, selected, hover, disabled, success, successBg;
    bool highContrast = false;
    COLORREF header, danger, pressed, primaryHover, primaryPressed;
    bool dark = false;
    bool operator==(const Palette& other) const {
        return std::tie(canvas, surface, text, muted, border, accent, onAccent, selected, hover, disabled, success, successBg,
            highContrast, header, danger, pressed, primaryHover, primaryPressed, dark) ==
            std::tie(other.canvas, other.surface, other.text, other.muted, other.border, other.accent, other.onAccent,
            other.selected, other.hover, other.disabled, other.success, other.successBg, other.highContrast,
            other.header, other.danger, other.pressed, other.primaryHover, other.primaryPressed, other.dark);
    }
};
extern Palette colors;
void initialize();
Palette paletteFor(bool dark, bool highContrast);
bool refresh();
void titleBar(HWND window);
void styleControl(HWND window);
void comboItem(const DRAWITEMSTRUCT& draw);
void shutdown();
HBRUSH surfaceBrush();
HBRUSH canvasBrush();
void fill(HDC dc, const RECT& rect, COLORREF color);
void rounded(HDC dc, const RECT& rect, COLORREF fill, COLORREF border, int radius, int stroke = 1);
void button(HDC dc, RECT rect, const wchar_t* text, HFONT font, Button style,
    bool enabled, bool hot, bool pressed, bool focused, bool selected, UINT dpi, COLORREF background, int icon = -1);
void listRow(HWND list, HDC dc, int row, HFONT font, UINT dpi, bool hot,
    int actionColumn = -1, int semanticColumn = -1, COLORREF semanticInk = CLR_INVALID);
}
