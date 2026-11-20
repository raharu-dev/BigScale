using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering.Universal;

namespace BigScale;

/// <summary>
/// Takes ownership of the scene scale and the temporal inputs BigScale needs, and restores exactly what
/// was there before. Every value is snapshotted <em>before</em> its first write, snapshots are keyed
/// on live Unity objects so a destroyed and recreated camera or pipeline asset can never inherit a
/// stale entry, and the pipeline's upscaling filter is never changed: requesting STP there would make
/// the game's own temporal stage run as well.
/// </summary>
internal sealed class GraphicsOwnership
{
    private sealed class CameraState
    {
        internal Camera Camera = null!;
        internal UniversalAdditionalCameraData Additional = null!;
        internal bool AllowDynamicResolution;
        internal bool RenderPostProcessing;
        internal bool RequiresDepthTexture;
        internal bool RequiresColorTexture;
        internal AntialiasingMode Antialiasing;
        internal bool Written;
    }

    private sealed class AssetState
    {
        internal UniversalRenderPipelineAsset Asset = null!;
        internal float RenderScale;
        internal int MsaaSampleCount;
        internal bool Written;
        /// <summary>
        /// Set only when this mod actually wrote <c>renderScale</c>. In an XR session the scale stays
        /// scene-owned, because URP forwards it to the XR runtime and would resize the SteamVR eye
        /// textures instead of an intermediate.
        /// </summary>
        internal bool ScaleWritten;
    }

    private readonly List<CameraState> _cameras = new();
    private AssetState? _asset;
    private readonly Action<string> _log;

    internal GraphicsOwnership(Action<string> log) => _log = log;

    internal bool Owned { get; private set; }

    /// <summary>The pipeline asset this ownership belongs to, or null when nothing is owned.</summary>
    internal UniversalRenderPipelineAsset? Asset => _asset?.Asset;

    /// <summary>Original scale/filter/AA captured before BigScale claimed rendering.</summary>
    internal float OriginalRenderScale => _asset?.RenderScale ?? 1f;
    internal int OriginalMsaaSampleCount => _asset?.MsaaSampleCount ?? 1;

    /// <summary>True when this asset is the one currently snapshotted.</summary>
    internal bool IsAsset(UniversalRenderPipelineAsset asset) => _asset != null && _asset.Asset == asset;

    /// <summary>
    /// Snapshots the asset and camera if they are not already captured, then applies the owned
    /// requests. Snapshotting always happens before the first write, so a restore can never retain
    /// this mod's own values.
    /// </summary>
    internal void Acquire(UniversalRenderPipelineAsset asset, Camera camera,
        UniversalAdditionalCameraData additional, float scale, bool ownScale, bool useUrpTemporalAA)
    {
        if (_asset != null && _asset.Asset != asset)
        {
            // A different pipeline asset took over: restore the previous one before dropping it.
            RestoreAsset();
            _asset = null;
        }
        if (_asset == null)
        {
            _asset = new AssetState
            {
                Asset = asset,
                RenderScale = asset.renderScale,
                MsaaSampleCount = asset.msaaSampleCount,
            };
            _log(ownScale
                ? $"BigScale takes ownership of the scene scale: original scale={asset.renderScale:F4} " +
                  $"msaa={asset.msaaSampleCount} upscalingFilter={asset.upscalingFilter} (left unchanged)."
                : $"BigScale takes ownership of the camera temporal inputs: scale={asset.renderScale:F4} stays " +
                  $"scene-owned (XR session), msaa={asset.msaaSampleCount} " +
                  $"upscalingFilter={asset.upscalingFilter} (left unchanged).");
        }
        Capitalise();
        Apply(asset, camera, additional, scale, ownScale, useUrpTemporalAA);
    }

    /// <summary>Applies the owned requests. Snapshots anything not yet captured first.</summary>
    internal void Apply(UniversalRenderPipelineAsset asset, Camera camera,
        UniversalAdditionalCameraData additional, float scale, bool ownScale, bool useUrpTemporalAA)
    {
        if (_asset == null || _asset.Asset != asset)
        {
            // Never write without a snapshot; fall back to a full acquire.
            Acquire(asset, camera, additional, scale, ownScale, useUrpTemporalAA);
            return;
        }

        var state = Find(camera);
        if (state == null || state.Additional != additional)
        {
            if (state != null) RestoreCamera(state);
            _cameras.RemoveAll(existing => existing.Camera == camera || existing.Camera == null);
            state = new CameraState
            {
                Camera = camera,
                Additional = additional,
                AllowDynamicResolution = camera.allowDynamicResolution,
                RenderPostProcessing = additional.renderPostProcessing,
                RequiresDepthTexture = additional.requiresDepthTexture,
                RequiresColorTexture = additional.requiresColorTexture,
                Antialiasing = additional.antialiasing,
            };
            _cameras.Add(state);
            _log($"temporal input snapshot camera={camera.name} id={camera.GetInstanceID()} " +
                 $"postProcessing={state.RenderPostProcessing} aa={state.Antialiasing} " +
                 $"depth={state.RequiresDepthTexture} dynamicResolution={state.AllowDynamicResolution}.");
        }

        // The scale is global; MSAA is global too and must be 1 or URP refuses temporal input.
        // Mark the snapshot written *before* the first mutation so a partial write still restores.
        if (_asset != null) _asset.Written = true;
        if (ownScale)
        {
            asset.renderScale = scale;
            if (_asset != null) _asset.ScaleWritten = true;
        }
        asset.msaaSampleCount = 1;

        // URP's IsTemporalAAEnabled() gates jitter, motion vectors and the depth texture, so Unity
        // TAA is still requested where the replacement orchestration depends on it. In an XR
        // session the stock post chain runs unchanged and would resolve the image a second time,
        // so the owned request there is None: depth and motion are requested through the renderer's
        // input summary instead. The pipeline's upscaling filter stays as the user configured it.
        additional.antialiasing = useUrpTemporalAA
            ? AntialiasingMode.TemporalAntiAliasing
            : AntialiasingMode.None;
        additional.requiresDepthTexture = true;
        additional.renderPostProcessing = true;
        camera.allowDynamicResolution = false;
        state.Written = true;
        Capitalise();
    }

