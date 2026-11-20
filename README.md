# BigScale

Temporal super resolution and optional frame generation for **Big Walk** (Unity 6, IL2CPP, Direct3D
11), with an in-game `Upscaling` category and a hold-TAB status line.

> **VR warning.** VR support is experimental and currently does not work reliably. It may not work,
> and it may not be supported in future releases. Use the desktop path unless you are developing VR.

## What it does

BigScale replaces only URP's temporal upscale stage inside `PostProcessPassRenderGraph` and keeps
Unity's own effect ordering, jitter, motion vectors, depth and per-camera history. Reconstruction
backends:

| Value | Backend |
|---|---|
| `Dlss` | NVIDIA DLSS Super Resolution through NGX on D3D11 |
| `DlssPresetL` | NVIDIA DLSS with NGX render preset L pinned |
| `Fsr2` | AMD FidelityFX Super Resolution 2 on D3D11 |
| `Fsr3` | AMD FidelityFX Super Resolution 3.1 |
| `Fsr4` | AMD FidelityFX Super Resolution 4 through the DX12 interop bridge |
| `XeSs` | Intel XeSS through the DX12 interop bridge |

Every backend shares one provider-neutral preset set: `Quality`, `Balanced`, `Performance`,
`UltraPerformance`, `NativeAA` (DLAA for DLSS, Native AA elsewhere). The render extent comes from the
backend's own reported optimum for the active viewport, not from a global scale slider, and URP's
post-processing is preserved rather than bypassed: the UberPost chain still runs, at full output
resolution. In VR each eye keeps its own history and its own extent.

The hold-TAB overlay reports the actual input → output conversion from a fully retired frame, per
eye, plus the frame-generation rows. It never reports a probe or a queued packet as active.

## Status

| Area | State |
|---|---|
| Desktop super resolution | Supported |
| Desktop frame generation | Experimental |
| VR super resolution | Broken/experimental — see the warning above |
| Streamline frame-generation provider | Work in progress, not selectable |
| ANR frame-generation provider | Local development only, never distributed |

ANR is never part of a release archive. It can only be merged from a local tree the user fetched
themselves; the installer refuses to touch anything it did not create.

## Requirements

- **Big Walk**, launched once with **BepInEx 6 (IL2CPP)** so the interop assemblies exist.
- A **Direct3D 11** GPU. The game's renderer must stay D3D11; every backend reconstructs in that
  device, and the DX12 paths are bridges around it, never a replacement renderer.
- **NVIDIA RTX** hardware only for the DLSS backends. FSR and XeSS run on any D3D11 GPU their own
  runtimes support.
- To build: **Visual Studio 2022** with the C++ workload and a Windows SDK (for the native bridge),
  the **.NET SDK 6 or newer** (for the plugin), and the **.NET 10 SDK** (for the tests and tools).

## Installation

1. Unzip `BigScale.zip` into the game root, so the files land in `BepInEx/plugins/BigScale/`.
2. Launch the game. BepInEx creates `BepInEx/config/raharu.bigscale.cfg` on first load.
3. Press `Delete` (or your configured key) to open the `Upscaling` category.

Do not install this over a BigWalkDLSS installation; see the next section.

## Upgrading from BigWalkDLSS

This is a clean break, not an in-place upgrade. The plugin GUID, configuration filename, data
directory and plugin directory are all new, so the two cannot share state:

- Delete `BepInEx/plugins/BigWalkDLSS` before installing. `scripts/install.ps1` refuses to run while
  that directory exists and tells you the path; it never deletes files it did not create.
- The old configuration `BepInEx/config/local.bigwalk.dlss.cfg` is not read or migrated. You may keep
  it for reference or delete it.
- BigScale creates its own `BepInEx/config/raharu.bigscale.cfg` and its own `BepInEx/BigScale` data
  directory.
- If an old `local.bigwalk.*` renderer is still loaded, BigScale detects the conflicting GUID, logs
  it, leaves its own master switch disabled and preserves both provider selections.

## Configuration

The file is `BepInEx/config/raharu.bigscale.cfg`. It is created on first launch and is safe to
hand-edit while the game is closed. Three representative keys:

| Key | Values | Meaning |
|---|---|---|
| `General/Enabled` | `true`, `false` | Master switch for super resolution and frame generation. Disabling restores the game's own renderer without rewriting either selection. |
| `Upscaling/Upscaler` | `Dlss`, `DlssPresetL`, `Fsr2`, `Fsr3`, `Fsr4`, `XeSs` | Remembered reconstruction backend. |
| `FrameGeneration/Provider` | `Off`, `Nvidia`, `Fsr`, `XeSs`, `Anr` | Experimental desktop frame generation, independent of the upscaler. |

The remaining keys are `Upscaling/Mode` (quality preset), `FrameGeneration/VrProvider` (VR only,
experimental), `FrameGeneration/Multiplier` (total presented frames per source frame, 2–6),
`UI/MenuKey` (the in-game category key; `None` disables it) and the development-only
`[Diagnostics]` section below. BepInEx's generated `# Acceptable values:` lists are removed from this
file after every save; the in-game panel is the primary editor for everything the file does not
explain.

