using System;
using System.Collections;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using BepInEx;
using BepInEx.Logging;
using BepInEx.Unity.IL2CPP;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;
using BigScale.Core;

namespace BigScale;

/// <summary>
/// BigScale Super Resolution. Runs on desktop and inside the existing Big Walk VR session; the
/// VR mod keeps its pose, stereo and compositor ownership.
/// </summary>
[BepInPlugin(PluginGuid, PluginName, PluginVersion)]
[BepInDependency(BigVisualsGuid, BepInDependency.DependencyFlags.SoftDependency)]
public sealed class Plugin : BasePlugin
{
    public const string PluginGuid = "raharu.bigscale";
    public const string PluginName = "BigScale";
    public const string PluginVersion = "1.0.0";
    private const string BigVisualsGuid = "Big_Visuals";

    /// <summary>
    /// Other temporal renderers that must not own the graph at the same time. The last entry is the
    /// pre-rename BigWalkDLSS plugin: it must never run beside this one.
    /// </summary>
    private static readonly string[] ConflictingGuids =
    {
        "local.bigwalk.native-dlss",
        "local.bigwalk.dlss",
    };

    internal static Plugin? Instance;
    internal new ManualLogSource Log = null!;
    internal ModSettings Settings = null!;
    internal RenderCoordinator? Coordinator { get; private set; }
    internal BigVisualsCompatibility Compatibility = null!;
    internal RenderingCallbacks? Callbacks { get; private set; }
    internal TabStatusHud? Hud { get; private set; }
    internal KeyMenu Menu = null!;
    internal Harmony Harmony = null!;

    private UrpPostProcessHook? _hook;
    private UrpFinalPassHook? _finalPassHook;
    private FrameGenerationCaptureHook? _fgHook;
    private FrameGenerationPlayerLoop? _fgLoop;
    private IntPtr _nativeLibrary;
    private bool _shutdown;
    private bool _settingsHookReported;

    public override void Load()
    {
        Instance = this;
        Log = base.Log;
        Config.SaveOnConfigSet = true;

        Settings = new ModSettings();
        Settings.Bind(Config, LogInfo);
        ApplyNativeDiagnostics(Settings);
        Settings.Changed += OnSettingsChanged;
        Compatibility = new BigVisualsCompatibility();
        Menu = new KeyMenu(Settings);

        RegisterInjectedComponents();

        Harmony = new Harmony(PluginGuid);
        var settingsHooks = SettingsPanel.Patches.Install(Harmony, ReportSettingsHooks);
        Log.LogInfo($"settings panel hooks installed: {settingsHooks}/10.");

        // The hold-TAB rows are installed before any renderer-support early return: they must stay
        // usable (or silent) when BigScale is unsupported, faulted or still loading.
        Hud = new TabStatusHud(ReportHudHookFailure);
        TabStatusHud.Instance = Hud;
        var hudHooks = TabStatusHud.Patches.Install(Harmony, ReportHudHookFailure);
        Log.LogInfo($"hold-TAB HUD hooks installed: {hudHooks}/2.");

        AddComponent<PluginBehaviour>();

        if (FindConflict() is { } conflict)
        {
            Log.LogError($"Another upscaling renderer is active ('{conflict}'). Only one temporal renderer may own " +
                         "the post-processing graph, so the master switch stays disabled and both provider " +
                         "selections are preserved.");
            Settings.Enabled.Value = false;
            return;
        }

        var support = ValidateEnvironment();
        if (support != null)
        {
            Log.LogWarning("BigScale inactive: " + support);
            AddDisabledCoordinator(support);
            return;
        }

        Coordinator = new RenderCoordinator(this, Settings, Compatibility);
        _hook = new UrpPostProcessHook(Coordinator, LogInfo);

        if (!LoadNative()) return;
        if (!_hook.Install(Harmony))
        {
            Coordinator.SetSupportMessage(_hook.Status);
            Log.LogError("BigScale inactive: " + _hook.Status);
            return;
        }

        Coordinator.AttachNative(Native.GetRenderEvent());
        // Diagnostics only: observes URP's own final transfer, never changes it. A failure here costs
        // one log line, not super resolution.
        _finalPassHook = new UrpFinalPassHook(Coordinator, LogInfo);
        if (!_finalPassHook.Install(Harmony)) Log.LogWarning(_finalPassHook.Status);
        // VR frame generation has its own ABI. A mismatch disables only that generator: the desktop
        // paths and super resolution are proven independently and must keep working.
        string? vrfgLayout;
        try
        {
            vrfgLayout = NativeVrFg.VerifyLayout();
        }
        catch (Exception exception)
        {
            vrfgLayout = exception.GetType().Name + ": " + exception.Message;
        }
        if (vrfgLayout != null)
        {
            Log.LogWarning("VR frame generation unavailable: " + vrfgLayout);
            Coordinator.VrFrameGeneration.SetUnavailable(vrfgLayout);
        }
        // Frame generation installs its own URP capture hook and player-loop boundaries. Without the
        // loop the frame id would never advance, so a failure disables frame generation explicitly
        // instead of letting the transport wait forever. It runs in its own setup scope: an FG
        // problem must never turn a working super-resolution start into a global Unsupported state.
        _fgHook = new FrameGenerationCaptureHook(Coordinator.FrameGeneration, LogInfo);
        _fgLoop = new FrameGenerationPlayerLoop();
        string? fgReason;
        try
        {
            Coordinator.FrameGeneration.AttachNative(NativeFg.GetRenderEvent());
            fgReason = Coordinator.FrameGeneration.Unavailable;
            if (fgReason == null && !_fgHook.Install(Harmony)) fgReason = _fgHook.Status;
            if (fgReason == null && !_fgLoop.Install()) fgReason = _fgLoop.Status;
        }
        catch (Exception exception)
        {
            fgReason = "frame generation setup failed: " + exception;
        }
        if (fgReason != null)
        {
            Log.LogWarning("frame generation unavailable: " + fgReason);
            Coordinator.FrameGeneration.SetUnavailable(fgReason);
            _fgHook.Uninstall();
            _fgLoop.Uninstall();
            _fgHook = null;
            _fgLoop = null;
        }
        else
        {
            Coordinator.FrameGeneration.CaptureHookStatus = _fgHook.Status;
            Log.LogInfo("frame generation capture ready: " + _fgHook.Status + "; " + _fgLoop.Status + ".");
        }
        Callbacks = new RenderingCallbacks(Coordinator, LogInfo);
        if (!Callbacks.Install(Harmony))
            Log.LogError("no per-camera entry point was available; BigScale keeps the game's renderer.");
        Log.LogInfo($"BigScale {PluginVersion} ready: Unity {Application.unityVersion}, game {Application.version}, " +
                    $"API {SystemInfo.graphicsDeviceType}, GPU {SystemInfo.graphicsDeviceName}, {_hook.Status}.");
    }

