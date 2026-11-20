using System;
using System.Linq;
using System.Reflection;
using HarmonyLib;
using UnityEngine.Rendering;
using UnityEngine.Rendering.RenderGraphModule;
using UnityEngine.Rendering.Universal;

namespace BigScale;

/// <summary>
/// Diagnostics-only observation of the stage this mod never replaces: URP's own transfer of the
/// post-processed image into the camera target, and the operands the uber post actually received.
/// The post-processing prefix can replace every effect pass, so a frozen or empty presented image has
/// to be attributed to one hop; these postfixes log those hops and change nothing.
/// </summary>
internal sealed class UrpFinalPassHook
{
    private readonly RenderCoordinator _coordinator;
    private readonly Action<string> _log;
    private Harmony? _harmony;
    private readonly System.Collections.Generic.List<MethodInfo> _patched = new();

    /// <summary>The by-ref shape every observed handle transport uses.</summary>
    private static readonly Type RefTexture = typeof(TextureHandle).MakeByRefType();

    internal UrpFinalPassHook(RenderCoordinator coordinator, Action<string> log)
    {
        _coordinator = coordinator;
        _log = log;
    }

    internal bool Installed { get; private set; }
    internal string Status { get; private set; } = "not installed";

    /// <summary>Installs the observations, or reports which method is missing. Never throws.</summary>
    internal bool Install(Harmony harmony)
    {
        if (Installed) return true;
        _harmony = harmony;

        var postPass = typeof(PostProcessPassRenderGraph);
        var uberPost = AccessTools.Method(postPass, "RenderUberPost");
        var postExit = AccessTools.Method(postPass, "RenderPostProcessingRenderGraph");
        var finalPassGraph = AccessTools.Method(postPass, "RenderFinalPassRenderGraph");
        var finalBlitType = postPass.Assembly.GetType("UnityEngine.Rendering.Universal.Internal.FinalBlitPass");
        var finalBlit = finalBlitType == null ? null : AccessTools.Method(finalBlitType, "Render");
        if (uberPost == null || postExit == null || finalPassGraph == null || finalBlit == null)
        {
            Status = "URP transfer methods are missing (UberPost=" + (uberPost != null) +
                     " PostExit=" + (postExit != null) + " FinalPassGraph=" + (finalPassGraph != null) +
                     " FinalBlit=" + (finalBlit != null) + ")";
            _log(Status);
            return false;
        }

        // Bind by exact metadata, never by a guessed overload: a postfix whose positional arguments do
        // not match the target fails to compile and would silently leave the trace incomplete.
        if (!Matches(uberPost, typeof(RenderGraph), typeof(ContextContainer), typeof(UniversalCameraData),
                typeof(UniversalPostProcessingData), RefTexture, RefTexture, RefTexture, RefTexture, RefTexture,
                typeof(bool), typeof(bool)))
            return Reject("RenderUberPost");
        if (!Matches(postExit, typeof(RenderGraph), typeof(ContextContainer), RefTexture, RefTexture,
                RefTexture, RefTexture, typeof(bool), typeof(bool), typeof(bool)))
            return Reject("RenderPostProcessingRenderGraph");
        if (!Matches(finalPassGraph, typeof(RenderGraph), typeof(ContextContainer), RefTexture, RefTexture,
                RefTexture, typeof(bool)))
            return Reject("RenderFinalPassRenderGraph");
        // This build appends a trailing flag to the documented transfer; source and destination keep
        // indices 3 and 4 in every shape, so one postfix serves both.
        if (!Matches(finalBlit, typeof(RenderGraph), typeof(ContextContainer), typeof(UniversalCameraData),
                RefTexture, RefTexture, typeof(TextureHandle), typeof(bool)) &&
            !Matches(finalBlit, typeof(RenderGraph), typeof(ContextContainer), typeof(UniversalCameraData),
                RefTexture, RefTexture, typeof(TextureHandle)))
            return Reject("FinalBlitPass.Render");

        try
        {
            Patch(harmony, uberPost, nameof(AfterUberPost));
            Patch(harmony, postExit, nameof(AfterPostProcessing));
            Patch(harmony, finalPassGraph, nameof(AfterFinalPassGraph));
            Patch(harmony, finalBlit, nameof(AfterFinalBlit));
            Installed = true;
            Status = "URP transfer observation installed";
            _log(Status);
            return true;
        }
        catch (Exception exception)
        {
            var failure = "could not observe the URP transfer: " + exception.Message;
            // Release whatever the earlier patches already installed before reporting the failure, so an
            // incomplete observation is never mistaken for a complete trace.
            Uninstall();
            Status = failure;
            _log(Status);
            return false;
        }
    }

