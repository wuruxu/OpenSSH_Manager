# Shared by the installer, uninstaller and their helper scripts.
function Select-SetupLanguage([string[]]$Preferred) {
    foreach ($tag in $Preferred) {
        $parts = $tag.ToLowerInvariant().Replace('_', '-').Split('-')
        if ($parts[0] -eq 'en') { return 'en' }
        if ($parts[0] -ne 'zh') { continue }
        $region = ''
        for ($i = 1; $i -lt $parts.Count; $i++) {
            if ($parts[$i].Length -eq 1) { break }
            if ($parts[$i] -in @('hans', 'chs')) { return 'zh-Hans' }
            if ($parts[$i] -in @('hant', 'cht')) { return 'zh-Hant' }
            if ($parts[$i].Length -eq 2) { $region = $parts[$i] }
        }
        if ($region -in @('tw', 'hk', 'mo')) { return 'zh-Hant' }
        return 'zh-Hans'
    }
    return 'en'
}
function Initialize-SetupLanguage([ValidateSet('auto', 'en', 'zh-Hans', 'zh-Hant')][string]$Language = 'auto') {
    if ($Language -ne 'auto') { $script:SetupLanguage = $Language; return }
    $preferred = @()
    if ($env:OS -eq 'Windows_NT') {
        try {
            if (-not ('OpenSSHManager.SetupLanguages' -as [type])) {
                Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace OpenSSHManager {
    public static class SetupLanguages {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern bool GetUserPreferredUILanguages(uint flags, out uint count, [Out] char[] buffer, ref uint size);
        public static string[] Read() {
            uint count, size = 0;
            if (!GetUserPreferredUILanguages(8, out count, null, ref size) || size == 0) return new string[0];
            char[] buffer = new char[size];
            if (!GetUserPreferredUILanguages(8, out count, buffer, ref size)) return new string[0];
            return new string(buffer).Split(new char[] { '\0' }, StringSplitOptions.RemoveEmptyEntries);
        }
    }
}
'@
            }
            $preferred = @([OpenSSHManager.SetupLanguages]::Read())
        } catch { $preferred = @() }
    }
    if (-not $preferred.Count) { $preferred = @([Globalization.CultureInfo]::CurrentUICulture.Name) }
    $script:SetupLanguage = Select-SetupLanguage $preferred
}
function Get-SetupText([string]$Key, [object[]]$Values = @()) {
    $entry = $script:SetupMessages[$Key]
    if (-not $entry) { throw "Unknown setup text ID: $Key" }
    $index = switch ($script:SetupLanguage) { 'zh-Hans' { 0 }; 'zh-Hant' { 1 }; default { 2 } }
    if ($Values.Count) { return [string]::Format($entry[$index], $Values) }
    return $entry[$index]
}
# Dot-sourcing a helper again must preserve the original user's selected language.
if (-not (Get-Variable SetupLanguage -Scope Script -ErrorAction SilentlyContinue)) {
    Initialize-SetupLanguage
}

