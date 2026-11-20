using System;
using System.Reflection;
using Il2CppInterop.Runtime;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.RenderGraphModule;
using UnityEngine.Rendering.Universal;

namespace BigScale;

/// <summary>
/// Reproduces URP's full <c>RenderPostProcessingRenderGraph</c> orchestration with the temporal
/// upscale stage replaced by BigScale. Effect ordering and the UberPost descriptor handling follow the
/// installed URP method surface; every effect is executed through URP's own helpers rather than
/// reimplemented.
///
/// Helpers whose parameter is a non-blittable managed struct (<c>TextureDesc</c> is boxed by
/// IL2CPP) are not called through the generated wrapper at all: bloom uses a cached method-info and
/// an unboxed payload call, and the lens-flare paths that would need the same treatment are refused
/// with an explicit compatibility reason instead of risking a bad call.
/// </summary>
internal sealed class StockPostProcessing
{
    /// <summary>Injected by Plugin once; used only for diagnostics.</summary>
    internal Action<string>? Log;

    private static readonly int FullscreenProjectionId = Shader.PropertyToID("_FullscreenProjMat");
    private static IntPtr _bloomSetupMethod;
    private static bool _bloomMethodResolved;
    private static string? _bloomMethodError;
    private readonly System.Collections.Generic.HashSet<int> _loggedCameras = new();

    /// <summary>Instance entry point so callers can report through this pass's logger.</summary>
    internal bool EnsureHelpers(out string? error)
    {
        var ready = ResolveHelpers(out error);
        if (!ready) Log?.Invoke("URP post-process helper unavailable: " + (error ?? "unknown"));
        return ready;
    }

    /// <summary>
    /// Resolves the installed boxed-value workaround target exactly once. Missing is a hard
    /// compatibility failure, never a reason to drop bloom silently.
    /// </summary>
    internal static bool ResolveHelpers(out string? error)
    {
        if (!_bloomMethodResolved)
        {
            _bloomMethodResolved = true;
            try
            {
                var field = typeof(PostProcessPassRenderGraph).GetField(
                    "NativeMethodInfoPtr_UberPostSetupBloomPass_Public_Void_RenderGraph_Material_byref_TextureDesc_0",
                    BindingFlags.NonPublic | BindingFlags.Static);
                if (field == null)
                {
                    _bloomMethodError = "UberPostSetupBloomPass method-info field is missing";
                }
                else
                {
                    var value = field.GetValue(null);
                    _bloomSetupMethod = value is IntPtr pointer ? pointer : IntPtr.Zero;
                    if (_bloomSetupMethod == IntPtr.Zero) _bloomMethodError = "UberPostSetupBloomPass method-info is null";
                }
            }
            catch (Exception exception)
            {
                _bloomMethodError = "UberPostSetupBloomPass lookup failed: " + exception.Message;
            }
        }
        error = _bloomMethodError;
        return error == null;
    }

