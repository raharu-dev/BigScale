#requires -Version 5.1
<#
.SYNOPSIS
  Builds BigScale into dist/ and packs dist/BigScale.zip and dist/BigScale-diagnostics.zip.

.DESCRIPTION
  Compiles the native bridge with the pinned Visual Studio CMake generator and the managed plugin
  with the .NET SDK, then stages only this mod's own files plus LICENSE, THIRD-PARTY-NOTICES.txt,
  README.txt and _ADDITIONAL_PROVIDERS.txt. The combined notice is composed from docs/licenses/ and
  third_party/, and the staged payload is checked against its release contract: nine binaries and
  those four text files.
  Game, BepInEx, interop, Big Walk VR and Big Visuals assemblies are never copied, and neither are
  the Streamline runtimes: that provider is not selectable in this release.
#>
[CmdletBinding()]
param(
    [string]$GameDir = '',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$NgxSdkDir = '',
    [string]$CMakePath = '',
    [string]$DotnetPath = '',
    [switch]$Clean,
    [switch]$SkipNative,
    [switch]$SkipManaged
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$expectedRuntimeHash = '3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983'
$expectedXessHash = '251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7'
$expectedUpscalerHash = 'd0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46'
$expectedFgDsrHash = '02297beedd285e822d3a64f314cf00faf378dcec0edc47ff0c4dd71b3a8c2f18'
$expectedDlssgHash = 'ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82'
$expectedXessFgHash = 'ec5e0c65e075570c6ede72618bb666d0be0c2e10b2ea9762c0fe8cb8e375ab27'
$expectedXellHash = 'd2030dcd694fda8f2ec7e044b13e6db8f0b56d4ba9113a5efad334e3f3ded8c7'

function Write-Step($message) { Write-Host "==> $message" -ForegroundColor Cyan }

# --- toolchain -------------------------------------------------------------------------------
$cmake = $CMakePath
if (-not [string]::IsNullOrWhiteSpace($cmake)) {
    if (-not (Test-Path -LiteralPath $cmake)) { throw "CMakePath '$cmake' does not exist." }
}
else {
    $cmakeCandidates = @(
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio/2022/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio/2022/Enterprise/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe')
    ) | Where-Object { Test-Path -LiteralPath $_ }
    $cmake = $cmakeCandidates | Select-Object -First 1
    if (-not $cmake) {
        $cmake = (Get-Command cmake.exe -ErrorAction SilentlyContinue | Select-Object -First 1).Source
    }
    if (-not $cmake) { throw 'CMake was not found. Install Visual Studio 2022 with the C++ CMake tools, or pass -CMakePath.' }
}

$dotnet = $DotnetPath
if (-not [string]::IsNullOrWhiteSpace($dotnet)) {
    if (-not (Test-Path -LiteralPath $dotnet)) { throw "DotnetPath '$dotnet' does not exist." }
}
else {
    $dotnet = (Get-Command dotnet.exe -ErrorAction SilentlyContinue | Select-Object -First 1).Source
    if (-not $dotnet) { throw '.NET SDK was not found on PATH, or pass -DotnetPath.' }
}

if ([string]::IsNullOrWhiteSpace($NgxSdkDir)) { $NgxSdkDir = Join-Path $repo 'third_party/dlss-sdk' }
if (-not (Test-Path -LiteralPath (Join-Path $NgxSdkDir 'include/nvsdk_ngx.h'))) {
    throw "Pinned NVIDIA DLSS SDK not found at '$NgxSdkDir'. Run scripts/fetch-deps.ps1 first."
}

# The game is only needed for the managed compile, which references the game's interop assemblies.
$gamePath = ''
if (-not $SkipManaged) {
    if ([string]::IsNullOrWhiteSpace($GameDir)) { $GameDir = $env:BIGSCALE_GAME_DIR }
    if ([string]::IsNullOrWhiteSpace($GameDir)) {
        throw 'GameDir was not supplied. Pass -GameDir or set BIGSCALE_GAME_DIR to a Big Walk install ' +
            'that has completed one BepInEx IL2CPP launch, or build with -SkipManaged.'
    }
    $gamePath = (Resolve-Path -LiteralPath $GameDir).Path
    if (-not (Test-Path -LiteralPath (Join-Path $gamePath 'BepInEx/interop/Assembly-CSharp.dll'))) {
        throw "GameDir '$gamePath' has no BepInEx/interop/Assembly-CSharp.dll. Launch the game once with BepInEx installed."
    }
}

$nativeOutput = $null
if (-not $SkipNative) {
    $buildDir = Join-Path $repo 'build/native'
    if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
        Write-Step 'Removing build/native'
        Remove-Item -LiteralPath $buildDir -Recurse -Force
    }
    Write-Step "Configuring native bridge ($Configuration)"
    & $cmake -S (Join-Path $repo 'src/native') -B $buildDir -G 'Visual Studio 17 2022' -A x64 `
        "-DNGX_SDK_DIR=$NgxSdkDir" -DBIGSCALE_BUILD_CHECKS=OFF
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }

    Write-Step "Building native bridge ($Configuration)"
    & $cmake --build $buildDir --config $Configuration --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
    $nativeOutput = Join-Path $buildDir "$Configuration/BigScale.Native.dll"
    if (-not (Test-Path -LiteralPath $nativeOutput)) { throw "Native bridge output missing: $nativeOutput" }
}

$managedOutput = $null
if (-not $SkipManaged) {
    Write-Step "Building managed plugin ($Configuration)"
    & $dotnet build (Join-Path $repo 'src/managed/BigScale.csproj') -c $Configuration "-p:GameDir=$gamePath" --nologo
    if ($LASTEXITCODE -ne 0) { throw 'Managed build failed.' }
    $managedOutput = Join-Path $repo "src/managed/bin/$Configuration/net6.0/BigScale.dll"
    if (-not (Test-Path -LiteralPath $managedOutput)) { throw "Managed plugin output missing: $managedOutput" }
}

# --- stage -----------------------------------------------------------------------------------
$distRoot = Join-Path $repo 'dist'
$plugin = Join-Path $distRoot 'BepInEx/plugins/BigScale'
if (Test-Path -LiteralPath $distRoot) { Remove-Item -LiteralPath $distRoot -Recurse -Force }
New-Item -ItemType Directory -Path $plugin -Force | Out-Null

# One attribution file replaces the per-upstream copies. Order is part of the shipped document, so
# the sources are listed here rather than discovered by wildcard.
$noticeSections = @(
    @{ Title = 'Unity Companion License'; Source = 'docs/licenses/UNITY-COMPANION-LICENSE.txt' },
    @{ Title = 'NVIDIA RTX SDKs License'; Source = 'docs/licenses/NVIDIA-RTX-SDK-LICENSE.txt' },
    @{ Title = 'AMD FidelityFX SDK License'; Source = 'third_party/fsr-sdk/docs/license.md' },
    @{ Title = 'AMD FidelityFX SDK Third-Party Notices'; Source = 'third_party/fsr-sdk/3rdpartynotice.md' },
    @{ Title = 'FidelityFX FSR 2 DX11 License'; Source = 'third_party/fsr2-dx11/LICENSE.txt' },
    @{ Title = 'Intel XeSS SDK License'; Source = 'third_party/xess-sdk/LICENSE.txt' },
    @{ Title = 'Intel XeSS Third-Party Programs'; Source = 'third_party/xess-sdk/third-party-programs.txt' },
    @{ Title = 'MinHook License'; Source = 'third_party/minhook/LICENSE.txt' },
    @{ Title = 'NVIDIA Streamline License'; Source = 'third_party/streamline/license.txt' },
    @{ Title = 'NVIDIA Streamline Third-Party Licenses'; Source = 'third_party/streamline/3rd-party-licenses.md' },
    @{ Title = 'MFGAdaUnlock License'; Source = 'third_party/mfg-ada-unlock/LICENSE' },
    @{ Title = 'MFGAdaUnlock NEFH License'; Source = 'third_party/mfg-ada-unlock/src/addons/mfgunlock/ampere/LICENSE.nefh' },
    @{ Title = 'MFGAdaUnlock dashdogy License'; Source = 'third_party/mfg-ada-unlock/src/addons/mfgunlock/xemfg/vendor/LICENSE.dashdogy.txt' },
    @{ Title = 'MFGAdaUnlock FFX API License'; Source = 'third_party/mfg-ada-unlock/src/addons/mfgunlock/xemfg/vendor/ffx_api/LICENSE.txt' },
    @{ Title = 'MFGAdaUnlock MinHook License'; Source = 'third_party/mfg-ada-unlock/src/addons/mfgunlock/xemfg/vendor/third_party/minhook/LICENSE.txt' },
    @{ Title = 'Valve OpenVR License'; Source = 'third_party/openvr/LICENSE' }
)

if ($managedOutput) { Copy-Item -LiteralPath $managedOutput -Destination $plugin }
if ($nativeOutput) {
    Copy-Item -LiteralPath $nativeOutput -Destination $plugin
    $runtime = Join-Path $NgxSdkDir 'lib/Windows_x86_64/rel/nvngx_dlss.dll'
    if (-not (Test-Path -LiteralPath $runtime)) { throw "Release runtime not found: $runtime" }
    $hash = (Get-FileHash -LiteralPath $runtime -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $expectedRuntimeHash) {
        throw "nvngx_dlss.dll hash mismatch. Expected $expectedRuntimeHash but found $hash."
    }
    Copy-Item -LiteralPath $runtime -Destination (Join-Path $plugin 'nvngx_dlss.dll')
    Write-Step "Verified nvngx_dlss.dll SHA-256 $hash"

    # Reconstruction runtimes. A missing vendored SDK fails the build rather than shipping an
    # installer that silently lacks a provider. Streamline's sl.*.dll files are deliberately absent:
    # that provider is work in progress and not selectable, so none of its runtimes are distributed.
    $xess = Join-Path $repo 'third_party/xess-sdk/bin/libxess.dll'
    $upscaler = Join-Path $repo 'third_party/fsr-sdk/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll'
    $fsrFg = Join-Path $repo 'third_party/fsr-sdk/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_dx12.dll'
    $dlssg = Join-Path $NgxSdkDir 'lib/Windows_x86_64/rel/nvngx_dlssg.dll'
    $xessFg = Join-Path $repo 'third_party/xess-sdk/bin/libxess_fg.dll'
    $xell = Join-Path $repo 'third_party/xess-sdk/bin/libxell.dll'
    foreach ($staged in @(
        @{ Path = $xess; Name = 'libxess.dll'; Hash = $expectedXessHash },
        @{ Path = $upscaler; Name = 'amd_fidelityfx_upscaler_dx12.dll'; Hash = $expectedUpscalerHash },
        @{ Path = $fsrFg; Name = 'amd_fidelityfx_framegeneration_dx12.dll'; Hash = $expectedFgDsrHash },
        @{ Path = $dlssg; Name = 'nvngx_dlssg.dll'; Hash = $expectedDlssgHash },
        @{ Path = $xessFg; Name = 'libxess_fg.dll'; Hash = $expectedXessFgHash },
        @{ Path = $xell; Name = 'libxell.dll'; Hash = $expectedXellHash })) {
        if (-not (Test-Path -LiteralPath $staged.Path)) { throw "Optional runtime not found: $($staged.Path). Run scripts/fetch-deps.ps1." }
        $actual = (Get-FileHash -LiteralPath $staged.Path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $staged.Hash) { throw "$($staged.Name) hash mismatch: $actual" }
        Copy-Item -LiteralPath $staged.Path -Destination (Join-Path $plugin $staged.Name)
        Write-Step "Verified $($staged.Name) SHA-256 $actual"
    }
}

# --- licence and notice -----------------------------------------------------------------------
# Text is read with ReadAllText so a source's own encoding and line endings survive; the output is
# UTF-8 without a BOM whatever the sources were.
$narrative = Join-Path $repo 'docs/licenses/NOTICE.md'
if (-not (Test-Path -LiteralPath $narrative)) { throw "Attribution narrative missing: $narrative" }

$combined = New-Object System.Text.StringBuilder
$null = $combined.AppendLine('BigScale - third-party notices')
$null = $combined.AppendLine()
$null = $combined.AppendLine('This file collects the attribution notices and complete licence texts for third-party')
$null = $combined.AppendLine('components included in or compiled into BigScale. Each section retains its own licence; the')
$null = $combined.AppendLine('compilation itself is not relicensed. BigScale''s own code is licensed under the Apache License')
$null = $combined.AppendLine('2.0 in the accompanying LICENSE file.')
$null = $combined.Append(([IO.File]::ReadAllText($narrative)).TrimEnd() + [Environment]::NewLine)

$banner = '=' * 80
foreach ($section in $noticeSections) {
    $sectionPath = Join-Path $repo $section.Source
    if (-not (Test-Path -LiteralPath $sectionPath)) {
        throw "Notice source missing: $sectionPath. Run scripts/fetch-deps.ps1."
    }
    $null = $combined.AppendLine()
    $null = $combined.AppendLine($banner)
    $null = $combined.AppendLine($section.Title)
    $null = $combined.AppendLine($banner)
    $null = $combined.AppendLine()
    $null = $combined.Append(([IO.File]::ReadAllText($sectionPath)).TrimEnd() + [Environment]::NewLine)
}

[IO.File]::WriteAllText((Join-Path $plugin 'THIRD-PARTY-NOTICES.txt'), $combined.ToString(), (New-Object System.Text.UTF8Encoding($false)))
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination (Join-Path $plugin 'LICENSE')
Copy-Item -LiteralPath (Join-Path $repo 'docs/README-release.txt') -Destination (Join-Path $plugin 'README.txt')
Copy-Item -LiteralPath (Join-Path $repo 'docs/_ADDITIONAL_PROVIDERS.txt') -Destination (Join-Path $plugin '_ADDITIONAL_PROVIDERS.txt')
Write-Step "Composed THIRD-PARTY-NOTICES.txt from $($noticeSections.Count + 1) sources"

# --- payload contract -------------------------------------------------------------------------
# The release is a fixed shape, so a stray notice copy or a forgotten runtime fails the build here
# rather than in the archive.
$payloadFiles = @(Get-ChildItem -LiteralPath $plugin -Recurse -File)
$textNames = @('LICENSE', 'THIRD-PARTY-NOTICES.txt', 'README.txt', '_ADDITIONAL_PROVIDERS.txt')
$binaries = @(
    'BigScale.dll', 'BigScale.Native.dll', 'amd_fidelityfx_framegeneration_dx12.dll',
    'amd_fidelityfx_upscaler_dx12.dll', 'libxell.dll', 'libxess.dll', 'libxess_fg.dll',
    'nvngx_dlss.dll', 'nvngx_dlssg.dll'
)
$strayText = @($payloadFiles | Where-Object {
        ($_.Extension -eq '.txt' -or $_.Extension -eq '.md') -and $textNames -notcontains $_.Name
    })
if ($strayText.Count -gt 0) {
    throw ("Payload carries text files that are not part of the release: " +
        (($strayText | ForEach-Object { $_.Name }) -join ', ') +
        '. The payload ships LICENSE, THIRD-PARTY-NOTICES.txt, README.txt and _ADDITIONAL_PROVIDERS.txt only.')
}
$missingText = @($textNames | Where-Object { -not (Test-Path -LiteralPath (Join-Path $plugin $_)) })
if ($missingText.Count -gt 0) { throw "Payload is missing: $($missingText -join ', ')." }
if ($nativeOutput -and $managedOutput) {
    $expectedNames = @($binaries) + $textNames
    $actualNames = @($payloadFiles | ForEach-Object { $_.Name })
    $missing = @($expectedNames | Where-Object { $actualNames -notcontains $_ })
    $unexpected = @($actualNames | Where-Object { $expectedNames -notcontains $_ })
    $expectedCount = $binaries.Count + $textNames.Count
    if ($missing.Count -gt 0 -or $unexpected.Count -gt 0 -or $payloadFiles.Count -ne $expectedCount) {
        $detail = "Expected $expectedCount payload files ($($binaries.Count) binaries plus " +
            ($textNames -join ', ') + "); found $($payloadFiles.Count)."
        if ($missing.Count -gt 0) { $detail += " Missing: $($missing -join ', ')." }
        if ($unexpected.Count -gt 0) { $detail += " Unexpected: $($unexpected -join ', ')." }
        throw $detail
    }
    Write-Step "Payload contract verified: $($payloadFiles.Count) files"
}

# --- diagnostics symbols ----------------------------------------------------------------------
# Native symbols go into their own local archive. The managed assembly is built without a PDB so
# the shipping DLL carries no absolute build path. Neither symbol file is staged into the plugin
# directory, so the installed runtime is exactly the released binaries.
$symbols = @()
if ($nativeOutput) {
    $nativePdb = [System.IO.Path]::ChangeExtension($nativeOutput, '.pdb')
    if (Test-Path -LiteralPath $nativePdb) { $symbols += $nativePdb }
}
if ($managedOutput) {
    $managedPdb = [System.IO.Path]::ChangeExtension($managedOutput, '.pdb')
    if (Test-Path -LiteralPath $managedPdb) { $symbols += $managedPdb }
}
$diagnostics = Join-Path $distRoot 'BigScale-diagnostics.zip'
if ($symbols.Count -gt 0) {
    $symbolDir = Join-Path $distRoot 'symbols'
    New-Item -ItemType Directory -Path $symbolDir -Force | Out-Null
    foreach ($symbol in $symbols) { Copy-Item -LiteralPath $symbol -Destination $symbolDir }
    Compress-Archive -Path (Join-Path $symbolDir '*') -DestinationPath $diagnostics -Force
    Remove-Item -LiteralPath $symbolDir -Recurse -Force
    Write-Step "Diagnostics symbols: $diagnostics"
}

# --- package ---------------------------------------------------------------------------------
$zip = Join-Path $distRoot 'BigScale.zip'
Write-Step 'Packing dist/BigScale.zip'
Compress-Archive -Path (Join-Path $distRoot 'BepInEx') -DestinationPath $zip -Force

Write-Step 'Staged payload'
Get-ChildItem -LiteralPath $plugin -Recurse -File |
    Sort-Object Name |
    ForEach-Object { '{0,12:N0}  {1}' -f $_.Length, $_.Name }

$manifest = @()
foreach ($file in (Get-ChildItem -LiteralPath $plugin -Recurse -File | Sort-Object Name)) {
    $manifest += '{0}  {1}' -f (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $file.Name
}
$manifest += '{0}  {1}' -f (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant(), 'BigScale.zip'
if (Test-Path -LiteralPath $diagnostics) {
    $manifest += '{0}  {1}' -f (Get-FileHash -LiteralPath $diagnostics -Algorithm SHA256).Hash.ToLowerInvariant(), 'BigScale-diagnostics.zip'
}
Set-Content -LiteralPath (Join-Path $distRoot 'SHA256SUMS.txt') -Value $manifest -Encoding ascii
Write-Step 'SHA-256 manifest written to dist/SHA256SUMS.txt'
Write-Host "Package: $zip" -ForegroundColor Green
