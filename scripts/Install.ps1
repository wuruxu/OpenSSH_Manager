#Requires -Version 5.1
[CmdletBinding()]
param([switch]$GuiOnly, [switch]$Interactive, [switch]$Elevated, [switch]$EngineOnly, [string]$Archive, [switch]$Repair)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
function Get-InstallMode([bool]$ServiceExists, [bool]$RequestedGuiOnly) {
    if ($RequestedGuiOnly -or $ServiceExists) { return 'GuiOnly' }
    return 'Full'
}
function Get-ExistingSshService {
    try { return Get-Service -Name sshd -ErrorAction Stop }
    catch {
        # Access-denied/SCM errors must not be mistaken for an absent service.
        if ($_.FullyQualifiedErrorId -notlike 'NoServiceFoundForGivenName,*') { throw }
        return $null
    }
}
function Get-ElevationArguments([string]$ScriptPath, [bool]$OnlyGui, [bool]$ShowResult) {
    # Encode the entire invocation; apostrophes in a path remain literal PowerShell data.
    $command = "& '" + $ScriptPath.Replace("'", "''") + "' -Elevated"
    if ($OnlyGui) { $command += ' -GuiOnly' }
    if ($ShowResult) { $command += ' -Interactive' }
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    return "-NoProfile -ExecutionPolicy Bypass -EncodedCommand $encoded"
}
function Wait-InstallerResult {
    if ($Interactive) { [void](Read-Host '按 Enter 关闭安装窗口') }
}