    /// <summary>
    /// Preflights the stock effect set for this camera and pass. Every failure is a reason to keep
    /// the game's own renderer, never a reason to record a partially replaced orchestration.
    /// </summary>
    internal bool Prepare(PostProcessPassRenderGraph pass, ContextContainer frameData, UniversalCameraData cameraData,
        bool hasFinalPass, bool encodeOutput, bool hasLut, out string? reason)
    {
        reason = null;
        var stack = VolumeManager.instance.stack;
        pass.m_DepthOfField = stack.GetComponent<DepthOfField>();
        pass.m_MotionBlur = stack.GetComponent<MotionBlur>();
        pass.m_PaniniProjection = stack.GetComponent<PaniniProjection>();
        pass.m_Bloom = stack.GetComponent<Bloom>();
        pass.m_LensFlareScreenSpace = stack.GetComponent<ScreenSpaceLensFlare>();
        pass.m_LensDistortion = stack.GetComponent<LensDistortion>();
        pass.m_ChromaticAberration = stack.GetComponent<ChromaticAberration>();
        pass.m_Vignette = stack.GetComponent<Vignette>();
        pass.m_ColorLookup = stack.GetComponent<ColorLookup>();
        pass.m_ColorAdjustments = stack.GetComponent<ColorAdjustments>();
        pass.m_Tonemapping = stack.GetComponent<Tonemapping>();
        pass.m_FilmGrain = stack.GetComponent<FilmGrain>();

        var postProcessingData = frameData.Get<UniversalPostProcessingData>();
        pass.m_UseFastSRGBLinearConversion = postProcessingData.useFastSRGBLinearConversion;
        pass.m_SupportDataDrivenLensFlare = postProcessingData.supportDataDrivenLensFlare;
        pass.m_SupportScreenSpaceLensFlare = postProcessingData.supportScreenSpaceLensFlare;
        pass.m_HasFinalPass = hasFinalPass;
        pass.m_EnableColorEncodingIfNeeded = encodeOutput;

        var cameraId = cameraData.camera.GetInstanceID();
        if (_loggedCameras.Add(cameraId))
            Log?.Invoke($"stock effects camera={cameraData.camera.name} id={cameraId} bloom={pass.m_Bloom.IsActive()} " +
                        $"tone={pass.m_Tonemapping.mode.value} postExposure={pass.m_ColorAdjustments.postExposure.value:F3} " +
                        $"dof={pass.m_DepthOfField.IsActive()} motionBlur={pass.m_MotionBlur.IsActive()} " +
                        $"panini={pass.m_PaniniProjection.IsActive()} vignette={pass.m_Vignette.IsActive()} " +
                        $"grain={pass.m_FilmGrain.IsActive()} lut={hasLut} uber={(pass.m_Materials.uber != null)} " +
                        $"xr={cameraData.xr.enabled}");

        if (pass.m_Materials.uber == null)
        {
            reason = "URP post-process material library is incomplete (uber shader missing)";
            return false;
        }

        // Effects that need a helper whose parameter is a boxed managed struct. Calling those
        // through the generated wrapper is what the recorded crash path did, so they stay stock.
        if (pass.m_LensFlareScreenSpace.IsActive() && pass.m_SupportScreenSpaceLensFlare)
        {
            reason = "screen-space lens flares are enabled; their URP helper takes a boxed TextureDesc and is " +
                     "not verified for this build";
            return false;
        }
        var lensFlaresActive = !LensFlareCommonSRP.Instance.IsEmpty() && pass.m_SupportDataDrivenLensFlare;
        if (lensFlaresActive)
        {
            reason = "data-driven lens flares are active; their URP helper takes a boxed TextureDesc and is not " +
                     "verified for this build";
            return false;
        }
        if ((pass.m_Bloom.IsActive() || pass.m_LensFlareScreenSpace.IsActive()) && !ResolveHelpers(out var helperError))
        {
            reason = helperError ?? "the bloom helper is unavailable";
            return false;
        }
        if (hasFinalPass || cameraData.isStopNaNEnabled)
        {
            if (cameraData.isStopNaNEnabled && pass.m_Materials.stopNaN == null)
            {
                reason = "URP stopNaN material is missing";
                return false;
            }
        }
        var dofMaterial = pass.m_DepthOfField.mode.value == DepthOfFieldMode.Gaussian
            ? pass.m_Materials.gaussianDepthOfField
            : pass.m_Materials.bokehDepthOfField;
        if (pass.m_DepthOfField.IsActive() && !cameraData.isSceneViewCamera && dofMaterial == null)
        {
            reason = "URP depth-of-field material is missing";
            return false;
        }
        if (pass.m_MotionBlur.IsActive() && pass.m_Materials.motionBlur == null && !cameraData.isSceneViewCamera)
        {
            reason = "URP motion-blur material is missing";
            return false;
        }
        // A missing LUT handle is legal: URP's RenderUberPost tolerates an invalid lookup texture.
        if (!hasLut) Log?.Invoke("no internal colour LUT handle this frame; grading uses the neutral path.");
        return true;
    }

