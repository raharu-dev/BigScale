using System;
using System.Reflection;
using BepInEx.Logging;
using HarmonyLib;
using Il2CppInterop.Runtime;
using UnityEngine;
using UnityEngine.Rendering;

namespace BigScale;

/// <summary>
/// Optional interception of Big Visuals so its global render-scale, upscaler and AA writes do not
/// fight the owned temporal path. Nothing is patched unless the installed signatures match, and
/// the mod's own settings file is never modified.
/// </summary>
internal sealed class BigVisualsCompatibility
{
    internal const string AssemblyName = "Big_Visuals";
    private const string SettingsTypeName = "Big_Visuals.Settings";

    private static readonly ManualLogSource Log = BepInEx.Logging.Logger.CreateLogSource("BigScale.BigVisuals");

    /// <summary>True while BigScale owns scene scale, so Big Visuals' global writes must be deferred.</summary>
    internal static bool DeferScaleWrites;

    /// <summary>Camera whose AA writes are suppressed; other cameras keep their stock behaviour.</summary>
    internal static Camera? OwnedCamera;

    private Harmony? _harmony;
    private bool _resolutionFailed;
    private int _attempts;
    private int _nextAttemptFrame;
    // A fourth of a second at 120 Hz: fast enough to catch a late load, cheap enough to be invisible.
    private const int ScanIntervalFrames = 30;
    private Type? _settingsType;
    private MethodInfo? _setResolutionScale;
    private MethodInfo? _setUpscaler;
    private MethodInfo? _applyPostProcessAa;
    private MethodInfo? _setAllCameraSettings;

    internal bool Installed { get; private set; }

    /// <summary>True once interception either installed or proved unavailable. Not retried after that.</summary>
    internal bool IsReady => Installed || _resolutionFailed;
    private string _status = "Big Visuals not detected";

    /// <summary>Resolves and patches Big Visuals. Safe to call repeatedly until it succeeds.</summary>
    internal bool Initialize(Harmony harmony)
    {
        if (Installed) return true;
        // The settings type is resolved from every loaded assembly, which is far too costly to repeat
        // every frame while the game is still loading. Retry on a cadence, bounded by the attempt cap.
        if (_settingsType == null && Time.frameCount < _nextAttemptFrame) return false;
        _nextAttemptFrame = Time.frameCount + ScanIntervalFrames;
        _settingsType ??= ResolveSettingsType();
        if (_settingsType == null)
        {
            // Big Visuals may simply not have loaded yet; keep retrying for a bounded number of frames.
            if (++_attempts > 600)
            {
                _resolutionFailed = true;
                _status = "Big Visuals not detected";
            }
            return false;
        }

        _setResolutionScale = _settingsType.GetMethod("SetResolutionScale", BindingFlags.Public | BindingFlags.Instance);
        _setUpscaler = _settingsType.GetMethod("SetUpscaler", BindingFlags.Public | BindingFlags.Instance);
        _applyPostProcessAa = _settingsType.GetMethod("ApplyPostProcessAA", BindingFlags.Public | BindingFlags.Instance,
            null, new[] { typeof(Camera), typeof(string) }, null);
        _setAllCameraSettings = _settingsType.GetMethod("SetAllCameraSettings", BindingFlags.Public | BindingFlags.Instance,
            null, Type.EmptyTypes, null);

        Log.LogInfo($"Big Visuals.Settings signatures: SetResolutionScale={Describe(_setResolutionScale)} " +
                    $"SetUpscaler={Describe(_setUpscaler)} ApplyPostProcessAA(Camera,string)={Describe(_applyPostProcessAa)}");

        var patched = 0;
        try
        {
            _harmony = harmony;
            if (_setResolutionScale != null)
            {
                harmony.Patch(_setResolutionScale, prefix: new HarmonyMethod(
                    typeof(BigVisualsCompatibility), nameof(PrefixSetResolutionScale)));
                patched++;
            }
            if (_setUpscaler != null)
            {
                harmony.Patch(_setUpscaler, prefix: new HarmonyMethod(
                    typeof(BigVisualsCompatibility), nameof(PrefixSetUpscaler)));
                patched++;
            }
            if (_applyPostProcessAa != null)
            {
                harmony.Patch(_applyPostProcessAa, prefix: new HarmonyMethod(
                    typeof(BigVisualsCompatibility), nameof(PrefixApplyPostProcessAa)));
                patched++;
            }
        }
        catch (Exception exception)
        {
            _status = "Big Visuals compatibility interception unavailable: " + exception.Message;
            Log.LogWarning(_status);
            DeferScaleWrites = false;
            return false;
        }

        Installed = patched > 0;
        _status = Installed
            ? $"Big Visuals compatibility active ({patched} method(s) intercepted); its shadows, LOD, anisotropy and FOV remain untouched."
            : "Big Visuals detected but its graphics setters differ in this build; BigScale keeps its own requests at the camera boundary.";
        Log.LogInfo(_status);
        return Installed;
    }

