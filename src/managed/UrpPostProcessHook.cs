using System;
using System.Reflection;
using HarmonyLib;
using UnityEngine.Rendering;
using UnityEngine.Rendering.RenderGraphModule;
using UnityEngine.Rendering.Universal;

namespace BigScale;

/// <summary>
/// The Harmony patches this mod installs into URP post-processing. The outer
/// <c>RenderPostProcessingRenderGraph</c> patch lets the coordinator decide per camera whether the
/// stock orchestration runs unchanged or is replaced by BigScale; the <c>RenderUberPost</c> patch lets an
/// XR camera keep that stock orchestration and substitute only the reconstructed HDR source and its
/// full-size destination.
/// </summary>
internal sealed class UrpPostProcessHook
{
    private readonly RenderCoordinator _coordinator;
    private readonly Action<string> _log;
    private Harmony? _harmony;
    private MethodInfo? _target;
    private MethodInfo? _uberPost;

    internal UrpPostProcessHook(RenderCoordinator coordinator, Action<string> log)
    {
        _coordinator = coordinator;
        _log = log;
    }

    internal bool Installed { get; private set; }
    internal string Status { get; private set; } = "not installed";

    /// <summary>Installs the patches, or reports exactly which signature is missing.</summary>
    internal bool Install(Harmony harmony)
    {
        if (Installed) return true;
        _target = AccessTools.Method(typeof(PostProcessPassRenderGraph), "RenderPostProcessingRenderGraph");
        if (_target == null)
        {
            Status = "PostProcessPassRenderGraph.RenderPostProcessingRenderGraph is missing";
            _log(Status);
            return false;
        }
        var outerReason = ValidateOuter(_target);
        if (outerReason != null)
        {
            Status = "RenderPostProcessingRenderGraph signature differs from the inspected build: " + outerReason;
            _log(Status);
            return false;
        }
        _uberPost = AccessTools.Method(typeof(PostProcessPassRenderGraph), "RenderUberPost");
        var uberReason = ValidateUberPost(_uberPost);
        if (uberReason != null)
        {
            Status = "PostProcessPassRenderGraph.RenderUberPost is unusable: " + uberReason;
            _log(Status);
            return false;
        }
        try
        {
            _harmony = harmony;
            harmony.Patch(_target,
                prefix: new HarmonyMethod(typeof(UrpPostProcessHook), nameof(Prefix)),
                postfix: new HarmonyMethod(typeof(UrpPostProcessHook), nameof(Postfix)),
                finalizer: new HarmonyMethod(typeof(UrpPostProcessHook), nameof(Finalizer)));
            harmony.Patch(_uberPost,
                postfix: new HarmonyMethod(typeof(UrpPostProcessHook), nameof(UberPostPostfix)));
            Installed = true;
            Status = "post-processing hooks installed";
            return true;
        }
        catch (Exception exception)
        {
            // A half-installed set is worse than none: the outer replacement without the UberPost
            // seam could publish text never reconstructed. Remove everything that did get applied.
            UnpatchAll();
            Status = "could not patch the URP post-processing entry points: " + exception.Message;
            _log(Status);
            return false;
        }
    }

    internal void Uninstall()
    {
        if (_harmony == null) return;
        UnpatchAll();
        _harmony = null;
        Installed = false;
        Status = "uninstalled";
    }

    /// <summary>Removes every patch this class may have applied, including a partial set.</summary>
    private void UnpatchAll()
    {
        if (_harmony == null) return;
        try
        {
            if (_target != null)
            {
                _harmony.Unpatch(_target,
                    AccessTools.Method(typeof(UrpPostProcessHook), nameof(Prefix)));
                _harmony.Unpatch(_target,
                    AccessTools.Method(typeof(UrpPostProcessHook), nameof(Postfix)));
                _harmony.Unpatch(_target,
                    AccessTools.Method(typeof(UrpPostProcessHook), nameof(Finalizer)));
            }
            if (_uberPost != null)
            {
                _harmony.Unpatch(_uberPost,
                    AccessTools.Method(typeof(UrpPostProcessHook), nameof(UberPostPostfix)));
            }
        }
        catch
        {
            // Best effort during shutdown and after a failed install.
        }
    }