    /// <summary>Effects URP runs before the temporal upscale stage.</summary>
    internal void BeforeReconstruction(PostProcessPassRenderGraph pass, RenderGraph renderGraph,
        UniversalResourceData resourceData, UniversalCameraData cameraData, ref TextureHandle color)
    {
        if (cameraData.isStopNaNEnabled && pass.m_Materials.stopNaN != null)
        {
            pass.RenderStopNaN(renderGraph, ref color, out var stopNaNTarget);
            color = stopNaNTarget;
        }

        if (cameraData.antialiasing == AntialiasingMode.SubpixelMorphologicalAntiAliasing)
        {
            pass.RenderSMAA(renderGraph, resourceData, cameraData.antialiasingQuality, ref color, out var smaaTarget);
            color = smaaTarget;
        }

        var dofMaterial = pass.m_DepthOfField.mode.value == DepthOfFieldMode.Gaussian
            ? pass.m_Materials.gaussianDepthOfField
            : pass.m_Materials.bokehDepthOfField;
        if (pass.m_DepthOfField.IsActive() && !cameraData.isSceneViewCamera && dofMaterial != null)
        {
            pass.RenderDoF(renderGraph, resourceData, cameraData, ref color, out var dofTarget);
            color = dofTarget;
        }
    }

    /// <summary>Effects URP runs after the temporal upscale stage, including the UberPost pass.</summary>
    internal void AfterReconstruction(PostProcessPassRenderGraph pass, RenderGraph renderGraph,
        ContextContainer frameData, UniversalResourceData resourceData, UniversalCameraData cameraData,
        bool resolveToDebugScreen, ref TextureHandle color, ref TextureHandle postProcessingTarget,
        ref TextureHandle lutTexture, ref TextureHandle overlayUITexture)
    {
        var postProcessingData = frameData.Get<UniversalPostProcessingData>();
        var isSceneViewCamera = cameraData.isSceneViewCamera;

        var useMotionBlur = pass.m_MotionBlur.IsActive() && !isSceneViewCamera && Application.isPlaying;
        if (useMotionBlur && pass.m_MotionBlur.mode.value == MotionBlurMode.CameraAndObjects)
            useMotionBlur = ScriptableRenderer.current != null && ScriptableRenderer.current.SupportsMotionVectors();
        if (useMotionBlur)
        {
            pass.RenderMotionBlur(renderGraph, resourceData, cameraData, ref color, out var motionBlurTarget);
            color = motionBlurTarget;
        }

        if (pass.m_PaniniProjection.IsActive() && !isSceneViewCamera)
        {
            pass.RenderPaniniProjection(renderGraph, cameraData.camera, ref color, out var paniniTarget);
            color = paniniTarget;
        }

        // UberPost
        var uber = pass.m_Materials.uber;
        uber.shaderKeywords = null;

        var sourceDescriptor = color.GetDescriptor(renderGraph);

        var bloomTexture = TextureHandle.nullHandle;
        if (pass.m_Bloom.IsActive())
        {
            pass.RenderBloomTexture(renderGraph, ref color, out bloomTexture, cameraData.isAlphaOutputEnabled);
            SetupBloomUnboxed(pass, renderGraph, uber, sourceDescriptor);
        }

        pass.SetupLensDistortion(uber, isSceneViewCamera);
        pass.SetupChromaticAberration(uber);
        pass.SetupVignette(uber, cameraData.xr, sourceDescriptor.width, sourceDescriptor.height);
        pass.SetupGrain(cameraData, uber);
        pass.SetupDithering(cameraData, uber);

        if (pass.RequireSRGBConversionBlitToBackBuffer(cameraData.requireSrgbConversion))
            CoreUtils.SetKeyword(uber, ShaderKeywordStrings.LinearToSRGBConversion, true);
        if (pass.m_UseFastSRGBLinearConversion)
            CoreUtils.SetKeyword(uber, ShaderKeywordStrings.UseFastSRGBLinearConversion, true);

        var requireHdrOutput = PostProcessPassRenderGraph.RequireHDROutput(cameraData);
        if (requireHdrOutput)
        {
            var operation = !pass.m_HasFinalPass && pass.m_EnableColorEncodingIfNeeded
                ? HDROutputUtils.Operation.ColorEncoding
                : HDROutputUtils.Operation.None;
            pass.SetupHDROutput(cameraData.hdrDisplayInformation, cameraData.hdrDisplayColorGamut, uber, operation,
                cameraData.rendersOverlayUI);
            var pixelRect = cameraData.pixelRect;
            RenderingUtils.SetupOffscreenUIViewportParams(uber, ref pixelRect, resourceData.isActiveTargetBackBuffer);
        }

        var enableAlphaOutput = cameraData.isAlphaOutputEnabled;

        var debugHandler = ScriptableRenderPass.GetActiveDebugHandler(cameraData);
        debugHandler?.UpdateShaderGlobalPropertiesForFinalValidationPass(renderGraph, cameraData,
            !pass.m_HasFinalPass && !resolveToDebugScreen);

        pass.RenderUberPost(renderGraph, frameData, cameraData, postProcessingData, ref color,
            ref postProcessingTarget, ref lutTexture, ref bloomTexture, ref overlayUITexture, requireHdrOutput,
            enableAlphaOutput);
    }