function Assert-NoReparse([string]$Path) {
    $itemPath = [IO.Path]::GetFullPath($Path)
    while ($itemPath) {
        if (Test-Path -LiteralPath $itemPath) {
            if ((Get-Item -LiteralPath $itemPath -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Reparse point is not supported: $itemPath"
            }
        }
        $itemPath = [IO.Path]::GetDirectoryName($itemPath)
    }
}
function Set-ProtectedAcl([string]$Path, [switch]$Directory, [switch]$Private) {
    $acl = if ($Directory) { New-Object Security.AccessControl.DirectorySecurity } else { New-Object Security.AccessControl.FileSecurity }
    $inherit = if ($Directory) { 'OICI' } else { '' }
    $sddl = "O:BAG:BAD:P(A;${inherit};FA;;;SY)(A;${inherit};FA;;;BA)"
    if (-not $Private) { $sddl += "(A;${inherit};0x1200a9;;;BU)" }
    $acl.SetSecurityDescriptorSddlForm($sddl)
    Set-Acl -LiteralPath $Path -AclObject $acl
}
function Check-Native([string]$Operation) {
    if ($LASTEXITCODE -ne 0) { throw "$Operation failed with exit code $LASTEXITCODE" }
}
function Invoke-HostKeyGeneration([string]$EngineDirectory) {
    & (Join-Path $EngineDirectory 'ssh-keygen.exe') -A | Out-Host
    Check-Native 'Host key generation'
}
function Invoke-ConfigValidation([string]$EngineDirectory, [string]$ConfigPath) {
    & (Join-Path $EngineDirectory 'sshd.exe') -t -f $ConfigPath | Out-Host
    Check-Native 'OpenSSH configuration validation'
}
function Register-EnginePath([string]$EngineDirectory, $State, [string]$StateFile) {
    $sftp = Join-Path $EngineDirectory 'sftp-server.exe'
    Assert-NoReparse $sftp
    $signature = Get-AuthenticodeSignature -LiteralPath $sftp
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
        throw 'Microsoft signature verification failed: sftp-server.exe'
    }
    & $sftp -Q requests | Out-Null
    Check-Native 'SFTP engine startup check'
    Add-EngineToMachinePath $EngineDirectory $State $StateFile
}
function Initialize-SshData([string]$EngineDirectory, [string]$DataDirectory) {
    Assert-NoReparse $DataDirectory
    if ((Test-Path -LiteralPath $DataDirectory) -and -not (Test-Path -LiteralPath $DataDirectory -PathType Container)) {
        throw "SSH data path is not a directory: $DataDirectory"
    }
    $config = Join-Path $DataDirectory 'sshd_config'
    # Reject links before changing inherited directory permissions or running keygen.
    $existingFiles = @()
    if (Test-Path -LiteralPath $DataDirectory) {
        foreach ($entry in @(Get-ChildItem -LiteralPath $DataDirectory -Force -Recurse)) {
            Assert-NoReparse $entry.FullName
        }
        foreach ($entry in @(Get-ChildItem -LiteralPath $DataDirectory -Force)) {
            if ($entry.Name -eq 'sshd_config' -or $entry.Name -eq 'administrators_authorized_keys' -or
                $entry.Name -like 'ssh_host_*_key' -or $entry.Name -like 'ssh_host_*_key.pub') {
                if ($entry.PSIsContainer) { throw "Expected an SSH configuration/key file: $($entry.FullName)" }
                $existingFiles += $entry
            }
        }
        # ssh-keygen -A would replace an orphaned .pub when generating its missing private key.
        foreach ($type in @('rsa', 'ecdsa', 'ed25519')) {
            $privatePath = Join-Path $DataDirectory "ssh_host_${type}_key"
            if ((Test-Path -LiteralPath "$privatePath.pub") -and -not (Test-Path -LiteralPath $privatePath)) {
                throw "Public host key has no matching private key: $privatePath.pub. Restore the private key before installing; no existing key was replaced."
            }
        }
    }
    $backupDirectory = $null
    $snapshots = @()
    if ($existingFiles.Count) {
        $backupRoot = $DataDirectory + '.manager-backups'
        Assert-NoReparse $backupRoot
        New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
        Set-ProtectedAcl $backupRoot -Directory -Private
        $backupDirectory = Join-Path $backupRoot ([Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $backupDirectory | Out-Null
        Set-ProtectedAcl $backupDirectory -Directory -Private
        foreach ($entry in $existingFiles) {
            $hash = (Get-FileHash -LiteralPath $entry.FullName -Algorithm SHA256).Hash
            $backupPath = Join-Path $backupDirectory $entry.Name
            Copy-Item -LiteralPath $entry.FullName -Destination $backupPath
            Set-ProtectedAcl $backupPath -Private
            if ((Get-FileHash -LiteralPath $backupPath -Algorithm SHA256).Hash -ne $hash) {
                throw "File changed during backup: $($entry.FullName). Installation stopped."
            }
            $snapshots += @{ Path = $entry.FullName; Hash = $hash }
        }
        Write-Host "Existing configuration and keys backed up to: $backupDirectory"
    }
    New-Item -ItemType Directory -Path $DataDirectory -Force | Out-Null
    Set-ProtectedAcl $DataDirectory -Directory
    if (-not (Test-Path -LiteralPath $config)) {
        $defaultConfig = [IO.File]::ReadAllText((Join-Path $EngineDirectory 'sshd_config_default'))
        $defaultConfig = [regex]::Replace($defaultConfig, '(?m)^\s*Subsystem\s+sftp\s+.*$', 'Subsystem sftp internal-sftp')
        # CreateNew refuses to overwrite a config created concurrently by another process.
        $stream = [IO.File]::Open($config, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try {
            $bytes = (New-Object Text.UTF8Encoding $false).GetBytes($defaultConfig)
            $stream.Write($bytes, 0, $bytes.Length)
        } finally { $stream.Dispose() }
        Write-Host "Created configuration: $config"
    } else { Write-Host "Preserving existing configuration: $config" }
    Set-ProtectedAcl $config
    # Existing private keys are retained by -A. Only missing host keys are generated.
    Invoke-HostKeyGeneration $EngineDirectory
    Get-ChildItem -LiteralPath $DataDirectory -Filter 'ssh_host_*_key' -File | ForEach-Object { Set-ProtectedAcl $_.FullName -Private }
    Get-ChildItem -LiteralPath $DataDirectory -Filter 'ssh_host_*_key.pub' -File | ForEach-Object { Set-ProtectedAcl $_.FullName }
    $adminKeys = Join-Path $DataDirectory 'administrators_authorized_keys'
    if (Test-Path -LiteralPath $adminKeys) { Set-ProtectedAcl $adminKeys -Private }
    foreach ($snapshot in $snapshots) {
        if ((Get-FileHash -LiteralPath $snapshot.Path -Algorithm SHA256).Hash -ne $snapshot.Hash) {
            throw "Existing file changed unexpectedly: $($snapshot.Path). Recovery copies: $backupDirectory"
        }
    }
    Invoke-ConfigValidation $EngineDirectory $config
    return $config
}

function Install-EngineService([string]$destination, [string]$dataDirectory, $state, [string]$stateFile) {
    if (Get-ExistingSshService) { throw 'An sshd service appeared during installation. Reload before continuing.' }
    $engine = Join-Path $destination 'engine'
    $sshd = Join-Path $engine 'sshd.exe'
    foreach ($file in @('sshd.exe', 'sshd-auth.exe', 'sshd-session.exe', 'ssh-keygen.exe', 'sftp-server.exe', 'ssh-agent.exe', 'libcrypto.dll')) {
        Assert-NoReparse (Join-Path $engine $file)
        $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $engine $file)
        if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
            throw "Microsoft signature verification failed: $file"
        }
    }
    $config = Initialize-SshData $engine $dataDirectory
    $effectiveConfig = & $sshd -T -f $config
    Check-Native 'Read effective OpenSSH configuration'
    $ports = @($effectiveConfig | Where-Object { $_ -match '^port ([0-9]+)$' } | ForEach-Object { ($_ -split '\s+')[1] } | Select-Object -Unique)
    if (-not $ports.Count) { throw 'No listening port found in effective OpenSSH configuration.' }
    Register-EnginePath $engine $state $stateFile

    # Preserve an existing OpenSSH event provider (and the existing ssh-agent service).
    $provider = Get-WinEvent -ListProvider OpenSSH -ErrorAction SilentlyContinue
    if (-not $provider) {
        $manifest = Join-Path $engine 'openssh-events.man'
        [xml]$xml = Get-Content -LiteralPath $manifest -Raw
        $xml.instrumentationManifest.instrumentation.events.provider.resourceFileName = Join-Path $engine 'ssh-agent.exe'
        $xml.instrumentationManifest.instrumentation.events.provider.messageFileName = Join-Path $engine 'ssh-agent.exe'
        $xml.Save($manifest)
        & "$env:SystemRoot\System32\wevtutil.exe" im $manifest
        Check-Native 'Register event provider'
        $state.RegisteredEvents = $true
        $state | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding UTF8
    }
    # Save ownership immediately, so a failed start still has a recoverable/uninstallable registration.
    New-Service -Name sshd -DisplayName 'OpenSSH SSH Server' -BinaryPathName ('"' + $sshd + '"') -StartupType Automatic | Out-Null
    $state.OwnsService = $true
    $state | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding UTF8
    & "$env:SystemRoot\System32\sc.exe" privs sshd SeAssignPrimaryTokenPrivilege/SeTcbPrivilege/SeBackupPrivilege/SeRestorePrivilege/SeImpersonatePrivilege
    Check-Native 'Configure service privileges'
    # Match the upstream RedirectionGuard mitigation, without overwriting existing settings.
    $mitigationPath = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\sshd.exe'
    if (-not (Test-Path -LiteralPath $mitigationPath)) { New-Item -Path $mitigationPath -Force | Out-Null }
    if (-not (Get-ItemProperty -Path $mitigationPath -Name MitigationOptions -ErrorAction SilentlyContinue)) {
        [byte[]]$mitigation = @(0) * 19; $mitigation[18] = 0x10
        New-ItemProperty -Path $mitigationPath -Name MitigationOptions -PropertyType Binary -Value $mitigation | Out-Null
    }
    Start-Service sshd
    (Get-Service sshd).WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
    if (Get-NetFirewallRule -Name 'OpenSSHManager-Server' -ErrorAction SilentlyContinue) {
        Set-NetFirewallRule -Name 'OpenSSHManager-Server' -Direction Inbound -Action Allow -Protocol TCP -LocalPort $ports -Program $sshd -Profile Domain,Private -RemoteAddress LocalSubnet -Enabled True | Out-Null
    } else {
        New-NetFirewallRule -Name 'OpenSSHManager-Server' -DisplayName 'OpenSSHManager-Server' -Direction Inbound -Action Allow -Protocol TCP -LocalPort $ports -Program $sshd -Profile Domain,Private -RemoteAddress LocalSubnet | Out-Null
    }
}

function Invoke-Installation {
    $destination = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'OpenSSHManager'
    $dataDirectory = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'ssh'
    $stateFile = Join-Path $destination 'InstallState.json'
    $manifestPath = Join-Path $PSScriptRoot 'package-hashes.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw 'Release manifest missing. Fully extract the release ZIP and run Install.cmd from that directory.'
    }
    if ([IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\') -eq [IO.Path]::GetFullPath($destination).TrimEnd('\')) {
        throw 'Run the installer from a freshly extracted release directory, not the installed Program Files directory.'
    }
    Assert-NoReparse $destination
    Assert-NoReparse $dataDirectory
    $requestedGuiOnly = [bool]$GuiOnly
    $existingService = Get-ExistingSshService
    $mode = Get-InstallMode ([bool]$existingService) ([bool]$GuiOnly)
    $GuiOnly = $mode -eq 'GuiOnly'
    if ($existingService) {
        Write-Host "检测到 sshd 服务（$($existingService.Status)）：仅安装或更新管理界面。" -ForegroundColor Cyan
        Write-Host '现有 SSH 引擎、服务状态、配置和密钥保持不变。'
    } elseif ($GuiOnly) {
        Write-Host '已指定 -GuiOnly：仅安装管理界面；当前没有 sshd 服务。' -ForegroundColor Yellow
    } else {
        Write-Host '未检测到 sshd 服务：安装引擎和管理界面，并注册、启动服务。' -ForegroundColor Cyan
    }
    $oldState = $null
    if (Test-Path -LiteralPath $destination) {
        if (-not (Test-Path -LiteralPath $stateFile)) { throw 'Destination exists without an installation marker.' }
        Assert-NoReparse $stateFile
        $oldState = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
        if ($oldState.Product -ne 'OpenSSHManager' -or $oldState.Directory -ne $destination) { throw 'Invalid installation marker.' }
        # With no registered sshd service, a previous interrupted installation can be resumed.
        $runningGui = @(Get-Process -Name OpenSSHManager -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $destination 'OpenSSHManager.exe') })
        if ($runningGui.Count) { throw 'Exit OpenSSH Manager from its tray menu before updating.' }
    }

    # Keep the expected hashes in memory, then verify protected destination files before execution.
    $hashes = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'package-hashes.json') -Raw | ConvertFrom-Json
    $rootFiles = @('OpenSSHManager.exe', 'OpenSSHManager.svg', 'README.md', 'LICENSE', 'engine.lock.json', 'Install.cmd', 'Install.ps1', 'Uninstall.ps1', 'EnginePath.ps1', 'EngineArchive.ps1', 'Test-Windows.ps1')
    # Validate the complete copy set before creating the installation marker or changing files.
    foreach ($required in $rootFiles) {
        if ($hashes.PSObject.Properties.Name -notcontains $required -or -not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $required) -PathType Leaf)) {
            throw "Incomplete release package: $required. Fully extract the new ZIP and run its Install.cmd."
        }
    }
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Set-ProtectedAcl $destination -Directory
    $state = [pscustomobject][ordered]@{
        Product = 'OpenSSHManager'; Version = '0.1.22'; Directory = $destination
        OwnsService = $false; RegisteredEvents = $false; EnginePathAdded = $false
    }
    if ($oldState) {
        $state.OwnsService = [bool]$oldState.OwnsService
        $state.RegisteredEvents = [bool]$oldState.RegisteredEvents
        if ($oldState.PSObject.Properties['EnginePathAdded']) { $state.EnginePathAdded = [bool]$oldState.EnginePathAdded }
    }
    # Record ownership before copying/validation, so failures can be retried without deleting files.
    $state | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding UTF8
    Set-ProtectedAcl $stateFile
    $copied = @()
    foreach ($property in $hashes.PSObject.Properties) {
        $relative = $property.Name
        if ($relative -match '(^[/\\]|(^|[/\\])\.\.([/\\]|$)|:)' ) { throw 'Invalid package manifest path.' }
        if ($rootFiles -notcontains $relative -and ($GuiOnly -or -not $relative.StartsWith('engine/'))) { continue }
        $source = Join-Path $PSScriptRoot $relative
        $target = Join-Path $destination $relative
        Assert-NoReparse $target
        $parent = Split-Path -Parent $target
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $target -Force
        if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $property.Value) { throw "Package checksum mismatch: $relative" }
        $copied += $relative
    }
    foreach ($required in $rootFiles) { if ($copied -notcontains $required) { throw "Missing package file: $required" } }
    # Load the helper only after copying it to the protected directory and verifying its hash.
    . (Join-Path $destination 'EnginePath.ps1')
    $engine = Join-Path $destination 'engine'
    if ($GuiOnly -and -not $requestedGuiOnly -and $existingService -and $state.OwnsService) {
        $registration = Get-CimInstance Win32_Service -Filter "Name='sshd'"
        if ($registration -and (Test-OwnedEngineService $state $registration.PathName $engine)) {
            Write-Host 'Checking the existing managed engine PATH for SSH/SFTP sessions.'
            Register-EnginePath $engine $state $stateFile
        }
    }
    if (-not $GuiOnly) { Install-EngineService $destination $dataDirectory $state $stateFile }
    $state | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding UTF8
    Set-ProtectedAcl $stateFile
    $shortcutPath = Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'OpenSSH Manager.lnk'
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($shortcutPath)
    $shortcut.TargetPath = Join-Path $destination 'OpenSSHManager.exe'
    $shortcut.Arguments = if ($requestedGuiOnly) { '--no-auto-install' } else { '' }
    $shortcut.WorkingDirectory = $destination
    $shortcut.Save()
    Write-Host "Installed: $destination"
    Write-Host 'Launch OpenSSH Manager from the Start menu as your normal desktop user.'
    Write-Host 'Bundled engine: Microsoft Win32-OpenSSH 10.4.0.0p1-Preview. No Cygwin or .NET GUI runtime is required.'
}

