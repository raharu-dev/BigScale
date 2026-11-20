using System;
using System.Reflection;
using HarmonyLib;
using UnityEngine.Rendering;
using UnityEngine.Rendering.RenderGraphModule;
using UnityEngine.Rendering.Universal;

namespace BigScale;

/// <summary>
/// Harmony patches that give frame generation its two URP inputs and request the depth/motion
/// guides XR super resolution reconstructs from.
///
/// <c>UniversalRenderer.OnRecordRenderGraph</c> marks the eligible primary camera's graph so depth and
/// motion can be requested and captured. <c>UniversalRenderer.GetRenderPassInputs</c> is asked for
/// depth and motion while that scope is active, which makes URP allocate them with super resolution
/// Off instead of forcing TAA or changing the render scale. Both declarations were verified against
/// the installed interop assembly.
/// </summary>
internal sealed class FrameGenerationCaptureHook
{
    private readonly FrameGenerationCoordinator _coordinator;
    private readonly Action<string> _log;
    private Harmony? _harmony;
    private MethodInfo? _renderGraph;
    private MethodInfo? _inputs;
    private MethodInfo? _renderOverlay;

    internal FrameGenerationCaptureHook(FrameGenerationCoordinator coordinator, Action<string> log)
    {
        _coordinator = coordinator;
        _log = log;
    }

    internal bool Installed { get; private set; }
    internal string Status { get; private set; } = "not installed";

    internal bool Install(Harmony harmony)
    {
        if (Installed) return true;
        _renderGraph = AccessTools.Method(typeof(UniversalRenderer), "OnRecordRenderGraph",
            new[] { typeof(RenderGraph), typeof(ScriptableRenderContext) });
        if (_renderGraph == null)
        {
            Status = "UniversalRenderer.OnRecordRenderGraph(RenderGraph, ScriptableRenderContext) is missing";
            _log(Status);
            return false;
        }
        _inputs = AccessTools.Method(typeof(UniversalRenderer), "GetRenderPassInputs");
        if (_inputs == null || _inputs.GetParameters().Length != 6)
        {
            Status = "UniversalRenderer.GetRenderPassInputs does not match the inspected build";
            _log(Status);
            return false;
        }
        var reference = typeof(TextureHandle).MakeByRefType();
        var overlaySignature = new[] { typeof(RenderGraph), typeof(ContextContainer), reference, reference };
        var overlay = AccessTools.Method(typeof(DrawScreenSpaceUIPass), "RenderOverlay", overlaySignature);
        // The name alone is not proof: this build could expose another overload (or the static
        // utility form) and a wrong target would leave frame generation permanently capture-less.
        var overlayReason = ValidateOverlay(overlay, overlaySignature);
        if (overlayReason != null)
        {
            Status = "DrawScreenSpaceUIPass.RenderOverlay(RenderGraph, ContextContainer, ref TextureHandle, " +
                     "ref TextureHandle) is unusable: " + overlayReason;
            _log(Status);
            return false;
        }
        _renderOverlay = overlay;
        try
        {
            _harmony = harmony;
            harmony.Patch(_renderGraph,
                prefix: new HarmonyMethod(typeof(FrameGenerationCaptureHook), nameof(RenderGraphPrefix)),
                postfix: new HarmonyMethod(typeof(FrameGenerationCaptureHook), nameof(RenderGraphPostfix)),
                finalizer: new HarmonyMethod(typeof(FrameGenerationCaptureHook), nameof(RenderGraphFinalizer)));
            harmony.Patch(_inputs,
                postfix: new HarmonyMethod(typeof(FrameGenerationCaptureHook), nameof(InputsPostfix)));
            harmony.Patch(overlay,
                prefix: new HarmonyMethod(typeof(FrameGenerationCaptureHook), nameof(OverlayPrefix)));
            Installed = true;
            Status = "frame-generation capture hooks installed";
            _log("frame-generation capture hooks installed; overlay prefix on the " +
                 (overlay.IsStatic ? "static" : "instance") + " overload " + Describe(overlay) +
                 "; render-graph prefix on " + Describe(_renderGraph) + ".");
            return true;
        }
        catch (Exception exception)
        {
            Status = "could not patch the URP frame-generation entry points: " + exception.Message;
            _log(Status);
            return false;
        }
    }

    /// <summary>
    /// Removes every patch this hook may have applied, including a partially installed set: the
    /// stored targets are the truth, never the <see cref="Installed"/> flag, so a failed install
    /// cannot leave a callback running.
    /// </summary>
    internal void Uninstall()
    {
        if (_harmony == null) return;
        try
        {
            if (_renderGraph != null)
            {
                _harmony.Unpatch(_renderGraph,
                    AccessTools.Method(typeof(FrameGenerationCaptureHook), nameof(RenderGraphPrefix)));
                _harmony.Unpatch(_renderGraph,
                    AccessTools.Method(typeof(FrameGenerationCaptureHook), nameof(RenderGraphPostfix)));
                _harmony.Unpatch(_renderGraph,
                    AccessTools.Method(typeof(FrameGenerationCaptureHook), nameof(RenderGraphFinalizer)));
            }
            if (_inputs != null)
                _harmony.Unpatch(_inputs,
                    AccessTools.Method(typeof(FrameGenerationCaptureHook), nameof(InputsPostfix)));
            if (_renderOverlay != null)
                _harmony.Unpatch(_renderOverlay,
                    AccessTools.Method(typeof(FrameGenerationCaptureHook), nameof(OverlayPrefix)));
        }
        catch
        {
            // Best effort during shutdown.
        }
        _harmony = null;
        Installed = false;
        Status = "uninstalled";
    }

