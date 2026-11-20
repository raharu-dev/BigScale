using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;
using UnityEngine.SceneManagement;

namespace BigScale;

/// <summary>
/// Runs this mod's per-camera preparation at the earliest legitimate point: before URP creates the
/// camera's frame data, so a committed scene scale is already visible to
/// <c>InitializeAdditionalCameraData</c> and to the graph the camera records.
///
/// Three entry points cover every shape of camera render, and they are all idempotent:
/// <list type="bullet">
/// <item><c>RenderPipelineManager.beginCameraRendering</c>, the pipeline's own per-camera event;</item>
/// <item>a prefix on <c>UniversalRenderPipeline.RenderCameraStack</c>, which is what the game
/// actually calls for gameplay cameras and runs before the begin-camera scope;</item>
/// <item>a prefix on the standalone <c>RenderSingleCamera(ScriptableRenderContext, Camera)</c>
/// overload, which the camera stack path does not use.</item>
/// </list>
///
/// Inside an XR session the Big Walk VR pose callback runs first; the optional postfix re-applies
/// the owned requests afterwards so the VR mod's own scale and pose corrections keep their ordering.
/// </summary>
internal sealed class RenderingCallbacks
{
    private readonly RenderCoordinator _coordinator;
    private readonly Action<string> _log;
    private bool _installed;
    private bool _vrPatchAttempted;
    private string _jitterState = "unavailable";
    private string _jitterReason = "the VR eye-matrix hook was never installed";
    private bool _eventSubscribed;
    private Action<ScriptableRenderContext, Camera>? _beginHandler;
    private bool _endEventSubscribed;
    private Action<ScriptableRenderContext, Camera>? _endHandler;
    private bool _sceneSubscribed;
    private Action<Scene>? _sceneHandler;

    internal RenderingCallbacks(RenderCoordinator coordinator, Action<string> log)
    {
        _coordinator = coordinator;
        _log = log;
    }

    internal bool Installed => _installed;

    /// <summary>Installs every entry point that exists in this build.</summary>
    internal bool Install(Harmony harmony)
    {
        if (_installed) return true;
        var installed = 0;
        installed += Patch(harmony, typeof(UniversalRenderPipeline), "RenderCameraStack",
            nameof(BeforeCameraStack), new[] { typeof(ScriptableRenderContext), typeof(Camera), typeof(bool) });
        installed += Patch(harmony, typeof(UniversalRenderPipeline), "RenderSingleCamera",
            nameof(BeforeRenderCamera), new[] { typeof(ScriptableRenderContext), typeof(Camera) });
        PatchXrExtent(harmony);

        try
        {
            _beginHandler = OnBeginCameraRendering;
            RenderPipelineManager.beginCameraRendering += _beginHandler;
            _eventSubscribed = true;
            installed++;
        }
        catch (Exception exception)
        {
            _log("could not subscribe to RenderPipelineManager.beginCameraRendering (continuing with the " +
                 "pipeline prefixes): " + exception.Message);
            _beginHandler = null;
        }

        try
        {
            _endHandler = OnEndCameraRendering;
            RenderPipelineManager.endCameraRendering += _endHandler;
            _endEventSubscribed = true;
            installed++;
        }
        catch (Exception exception)
        {
            _log("could not subscribe to RenderPipelineManager.endCameraRendering (continuing with the " +
                 "pipeline prefixes): " + exception.Message);
            _endHandler = null;
        }

        try
        {
            _sceneHandler = OnSceneUnloaded;
            SceneManager.sceneUnloaded += _sceneHandler;
            _sceneSubscribed = true;
        }
        catch (Exception exception)
        {
            _log("could not subscribe to SceneManager.sceneUnloaded: " + exception.Message);
            _sceneHandler = null;
        }
        _installed = installed > 0;
        if (!_installed)
            _log("no per-camera preparation entry point was available; the scene scale cannot be committed " +
                 "before URP creates its camera data.");
        TryInstallVrOrdering(harmony);
        return _installed;
    }