# Simplified Chinese, Traditional Chinese, English. Placeholders are format arguments.
$script:SetupMessages = @{
    ExistingServiceUnchanged = @('检测到现有 sshd 服务，未修改安装。', '偵測到現有 sshd 服務，未修改安裝。', 'An existing sshd service was detected; no installation changes were made.')
    EngineRepaired = @('OpenSSH 引擎已修复。现有服务注册和启动类型已保留。', 'OpenSSH 引擎已修復。現有服務註冊和啟動類型已保留。', 'OpenSSH engine repaired. Existing service registration and startup type were preserved.')
    EngineStarted = @('OpenSSH 引擎已安装并启动。', 'OpenSSH 引擎已安裝並啟動。', 'OpenSSH engine installed and started.')
    CloseWindow = @('按 Enter 关闭安装窗口', '按 Enter 關閉安裝視窗', 'Press Enter to close the installer')
    ReparsePath = @('不支持重解析点路径：{0}', '不支援重新剖析點路徑：{0}', 'Reparse point is not supported: {0}')
    NativeFailed = @('{0} 失败，退出代码为 {1}', '{0} 失敗，結束代碼為 {1}', '{0} failed with exit code {1}')
    HostKeys = @('生成主机密钥', '產生主機金鑰', 'Host key generation')
    ValidateConfig = @('验证 OpenSSH 配置', '驗證 OpenSSH 設定', 'OpenSSH configuration validation')
    SftpSignature = @('Microsoft 签名验证失败：sftp-server.exe', 'Microsoft 簽章驗證失敗：sftp-server.exe', 'Microsoft signature verification failed: sftp-server.exe')
    SftpStartup = @('检查 SFTP 引擎启动', '檢查 SFTP 引擎啟動', 'SFTP engine startup check')
    DataDirectory = @('SSH 数据路径不是目录：{0}', 'SSH 資料路徑不是目錄：{0}', 'SSH data path is not a directory: {0}')
    ExpectedFile = @('需要 SSH 配置或密钥文件：{0}', '需要 SSH 設定或金鑰檔案：{0}', 'Expected an SSH configuration/key file: {0}')
    PrivateKeyMissing = @('主机公钥缺少配套私钥：{0}.pub。请先恢复私钥；未替换任何现有密钥。', '主機公鑰缺少配對私鑰：{0}.pub。請先還原私鑰；未取代任何現有金鑰。', 'Public host key has no matching private key: {0}.pub. Restore the private key before installing; no existing key was replaced.')
    BackupChanged = @('备份期间文件已变更：{0}。安装已停止。', '備份期間檔案已變更：{0}。安裝已停止。', 'File changed during backup: {0}. Installation stopped.')
    BackupSaved = @('现有配置和密钥已备份到：{0}', '現有設定和金鑰已備份至：{0}', 'Existing configuration and keys backed up to: {0}')
    ConfigCreated = @('已创建配置：{0}', '已建立設定：{0}', 'Created configuration: {0}')
    ConfigPreserved = @('保留现有配置：{0}', '保留現有設定：{0}', 'Preserving existing configuration: {0}')
    ExistingChanged = @('现有文件意外变更：{0}。恢复副本：{1}', '現有檔案意外變更：{0}。備份副本：{1}', 'Existing file changed unexpectedly: {0}. Recovery copies: {1}')
    ServiceAppeared = @('安装期间出现了 sshd 服务，请重新加载后继续。', '安裝期間出現了 sshd 服務，請重新載入後繼續。', 'An sshd service appeared during installation. Reload before continuing.')
    SignatureFailed = @('Microsoft 签名验证失败：{0}', 'Microsoft 簽章驗證失敗：{0}', 'Microsoft signature verification failed: {0}')
    ReadEffectiveConfig = @('读取有效的 OpenSSH 配置', '讀取有效的 OpenSSH 設定', 'Read effective OpenSSH configuration')
    NoPort = @('有效 OpenSSH 配置中未找到监听端口。', '有效 OpenSSH 設定中找不到接聽連接埠。', 'No listening port found in effective OpenSSH configuration.')
    RegisterEvents = @('注册事件提供程序', '註冊事件提供者', 'Register event provider')
    ServicePrivileges = @('配置服务权限', '設定服務權限', 'Configure service privileges')
    ManifestMissing = @('缺少发布清单。请完整解压发布 ZIP，并从该目录运行 Install.cmd。', '缺少發佈資訊清單。請完整解壓縮發佈 ZIP，並從該目錄執行 Install.cmd。', 'Release manifest missing. Fully extract the release ZIP and run Install.cmd from that directory.')
    FreshDirectory = @('请从新解压的发布目录运行安装程序，不要从 Program Files 安装目录运行。', '請從新解壓縮的發佈目錄執行安裝程式，請勿從 Program Files 安裝目錄執行。', 'Run the installer from a freshly extracted release directory, not the installed Program Files directory.')
    ExistingService = @('检测到 sshd 服务（{0}）：仅安装或更新管理界面。', '偵測到 sshd 服務（{0}）：僅安裝或更新管理介面。', 'sshd service detected ({0}): installing or updating the GUI only.')
    PreserveEngine = @('现有 SSH 引擎、服务状态、配置和密钥保持不变。', '現有 SSH 引擎、服務狀態、設定和金鑰保持不變。', 'Existing SSH engine, service state, configuration and keys are preserved.')
    GuiOnly = @('已指定 -GuiOnly：仅安装管理界面；当前没有 sshd 服务。', '已指定 -GuiOnly：僅安裝管理介面；目前沒有 sshd 服務。', '-GuiOnly specified: installing the GUI only; no sshd service exists.')
    FullInstall = @('未检测到 sshd 服务：安装引擎和管理界面，并注册、启动服务。', '未偵測到 sshd 服務：安裝引擎和管理介面，並註冊、啟動服務。', 'No sshd service found: installing the engine and GUI, then registering and starting the service.')
    DestinationUnowned = @('目标已存在，但没有安装标记。', '目標已存在，但沒有安裝標記。', 'Destination exists without an installation marker.')
    MarkerInvalid = @('安装标记无效。', '安裝標記無效。', 'Invalid installation marker.')
    ExitBeforeUpdate = @('更新前请从托盘菜单退出 OpenSSH Manager。', '更新前請從系統匣選單結束 OpenSSH Manager。', 'Exit OpenSSH Manager from its tray menu before updating.')
    PackageIncomplete = @('发布包不完整：{0}。请完整解压新 ZIP 并运行其中的 Install.cmd。', '發佈套件不完整：{0}。請完整解壓縮新 ZIP 並執行其中的 Install.cmd。', 'Incomplete release package: {0}. Fully extract the new ZIP and run its Install.cmd.')
    ManifestPath = @('包清单路径无效。', '套件資訊清單路徑無效。', 'Invalid package manifest path.')
    PackageChecksum = @('包校验和不匹配：{0}', '套件檢查碼不符：{0}', 'Package checksum mismatch: {0}')
    PackageFileMissing = @('缺少包文件：{0}', '缺少套件檔案：{0}', 'Missing package file: {0}')
    CheckEnginePath = @('正在检查用于 SSH/SFTP 会话的现有托管引擎 PATH。', '正在檢查用於 SSH/SFTP 工作階段的現有受管理引擎 PATH。', 'Checking the existing managed engine PATH for SSH/SFTP sessions.')
    Installed = @('已安装：{0}', '已安裝：{0}', 'Installed: {0}')
    LaunchNormalUser = @('请以普通桌面用户身份从开始菜单打开 OpenSSH Manager。', '請以一般桌面使用者身分從開始功能表開啟 OpenSSH Manager。', 'Launch OpenSSH Manager from the Start menu as your normal desktop user.')
    BundledEngine = @('内置引擎：Microsoft Win32-OpenSSH 10.4.0.0p1-Preview。无需 Cygwin 或 .NET 图形运行时。', '內建引擎：Microsoft Win32-OpenSSH 10.4.0.0p1-Preview。無需 Cygwin 或 .NET 圖形執行階段。', 'Bundled engine: Microsoft Win32-OpenSSH 10.4.0.0p1-Preview. No Cygwin or .NET GUI runtime is required.')
    Windows64 = @('此安装包需要 64 位 Windows。', '此安裝套件需要 64 位元 Windows。', 'This package requires 64-bit Windows.')
    EngineElevation = @('引擎安装必须由提权的界面辅助进程启动。', '引擎安裝必須由已提升權限的介面輔助處理程序啟動。', 'Engine installation must be launched by the elevated GUI worker.')
    BootstrapFailed = @('管理员授权或 64 位 PowerShell 启动失败。', '系統管理員授權或 64 位元 PowerShell 啟動失敗。', 'Administrator / 64-bit PowerShell bootstrap did not succeed.')
    RequestElevation = @('正在请求管理员权限。取消 UAC 将取消安装。', '正在請求系統管理員權限。取消 UAC 將取消安裝。', 'Requesting administrator access. Canceling UAC cancels installation.')
    InstallBusyWait = @('另一项 OpenSSH Manager 安装或卸载正在运行，请等待其完成。', '另一項 OpenSSH Manager 安裝或解除安裝正在執行，請等待其完成。', 'Another OpenSSH Manager installation or removal is running. Wait for it to finish.')
    EngineComplete = @('SSH 引擎安装完成。', 'SSH 引擎安裝完成。', 'SSH engine installation completed.')
    InstallComplete = @('安装完成。请从开始菜单打开 OpenSSH Manager。', '安裝完成。請從開始功能表開啟 OpenSSH Manager。', 'Installation complete. Open OpenSSH Manager from the Start menu.')
    ElevationCanceled = @('已取消管理员授权，未开始安装。', '已取消系統管理員授權，未開始安裝。', 'Administrator approval canceled; installation did not start.')
    InstallIncomplete = @('安装未完成：', '安裝未完成：', 'Installation incomplete:')
    KeepError = @('请保留以上错误。若提示程序正在运行，请先从托盘退出旧版，再重试。', '請保留以上錯誤。若提示程式正在執行，請先從系統匣結束舊版，再重試。', 'Keep the errors above. If the app is running, exit the old version from its tray before retrying.')
    InstallBusy = @('另一项 OpenSSH Manager 安装或卸载正在运行。', '另一項 OpenSSH Manager 安裝或解除安裝正在執行。', 'Another OpenSSH Manager installation or removal is running.')
    MarkerMissing = @('找不到安装标记。', '找不到安裝標記。', 'Installation marker not found.')
    ReparseUnsupported = @('不支持重解析点。', '不支援重新剖析點。', 'Reparse point not supported.')
    ExitBeforeUninstall = @('卸载前请从托盘菜单退出 OpenSSH Manager。', '解除安裝前請從系統匣選單結束 OpenSSH Manager。', 'Exit OpenSSH Manager from the tray menu before uninstalling.')
    ServiceOwnership = @('sshd 注册信息现已属于其他安装，拒绝删除。', 'sshd 註冊資訊現已屬於其他安裝，拒絕刪除。', 'The sshd registration now belongs to another installation; refusing to remove it.')
    ServiceRemoval = @('服务删除失败。', '服務刪除失敗。', 'Service removal failed.')
    GuiRemoved = @('已删除界面及本次安装拥有的 SSH 服务。', '已刪除介面及此次安裝擁有的 SSH 服務。', 'GUI removed. Any SSH service owned by this installation was removed.')
    DataPreserved = @('主机密钥、账号密钥、配置、恢复副本和事件资源已保留。', '主機金鑰、帳號金鑰、設定、備份副本和事件資源已保留。', 'Host keys, account keys, configuration, recovery copies and event resources were preserved.')
    OtherStartup = @('其他用户可从 Windows 启动应用中删除其托盘自启动项。', '其他使用者可從 Windows 啟動應用程式中刪除其系統匣自動啟動項目。', 'Other users may remove their tray auto-start entry from Windows Startup Apps.')
    EngineDigest = @('引擎摘要无效。', '引擎摘要無效。', 'Invalid engine digest.')
    ZipChecksum = @('引擎 ZIP 校验和不匹配。', '引擎 ZIP 檢查碼不符。', 'Engine ZIP checksum mismatch.')
    ArchiveEntries = @('引擎压缩包条目过多。', '引擎壓縮檔項目過多。', 'Too many engine archive entries.')
    ArchiveEntry = @('不符合预期的压缩包条目：{0}', '不符預期的壓縮檔項目：{0}', 'Unexpected archive entry: {0}')
    ArchivePath = @('压缩包路径不安全。', '壓縮檔路徑不安全。', 'Unsafe archive path.')
    ArchiveLinks = @('不支持压缩包内的链接。', '不支援壓縮檔內的連結。', 'Archive links are not supported.')
    ArchiveDuplicate = @('压缩包路径重复。', '壓縮檔路徑重複。', 'Duplicate archive path.')
    ArchiveWindowsPath = @('压缩包中的 Windows 路径无效。', '壓縮檔中的 Windows 路徑無效。', 'Invalid Windows archive path.')
    ArchiveSize = @('引擎压缩包过大。', '引擎壓縮檔過大。', 'Engine archive is too large.')
    ZipIncomplete = @('引擎 ZIP 不完整：{0}', '引擎 ZIP 不完整：{0}', 'Engine ZIP is incomplete: {0}')
    ExtractionTarget = @('解压目标已存在。', '解壓縮目標已存在。', 'Archive extraction target already exists.')
    RepairOwnership = @('拒绝修复：服务路径和安装归属标记必须与本程序匹配。', '拒絕修復：服務路徑和安裝歸屬標記必須與本程式相符。', 'Repair refused: service path and installation ownership marker must match this manager.')
    RepairRequirements = @('修复需要现有的自有服务和已验证的引擎 ZIP。', '修復需要現有的自有服務及已驗證的引擎 ZIP。', 'Repair requires an existing owned service and a verified engine ZIP.')
    ExistingUnowned = @('现有安装目录没有归属标记，已停止自动安装。', '現有安裝目錄沒有歸屬標記，已停止自動安裝。', 'Existing installation directory has no ownership marker; automatic installation stopped.')
    NoReusableEngine = @('找不到可重用的引擎。', '找不到可重複使用的引擎。', 'No reusable engine was found.')
    InstalledChecksum = @('已安装引擎的校验和不匹配。', '已安裝引擎的檢查碼不符。', 'Installed engine checksum mismatch.')
    EnvironmentKey = @('找不到系统环境注册表项。', '找不到系統環境登錄機碼。', 'System environment registry key not found.')
    PathType = @('系统 PATH 的注册表类型不受支持。', '系統 PATH 的登錄類型不受支援。', 'System PATH has an unsupported registry type.')
    PathChanged = @('安装期间系统 PATH 已变更。请重试，不要覆盖其他修改。', '安裝期間系統 PATH 已變更。請重試，請勿覆寫其他修改。', 'System PATH changed during installation. Retry without overwriting the other change.')
    PathSftpMissing = @('无法注册引擎 PATH：缺少 sftp-server.exe。', '無法註冊引擎 PATH：缺少 sftp-server.exe。', 'Cannot register engine PATH: sftp-server.exe is missing.')
    PathRegistered = @('已将引擎目录注册到系统 PATH，供新的 SSH/SFTP 会话使用。', '已將引擎目錄註冊至系統 PATH，供新的 SSH/SFTP 工作階段使用。', 'Registered the engine directory in system PATH for new SSH/SFTP sessions.')
    PathRemoved = @('已移除此安装添加的系统 PATH 项。', '已移除此安裝新增的系統 PATH 項目。', 'Removed the system PATH entry added by this installation.')
}
