# Actual ownership/update helpers with an in-memory registry; no elevation or system PATH changes.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../scripts/EnginePath.ps1')
function Assert-True($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Expect-Failure([scriptblock]$Action, [string]$Message) {
    $failed = $false
    try { & $Action } catch { $failed = $true }
    Assert-True $failed $Message
}
$script:pathValue = ''; $script:pathKind = [Microsoft.Win32.RegistryValueKind]::ExpandString
$script:writes = 0; $script:denyWrite = $false; $script:concurrentEdit = $false
function Get-MachinePathSnapshot { return [pscustomobject]@{ Value = $script:pathValue; Kind = $script:pathKind } }
function Set-MachinePathSnapshot($Snapshot, [string]$Value) {
    if ($script:denyWrite) { throw 'Registry write denied (fixture).' }
    if ($script:concurrentEdit) { $script:pathValue += ';C:\ConcurrentTool'; throw 'Concurrent registry edit (fixture).' }
    Assert-True ($Snapshot.Value -ceq $script:pathValue) 'Stale PATH snapshot.'
    Assert-True ($Snapshot.Kind -eq $script:pathKind) 'Registry value type changed.'
    $script:pathValue = $Value; $script:writes++
}
$root = Join-Path ([IO.Path]::GetTempPath()) ('manager-engine-path-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $engine = Join-Path $root 'engine'
    New-Item -ItemType Directory -Path $engine | Out-Null
    [IO.File]::WriteAllText((Join-Path $engine 'sftp-server.exe'), 'fixture')
    $marker = Join-Path $root 'InstallState.json'
    $state = [pscustomobject]@{ Product = 'OpenSSHManager'; Directory = $root; OwnsService = $true; EnginePathAdded = $false }
    $original = '%SystemRoot%\System32;C:\Tools;;C:\Keep;'
    $script:pathValue = $original
    Add-EngineToMachinePath $engine $state $marker
    Assert-True ($script:pathValue -ceq ($original + ';' + $engine)) 'Existing PATH bytes or order changed.'
    Assert-True ((Get-Content $marker -Raw | ConvertFrom-Json).EnginePathAdded) 'Addition ownership not persisted.'
    $writes = $script:writes
    Add-EngineToMachinePath $engine $state $marker
    Assert-True ($script:writes -eq $writes -and $state.EnginePathAdded) 'Retry duplicated PATH or lost ownership.'
    $script:pathValue += ';C:\AddedAfterInstall'
    Remove-EngineFromMachinePath $engine $state $marker
    Assert-True ($script:pathValue -ceq ($original + ';C:\AddedAfterInstall')) 'Uninstall overwrote unrelated/new PATH entries.'
    Assert-True (-not (Get-Content $marker -Raw | ConvertFrom-Json).EnginePathAdded) 'Removal ownership not persisted.'
    $writes = $script:writes
    Remove-EngineFromMachinePath $engine $state $marker
    Assert-True ($script:writes -eq $writes) 'Repeated uninstall changed PATH.'
    Write-Output 'PASS: install/retry/uninstall preserve raw PATH, later edits and ownership.'

    $manual = 'C:\Tools; "' + $engine.ToUpperInvariant().Replace('/', '\') + '\" ;C:\Other'
    $script:pathValue = $manual
    Add-EngineToMachinePath $engine $state $marker
    Assert-True (-not $state.EnginePathAdded -and $script:pathValue -ceq $manual) 'Manual equivalent entry was duplicated or claimed.'
    Remove-EngineFromMachinePath $engine $state $marker
    Assert-True ($script:pathValue -ceq $manual) 'Manual entry was removed.'
    $legacy = [pscustomobject]@{ OwnsService = $true }
    Remove-EngineFromMachinePath $engine $legacy $marker
    Assert-True ($script:pathValue -ceq $manual) 'Legacy marker removed an unowned entry.'
    Write-Output 'PASS: casing, slash, quotes, trailing separator and legacy/manual entries.'

    foreach ($empty in @($null, '')) {
        $script:pathValue = $empty
        Add-EngineToMachinePath $engine $state $marker
        Assert-True ($script:pathValue -eq $engine) 'Empty PATH created a leading empty entry.'
        Remove-EngineFromMachinePath $engine $state $marker
        Assert-True ($script:pathValue -eq '') 'Single-entry PATH was not removed.'
    }
    $script:pathKind = [Microsoft.Win32.RegistryValueKind]::String
    $script:pathValue = 'C:\Tools'
    Add-EngineToMachinePath $engine $state $marker
    # A user-added duplicate must survive removing our one entry.
    $script:pathValue = $engine + ';' + $script:pathValue
    Remove-EngineFromMachinePath $engine $state $marker
    Assert-True ($script:pathValue -ceq ($engine + ';C:\Tools')) 'Uninstall removed multiple equivalent entries.'
    Write-Output 'PASS: missing/empty PATH, REG_SZ, and duplicate preservation.'

    $script:pathValue = $original; $script:denyWrite = $true
    Expect-Failure { Add-EngineToMachinePath $engine $state $marker } 'Registry denial was ignored.'
    Assert-True (-not $state.EnginePathAdded -and $script:pathValue -ceq $original) 'Failed addition changed PATH or ownership.'
    Assert-True (-not (Get-Content $marker -Raw | ConvertFrom-Json).EnginePathAdded) 'Failed addition claimed persisted ownership.'
    $script:denyWrite = $false
    Add-EngineToMachinePath $engine $state $marker
    $script:denyWrite = $true
    Expect-Failure { Remove-EngineFromMachinePath $engine $state $marker } 'Removal denial was ignored.'
    Assert-True ($state.EnginePathAdded -and (Get-Content $marker -Raw | ConvertFrom-Json).EnginePathAdded) 'Failed removal lost ownership.'
    $script:denyWrite = $false
    Remove-EngineFromMachinePath $engine $state $marker
    $script:concurrentEdit = $true
    Expect-Failure { Add-EngineToMachinePath $engine $state $marker } 'Concurrent PATH edit was ignored.'
    Assert-True ($script:pathValue -ceq ($original + ';C:\ConcurrentTool') -and -not $state.EnginePathAdded) 'Concurrent edit was lost or claimed.'
    $script:concurrentEdit = $false
    $before = $script:pathValue
    Expect-Failure { Add-EngineToMachinePath $engine $state (Join-Path $root 'missing/state.json') } 'Marker save failure was ignored.'
    Assert-True ($script:pathValue -ceq $before) 'PATH changed before ownership could be saved.'
    $state.EnginePathAdded = $false
    Expect-Failure { Add-EngineToMachinePath (Join-Path $root 'missing-engine') $state $marker } 'Missing SFTP engine was accepted.'
    Write-Output 'PASS: registry/save failures, concurrent edits and missing engine are safe to retry.'

    $command = '"' + (Join-Path $engine 'sshd.exe') + '"'
    Assert-True (Test-OwnedEngineService $state $command $engine) 'Owned engine was not recognized for repair.'
    Assert-True (-not (Test-OwnedEngineService $state '"C:\Windows\System32\OpenSSH\sshd.exe"' $engine)) 'External engine was recognized for repair.'
    Assert-True (-not (Test-OwnedEngineService $state ($command + ' -f other.conf') $engine)) 'Custom service command was recognized for repair.'
    $state.Product = 'OtherProduct'
    Assert-True (-not (Test-OwnedEngineService $state $command $engine)) 'Foreign marker accepted.'
    $state.Product = 'OpenSSHManager'; $state.Directory = $root + '-other'
    Assert-True (-not (Test-OwnedEngineService $state $command $engine)) 'Mismatched installation directory accepted.'
    $state.Directory = $root; $state.OwnsService = 'true'
    Assert-True (-not (Test-OwnedEngineService $state $command $engine)) 'String ownership accepted as boolean.'
    $state.OwnsService = $false
    Assert-True (-not (Test-OwnedEngineService $state $command $engine)) 'Unowned engine was recognized for repair.'
    Assert-True (-not (Test-OwnedEngineService $null $command $engine)) 'Missing marker was recognized for repair.'
    Write-Output 'PASS: repair is restricted to the recorded service and exact engine registration.'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}