    /// <summary>
    /// Optional postfix on the VR mod's pose callback. Applied only when that exact method exists; a
    /// different VR build simply keeps the pipeline ordering. The eye-matrix hook is installed from
    /// here because it is the only place that can reach the same per-camera ordering, and an
    /// unavailable hook is reported to the coordinator so XR never takes reduced-resolution ownership
    /// without a reconstructed jitter history.
    /// </summary>
    private void TryInstallVrOrdering(Harmony harmony)
    {
        if (_vrPatchAttempted) return;
        _vrPatchAttempted = true;
        try
        {
            var type = ResolveVrType();
            if (type == null)
            {
                Unavailable("BigWalkVR.VrRenderer is not in a loaded assembly");
                _log("BigWalkVR.VrRenderer not found in the loaded BigWalkVR assembly; using the pipeline ordering.");
                return;
            }
            var method = AccessTools.Method(type, "UpdateCameraBeforeRender");
            if (method == null)
            {
                Unavailable("BigWalkVR.VrRenderer.UpdateCameraBeforeRender is missing");
                _log("BigWalkVR.VrRenderer.UpdateCameraBeforeRender not found; using the pipeline ordering.");
                return;
            }
            // The postfix binds positionally, so a different parameter list must never be patched:
            // a Camera bound to the context struct is the crash this guard exists to prevent.
            var parameters = method.GetParameters();
            if (parameters.Length != 2 ||
                parameters[0].ParameterType != typeof(ScriptableRenderContext) ||
                parameters[1].ParameterType != typeof(Camera))
            {
                var observed = "";
                foreach (var parameter in parameters)
                {
                    if (observed.Length > 0) observed += ", ";
                    observed += parameter.ParameterType.FullName;
                }
                Unavailable("UpdateCameraBeforeRender has an unexpected signature: " + observed);
                _log($"BigWalkVR.VrRenderer.UpdateCameraBeforeRender has an unexpected signature ({observed}); " +
                     "using the pipeline ordering.");
                return;
            }
            harmony.Patch(method, postfix: new HarmonyMethod(typeof(RenderingCallbacks), nameof(AfterVrCamera)));
            _log("VR pose callback ordering preserved: BigScale requests are applied after VrRenderer.UpdateCameraBeforeRender.");
            InstallVrJitter(harmony);
        }
        catch (Exception exception)
        {
            Unavailable("the VR pose callback could not be patched: " + exception.Message);
            _log("could not patch the VR pose callback (continuing without it): " + exception.Message);
        }
    }

    /// <summary>Records one exact reason why XR must not claim a jittered reconstruction.</summary>
    private void Unavailable(string reason)
    {
        _coordinator.MarkVrJitterHook(false, reason);
        _jitterState = "unavailable";
        _jitterReason = reason;
    }

    /// <summary>
    /// The state the coordinator needs to know before it may own a reduced XR render: whether the
    /// eye-matrix hook installed, and the exact reason when it did not.
    /// </summary>
    internal bool VrJitterInstalled => _jitterState == "installed";

    internal string VrJitterReason => _jitterState == "installed" ? "" : _jitterReason;

    /// <summary>
    /// Target of the transpiler: a Harmony method that reads and writes no game state. It receives
    /// the VR mod's own freshly built GPU projection and returns the same projection with this frame's
    /// sub-pixel sample applied, so every matrix the mod derives from it is consistent.
    /// </summary>
    internal static Matrix4x4 ApplyXrProjection(Matrix4x4 gpuProjection, int eye, Camera camera)
    {
        try
        {
            var coordinator = Plugin.Instance?.Coordinator;
            return coordinator == null ? gpuProjection : coordinator.ApplyXrProjection(gpuProjection, eye, camera);
        }
        catch (Exception exception)
        {
            Plugin.Instance?.LogError("VR jitter injection failed: " + exception.Message);
            return gpuProjection;
        }
    }

