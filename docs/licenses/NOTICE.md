# BigScale — notices

Copyright 2026 Raharu. This package contains work from more than one source.

## Independently authored code

`BigScale.dll`, `BigScale.Native.dll` and everything under `src/` that is not listed below are
original work for this integration, licensed under the Apache License 2.0 (see the repository root
`LICENSE`, which this directory also carries as `APACHE-2.0.txt`). The project was previously
distributed as `BigWalkDLSS`; this release is a clean break with a new plugin GUID, config filename
and plugin directory, and no code from that name is redistributed under the old identity.

## Derived from Unity's Universal Render Pipeline

`src/managed/StockPostProcessing.cs` reproduces the effect ordering, per-effect XR conditions and
descriptor handling of
`Packages/com.unity.render-pipelines.universal/Runtime/Passes/PostProcessPassRenderGraph.cs`
from Unity Technologies' Graphics repository (commit
`4064d5bb6a1e3748a7cdddc244b89934ad1c3145`). It calls URP's own runtime helpers rather than copying
their bodies. Portions are therefore subject to the Unity Companion License; see
`UNITY-COMPANION-LICENSE.txt`. This software is not affiliated with or endorsed by Unity
Technologies.

## NVIDIA DLSS SDK

`nvngx_dlss.dll` and `nvngx_dlssg.dll` are the unmodified release runtimes from the NVIDIA DLSS SDK
310.9.1, commit `374959484e79a640feaba44c93ac8cfb0a03f5b5`. `BigScale.Native.dll` is linked against
the SDK's static NGX loader and its headers. This software contains source code provided by NVIDIA
Corporation. The SDK is used and redistributed under the NVIDIA RTX SDKs License; see
`NVIDIA-RTX-SDK-LICENSE.txt`. No NVIDIA development runtime, internal diagnostics or sample
application is included.

## AMD FidelityFX SDK and the FSR 2 DX11 port

The FidelityFX runtime files `amd_fidelityfx_upscaler_dx12.dll` (SHA-256
`d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46`) and
`amd_fidelityfx_framegeneration_dx12.dll` (SHA-256
`02297beedd285e822d3a64f314cf00faf378dcec0edc47ff0c4dd71b3a8c2f18`) are the unmodified signed
runtimes from FidelityFX SDK 2.3.0, commit `60f4ea81909200d8542eca14dccb2628b763a9a3`, and are used
under the MIT licence in `FIDELITYFX-SDK-LICENSE.md`; see also
`FIDELITYFX-SDK-THIRD-PARTY-NOTICES.md`. These two signed runtimes are loaded directly by this
plugin; no separate FidelityFX loader DLL is distributed or required. FSR 2 is statically linked
from the OptiScaler FidelityFX-FSR2-DX11 port at commit
`f2e3f86390746eb3f0bd1b28e91ea3cbc790ee76`, also MIT (`FIDELITYFX-LICENSE.txt`). FSR 3.1 and FSR 4
use the same installed FidelityFX provider.

## Intel XeSS SDK

`libxess.dll` (SHA-256 `251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7`),
`libxess_fg.dll` (SHA-256
`ec5e0c65e075570c6ede72618bb666d0be0c2e10b2ea9762c0fe8cb8e375ab27`) and `libxell.dll` (SHA-256
`d2030dcd694fda8f2ec7e044b13e6db8f0b56d4ba9113a5efad334e3f3ded8c7`) are the unmodified XeSS 3.0.2
runtimes from the Intel XeSS SDK, commit `8fe81bdbbaf00b3c1b733fd0d830c333dc84e6f0`. They are used
and redistributed under the Intel licence in `XESS-LICENSE.txt`; see also
`XESS-THIRD-PARTY-PROGRAMS.txt`.

## MinHook

`BigScale.Native.dll` statically links MinHook 1.3.4, commit
`c3fcafdc10146beb5919319d0683e44e3c30d537`, under the BSD-2-Clause licence; see
`MINHOOK-LICENSE.txt`. The detours are installed only for the desktop presenter and never replace
the game's swapchain.

## NVIDIA Streamline

The Streamline provider is work in progress and is not selectable in this release, so the release
archive does not distribute any `sl.*.dll`. The Streamline SDK's headers and the static interposer
import library are compiled into the development build of `BigScale.Native.dll` (from Streamline SDK
2.14.16, archive SHA-256
`32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b`); the interposer is loaded
dynamically at execution time and is never linked into a shipping payload. The Streamline licence and
third-party notices still apply to that source use; see `STREAMLINE-LICENSE.txt`,
`STREAMLINE-THIRD-PARTY-LICENSES.md` and `NVIDIA-RTX-SDK-LICENSE.txt`.

## MFGAdaUnlock (Skyrim/RenoDX add-on profiles)

The frame-generation provider applies byte-exact profile transactions derived from the MIT-licensed
MFGAdaUnlock-RenoDx project, commit `1226d4f2d60315ac807f614e2c290921265c69ea`, Copyright (c) 2026
ImDreamt; see `MFG-UNLOCK-LICENSE.txt` together with the nested licences staged beside it:
`MFG-UNLOCK-NEFH-LICENSE.txt`, `MFG-UNLOCK-DASHDDOGY-LICENSE.txt`,
`MFG-UNLOCK-FFX-API-LICENSE.txt` and `MFG-UNLOCK-MINHOOK-LICENSE.txt`. The technique (architecture
gates, interpolation-kernel temporal correction) originates with dashdogy's RTX40MFG-Unlock. The
headers are compiled into `BigScale.Native.dll` and patch the loaded NVIDIA modules in memory; no
modified NVIDIA binary is redistributed.

## Valve OpenVR SDK

`BigScale.Native.dll` compiles against the OpenVR SDK headers (commit
`91825305130f446f82054c1ec3d416321ace0072`, SDK 2.12.14) under the BSD-3-Clause licence; see
`OPENVR-LICENSE.txt`. Only interface declarations are used: `openvr_api.dll` is the game's own copy,
`VR_GetGenericInterface` is resolved from it at execution time, and nothing links `openvr_api.lib`.
No OpenVR runtime or driver is redistributed here.

## Integration reference

The public project Big Beautiful Walk (commit
`d062f9b41c4a7d24a497a45e95b1d76f70a6376d`) was consulted for the demonstrated IL2CPP interop
pattern, specifically the boxed-value workaround for URP's by-reference `TextureDesc` helper and the
use of an already-instantiated pass-data type in `RenderGraph`. No unrelated graphics controls,
upscaler backends, frame generation, installer or artwork from that project are included, its
monitor-only renderer behaviour is not part of this package, and it is no longer fetched by
`scripts/fetch-deps.ps1`.

## Not included

No game assembly, BepInEx assembly, Unity interop assembly, Big Walk VR file or Big Visuals file is
redistributed here. This package installs only its own plugin directory.
