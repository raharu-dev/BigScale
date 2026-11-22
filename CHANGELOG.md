# Changelog

## BigScale 1.0.0 — 2026-11-22

First public release under the name **BigScale**. The project was previously distributed as
BigWalkDLSS; this release is a clean break rather than a rename in place.

### Neural Rendering (experimental, DLSS5)
- Opt-in NGX feature-18 stage that runs before or after the selected upscaler and always before
  frame generation. It reuses the D3D11↔D3D12 interop bridge and the converted per-eye scratch
  textures; a missing runtime, unsupported GPU/layout, extent below 256×256 or create/evaluate
  failure leaves the upscaler and frame generation untouched.
- New `[DLSS5]` configuration and a second `Neural Rendering` settings category with a master
  switch, Default/Natural/Cinematic presets, timing, three strength sliders, 1–5 sequential passes,
  a character mask and a read-only status row. `[DLSS5] Enabled` is only the category gate.
- The user-supplied `nvngx_dlssnr.dll` is never redistributed. The bridge exports the five NGX D3D12
  entry points and is copied as `nvngx.dll` beside the real runtime; no import table is patched.
  The alias must be a distinct copy: a hard link to the already-loaded `BigScale.Native.dll` resolves
  to that module, whose path lacks `nvngx.dll`, so the compatibility runtime rejects `Init_Ext`.
- The bridge ABI is now 5: `BIGSCALE_Frame` (192 bytes) carries the character mask pointer/extent and
  `BIGSCALE_Status.neuralState` reports the stage state.
- The character mask covers what each player's own shaders draw, local body included: every
  opaque character submesh is drawn with its material's forward pass into a private depth buffer,
  and only pixels that received depth are masked. A flat silhouette of the local body had left an
  unedited pill-shaped patch below the first-person view. Visibility is re-checked every frame
  against the camera's culling mask; inactive objects, shadow-only proxies, transparent blob
  shadows, decals, disabled renderers, non-mesh effects and the local nose are excluded.

### Identity and installation
- Plugin GUID `raharu.bigscale`, plugin directory `BepInEx/plugins/BigScale`, data directory
  `BepInEx/BigScale` and configuration file `BepInEx/config/raharu.bigscale.cfg`.
- Assemblies `BigScale.dll` and `BigScale.Native.dll`, native exports `BIGSCALE_*`, and CMake
  targets `BigScale.Native`, `BigScale.NativeShaders` and `BigScaleChecks`. The ABI versions are 5
  (super resolution), 7 (desktop frame generation) and 1 (VR frame generation); the neural
  configuration has its own `BIGSCALE_NEURAL_ABI_VERSION` 1.
- No automatic migration and no automatic deletion of a pre-rename BigWalkDLSS install. The
  installer fails with an actionable message when `BepInEx/plugins/BigWalkDLSS` still exists, and the
  plugin refuses to run beside a `local.bigwalk.*` renderer. For a pre-release BigScale payload, the
  installer replaces the 18 individual notice files with `THIRD-PARTY-NOTICES.txt`; a manual
  `providers/anr` tree is preserved.

### Configuration
- The schema is enum-typed and self-documenting: BepInEx's generated `# Acceptable values:` and
  `# Multiple values can be set` blocks are removed from this plugin's file after every save.
- New development-only `[Diagnostics]` section: `Enabled`, `FrameCaptureKey`, `StageProbe`,
  `XrProof`, `Dx12Validation` and `NativeLog`. These keys are configuration-only and never appear in
  the in-game panel. `XrProof` and `Dx12Validation` are read once at startup and need a restart.
- All legacy migration code is gone: no old key is read, migrated or deleted, and the removed
  bilinear-comparison switch is no longer special-cased.
- `Upscaling/Off` and an unreadable provider disable the master switch and remember Dlss instead of
  enabling a substitute provider.

### Frame generation
- The Streamline provider stays in the source but is not selectable; a stored value is normalized to
  `Off` with one explanation. Its `sl.*.dll` runtimes are no longer part of the release payload.
- ANR remains unshipped; the README links the newest ANR release and gives the `providers/anr` copy
  path; selecting ANR without the tree reports the manual-install warning.
- The presentation-proof boundary traces are compiled out of shipping builds; only the offline
  harness measures pair timing.

### VR
- VR support remains in the source and is **experimental and currently broken**; it is documented as
  such rather than presented as supported.
- VR frame generation is selectable only as `Off` or AMD FSR, and its guide log is bounded to the
  first three packets and then every 600th instead of every source frame.

### Performance and stability
- The stale-lease and settlement paths reuse their lists instead of allocating per frame, and the
  per-packet struct sizes are cached instead of re-measured with `Marshal.SizeOf` on every frame.