    /// <summary>Checks the installed 9-parameter outer orchestration against this build.</summary>
    private static string? ValidateOuter(MethodInfo? method)
    {
        if (method == null) return "the method does not exist in this build";
        if (method.DeclaringType != typeof(PostProcessPassRenderGraph))
            return "it is declared by " + (method.DeclaringType?.FullName ?? "?");
        if (method.ReturnType != typeof(void)) return "it returns " + method.ReturnType.Name;
        if (method.IsStatic) return "it is static";
        var parameters = method.GetParameters();
        if (parameters.Length != 9 || parameters[0].ParameterType != typeof(RenderGraph) ||
            parameters[1].ParameterType != typeof(ContextContainer) ||
            parameters[2].ParameterType != typeof(TextureHandle).MakeByRefType() ||
            parameters[5].ParameterType != typeof(TextureHandle).MakeByRefType() ||
            parameters[6].ParameterType != typeof(bool))
            return $"it takes {parameters.Length} parameters that do not match the inspected vector";
        return null;
    }

    /// <summary>
    /// Checks the installed 11-parameter UberPost seam. The two handle arguments are bound
    /// positionally by the patch declarations; the rest are identity checks so a renamed or
    /// reordered build can never be substituted through.
    /// </summary>
    private static string? ValidateUberPost(MethodInfo? method)
    {
        if (method == null) return "the method does not exist in this build";
        if (method.DeclaringType != typeof(PostProcessPassRenderGraph))
            return "it is declared by " + (method.DeclaringType?.FullName ?? "?");
        if (method.ReturnType != typeof(void)) return "it returns " + method.ReturnType.Name;
        if (method.IsStatic) return "it is static";
        var parameters = method.GetParameters();
        if (parameters.Length != 11) return $"it takes {parameters.Length} parameters, not 11";
        var expected = new[]
        {
            typeof(RenderGraph), typeof(ContextContainer), typeof(UniversalCameraData),
            typeof(UniversalPostProcessingData),
            typeof(TextureHandle).MakeByRefType(), typeof(TextureHandle).MakeByRefType(),
            typeof(TextureHandle).MakeByRefType(), typeof(TextureHandle).MakeByRefType(),
            typeof(TextureHandle).MakeByRefType(),
            typeof(bool), typeof(bool),
        };
        for (var i = 0; i < expected.Length; i++)
        {
            var actual = parameters[i].ParameterType;
            if (actual != expected[i]) return $"parameter {i} is {actual.Name}, not {expected[i].Name}";
        }
        return null;
    }

    private static bool Prefix(PostProcessPassRenderGraph __instance, RenderGraph renderGraph,
        ContextContainer frameData, ref TextureHandle activeCameraColorTexture, ref TextureHandle lutTexture,
        ref TextureHandle overlayUITexture, ref TextureHandle postProcessingTarget, bool hasFinalPass,
        bool resolveToDebugScreen, bool enableColorEndingIfNeeded, ref int __state)
    {
        var instance = Plugin.Instance;
        var coordinator = instance?.Coordinator;
        if (coordinator == null) return true;
        try
        {
            var ownsScope = coordinator.OnPostProcess(__instance, renderGraph, frameData,
                ref activeCameraColorTexture, ref lutTexture, ref overlayUITexture, ref postProcessingTarget,
                hasFinalPass, resolveToDebugScreen, enableColorEndingIfNeeded, out var scopeToken);
            __state = scopeToken;
            return ownsScope;
        }
        catch (Exception exception)
        {
            __state = 0;
            instance!.LogError("post-processing prefix failed; running the stock path: " + exception);
            return true;
        }
    }

    /// <summary>
    /// Runs after the original orchestration returned normally. For an owned XR scope this publishes
    /// the full-size post-processed destination and performs the one frame-generation capture.
    /// </summary>
    private static void Postfix(PostProcessPassRenderGraph __instance, RenderGraph renderGraph,
        ContextContainer frameData, ref TextureHandle postProcessingTarget, int __state)
    {
        var coordinator = Plugin.Instance?.Coordinator;
        if (coordinator == null) return;
        try
        {
            coordinator.OnPostProcessReturned(__instance, renderGraph, frameData, ref postProcessingTarget,
                __state);
        }
        catch (Exception exception)
        {
            coordinator.ReportXrPostFailure(exception);
        }
    }

    /// <summary>Clears the per-invocation XR scope on every path, including a thrown original.</summary>
    private static void Finalizer(int __state)
    {
        var coordinator = Plugin.Instance?.Coordinator;
        if (coordinator == null) return;
        try
        {
            coordinator.OnPostProcessExited(__state);
        }
        catch
        {
            // The scope is discarded next frame even if cleanup itself fails.
        }
    }

    private static void UberPostPostfix(PostProcessPassRenderGraph __instance, RenderGraph __0,
        ContextContainer __1, UniversalCameraData __2)
    {
        var coordinator = Plugin.Instance?.Coordinator;
        if (coordinator == null) return;
        try
        {
            coordinator.OnXrUberPostReturned(__instance, __0, __1, __2);
        }
        catch (Exception exception)
        {
            coordinator.ReportXrPostFailure(exception);
        }
    }
}