# Entry point: bootstrap before any protected writes, then re-detect after elevation.
try {
    if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitOperatingSystem) {
        throw 'This package requires 64-bit Windows.'
    }
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    try {
        $principal = New-Object Security.Principal.WindowsPrincipal $identity
        $administrator = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    } finally { $identity.Dispose() }
    if (-not $administrator -or -not [Environment]::Is64BitProcess) {
        if ($EngineOnly) { throw 'Engine installation must be launched by the elevated GUI worker.' }
        if ($Elevated) { throw 'Administrator / 64-bit PowerShell bootstrap did not succeed.' }
        $psDirectory = if ([Environment]::Is64BitProcess) { 'System32' } else { 'Sysnative' }
        $powershell = Join-Path $env:SystemRoot "$psDirectory\WindowsPowerShell\v1.0\powershell.exe"
        Write-Host '正在请求管理员权限。取消 UAC 将取消安装。'
        $arguments = Get-ElevationArguments $PSCommandPath ([bool]$GuiOnly) ([bool]$Interactive)
        $child = Start-Process -FilePath $powershell -ArgumentList $arguments -Verb RunAs -Wait -PassThru
        exit $child.ExitCode
    }
    # Serialize installation/removal so service, PATH and ownership updates cannot overlap.
    $mutex = New-Object Threading.Mutex($false, 'Global\OpenSSHManager.Install')
    $locked = $false
    try {
        try { $locked = $mutex.WaitOne(0) }
        catch [Threading.AbandonedMutexException] { $locked = $true }
        if (-not $locked) { throw 'Another OpenSSH Manager installation or removal is running. Wait for it to finish.' }
        if ($EngineOnly) {
            [Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
            . (Join-Path $PSScriptRoot 'EnginePath.ps1')
            . (Join-Path $PSScriptRoot 'EngineArchive.ps1')
            Invoke-EngineInstallation $Archive -Repair:$Repair
        } else { Invoke-Installation }
    } finally {
        if ($locked) { $mutex.ReleaseMutex() }
        $mutex.Dispose()
    }
    Write-Host ''
    if ($EngineOnly) { Write-Host 'SSH engine installation completed.' }
    else { Write-Host '安装完成。请从开始菜单打开 OpenSSH Manager。' -ForegroundColor Green }
    Wait-InstallerResult
    exit 0
} catch {
    $cancelled = $false
    $exception = $_.Exception
    while ($exception) {
        if ($exception -is [ComponentModel.Win32Exception] -and $exception.NativeErrorCode -eq 1223) { $cancelled = $true }
        $exception = $exception.InnerException
    }
    Write-Host ''
    if ($cancelled) {
        Write-Host '已取消管理员授权，未开始安装。' -ForegroundColor Yellow
    } else {
        Write-Host '安装未完成：' -ForegroundColor Red
        Write-Host ($_ | Out-String) -ForegroundColor Red
        Write-Host '请保留以上错误。若提示程序正在运行，请先从托盘退出旧版，再重试。'
    }
    Wait-InstallerResult
    if ($cancelled) { exit 1223 }
    exit 1
}
