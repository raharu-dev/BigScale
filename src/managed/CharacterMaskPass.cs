using System;
using BigScale.Core;
using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;
using UnityEngine.Rendering.RenderGraphModule;
using UnityEngine.Rendering.Universal;
using PassData = UnityEngine.Rendering.Universal.PostProcessPassRenderGraph.DoFGaussianPassData;

namespace BigScale;

/// <summary>
/// Records the character coverage mask into the active URP render graph.
///
/// The mask is a single-channel, output-sized target whose clear value is black, meaning "not a
/// character". It is built in two stages so it covers exactly what the game's own shaders draw:
/// first every character submesh supplied by <c>CharacterMaskSource</c> is drawn with its real
/// material and forward pass, with the camera's view and projection, into a scratch colour target
/// and a private depth target, both cleared. Then a fullscreen triangle at far depth is drawn white
/// into the mask with <c>ZTest NotEqual</c> against that depth, so a pixel turns white exactly where a
/// character fragment wrote depth. Geometry the game hides in its shaders (clip, no depth write)
/// never enters the mask. The native neural-rendering stage blends its model answer over every
/// other pixel and leaves the protected characters untouched.
///
/// The pass reuses URP's <c>DoFGaussianPassData</c> rather than a private pass-data type: IL2CPP can
/// only marshal a generic render function for a type the runtime already instantiated, and a fresh
/// managed nested type is not one of them. The mask handle, material and a pooled payload token ride
/// in that type; the callback resolves the payload, binds the graph texture with an explicit
/// <c>SetRenderTarget</c> and draws. The unsafe callback path takes <c>UnsafeGraphContext</c>, which
/// marshals correctly (a raster pass's <c>RasterGraphContext</c> is a non-blittable struct that the
/// delegate bridge rejects before the plugin can load).
///
/// One reusable callback is built in the constructor and no delegate, closure or renderer array is
/// created on the render thread. View and projection are copied into the payload at record time.
/// A record that cannot produce a mask returns <see cref="TextureHandle.nullHandle"/> and never
/// leaves a half-recorded pass behind, so the caller simply runs the neural stage unmasked.
internal sealed class CharacterMaskPass
{
    private const string PassName = "BigScale.CharacterMask";
    private const string TextureName = "BigScale.CharacterMask";
    private const string DepthTextureName = "BigScale.CharacterMaskDepth";
    private const string ScratchTextureName = "BigScale.CharacterMaskScratch";
    private const int PayloadCount = 8;

    /// <summary>
    /// One recorded mask draw. The draw list is owned by <c>CharacterMaskSource</c>; only a reference
    /// is stored. The token lets a delayed graph reject a recycled payload.
    /// </summary>
    private sealed class Payload
    {
        internal int Token;
        internal CharacterMaskDraw[] Draws = Array.Empty<CharacterMaskDraw>();
        internal int CullingMask;
        internal Material? Material;
        internal Mesh? FillMesh;
        internal Matrix4x4 WorldToCamera;
        internal Matrix4x4 Projection;
    }

    // Built once: an IL2CPP delegate conversion per frame would both allocate and put a marshalling
    // failure on the render thread.
    private readonly BaseRenderFunc<PassData, UnsafeGraphContext> _callback;
    private readonly Payload[] _payloads = new Payload[PayloadCount];
    private readonly Action<string>? _log;
    // One message per failure kind. The mask is requested every frame while masking is enabled, so a
    // repeated failure must never serialise a string per frame.
    private bool _loggedMissingCamera;
    private bool _loggedRecordingFailure;
    private bool _loggedExecutionFailure;
    private int _cursor;
    // Last logged draw count and culling mask; the line is built only when either changes.
    private int _lastDrawn = -1;
    private int _lastCullingMask;
    private int _nextToken;

    /// <param name="log">Diagnostics sink; null keeps the pass silent.</param>
    internal CharacterMaskPass(Action<string>? log = null)
    {
        _log = log;
        for (var i = 0; i < _payloads.Length; i++) _payloads[i] = new Payload();
        _callback = new Action<PassData, UnsafeGraphContext>(Execute);
    }

