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

# Ownership check: every existing file must either be part of this package or be runtime output.
# The native log is only ever written at the plugin root; a same-named file further down is foreign.
$allowedRuntime = @('native.log')
# ANR writes its own private Vulkan loader and a timestamped runtime log into its provider directory.
# Those two exact shapes are runtime output of the local ANR tree, so they are tolerated only when
# that tree was merged; anything else, including nested files or a root-level copy, is still refused.
$allowedAnrRuntime = @('providers/anr/anr-vkloader.dll')
$anrRuntimeLog = '^providers/anr/nr_fg-[0-9]{8}-[0-9]{6}\.log$'
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
        if ($IncludeAnrFg -and ($allowedAnrRuntime -contains $relative -or $relative -match $anrRuntimeLog)) { continue }
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
foreach ($file in $anrFiles) {
    $relative = $file.FullName.Substring($anrRoot.Length).TrimStart('\', '/')
    $target = Join-Path (Join-Path $destination 'providers/anr') $relative
    # A provider-local tuning file that already exists is the user's, not this package's.
    if ((Split-Path -Leaf $relative) -eq 'nr_fg.ini' -and (Test-Path -LiteralPath $target)) { continue }
    $parent = Split-Path -Parent $target
    if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    Copy-Item -LiteralPath $file.FullName -Destination $target -Force
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
