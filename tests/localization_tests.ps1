# Pure language/catalog checks. Does not install, elevate or modify system state.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../scripts/Localization.ps1')
function Assert-True($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
foreach ($tag in @('zh-CN', 'zh-SG', 'zh-Hans', 'ZH_hAnS_TW', 'zh', 'zh-CHS', 'zh-CN-x-hant')) {
    Assert-True ((Select-SetupLanguage @($tag)) -eq 'zh-Hans') "Incorrect simplified selection: $tag"
}
foreach ($tag in @('zh-TW', 'zh-HK', 'zh-MO', 'zh-Hant', 'zh-Hant-CN', 'zh-CHT')) {
    Assert-True ((Select-SetupLanguage @($tag)) -eq 'zh-Hant') "Incorrect traditional selection: $tag"
}
Assert-True ((Select-SetupLanguage @('fr-FR', 'zh-HK', 'en-US')) -eq 'zh-Hant') 'Preference order changed.'
Assert-True ((Select-SetupLanguage @('en-GB', 'zh-CN')) -eq 'en') 'First supported preference ignored.'
Assert-True ((Select-SetupLanguage @('english', 'zhfoo', 'ja-JP')) -eq 'en') 'Unsupported languages did not fall back to English.'
Assert-True ((Select-SetupLanguage @()) -eq 'en') 'Empty preference list did not fall back to English.'
foreach ($language in @('en', 'zh-Hans', 'zh-Hant')) {
    Initialize-SetupLanguage $language
    foreach ($key in $script:SetupMessages.Keys) {
        $entry = $script:SetupMessages[$key]
        Assert-True ($entry.Count -eq 3) "Incomplete translation: $key"
        $expectedSlots = @([regex]::Matches($entry[2], '\{\d+\}') | ForEach-Object { $_.Value }) -join ','
        foreach ($translation in $entry) {
            Assert-True (-not [string]::IsNullOrWhiteSpace($translation)) "Empty translation: $key"
            $slots = @([regex]::Matches($translation, '\{\d+\}') | ForEach-Object { $_.Value }) -join ','
            Assert-True ($slots -eq $expectedSlots) "Formatting arguments differ: $key"
        }
        $actual = Get-SetupText $key @('path with ''quotes'' & $([throw]) {0}', 'code')
        Assert-True (-not [string]::IsNullOrEmpty($actual)) "Formatting failed: $key"
    }
    $formatted = Get-SetupText 'NativeFailed' @('literal {1} $([throw])', 17)
    Assert-True ($formatted.Contains('literal {1} $([throw])') -and $formatted.Contains('17')) 'Arguments were evaluated or reformatted.'
    . (Join-Path $PSScriptRoot '../scripts/EnginePath.ps1')
    . (Join-Path $PSScriptRoot '../scripts/EngineArchive.ps1')
    Assert-True ($script:SetupLanguage -eq $language) 'Loading helpers changed the caller language.'
}
Initialize-SetupLanguage 'zh-Hant'
Assert-True ((Get-SetupText 'CloseWindow') -eq '按 Enter 關閉安裝視窗') 'Traditional wording changed.'
foreach ($file in Get-ChildItem (Join-Path $PSScriptRoot '../scripts'), $PSScriptRoot -Filter '*.ps1') {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$tokens, [ref]$errors)
    Assert-True (-not $errors.Count) "PowerShell syntax error in $($file.Name): $errors"
    # Compile the UI-language P/Invoke declaration even on non-Windows test hosts.
    if ($file.Name -eq 'Localization.ps1' -and -not ('OpenSSHManager.SetupLanguages' -as [type])) {
        $source = $ast.Find({ param($node)
            $node -is [Management.Automation.Language.StringConstantExpressionAst] -and
                $node.Value.Contains('public static class SetupLanguages')
        }, $true)
        Assert-True ($null -ne $source) 'Missing native language detection helper.'
        Add-Type -TypeDefinition $source.Value
    }
}
Write-Output 'PASS: setup language selection, translation completeness, formatting, helper inheritance and script syntax.'
