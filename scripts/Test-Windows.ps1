#Requires -Version 5.1
[CmdletBinding()]
param([string]$Manager = (Join-Path $PSScriptRoot 'OpenSSHManager.exe'), [switch]$CheckService,
    [ValidateSet('auto', 'en', 'zh-Hans', 'zh-Hant')][string]$UiLanguage = 'auto')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Localization.ps1')
Initialize-SetupLanguage $UiLanguage
if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell.' }
if (Get-Process -Name OpenSSHManager -ErrorAction SilentlyContinue) { throw 'Exit any running manager first.' }
# Exercise only navigation and viewport behavior; never click a service/configuration action.
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class ManagerUiProbe {
    private delegate bool ChildCallback(IntPtr window, IntPtr data);
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct ScrollInfo {
        public uint Size, Mask; public int Min, Max; public uint Page; public int Position, Track;
    }
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr parent, ChildCallback callback, IntPtr data);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(IntPtr window, StringBuilder name, int count);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] private static extern int GetDlgCtrlID(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll", EntryPoint="SendMessageW")] private static extern IntPtr SendRect(IntPtr window, uint message, IntPtr wp, ref Rect rect);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] private static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] private static extern bool ClientToScreen(IntPtr window, ref Point point);
    [DllImport("user32.dll", EntryPoint="GetWindowLongW")] private static extern int GetWindowLong(IntPtr window, int index);
    [DllImport("user32.dll")] private static extern bool GetScrollInfo(IntPtr window, int bar, ref ScrollInfo info);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] private static extern IntPtr GetDlgItem(IntPtr parent, int id);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr child);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool SetWindowText(IntPtr window, string text);
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")] private static extern int GetSystemMetricsForDpi(int index, uint dpi);
    [DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr process, uint kind);
    private static List<IntPtr> Children(IntPtr window) {
        var items = new List<IntPtr>();
        EnumChildWindows(window, (child, unused) => { items.Add(child); return true; }, IntPtr.Zero);
        return items;
    }
    public static IntPtr Navigation(IntPtr window) {
        foreach (var child in Children(window)) {
            var name = new StringBuilder(80); GetClassName(child, name, name.Capacity);
            if (name.ToString() == "ListBox") return child;
        }
        throw new Exception("Native navigation list was not found.");
    }
    public static int VisibleLists(IntPtr window) {
        int count = 0;
        foreach (var child in Children(window)) {
            var name = new StringBuilder(80); GetClassName(child, name, name.Capacity);
            if (name.ToString() == "SysListView32" && IsWindowVisible(child)) count++;
        }
        return count;
    }
    public static bool VisibleControl(IntPtr window, int id) { return IsWindowVisible(GetDlgItem(window, id)); }
    public static string Caption(IntPtr window, int id) {
        var text = new StringBuilder(256); GetWindowText(GetDlgItem(window, id), text, text.Capacity); return text.ToString();
    }
    public static bool VisibleButton(IntPtr window, string caption) {
        foreach (var child in Children(window)) {
            var name = new StringBuilder(80); GetClassName(child, name, name.Capacity);
            if (name.ToString() != "Button" || !IsWindowVisible(child)) continue;
            var text = new StringBuilder(256); GetWindowText(child, text, text.Capacity);
            if (text.ToString() == caption) return true;
        }
        return false;
    }
    public static void CheckNavigation(IntPtr navigation) {
        Rect client; GetClientRect(navigation, out client);
        if (!IsWindowVisible(navigation) || SendMessage(navigation, 0x18E, IntPtr.Zero, IntPtr.Zero).ToInt64() != 0)
            throw new Exception("Navigation scrolled the overview item out of view."); // LB_GETTOPINDEX
        for (int i = 0; i < 4; ++i) {
            var row = new Rect();
            if (SendRect(navigation, 0x198, new IntPtr(i), ref row).ToInt64() == -1 ||
                row.Top < client.Top || row.Bottom > client.Bottom || row.Bottom <= row.Top)
                throw new Exception("A navigation item is not fully visible."); // LB_GETITEMRECT
        }
    }
    public static void SelectPage(IntPtr window, IntPtr navigation, int index) {
        SendMessage(navigation, 0x186, new IntPtr(index), IntPtr.Zero); // LB_SETCURSEL
        CheckNavigation(navigation); // Check native selection before the page handler can correct scrolling.
        SendMessage(window, 0x111, new IntPtr(GetDlgCtrlID(navigation) | (1 << 16)), navigation); // LBN_SELCHANGE
        CheckNavigation(navigation);
        if (index == 1) foreach (var child in Children(window)) {
            if (IsWindowVisible(child) && GetDlgCtrlID(child) == 0x6001)
                throw new Exception("Connection/authentication page displays a horizontal capsule.");
        }
    }
    public static void Resize(IntPtr window, int width, int height) {
        if (!SetWindowPos(window, IntPtr.Zero, 0, 0, width, height, 0x16)) throw new Exception("Window resize failed.");
    }
    public static void CheckLayout(IntPtr window, bool servicePage) {
        if ((GetWindowLong(window, -16) & 0x00300000) != 0 || GetDlgItem(window, 0x6001) != IntPtr.Zero || GetDlgItem(window, 0x6002) != IntPtr.Zero)
            throw new Exception("Main window still has scrollbars.");
        Rect client; GetClientRect(window, out client);
        var origin = new Point(); ClientToScreen(window, ref origin);
        foreach (var child in Children(window)) {
            if (GetParent(child) != window || !IsWindowVisible(child)) continue;
            Rect r; GetWindowRect(child, out r);
            if (r.Left < origin.X || r.Top < origin.Y || r.Right > origin.X + client.Right + 1 || r.Bottom > origin.Y + client.Bottom + 1)
                throw new Exception("A main-window control extends outside the scaled viewport.");
        }
        if (servicePage) {
            Rect button; GetWindowRect(GetDlgItem(window, 101), out button);
            int expectedX = (int)Math.Floor(237.0 * client.Right / 1020 + .5);
            int expectedY = (int)Math.Floor(394.0 * client.Bottom / 760 + .5);
            int expectedRight = (int)Math.Floor(349.0 * client.Right / 1020 + .5);
            if (Math.Abs(button.Left - origin.X - expectedX) > 1 || Math.Abs(button.Top - origin.Y - expectedY) > 1 ||
                Math.Abs(button.Right - origin.X - expectedRight) > 1) throw new Exception("Control geometry is not proportional to the viewport.");
        }
    }
    public static string EventGeometry(IntPtr window) {
        IntPtr list = IntPtr.Zero;
        foreach (var child in Children(window)) if (GetDlgCtrlID(child) == 127) { list = child; break; }
        if (list == IntPtr.Zero) throw new Exception("Event list missing.");
        IntPtr host = GetParent(list), header = IntPtr.Zero;
        foreach (var child in Children(host)) {
            var name = new StringBuilder(80); GetClassName(child, name, name.Capacity);
            if (name.ToString() == "SysHeader32" && GetParent(child) != list) { header = child; break; }
        }
        if (header == IntPtr.Zero) throw new Exception("Event header missing.");
        Rect client, panel, body, heading;
        GetClientRect(window, out client); GetWindowRect(host, out panel); GetWindowRect(list, out body);
        GetWindowRect(GetParent(header), out heading);
        var origin = new Point(); ClientToScreen(window, ref origin);
        // Baseline table bounds after the page's reduced padding.
        int x = (int)Math.Floor(237.0 * client.Right / 1020 + .5);
        int y = (int)Math.Floor(270.0 * client.Bottom / 760 + .5);
        int right = (int)Math.Floor(981.0 * client.Right / 1020 + .5);
        int bottom = (int)Math.Floor(495.0 * client.Bottom / 760 + .5);
        if (Math.Abs(panel.Left - origin.X - x) > 1 || Math.Abs(panel.Top - origin.Y - y) > 1 ||
            Math.Abs(panel.Right - origin.X - right) > 1 || Math.Abs(panel.Bottom - origin.Y - bottom) > 1)
            throw new Exception("Event list was not laid out while its page was hidden.");
        if (heading.Top != panel.Top || heading.Bottom != body.Top || body.Bottom != panel.Bottom || body.Left != panel.Left)
            throw new Exception("Event header/body positions are inconsistent.");
        return String.Join(",", new int[] { panel.Left, panel.Top, panel.Right, panel.Bottom,
            body.Left, body.Top, body.Right, body.Bottom, heading.Top, heading.Bottom });
    }
    public static IntPtr Bar(IntPtr window, int axis) {
        var bar = GetDlgItem(window, 0x6001 + axis);
        if (bar == IntPtr.Zero) throw new Exception("Capsule scrollbar missing.");
        return bar;
    }
    public static ScrollInfo Scroll(IntPtr window, int axis) {
        var info = new ScrollInfo { Size = (uint)Marshal.SizeOf(typeof(ScrollInfo)), Mask = 0x17 };
        if (!GetScrollInfo(Bar(window, axis), 2, ref info)) throw new Exception("Capsule scroll state missing.");
        return info;
    }
    public static void CheckThickness(IntPtr window, int axis) {
        Rect rect; GetWindowRect(Bar(window, axis), out rect);
        int measured = axis == 0 ? rect.Bottom - rect.Top : rect.Right - rect.Left;
        int expected = Math.Max(4, (GetSystemMetricsForDpi(axis == 0 ? 3 : 2, GetDpiForWindow(window)) + 1) / 2);
        if (measured != expected) throw new Exception("Scrollbar thickness is not half the native metric.");
    }
    public static IntPtr VisibleEdit(IntPtr window) {
        foreach (var child in Children(window)) {
            var name = new StringBuilder(80); GetClassName(child, name, name.Capacity);
            if (name.ToString() == "Edit" && IsWindowVisible(child)) return child;
        }
        throw new Exception("No visible edit control.");
    }
    public static ScrollInfo NativeScroll(IntPtr window, int axis) {
        var info = new ScrollInfo { Size = (uint)Marshal.SizeOf(typeof(ScrollInfo)), Mask = 0x17 };
        if (!GetScrollInfo(window, axis, ref info)) throw new Exception("Native text scroll state missing.");
        return info;
    }
}
'@
$serviceBefore = Get-Service -Name sshd -ErrorAction SilentlyContinue
$gui = Start-Process -FilePath $Manager -ArgumentList "--ui-language=$script:SetupLanguage" -PassThru
try {
    Start-Sleep -Seconds 3
    $gui.Refresh()
    if ($gui.HasExited -or -not $gui.Responding -or -not $gui.MainWindowHandle) { throw 'GUI did not create a responding main window.' }
    $window = $gui.MainWindowHandle
    $navigation = [ManagerUiProbe]::Navigation($window)
    if ([ManagerUiProbe]::SendMessage($navigation, 0x18B, [IntPtr]::Zero, [IntPtr]::Zero).ToInt64() -ne 4) { throw 'Expected exactly four navigation pages.' }
    $captions = switch ($script:SetupLanguage) {
        'zh-Hans' { @('', '保存并应用', '导入公钥', '刷新登录日志') }
        'zh-Hant' { @('', '儲存並套用', '匯入公鑰', '重新整理日誌') }
        default { @('', 'Save & apply', 'Import key', 'Refresh logs') }
    }
    $startupCaptions = switch ($script:SetupLanguage) {
        'zh-Hans' { @('检测中…', '下载引擎', '安装服务', '修复安装', '启动服务', '引擎异常', '状态未知') }
        'zh-Hant' { @('偵測中…', '下載引擎', '安裝服務', '修復安裝', '啟動服務', '引擎異常', '狀態未知') }
        default { @('Checking…', 'Download', 'Install service', 'Repair', 'Start service', 'Engine issue', 'Unknown state') }
    }
    $primary = [ManagerUiProbe]::Caption($window, 101)
    if ($primary -notin $startupCaptions) { throw "Unexpected startup action or UI language: $primary" }
    if (-not $serviceBefore) {
        if (Get-Service -Name sshd -ErrorAction SilentlyContinue) { throw 'Startup unexpectedly registered sshd.' }
        if ([ManagerUiProbe]::VisibleControl($window, 102) -or [ManagerUiProbe]::VisibleControl($window, 103)) { throw 'Service controls visible before installation.' }
    }
    $eventBefore = [ManagerUiProbe]::EventGeometry($window)
    [ManagerUiProbe]::SelectPage($window, $navigation, 3)
    if ([ManagerUiProbe]::EventGeometry($window) -ne $eventBefore) { throw 'First visit moved the event list.' }
    Start-Sleep -Milliseconds 300
    if ([ManagerUiProbe]::EventGeometry($window) -ne $eventBefore) { throw 'Event list moved during its first paint.' }
    Start-Sleep -Milliseconds 1100
    if ([ManagerUiProbe]::EventGeometry($window) -ne $eventBefore) { throw 'Event list moved after a status refresh.' }
    [ManagerUiProbe]::SelectPage($window, $navigation, 0)
    $resourcesBefore = [ManagerUiProbe]::GetGuiResources($gui.Handle, 0)
    for ($round = 0; $round -lt 10; $round++) {
        for ($page = 0; $page -lt 4; $page++) {
            [ManagerUiProbe]::SelectPage($window, $navigation, $page)
            $expectedLists = if ($page -ge 2) { 1 } else { 0 }
            if ([ManagerUiProbe]::VisibleLists($window) -ne $expectedLists) { throw "Page $page list visibility mismatch." }
            for ($other = 0; $other -lt 4; $other++) {
                $visible = if ($other -eq 0) { [ManagerUiProbe]::VisibleControl($window, 101) } else { [ManagerUiProbe]::VisibleButton($window, $captions[$other]) }
                if ($visible -ne ($page -eq $other)) {
                    throw "Page $page visibility mismatch: $($captions[$other])"
                }
            }
        }
    }
    $resourcesAfter = [ManagerUiProbe]::GetGuiResources($gui.Handle, 0)
    if ($resourcesAfter -gt $resourcesBefore + 32) { throw 'GDI resources grew unexpectedly during navigation.' }
    [ManagerUiProbe]::SelectPage($window, $navigation, 0)
    [void][ManagerUiProbe]::SendMessage($navigation, 0x100, [IntPtr]0x28, [IntPtr]::Zero) # Down arrow
    if (-not [ManagerUiProbe]::VisibleButton($window, $captions[1])) { throw 'Navigation arrow key did not change the page.' }
    [ManagerUiProbe]::CheckNavigation($navigation)
    $original = New-Object ManagerUiProbe+Rect
    [void][ManagerUiProbe]::GetWindowRect($window, [ref]$original)
    foreach ($size in @(@(740, 580), @(1200, 799), @(1200, 800), @(1200, 801), @(1500, 1000), @(800, 900))) {
        [ManagerUiProbe]::SelectPage($window, $navigation, 0)
        [ManagerUiProbe]::Resize($window, $size[0], $size[1])
        $hiddenEvent = [ManagerUiProbe]::EventGeometry($window)
        foreach ($page in @(0, 1, 2, 3)) {
            [ManagerUiProbe]::SelectPage($window, $navigation, $page)
            [ManagerUiProbe]::CheckLayout($window, $page -eq 0)
            if ($page -eq 3 -and [ManagerUiProbe]::EventGeometry($window) -ne $hiddenEvent) { throw 'Event list moved after resizing another page.' }
        }
    }
    [ManagerUiProbe]::Resize($window, $original.Right - $original.Left, $original.Bottom - $original.Top)
    [ManagerUiProbe]::SelectPage($window, $navigation, 0)
    [ManagerUiProbe]::SelectPage($window, $navigation, 3)
    $detail = [ManagerUiProbe]::VisibleEdit($window)
    $fixture = ('Long event detail ' * 100) + "`r`n" + ((1..240 | ForEach-Object { "Event detail line $_" }) -join "`r`n")
    if (-not [ManagerUiProbe]::SetWindowText($detail, $fixture)) { throw 'Could not populate test-only event detail.' }
    Start-Sleep -Milliseconds 200
    $hostWindow = [ManagerUiProbe]::GetParent($detail)
    foreach ($axis in @(0, 1)) {
        [ManagerUiProbe]::CheckThickness($hostWindow, $axis)
        $native = [ManagerUiProbe]::NativeScroll($detail, $axis)
        $capsule = [ManagerUiProbe]::Scroll($hostWindow, $axis)
        if ($native.Max -ne $capsule.Max -or $native.Page -ne $capsule.Page) { throw 'Text scrollbar range is out of sync.' }
        $message = if ($axis -eq 0) { 0x114 } else { 0x115 }
        [void][ManagerUiProbe]::SendMessage($hostWindow, $message, [IntPtr]7, [ManagerUiProbe]::Bar($hostWindow, $axis))
        $native = [ManagerUiProbe]::NativeScroll($detail, $axis)
        $capsule = [ManagerUiProbe]::Scroll($hostWindow, $axis)
        if ($native.Position -ne $capsule.Position -or $native.Position -le 0) { throw 'Text scrollbar failed to move or synchronize.' }
    }
    [void][ManagerUiProbe]::SetWindowText($detail, '')
    [ManagerUiProbe]::SelectPage($window, $navigation, 0)
    Write-Host 'PASS: four pages, navigation keys, GDI resources, proportional layout without main scrollbars, internal half-width capsules and text-scroll synchronization.' 
    $gui.CloseMainWindow() | Out-Null
    Start-Sleep -Seconds 1
    $gui.Refresh()
    if ($gui.HasExited) { throw 'Closing the window must keep the tray process alive.' }
    Write-Host 'PASS: native window starts and close-to-tray keeps the process alive.'
} finally {
    if (-not $gui.HasExited) { Stop-Process -Id $gui.Id }
}
if ($CheckService) {
    $service = Get-CimInstance Win32_Service -Filter "Name='sshd'"
    if (-not $service) { throw 'sshd is not installed.' }
    if ($service.PathName -notmatch '^"?([^"\r\n]+?sshd\.exe)"?$') { throw 'Unsupported service command line.' }
    $exe = $Matches[1]
    $config = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'ssh\sshd_config'
    & $exe -t -f $config
    if ($LASTEXITCODE -ne 0) { throw 'sshd configuration validation failed (run elevated to read host keys).' }
    Write-Host "PASS: configuration validated; service state is $($service.State)."
}
Write-Host 'Still verify UAC cancellation, native control appearance, keyboard focus, high contrast and navigation at 100%/150%/200% DPI, both event channels, public-key login, domain login, terminal, SFTP and rollback on a Windows test machine.'
