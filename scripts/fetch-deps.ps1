#requires -Version 5.1
<#
.SYNOPSIS
  Acquires the pinned third-party sources and runtimes BigScale builds against.

.DESCRIPTION
  Fetches each dependency at the exact commit or archive this project is bound to, verifies the
  pinned runtime hashes, and leaves everything under third_party/. Nothing is published anywhere and
  nothing here is redistributed: scripts/build.ps1 stages only the files the release payload needs.
#>
[CmdletBinding()]
param(
    [string]$SdkCommit = '374959484e79a640feaba44c93ac8cfb0a03f5b5',
    [string]$SdkRepository = 'https://github.com/NVIDIA/DLSS.git',
    [string]$Fsr2Commit = 'f2e3f86390746eb3f0bd1b28e91ea3cbc790ee76',
    [string]$Fsr2Repository = 'https://github.com/optiscaler/FidelityFX-FSR2-DX11.git',
    [string]$FsrSdkCommit = '60f4ea81909200d8542eca14dccb2628b763a9a3',
    [string]$FsrSdkRepository = 'https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git',
    [string]$XessCommit = '8fe81bdbbaf00b3c1b733fd0d830c333dc84e6f0',
    [string]$XessRepository = 'https://github.com/intel/xess.git',
    [string]$MinHookCommit = 'c3fcafdc10146beb5919319d0683e44e3c30d537',
    [string]$MinHookRepository = 'https://github.com/TsudaKageyu/minhook.git',
    [string]$OpenVrCommit = '91825305130f446f82054c1ec3d416321ace0072',
    [string]$OpenVrRepository = 'https://github.com/ValveSoftware/openvr.git',
    [switch]$IncludeAnrFg
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$thirdParty = Join-Path $repo 'third_party'
New-Item -ItemType Directory -Path $thirdParty -Force | Out-Null

function Write-Step($message) { Write-Host "==> $message" -ForegroundColor Cyan }

function Sync-Repository($path, $url, $commit) {
    if (Test-Path -LiteralPath $path) {
        if (-not (Test-Path -LiteralPath (Join-Path $path '.git'))) {
            throw "'$path' exists but is not a git checkout. Delete this directory and rerun scripts/fetch-deps.ps1."
        }
    }
    else {
        Write-Step "Cloning $url into $path"
        & git clone $url $path
        if ($LASTEXITCODE -ne 0) { throw "git clone failed for $url" }
    }
    Write-Step "Checking out $commit"
    & git -C $path fetch --depth 1 origin $commit
    if ($LASTEXITCODE -ne 0) { throw "git fetch failed for $commit" }
    & git -C $path checkout --detach $commit
    if ($LASTEXITCODE -ne 0) { throw "git checkout failed for $commit" }
    $head = (& git -C $path rev-parse HEAD).Trim()
    if ($head -ne $commit) { throw "Unexpected HEAD $head, expected $commit" }
}

$sdk = Join-Path $thirdParty 'dlss-sdk'
Sync-Repository $sdk $SdkRepository $SdkCommit

$runtime = Join-Path $sdk 'lib/Windows_x86_64/rel/nvngx_dlss.dll'
$expected = '3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983'
$actual = (Get-FileHash -LiteralPath $runtime -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -ne $expected) { throw "nvngx_dlss.dll mismatch: $actual (expected $expected)" }
Write-Step "Verified release runtime SHA-256 $actual"

# Reconstruction backends and the transient presentation detours.
Sync-Repository (Join-Path $thirdParty 'fsr2-dx11') $Fsr2Repository $Fsr2Commit
Sync-Repository (Join-Path $thirdParty 'fsr-sdk') $FsrSdkRepository $FsrSdkCommit
Sync-Repository (Join-Path $thirdParty 'xess-sdk') $XessRepository $XessCommit
Sync-Repository (Join-Path $thirdParty 'minhook') $MinHookRepository $MinHookCommit
# Valve's OpenVR SDK headers: the VR frame-generation module compiles against the exact interface
# version the game's XRSDKOpenVR fork calls (IVRCompositor_029, IVRSystem_023, IVRSettings_003).
Sync-Repository (Join-Path $thirdParty 'openvr') $OpenVrRepository $OpenVrCommit
$openVrHeader = Join-Path $thirdParty 'openvr/headers/openvr.h'
if (-not (Test-Path -LiteralPath $openVrHeader)) { throw "OpenVR headers missing: $openVrHeader" }
$openVrText = Get-Content -LiteralPath $openVrHeader -Raw
if ($openVrText -notmatch 'IVRCompositor_029' -or $openVrText -notmatch 'IVRSystem_023') {
    throw "openvr.h at $openVrHeader does not expose IVRCompositor_029 and IVRSystem_023"
}
Write-Step "Verified OpenVR headers at $openVrHeader"

$runtimeHashes = @{
    'Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll' = 'd0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46'
    'Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_dx12.dll' = '02297beedd285e822d3a64f314cf00faf378dcec0edc47ff0c4dd71b3a8c2f18'
}
foreach ($entry in $runtimeHashes.GetEnumerator()) {
    $path = Join-Path (Join-Path $thirdParty 'fsr-sdk') $entry.Key
    if (-not (Test-Path -LiteralPath $path)) { throw "FidelityFX runtime missing: $path" }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $entry.Value) { throw "FidelityFX runtime hash mismatch: $path ($actual)" }
}
$xeSSRuntime = Join-Path $thirdParty 'xess-sdk/bin/libxess.dll'
if (-not (Test-Path -LiteralPath $xeSSRuntime)) { throw "XeSS runtime missing: $xeSSRuntime" }
$fgRuntimeHashes = @{
    'bin/libxess_fg.dll' = 'ec5e0c65e075570c6ede72618bb666d0be0c2e10b2ea9762c0fe8cb8e375ab27'
    'bin/libxell.dll' = 'd2030dcd694fda8f2ec7e044b13e6db8f0b56d4ba9113a5efad334e3f3ded8c7'
}
foreach ($entry in $fgRuntimeHashes.GetEnumerator()) {
    $path = Join-Path (Join-Path $thirdParty 'xess-sdk') $entry.Key
    if (-not (Test-Path -LiteralPath $path)) { throw "XeSS frame-generation runtime missing: $path" }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $entry.Value) { throw "XeSS frame-generation runtime hash mismatch: $path ($actual)" }
}
$dlssgRuntime = Join-Path $sdk 'lib/Windows_x86_64/rel/nvngx_dlssg.dll'
$expectedDlssg = 'ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82'
if (-not (Test-Path -LiteralPath $dlssgRuntime)) { throw "DLSS frame-generation runtime missing: $dlssgRuntime" }
$actualDlssg = (Get-FileHash -LiteralPath $dlssgRuntime -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualDlssg -ne $expectedDlssg) { throw "nvngx_dlssg.dll hash mismatch: $actualDlssg" }
Write-Step "Verified FidelityFX, XeSS and DLSS frame-generation runtimes"