    /// <summary>
    /// Proves the resolved method really is the overlay entry point this build calls, not merely a
    /// method of that name. A wrong target is a hard install failure: a silent capture-less frame
    /// generation is worse than an explicit "unavailable" line.
    /// </summary>
    private static string? ValidateOverlay(MethodInfo? method, Type[] expected)
    {
        if (method == null) return "the method does not exist in this build";
        if (method.DeclaringType != typeof(DrawScreenSpaceUIPass))
            return "it is declared by " + (method.DeclaringType?.FullName ?? "?");
        if (method.ReturnType != typeof(void)) return "it returns " + method.ReturnType.Name;
        var parameters = method.GetParameters();
        if (parameters.Length != expected.Length)
            return $"it takes {parameters.Length} parameters, not {expected.Length}";
        for (var i = 0; i < expected.Length; i++)
        {
            var actual = parameters[i].ParameterType;
            if (actual != expected[i])
                return $"parameter {i} is {actual.Name}, not {expected[i].Name}";
            // The two handles must be by-ref: the prefix replaces the caller's handle.
            if (i >= 2 && !actual.IsByRef) return $"parameter {i} is not passed by reference";
        }
        return null;
    }

    private static string Describe(MethodInfo? method)
    {
        if (method == null) return "<null>";
        var parameters = method.GetParameters();
        var names = new string[parameters.Length];
        for (var i = 0; i < parameters.Length; i++)
            names[i] = parameters[i].ParameterType.Name + " " + parameters[i].Name;
        return (method.DeclaringType?.Name ?? "?") + "." + method.Name + "(" + string.Join(", ", names) + ")";
    }

    private static void RenderGraphPrefix(UniversalRenderer __instance, ref int __state)
    {
        var plugin = Plugin.Instance;
        var coordinator = plugin?.Coordinator;
        if (coordinator == null) return;
        // 0 marks a prefix that never armed the guide request, so the finalizer can tell that from an
        // armed scope whose previous value was false. 1/2 carry that previous value.
        try
        {
            __state = coordinator.BeginXrReconstructionInputs(__instance) ? 2 : 1;
        }
        catch (Exception exception)
        {
            __state = 1;
            coordinator.ReportXrInputFailure(exception);
        }
        var frameGeneration = coordinator.FrameGeneration;
        try
        {
            frameGeneration.CaptureBegin(__instance);
        }
        catch (Exception exception)
        {
            frameGeneration.CaptureAbort();
            frameGeneration.ReportCaptureFailure("frame-generation capture prefix failed: " + exception);
        }
    }

    private static void RenderGraphPostfix(UniversalRenderer __instance, RenderGraph renderGraph)
    {
        var coordinator = Plugin.Instance?.Coordinator?.FrameGeneration;
        if (coordinator == null) return;
        try
        {
            coordinator.CaptureEnd(__instance, renderGraph);
        }
        catch (Exception exception)
        {
            coordinator.CaptureAbort();
            coordinator.ReportCaptureFailure("frame-generation capture postfix failed: " + exception);
        }
    }

    private static void RenderGraphFinalizer(int __state)
    {
        var coordinator = Plugin.Instance?.Coordinator;
        if (coordinator == null) return;
        coordinator.FrameGeneration.CaptureAbort();
        if (__state != 0) coordinator.EndXrReconstructionInputs(__state == 2);
    }

    /// <summary>
    /// Runs while the game records its SDR overlay pass. The colour handle is the final scene image
    /// without HUD; capturing it here is the only correct HUD-less source.
    /// </summary>
    private static void OverlayPrefix(RenderGraph renderGraph, ContextContainer frameData,
        ref TextureHandle colorBuffer)
    {
        var coordinator = Plugin.Instance?.Coordinator?.FrameGeneration;
        if (coordinator == null) return;
        try
        {
            coordinator.CaptureBeforeUi(renderGraph, frameData, colorBuffer);
        }
        catch (Exception exception)
        {
            coordinator.ReportCaptureFailure("pre-overlay frame-generation capture failed: " + exception);
        }
    }

    private static void InputsPostfix(ref UniversalRenderer.RenderPassInputSummary __result)
    {
        var coordinator = Plugin.Instance?.Coordinator;
        if (coordinator == null) return;
        // Super resolution owns its own request scope: frame generation being off or capturing no
        // graph in VR must never suppress the guides BigScale reconstructs from.
        try
        {
            coordinator.RequestXrReconstructionInputs(ref __result);
        }
        catch (Exception exception)
        {
            coordinator.ReportXrInputFailure(exception);
        }
        var frameGeneration = coordinator.FrameGeneration;
        try
        {
            frameGeneration.RequestCaptureInputs(ref __result);
        }
        catch (Exception exception)
        {
            frameGeneration.CaptureAbort();
            frameGeneration.ReportCaptureFailure("frame-generation input request failed: " + exception);
        }
    }
}
