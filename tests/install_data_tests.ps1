# Exercises the actual installer's file preparation on a temporary directory.
# Native OpenSSH calls and Windows ACL application are mocked; no service is installed.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../scripts/Localization.ps1')
Initialize-SetupLanguage 'en'
$installer = Join-Path $PSScriptRoot '../scripts/Install.ps1'
$tokens = $null; $parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($installer, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
foreach ($name in @('Assert-NoReparse', 'Initialize-SshData')) {
    $node = $ast.Find({ param($entry) $entry -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $entry.Name -eq $name }, $true)
    if (-not $node) { throw "Installer function missing: $name" }
    . ([scriptblock]::Create($node.Extent.Text))
}
function Assert-True($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
$script:aclCalls = @()
$script:keygenCalls = 0
function Set-ProtectedAcl([string]$Path, [switch]$Directory, [switch]$Private) {
    $script:aclCalls += @{ Path = $Path; Private = [bool]$Private; Directory = [bool]$Directory }
}
function Invoke-HostKeyGeneration([string]$EngineDirectory) {
    $script:keygenCalls++
    foreach ($type in @('rsa', 'ecdsa', 'ed25519')) {
        $path = Join-Path $script:caseDirectory "ssh_host_${type}_key"
        if (-not (Test-Path -LiteralPath $path)) {
            [IO.File]::WriteAllText($path, "generated private $type")
            [IO.File]::WriteAllText("$path.pub", "generated public $type")
        }
    }
}
function Invoke-ConfigValidation([string]$EngineDirectory, [string]$ConfigPath) {
    if ([IO.File]::ReadAllText($ConfigPath).Contains('INVALID_CONFIG')) { throw 'Fixture validation failure' }
}
function Expect-Failure([scriptblock]$Action, [string]$Message) {
    $failed = $false
    try { & $Action | Out-Null } catch { $failed = $true }
    Assert-True $failed $Message
}
$root = Join-Path ([IO.Path]::GetTempPath()) ('manager-install-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $engine = Join-Path $root 'engine'
    New-Item -ItemType Directory -Path $engine | Out-Null
    [IO.File]::WriteAllText((Join-Path $engine 'sshd_config_default'), "Port 22`nSubsystem sftp sftp-server.exe`n")
    foreach ($existingEmpty in @($false, $true)) {
        $script:caseDirectory = Join-Path $root ('fresh-' + $existingEmpty)
        if ($existingEmpty) { New-Item -ItemType Directory -Path $script:caseDirectory | Out-Null }
        $result = @(Initialize-SshData $engine $script:caseDirectory)
        $expected = Join-Path $script:caseDirectory 'sshd_config'
        Assert-True ($result.Count -eq 1 -and $result[0] -eq $expected) 'Installer returned unexpected pipeline data.'
        Assert-True ([IO.File]::ReadAllText($expected).Contains('Subsystem sftp internal-sftp')) 'Missing/default config was not initialized.'
    }
    Write-Output 'PASS: new directory and existing empty directory initialize successfully.'

    $script:caseDirectory = Join-Path $root 'config-only'
    New-Item -ItemType Directory -Path $script:caseDirectory | Out-Null
    $configPath = Join-Path $script:caseDirectory 'sshd_config'
    $custom = "# user configuration`r`nPort 2222`r`nSubsystem sftp internal-sftp`r`nMatch Group administrators`r`n    AuthorizedKeysFile __PROGRAMDATA__/ssh/administrators_authorized_keys`r`n"
    [IO.File]::WriteAllText($configPath, $custom)
    Initialize-SshData $engine $script:caseDirectory | Out-Null
    Assert-True ([IO.File]::ReadAllText($configPath) -ceq $custom) 'Existing config was overwritten.'
    Assert-True (Test-Path -LiteralPath (Join-Path $script:caseDirectory 'ssh_host_ed25519_key')) 'Missing host keys were not generated.'
    Write-Output 'PASS: existing configuration is preserved while missing host keys are generated.'

    $script:caseDirectory = Join-Path $root 'existing-keys'
    New-Item -ItemType Directory -Path $script:caseDirectory | Out-Null
    $originals = @{
        'sshd_config' = $custom
        'ssh_host_ed25519_key' = 'original private key'
        'ssh_host_ed25519_key.pub' = 'original public key'
        'administrators_authorized_keys' = 'existing authorized keys'
    }
    foreach ($entry in $originals.GetEnumerator()) { [IO.File]::WriteAllText((Join-Path $script:caseDirectory $entry.Key), $entry.Value) }
    Initialize-SshData $engine $script:caseDirectory | Out-Null
    $backup = @(Get-ChildItem -LiteralPath ($script:caseDirectory + '.manager-backups') -Directory)[0]
    foreach ($entry in $originals.GetEnumerator()) {
        Assert-True ([IO.File]::ReadAllText((Join-Path $script:caseDirectory $entry.Key)) -ceq $entry.Value) "Changed existing file: $($entry.Key)"
        Assert-True ([IO.File]::ReadAllText((Join-Path $backup.FullName $entry.Key)) -ceq $entry.Value) "Backup differs: $($entry.Key)"
    }
    $privateAcl = @($script:aclCalls | Where-Object { $_.Path -eq (Join-Path $script:caseDirectory 'administrators_authorized_keys') -and $_.Private })
    Assert-True ($privateAcl.Count -gt 0) 'Administrator authorized_keys was not given a private ACL.'
    Initialize-SshData $engine $script:caseDirectory | Out-Null
    Assert-True ([IO.File]::ReadAllText((Join-Path $script:caseDirectory 'ssh_host_ed25519_key')) -ceq 'original private key') 'Retry replaced host identity.'
    Write-Output 'PASS: host identity and authorized keys survive installation/retry; backups match exactly.'

    $script:caseDirectory = Join-Path $root 'orphan-public'
    New-Item -ItemType Directory -Path $script:caseDirectory | Out-Null
    $orphan = Join-Path $script:caseDirectory 'ssh_host_ed25519_key.pub'
    [IO.File]::WriteAllText($orphan, 'orphan identity')
    $before = $script:keygenCalls
    Expect-Failure { Initialize-SshData $engine $script:caseDirectory } 'Orphan public key should block automatic generation.'
    Assert-True ($script:keygenCalls -eq $before) 'Keygen was called for an orphan public key.'
    Assert-True ([IO.File]::ReadAllText($orphan) -ceq 'orphan identity') 'Orphan public key changed.'

    $script:caseDirectory = Join-Path $root 'invalid-config'
    New-Item -ItemType Directory -Path $script:caseDirectory | Out-Null
    $configPath = Join-Path $script:caseDirectory 'sshd_config'
    [IO.File]::WriteAllText($configPath, 'INVALID_CONFIG')
    Expect-Failure { Initialize-SshData $engine $script:caseDirectory } 'Invalid configuration should stop installation.'
    Assert-True ([IO.File]::ReadAllText($configPath) -ceq 'INVALID_CONFIG') 'Invalid user config was silently replaced.'
    Write-Output 'PASS: orphan keys and invalid configuration stop installation without replacing user data.'
} finally {
    # The test exclusively owns this freshly generated temporary directory.
    Remove-Item -LiteralPath $root -Recurse -Force
}