    /// <summary>
    /// IL2CPP cannot instantiate a managed MonoBehaviour that was never registered, so both injected
    /// components are registered before their first AddComponent.
    /// </summary>
    private void RegisterInjectedComponents()
    {
        TryRegister(typeof(PluginBehaviour));
        TryRegister(typeof(SettingsPanel.PanelHost));
    }

    private static void TryRegister(Type type)
    {
        try
        {
            Il2CppInterop.Runtime.Injection.ClassInjector.RegisterTypeInIl2Cpp(type);
        }
        catch (Exception exception)
        {
            // Already registered is the normal second case; anything else is reported by the
            // component failing to attach later.
            Instance?.Log.LogDebug($"component registration for {type.Name}: {exception.Message}");
        }
    }

    /// <summary>
    /// Publishes the two startup-only diagnostic switches to the native bridge through the process
    /// environment. This is internal plumbing, not a user-facing variable: the configuration file is
    /// the only thing a player edits, and both keys say a restart is required.
    /// </summary>
    private void ApplyNativeDiagnostics(ModSettings settings)
    {
        try
        {
            Environment.SetEnvironmentVariable("BIGSCALE_XR_PROOF", settings.XrProof.Value ? "1" : null);
            Environment.SetEnvironmentVariable("BIGSCALE_DX12_VALIDATION",
                settings.Dx12Validation.Value ? "1" : null);
            // Variables the pre-rename build read, cleared so a shell that still exports them cannot
            // make a run look configured when it is not. Nothing in this build reads them.
            Environment.SetEnvironmentVariable("BWDLSS_VERIFY_XR", null);
            Environment.SetEnvironmentVariable("BWDLSS_DX12_VALIDATION", null);
        }
        catch (Exception exception)
        {
            Log.LogWarning("could not publish the native diagnostics switches: " + exception.Message);
        }
    }

    private void ReportSettingsHooks(string message)
    {
        if (_settingsHookReported) return;
        _settingsHookReported = true;
        Log.LogWarning(message);
    }

    private void ReportHudHookFailure(string message) => Log.LogWarning(message);

    private void OnSettingsChanged()
    {
        try
        {
            Coordinator?.NotifySettingsChanged();
        }
        catch (Exception exception)
        {
            Log.LogWarning("applying a settings change failed: " + exception.Message);
        }
    }

    private void AddDisabledCoordinator(string reason)
    {
        Coordinator = new RenderCoordinator(this, Settings, Compatibility);
        Coordinator.SetSupportMessage(reason);
    }

    internal void LogInfo(string message) => Log.LogInfo(message);

    internal void LogError(string message) => Log.LogError(message);