    /// <summary>
    /// URP's generated by-ref <c>TextureDesc</c> wrapper hands native code the boxed object header
    /// instead of the value payload, so the unmodified native method is invoked with the unboxed
    /// payload address.
    /// </summary>
    private static unsafe void SetupBloomUnboxed(PostProcessPassRenderGraph pass, RenderGraph renderGraph,
        Material uber, TextureDesc descriptor)
    {
        if (!ResolveHelpers(out var error)) throw new InvalidOperationException(error);
        IntPtr* arguments = stackalloc IntPtr[3];
        arguments[0] = renderGraph.Pointer;
        arguments[1] = uber.Pointer;
        arguments[2] = IL2CPP.il2cpp_object_unbox(descriptor.Pointer);
        var exception = IntPtr.Zero;
        IL2CPP.il2cpp_runtime_invoke(_bloomSetupMethod, pass.Pointer, (void**)arguments, ref exception);
        Il2CppException.RaiseExceptionIfNecessary(exception);
    }

    /// <summary>Records the fixed-function projection global URP sets before its post-processing passes.</summary>
    internal static void RecordProjectionSetup(RenderGraph renderGraph)
    {
        var builder = renderGraph.AddUnsafePass<PostProcessPassRenderGraph.DoFGaussianPassData>(
            "BigScale.PostFXSetup", out _);
        try
        {
            var common = builder.Cast<IBaseRenderGraphBuilder>();
            common.AllowPassCulling(false);
            common.AllowGlobalStateModification(true);
            BaseRenderFunc<PostProcessPassRenderGraph.DoFGaussianPassData, UnsafeGraphContext> callback =
                PostFxSetupCallback.Instance;
            builder.SetRenderFunc(callback);
        }
        finally
        {
            builder.Cast<Il2CppSystem.IDisposable>().Dispose();
        }
    }

    /// <summary>Single reusable render func for the projection setup pass.</summary>
    private sealed class PostFxSetupCallback
    {
        internal static readonly BaseRenderFunc<PostProcessPassRenderGraph.DoFGaussianPassData, UnsafeGraphContext> Instance =
            new Action<PostProcessPassRenderGraph.DoFGaussianPassData, UnsafeGraphContext>(Execute);

        private static void Execute(PostProcessPassRenderGraph.DoFGaussianPassData data, UnsafeGraphContext context)
        {
            var commandBuffer = CommandBufferHelpers.GetNativeCommandBuffer(context.cmd);
            commandBuffer.SetGlobalMatrix(FullscreenProjectionId, GL.GetGPUProjectionMatrix(Matrix4x4.identity, true));
        }
    }
}