    /// <summary>
    /// Matches the one <c>GL.GetGPUProjectionMatrix</c> call in <c>ReadEye</c> and inserts the sample
    /// between it and every use of its result. Patching at the source is what keeps the remap, the
    /// packed view-projection and its inverse describing one projection; rewriting the stored blocks
    /// afterwards left the inverse unjittered and let a second read discard a jittered forward matrix.
    /// </summary>
    private static IEnumerable<CodeInstruction> InjectXrProjection(IEnumerable<CodeInstruction> instructions)
    {
        var list = new List<CodeInstruction>(instructions);
        var hits = 0;
        var at = -1;
        for (var i = 0; i < list.Count; i++)
        {
            // Matched by declared type and name rather than a resolved MethodInfo: the interop assembly
            // that owns the call is not necessarily the Unity reference this plugin compiled against.
            if (list[i].operand is not MethodInfo called) continue;
            if (called.Name != "GetGPUProjectionMatrix") continue;
            if (called.DeclaringType?.FullName != "UnityEngine.GL") continue;
            hits++;
            at = i;
        }
        if (hits != 1)
            throw new InvalidOperationException($"ReadEye contains {hits} GPU projection calls, expected exactly one");
        var apply = AccessTools.Method(typeof(RenderingCallbacks), nameof(ApplyXrProjection));
        list.Insert(at + 1, new CodeInstruction(OpCodes.Ldarg_1));
        list.Insert(at + 2, new CodeInstruction(OpCodes.Ldarg_2));
        list.Insert(at + 3, new CodeInstruction(OpCodes.Call, apply));
        return list;
    }

    /// <summary>
    /// Postfix on the VR mod's per-eye matrix builder. It no longer changes anything: the projection
    /// itself already carried the sample before the mod derived its matrices. It only feeds the
    /// verification invariant and never throws into the game's render loop.
    /// </summary>
    private static void AfterReadEyeVerify(int __0, Camera __1, bool __result, float[] ___eyeMatrices)
    {
        if (!__result) return;
        try
        {
            var coordinator = Plugin.Instance?.Coordinator;
            coordinator?.VerifyXrEyeMatrices(__0, __1, ___eyeMatrices);
        }
        catch (Exception exception)
        {
            Plugin.Instance?.LogError("XR projection verification failed: " + exception.Message);
        }
    }

    private void InstallVrJitter(Harmony harmony)
    {
        try
        {
            Type? type = null;
            foreach (var assembly in AppDomain.CurrentDomain.GetAssemblies())
            {
                if (!string.Equals(assembly.GetName().Name, "BigWalkVR", StringComparison.Ordinal)) continue;
                try
                {
                    type = assembly.GetType("BigWalkVR.VrStereoShaderState", false);
                }
                catch
                {
                    type = null;
                }
                break;
            }
            if (type == null)
            {
                Unavailable("BigWalkVR.VrStereoShaderState is missing");
                _log("VR jitter injection unavailable (BigWalkVR.VrStereoShaderState not found); VR super " +
                     "resolution stays off rather than running without a jittered history.");
                return;
            }
            var method = AccessTools.Method(type, "ReadEye");
            if (method == null)
            {
                Unavailable("VrStereoShaderState.ReadEye is missing");
                _log("VR jitter injection unavailable (VrStereoShaderState.ReadEye not found); VR super " +
                     "resolution stays off rather than running without a jittered history.");
                return;
            }
            var expected = new[]
            {
                typeof(int), typeof(Camera), typeof(Matrix4x4).MakeByRefType(), typeof(Vector3).MakeByRefType(),
            };
            var parameters = method.GetParameters();
            var matches = parameters.Length == expected.Length;
            for (var i = 0; matches && i < expected.Length; i++)
                matches = parameters[i].ParameterType == expected[i];
            matches &= method.ReturnType == typeof(bool);
            if (!matches)
            {
                var observed = "";
                foreach (var parameter in parameters)
                {
                    if (observed.Length > 0) observed += ", ";
                    observed += parameter.ParameterType.FullName;
                }
                Unavailable("ReadEye has an unexpected signature: " + observed);
                _log($"VR jitter injection unavailable (VrStereoShaderState.ReadEye has an unexpected signature ({observed})); " +
                     "VR super resolution stays off rather than running without a jittered history.");
                return;
            }
            harmony.Patch(method,
                transpiler: new HarmonyMethod(typeof(RenderingCallbacks), nameof(InjectXrProjection)),
                postfix: new HarmonyMethod(typeof(RenderingCallbacks), nameof(AfterReadEyeVerify)));
            _coordinator.MarkVrJitterHook(true, "");
            _jitterState = "installed";
            _log("VR jitter injection installed on BigWalkVR.VrStereoShaderState.ReadEye (source projection).");
        }
        catch (Exception exception)
        {
            Unavailable(exception.Message);
            _log("VR jitter injection unavailable (" + exception.Message + "); VR super resolution stays " +
                 "off rather than running without a jittered history.");
        }
    }

