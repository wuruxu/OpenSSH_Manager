# Exercise actual archive validation without installing a service or elevating.
param([string]$OfficialArchive)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../scripts/EngineArchive.ps1')
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Assert-True($ok, [string]$message) { if (-not $ok) { throw $message } }
$root = Join-Path ([IO.Path]::GetTempPath()) ('manager-archive-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$required = @('sshd.exe','sshd-auth.exe','sshd-session.exe','sftp-server.exe','ssh-keygen.exe','ssh-agent.exe','libcrypto.dll','sshd_config_default','openssh-events.man')
function Make-Zip([string[]]$Names, [switch]$Link) {
    $path = Join-Path $root ([Guid]::NewGuid().ToString('N') + '.zip')
    $zip = [IO.Compression.ZipFile]::Open($path, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in $Names) {
            $entry = $zip.CreateEntry($name)
            if ($Link) { $entry.ExternalAttributes = -1577058304 } # Unix S_IFLNK | 0777
            $stream = $entry.Open()
            try { $bytes = [Text.Encoding]::UTF8.GetBytes('fixture:' + $name); $stream.Write($bytes, 0, $bytes.Length) }
            finally { $stream.Dispose() }
        }
    } finally { $zip.Dispose() }
    return $path
}
function Reject-Zip([string]$Path, [string]$Hash = (Get-FileHash -LiteralPath $Path).Hash) {
    $target = Join-Path $root ([Guid]::NewGuid().ToString('N'))
    $failed = $false
    try { Expand-VerifiedEngineArchive $Path $target $Hash } catch { $failed = $true }
    Assert-True $failed 'Invalid archive was accepted.'
    Assert-True (-not (Test-Path -LiteralPath $target)) 'Invalid archive wrote extracted files before validation.'
}
try {
    $names = @($required | ForEach-Object { 'OpenSSH-Win64/' + $_ })
    $valid = Make-Zip ($names + 'OpenSSH-Win64/docs/notice.txt')
    $target = Join-Path $root 'valid'
    Expand-VerifiedEngineArchive $valid $target (Get-FileHash -LiteralPath $valid).Hash
    Assert-True ((Get-ChildItem -LiteralPath $target -Recurse -File).Count -eq 10) 'Valid archive lost files.'
    foreach ($name in $required) {
        Assert-True ((Get-Content -LiteralPath (Join-Path $target $name) -Raw) -eq ('fixture:OpenSSH-Win64/' + $name)) 'Extracted content changed.'
    }
    Reject-Zip $valid ('0' * 64)
    Reject-Zip (Make-Zip @('OpenSSH-Win64/sshd.exe'))
    Reject-Zip (Make-Zip ($names + 'OpenSSH-Win64/SSHD.EXE'))
    foreach ($bad in @('Other/evil.exe', 'OpenSSH-Win64/../escape', 'OpenSSH-Win64//absolute',
        'OpenSSH-Win64/a/../../escape', 'OpenSSH-Win64/./file', 'OpenSSH-Win64/C:/file',
        'OpenSSH-Win64/a\file', 'OpenSSH-Win64/file:stream', 'OpenSSH-Win64/trailing.',
        'OpenSSH-Win64/trailing ', 'OpenSSH-Win64/CON.txt', 'OpenSSH-Win64/aux/file', 'OpenSSH-Win64/a//b')) {
        Reject-Zip (Make-Zip ($names + $bad))
    }
    Reject-Zip (Make-Zip $names -Link)
    $failed = $false
    try { Expand-VerifiedEngineArchive $valid $target (Get-FileHash -LiteralPath $valid).Hash } catch { $failed = $true }
    Assert-True $failed 'Existing extraction directory was overwritten.'
    Write-Output 'PASS: extraction, SHA-256, incomplete archives, duplicate paths, traversal, Windows aliases, links and existing destinations.'

    # The post-UAC recheck must return before any archive or filesystem access.
    function Get-ExistingSshService { return [pscustomobject]@{ Name = 'sshd'; Status = 'Stopped' } }
    Invoke-EngineInstallation 'nonexistent.zip' | Out-Null
    function Get-ExistingSshService { throw 'SCM access denied' }
    $failed = $false
    try { Invoke-EngineInstallation 'nonexistent.zip' } catch { $failed = $_.Exception.Message -eq 'SCM access denied' }
    Assert-True $failed 'Service query errors did not stop engine installation.'
    Write-Output 'PASS: existing services and detection failures cannot trigger installation.'
    if ($OfficialArchive) {
        $lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../engine.lock.json') -Raw | ConvertFrom-Json
        Expand-VerifiedEngineArchive $OfficialArchive (Join-Path $root 'official') $lock.sha256
        Write-Output 'PASS: pinned official ZIP hash and extraction.'
    }
} finally { Remove-Item -LiteralPath $root -Recurse -Force }
