. (Join-Path $PSScriptRoot 'Localization.ps1')
# Loaded only by the embedded, elevated engine installer. No entry point on load.
function Expand-VerifiedEngineArchive([string]$ArchivePath, [string]$Target, [string]$ExpectedHash) {
    if ($ExpectedHash -notmatch '^[0-9a-fA-F]{64}$') { throw (Get-SetupText 'EngineDigest') }
    if ((Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash -ne $ExpectedHash) { throw (Get-SetupText 'ZipChecksum') }
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $names = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        $entries = @(); [long]$total = 0
        if ($zip.Entries.Count -gt 4096) { throw (Get-SetupText 'ArchiveEntries') }
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName
            if (-not $name.StartsWith('OpenSSH-Win64/', [StringComparison]::Ordinal) -or $name.Contains('\') -or $name.Contains(':')) {
                throw (Get-SetupText 'ArchiveEntry' @($name))
            }
            $relative = $name.Substring(14)
            if (($relative -split '/') -contains '..' -or ($relative -split '/') -contains '.' -or $relative.StartsWith('/')) { throw (Get-SetupText 'ArchivePath') }
            if (($entry.ExternalAttributes -shr 16 -band 0xF000) -eq 0xA000) { throw (Get-SetupText 'ArchiveLinks') }
            if (-not $relative -or $name.EndsWith('/')) { continue }
            if (-not $names.Add($relative)) { throw (Get-SetupText 'ArchiveDuplicate') }
            foreach ($part in ($relative -split '/')) {
                if (-not $part -or $part -match '[<>"|?*\x00-\x1f]' -or $part.EndsWith('.') -or $part.EndsWith(' ') -or
                    $part -match '^(?i:con|prn|aux|nul|com[0-9]|lpt[0-9])(?:\.|$)') { throw (Get-SetupText 'ArchiveWindowsPath') }
            }
            $total += $entry.Length
            if ($entry.Length -gt 64MB -or $total -gt 256MB) { throw (Get-SetupText 'ArchiveSize') }
            $entries += [pscustomobject]@{ Entry = $entry; Relative = $relative }
        }
        foreach ($required in @('sshd.exe','sshd-auth.exe','sshd-session.exe','sftp-server.exe','ssh-keygen.exe','ssh-agent.exe','libcrypto.dll','sshd_config_default','openssh-events.man')) {
            if (-not $names.Contains($required)) { throw (Get-SetupText 'ZipIncomplete' @($required)) }
        }
        # Validate all paths before extracting. The target is a new protected staging directory.
        if (Test-Path -LiteralPath $Target) { throw (Get-SetupText 'ExtractionTarget') }
        New-Item -ItemType Directory -Path $Target | Out-Null
        foreach ($item in $entries) {
            $path = Join-Path $Target $item.Relative
            New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($item.Entry, $path, $false)
        }
    } finally { $zip.Dispose() }
}
function Assert-OwnedEngineRepair([string]$Destination) {
    $engine = Join-Path $Destination 'engine'
    $marker = Join-Path $Destination 'InstallState.json'
    Assert-NoReparse $engine
    Assert-NoReparse $marker
    $state = Get-Content -LiteralPath $marker -Raw -ErrorAction Stop | ConvertFrom-Json
    $registration = Get-CimInstance Win32_Service -Filter "Name='sshd'" -ErrorAction Stop
    if (-not $registration -or -not (Test-OwnedEngineService $state $registration.PathName $engine)) {
        throw (Get-SetupText 'RepairOwnership')
    }
    return $registration
}
function Invoke-EngineInstallation([string]$ArchivePath, [switch]$Repair) {
    # Installation never touches an existing service. Repair requires a separate
    # explicit command and fresh ownership checks before stopping/copying/starting.
    $existing = Get-ExistingSshService
    if ($existing -and -not $Repair) { Write-Output (Get-SetupText 'ExistingServiceUnchanged'); return }
    if ($Repair -and (-not $existing -or -not $ArchivePath)) { throw (Get-SetupText 'RepairRequirements') }
    $destination = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'OpenSSHManager'
    $dataDirectory = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'ssh'
    $stateFile = Join-Path $destination 'InstallState.json'
    Assert-NoReparse $destination
    Assert-NoReparse $dataDirectory
    Assert-NoReparse $stateFile
    $state = [pscustomobject]@{ Product = 'OpenSSHManager'; Version = '0.1.22'; Directory = $destination; OwnsService = $false; RegisteredEvents = $false; EnginePathAdded = $false }
    if (Test-Path -LiteralPath $destination) {
        if (-not (Test-Path -LiteralPath $stateFile -PathType Leaf)) { throw (Get-SetupText 'ExistingUnowned') }
        $state = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
        if ($state.Product -ne 'OpenSSHManager' -or $state.Directory -ne $destination) { throw (Get-SetupText 'MarkerInvalid') }
        if (-not $state.PSObject.Properties['EnginePathAdded']) { $state | Add-Member -NotePropertyName EnginePathAdded -NotePropertyValue $false }
    }
    $engine = Join-Path $destination 'engine'
    if ($Repair) { $null = Assert-OwnedEngineRepair $destination }
    $source = $null
    if ($ArchivePath) {
        $lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'engine.lock.json') -Raw | ConvertFrom-Json
        $source = Join-Path $PSScriptRoot 'extracted'
        Expand-VerifiedEngineArchive $ArchivePath $source $lock.sha256
        foreach ($file in @(Get-ChildItem -LiteralPath $source -Recurse -File | Where-Object { $_.Extension -in @('.exe', '.dll') })) {
            $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
            if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') { throw (Get-SetupText 'SignatureFailed' @($($file.Name))) }
        }
    } elseif (-not (Test-Path -LiteralPath $engine -PathType Container)) { throw (Get-SetupText 'NoReusableEngine') }
    if ($Repair) {
        $null = Assert-OwnedEngineRepair $destination
        # Do not kill unrelated SSH services or established worker processes.
        Stop-Service -Name sshd -ErrorAction Stop
        (Get-Service sshd).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
        $null = Assert-OwnedEngineRepair $destination
    }
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Set-ProtectedAcl $destination -Directory
    $state | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding UTF8
    Set-ProtectedAcl $stateFile
    if ($source) {
        Assert-NoReparse $engine
        New-Item -ItemType Directory -Path $engine -Force | Out-Null
        Set-ProtectedAcl $engine -Directory
        foreach ($file in @(Get-ChildItem -LiteralPath $source -Recurse -File)) {
            $relative = $file.FullName.Substring($source.Length + 1)
            $target = Join-Path $engine $relative
            Assert-NoReparse $target
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            Copy-Item -LiteralPath $file.FullName -Destination $target -Force
            Set-ProtectedAcl $target
            if ((Get-FileHash -LiteralPath $target).Hash -ne (Get-FileHash -LiteralPath $file.FullName).Hash) { throw (Get-SetupText 'InstalledChecksum') }
        }
    }
    # Leave the running GUI untouched. Keep trusted removal/PATH helpers for this engine installation.
    foreach ($name in @('EnginePath.ps1', 'Uninstall.ps1', 'Localization.ps1')) {
        $target = Join-Path $destination $name
        Assert-NoReparse $target
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $target -Force
        Set-ProtectedAcl $target
    }
    if ($Repair) {
        # Never regenerate/replace user configuration or host/account keys during
        # engine repair; a bad or missing configuration remains an explicit error.
        $registration = Assert-OwnedEngineRepair $destination
        Invoke-ConfigValidation $engine (Join-Path $dataDirectory 'sshd_config')
        Register-EnginePath $engine $state $stateFile
        if ($registration.StartMode -ne 'Disabled') {
            $null = Assert-OwnedEngineRepair $destination
            Start-Service -Name sshd -ErrorAction Stop
            (Get-Service sshd).WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
        }
    } else { Install-EngineService $destination $dataDirectory $state $stateFile }
    Save-EnginePathState $state $stateFile
    if ($Repair) { Write-Output (Get-SetupText 'EngineRepaired') }
    else { Write-Output (Get-SetupText 'EngineStarted') }
}