    /// <summary>
    /// Reapplies the already-snapshotted requests when the scene drifted them, without capturing the
    /// drifted value as the new baseline. Called at the pre-camera-data boundary, where writing the
    /// pipeline asset is legal and before URP derives its camera data. Returns true when it wrote.
    /// </summary>
    internal bool EnsureApplied(UniversalRenderPipelineAsset asset, Camera camera,
        UniversalAdditionalCameraData additional, float scale, bool ownScale, bool useUrpTemporalAA)
    {
        if (_asset == null || _asset.Asset != asset)
        {
            Acquire(asset, camera, additional, scale, ownScale, useUrpTemporalAA);
            return true;
        }
        var driftedScale = asset.renderScale;
        var driftedMsaa = asset.msaaSampleCount;
        var expectedAntialiasing = useUrpTemporalAA
            ? AntialiasingMode.TemporalAntiAliasing
            : AntialiasingMode.None;
        var state = Find(camera);
        // A scene load can rewrite the camera's own requests without touching the global scale.
        var cameraDrifted = state != null && (additional.antialiasing != expectedAntialiasing ||
            !additional.requiresDepthTexture || !additional.renderPostProcessing ||
            camera.allowDynamicResolution);
        // A scene-owned scale is not drift: in an XR session the game and Big Visuals keep sizing the
        // eye textures, and only MSAA plus the camera requests are this mod's to reassert.
        var scaleDrifted = ownScale && System.Math.Abs(driftedScale - scale) > 1e-6f;
        if (!scaleDrifted && driftedMsaa == 1 && !cameraDrifted)
            return false;
        Apply(asset, camera, additional, scale, ownScale, useUrpTemporalAA);
        _log(cameraDrifted
            ? $"camera {camera.name} drifted to aa={additional.antialiasing} " +
              $"depth={additional.requiresDepthTexture} post={additional.renderPostProcessing} " +
              $"dynamicResolution={camera.allowDynamicResolution}; reasserted the owned requests."
            : scaleDrifted
                ? $"the scene drifted to scale={driftedScale:F4} msaa={driftedMsaa}; " +
                  $"reasserted scale={scale:F4} msaa=1."
                : $"the scene drifted to msaa={driftedMsaa}; reasserted msaa=1.");
        return true;
    }

    private void Capitalise() => Owned = _asset != null;

    private CameraState? Find(Camera camera)
    {
        foreach (var state in _cameras)
        {
            if (state.Camera == null) continue;
            if (state.Camera == camera) return state;
        }
        return null;
    }

    /// <summary>Writes the snapshot for one camera back. Safe on a destroyed camera.</summary>
    private void RestoreCamera(CameraState state)
    {
        if (!state.Written) return;
        if (state.Camera == null || state.Additional == null) return;
        state.Camera.allowDynamicResolution = state.AllowDynamicResolution;
        state.Additional.renderPostProcessing = state.RenderPostProcessing;
        state.Additional.requiresDepthTexture = state.RequiresDepthTexture;
        state.Additional.requiresColorTexture = state.RequiresColorTexture;
        state.Additional.antialiasing = state.Antialiasing;
        state.Written = false;
    }

    private void RestoreAsset()
    {
        if (_asset == null || !_asset.Written || _asset.Asset == null) return;
        if (_asset.ScaleWritten) _asset.Asset.renderScale = _asset.RenderScale;
        _asset.Asset.msaaSampleCount = _asset.MsaaSampleCount;
        _asset.Written = false;
        _asset.ScaleWritten = false;
    }

    /// <summary>Restores every snapshot and releases ownership. Does not reapply other mods' values.</summary>
    internal void Restore()
    {
        foreach (var state in _cameras) RestoreCamera(state);
        _cameras.Clear();
        RestoreAsset();
        _asset = null;
        Owned = false;
    }

    /// <summary>Forgets a camera that no longer exists so a replacement cannot reuse its snapshot.</summary>
    internal void Forget(Camera camera)
    {
        for (var i = _cameras.Count - 1; i >= 0; i--)
        {
            var state = _cameras[i];
            if (state.Camera != camera && state.Camera != null) continue;
            RestoreCamera(state);
            _cameras.RemoveAt(i);
        }
    }

    /// <summary>Drops camera snapshots that reference destroyed objects.</summary>
    internal void PruneDestroyed()
    {
        for (var i = _cameras.Count - 1; i >= 0; i--)
        {
            var state = _cameras[i];
            if (state.Camera != null && state.Additional != null) continue;
            _cameras.RemoveAt(i);
        }
    }
}