    /// <summary>
    /// Records the mask at the requested extent. Returns <see cref="TextureHandle.nullHandle"/> when
    /// there is nothing to draw or the graph refused the pass.
    /// </summary>
    /// <param name="width">Mask width; the caller's output extent is authoritative.</param>
    /// <param name="height">Mask height; the caller's output extent is authoritative.</param>
    internal TextureHandle Record(RenderGraph graph, UniversalCameraData cameraData,
        CharacterMaskDraw[] draws, Material fillMaterial, Mesh fillMesh, int width, int height)
    {
        if (graph == null || draws == null || draws.Length == 0 || fillMaterial == null || fillMesh == null ||
            width <= 0 || height <= 0)
        {
            return TextureHandle.nullHandle;
        }

        // The matrices must come from the camera the frame was rendered with; without it the mask
        // cannot be placed and the neural stage has to run with the unmodified image.
        var camera = cameraData?.camera;
        if (camera == null)
        {
            if (!_loggedMissingCamera)
            {
                _loggedMissingCamera = true;
                _log?.Invoke("the character mask was requested without a camera; masking is skipped.");
            }
            return TextureHandle.nullHandle;
        }

        try
        {
            var description = new TextureDesc(width, height)
            {
                format = GraphicsFormat.R8_UNorm,
                clearBuffer = true,
                name = TextureName,
                filterMode = FilterMode.Bilinear,
                wrapMode = TextureWrapMode.Clamp,
                useMipMap = false,
                enableRandomWrite = false,
                // No depth attachment. The texture is a colour target.
                depthBufferBits = DepthBits.None,
            };
            var mask = graph.CreateTexture(ref description);
            var depthDescription = new TextureDesc(width, height)
            {
                format = GraphicsFormat.D32_SFloat,
                clearBuffer = false,
                name = DepthTextureName,
                filterMode = FilterMode.Point,
                wrapMode = TextureWrapMode.Clamp,
                useMipMap = false,
                enableRandomWrite = false,
            };
            var depth = graph.CreateTexture(ref depthDescription);
            var scratchDescription = new TextureDesc(width, height)
            {
                format = GraphicsFormat.R8_UNorm,
                clearBuffer = false,
                name = ScratchTextureName,
                filterMode = FilterMode.Bilinear,
                wrapMode = TextureWrapMode.Clamp,
                useMipMap = false,
                enableRandomWrite = false,
                depthBufferBits = DepthBits.None,
            };
            var scratch = graph.CreateTexture(ref scratchDescription);

            var payload = _payloads[_cursor];
            _cursor = (_cursor + 1) % _payloads.Length;
            payload.Token = ++_nextToken;
            payload.Draws = draws;
            payload.CullingMask = camera.cullingMask;
            payload.Material = fillMaterial;
            payload.FillMesh = fillMesh;
            payload.WorldToCamera = camera.worldToCameraMatrix;
            payload.Projection = camera.projectionMatrix;

            var builder = graph.AddUnsafePass<PassData>(PassName, out var data);
            try
            {
                // Every pooled field is initialised; an unused handle must be invalid, never a
                // leftover from another pass.
                data.sourceTexture = mask;
                data.depthTexture = depth;
                data.halfCoCTexture = TextureHandle.nullHandle;
                data.fullCoCTexture = TextureHandle.nullHandle;
                data.pingTexture = scratch;
                data.pongTexture = TextureHandle.nullHandle;
                data.destination = TextureHandle.nullHandle;
                data.material = fillMaterial;
                data.materialCoC = null;
                data.cocParams = Vector3.zero;
                data.highQualitySamplingValue = false;
                data.downsample = payload.Token;

                var common = builder.Cast<IBaseRenderGraphBuilder>();
                // The callback writes the textures through explicit SetRenderTarget calls, so the
                // graph only needs the write declarations for ordering and allocation.
                common.UseTexture(ref mask, AccessFlags.Write);
                common.UseTexture(ref depth, AccessFlags.Write);
                common.UseTexture(ref scratch, AccessFlags.Write);
                common.AllowPassCulling(false);
                // The draw overrides the camera matrices, which is global state.
                common.AllowGlobalStateModification(true);
                builder.SetRenderFunc(_callback);
                return mask;
            }
            finally
            {
                builder.Cast<Il2CppSystem.IDisposable>().Dispose();
            }
        }
        catch (Exception exception)
        {
            if (!_loggedRecordingFailure)
            {
                _loggedRecordingFailure = true;
                _log?.Invoke("recording the character mask failed: " + exception);
            }
            return TextureHandle.nullHandle;
        }
    }

