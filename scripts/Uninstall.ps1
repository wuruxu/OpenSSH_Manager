#Requires -Version 5.1
#Requires -RunAsAdministrator
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$mutex = New-Object Threading.Mutex($false, 'Global\OpenSSHManager.Install')
$locked = $false
try {
    try { $locked = $mutex.WaitOne(0) }
    catch [Threading.AbandonedMutexException] { $locked = $true }
    if (-not $locked) { throw 'Another OpenSSH Manager installation or removal is running.' }
    $destination = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'OpenSSHManager'
    $stateFile = Join-Path $destination 'InstallState.json'
    if (-not (Test-Path -LiteralPath $stateFile)) { throw 'Installation marker not found.' }
    foreach ($path in @($destination, $stateFile)) {
        if ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse point not supported.' }
    }
    $state = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
    if ($state.Product -ne 'OpenSSHManager' -or $state.Directory -ne $destination) { throw 'Invalid installation marker.' }
    if (@(Get-Process -Name OpenSSHManager -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $destination 'OpenSSHManager.exe') }).Count) {
        throw 'Exit OpenSSH Manager from the tray menu before uninstalling.'
    }
    if ($state.OwnsService) {
        $service = Get-CimInstance Win32_Service -Filter "Name='sshd'"
        if ($service) {
            $expected = '"' + (Join-Path $destination 'engine\sshd.exe') + '"'
            if ($service.PathName -ne $expected) { throw 'The sshd registration now belongs to another installation; refusing to remove it.' }
            Stop-Service sshd
            (Get-Service sshd).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
            & "$env:SystemRoot\System32\sc.exe" delete sshd
            if ($LASTEXITCODE -ne 0) { throw 'Service removal failed.' }
        }
        Get-NetFirewallRule -Name 'OpenSSHManager-Server' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    }
    # Keep the event resource files/provider for historical events and any existing ssh-agent.
    # Keep all host keys, authorized_keys and configuration under ProgramData\ssh.
    if ($state.PSObject.Properties['EnginePathAdded'] -and $state.EnginePathAdded) {
        $pathHelper = Join-Path $destination 'EnginePath.ps1'
        if ((Get-Item -LiteralPath $pathHelper -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse point not supported.' }
        . $pathHelper
        Remove-EngineFromMachinePath (Join-Path $destination 'engine') $state $stateFile
    }
    Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name OpenSSHManager -ErrorAction SilentlyContinue
    $shortcut = Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'OpenSSH Manager.lnk'
    if (Test-Path -LiteralPath $shortcut) { Remove-Item -LiteralPath $shortcut }
    $gui = Join-Path $destination 'OpenSSHManager.exe'
    if (Test-Path -LiteralPath $gui) { Remove-Item -LiteralPath $gui }
    Write-Host 'GUI removed. Any SSH service owned by this installation was removed.'
    Write-Host 'Host keys, account keys, configuration, recovery copies and event resources were preserved.'
    Write-Host 'Other users may remove their tray auto-start entry from Windows Startup Apps.'
} finally {
    if ($locked) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
