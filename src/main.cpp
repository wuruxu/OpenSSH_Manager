#include "platform.h"
#include "build_info.h"
#include "bootstrap.h"
#include "setup_action.h"
#include "theme.h"
#include "scrollbar.h"
#include "layout_math.h"
#include "key_channel.h"
#include "key_popup.h"
#include <windowsx.h>
#include <process.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <commdlg.h>
#include <objbase.h>
#include <algorithm>
#include <stdexcept>
#include <vector>

using namespace manager;
namespace {
constexpr wchar_t windowClass[] = L"OpenSSHManager.Window.1";
constexpr int baseWidth = layoutmath::designWidth, baseHeight = layoutmath::designHeight;
constexpr int contentTop = 132, startupOptionGap = 22;
constexpr UINT trayMessage = WM_APP + 1, applyThemeMessage = WM_APP + 72, setupMessage = WM_APP + 73;
enum Control { Navigation = 100, Start, Stop, Restart, Auto, Reload, Validate, Apply, Port, Listen,
    Password, Pubkey, Forwarding, Users, Groups, Shell, SetShell, Events, OpenFolder,
    AutoTray, Show, Exit, Firewall, KeyTarget, KeyAdd, KeyRemove, EventList = 127, KeyList, KeyRefresh, SetupOffline = 131, SetupCancel };
enum Anchor { Fixed = 0, GrowX = 1, MoveX = 2, GrowY = 4, MoveY = 8 };
enum FontRole { Body, Small, Strong, PageTitle };
struct Item {
    HWND window; int page, x, y, width, height;
    int anchor = Fixed; FontRole font = Body;
    bool muted = false, canvas = false, frame = false;
};
struct Card { int page, x, y, width, height; int anchor = GrowX; };
struct App {
    HWND window = nullptr, navigation = nullptr, status = nullptr, details = nullptr, note = nullptr, events = nullptr, startHint = nullptr;
    HWND account = nullptr, accountDetail = nullptr, pageTitle = nullptr, pageSubtitle = nullptr, brand = nullptr;
    HWND keys = nullptr, keyTooltip = nullptr;
    std::vector<AuthorizedKeyEntry> keyEntries;
    std::string keyRevision, keySource;
    std::wstring keyTooltipText;
    KeyChannel keyChannel, setupChannel;
    EngineSetup setup;
    bool setupBusy = false, setupPreparing = false, setupAdmin = false, setupCancelRequested = false, setupReady = false, setupRepairOperation = false;
    SetupActivity setupActivity = SetupActivity::Idle;
    int setupPercent = -1;
    std::wstring setupArchive;
    std::wstring setupMessageText;
    bool keyAdminOperation = false, keyLoaded = false, keysAdministrators = false, keyActionsBusy = false;
    int keyHotRow = -1, keyHotColumn = -1, eventHotRow = -1;
    HWND eventDetail = nullptr, eventHint = nullptr, keyHint = nullptr, hot = nullptr, listenHint = nullptr;
    HWND portLabel = nullptr, listenLabel = nullptr;
    std::vector<NetworkAddress> listenAddresses;
    int viewportW = baseWidth, viewportH = baseHeight;
    UINT uiDpi = 96;
    HDC backgroundDC = nullptr;
    HBITMAP backgroundBitmap = nullptr, backgroundOld = nullptr;
    int backgroundW = 0, backgroundH = 0;
    bool backgroundDirty = true;
    HICON icon = nullptr, smallIcon = nullptr;
    HANDLE eventWorker = nullptr;
    EventBatch pendingEvents, eventData;
    HFONT font = nullptr, small = nullptr, heading = nullptr, title = nullptr;
    HIMAGELIST rowImages = nullptr;
    UINT dpi = 96, taskbarCreated = 0;
    NOTIFYICONDATAW tray{};
    std::vector<Item> items;
    std::vector<Card> cards;
    bool ready = false, layingOut = false, themePending = false;
    ServiceInfo service;
    Config config;
    std::string revision;
    std::wstring statusError, trayTip;
    AccountIdentity identity;
    bool identityKnown = false, keyTargetInitialized = false;
    HANDLE worker = nullptr;
    bool workerAppliesConfig = false;
    bool startupChangePending = false;
    bool dirty = false, loading = false, closing = false, configLoaded = false;
    int page = 0;
} app;
const wchar_t* pageTitles[] = {L"服务概览", L"连接与认证", L"终端与公钥", L"事件日志"};
const wchar_t* pageDescriptions[] = {
    L"管理 SSH 服务、启动方式和桌面托盘。",
    L"设置监听地址、认证方式及允许访问的账号。",
    L"选择新会话的终端，管理用于 SSH 登录的公钥。",
    L"查看登录结果与连接记录，选择事件可查看完整详情。"
};
int scale(int value) { return MulDiv(value, static_cast<int>(app.uiDpi), 96); }
int sx(int value) { return layoutmath::Viewport{app.viewportW, app.viewportH}.x(value); }
int sy(int value) { return layoutmath::Viewport{app.viewportW, app.viewportH}.y(value); }
HWND get(int id) {
    for (const auto& item : app.items) if (GetDlgCtrlID(item.window) == id) return item.window;
    return GetDlgItem(app.window, id);
}
Item* findItem(HWND window) {
    for (auto& item : app.items) if (item.window == window) return &item;
    return nullptr;
}
HFONT itemFont(FontRole role) {
    return role == Small ? app.small : role == Strong ? app.heading : role == PageTitle ? app.title : app.font;
}
RECT position(int x, int y, int width, int height, int = Fixed) {
    return {sx(x), sy(y), sx(x + width), sy(y + height)};
}
RECT controlPosition(const Item& item) {
    RECT rect = position(item.x, item.y, item.width, item.height, item.anchor);
    // Keep the port text left-aligned with the checkbox above, and measure
    // its text so the visible input border still has exactly the same gap.
    if (GetDlgCtrlID(item.window) == Port && app.portLabel) {
        if (auto* caption = findItem(app.portLabel)) {
            SIZE extent{scale(28), 0};
            if (HDC dc = GetDC(app.window)) {
                auto old = SelectObject(dc, app.font);
                GetTextExtentPoint32W(dc, L"端口", 2, &extent);
                SelectObject(dc, old); ReleaseDC(app.window, dc);
            }
            int left = sx(caption->x) + extent.cx + scale(12) + scale(9);
            rect.right += left - rect.left; rect.left = left;
        }
    } else if (item.window == app.listenLabel) {
        if (auto* field = findItem(get(Listen))) rect.right = sx(field->x) - scale(12);
    }
    return rect;
}
std::wstring text(HWND window) {
    int length = GetWindowTextLengthW(window);
    std::wstring value(static_cast<size_t>(length) + 1, 0);
    GetWindowTextW(window, value.data(), length + 1);
    value.resize(length); return value;
}
void set(int id, const std::wstring& value) { if (text(get(id)) != value) SetWindowTextW(get(id), value.c_str()); }
bool checked(int id) { return SendMessageW(get(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
void check(int id, bool value) { SendMessageW(get(id), BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0); }
void error(const std::exception& e) { MessageBoxW(app.window, wide(e.what()).c_str(), L"OpenSSH Manager", MB_OK | MB_ICONERROR); }
void layout(bool redraw = true);
void moveControls(bool redraw = true);
void loadKeys(bool authorize = false);
void hideKeyTooltip();
void fitKeyColumns();
LRESULT CALLBACK controlProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (message == WM_LBUTTONDOWN) {
        wchar_t cls[16]{}; GetClassNameW(window, cls, 16);
        if (wcscmp(cls, L"Static") == 0) SetFocus(app.window);
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE) {
        if (auto* item = findItem(window); item && item->frame) {
            app.backgroundDirty = true;
            RECT frame = controlPosition(*item); InflateRect(&frame, scale(10), scale(8));
            InvalidateRect(app.window, &frame, FALSE);
        }
    }
    if (message == WM_MOUSEMOVE && (GetWindowLongPtrW(window, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW) {
        wchar_t cls[16]{}; GetClassNameW(window, cls, 16);
        if (wcscmp(cls, L"Button") == 0 && app.hot != window) {
            HWND previous = app.hot; app.hot = window;
            if (previous) InvalidateRect(previous, nullptr, FALSE);
            InvalidateRect(window, nullptr, FALSE);
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking);
        }
    }
    if (message == WM_MOUSELEAVE && app.hot == window) { app.hot = nullptr; InvalidateRect(window, nullptr, FALSE); }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, controlProc, id);
    return DefSubclassProc(window, message, wp, lp);
}
HWND control(const wchar_t* cls, const wchar_t* caption, int id, int page, int x, int y, int width, int height, DWORD style = 0, int anchor = Fixed, bool canvas = false) {
    bool scrolling = (wcscmp(cls, L"EDIT") == 0 && (style & ES_MULTILINE)) || wcscmp(cls, WC_LISTVIEWW) == 0;
    HWND parent = scrolling ? capsule::host(app.window) : app.window;
    HWND window = CreateWindowExW(0, cls, caption,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, scrolling ? 0 : scale(x), scrolling ? 0 : scale(y), scale(width), scale(height), parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (!window) throw std::runtime_error("Could not create native UI control.");
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
    app.items.push_back({window, page, x, y, width, height, anchor});
    app.items.back().canvas = canvas;
    if (scrolling) capsule::attach(parent, window, wcscmp(cls, WC_LISTVIEWW) == 0, canvas);
    SetWindowSubclass(window, controlProc, 1, 0);
    theme::styleControl(window);
    if (wcscmp(cls, L"EDIT") == 0) SendMessageW(window, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(scale(2), scale(2)));
    return window;
}
HWND label(const wchar_t* value, int page, int x, int y, int width, int height = 24,
    FontRole font = Body, bool muted = false, bool canvas = false, int anchor = Fixed) {
    HWND window = control(L"STATIC", value, 0, page, x, y, width, height, SS_NOPREFIX, anchor);
    auto& item = app.items.back(); item.font = font; item.muted = muted; item.canvas = canvas;
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(itemFont(font)), TRUE);
    return window;
}
void button(const wchar_t* value, int id, int page, int x, int y, int width = 120, int anchor = Fixed, bool canvas = false) {
    control(L"BUTTON", value, id, page, x, y, width, 38, WS_TABSTOP | BS_OWNERDRAW, anchor);
    app.items.back().canvas = canvas;
}
void edit(int id, int page, int x, int y, int width, int anchor = Fixed) {
    control(L"EDIT", L"", id, page, x, y, width, 24, WS_TABSTOP | ES_AUTOHSCROLL, anchor);
    app.items.back().frame = true;
    SendMessageW(get(id), EM_SETLIMITTEXT, (id == Port ? 5 : id == Listen ? 64 : 2048), 0);
}
void toggle(const wchar_t* value, int id, int page, int x, int y, int width = 300) {
    control(L"BUTTON", value, id, page, x, y, width, 28, WS_TABSTOP | BS_AUTOCHECKBOX);
}
void card(int page, int x, int y, int width, int height, int anchor = GrowX) { app.cards.push_back({page, x, y, width, height, anchor}); }
bool pageVisible(const Item& item, int page) {
    if (item.page != page) return false;
    int id = GetDlgCtrlID(item.window);
    if (id == Stop || id == Restart) return app.service.installed && !app.setupBusy && !app.service.engineMissing;
    if (id == SetupOffline) return (!app.service.installed || app.service.repairAllowed) && app.statusError.empty();
    if (id == SetupCancel) return app.setupActivity == SetupActivity::Preparing;
    return true;
}
PrimaryAction currentPrimaryAction() {
    return primaryAction(app.statusError.empty(), app.service.installed, app.setupReady, app.setupActivity, app.service.engineMissing, app.service.repairAllowed);
}
std::wstring primaryCaption() {
    switch (currentPrimaryAction()) {
    case PrimaryAction::Checking: return L"检测中…";
    case PrimaryAction::Preparing: return app.setupPercent < 0 ? L"准备中…" : L"下载中 " + std::to_wstring(app.setupPercent) + L"%";
    case PrimaryAction::Installing: return L"安装中…";
    case PrimaryAction::Download: return L"下载引擎";
    case PrimaryAction::Install: return L"安装服务";
    case PrimaryAction::Repair: return L"修复安装";
    case PrimaryAction::Start: return L"启动服务";
    default: return app.service.engineMissing ? L"引擎异常" : L"状态未知";
    }
}
void showPage(int page) {
    if (page < 0 || page > 3) return;
    closeKeyPopup(); hideKeyTooltip(); app.keyHotRow = app.keyHotColumn = app.eventHotRow = -1;
    app.page = page;
    // Finish geometry (including the independent table headers) while the new
    // page is still hidden. Showing first exposed stale/uninitialized positions.
    moveControls(false);
    capsule::refresh(app.keys); capsule::refresh(app.events);
    // Leave the sidebar and persistent controls alone. Batch only page visibility.
    HDWP batch = BeginDeferWindowPos(static_cast<int>(app.items.size()));
    bool failed = !batch;
    for (const auto& item : app.items) {
        if (item.page < 0) continue;
        HWND window = capsule::outer(item.window);
        bool visible = pageVisible(item, page);
        if (((GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0) == visible) continue;
        UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW |
            (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
        if (batch) { batch = DeferWindowPos(batch, window, nullptr, 0, 0, 0, 0, flags); if (!batch) failed = true; }
    }
    if (batch && !EndDeferWindowPos(batch)) failed = true;
    if (failed) for (const auto& item : app.items) if (item.page >= 0)
        SetWindowPos(capsule::outer(item.window), nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | (pageVisible(item, page) ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    if (SendMessageW(app.navigation, LB_GETCURSEL, 0, 0) != page) SendMessageW(app.navigation, LB_SETCURSEL, page, 0);
    SetWindowTextW(app.pageTitle, pageTitles[page]);
    SetWindowTextW(app.pageSubtitle, pageDescriptions[page]);
    app.backgroundDirty = true;
    RECT content{sx(196), sy(110), app.viewportW, app.viewportH};
    RedrawWindow(app.window, &content, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
void fonts() {
    HFONT old[] = {app.font, app.small, app.heading, app.title};
    auto make = [](int size, int weight) {
        return CreateFontW(-scale(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    };
    app.font = make(14, FW_NORMAL); app.small = make(12, FW_NORMAL);
    app.heading = make(16, FW_SEMIBOLD); app.title = make(24, FW_SEMIBOLD);
    for (const auto& item : app.items) SendMessageW(item.window, WM_SETFONT, reinterpret_cast<WPARAM>(itemFont(item.font)), FALSE);
    for (int id : {Shell, KeyTarget, Listen}) if (get(id)) {
        SendMessageW(get(id), CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), sy(26));
        SendMessageW(get(id), CB_SETITEMHEIGHT, 0, sy(28));
    }
    // The compact selector shares its heading row; keep full target paths
    // readable in the expanded list at every font scale.
    if (get(KeyTarget)) SendMessageW(get(KeyTarget), CB_SETDROPPEDWIDTH, scale(620), 0);
    if (app.events) {
        auto images = ImageList_Create(1, scale(36), ILC_COLOR32, 1, 1);
        ListView_SetImageList(app.events, images, LVSIL_SMALL);
        if (app.keys) ListView_SetImageList(app.keys, images, LVSIL_SMALL);
        if (app.rowImages) ImageList_Destroy(app.rowImages);
        app.rowImages = images;
        SendMessageW(ListView_GetHeader(app.events), WM_SETFONT, reinterpret_cast<WPARAM>(app.small), FALSE);
    }
    if (app.keys) SendMessageW(ListView_GetHeader(app.keys), WM_SETFONT, reinterpret_cast<WPARAM>(app.small), FALSE);
    for (HWND list : {app.events, app.keys}) if (list && capsule::header(list))
        SendMessageW(capsule::header(list), WM_SETFONT, reinterpret_cast<WPARAM>(app.small), FALSE);
    for (auto font : old) if (font) DeleteObject(font);
}
void moveControls(bool redraw) {
    HDWP batch = BeginDeferWindowPos(static_cast<int>(app.items.size()));
    bool fallback = !batch;
    for (const auto& item : app.items) {
        HWND window = capsule::outer(item.window);
        RECT rect = controlPosition(item), previous{};
        GetWindowRect(window, &previous); MapWindowPoints(nullptr, app.window, reinterpret_cast<POINT*>(&previous), 2);
        if (EqualRect(&rect, &previous)) continue;
        UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS;
        if (rect.right - rect.left == previous.right - previous.left && rect.bottom - rect.top == previous.bottom - previous.top) flags |= SWP_NOSIZE;
        if (batch) {
            batch = DeferWindowPos(batch, window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, flags);
            if (!batch) fallback = true;
        }
    }
    if (batch && !EndDeferWindowPos(batch)) fallback = true;
    if (fallback) for (const auto& item : app.items) {
        RECT rect = controlPosition(item);
        SetWindowPos(capsule::outer(item.window), nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS);
    }
    if (app.navigation) {
        // Independently rounded row heights can exceed the scaled ListBox by
        // a pixel, making selection of the last row scroll the first one away.
        RECT client{}; GetClientRect(app.navigation, &client);
        int count = static_cast<int>(SendMessageW(app.navigation, LB_GETCOUNT, 0, 0));
        if (count > 0) {
            int height = (std::max)(1, static_cast<int>(client.bottom - client.top) / count);
            if (SendMessageW(app.navigation, LB_GETITEMHEIGHT, 0, 0) != height)
                SendMessageW(app.navigation, LB_SETITEMHEIGHT, 0, height);
            if (SendMessageW(app.navigation, LB_GETTOPINDEX, 0, 0) != 0)
                SendMessageW(app.navigation, LB_SETTOPINDEX, 0, 0);
        }
    }
    if (redraw) RedrawWindow(app.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
void layout(bool redraw) {
    if (!app.window || app.layingOut) return;
    RECT client{}; GetClientRect(app.window, &client);
    if (client.right <= 0 || client.bottom <= 0) return;
    app.layingOut = true;
    app.viewportW = client.right; app.viewportH = client.bottom;
    UINT nextDpi = layoutmath::Viewport{app.viewportW, app.viewportH}.fontDpi();
    if (app.uiDpi != nextDpi) { app.uiDpi = nextDpi; fonts(); }
    for (int id : {Shell, KeyTarget, Listen}) if (get(id) && SendMessageW(get(id), CB_GETITEMHEIGHT, static_cast<WPARAM>(-1), 0) != sy(26))
        SendMessageW(get(id), CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), sy(26));
    moveControls(false);
    const int columns[] = {150, 54, 105, 132, 350};
    if (app.events && !capsule::hasCustomColumns(app.events)) for (int i = 0; i < 5; ++i) if (ListView_GetColumnWidth(app.events, i) != sx(columns[i]))
        ListView_SetColumnWidth(app.events, i, sx(columns[i]));
    fitKeyColumns(); capsule::refresh(app.keys); capsule::refresh(app.events);
    app.backgroundDirty = true; app.layingOut = false;
    if (redraw) RedrawWindow(app.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
void paintScene(HDC dc) {
    const auto& c = theme::colors;
    RECT client{0, 0, app.viewportW, app.viewportH}; theme::fill(dc, client, c.canvas);
    theme::fill(dc, position(0, 0, 196, baseHeight, GrowY), c.surface);
    theme::fill(dc, position(196, 0, baseWidth - 196, 110, GrowX), c.surface);
    theme::fill(dc, position(195, 0, 1, baseHeight, GrowY), c.border);
    theme::fill(dc, position(196, 109, baseWidth - 196, 1, GrowX), c.border);
    for (const auto& panel : app.cards) if (panel.page == app.page)
        theme::rounded(dc, position(panel.x, panel.y, panel.width, panel.height, panel.anchor), c.surface, c.border, scale(12), scale(1));
    for (const auto& item : app.items) if (item.frame && item.page == app.page) {
        RECT rect = controlPosition(item);
        InflateRect(&rect, scale(9), scale(7));
        theme::rounded(dc, rect, c.surface, GetFocus() == item.window ? c.accent : c.border, scale(7), scale(GetFocus() == item.window ? 2 : 1));
    }
}
void releaseBackground() {
    if (app.backgroundDC) {
        SelectObject(app.backgroundDC, app.backgroundOld); DeleteObject(app.backgroundBitmap); DeleteDC(app.backgroundDC);
    }
    app.backgroundDC = nullptr; app.backgroundBitmap = app.backgroundOld = nullptr;
}
void paintBackground(HDC dc) {
    int width = app.viewportW, height = app.viewportH;
    if (!app.backgroundDC || width != app.backgroundW || height != app.backgroundH) {
        releaseBackground(); app.backgroundDC = CreateCompatibleDC(dc); app.backgroundBitmap = CreateCompatibleBitmap(dc, width, height);
        if (!app.backgroundDC || !app.backgroundBitmap) {
            if (app.backgroundDC) DeleteDC(app.backgroundDC);
            if (app.backgroundBitmap) DeleteObject(app.backgroundBitmap);
            app.backgroundDC = nullptr; app.backgroundBitmap = nullptr; paintScene(dc); return;
        }
        app.backgroundOld = static_cast<HBITMAP>(SelectObject(app.backgroundDC, app.backgroundBitmap));
        app.backgroundW = width; app.backgroundH = height; app.backgroundDirty = true;
    }
    if (app.backgroundDirty) {
        paintScene(app.backgroundDC); app.backgroundDirty = false;
    }
    RECT area{}; GetClientRect(app.window, &area); theme::fill(dc, area, theme::colors.canvas);
    BitBlt(dc, 0, 0, app.viewportW, app.viewportH, app.backgroundDC, 0, 0, SRCCOPY);
}
void loadAppIcons() {
    auto icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, MulDiv(64, app.dpi, 96), MulDiv(64, app.dpi, 96), 0));
    int smallSize = GetSystemMetricsForDpi(SM_CXSMICON, app.dpi);
    auto small = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, smallSize, smallSize, 0));
    if (!icon || !small) {
        if (icon) DestroyIcon(icon);
        if (small) DestroyIcon(small);
        throw std::runtime_error("Could not load the application icon.");
    }
    auto old = app.icon, oldSmall = app.smallIcon; app.icon = icon; app.smallIcon = small;
    if (app.window) {
        SendMessageW(app.window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(app.window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
        SetClassLongPtrW(app.window, GCLP_HICON, reinterpret_cast<LONG_PTR>(icon));
        SetClassLongPtrW(app.window, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(small));
        if (app.brand) InvalidateRect(app.brand, nullptr, FALSE);
    }
    if (old) DestroyIcon(old);
    if (oldSmall) DestroyIcon(oldSmall);
}
void drawControl(const DRAWITEMSTRUCT& draw) {
    if (draw.CtlType == ODT_COMBOBOX) { theme::comboItem(draw); return; }
    const auto& c = theme::colors;
    if (draw.hwndItem == app.brand) {
        theme::fill(draw.hDC, draw.rcItem, c.surface);
        int size = (std::min)(draw.rcItem.right - draw.rcItem.left, draw.rcItem.bottom - draw.rcItem.top);
        DrawIconEx(draw.hDC, (draw.rcItem.left + draw.rcItem.right - size) / 2, (draw.rcItem.top + draw.rcItem.bottom - size) / 2,
            app.icon, size, size, 0, nullptr, DI_NORMAL);
    } else if (draw.CtlID == Navigation && draw.itemID < 4) {
        RECT rect = draw.rcItem; InflateRect(&rect, -scale(2), -scale(4));
        theme::fill(draw.hDC, draw.rcItem, c.surface);
        theme::button(draw.hDC, rect, pageTitles[draw.itemID], app.font, theme::Button::Navigation, true, false, false,
            (draw.itemState & ODS_FOCUS) != 0 && !(draw.itemState & ODS_NOFOCUSRECT), (draw.itemState & ODS_SELECTED) != 0, app.uiDpi, c.surface, static_cast<int>(draw.itemID));
    } else if (draw.hwndItem == app.status) {
        bool good = app.statusError.empty() && app.service.installed && app.service.state == SERVICE_RUNNING && !app.worker;
        COLORREF ink = good ? c.success : c.muted, face = good ? c.successBg : c.canvas;
        theme::fill(draw.hDC, draw.rcItem, c.surface);
        RECT pill = draw.rcItem; InflateRect(&pill, 0, -scale(3));
        theme::rounded(draw.hDC, pill, face, face, scale(20), 0);
        RECT dot{pill.left + scale(14), (pill.top + pill.bottom) / 2 - scale(4), pill.left + scale(22), (pill.top + pill.bottom) / 2 + scale(4)};
        theme::rounded(draw.hDC, dot, ink, ink, scale(4), 0);
        RECT caption = pill; caption.left += scale(32); caption.right -= scale(10);
        auto old = SelectObject(draw.hDC, app.font); SetBkMode(draw.hDC, TRANSPARENT); SetTextColor(draw.hDC, ink);
        DrawTextW(draw.hDC, text(app.status).c_str(), -1, &caption, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX); SelectObject(draw.hDC, old);
    } else if (draw.CtlType == ODT_BUTTON) {
        auto style = draw.CtlID == Start || draw.CtlID == Apply ? theme::Button::Primary :
            draw.CtlID == Stop || draw.CtlID == KeyRemove ? theme::Button::Danger : theme::Button::Secondary;
        const auto* item = findItem(draw.hwndItem);
        theme::button(draw.hDC, draw.rcItem, text(draw.hwndItem).c_str(), app.font, style,
            !(draw.itemState & ODS_DISABLED), app.hot == draw.hwndItem, (draw.itemState & ODS_SELECTED) != 0,
            (draw.itemState & ODS_FOCUS) != 0 && !(draw.itemState & ODS_NOFOCUSRECT), false, app.uiDpi,
            item && item->canvas ? c.canvas : c.surface);
    }
}
void trayIcon(bool add) {
    app.tray.cbSize = sizeof(app.tray); app.tray.hWnd = app.window; app.tray.uID = 1;
    app.tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    app.tray.uCallbackMessage = trayMessage;
    const bool sameIcon = app.tray.hIcon == app.smallIcon;
    app.tray.hIcon = app.smallIcon;
    auto tip = L"OpenSSH — " + (!app.statusError.empty() ? std::wstring(L"无法读取状态") : app.service.installed ? stateName(app.service.state) : std::wstring(L"未安装"));
    if (!add && tip == app.trayTip && sameIcon) return;
    app.trayTip = tip;
    wcsncpy_s(app.tray.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &app.tray);
    app.tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &app.tray);
}
bool autorun() {
    DWORD size = 0;
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"OpenSSHManager", RRF_RT_REG_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS;
}
void setAutorun(bool enabled) {
    HKEY key = nullptr;
    auto code = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (code != ERROR_SUCCESS) throw std::runtime_error(utf8(errorText(code)));
    auto command = quoteArgument(modulePath()) + L" --tray";
    code = enabled ? RegSetValueExW(key, L"OpenSSHManager", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * 2))
        : RegDeleteValueW(key, L"OpenSSHManager");
    RegCloseKey(key);
    if (code != ERROR_SUCCESS && code != ERROR_FILE_NOT_FOUND) throw std::runtime_error(utf8(errorText(code)));
}
std::wstring startDisabledReason() {
    if (app.setupBusy) return L"正在准备或安装 SSH 引擎，请等待完成。下载阶段可以取消。";
    if (app.worker) return L"正在执行管理操作，请等待 UAC 或操作窗口完成。";
    if (!app.statusError.empty()) return L"无法读取服务状态。上方显示具体错误；修复后点击“重新加载”。";
    if (!app.service.installed) return L"sshd 尚未安装，请先下载引擎并安装服务。";
    if (!app.service.problem.empty()) return L"启动不可用：服务注册方式暂不受支持。请查看上方的路径和具体原因。";
    if (app.service.state == SERVICE_RUNNING) return L"服务已经运行，“启动服务”无需再次点击。可以使用“停止服务”或“重启服务”。";
    if (app.service.startType == SERVICE_DISABLED) return L"启动不可用：服务已被禁用。请先勾选“SSH 服务开机启动”。";
    if (app.service.state != SERVICE_STOPPED) return L"启动不可用：服务当前为“" + stateName(app.service.state) + L"”，请等待状态转换或检查 Windows 服务管理器。";
    return {};
}
void enable(HWND window, bool enabled) {
    if (window && (IsWindowEnabled(window) != FALSE) != enabled) EnableWindow(window, enabled);
}
bool needsEngineSetup() {
    return app.statusError.empty() && (!app.service.installed || app.service.repairAllowed);
}
void enableControls() {
    bool available = !app.worker && !app.setupBusy && app.statusError.empty() && app.service.installed && app.service.problem.empty();
    // Each control gets its final state once; enabling then disabling service
    // buttons on every status tick produces visible flashes and focus churn.
    for (int id : {Auto, Validate, SetShell, Firewall, KeyAdd}) enable(get(id), available);
    bool canInstall = !app.worker && !app.setupBusy && needsEngineSetup();
    enable(get(SetupOffline), canInstall);
    enable(get(SetupCancel), app.setupPreparing && !app.setupCancelRequested);
    for (int id : {Stop, Restart, SetupOffline, SetupCancel}) {
        HWND control = get(id);
        const auto* item = findItem(control);
        bool visible = item && pageVisible(*item, app.page);
        if (control && ((GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0) != visible)
            ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
    }
    auto reason = startDisabledReason();
    auto action = currentPrimaryAction();
    auto caption = primaryCaption();
    if (text(get(Start)) != caption) SetWindowTextW(get(Start), caption.c_str());
    enable(get(Start), !app.worker && (action == PrimaryAction::Download || action == PrimaryAction::Install || action == PrimaryAction::Repair ||
        (action == PrimaryAction::Start && reason.empty())));
    enable(get(Stop), available && app.service.state == SERVICE_RUNNING);
    enable(get(Restart), available && app.service.state == SERVICE_RUNNING && app.service.startType != SERVICE_DISABLED);
    enable(get(Apply), available && app.configLoaded && app.config.problem.empty() && app.dirty);
    for (int id : {Port, Listen, Password, Pubkey, Forwarding, Users, Groups})
        enable(get(id), available && app.configLoaded && app.config.problem.empty());
    enable(get(Reload), !app.worker && !app.setupBusy);
    enable(get(KeyTarget), !app.worker && !app.setupBusy);
    enable(get(KeyRefresh), !app.worker && !app.setupBusy);
    if (app.keyActionsBusy != (app.worker != nullptr)) {
        app.keyActionsBusy = app.worker != nullptr;
        if (app.keys) InvalidateRect(app.keys, nullptr, FALSE);
    }
    if (app.service.repairAllowed && !app.setupBusy && !app.worker && app.statusError.empty())
        reason = app.setupReady ? L"引擎已准备好。点击“修复安装”恢复本程序的引擎，保留配置、密钥和服务启动类型。" :
            L"本程序引擎缺失。请下载引擎或选择离线包，再点击“修复安装”。";
    else if (!app.service.installed && !app.setupBusy && !app.worker && app.statusError.empty())
        reason = app.setupReady ? L"引擎已准备好。点击“安装服务”后请求 UAC，安装并启动服务。" :
            L"点击“下载引擎”，或选择离线包。下载完成后需点击“安装服务”。";
    auto hint = reason.empty() ? std::wstring(L"服务已停止，可以点击“启动服务”；需要权限时会弹出 UAC。") : reason;
    if (app.startHint && text(app.startHint) != hint) SetWindowTextW(app.startHint, hint.c_str());
}
void refreshStatus() {
    bool previouslyRepairable = app.service.repairAllowed;
    try { app.service = serviceInfo(); app.statusError.clear(); }
    catch (const std::exception& e) { app.service = {}; app.statusError = wide(e.what()); }
    if (app.service.repairAllowed && app.setupArchive.empty()) app.setupReady = false;
    if (app.service.repairAllowed && !previouslyRepairable && !app.setupBusy)
        PostMessageW(app.window, setupMessage, 0, 0);
    std::wstring title = app.setupBusy ? L"SSH · " + primaryCaption() : app.worker ? L"正在操作…" : !app.statusError.empty() ? L"状态读取失败" : app.service.engineMissing ? L"SSH · 引擎缺失" : app.service.installed ? L"SSH · " + stateName(app.service.state) : L"SSH · 未安装";
    if (text(app.status) != title) SetWindowTextW(app.status, title.c_str());
    std::wstring detail = app.service.installed ?
        L"服务名：sshd\r\nsshd 版本：" + (app.service.version.empty() ? std::wstring(L"未知（无法读取文件版本）") : app.service.version) + L"\r\n启动方式：" + std::wstring(app.service.startType == SERVICE_AUTO_START ? L"自动" : app.service.startType == SERVICE_DISABLED ? L"已禁用" : L"手动") +
        L"    进程 ID：" + std::to_wstring(app.service.pid) + L"\r\n引擎：" + app.service.exe + L"\r\n配置：" + app.service.config :
        L"尚未注册名为 sshd 的 Windows 服务。";
    if (!app.setupMessageText.empty()) detail += L"\r\n" + app.setupMessageText;
    if (!app.statusError.empty()) detail = app.statusError;
    if (!app.service.problem.empty()) detail += L"\r\n" + app.service.problem;
    // Do not reset selection/scroll every second while the user copies diagnostics.
    if (text(app.details) != detail) SetWindowTextW(app.details, detail.c_str());
    bool automatic = app.service.installed && app.service.startType == SERVICE_AUTO_START;
    if (!app.startupChangePending && checked(Auto) != automatic) check(Auto, automatic);
    trayIcon(false); enableControls();
}
void updateKeyHint() {
    if (!app.keyHint) return;
    bool administrators = SendMessageW(get(KeyTarget), CB_GETCURSEL, 0, 0) == 1;
    std::wstring hint;
    try { hint = L"写入位置：" + publicKeyFilePath(administrators) + L"\r\n"; }
    catch (const std::exception& e) { hint = wide(e.what()) + L"\r\n"; }
    if (administrators) hint += L"管理员组共用文件，默认配置下适用于管理员账号，并非个人专用。";
    else if (app.identityKnown && app.identity.administrator) hint += L"当前账号属于管理员组：默认配置不会读取此个人文件，请选管理员组共用。";
    else hint += L"仅管理当前界面账号的个人公钥文件，其他登录账号需要自己的授权文件。";
    hint += L"\r\n导入后无需重启；客户端需使用配套私钥。自定义 Match / 公钥路径请另行核对。";
    if (text(app.keyHint) != hint) SetWindowTextW(app.keyHint, hint.c_str());
}
void refreshIdentity() {
    try {
        app.identity = currentAccountIdentity();
        app.identityKnown = true;
        auto title = app.identity.name + L"（" + (app.identity.administrator ? L"管理员" : L"普通用户") +
            L" · " + (app.identity.elevated ? L"已提权" : L"未提权") + L"）";
        SetWindowTextW(app.account, title.c_str());
        auto detail = L"界面账号：" + app.identity.qualifiedName + L"  ·  公钥应对应实际 SSH 登录账号";
        SetWindowTextW(app.accountDetail, detail.c_str());
        if (!app.keyTargetInitialized) {
            SendMessageW(get(KeyTarget), CB_SETCURSEL, app.identity.administrator ? 1 : 0, 0);
            app.keyTargetInitialized = true;
        }
    } catch (const std::exception& e) {
        app.identityKnown = false;
        SetWindowTextW(app.account, L"账号身份读取失败");
        SetWindowTextW(app.accountDetail, wide(e.what()).c_str());
    }
    updateKeyHint();
    loadKeys();
}
constexpr wchar_t allListenAddresses[] = L"全部网络接口（IPv4 / IPv6）";
std::wstring listenCaption(const NetworkAddress& entry) { return entry.name + L" — " + entry.address; }
std::wstring listenValue() {
    int index = static_cast<int>(SendMessageW(get(Listen), CB_GETCURSEL, 0, 0));
    if (index == 0) return L"";
    if (index > 0 && static_cast<size_t>(index) <= app.listenAddresses.size()) return app.listenAddresses[index - 1].address;
    throw std::runtime_error("Please select a listen address.");
}
void refreshListenAddresses(const std::wstring& value) {
    std::vector<NetworkAddress> entries;
    std::wstring hint = L"选择网络接口地址；地址变化后需重新选择。";
    try { entries = networkAddresses(); }
    catch (...) { hint = L"无法读取网络接口，已保留当前配置；重新展开列表可重试。"; }
    // A saved address may belong to a disconnected adapter. Keep it selectable
    // so merely opening/saving the page never changes the binding to all IPs.
    if (!value.empty() && std::none_of(entries.begin(), entries.end(), [&](const auto& entry) { return entry.address == value; }))
        entries.push_back({L"当前配置（接口不可用）", value});
    struct LoadingGuard {
        bool previous = app.loading;
        LoadingGuard() { app.loading = true; }
        ~LoadingGuard() { app.loading = previous; }
    } loadingGuard;
    // The list is refreshed only on reload/dropdown, not on the service timer.
    SendMessageW(get(Listen), CB_RESETCONTENT, 0, 0);
    app.listenAddresses = std::move(entries);
    SendMessageW(get(Listen), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(allListenAddresses));
    int selected = value.empty() ? 0 : -1;
    for (size_t i = 0; i < app.listenAddresses.size(); ++i) {
        auto caption = listenCaption(app.listenAddresses[i]);
        SendMessageW(get(Listen), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(caption.c_str()));
        if (selected < 0 && app.listenAddresses[i].address == value) selected = static_cast<int>(i + 1);
    }
    SendMessageW(get(Listen), CB_SETCURSEL, selected, 0);
    if (text(app.listenHint) != hint) SetWindowTextW(app.listenHint, hint.c_str());
}
void loadConfig() {
    app.loading = true; app.configLoaded = false;
    refreshIdentity();
    try {
        refreshStatus();
        if (app.service.installed && app.service.problem.empty()) {
            auto bytes = readFile(app.service.config);
            app.revision = digest(bytes); app.config = parseConfig(bytes); app.configLoaded = true;
            auto& s = app.config.settings;
            set(Port, wide(s.port)); refreshListenAddresses(wide(s.listen)); set(Users, wide(s.users)); set(Groups, wide(s.groups));
            check(Password, s.password); check(Pubkey, s.pubkey); check(Forwarding, s.forwarding);
            SetWindowTextW(app.note, app.config.problem.empty() ?
                (app.config.hasMatch ? L"这里只修改全局默认值。现有 Match 条件规则会保留，特定用户的实际配置可能不同。" : L"保存会校验配置，并在服务运行时重启；可能中断现有 SSH 会话。") : wide(app.config.problem).c_str());
        } else SetWindowTextW(app.note, L"安装并初始化标准 sshd 服务后可编辑配置。");
        auto shell = defaultShell();
        SendMessageW(get(Shell), CB_SETCURSEL, shell.find(L"powershell.exe") != std::wstring::npos ? 1 : 0, 0);
        app.dirty = false;
    } catch (const std::exception& e) { SetWindowTextW(app.note, wide(e.what()).c_str()); }
    app.loading = false; enableControls();
}
void startSetup(const std::wstring& archive = {}, bool download = false) {
    if (app.worker || app.setupBusy) return;
    refreshStatus();
    if (!needsEngineSetup()) return;
    app.setup.start(archive, download);
    app.setupReady = false; app.setupArchive.clear(); app.setupPercent = -1;
    app.setupCancelRequested = false;
    app.setupBusy = app.setupPreparing = true;
    app.setupActivity = download || !archive.empty() ? SetupActivity::Preparing : SetupActivity::Checking;
    app.setupMessageText = L"正在检查本地引擎…";
    refreshStatus();
}
void selectOfflineEngine() {
    if (app.worker || app.setupBusy) return;
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = app.window;
    dialog.lpstrFilter = L"OpenSSH 引擎 ZIP\0*.zip\0\0";
    dialog.lpstrTitle = L"选择本应用支持版本的官方 OpenSSH-Win64.zip";
    dialog.lpstrFile = path; dialog.nMaxFile = 32768;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog)) startSetup(path);
}
void pollSetup() {
    if (!app.setupPreparing) return;
    auto update = app.setup.snapshot();
    app.setupMessageText = update.message; app.setupPercent = update.percent;
    if (update.percent >= 0) app.setupMessageText += L"  " + std::to_wstring(update.percent) + L"%";
    if (update.stage == SetupStage::Preparing) return;
    app.setupPreparing = app.setupBusy = false; app.setupActivity = SetupActivity::Idle;
    if (app.setupCancelRequested) {
        app.setupMessageText = L"已取消准备，可以重新下载或选择离线包。"; return;
    }
    app.setupReady = update.stage == SetupStage::Ready;
    app.setupArchive = app.setupReady ? update.archive : std::wstring{};
    // Preparation ends here. Only an explicit primary-button click may elevate.
}
void installPreparedEngine() {
    if (app.worker || app.setupBusy || !app.setupReady) return;
    refreshStatus();
    if (!needsEngineSetup()) return;
    app.setupRepairOperation = app.service.repairAllowed;
    app.setupBusy = true; app.setupActivity = SetupActivity::Installing;
    try {
        app.setupMessageText = L"等待 Windows UAC 授权…";
        refreshStatus();
        app.setupChannel.open();
        app.workerAppliesConfig = false;
        app.worker = elevate(app.window, {L"--admin", app.setupRepairOperation ? L"engine-repair" : L"engine-install", app.setupArchive, app.setupChannel.token});
        app.setupAdmin = app.worker != nullptr;
        if (!app.worker) {
            app.setupBusy = false; app.setupActivity = SetupActivity::Idle; app.setupChannel.close();
            app.setupMessageText = app.setupRepairOperation ? L"已取消 UAC 授权。点击“修复安装”可继续。" : L"已取消 UAC 授权。点击“安装服务”可继续，无需重新下载。";
        } else app.setupMessageText = L"正在校验签名、安装引擎并启动服务…";
    } catch (const std::exception& e) {
        app.setupBusy = false; app.setupActivity = SetupActivity::Idle;
        app.setupChannel.close(); app.setupMessageText = wide(e.what());
    }
    refreshStatus();
}
void begin(std::vector<std::wstring> args) {
    if (app.worker || app.setupBusy) return;
    app.workerAppliesConfig = !args.empty() && args[0] == L"apply";
    args.insert(args.begin(), L"--admin");
    app.worker = elevate(app.window, args);
    refreshStatus();
}
void applySettings() {
    Settings s;
    s.port = utf8(text(get(Port))); s.listen = trim(utf8(listenValue()));
    s.users = trim(utf8(text(get(Users)))); s.groups = trim(utf8(text(get(Groups))));
    s.password = checked(Password); s.pubkey = checked(Pubkey); s.forwarding = checked(Forwarding);
    validateSettings(s);
    // Windows' own UAC approval is the final authorization; no additional approval loop.
    begin({L"apply", wide(app.revision), wide(s.port), wide(s.listen), s.password ? L"yes" : L"no",
        s.pubkey ? L"yes" : L"no", s.forwarding ? L"yes" : L"no", wide(s.users), wide(s.groups)});
}
bool keyTargetAdmin() { return SendMessageW(get(KeyTarget), CB_GETCURSEL, 0, 0) == 1; }
void hideKeyTooltip() {
    if (!app.keyTooltip) return;
    TOOLINFOW info{}; info.cbSize = sizeof(info); info.hwnd = app.keys; info.uId = 1;
    SendMessageW(app.keyTooltip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&info));
}
void fitKeyColumns() {
    if (!app.keys || capsule::hasCustomColumns(app.keys)) return;
    RECT rect{}; GetClientRect(app.keys, &rect);
    const int widths[] = {(std::max)(scale(100), static_cast<int>(rect.right) - scale(250) - 1), scale(194), scale(56)};
    for (int i = 0; i < 3; ++i) if (ListView_GetColumnWidth(app.keys, i) != widths[i]) ListView_SetColumnWidth(app.keys, i, widths[i]);
}
void displayKeys(const std::string& source, bool administrators) {
    // A refresh with identical data must not rebuild rows or reset selection/scroll.
    if (!app.keyLoaded || app.keysAdministrators != administrators || app.keySource != source) {
        hideKeyTooltip(); app.keyHotRow = app.keyHotColumn = -1;
        auto entries = authorizedKeyEntries(source);
        app.keyEntries = std::move(entries); app.keySource = source; app.keyRevision = digest(source);
        app.keysAdministrators = administrators; app.keyLoaded = true;
        // Redraw is suspended on the child list; page visibility belongs to its host.
        capsule::ListUpdate update(app.keys);
        ListView_DeleteAllItems(app.keys);
        for (size_t i = 0; i < app.keyEntries.size(); ++i) {
            const auto& key = app.keyEntries[i];
            auto caption = key.comment.empty() ? L"未命名公钥 " + std::to_wstring(i + 1) : wide(key.comment);
            auto type = wide(key.type);
            LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = static_cast<int>(i); item.pszText = caption.data();
            ListView_InsertItem(app.keys, &item);
            ListView_SetItemText(app.keys, item.iItem, 1, type.data());
            ListView_SetItemText(app.keys, item.iItem, 2, const_cast<LPWSTR>(L"删除"));
        }
        fitKeyColumns();
    }
    auto hint = L"共 " + std::to_wstring(app.keyEntries.size()) + L" 把公钥 · 悬停查看全文 · 右侧删除\r\n" + publicKeyFilePath(administrators);
    if (!administrators && app.identityKnown && app.identity.administrator)
        hint = L"当前账号属于管理员组，默认配置读取管理员共用文件。\r\n" + publicKeyFilePath(false);
    if (text(app.keyHint) != hint) SetWindowTextW(app.keyHint, hint.c_str());
    set(KeyRefresh, L"刷新列表");
}
bool beginKeyAdmin(std::vector<std::wstring> args) {
    if (app.worker || app.setupBusy) return false;
    app.keyChannel.open(); args.push_back(app.keyChannel.token); args.insert(args.begin(), L"--admin");
    try { app.worker = elevate(keyPopupWindow() ? keyPopupWindow() : app.window, args); }
    catch (...) { app.keyChannel.close(); throw; }
    if (!app.worker) { app.keyChannel.close(); return false; }
    app.keyAdminOperation = true; app.workerAppliesConfig = false;
    SetTimer(app.window, 3, 100, nullptr);
    SetWindowTextW(app.keyHint, L"正在读取或更新管理员组共用公钥…");
    refreshStatus(); return true;
}
void loadKeys(bool authorize) {
    if (!app.keys || app.worker || app.setupBusy) return;
    bool administrators = keyTargetAdmin();
    try { displayKeys(readPublicKeys(administrators), administrators); }
    catch (const std::exception& e) {
        app.keyLoaded = false; app.keyEntries.clear(); app.keyRevision.clear(); hideKeyTooltip();
        ListView_DeleteAllItems(app.keys);
        auto hint = wide(e.what());
        if (administrators) {
            hint = L"读取此文件需要管理员权限，请点击“授权读取”。公钥文件权限保持不变。";
            set(KeyRefresh, L"授权读取");
        }
        SetWindowTextW(app.keyHint, hint.c_str());
        if (authorize && administrators) beginKeyAdmin({L"key-list"});
    }
}
void importKey() {
    if (app.worker || app.setupBusy) return;
    const bool administrators = keyTargetAdmin();
    showKeyPopup(app.window, get(KeyAdd), administrators ? L"写入管理员组共用公钥文件" : L"写入当前 Windows 用户的 authorized_keys", [administrators](const std::string& key) {
        if (administrators) return beginKeyAdmin({L"key-import", wide(key)});
        managePublicKey(key, false, false); loadKeys(); return true;
    });
}
void removeKey(int index) {
    if (app.worker || app.setupBusy || !app.keyLoaded || index < 0 || static_cast<size_t>(index) >= app.keyEntries.size()) return;
    hideKeyTooltip();
    const auto& key = app.keyEntries[index];
    auto prompt = L"删除这条授权公钥？使用此条目的新登录将被拒绝。\r\n\r\n" +
        wide(key.comment.empty() ? key.type : key.comment) + L"\r\n" + publicKeyFilePath(app.keysAdministrators);
    if (MessageBoxW(app.window, prompt.c_str(), L"删除公钥", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES) return;
    if (app.keysAdministrators) beginKeyAdmin({L"key-delete", wide(app.keyRevision), std::to_wstring(index)});
    else { managePublicKey({}, true, false, app.keyRevision, static_cast<size_t>(index)); loadKeys(); }
}
void invalidateListRow(HWND list, int row) {
    RECT rect{}, client{};
    if (row < 0 || !ListView_GetItemRect(list, row, &rect, LVIR_BOUNDS)) return;
    GetClientRect(list, &client); rect.left = 0; rect.right = client.right;
    InvalidateRect(list, &rect, FALSE);
}
LRESULT CALLBACK eventListProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    int next = app.eventHotRow;
    if (message == WM_MOUSEMOVE) {
        LVHITTESTINFO hit{}; hit.pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        next = ListView_SubItemHitTest(window, &hit);
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking);
    } else if (message == WM_MOUSELEAVE || message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL ||
        message == WM_HSCROLL || message == WM_VSCROLL || message == LVM_SCROLL || message == LVM_ENSUREVISIBLE ||
        message == WM_SIZE || message == WM_KILLFOCUS) next = -1;
    if (next != app.eventHotRow) {
        invalidateListRow(window, app.eventHotRow); invalidateListRow(window, next); app.eventHotRow = next;
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_UPDATEUISTATE)
        invalidateListRow(window, ListView_GetNextItem(window, -1, LVNI_FOCUSED));
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, eventListProc, id);
    return DefSubclassProc(window, message, wp, lp);
}
LRESULT CALLBACK keyListProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (message == WM_MOUSEMOVE) {
        LVHITTESTINFO hit{}; hit.pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ListView_SubItemHitTest(window, &hit);
        if (hit.iItem != app.keyHotRow || hit.iSubItem != app.keyHotColumn) {
            hideKeyTooltip();
            for (int row : {app.keyHotRow, hit.iItem}) invalidateListRow(window, row);
            app.keyHotRow = hit.iItem; app.keyHotColumn = hit.iSubItem;
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_HOVER | TME_LEAVE, window, HOVER_DEFAULT}; TrackMouseEvent(&tracking);
        }
    }
    if (message == WM_MOUSEHOVER && app.keyHotRow >= 0 && static_cast<size_t>(app.keyHotRow) < app.keyEntries.size()) {
        auto full = wide(app.keyEntries[app.keyHotRow].text); app.keyTooltipText.clear();
        if (app.keyHotColumn == 2) app.keyTooltipText = L"删除这条公钥\r\n";
        // Explicit wrapping also handles long unbroken base64 without truncation.
        for (size_t i = 0; i < full.size(); ++i) { app.keyTooltipText += full[i]; if ((i + 1) % 88 == 0) app.keyTooltipText += L'\n'; }
        TOOLINFOW info{}; info.cbSize = sizeof(info); info.hwnd = app.keys; info.uId = 1; info.lpszText = app.keyTooltipText.data();
        SendMessageW(app.keyTooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info));
        POINT point{}; GetCursorPos(&point);
        SendMessageW(app.keyTooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(point.x + scale(12), point.y + scale(20)));
        SendMessageW(app.keyTooltip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&info));
    }
    if (message == WM_MOUSELEAVE || message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL || message == WM_HSCROLL || message == WM_VSCROLL || message == WM_LBUTTONDOWN || message == WM_KILLFOCUS || message == LVM_SCROLL || message == LVM_ENSUREVISIBLE || message == WM_SIZE) {
        hideKeyTooltip();
        invalidateListRow(window, app.keyHotRow);
        app.keyHotRow = app.keyHotColumn = -1;
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_UPDATEUISTATE)
        invalidateListRow(window, ListView_GetNextItem(window, -1, LVNI_FOCUSED));
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, keyListProc, id);
    auto result = DefSubclassProc(window, message, wp, lp);
    if (message == WM_SIZE) fitKeyColumns();
    return result;
}
void drawDelete(const NMLVCUSTOMDRAW& draw) {
    RECT rect{}; ListView_GetSubItemRect(app.keys, static_cast<int>(draw.nmcd.dwItemSpec), 2, LVIR_BOUNDS, &rect);
    bool selected = (ListView_GetItemState(app.keys, static_cast<int>(draw.nmcd.dwItemSpec), LVIS_SELECTED) & LVIS_SELECTED) != 0;
    // The shared row renderer already painted the action cell's background.
    int x = (rect.left + rect.right) / 2, y = (rect.top + rect.bottom) / 2;
    bool hot = app.keyHotRow == static_cast<int>(draw.nmcd.dwItemSpec) && app.keyHotColumn == 2;
    COLORREF ink = app.worker ? theme::colors.muted : theme::colors.danger;
    if (theme::colors.highContrast)
        ink = app.worker ? theme::colors.muted : selected || app.keyHotRow == static_cast<int>(draw.nmcd.dwItemSpec) ? theme::colors.onAccent : theme::colors.text;
    auto pen = CreatePen(PS_SOLID, (std::max)(1, scale(hot && !app.worker ? 3 : 2)), ink); auto old = SelectObject(draw.nmcd.hdc, pen);
    auto brush = SelectObject(draw.nmcd.hdc, GetStockObject(NULL_BRUSH));
    Rectangle(draw.nmcd.hdc, x - scale(5), y - scale(4), x + scale(6), y + scale(8));
    MoveToEx(draw.nmcd.hdc, x - scale(8), y - scale(7), nullptr); LineTo(draw.nmcd.hdc, x + scale(9), y - scale(7));
    MoveToEx(draw.nmcd.hdc, x - scale(3), y - scale(10), nullptr); LineTo(draw.nmcd.hdc, x + scale(4), y - scale(10));
    SelectObject(draw.nmcd.hdc, brush); SelectObject(draw.nmcd.hdc, old); DeleteObject(pen);
}
unsigned __stdcall eventThread(void*) {
    try { app.pendingEvents = readEvents(); }
    catch (const std::exception& e) { app.pendingEvents.warning = wide(e.what()); }
    catch (...) { app.pendingEvents.warning = L"读取事件失败。"; }
    return 0;
}
void loadEvents() {
    if (app.eventWorker) return;
    app.pendingEvents = {};
    app.eventWorker = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, eventThread, nullptr, 0, nullptr));
    if (!app.eventWorker) throw std::runtime_error("Could not start event reader.");
    enable(get(Events), FALSE);
    SetWindowTextW(app.eventHint, L"正在读取 Operational 和 Admin 通道…");
}
void displayEvents() {
    app.eventHotRow = -1;
    app.eventData = std::move(app.pendingEvents);
    capsule::ListUpdate update(app.events);
    ListView_DeleteAllItems(app.events);
    for (size_t i = 0; i < app.eventData.records.size(); ++i) {
        auto& record = app.eventData.records[i];
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = static_cast<int>(i); item.pszText = record.time.data();
        ListView_InsertItem(app.events, &item);
        int column = 1;
        for (auto* value : {&record.result, &record.user, &record.address, &record.explanation}) {
            ListView_SetItemText(app.events, static_cast<int>(i), column++, value->data());
        }
    }
    SetWindowTextW(app.eventDetail, app.eventData.warning.empty() ? L"选择一条事件查看完整消息和 XML。" : app.eventData.warning.c_str());
    std::wstring hint = L"已读取 " + std::to_wstring(app.eventData.records.size()) + L" 条（本机时区）。";
    if (!app.eventData.warning.empty()) hint += L"部分读取失败，详情见下方。";
    else if (app.eventData.records.empty()) hint += L"暂无事件；文件日志不在此列。";
    SetWindowTextW(app.eventHint, hint.c_str());
    if (!app.eventData.records.empty()) ListView_SetItemState(app.events, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    enable(get(Events), TRUE);
}
void display() {
    ShowWindow(app.window, SW_RESTORE);
    SetForegroundWindow(app.window);
}
void trayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, Show, L"打开控制面板");
    bool serviceActionAvailable = app.service.state == SERVICE_RUNNING ?
        !app.worker && app.statusError.empty() && app.service.installed && app.service.problem.empty() : startDisabledReason().empty();
    if (app.service.installed) AppendMenuW(menu, MF_STRING | (serviceActionAvailable ? 0 : MF_GRAYED),
        app.service.state == SERVICE_RUNNING ? Stop : Start, app.service.state == SERVICE_RUNNING ? L"停止 SSH 服务" : L"启动 SSH 服务");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Exit, L"退出托盘（服务继续运行）");
    POINT point{}; GetCursorPos(&point); SetForegroundWindow(app.window);
    auto id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, app.window, nullptr);
    DestroyMenu(menu); PostMessageW(app.window, WM_NULL, 0, 0);
    if (id) PostMessageW(app.window, WM_COMMAND, id, 0);
}
void reducePagePadding() {
    // Right page: left/right padding 32/28 -> 16/14, bottom padding 16 -> 8.
    // Align its title with the navigation top, keeping the bottom at y=752.
    // Map the existing page content into the reclaimed space before viewport
    // scaling; the sidebar and account/status strip have their own layout.
    auto compact = [](int& x, int& y, int& width, int& height) {
        if (x < 228 || y < 136) return;
        auto mapX = [](int value) { return 212 + MulDiv(value - 228, 794, 764); };
        auto mapY = [](int value) { return contentTop + MulDiv(value - 136, 752 - contentTop, 608); };
        int right = mapX(x + width), bottom = mapY(y + height);
        x = mapX(x); y = mapY(y); width = right - x; height = bottom - y;
    };
    for (auto& item : app.items) compact(item.x, item.y, item.width, item.height);
    for (auto& panel : app.cards) compact(panel.x, panel.y, panel.width, panel.height);
    // The header is outside the page padding transform; align its account
    // text to the transformed page title while keeping the right edge fixed.
    if (auto* title = findItem(app.pageTitle)) {
        for (HWND window : {app.account, app.accountDetail}) if (auto* item = findItem(window)) {
            item->width += item->x - title->x;
            item->x = title->x;
        }
    }
    // Apply the gap after padding conversion so it remains 22 design pixels.
    auto* tray = findItem(get(AutoTray));
    auto* automatic = findItem(get(Auto));
    if (tray && automatic) {
        automatic->x = tray->x + tray->width + startupOptionGap;
        automatic->y = tray->y;
    }
}
std::wstring localBuildTime() {
    const ULONGLONG ticks = (buildinfo::compiledEpoch + 11644473600ULL) * 10000000ULL;
    FILETIME utc{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME utcTime{}, local{};
    if (!FileTimeToSystemTime(&utc, &utcTime) || !SystemTimeToTzSpecificLocalTime(nullptr, &utcTime, &local)) return L"未知";
    wchar_t value[32]{};
    swprintf_s(value, L"%02u-%02u %02u:%02u", local.wMonth, local.wDay, local.wHour, local.wMinute);
    return value;
}
void createUi() {
    app.dpi = GetDpiForWindow(app.window);
    RECT client{}; GetClientRect(app.window, &client); app.viewportW = client.right; app.viewportH = client.bottom;
    app.uiDpi = layoutmath::Viewport{app.viewportW, app.viewportH}.fontDpi(); fonts();
    app.brand = control(L"STATIC", L"OpenSSH Manager", 0, -1, 140, 24, 40, 40, SS_OWNERDRAW);
    label(L"OpenSSH", -1, 20, 25, 112, 26, Strong);
    label(L"MANAGER", -1, 21, 51, 110, 20, Small, true);
    label(buildinfo::version, -1, 20, 76, 156, 16, Small, true);
    auto buildTime = std::wstring(L"编译 ") + localBuildTime();
    label(buildTime.c_str(), -1, 20, 96, 156, 18, Small, true);
    app.navigation = control(L"LISTBOX", L"功能导航", Navigation, -1, 12, contentTop, 172, 208,
        WS_TABSTOP | LBS_NOTIFY | LBS_HASSTRINGS | LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT);
    SendMessageW(app.navigation, LB_SETITEMHEIGHT, 0, sy(52));
    for (auto caption : pageTitles) SendMessageW(app.navigation, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(caption));
    label(L"后台服务独立运行", -1, 24, 650, 152, 20, Small, true, false, MoveY);
    label(L"关闭窗口可收起到托盘", -1, 24, 674, 156, 20, Small, true, false, MoveY);
    app.account = control(L"STATIC", L"正在读取账号…", 0, -1, 228, 25, 548, 26, SS_ENDELLIPSIS | SS_NOPREFIX, GrowX);
    app.items.back().font = Strong;
    app.accountDetail = control(L"STATIC", L"", 0, -1, 228, 51, 548, 20, SS_ENDELLIPSIS | SS_NOPREFIX, GrowX);
    app.items.back().font = Small; app.items.back().muted = true;
    app.status = control(L"STATIC", L"SSH 服务", 0, -1, 800, 26, 192, 44, SS_OWNERDRAW, MoveX);
    app.pageTitle = label(pageTitles[0], -1, 228, 136, 764, 34, PageTitle, false, true, GrowX);
    app.pageSubtitle = label(pageDescriptions[0], -1, 230, 176, 760, 22, Body, true, true, GrowX);
    label(L"更改系统设置时会请求管理员授权。", -1, 230, 724, 760, 20, Small, true, true, GrowX | MoveY);

    card(0, 228, 208, 764, 282);
    label(L"SSH 服务", 0, 252, 230, 716, 24, Strong);
    app.details = control(L"EDIT", L"", 0, 0, 252, 270, 716, 112,
        WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, GrowX);
    app.items.back().muted = true;
    button(L"启动服务", Start, 0, 252, 398, 108);
    button(L"停止服务", Stop, 0, 372, 398, 108);
    button(L"重启服务", Restart, 0, 492, 398, 108);
    button(L"选择离线包", SetupOffline, 0, 372, 398, 108);
    button(L"取消下载", SetupCancel, 0, 492, 398, 116);
    app.startHint = label(L"", 0, 252, 446, 716, 36, Small, true, false, GrowX);
    card(0, 228, 506, 764, 188);
    label(L"启动与维护", 0, 252, 528, 716, 24, Strong);
    toggle(L"登录 Windows 后显示托盘", AutoTray, 0, 252, 563, 240);
    check(AutoTray, autorun());
    toggle(L"SSH 服务开机启动", Auto, 0, 514, 563, 224);
    button(L"检查配置", Validate, 0, 252, 606, 116);
    button(L"打开配置目录", OpenFolder, 0, 380, 606, 148);
    button(L"同步防火墙规则", Firewall, 0, 540, 606, 176);
    label(L"退出托盘不会停止服务。防火墙默认允许域 / 专用网络的本地子网。", 0, 252, 660, 716, 20, Small, true, false, GrowX);

    // Heading-to-controls gap: 22 -> 11 design pixels on both rows.
    card(1, 228, 208, 764, 156);
    label(L"认证与转发", 1, 252, 232, 120, 24, Strong);
    toggle(L"允许密码认证", Password, 1, 383, 228, 170);
    toggle(L"允许公钥认证", Pubkey, 1, 575, 228, 170);
    toggle(L"允许 TCP 转发", Forwarding, 1, 767, 228, 187);
    label(L"网络监听", 1, 252, 288, 120, 24, Strong);
    app.portLabel = label(L"端口", 1, 383, 288, 40);
    edit(Port, 1, 435, 288, 64);
    app.listenLabel = label(L"监听 IP", 1, 515, 288, 70);
    SetWindowLongPtrW(app.listenLabel, GWL_STYLE, GetWindowLongPtrW(app.listenLabel, GWL_STYLE) | SS_RIGHT);
    control(WC_COMBOBOXW, L"", Listen, 1, 593, 284, 253, 240,
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL, GrowX);
    app.listenHint = label(L"选择网络接口地址；地址变化后需重新选择。", 1, 252, 330, 716, 18, Small, true, false, GrowX);
    refreshListenAddresses(L"");
    card(1, 228, 380, 764, 192);
    label(L"访问范围", 1, 252, 400, 716, 24, Strong);
    label(L"允许的用户", 1, 252, 440, 108); edit(Users, 1, 376, 440, 578, GrowX);
    label(L"允许的用户组", 1, 252, 486, 108); edit(Groups, 1, 376, 486, 578, GrowX);
    label(L"账号使用小写，多个条目用空格分隔，如 domain\\alice bob。\r\n带空格的组名加双引号；留空表示不增加该项限制。", 1, 252, 528, 716, 30, Small, true, false, GrowX);
    app.note = control(L"EDIT", L"", 0, 1, 230, 586, 554, 44, WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY, GrowX, true);
    app.items.back().canvas = true; app.items.back().muted = true; app.items.back().font = Small;
    button(L"保存并应用", Apply, 1, 812, 592, 180, MoveX, true);

    card(2, 228, 208, 764, 134);
    label(L"默认终端", 2, 252, 230, 716, 24, Strong);
    label(L"新会话使用的命令行环境", 2, 252, 270, 190, 22, Body, true);
    control(WC_COMBOBOXW, L"", Shell, 2, 464, 266, 266, 180, CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL, GrowX);
    SendMessageW(get(Shell), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Windows 命令提示符（CMD）"));
    SendMessageW(get(Shell), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Windows PowerShell"));
    button(L"应用终端设置", SetShell, 2, 752, 260, 216, MoveX);
    label(L"应用于所有账号新建的 SSH 会话，已有会话继续使用原终端。", 2, 252, 308, 716, 20, Small, true, false, GrowX);
    card(2, 228, 358, 764, 336, GrowX | GrowY);
    HWND keyTitle = label(L"授权公钥", 2, 252, 380, 64, 24, Strong);
    button(L"刷新列表", KeyRefresh, 2, 716, 370, 116, MoveX);
    button(L"导入公钥", KeyAdd, 2, 844, 370, 124, MoveX);
    control(WC_COMBOBOXW, L"", KeyTarget, 2, 338, 376, 356, 120, CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL, GrowX);
    SendMessageW(get(KeyTarget), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"当前 Windows 用户：.ssh\\authorized_keys"));
    SendMessageW(get(KeyTarget), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"管理员组共用：administrators_authorized_keys"));
    SendMessageW(get(KeyTarget), CB_SETCURSEL, 0, 0);
    app.keys = control(WC_LISTVIEWW, L"授权公钥列表", KeyList, 2, 252, 418, 716, 218,
        WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS, GrowX | GrowY);
    ListView_SetExtendedListViewStyle(app.keys, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(app.keys, theme::colors.surface); ListView_SetTextBkColor(app.keys, theme::colors.surface); ListView_SetTextColor(app.keys, theme::colors.text);
    int keyColumn = 0;
    for (auto caption : {L"备注", L"类型", L"操作"}) {
        LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH; column.cx = scale(keyColumn == 0 ? 466 : keyColumn == 1 ? 194 : 56);
        column.pszText = const_cast<LPWSTR>(caption); ListView_InsertColumn(app.keys, keyColumn++, &column);
    }
    SetWindowSubclass(app.keys, keyListProc, 3, 0);
    app.keyTooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, app.window, nullptr, GetModuleHandleW(nullptr), nullptr);
    TOOLINFOW tool{}; tool.cbSize = sizeof(tool); tool.uFlags = TTF_TRACK | TTF_ABSOLUTE | TTF_TRANSPARENT;
    tool.hwnd = app.keys; tool.uId = 1; tool.lpszText = const_cast<LPWSTR>(L"");
    SendMessageW(app.keyTooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    SendMessageW(app.keyTooltip, TTM_SETMAXTIPWIDTH, 0, scale(760));
    app.keyHint = label(L"", 2, 252, 646, 716, 40, Small, true, false, GrowX | MoveY);

    card(3, 228, 208, 764, 486, GrowX | GrowY);
    label(L"最近的连接记录", 3, 252, 230, 400, 24, Strong);
    button(L"刷新登录日志", Events, 3, 812, 224, 156, MoveX);
    app.events = control(WC_LISTVIEWW, L"登录事件", EventList, 3, 252, 278, 716, 218,
        WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS, GrowX | GrowY);
    SetWindowSubclass(app.events, eventListProc, 5, 0);
    ListView_SetExtendedListViewStyle(app.events, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ListView_SetBkColor(app.events, theme::colors.surface); ListView_SetTextBkColor(app.events, theme::colors.surface); ListView_SetTextColor(app.events, theme::colors.text);
    int column = 0;
    for (auto caption : {L"时间", L"结果", L"账号", L"来源 IP", L"说明"}) {
        const int widths[] = {150, 54, 105, 132, 350};
        LVCOLUMNW item{}; item.mask = LVCF_TEXT | LVCF_WIDTH; item.pszText = const_cast<LPWSTR>(caption); item.cx = scale(widths[column]);
        ListView_InsertColumn(app.events, column, &item); ++column;
    }
    label(L"事件详情", 3, 252, 510, 716, 24, Strong, false, false, GrowX | MoveY);
    app.eventDetail = control(L"EDIT", L"选择事件可查看完整消息和 XML。", 0, 3, 252, 542, 716, 106,
        WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL, GrowX | MoveY);
    app.items.back().font = Small; app.items.back().muted = true;
    SendMessageW(app.eventDetail, EM_SETLIMITTEXT, 2 * 1024 * 1024, 0);
    app.eventHint = label(L"合并 Operational / Admin；最多显示最近 200 条。", 3, 252, 664, 716, 20, Small, true, false, GrowX | MoveY);
    button(L"重新加载", Reload, -1, 20, 704, 156, MoveY);
    SetWindowTheme(app.keyTooltip, L"", L"");
    SendMessageW(app.keyTooltip, TTM_SETTIPBKCOLOR, theme::colors.surface, 0);
    SendMessageW(app.keyTooltip, TTM_SETTIPTEXTCOLOR, theme::colors.text, 0);
    theme::titleBar(app.window);
    reducePagePadding();
    // Preserve exactly 22 design pixels after the page padding transform.
    if (auto* target = findItem(get(KeyTarget))) {
        auto* title = findItem(keyTitle);
        auto* refresh = findItem(get(KeyRefresh));
        target->x = title->x + title->width + 22;
        target->width = refresh->x - 22 - target->x;
    }
    fonts(); app.ready = true; showPage(0); trayIcon(true); loadConfig();
    SetTimer(app.window, 1, 1000, nullptr);
    PostMessageW(app.window, setupMessage, 0, 0);
}
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    try {
        if (app.taskbarCreated && message == app.taskbarCreated) { trayIcon(true); return 0; }
        switch (message) {
        case WM_CREATE: app.window = window; app.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated"); createUi(); return 0;
        case setupMessage:
            if (needsEngineSetup()) {
                startSetup();
            }
            return 0;
        case WM_LBUTTONDOWN:
            // Empty page/card space is a focus target, just like another view.
            for (int id : {Listen, Shell, KeyTarget}) SendMessageW(get(id), CB_SHOWDROPDOWN, FALSE, 0);
            SetFocus(window); return 0;
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE) {
                for (int id : {Listen, Shell, KeyTarget}) SendMessageW(get(id), CB_SHOWDROPDOWN, FALSE, 0);
                if (GetFocus() == window || IsChild(window, GetFocus())) SetFocus(nullptr);
            }
            break;
        case WM_COMMAND: {
            int id = LOWORD(wparam), code = HIWORD(wparam);
            if (id == IDOK && !lparam) {
                HWND focus = GetFocus(); wchar_t cls[16]{}; GetClassNameW(focus, cls, 16);
                if (wcscmp(cls, L"Button") == 0 && IsWindowEnabled(focus) && (GetWindowLongPtrW(focus, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW)
                    SendMessageW(focus, BM_CLICK, 0, 0);
                return 0;
            }
            if (id == Navigation && code == LBN_SELCHANGE) {
                showPage(static_cast<int>(SendMessageW(app.navigation, LB_GETCURSEL, 0, 0))); return 0;
            }
            if (id == Listen && code == CBN_DROPDOWN) { refreshListenAddresses(listenValue()); return 0; }
            if (id == Listen && code == CBN_SELCHANGE) {
                if (!app.loading) { app.dirty = true; enableControls(); }
                return 0;
            }
            if (id == KeyTarget && code == CBN_SELCHANGE) {
                app.keyTargetInitialized = true;
                closeKeyPopup(); updateKeyHint(); loadKeys(); return 0;
            }
            if (!app.loading && ((code == EN_CHANGE && (id == Port || id == Users || id == Groups)) ||
                (code == BN_CLICKED && (id == Password || id == Pubkey || id == Forwarding)))) {
                app.dirty = true; enableControls(); return 0;
            }
            if (code != BN_CLICKED && lparam != 0) break;
            switch (id) {
            case SetupOffline: selectOfflineEngine(); break;
            case SetupCancel:
                app.setupCancelRequested = true; app.setup.cancel(); app.setupMessageText = L"正在取消下载，请稍候…"; refreshStatus(); break;
            case Start:
                refreshStatus();
                switch (currentPrimaryAction()) {
                case PrimaryAction::Download: startSetup({}, true); break;
                case PrimaryAction::Install: case PrimaryAction::Repair: installPreparedEngine(); break;
                case PrimaryAction::Start: if (startDisabledReason().empty()) begin({L"start"}); break;
                default: break;
                }
                break;
            case Stop: begin({L"stop"}); break;
            case Restart: begin({L"restart"}); break;
            case Auto: {
                bool automatic = checked(Auto);
                app.startupChangePending = true;
                try { begin({automatic ? L"auto" : L"manual"}); }
                catch (...) { app.startupChangePending = false; refreshStatus(); throw; }
                if (!app.worker) { app.startupChangePending = false; refreshStatus(); }
                break;
            }
            case Validate: begin({L"validate"}); break;
            case Firewall: begin({L"firewall"}); break;
            case Apply: applySettings(); break;
            case SetShell: begin({L"shell", SendMessageW(get(Shell), CB_GETCURSEL, 0, 0) == 1 ? L"powershell" : L"cmd"}); break;
            case Reload:
                if (!app.dirty || MessageBoxW(window, L"重新加载会放弃未保存的设置。继续？", L"重新加载", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    loadConfig(); startSetup();
                }
                break;
            case AutoTray:
                try { setAutorun(checked(AutoTray)); }
                catch (...) { check(AutoTray, autorun()); throw; }
                break;
            case Events: loadEvents(); break;
            case OpenFolder: ShellExecuteW(window, L"open", dataPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
            case KeyAdd: importKey(); break;
            case KeyRefresh: loadKeys(true); break;
            case Show: display(); break;
            case Exit:
                if (app.worker || app.eventWorker || app.setupBusy) { display(); MessageBoxW(window, L"请等待当前管理操作完成。", L"OpenSSH Manager", MB_OK); break; }
                if (app.dirty && MessageBoxW(window, L"有未保存的设置。退出托盘并放弃这些设置？SSH 服务将继续运行。", L"退出", MB_YESNO | MB_ICONQUESTION) != IDYES) break;
                app.closing = true; DestroyWindow(window); break;
            } return 0;
        }
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR*>(lparam)->idFrom == KeyList) {
                auto code = reinterpret_cast<NMHDR*>(lparam)->code;
                if (code == NM_CUSTOMDRAW) {
                    const auto& draw = *reinterpret_cast<NMLVCUSTOMDRAW*>(lparam);
                    if (draw.nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                    if (draw.nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                        int row = static_cast<int>(draw.nmcd.dwItemSpec);
                        theme::listRow(app.keys, draw.nmcd.hdc, row, app.font, app.uiDpi, row == app.keyHotRow, 2);
                        drawDelete(draw);
                        return CDRF_SKIPDEFAULT;
                    }
                    return CDRF_DODEFAULT;
                }
                if (code == LVN_ITEMCHANGED) {
                    const auto& changed = *reinterpret_cast<NMLISTVIEW*>(lparam);
                    if (changed.iItem >= 0 && (changed.uChanged & LVIF_STATE) && ((changed.uOldState ^ changed.uNewState) & LVIS_SELECTED)) {
                        invalidateListRow(app.keys, changed.iItem);
                    }
                }
                if (code == NM_CLICK) {
                    auto& hit = *reinterpret_cast<NMITEMACTIVATE*>(lparam);
                    if (hit.iSubItem == 2) removeKey(hit.iItem);
                }
                if (code == LVN_KEYDOWN && reinterpret_cast<NMLVKEYDOWN*>(lparam)->wVKey == VK_DELETE)
                    removeKey(ListView_GetNextItem(app.keys, -1, LVNI_SELECTED));
                return 0;
            }
            if (reinterpret_cast<NMHDR*>(lparam)->idFrom == EventList && reinterpret_cast<NMHDR*>(lparam)->code == NM_CUSTOMDRAW) {
                auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lparam);
                if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    int row = static_cast<int>(draw->nmcd.dwItemSpec);
                    COLORREF ink = theme::colors.muted;
                    if (draw->nmcd.dwItemSpec < app.eventData.records.size()) {
                        const auto& result = app.eventData.records[row].result;
                        ink = result == L"成功" ? theme::colors.success : result == L"失败" || result == L"错误" ? theme::colors.danger : theme::colors.muted;
                    }
                    theme::listRow(app.events, draw->nmcd.hdc, row, app.font, app.uiDpi, row == app.eventHotRow, -1, 1, ink);
                    return CDRF_SKIPDEFAULT;
                }
                return CDRF_DODEFAULT;
            }
            if (reinterpret_cast<NMHDR*>(lparam)->idFrom == EventList && reinterpret_cast<NMHDR*>(lparam)->code == LVN_ITEMCHANGED) {
                int index = ListView_GetNextItem(app.events, -1, LVNI_SELECTED);
                if (index >= 0 && static_cast<size_t>(index) < app.eventData.records.size()) {
                    auto detail = app.eventData.warning + app.eventData.records[index].details;
                    if (text(app.eventDetail) != detail) SetWindowTextW(app.eventDetail, detail.c_str());
                }
            }
            return 0;
        case WM_DRAWITEM: drawControl(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam)); return TRUE;
        case WM_MEASUREITEM: {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            measure->itemHeight = measure->CtlType == ODT_COMBOBOX ? sy(28) : sy(52); return TRUE;
        }
        case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wparam); auto* item = findItem(reinterpret_cast<HWND>(lparam));
            bool onCanvas = item && item->canvas;
            COLORREF background = onCanvas ? theme::colors.canvas : theme::colors.surface;
            SetBkMode(dc, OPAQUE); SetBkColor(dc, background);
            SetTextColor(dc, item && (item->muted || !IsWindowEnabled(item->window)) ? theme::colors.muted : theme::colors.text);
            return reinterpret_cast<LRESULT>(onCanvas ? theme::canvasBrush() : theme::surfaceBrush());
        }
        // Themed native checkboxes request their parent's background when painting.
        case WM_ERASEBKGND: paintBackground(reinterpret_cast<HDC>(wparam)); return 1;
        case WM_PRINTCLIENT: paintBackground(reinterpret_cast<HDC>(wparam)); return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint); paintBackground(dc); EndPaint(window, &paint); return 0;
        }
        case WM_SIZE: closeKeyPopup(); hideKeyTooltip(); if (wparam != SIZE_MINIMIZED) layout(); return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize = {MulDiv(740, app.dpi, 96), MulDiv(580, app.dpi, 96)}; return 0;
        }
        // The main window has no scrollable virtual canvas. Child lists/editors
        // consume their own wheel/scroll messages; unused input stops here.
        case WM_VSCROLL: case WM_HSCROLL: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: return 0;
        case WM_SETTINGCHANGE: case WM_THEMECHANGED: case WM_SYSCOLORCHANGE:
            // Windows can broadcast several notifications for one settings change.
            // Coalesce them, and do not repaint for unrelated system settings.
            if (!app.themePending) { app.themePending = true; PostMessageW(window, applyThemeMessage, 0, 0); }
            return 0;
        case applyThemeMessage:
            app.themePending = false;
            if (!theme::refresh()) return 0;
            app.backgroundDirty = true; theme::titleBar(window);
            for (HWND list : {app.events, app.keys}) if (list) {
                ListView_SetBkColor(list, theme::colors.surface); ListView_SetTextBkColor(list, theme::colors.surface); ListView_SetTextColor(list, theme::colors.text);
            }
            if (app.keyTooltip) {
                SendMessageW(app.keyTooltip, TTM_SETTIPBKCOLOR, theme::colors.surface, 0);
                SendMessageW(app.keyTooltip, TTM_SETTIPTEXTCOLOR, theme::colors.text, 0);
                InvalidateRect(app.keyTooltip, nullptr, FALSE);
            }
            if (HWND popup = keyPopupWindow()) {
                theme::titleBar(popup);
                RedrawWindow(popup, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            }
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN); return 0;
        case WM_TIMER:
            pollSetup();
            if (app.worker && app.setupAdmin) {
                try { app.setupChannel.poll(app.worker); }
                catch (const std::exception& e) { app.setupChannel.close(); app.setupMessageText = wide(e.what()); }
            }
            if (app.eventWorker && WaitForSingleObject(app.eventWorker, 0) == WAIT_OBJECT_0) {
                CloseHandle(app.eventWorker); app.eventWorker = nullptr; displayEvents();
            }
            if (app.worker && app.keyAdminOperation) {
                try { app.keyChannel.poll(app.worker); }
                catch (const std::exception& e) {
                    app.keyChannel.close(); // Disconnect a failed channel so the helper cannot wait forever.
                    SetWindowTextW(app.keyHint, wide(e.what()).c_str());
                }
            }
            if (app.worker && WaitForSingleObject(app.worker, 0) == WAIT_OBJECT_0) {
                DWORD code = 1; GetExitCodeProcess(app.worker, &code); CloseHandle(app.worker); app.worker = nullptr;
                app.startupChangePending = false;
                if (app.setupAdmin) {
                    app.setupAdmin = app.setupBusy = false; app.setupActivity = SetupActivity::Idle;
                    try {
                        auto result = app.setupChannel.result();
                        if (code == 0 && result.rfind("OK\n", 0) == 0) {
                            app.setupMessageText = app.setupRepairOperation ? L"引擎修复完成，原服务启动类型保持不变。" : L"引擎安装完成。"; loadConfig();
                        } else app.setupMessageText = L"安装未完成：\r\n" + wide(result.rfind("ERROR\n", 0) == 0 ? result.substr(6) : result);
                    } catch (...) {
                        app.setupMessageText = L"未收到安装结果。请检查服务状态后重试。";
                    }
                    app.setupChannel.close();
                } else if (app.keyAdminOperation) {
                    app.keyAdminOperation = false; KillTimer(window, 3);
                    try {
                        if (code == 0) displayKeys(app.keyChannel.result(), true);
                        else SetWindowTextW(app.keyHint, L"操作未完成，原列表已保留。请刷新后核对。" );
                    } catch (...) { app.keyChannel.close(); enableControls(); throw; }
                    app.keyChannel.close(); enableControls();
                } else if (code == 0 && (app.workerAppliesConfig || !app.dirty)) loadConfig();
            }
            if (wparam != 3) refreshStatus();
            return 0;
        case trayMessage:
            if (LOWORD(lparam) == NIN_SELECT || LOWORD(lparam) == NIN_KEYSELECT || LOWORD(lparam) == WM_LBUTTONDBLCLK) display();
            else if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == WM_RBUTTONUP) trayMenu();
            return 0;
        case WM_DPICHANGED: {
            closeKeyPopup(); hideKeyTooltip();
            app.dpi = HIWORD(wparam); loadAppIcons(); app.trayTip.clear(); trayIcon(false);
            SendMessageW(app.keyTooltip, TTM_SETMAXTIPWIDTH, 0, scale(760));
            auto rect = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CLOSE: closeKeyPopup(); hideKeyTooltip(); ShowWindow(window, SW_HIDE); return 0;
        case WM_DESTROY:
            KillTimer(window, 1); KillTimer(window, 3); closeKeyPopup(); app.keyChannel.close(); app.setup.cancel(); app.setupChannel.close(); releaseBackground(); Shell_NotifyIconW(NIM_DELETE, &app.tray);
            if (app.worker) CloseHandle(app.worker);
            if (app.rowImages) ImageList_Destroy(app.rowImages);
            DeleteObject(app.font); DeleteObject(app.small); DeleteObject(app.heading); DeleteObject(app.title);
            PostQuitMessage(0); return 0;
        }
    } catch (const std::exception& e) {
        // Avoid repeated error dialogs from the status refresh timer.
        if (message == WM_TIMER && wparam == 3) SetWindowTextW(app.keyHint, wide(e.what()).c_str());
        else if (message == WM_TIMER) { if (text(app.status) != L"无法读取服务状态，请重新加载。") SetWindowTextW(app.status, L"无法读取服务状态，请重新加载。"); }
        else error(e);
        if (message == WM_CREATE) return -1;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int count = 0; auto argv = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::wstring> args;
    for (int i = 1; argv && i < count; ++i) args.emplace_back(argv[i]);
    LocalFree(argv);
    if (!args.empty() && args[0] == L"--admin") {
        args.erase(args.begin()); int result = adminMain(args); CoUninitialize(); return result;
    }
    bool trayOnly = false;
    for (const auto& arg : args) {
        if (arg == L"--tray") trayOnly = true;
        else if (arg == L"--no-auto-install") {} // Legacy option: startup is always detection-only.
        else { MessageBoxW(nullptr, L"支持的参数：--tray、--no-auto-install", L"OpenSSH Manager", MB_OK | MB_ICONERROR); CoUninitialize(); return 1; }
    }
    HANDLE singleton = CreateMutexW(nullptr, TRUE, L"Local\\OpenSSHManager.Desktop");
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(windowClass, nullptr)) { ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing); }
        if (singleton) CloseHandle(singleton);
        CoUninitialize(); return 0;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
    app.dpi = GetDpiForSystem();
    try { theme::initialize(); loadAppIcons(); }
    catch (const std::exception& e) { MessageBoxW(nullptr, wide(e.what()).c_str(), L"OpenSSH Manager", MB_OK | MB_ICONERROR); CloseHandle(singleton); CoUninitialize(); return 1; }
    WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls);
    cls.hInstance = instance; cls.lpfnWndProc = windowProc; cls.lpszClassName = windowClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hIcon = app.icon; cls.hIconSm = app.smallIcon;
    cls.hbrBackground = nullptr;
    RegisterClassExW(&cls);
    app.dpi = GetDpiForSystem();
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    RECT rect{0, 0, MulDiv(baseWidth, app.dpi, 96), MulDiv(baseHeight, app.dpi, 96)};
    AdjustWindowRectExForDpi(&rect, style, FALSE, 0, app.dpi);
    RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int width = (std::min)(static_cast<int>(rect.right - rect.left), static_cast<int>(work.right - work.left) - MulDiv(32, app.dpi, 96));
    int height = (std::min)(static_cast<int>(rect.bottom - rect.top), static_cast<int>(work.bottom - work.top) - MulDiv(32, app.dpi, 96));
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, windowClass, L"OpenSSH Manager", style,
        work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2,
        width, height, nullptr, nullptr, instance, nullptr);
    if (!window) { theme::shutdown(); CloseHandle(singleton); CoUninitialize(); return 1; }
    if (!trayOnly) ShowWindow(window, show);
    MSG message{}; int result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (keyPopupWindow() && IsDialogMessageW(keyPopupWindow(), &message)) continue;
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    DestroyIcon(app.icon); DestroyIcon(app.smallIcon);
    theme::shutdown(); CloseHandle(singleton); CoUninitialize();
    return result < 0 ? 1 : static_cast<int>(message.wParam);
}
