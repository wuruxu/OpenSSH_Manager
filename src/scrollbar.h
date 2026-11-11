#pragma once
#include <windows.h>
#include "scroll_math.h"
namespace capsule {
constexpr UINT positionChanged = WM_APP + 42;
constexpr int horizontalId = 0x6001, verticalId = 0x6002;
HWND create(HWND parent, bool vertical, int id, bool canvas);
int thickness(UINT dpi, bool vertical);
scrollmath::Range range(HWND bar);
void configure(HWND bar, const scrollmath::Range& range);
int action(HWND bar, UINT code, int line = 28);
// Native edit/list controls retain their own scrolling engine and accessibility.
// Edit controls use a clipping host. Lists sit below an independent header;
// gutter overlays replace scrollbar artwork without feeding back into sizing.
HWND host(HWND owner);
void attach(HWND host, HWND child, bool listView, bool canvas);
HWND outer(HWND child);
HWND header(HWND child);
// Manual header sizing takes precedence over automatic column fitting.
bool hasCustomColumns(HWND child);
void refresh(HWND child);
// Batch data/column updates for both native windows, including exception paths.
class ListUpdate {
    HWND list_, header_;
public:
    explicit ListUpdate(HWND list);
    ~ListUpdate();
    ListUpdate(const ListUpdate&) = delete;
    ListUpdate& operator=(const ListUpdate&) = delete;
};
}