if ($IncludeAnrFg) {
    # Apex Neural Rendering is a separate NGX-compatible provider with no redistribution grant in
    # its archive, so it is acquired into the local workspace and never staged into dist/.
    $anrUrl = 'https://github.com/apex-compute/ANR/releases/download/FG-V.B14/anr-fg-V.B14-addon.zip'
    $anrZipHash = 'cf123530037b2f349d70f6e13331cd255bbb69a17b610d4bc7fc103559f20d7e'
    $anrProviderHash = 'cd3329fe10a97d0359169eda5bb6d5c8ebdb8737cd4fde277de1aa474e624e86'
    $anrRoot = Join-Path $thirdParty 'anr-fg'
    $download = Join-Path $thirdParty 'anr-fg-V.B14-addon.zip'
    if (-not (Test-Path -LiteralPath $download)) {
        Write-Step "Downloading $anrUrl"
        Invoke-WebRequest -Uri $anrUrl -OutFile $download -UseBasicParsing
    }
    $zipHash = (Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($zipHash -ne $anrZipHash) { throw "ANR archive hash mismatch: $zipHash (expected $anrZipHash)" }
    $temp = Join-Path ([System.IO.Path]::GetTempPath()) ("bigscale-anr-" + [System.Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temp -Force | Out-Null
    try {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = [System.IO.Compression.ZipFile]::OpenRead($download)
        try {
            $root = (Resolve-Path -LiteralPath $temp).Path
            foreach ($entry in $archive.Entries) {
                if ([string]::IsNullOrEmpty($entry.Name)) { continue }
                $target = Join-Path $root $entry.FullName
                $full = [System.IO.Path]::GetFullPath($target)
                if (-not $full.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
                    throw "ANR archive entry escapes the extraction directory: $($entry.FullName)"
                }
                $parent = Split-Path -Parent $full
                if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
                [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $full, $true)
            }
        }
        finally {
            $archive.Dispose()
        }
        $source = Join-Path $temp 'Apex Neural Rendering FG/dlssg'
        if (-not (Test-Path -LiteralPath (Join-Path $source 'nvngx_dlssg.dll'))) {
            throw "The ANR archive has no 'Apex Neural Rendering FG/dlssg' runtime tree."
        }
        $providerHash = (Get-FileHash -LiteralPath (Join-Path $source 'nvngx_dlssg.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($providerHash -ne $anrProviderHash) { throw "ANR provider hash mismatch: $providerHash" }
        if (Test-Path -LiteralPath $anrRoot) { Remove-Item -LiteralPath $anrRoot -Recurse -Force }
        New-Item -ItemType Directory -Path $anrRoot -Force | Out-Null
        Copy-Item -Path (Join-Path $source '*') -Destination $anrRoot -Recurse -Force
        Write-Step "ANR frame generation staged locally at $anrRoot (never redistributed)"
    }
    finally {
        if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
    }
}

# NVIDIA Streamline supplies the DLSS-G host that the frame-generation provider drives, and the
# MFGAdaUnlock tree is the exact pinned source of the profile transactions that unlock multi-frame
# generation on Ada. The Streamline runtimes are not staged into the release payload; the provider
# stays unselectable and scripts/build.ps1 never copies sl.*.dll.
Sync-Repository (Join-Path $thirdParty 'mfg-ada-unlock') 'https://github.com/mavismmg/MFGAdaUnlock-RenoDx.git' '1226d4f2d60315ac807f614e2c290921265c69ea'
$slUrl = 'https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip'
$slDownload = Join-Path $thirdParty 'streamline-sdk-v2.14.16.zip'
$slRoot = Join-Path $thirdParty 'streamline'
$slArchiveHash = '32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b'
if (-not (Test-Path -LiteralPath $slDownload)) {
    Write-Step "Downloading $slUrl"
    Invoke-WebRequest -Uri $slUrl -OutFile $slDownload -UseBasicParsing
}
$slActual = (Get-FileHash -LiteralPath $slDownload -Algorithm SHA256).Hash.ToLowerInvariant()
if ($slActual -ne $slArchiveHash) {
    throw "Streamline archive hash mismatch: $slActual (expected $slArchiveHash). Delete '$slDownload' and rerun."
}
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("bigscale-sl-" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp -Force | Out-Null
try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($slDownload)
    try {
        $root = (Resolve-Path -LiteralPath $temp).Path
        foreach ($entry in $archive.Entries) {
            if ([string]::IsNullOrEmpty($entry.Name)) { continue }
            $target = Join-Path $root $entry.FullName
            $full = [System.IO.Path]::GetFullPath($target)
            if (-not $full.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "Streamline archive entry escapes the extraction directory: $($entry.FullName)"
            }
            $parent = Split-Path -Parent $full
            if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $full, $true)
        }
    }
    finally {
        $archive.Dispose()
    }
    # The archive wraps the SDK in one top-level directory whose name is not relied on; the
    # headers identify the real root, which is flattened onto third_party/streamline.
    $header = @(Get-ChildItem -LiteralPath $temp -Recurse -File -Filter 'sl.h' |
        Where-Object { $_.DirectoryName -match '(?i)[\\/]include$' } | Select-Object -First 1)
    if ($header.Count -eq 0) { throw "The Streamline archive has no include/sl.h." }
    $sdkRoot = Split-Path -Parent $header[0].DirectoryName
    if (Test-Path -LiteralPath $slRoot) { Remove-Item -LiteralPath $slRoot -Recurse -Force }
    New-Item -ItemType Directory -Path $slRoot -Force | Out-Null
    Copy-Item -Path (Join-Path $sdkRoot '*') -Destination $slRoot -Recurse -Force
}
finally {
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
}
$interposer = @(Get-ChildItem -LiteralPath $slRoot -Recurse -File -Filter 'sl.interposer.lib' | Select-Object -First 1)
$dlssg = @(Get-ChildItem -LiteralPath $slRoot -Recurse -File -Filter 'sl.dlss_g.dll' | Select-Object -First 1)
if ($interposer.Count -eq 0) { throw "Streamline SDK has no sl.interposer.lib under $slRoot" }
if ($dlssg.Count -eq 0) { throw "Streamline SDK has no sl.dlss_g.dll under $slRoot" }
$slDlssgHash = (Get-FileHash -LiteralPath $dlssg[0].FullName -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Step "Streamline SDK staged at $slRoot"
Write-Step "sl.interposer.lib: $($interposer[0].FullName)"
# The unlock profiles bind to exact images; this pins the one the development provider resolves to.
$expectedSlDlssg = '3ed9538df724993bea8a050195a6a78b9c8c8a566d5b05ade477aab3c390e90f'
if ($slDlssgHash -ne $expectedSlDlssg) {
    throw "sl.dlss_g.dll mismatch: $slDlssgHash (expected $expectedSlDlssg)"
}
Write-Step "Verified sl.dlss_g.dll SHA-256 $slDlssgHash"

# midpoint.hpp of the profile source includes this header without using a single ReShade symbol.
# The compat root keeps that include resolvable without vendoring the ReShade SDK.
$compatRoot = Join-Path (Join-Path $thirdParty 'mfg-compat') 'include'
New-Item -ItemType Directory -Path $compatRoot -Force | Out-Null
$compatHeader = @'
// Include compatibility for third_party/mfg-ada-unlock: mfgunlock/midpoint.hpp includes
// <include/reshade.hpp> but references no ReShade symbol, so an empty header satisfies the include.
#pragma once
'@
Set-Content -LiteralPath (Join-Path $compatRoot 'reshade.hpp') -Value $compatHeader -Encoding ASCII

Write-Host 'BigScale dependencies ready.' -ForegroundColor Green