    private void Execute(PassData data, UnsafeGraphContext context)
    {
        try
        {
            Payload? payload = null;
            for (var i = 0; i < _payloads.Length; i++)
            {
                if (_payloads[i].Token != data.downsample) continue;
                payload = _payloads[i];
                break;
            }
            var mask = UpscalingPass.ResolveTexture(data.sourceTexture);
            var depth = UpscalingPass.ResolveTexture(data.depthTexture);
            var scratch = UpscalingPass.ResolveTexture(data.pingTexture);
            if (payload == null || mask == null || depth == null || scratch == null ||
                payload.Material == null || payload.FillMesh == null)
            {
                if (!_loggedExecutionFailure)
                {
                    _loggedExecutionFailure = true;
                    _log?.Invoke("the character mask could not be resolved at execution; masking is skipped.");
                }
                return;
            }
            var commands = CommandBufferHelpers.GetNativeCommandBuffer(context.cmd);
            var depthTarget = new RenderTargetIdentifier(depth);

            // Stage 1: the game's own forward passes write character depth. The colour they write to
            // the scratch target is discarded.
            commands.SetRenderTarget(new RenderTargetIdentifier(scratch), depthTarget);
            commands.ClearRenderTarget(true, true, Color.clear);
            commands.SetViewProjectionMatrices(payload.WorldToCamera, payload.Projection);
            var draws = payload.Draws;
            var drawn = 0;
            for (var i = 0; i < draws.Length; i++)
            {
                var draw = draws[i];
                var renderer = draw.Renderer;
                var owner = draw.Owner;
                if (renderer == null || owner == null || draw.Material == null) continue;
                // Live state: visibility and layers can change between the source's rescans.
                if (!CharacterMaskPolicy.IsDrawnByCamera(payload.CullingMask, owner.layer, renderer.enabled,
                        owner.activeInHierarchy, renderer.forceRenderingOff,
                        renderer.shadowCastingMode == ShadowCastingMode.ShadowsOnly))
                {
                    continue;
                }
                commands.DrawRenderer(renderer, draw.Material, draw.Submesh, draw.Pass);
                drawn++;
            }

            // Stage 2: white wherever stage 1 left depth. Black is "not a character"; the native
            // composite blends the model answer everywhere the mask stays black.
            commands.SetRenderTarget(new RenderTargetIdentifier(mask), depthTarget);
            commands.ClearRenderTarget(false, true, Color.black);
            if (drawn > 0)
            {
                commands.SetViewProjectionMatrices(Matrix4x4.identity, Matrix4x4.identity);
                commands.DrawMesh(payload.FillMesh, Matrix4x4.identity, payload.Material, 0, 0);
                commands.SetViewProjectionMatrices(payload.WorldToCamera, payload.Projection);
            }
            if (drawn != _lastDrawn || payload.CullingMask != _lastCullingMask)
            {
                _lastDrawn = drawn;
                _lastCullingMask = payload.CullingMask;
                _log?.Invoke($"character mask: drawing {drawn}/{draws.Length} draw(s), camera culling mask 0x{payload.CullingMask:X8}");
            }
        }
        catch (Exception exception)
        {
            if (_loggedExecutionFailure) return;
            _loggedExecutionFailure = true;
            _log?.Invoke("drawing the character mask failed: " + exception);
        }
    }
}
