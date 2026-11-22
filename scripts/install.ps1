#requires -Version 5.1
<#
.SYNOPSIS
  Installs the staged BigScale package into an existing Big Walk installation.

.DESCRIPTION
  Copies only this mod's own plugin directory. The game executable, boot options, proxy DLLs,
  existing configurations and the Big Walk VR files are never read or written. Installation is
  refused while the game is running and refused when the destination holds files this package does
  not own, so an unrecognised file is never deleted. A pre-rename BigWalkDLSS installation is never
  migrated or removed: this installer fails and names what the user has to delete.

  Files under providers/anr are the user's own manual ANR installation and are always preserved.
  The only files this installer deletes are the exact notice files an older 1.0.0 BigScale payload
  shipped individually, which THIRD-PARTY-NOTICES.txt now replaces, and the old ANR-INSTALL.txt,
  which _ADDITIONAL_PROVIDERS.txt now replaces.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GameDir,
    [string]$PackageDir = '',
    [switch]$IncludeAnrFg
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

function Write-Step($message) { Write-Host "==> $message" -ForegroundColor Cyan }

$GameDir = (Resolve-Path -LiteralPath $GameDir).Path
if (-not (Test-Path -LiteralPath (Join-Path $GameDir 'BepInEx/plugins'))) {
    throw "GameDir '$GameDir' has no BepInEx/plugins directory. Launch the game once with BepInEx installed."
}

$running = Get-Process -Name 'Big Walk' -ErrorAction SilentlyContinue
if ($running) {
    throw "Big Walk is running (PID $($running.Id -join ', ')). Close the game before installing."
}

if ([string]::IsNullOrWhiteSpace($PackageDir)) { $PackageDir = Join-Path $repo 'dist/BepInEx/plugins/BigScale' }
if (-not (Test-Path -LiteralPath $PackageDir)) {
    throw "Staged package not found at '$PackageDir'. Run scripts/build.ps1 first."
}

$source = (Resolve-Path -LiteralPath $PackageDir).Path
$destination = Join-Path $GameDir 'BepInEx/plugins/BigScale'

# Clean break: the pre-rename plugin is a different GUID and a different directory. Nothing is
# migrated and nothing the user installed is deleted.
$legacy = Join-Path $GameDir 'BepInEx/plugins/BigWalkDLSS'
if (Test-Path -LiteralPath $legacy) {
    throw ("An older BigWalkDLSS installation exists at '$legacy'. Remove that directory (and " +
           "optionally BepInEx/config/local.bigwalk.dlss.cfg) before installing BigScale. This " +
           "installer never deletes files it did not create.")
}

$payload = @(Get-ChildItem -LiteralPath $source -Recurse -File)
if (-not $payload) { throw "Staged package '$source' is empty." }

# The local ANR runtime is never redistributed: it is merged from the verified local tree only when
# the caller asks for it, and its files are owned like every other provider payload.
$anrRoot = Join-Path $repo 'third_party/anr-fg'
$anrFiles = @()
if ($IncludeAnrFg) {
    if (-not (Test-Path -LiteralPath (Join-Path $anrRoot 'nvngx_dlssg.dll'))) {
        throw "Local ANR runtime not found at '$anrRoot'. Run scripts/fetch-deps.ps1 -IncludeAnrFg first."
    }
    $anrFiles = @(Get-ChildItem -LiteralPath $anrRoot -Recurse -File)
    Write-Step "Merging the local ANR runtime ($($anrFiles.Count) files, never redistributed)"
}

