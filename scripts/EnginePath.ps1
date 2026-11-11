. (Join-Path $PSScriptRoot 'Localization.ps1')
# Shared installer/uninstaller helpers. No changes occur when this file is loaded.
# Read the raw registry value so %SystemRoot% and REG_EXPAND_SZ are preserved.
function Get-EnginePathIdentity([string]$Value) {
    return [Environment]::ExpandEnvironmentVariables($Value.Trim().Trim('"')).Replace('/', '\').TrimEnd('\')
}
function Get-MachinePathSnapshot {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $key = $base.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager\Environment', $false)
        if (-not $key) { throw (Get-SetupText 'EnvironmentKey') }
        try {
            $value = $key.GetValue('Path', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            $kind = if ($null -eq $value) { [Microsoft.Win32.RegistryValueKind]::ExpandString } else { $key.GetValueKind('Path') }
            if ($kind -notin @([Microsoft.Win32.RegistryValueKind]::String, [Microsoft.Win32.RegistryValueKind]::ExpandString)) {
                throw (Get-SetupText 'PathType')
            }
            return [pscustomobject]@{ Value = $value; Kind = $kind }
        } finally { $key.Dispose() }
    } finally { $base.Dispose() }
}
function Set-MachinePathSnapshot($Snapshot, [string]$Value) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $key = $base.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager\Environment', $true)
        if (-not $key) { throw (Get-SetupText 'EnvironmentKey') }
        try {
            $current = $key.GetValue('Path', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            if ($current -cne $Snapshot.Value -or ($null -ne $current -and $key.GetValueKind('Path') -ne $Snapshot.Kind)) {
                throw (Get-SetupText 'PathChanged')
            }
            $key.SetValue('Path', $Value, $Snapshot.Kind)
        } finally { $key.Dispose() }
    } finally { $base.Dispose() }
}
function Save-EnginePathState($State, [string]$StateFile) {
    $State | ConvertTo-Json | Set-Content -LiteralPath $StateFile -Encoding UTF8
}
function Add-EngineToMachinePath([string]$EngineDirectory, $State, [string]$StateFile) {
    if (-not (Test-Path -LiteralPath (Join-Path $EngineDirectory 'sftp-server.exe') -PathType Leaf)) {
        throw (Get-SetupText 'PathSftpMissing')
    }
    $snapshot = Get-MachinePathSnapshot
    $identity = Get-EnginePathIdentity $EngineDirectory
    foreach ($entry in ([string]$snapshot.Value -split ';')) {
        if ((Get-EnginePathIdentity $entry) -eq $identity) { return } # Do not claim a user's existing entry.
    }
    $previous = [bool]$State.EnginePathAdded
    $State.EnginePathAdded = $true
    # Persist ownership first so a later service-start failure remains uninstallable.
    Save-EnginePathState $State $StateFile
    $value = if ([string]::IsNullOrEmpty($snapshot.Value)) { $EngineDirectory } else { $snapshot.Value + ';' + $EngineDirectory }
    try { Set-MachinePathSnapshot $snapshot $value }
    catch {
        $State.EnginePathAdded = $previous
        Save-EnginePathState $State $StateFile
        throw
    }
    Write-Host (Get-SetupText 'PathRegistered')
}
function Remove-EngineFromMachinePath([string]$EngineDirectory, $State, [string]$StateFile) {
    # Old installations and manually registered entries do not belong to us.
    if (-not $State.PSObject.Properties['EnginePathAdded'] -or -not $State.EnginePathAdded) { return }
    $snapshot = Get-MachinePathSnapshot
    $identity = Get-EnginePathIdentity $EngineDirectory
    $entries = @([string]$snapshot.Value -split ';')
    $index = -1
    for ($i = 0; $i -lt $entries.Count; $i++) {
        if ((Get-EnginePathIdentity $entries[$i]) -eq $identity) { $index = $i }
    }
    # Remove only one matching entry; preserve unrelated edits and duplicates.
    $remaining = for ($i = 0; $i -lt $entries.Count; $i++) { if ($i -ne $index) { $entries[$i] } }
    $State.EnginePathAdded = $false
    Save-EnginePathState $State $StateFile
    if ($index -lt 0) { return }
    try { Set-MachinePathSnapshot $snapshot ($remaining -join ';') }
    catch {
        $State.EnginePathAdded = $true
        Save-EnginePathState $State $StateFile
        throw
    }
    Write-Host (Get-SetupText 'PathRemoved')
}
function Test-OwnedEngineService($State, [string]$ServiceCommand, [string]$EngineDirectory) {
    if ($null -eq $State) { return $false }
    foreach ($name in @('Product', 'Directory', 'OwnsService')) {
        if (-not $State.PSObject.Properties[$name]) { return $false }
    }
    return $State.Product -ceq 'OpenSSHManager' -and $State.Directory -is [string] -and
        $State.Directory -eq (Split-Path -Parent $EngineDirectory) -and
        $State.OwnsService -is [bool] -and $State.OwnsService -and
        $ServiceCommand -eq ('"' + (Join-Path $EngineDirectory 'sshd.exe') + '"')
}