    /// <summary>Reapplies Big Visuals' current configuration after the owned values were restored.</summary>
    internal void Reapply()
    {
        if (_settingsType == null) return;
        DeferScaleWrites = false;
        OwnedCamera = null;
        try
        {
            var instance = GetInstance();
            if (instance == null) return;
            _setResolutionScale?.Invoke(instance, null);
            _setUpscaler?.Invoke(instance, null);
            _setAllCameraSettings?.Invoke(instance, null);
            Log.LogInfo("Big Visuals values reapplied after BigScale released rendering.");
        }
        catch (Exception exception)
        {
            Log.LogWarning("Could not reapply Big Visuals values: " + exception.Message);
        }
    }

    private static object? _instance;
    private static bool _instanceResolved;

    /// <summary>
    /// Big Visuals' Settings members are static in the installed build, but the resolvers probe both
    /// shapes so a static-only API still works.
    /// </summary>
    private object? GetInstance()
    {
        if (_instanceResolved) return _instance;
        _instanceResolved = true;
        var flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static;
        // The installed build exposes the instance as a static property on the plugin type, so
        // search every type in the assembly for a static holder of Settings.
        foreach (var type in _settingsType!.Assembly.GetTypes())
        {
            foreach (var field in type.GetFields(flags))
            {
                if (field.FieldType != _settingsType) continue;
                _instance = field.GetValue(null);
                if (_instance != null) return _instance;
            }
            foreach (var property in type.GetProperties(flags))
            {
                if (property.PropertyType != _settingsType || property.GetIndexParameters().Length != 0) continue;
                _instance = property.GetValue(null);
                if (_instance != null) return _instance;
            }
        }
        return _instance;
    }

    private static string Describe(MethodInfo? method) =>
        method == null ? "missing" : method.IsStatic ? "static" : "instance";

    private static Type? ResolveSettingsType()
    {
        foreach (var assembly in AppDomain.CurrentDomain.GetAssemblies())
        {
            if (assembly.GetName().Name != AssemblyName) continue;
            try
            {
                return assembly.GetType(SettingsTypeName, false);
            }
            catch
            {
                return null;
            }
        }
        return null;
    }

    private static bool PrefixSetResolutionScale() => !DeferScaleWrites;

    private static bool PrefixSetUpscaler() => !DeferScaleWrites;

    private static bool PrefixApplyPostProcessAa(Camera camera)
    {
        if (camera == null || OwnedCamera == null) return true;
        return camera.GetInstanceID() != OwnedCamera.GetInstanceID();
    }

    /// <summary>Unpatches only this mod's own Harmony patches.</summary>
    internal void Dispose()
    {
        try
        {
            if (_harmony == null) return;
            if (_setResolutionScale != null) _harmony.Unpatch(_setResolutionScale,
                AccessTools.Method(typeof(BigVisualsCompatibility), nameof(PrefixSetResolutionScale)));
            if (_setUpscaler != null) _harmony.Unpatch(_setUpscaler,
                AccessTools.Method(typeof(BigVisualsCompatibility), nameof(PrefixSetUpscaler)));
            if (_applyPostProcessAa != null) _harmony.Unpatch(_applyPostProcessAa,
                AccessTools.Method(typeof(BigVisualsCompatibility), nameof(PrefixApplyPostProcessAa)));
        }
        catch
        {
            // Unpatching is best effort during shutdown.
        }
        Installed = false;
        DeferScaleWrites = false;
        OwnedCamera = null;
    }
}