# Ownership check: every existing file must be part of this package, runtime output, a notice file
# this release replaces, or the user's own ANR tree. The native log is only ever written at the
# plugin root; a same-named file further down is foreign.
$allowedRuntime = @('native.log')
# providers/anr holds a manual installation the user made from Apex Compute's releases, so it is
# theirs whether or not -IncludeAnrFg merges a local tree in this run.
$anrPrefix = 'providers/anr/'
# The 1.0.0 payload shipped its notices as individual files. These exact names are removed once the
# combined notice is copied; they are never matched by wildcard or extension.
$legacyNotices = @(
    'APACHE-2.0.txt', 'FIDELITYFX-LICENSE.txt', 'FIDELITYFX-SDK-LICENSE.md',
    'FIDELITYFX-SDK-THIRD-PARTY-NOTICES.md', 'MFG-UNLOCK-DASHDDOGY-LICENSE.txt',
    'MFG-UNLOCK-FFX-API-LICENSE.txt', 'MFG-UNLOCK-LICENSE.txt', 'MFG-UNLOCK-MINHOOK-LICENSE.txt',
    'MFG-UNLOCK-NEFH-LICENSE.txt', 'MINHOOK-LICENSE.txt', 'NOTICE.md',
    'NVIDIA-RTX-SDK-LICENSE.txt', 'OPENVR-LICENSE.txt', 'STREAMLINE-LICENSE.txt',
    'STREAMLINE-THIRD-PARTY-LICENSES.md', 'UNITY-COMPANION-LICENSE.txt', 'XESS-LICENSE.txt',
    'XESS-THIRD-PARTY-PROGRAMS.txt'
)
# The previous payload shipped its optional-provider instructions as ANR-INSTALL.txt. The renamed
# _ADDITIONAL_PROVIDERS.txt supersedes it, so the old name is removed once by exact name only.
$replacedPayloadText = @('ANR-INSTALL.txt')
$legacyPresent = @()
$replacedPayloadPresent = @()
$anrPreserved = @()
if (Test-Path -LiteralPath $destination) {
    $owned = @{}
    foreach ($file in $payload) {
        $relative = $file.FullName.Substring($source.Length).TrimStart('\', '/').Replace('\', '/')
        $owned[$relative] = $true
    }
    foreach ($file in $anrFiles) {
        $relative = (Join-Path 'providers/anr' $file.FullName.Substring($anrRoot.Length).TrimStart('\', '/')).Replace('\', '/')
        $owned[$relative] = $file.FullName
    }
    $foreign = @()
    foreach ($existing in Get-ChildItem -LiteralPath $destination -Recurse -File) {
        $relative = $existing.FullName.Substring($destination.Length).TrimStart('\', '/').Replace('\', '/')
        if ($owned.ContainsKey($relative)) { continue }
        if ($allowedRuntime -contains $relative) { continue }
        if ($legacyNotices -contains $relative) { $legacyPresent += $relative; continue }
        if ($replacedPayloadText -contains $relative) { $replacedPayloadPresent += $relative; continue }
        if ($relative.StartsWith($anrPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            $anrPreserved += $relative
            continue
        }
        $foreign += $relative
    }
    if ($foreign.Count -gt 0) {
        throw ("Refusing to install: '$destination' contains files this package does not own: " +
               ($foreign -join ', ') +
               ". Move them away first; this installer never deletes unmanaged files.")
    }
    Write-Step "Upgrading an existing BigScale installation at $destination"
} else {
    Write-Step "Creating $destination"
}
New-Item -ItemType Directory -Path $destination -Force | Out-Null

foreach ($file in $payload) {
    $relative = $file.FullName.Substring($source.Length).TrimStart('\', '/')
    $target = Join-Path $destination $relative
    $parent = Split-Path -Parent $target
    if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    Copy-Item -LiteralPath $file.FullName -Destination $target -Force
}
# The only deletions this installer performs. Exact names only, and only the ones that were present.
foreach ($legacyName in $legacyPresent) {
    Remove-Item -LiteralPath (Join-Path $destination $legacyName) -Force
}
if ($legacyPresent.Count -gt 0) {
    Write-Step "Removed $($legacyPresent.Count) legacy notice files replaced by THIRD-PARTY-NOTICES.txt"
}
foreach ($replacedName in $replacedPayloadPresent) {
    Remove-Item -LiteralPath (Join-Path $destination $replacedName) -Force
}
if ($replacedPayloadPresent.Count -gt 0) {
    Write-Step "Removed $($replacedPayloadPresent.Count) superseded payload text file replaced by _ADDITIONAL_PROVIDERS.txt"
}
foreach ($file in $anrFiles) {
    $relative = $file.FullName.Substring($anrRoot.Length).TrimStart('\', '/')
    $target = Join-Path (Join-Path $destination 'providers/anr') $relative
    # A provider-local tuning file that already exists is the user's, not this package's.
    if ((Split-Path -Leaf $relative) -eq 'nr_fg.ini' -and (Test-Path -LiteralPath $target)) { continue }
    $parent = Split-Path -Parent $target
    if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    Copy-Item -LiteralPath $file.FullName -Destination $target -Force
}
if (-not $IncludeAnrFg -and $anrPreserved.Count -gt 0) {
    Write-Step "Preserved $($anrPreserved.Count) existing ANR files under providers/anr"
}

$config = Join-Path $GameDir 'BepInEx/config/raharu.bigscale.cfg'
if (Test-Path -LiteralPath $config) {
    Write-Step "Existing configuration preserved: $config"
} else {
    Write-Step 'BepInEx/config/raharu.bigscale.cfg is created by the game on first launch.'
}

Write-Step 'Installed files'
Get-ChildItem -LiteralPath $destination -Recurse -File | Sort-Object Name |
    ForEach-Object { '{0,12:N0}  {1}' -f $_.Length, $_.Name }
Write-Host "Installed into $destination" -ForegroundColor Green