- The native log keeps one handle open and rotates at 16 MiB instead of opening and closing the file
  for every line.
- The late-pass inventory and the XR proof taps are skipped entirely when their diagnostics are off,
  which also removes a per-frame list allocation in normal VR play.
- The Big Visuals assembly scan is throttled to once every 30 frames, and an empty player-character
  scan now backs off instead of running every frame.

### Verification
- `tests/LogicChecks` covers the pure logic and the managed side of the ABI; `tests/SettingsChecks`
  binds the real `ModSettings` against the installed BepInEx core; `tests/NativeChecks` exercises the
  production native sources on a real device. Provider checks report `NOT EXERCISED` when the
  hardware or runtime is absent, never a pass.

### Packaging, documentation and licensing
- Root `LICENSE` (Apache-2.0), a short root `NOTICE` with the full attribution in
  `docs/licenses/NOTICE.md`, a `.gitignore`, and a rewritten `README.md`. The release carries
  `LICENSE`, `THIRD-PARTY-NOTICES.txt`, `README.txt` and `_ADDITIONAL_PROVIDERS.txt`; all 16 upstream
  licence and notice texts plus the attribution narrative are preserved verbatim in the combined
  notice. The installer preserves a manually installed `providers/anr` tree. No game, BepInEx, Unity,
  Big Walk VR or Big Visuals assembly is redistributed, and no `sl.*.dll` is shipped.

## BigWalkDLSS history (pre-rename)

The entries below describe the builds that were published under the previous name. Crash-dump
analysis and per-frame investigations are omitted; only user-facing changes remain.

### 1.0.0
- Initial implementation: temporal super-resolution through NVIDIA DLSS, replacing only URP's
  temporal upscale stage while keeping Unity's effect ordering, jitter, motion vectors, depth and
  per-camera history.
- Flat cdecl native ABI with struct sizes and every field offset compared at startup, a fixed packet
  ring, and an isolated D3D11 context state around all plugin work.
- `NVIDIA DLSS` settings category cloned from the game's own uGUI controls.

### 1.0.1
- Repairs the recorded launch crash: the pipeline asset's upscaling filter is never written, so the
  game's own STP stage cannot be requested on top of the reconstruction.
- Ownership is applied at a pre-camera boundary that actually runs for gameplay cameras, with every
  camera and asset value snapshotted before its first write and restored on disable, fault, camera
  change and unload. Faults latch until an explicit settings transition.
- Adds the Delete-key menu.

### 1.0.2
- Delete opens the game's own pause/settings screen first and builds the category from the game's
  `SettingsMenu` lifecycle instead of before its serialized fields exist.
- Capability probes are validated per operation, so a read-only probe no longer needs the full
  temporal input set.
- Completions distinguish an **executed** packet from a **retired** one: only a retired packet
  releases a resource or proves activity.

### 1.0.3
- Row captions and values bind the visible TMP components and survive a language refresh.
- The category tab is placed after Graphics and the eight stock tabs are reflowed inside their own
  column, so nothing overlaps.
- A recorded tick failure now reports `Faulted` with its reason instead of a stale `Active`.

### 1.0.4
- Features and scratch textures are created at exactly the evaluated render extent, and the active
  viewport is read before any post-upscale write, fixing a reduced image anchored to the lower-left.
- When URP allocates its post-processing attachment at the reduced extent, the stock `UberPost` is
  written into a full-output texture so URP's final pass performs no second spatial upscale.
- The settings category is built through the native component system, so the IL2CPP `RectTransform`
  downcast no longer prevents it from appearing; the shortcut applies menu mode and cursor release
  only to a pause screen it opened itself.

### 1.0.5 — frame-generation capture and quality
- Desktop frame generation for the NVIDIA, ANR, FSR and XeSS providers, with a bounded diagnostic
  capture of real, HUD-less, generated, depth and motion planes.
- The HUD-less source is the game's imported backbuffer and the exact graph texture, validated by
  identity rather than by a graph descriptor.
- NVIDIA DLSS gains render preset L, and provider teardown, suppression and error reporting became
  truthful per provider.

### 1.0.6 — VR per-eye and multi-frame generation
- Repairs a launch crash in a VR session and reconstructs each eye separately with per-eye history,
  including an optional AMD FSR frame-generation path inserted at the VR compositor.
- Player characters carry real per-object motion through URP's own culled motion pass instead of the
  camera-only field.
- Real multiple-frame generation with a truthful multiplier and restart state, hold-TAB status rows
  showing the measured total, and frame generation locked off inside a VR session unless the VR
  provider is the one selected.