    /// <summary>API check that does not need the native bridge. Null means usable.</summary>
    private static string? ValidateEnvironment()
    {
        // The game renderer must stay D3D11: every backend reconstructs in that device, and the
        // experimental DX12 paths are bridges around it, never a replacement renderer.
        if (SystemInfo.graphicsDeviceType != GraphicsDeviceType.Direct3D11)
            return $"graphics API is {SystemInfo.graphicsDeviceType}; upscaling requires the Direct3D 11 renderer.";
        return null;
    }

    /// <summary>Detects a conflicting renderer without taking a compile-time dependency on it.</summary>
    private static string? FindConflict()
    {
        try
        {
            var chainloader = Type.GetType("BepInEx.Bootstrap.Chainloader, BepInEx.Core");
            var property = chainloader?.GetProperty("PluginInfos", BindingFlags.Public | BindingFlags.Static);
            if (property?.GetValue(null) is not IDictionary plugins) return null;
            foreach (var guid in ConflictingGuids)
                if (plugins.Contains(guid)) return guid;
        }
        catch
        {
            // A different BepInEx build shape simply means the conflict check is skipped.
        }
        return null;
    }

    private bool LoadNative()
    {
        var pluginDirectory = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location)!;
        var dataDirectory = Path.Combine(Paths.BepInExRootPath, "BigScale");
        Directory.CreateDirectory(dataDirectory);

        var bridge = Path.Combine(pluginDirectory, "BigScale.Native.dll");
        if (!File.Exists(bridge))
        {
            Coordinator!.SetSupportMessage("BigScale.Native.dll is missing from the plugin directory");
            Log.LogError("BigScale inactive: " + bridge + " does not exist.");
            return false;
        }
        try
        {
            _nativeLibrary = NativeLibrary.Load(bridge);
            NativeLibrary.SetDllImportResolver(typeof(Plugin).Assembly, (name, _, _) =>
                name == Native.Dll ? _nativeLibrary : IntPtr.Zero);
        }
        catch (Exception exception)
        {
            Coordinator!.SetSupportMessage("could not load BigScale.Native.dll: " + exception.Message);
            Log.LogError("BigScale inactive: " + exception);
            return false;
        }

        var layout = Native.VerifyLayout();
        if (layout != null)
        {
            Coordinator!.SetSupportMessage("native bridge ABI mismatch: " + layout);
            Log.LogError("BigScale inactive: " + layout);
            return false;
        }
        string? fgLayout;
        try
        {
            fgLayout = NativeFg.VerifyLayout();
        }
        catch (Exception exception)
        {
            // An older installed bridge has no FG exports at all; that is an explicit mismatch.
            fgLayout = exception.GetType().Name + ": " + exception.Message;
        }
        if (fgLayout != null)
        {
            Coordinator!.SetSupportMessage("frame-generation ABI mismatch: " + fgLayout);
            Log.LogError("BigScale inactive: " + fgLayout);
            return false;
        }

        if (Native.Initialize(pluginDirectory, dataDirectory) == 0)
        {
            Coordinator!.SetSupportMessage("native bridge refused initialisation; see BepInEx/BigScale/native.log");
            Log.LogError("BigScale inactive: BIGSCALE_Initialize returned 0.");
            return false;
        }
        Native.SetEngineVersion(Application.unityVersion);
        Log.LogInfo($"native bridge loaded from {pluginDirectory}; ABI {Native.GetAbiVersion()}.");
        return true;
    }

    public override bool Unload()
    {
        Shutdown();
        return true;
    }

    internal void ShutdownFromBehaviour() => Shutdown();

    private void Shutdown()
    {
        if (_shutdown) return;
        _shutdown = true;

        try
        {
            Settings.Changed -= OnSettingsChanged;
            Menu?.Forget();
            SettingsPanel.DisposeAll();
            TabStatusHud.Instance = null;
            Hud?.Dispose();
            Hud = null;
            Callbacks?.Uninstall(Harmony);
            _hook?.Uninstall();
            _finalPassHook?.Uninstall();
            _fgLoop?.Uninstall();
            _fgHook?.Uninstall();
            Coordinator?.PlayerMotion.Shutdown();
            Coordinator?.Ownership.Restore();
            Coordinator?.Compatibility.Reapply();
            // The presenter child window and detours are inert passthrough after this; the detours
            // themselves stay installed rather than unpatching code another thread may be inside.
            NativeFg.Shutdown();
            NativeVrFg.Shutdown();
        }
        catch (Exception exception)
        {
            Log.LogWarning("restoring graphics settings during shutdown: " + exception.Message);
        }

        // The drain happens on the render thread at the next render event, after everything already
        // queued. Nothing device-owned is released from this thread.
        if (_nativeLibrary != IntPtr.Zero)
        {
            try
            {
                Native.RequestShutdown();
            }
            catch
            {
                // The bridge may already be unloaded during process teardown.
            }
        }
        Compatibility.Dispose();
        Harmony.UnpatchSelf();
        Log.LogInfo("BigScale unloaded; only its own state was released.");
    }
}