## Diagnostics (development only)

These five keys live in `[Diagnostics]`. They are configuration-only: they never appear in the
in-game panel and they are not meant for normal play. `XrProof` and `Dx12Validation` are read once at
startup, so changing them requires a restart.

| Key | Default | What it writes |
|---|---|---|
| `Enabled` | `false` | Aggregate per-eye counters, history and result codes plus one centre-texel sample read back natively. Never writes an image and never changes a pixel. |
| `FrameCaptureKey` | `F9` | With `Enabled`, captures the next five frame-generation pairs (real, HUD-less, generated, depth, motion) under `BepInEx/BigScale/fg-capture/`. `None` disables it. |
| `StageProbe` | `false` | 256×256 input/output hashes, BMP pairs at evaluations 120 and 600, and the 8×8 guide sample under `BepInEx/BigScale`. Adds GPU readbacks; changes no pixel. |
| `XrProof` | `false` | Starts the bounded presentation proof from `FrameCaptureKey` after a warm stereo history: identity band, late-pass removal, and the `xr-proof`/`xr-tap`/`sr-probe` BMPs. |
| `Dx12Validation` | `false` | Enables the D3D12 debug layer for the shared interop device. Very large slowdown; validation messages go to `BepInEx/BigScale/native.log`. |

## In-game panel

The `Upscaling` category has exactly five rows: **Enabled**, **Upscaler**, **Mode**, **Frame
Generation (Experimental)** and **Frame Multiplier**. The frame-generation row swaps to the VR
provider inside a VR session, where only `Off` and AMD FSR are offered. The hold-TAB status rows are
read-only: they show the upscaler conversion per eye, the frame-generation state and the measured
presented-frame total.

## Build and install

The game path is never stored in the repository. Pass `-GameDir`, or set `BIGSCALE_GAME_DIR`.

```powershell
# Fetch the pinned SDKs and runtimes into third_party/ (never tracked)
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/fetch-deps.ps1

# Build the bridge and the plugin, stage the payload and pack the zips
$env:BIGSCALE_GAME_DIR = 'D:\Games\Big Walk'
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -Clean

# Or install straight into the game (refuses while it runs, refuses an old install)
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/install.ps1 -GameDir $env:BIGSCALE_GAME_DIR
```

`build.ps1` also takes `-CMakePath` and `-DotnetPath` when the toolchain is not on `PATH`, and
`-SkipManaged` / `-SkipNative` for a partial build. `-Clean` removes `build/native` first and is what
a release build should use. Output: `dist/BepInEx/plugins/BigScale/`, `dist/BigScale.zip`,
`dist/BigScale-diagnostics.zip` (symbols only, never installed) and `dist/SHA256SUMS.txt`.

## Tests

```powershell
# Pure logic and the managed side of the ABI; no game required
dotnet run --project tests/LogicChecks -c Release
dotnet run --project tests/LogicChecks -c Release -- dist/BepInEx/plugins/BigScale/BigScale.Native.dll

# Real settings binding; needs a launched Big Walk install
dotnet run --project tests/SettingsChecks -c Release -p:GameDir="D:\Games\Big Walk"

# Native checks on a real device
cmake -S src/native -B build/native -G "Visual Studio 17 2022" -A x64 -DBIGSCALE_BUILD_CHECKS=ON
cmake --build build/native --config Release --target BigScaleChecks
build/native/Release/BigScaleChecks.exe
build/native/Release/BigScaleChecks.exe --upscaler dlss
```

Expected: `ALL CHECKS PASSED` for `LogicChecks`, `ALL SETTINGS CHECKS PASSED` for `SettingsChecks`, and
a zero failure count for the native checks. A provider check that cannot run on the available GPU
reports `NOT EXERCISED` with a reason; that is not a pass.

## Repository layout

| Path | Contents |
|---|---|
| `src/managed` | The C# BepInEx plugin (`BigScale.dll`) |
| `src/native` | The C++17 bridge (`BigScale.Native.dll`) and its providers |
| `tests` | `LogicChecks`, `SettingsChecks`, `NativeChecks` and a Streamline harness |
| `scripts` | `fetch-deps.ps1`, `build.ps1`, `install.ps1` |
| `tools` | Capture analysis helpers and the IL metadata dumper |
| `docs/licenses` | Upstream licences and the full notice |

`third_party/` holds the fetched SDKs and is never tracked or redistributed.

## Licence and notices

Original code is licensed under the Apache License 2.0; see [`LICENSE`](LICENSE). Third-party
components keep their own licences — NVIDIA RTX SDKs, AMD FidelityFX, Intel XeSS, MinHook, Valve
OpenVR, Unity Companion License for the URP-derived ordering code, and the MFGAdaUnlock profile
source. The complete attribution is in [`docs/licenses/NOTICE.md`](docs/licenses/NOTICE.md), which
also ships inside the release archive.

## Verification limits

The offline checks prove API execution, layout parity, conversion and coverage. They do not prove
in-game image quality, presentation, latency or stability on your hardware: that acceptance is manual,
and it is why the diagnostics section exists. A passing build is not a claim that a provider produces
a good image on every GPU.