    /// <summary>
    /// Postfix on URP's XR camera-data pass. In an XR session <c>cameraTargetDescriptor</c> is the
    /// per-eye intermediate extent, so this is the only place a reduced render size can be declared
    /// without touching <c>asset.renderScale</c>, which URP forwards to the XR runtime and would
    /// resize the SteamVR eye textures themselves.
    /// </summary>
    private void PatchXrExtent(Harmony harmony)
    {
        try
        {
            var target = AccessTools.Method(typeof(UniversalRenderPipeline), "UpdateCameraData");
            if (target == null)
            {
                _log("XR render extent hook unavailable: UniversalRenderPipeline.UpdateCameraData is missing " +
                     "in this build; super resolution stays off in VR.");
                return;
            }
            var parameters = target.GetParameters();
            if (parameters.Length != 2 || parameters[0].ParameterType != typeof(UniversalCameraData))
            {
                _log($"XR render extent hook unavailable: UpdateCameraData has {parameters.Length} parameter(s) " +
                     "and an unexpected first parameter; super resolution stays off in VR.");
                return;
            }
            harmony.Patch(target,
                postfix: new HarmonyMethod(typeof(RenderingCallbacks), nameof(AfterUpdateCameraData)));
            _coordinator.MarkXrExtentHook(true);
            _log("XR render extent hook installed on UniversalRenderPipeline.UpdateCameraData.");
        }
        catch (Exception exception)
        {
            _log("XR render extent hook unavailable: " + exception.Message +
                 "; super resolution stays off in VR.");
        }
    }

    private static void AfterUpdateCameraData(UniversalCameraData __0)
    {
        var coordinator = Plugin.Instance?.Coordinator;
        if (coordinator == null) return;
        try
        {
            coordinator.OnXrCameraData(__0);
        }
        catch (Exception exception)
        {
            Plugin.Instance!.LogError("XR render extent handler failed: " + exception.Message);
        }
    }

    /// <summary>
    /// Resolves the VR type from its own assembly instead of scanning every interop assembly, which
    /// is slow and can surface unrelated type-load failures.
    /// </summary>
    private static Type? ResolveVrType()
    {
        foreach (var assembly in AppDomain.CurrentDomain.GetAssemblies())
        {
            if (!string.Equals(assembly.GetName().Name, "BigWalkVR", StringComparison.Ordinal)) continue;
            try
            {
                return assembly.GetType("BigWalkVR.VrRenderer", false);
            }
            catch
            {
                return null;
            }
        }
        return null;
    }

    private void OnBeginCameraRendering(ScriptableRenderContext context, Camera camera) =>
        Prepare(camera, "beginCameraRendering");

    private void OnEndCameraRendering(ScriptableRenderContext context, Camera camera) =>
        _coordinator.OnEndCameraRendering(context, camera);

    private void OnSceneUnloaded(Scene scene)
    {
        try
        {
            _coordinator.OnPrimarySceneUnloaded(scene.handle);
        }
        catch (Exception exception)
        {
            _log("scene-unloaded handling failed: " + exception.Message);
        }
    }