    /// <summary>Reports a metadata shape this build cannot bind; observation stays disabled.</summary>
    private bool Reject(string methodName)
    {
        Status = "URP transfer signature mismatch: " + methodName;
        _log(Status);
        return false;
    }

    /// <summary>Checks one instance method against an exact parameter vector.</summary>
    private static bool Matches(MethodInfo? method, params Type[] parameters)
    {
        if (method == null) return false;
        if (method.IsStatic || method.ReturnType != typeof(void)) return false;
        return method.GetParameters().Select(parameter => parameter.ParameterType).SequenceEqual(parameters);
    }

    private void Patch(Harmony harmony, MethodInfo target, string postfix)
    {
        harmony.Patch(target, postfix: new HarmonyMethod(typeof(UrpFinalPassHook), postfix));
        _patched.Add(target);
    }

    internal void Uninstall()
    {
        if (_harmony == null) return;
        try
        {
            foreach (var target in _patched)
            {
                foreach (var name in new[]
                         {
                             nameof(AfterUberPost), nameof(AfterPostProcessing), nameof(AfterFinalPassGraph),
                             nameof(AfterFinalBlit)
                         })
                {
                    var postfix = AccessTools.Method(typeof(UrpFinalPassHook), name);
                    if (postfix != null) _harmony.Unpatch(target, postfix);
                }
            }
        }
        catch
        {
            // Best effort during shutdown.
        }
        _patched.Clear();
        Installed = false;
        Status = "uninstalled";
    }

    // Positional names bind by index: the shipped uber post names its handles sourceTexture/destTexture,
    // so a postfix that declared those names would fail to compile against them.
    private static void AfterUberPost(RenderGraph __0, ContextContainer __1, ref TextureHandle __4,
        ref TextureHandle __5)
    {
        var coordinator = Plugin.Instance?.Coordinator;
        if (coordinator == null) return;
        try
        {
            coordinator.OnUrpUberPost(__0, __1, __4, __5);
        }
        catch (Exception exception)
        {
            Plugin.Instance!.LogError("uber post observation failed: " + exception.Message);
        }
    }

    private static void AfterPostProcessing(RenderGraph __0, ContextContainer __1)
    {
        var coordinator = Plugin.Instance?.Coordinator;
        if (coordinator == null) return;
        try
        {
            coordinator.OnUrpPostExit(__0, __1);
        }
        catch (Exception exception)
        {
            Plugin.Instance!.LogError("post exit observation failed: " + exception.Message);
        }
    }

    /// <summary>
    /// URP's graph-based final transfer: (renderGraph, frameData, source, overlayUI, target, encoding).
    /// Source and target keep indices 2 and 4 in this build.
    /// </summary>
    private static void AfterFinalPassGraph(RenderGraph __0, ContextContainer __1, ref TextureHandle __2,
        ref TextureHandle __4)
    {
        var coordinator = Plugin.Instance?.Coordinator;
        if (coordinator == null) return;
        try
        {
            coordinator.OnUrpFinalPostGraph(__0, __1, __2, __4);
        }
        catch (Exception exception)
        {
            Plugin.Instance!.LogError("final pass observation failed: " + exception.Message);
        }
    }

    /// <summary>URP's own transfer: (renderGraph, frameData, cameraData, source, dest, overlayUI, ...).</summary>
    private static void AfterFinalBlit(RenderGraph __0, ContextContainer __1, ref TextureHandle __3,
        ref TextureHandle __4)
    {
        var coordinator = Plugin.Instance?.Coordinator;
        if (coordinator == null) return;
        try
        {
            coordinator.OnUrpFinalBlit(__0, __1, __3, __4);
        }
        catch (Exception exception)
        {
            Plugin.Instance!.LogError("final blit observation failed: " + exception.Message);
        }
    }
}
