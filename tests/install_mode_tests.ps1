# Test the installer's actual mode selection, SCM error handling, and elevation quoting.
# Does not request elevation, modify services, or write to Program Files.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../scripts/Localization.ps1')
Initialize-SetupLanguage 'en'
$tokens = $null; $parseErrors = $null
$installer = Join-Path $PSScriptRoot '../scripts/Install.ps1'
$ast = [System.Management.Automation.Language.Parser]::ParseFile($installer, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
foreach ($name in @('Get-InstallMode', 'Get-ExistingSshService', 'Get-ElevationArguments')) {
    $node = $ast.Find({ param($entry) $entry -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $entry.Name -eq $name }, $true)
    if (-not $node) { throw "Installer function missing: $name" }
    . ([scriptblock]::Create($node.Extent.Text))
}
function Assert-True($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
Assert-True ((Get-InstallMode $false $false) -eq 'Full') 'Absent service must select full installation.'
Assert-True ((Get-InstallMode $true $false) -eq 'GuiOnly') 'Existing service must select GUI-only automatically.'
Assert-True ((Get-InstallMode $false $true) -eq 'GuiOnly') 'Explicit GUI-only must not install a service.'
Assert-True ((Get-InstallMode $true $true) -eq 'GuiOnly') 'Explicit GUI-only with service must remain GUI-only.'
Write-Output 'PASS: first install, existing service, and explicit GUI-only selection.'

$script:serviceScenario = 'present'
function Get-Service {
    [CmdletBinding()] param([string]$Name)
    Assert-True ($Name -eq 'sshd') 'Unexpected service lookup.'
    switch ($script:serviceScenario) {
        'present' { return [pscustomobject]@{ Name = 'sshd'; Status = 'Stopped' } }
        'absent' { Write-Error -Message 'No such service' -ErrorId 'NoServiceFoundForGivenName' -Category ObjectNotFound }
        'denied' { Write-Error -Message 'SCM access denied' -ErrorId 'CouldNotGetService' -Category PermissionDenied }
    }
}
Assert-True ((Get-ExistingSshService).Status -eq 'Stopped') 'Stopped service must still count as installed.'
$script:serviceScenario = 'absent'
Assert-True ($null -eq (Get-ExistingSshService)) 'Missing service was not detected.'
$script:serviceScenario = 'denied'
$failed = $false
try { Get-ExistingSshService | Out-Null } catch { $failed = $true }
Assert-True $failed 'SCM errors must not be interpreted as a missing service.'
Write-Output 'PASS: stopped/missing services distinguished from detection errors.'

$root = Join-Path ([IO.Path]::GetTempPath()) ('manager-bootstrap-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
try {
    # All characters are legal in Windows filenames, including apostrophe and command syntax.
    $folder = Join-Path $root 'user''s files & $([throw]) [x]; test'
    [IO.Directory]::CreateDirectory($folder) | Out-Null
    $scriptPath = Join-Path $folder 'Install.ps1'
    [IO.File]::WriteAllText($scriptPath, @'
param([switch]$GuiOnly, [switch]$Interactive, [switch]$Elevated, [string]$UiLanguage)
$global:ManagerBootstrapProbe = @([bool]$GuiOnly, [bool]$Interactive, [bool]$Elevated, $UiLanguage)
'@)
    foreach ($language in @('en', 'zh-Hans', 'zh-Hant')) {
        foreach ($gui in @($false, $true)) {
            foreach ($interactive in @($false, $true)) {
                $arguments = Get-ElevationArguments $scriptPath $gui $interactive $language
                Assert-True ($arguments -match '^-NoProfile -ExecutionPolicy Bypass -EncodedCommand ([A-Za-z0-9+/=]+)$') 'Unexpected elevation argument shape.'
                $command = [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($Matches[1]))
                & ([scriptblock]::Create($command))
                Assert-True ($global:ManagerBootstrapProbe[0] -eq $gui -and $global:ManagerBootstrapProbe[1] -eq $interactive -and $global:ManagerBootstrapProbe[2]) 'Elevation changed path or switch values.'
                Assert-True ($global:ManagerBootstrapProbe[3] -eq $language) 'Elevation changed the UI language.'
            }
        }
    }
    Write-Output 'PASS: elevation quoting preserves special-character paths and all switch combinations.'
} finally {
    Remove-Variable ManagerBootstrapProbe -Scope Global -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $root -Recurse -Force
}