    private static void BeforeCameraStack(ScriptableRenderContext __0, Camera __1, bool __2)
    {
        var callbacks = Plugin.Instance?.Callbacks;
        if (callbacks == null) return;
        try
        {
            // The installed original names this parameter baseCamera, so the patch binds positionally.
            callbacks.Prepare(__1, "URP camera stack");
        }
        catch (Exception exception)
        {
            Plugin.Instance!.LogError("per-camera preparation failed: " + exception.Message);
        }
    }

    private static void BeforeRenderCamera(ScriptableRenderContext __0, Camera __1)
    {
        var callbacks = Plugin.Instance?.Callbacks;
        if (callbacks == null) return;
        try
        {
            callbacks.Prepare(__1, "URP standalone camera entry");
        }
        catch (Exception exception)
        {
            Plugin.Instance!.LogError("per-camera preparation failed: " + exception.Message);
        }
    }

    /// <summary>
    /// Runs after the VR mod's own pose callback. Parameter zero is the
    /// <see cref="ScriptableRenderContext"/> struct, never the camera: binding a camera to it makes
    /// IL2CPP treat a value type as a managed object reference and faults inside the GC handle lookup.
    /// </summary>
    private static void AfterVrCamera(ScriptableRenderContext __0, Camera __1)
    {
        var callbacks = Plugin.Instance?.Callbacks;
        if (callbacks == null) return;
        try
        {
            callbacks.Prepare(__1, "after VrRenderer.UpdateCameraBeforeRender");
            Plugin.Instance!.Coordinator?.OnVrCameraRendered(__0, __1);
        }
        catch (Exception exception)
        {
            Plugin.Instance!.LogError("VR ordering handler failed: " + exception.Message);
        }
    }

    /// <summary>
    /// Hands the camera to the coordinator, which decides whether anything may be written. This
    /// method never mutates a camera or pipeline value itself.
    /// </summary>
    internal void Prepare(Camera? camera, string source)
    {
        if (camera == null) return;
        _coordinator.OnCameraBoundary(camera, source);
    }

    internal void Uninstall(Harmony harmony)
    {
        if (!_installed) return;
        try
        {
            if (_eventSubscribed && _beginHandler != null)
            {
                RenderPipelineManager.beginCameraRendering -= _beginHandler;
                _eventSubscribed = false;
            }
            if (_endEventSubscribed && _endHandler != null)
            {
                RenderPipelineManager.endCameraRendering -= _endHandler;
                _endEventSubscribed = false;
            }
            if (_sceneSubscribed && _sceneHandler != null)
            {
                SceneManager.sceneUnloaded -= _sceneHandler;
                _sceneSubscribed = false;
            }
        }
        catch
        {
            // Best effort during shutdown.
        }
        try
        {
            var stack = AccessTools.Method(typeof(UniversalRenderPipeline), "RenderCameraStack",
                new[] { typeof(ScriptableRenderContext), typeof(Camera), typeof(bool) });
            if (stack != null)
                harmony.Unpatch(stack, AccessTools.Method(typeof(RenderingCallbacks), nameof(BeforeCameraStack)));
            var entry = AccessTools.Method(typeof(UniversalRenderPipeline), "RenderSingleCamera",
                new[] { typeof(ScriptableRenderContext), typeof(Camera) });
            if (entry != null)
                harmony.Unpatch(entry, AccessTools.Method(typeof(RenderingCallbacks), nameof(BeforeRenderCamera)));
        }
        catch
        {
            // Best effort during shutdown.
        }
        _installed = false;
    }

    private int Patch(Harmony harmony, Type type, string method, string prefix, Type[] signature)
    {
        try
        {
            var target = AccessTools.Method(type, method, signature);
            if (target == null)
            {
                _log($"{type.Name}.{method}: not found in this build; another per-camera entry point covers it.");
                return 0;
            }
            harmony.Patch(target, prefix: new HarmonyMethod(typeof(RenderingCallbacks), prefix));
            return 1;
        }
        catch (Exception exception)
        {
            _log($"{type.Name}.{method}: could not be patched ({exception.GetType().Name}); another per-camera " +
                 "entry point covers it.");
            return 0;
        }
    }
}
