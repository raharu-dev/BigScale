using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
using BigScale.Core;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.RenderGraphModule;
using UnityEngine.Rendering.Universal;

namespace BigScale;

/// <summary>
/// Owns the managed half of frame generation: the requested provider, the camera/history identity,
/// the XeLL latency markers and the capture packet recorded into the URP graph.
///
/// The native side owns every GPU resource and all provider state. This class only publishes CPU
/// values, snapshots the camera once per frame, and reports the native state verbatim. It works with
/// super resolution disabled, because capture readiness is not SR readiness.
///
/// Geometry changes are published to the main-thread <see cref="Tick"/>; only Tick increments the
/// generation and resets history/activity, so a capture callback never mutates shared policy.
/// A capture or setup failure is latched on the main thread, published to the native side as an
/// effective provider of Off, and only an explicit provider or camera/geometry transition re-arms.
/// </summary>
internal sealed class FrameGenerationCoordinator
{
    private sealed class CaptureGeometry
    {
        internal int CameraId;
        internal int RenderWidth;
        internal int RenderHeight;
        internal int OutputWidth;
        internal int OutputHeight;
        internal int HistoryEpoch;
        internal bool ReversedDepth;
        internal bool RendersOverlay;
        internal uint Generation;

        // Identity is the camera plus everything the provider's history depends on. The history
        // epoch itself is derived from identity changes (it is assigned on approval), never compared
        // against the snapshot that produced it, so it cannot feed back into the decision.
        internal bool SameExtent(CaptureGeometry other) =>
            other != null && CameraId == other.CameraId && RenderWidth == other.RenderWidth &&
            RenderHeight == other.RenderHeight && OutputWidth == other.OutputWidth &&
            OutputHeight == other.OutputHeight && ReversedDepth == other.ReversedDepth;
    }

    private readonly FrameGenerationPass _pass;
    private readonly Action<string> _log;
    // Startup snapshot of Diagnostics/XrProof: the presentation proof owns the capture key for its own
    // bounded sequence, so a frame-generation capture must never be armed beside it.
    private readonly bool _xrProof;

    private IntPtr _renderEvent;
    private string? _unavailable;
    private string? _captureFailure;
    private bool _sceneSuspended;
    private string? _nativeFault;
    private bool _nativeFaultUnsupported;

    private int _provider = int.MinValue;
    private int _effectiveProvider = int.MinValue;
    private int _xr;
    private int _menu;
    // Same literal as the native transport's kRestartToSwitch; the row reason is composed locally
    // because the native latch only exists after the worker refuses a capture.
    private const string RestartReason =
        "restart the game to switch between ANR and other frame-generation providers";
    // Gameplay ownership sampled from the game's own world/player state on the main thread. Frame
    // generation never runs on a title, loading or menu camera, however eligible its URP properties
    // look. `_gameplayCameraId` is the local player's own camera, not the super-resolution primary.
    private bool _gameplayActive;
    private int _gameplayCameraId;
    private bool _gameplayReadFailed;
    // Diagnostics capture: a bad key binding is reported once, and the player-renderer inventory is
    // emitted once per adopted gameplay camera.
    private bool _captureKeyFailed;
    private int _rendererInventoryCameraId;
    // Frame-generation camera identity, written only by geometry approval from the main thread. It
    // is not the super-resolution primary: FG must run while SR is off, faulted or unsupported.
    private int _cameraId;
    private int _epoch;
    private uint _generation;
    private uint _configuredGeneration = uint.MaxValue;
    private uint _captureReady;
    private IntPtr _presenterParent;
    private IntPtr _presenterAttempt;
    private ulong _presentId;
    private string? _pendingFailure;

    // Geometry approval. Capture callbacks publish a change-only snapshot; Tick consumes it and is
    // the only writer of the generation and the approved snapshot.
    private CaptureGeometry? _approvedGeometry;
    private CaptureGeometry? _pendingGeometry;
    // The last approved snapshot, retained across an automatic gameplay suspension so returning to
    // the same camera does not look like a real camera change that would clear a latched fault.
    private CaptureGeometry? _suspendedGeometry;

    // One-shot camera diagnostics: the first decision for each camera and reason is reported, so an
    // ineligible camera names the exact gate instead of leaving the card on a silent native wait.
    private readonly HashSet<string> _cameraLog = new();

    // Diagnostics identity. Only these values are compared on ordinary frames, so no telemetry
    // string is ever formatted unless something actually changed or 600 presents elapsed.
    private int _loggedProvider = int.MinValue;
    private uint _loggedGeneration = uint.MaxValue;
    private int _loggedState = int.MinValue;
    private string _loggedReason = "";
    private float _diagnosticWindowStart;

    // Capture scope. Only the render thread writes these while a graph is being recorded.
    private bool _scope;
    private bool _scopeRecorded;
    // Written on the main thread, read on the render thread: true while the configuration asks for a
    // reconstruction at all, so a graph without a HUD-less image is transient rather than a fault.
    // Starts true so a graph recorded before the first tick can never latch a false fault.
    private volatile bool _reconstructionActive = true;
    private bool _scopeNeedsHudless;
    private UniversalCameraData? _scopeCamera;
    private Matrix4x4 _scopeJitter;
    private CaptureGeometry? _scopeGeometry;
    private uint _scopeGeneration;
    private int _scopeEpoch;

    // Previous-frame camera matrices for the frame-generation reprojection contract.
    private bool _previousValid;
    private int _previousCameraId = -1;
    private int _previousEpoch = -1;
    private Matrix4x4 _previousView;
    private Matrix4x4 _previousProjection;

    private readonly FrameGenerationActivity _activity = new();
    // Held outside the UI so opening TAB can never expose a rate measured for another generation.
    private readonly HudTelemetry _presentedRate = new();
    private bool _activityNeedsReset;
    private NativeFg.Status _lastStatus;
    private bool _hasStatus;
    private int _lastRequested;
    private FrameGenerationKind _lastRequestedKind;
    // Total multiplier proven by the native capability query, indexed by FrameGenerationKind. Zero
    // means unknown; the pinned FSR integration is fixed at 2x. A provider switch never borrows
    // another provider's entry.
    private readonly int[] _providerMaximum = { 0, 0, 2, 0, 0, 0 };
    private int _effectiveMultiplier = 2;
    private int _configuredMultiplier = int.MinValue;

    internal FrameGenerationCoordinator(Action<string> log, bool xrProof)
    {
        _log = log;
        _xrProof = xrProof;
        _pass = new FrameGenerationPass(log);
    }

    /// <summary>The frame-generation row of the hold-TAB HUD, refreshed with the native status.</summary>
    internal string HudLine { get; private set; } = "FG: Off - 1x - Off";

    /// <summary>
    /// True only for a provider/generation that the native transport's claimed process family
    /// cannot run, i.e. when the selection needs a game restart. Derived from the selection, so it is
    /// right the moment the provider changes; it is never inferred from a reason string and it is
    /// false in VR, where the generator is intentionally disabled for the whole session.
    /// </summary>
    internal bool RestartRequiredFor(FrameGenerationKind requested) =>
        _hasStatus && _xr == 0 && !VrState.Active && RestartFor(_lastStatus, requested);

    /// <summary>
    /// The cross-family rule the native worker enforces. A process that initialized ANR can never
    /// initialize another provider and vice versa, because ANR keeps its D3D12 hooks installed for
    /// the rest of the process.
    /// </summary>
    private static bool RestartFor(in NativeFg.Status status, FrameGenerationKind requested) =>
        requested != FrameGenerationKind.Off && status.ProcessFamily != NativeFg.FamilyUnused &&
        status.ProcessFamily != (requested switch
        {
            FrameGenerationKind.Anr => NativeFg.FamilyAnr,
            FrameGenerationKind.Streamline => NativeFg.FamilyStreamline,
            _ => NativeFg.FamilyNormal,
        });

    /// <summary>
    /// True when the stock FPS counter must be decorated: the player selected a generator and this
    /// session is not VR. An effectively Off provider, including VR and the master switch, leaves the
    /// stock counter exactly as the game drew it.
    /// </summary>
    internal bool FpsDecorationActive =>
        _lastRequestedKind != FrameGenerationKind.Off && _xr == 0 && !VrState.Active;

    /// <summary>
    /// Total multiplier the selected provider proved it supports, or zero before its capability
    /// answer arrived. Read by the settings panel; the remembered preference is never modified.
    /// </summary>
    internal int ReportedMaximumMultiplier(FrameGenerationKind provider) =>
        provider == FrameGenerationKind.Off ? 0 : _providerMaximum[(int)provider];

    /// <summary>The newest native snapshot, or false before one was read.</summary>
    internal bool TryGetStatus(out NativeFg.Status status)
    {
        status = _lastStatus;
        return _hasStatus;
    }

    /// <summary>The capture hook's install status, reported in the one-shot camera diagnostic.</summary>
    internal string CaptureHookStatus { get; set; } = "not installed";

    /// <summary>The setup reason that disabled frame generation, or null while it is usable.</summary>
    internal string? Unavailable => _unavailable;

    /// <summary>
    /// Stores the validated native render event. A null pointer means frame generation cannot run;
    /// the reason is retained so the card reports it instead of a permanent native wait.
    /// </summary>
    internal void AttachNative(IntPtr renderEvent)
    {
        if (renderEvent == IntPtr.Zero)
        {
            SetUnavailable("the frame-generation render event is unavailable");
            return;
        }
        _renderEvent = renderEvent;
    }

    /// <summary>Retains a hard setup reason and stops every further capture attempt.</summary>
    internal void SetUnavailable(string reason)
    {
        _unavailable = string.IsNullOrEmpty(reason) ? "frame generation could not be set up" : reason;
        _scope = false;
        _presentedRate.Reset();
    }

    /// <summary>
    /// Called by the capture hook from a render thread. Only the first failure is kept; the owning
    /// tick logs it and publishes the effective Off provider exactly once.
    /// </summary>
    internal void ReportCaptureFailure(string reason) =>
        PublishFailure(string.IsNullOrEmpty(reason) ? "the frame-generation capture failed" : reason);

    private void PublishFailure(string reason) => Interlocked.CompareExchange(ref _pendingFailure, reason, null);

    private string? TakeFailure() => Interlocked.Exchange(ref _pendingFailure, null);

    /// <summary>Called from the player loop before input sampling: arms one XeLL frame id.</summary>
    internal void BeginFrame()
    {
        // A call boundary can run before the first tick; the process VR verdict is therefore read
        // here directly instead of trusting the last published configuration.
        if (_renderEvent == IntPtr.Zero || _nativeFault != null || !_gameplayActive || _xr != 0 ||
            VrState.Active || _lastRequestedKind == FrameGenerationKind.Off) return;
        _presentId++;
        NativeFg.BeginFrame(_presentId);
    }

    /// <summary>Called from the player loop after LateUpdate: closes the simulation phase.</summary>
    internal void EndSimulation()
    {
        if (_renderEvent == IntPtr.Zero || !_gameplayActive || _xr != 0 || VrState.Active ||
            _lastRequestedKind == FrameGenerationKind.Off) return;
        NativeFg.EndSimulation(_presentId);
    }

    internal void Tick(ModSettings settings, bool xrActive, bool menuVisible)
    {
        _pass.AbandonStaleLeases(Time.frameCount);

        // Render callbacks and the capture hook publish failures; the main thread owns the latch and
        // the log, so each defect is reported once instead of never or once per frame.
        if (_pass.TakeFailure() is { } renderFailure) ReportCaptureFailure(renderFailure);
        if (TakeFailure() is { } failure && _captureFailure == null)
        {
            _captureFailure = failure;
            _captureReady = 0;
            _scope = false;
            _log("frame generation faulted: " + failure);
        }

        var requestedKind = settings.Enabled.Value
            ? UpscalingModes.Selectable(settings.FrameGeneration.Value)
            : FrameGenerationKind.Off;
        var requested = (int)requestedKind;
        if (_renderEvent == IntPtr.Zero)
        {
            // Before attachment no native export may be called at all.
            _lastRequested = requested;
            _lastRequestedKind = requestedKind;
            HudLine = ComposeUnavailable(requestedKind);
            return;
        }

        // Gameplay ownership is sampled before geometry approval: a snapshot recorded by a camera
        // that no longer belongs to the local player is never approved.
        UpdateGameplayState();
        PollCaptureKey(settings);
        if (settings.Diagnostics.Value && _gameplayActive && _gameplayCameraId != _rendererInventoryCameraId)
        {
            LogPlayerRenderers();
            _rendererInventoryCameraId = _gameplayCameraId;
        }
        ApprovePendingGeometry();

        var xr = xrActive ? 1 : 0;
        var menu = menuVisible ? 1 : 0;
        // Only an explicit provider change re-arms a latched fault here. Camera, extent and output
        // identity belong to geometry approval below, which is the only writer of those values.
        if (requested != _provider)
        {
            InvalidateGeneration();
            _presenterAttempt = IntPtr.Zero;
        }

        var effective = _sceneSuspended || !_gameplayActive || _unavailable != null ||
                _captureFailure != null || xr != 0
            ? (int)FrameGenerationKind.Off
            : requested;
        // The remembered total is a preference, never the value sent to the provider. An unknown
        // capability bootstraps the safe 2x instead of invoking an unverified multi-frame path, and
        // the stored selection is never clamped in place.
        var remembered = settings.FrameMultiplier.Value;
        if (remembered < 2) remembered = 2;
        if (remembered > 6) remembered = 6;
        var maximum = requestedKind == FrameGenerationKind.Off
            ? 0
            : _providerMaximum[(int)requestedKind];
        var effectiveMultiplier = maximum > 0 ? Math.Min(remembered, maximum) : 2;
        if (effectiveMultiplier != _effectiveMultiplier)
        {
            _effectiveMultiplier = effectiveMultiplier;
            // A multiplier change is a new configuration generation: the worker owns the provider
            // parameter change, and queued work of the old generation must not consume this count.
            InvalidateGeneration();
        }
        if (requested != _provider || xr != _xr || menu != _menu || effective != _effectiveProvider ||
            _generation != _configuredGeneration || effectiveMultiplier != _configuredMultiplier)
        {
            _provider = requested;
            _xr = xr;
            _menu = menu;
            // The requested value stays in the settings and in the label; the native side only ever
            // sees the effective provider, so Off stops generation without rewriting the choice.
            _effectiveProvider = effective;
            _configuredGeneration = _generation;
            _configuredMultiplier = effectiveMultiplier;
            var config = new NativeFg.Config
            {
                Abi = NativeFg.AbiVersion,
                Size = (uint)NativeFg.ConfigSize,
                Provider = effective,
                Generation = _generation,
                PrimaryCameraId = _cameraId,
                PrimaryHistoryEpoch = _epoch,
                CaptureReady = (int)_captureReady,
                XrActive = xr,
                MenuVisible = menu,
                Multiplier = (uint)effectiveMultiplier,
            };
            NativeFg.Configure(ref config);
            _activityNeedsReset = true;
        }

        if (requested != (int)FrameGenerationKind.Off && _gameplayActive && _unavailable == null &&
            _captureFailure == null && xr == 0 && !VrState.Active)
            CreatePresenterIfReady();

        var status = new NativeFg.Status();
        if (NativeFg.GetStatus(ref status) == 0)
        {
            _hasStatus = false;
            _lastRequested = requested;
            _lastRequestedKind = requestedKind;
            HudLine = UpscalingStatus.FrameGeneration(UpscalingStatus.FgUnsupported, requestedKind, 1,
                "status unavailable");
            return;
        }
        _hasStatus = true;
        _lastStatus = status;
        _lastRequested = requested;
        _lastRequestedKind = requestedKind;
        // A capability report is trusted only when it names the provider and generation this
        // configuration actually sent. A stale report can never raise another provider's ceiling.
        if (requestedKind != FrameGenerationKind.Off && effective != (int)FrameGenerationKind.Off &&
            status.RequestedProvider == effective && status.Generation == _generation &&
            status.MaxMultiplier > 0)
        {
            var reported = (int)Math.Min(status.MaxMultiplier, 6u);
            if (reported >= 2 && reported != _providerMaximum[(int)requestedKind])
                _providerMaximum[(int)requestedKind] = reported;
        }
        // Retirements are ordered: acknowledge everything up to the newest one the transport published.
        if (status.RetirementCount > 0) NativeFg.AckCaptures(NewestRetirement(status));
        if (_activityNeedsReset && status.Generation == _generation)
        {
            _activity.Reset(_generation, requestedKind, status.GeneratedPresents);
            _activityNeedsReset = false;
        }
        // A real provider fault is latched once and blocks further capture/latency work until an
        // explicit selection or real camera/geometry change re-arms it. Menu suspension never erases
        // the reason.
        if ((status.State is NativeFgState.Faulted or NativeFgState.Unsupported) && _nativeFault == null)
        {
            _nativeFaultUnsupported = status.State == NativeFgState.Unsupported;
            _nativeFault = string.IsNullOrEmpty(status.ReasonText())
                ? "the frame-generation provider reported a terminal failure"
                : status.ReasonText();
            _captureReady = 0;
            _scope = false;
            _log("frame generation faulted: " + _nativeFault);
        }
        _activity.Observe(status.Generation, (FrameGenerationKind)status.ActiveProvider,
            status.State == NativeFgState.Active, status.GeneratedPresents);
        // Visibility is a window operation: it must stay on this (the window owner's) thread.
        var showPresenter = _unavailable == null && _captureFailure == null && _gameplayActive &&
            status.State == NativeFgState.Active && status.GeneratedPresents > 0 && xr == 0 && menu == 0 &&
            _activity.Confirmed(_generation, requestedKind);
        NativeFg.SetPresenterVisible(showPresenter ? 1 : 0);
        HudLine = Compose(status, requestedKind, menu != 0);
        LogDiagnostics(settings, status, requested);
    }

    /// <summary>
    /// Measured presented rate for the decorated stock FPS counter, or null when the HUD must show
    /// <c>--</c>: no full window has completed for this provider/generation yet, a suspension or
    /// fault is in force, the selection needs a restart, or VR owns the display. Between
    /// measurement windows the last completed rate of the same identity is returned, so the decorated
    /// counter never drops the total it already proved.
    /// </summary>
    internal int? SamplePresentedFps(double nowSeconds)
    {
        if (_xr != 0 || VrState.Active || _menu != 0 ||
            _effectiveProvider == (int)FrameGenerationKind.Off || _unavailable != null ||
            _captureFailure != null || _nativeFault != null || _sceneSuspended || !_gameplayActive ||
            !TryGetStatus(out var status))
        {
            _presentedRate.Reset();
            return null;
        }
        if (status.Generation != _generation || RestartFor(status, _lastRequestedKind) ||
            status.State is NativeFgState.Faulted or NativeFgState.Unsupported or NativeFgState.Suspended
                or NativeFgState.Off or NativeFgState.Draining)
        {
            _presentedRate.Reset();
            return null;
        }
        if (!_activity.Confirmed(_generation, _lastRequestedKind))
        {
            _presentedRate.Reset();
            return null;
        }
        var provider = status.ActiveProvider != 0
            ? (FrameGenerationKind)status.ActiveProvider
            : _lastRequestedKind;
        var multiplier = Math.Max(2, _effectiveMultiplier);
        return _presentedRate.Observe(nowSeconds, status.Generation, provider, multiplier,
            status.RenderedPresents, status.GeneratedPresents, out var totalFps)
            ? totalFps
            : (int?)null;
    }

    /// <summary>
    /// Called when the settings page opens or closes. Uses the cached native snapshot so the row
    /// immediately tells the truth about the suspended capture instead of waiting for the next
    /// bounded panel refresh.
    /// </summary>
    internal void RefreshStatusLine(bool settingsOpen)
    {
        // The panel opens between ticks; hide the generated presenter right away rather than one
        // frame later. This is a window operation and therefore only legal on this thread.
        if (settingsOpen) NativeFg.SetPresenterVisible(0);
        if (!_hasStatus)
        {
            HudLine = ComposeUnavailable(_lastRequestedKind);
            return;
        }
        HudLine = Compose(_lastStatus, _lastRequestedKind, settingsOpen);
    }

    /// <summary>
    /// The primary scene unloaded. Frame generation publishes an effective Off to the native side
    /// (teardown runs on the worker) while the user's provider selection stays intact.
    /// </summary>
    internal void SuspendForScene()
    {
        _sceneSuspended = true;
        _captureReady = 0;
        _scope = false;
        _presentedRate.Reset();
        NativeFg.SetPresenterVisible(0);
    }

    /// <summary>A new primary camera was adopted; the next Tick reconfigures the requested provider.</summary>
    internal void ResumeAfterScene()
    {
        _sceneSuspended = false;
    }

    /// <summary>
    /// Drops every derived capture value and advances the generation. An explicit provider change
    /// also clears a latched fault and any retained pre-suspension snapshot; an automatic gameplay
    /// transition keeps the fault and retains that snapshot, so returning to the same camera cannot
    /// silently retry it.
    /// </summary>
    private void InvalidateGeneration(bool clearFaults = true)
    {
        if (clearFaults)
        {
            _nativeFault = null;
            _nativeFaultUnsupported = false;
            _captureFailure = null;
            _suspendedGeometry = null;
        }
        else
        {
            _suspendedGeometry = _approvedGeometry ?? _suspendedGeometry;
        }
        _generation++;
        _presentedRate.Reset();
        _captureReady = 0;
        _previousValid = false;
        _scope = false;
        _approvedGeometry = null;
        Interlocked.Exchange(ref _pendingGeometry, null);
        _activityNeedsReset = true;
    }

    private void ApprovePendingGeometry()
    {
        var pending = Interlocked.Exchange(ref _pendingGeometry, null);
        if (pending == null) return;
        // A snapshot published before a gameplay transition may belong to a camera the local player
        // no longer owns; it is never approved.
        if (!_gameplayActive || pending.CameraId != _gameplayCameraId) return;
        // Absence of an active snapshot after an automatic suspension is not a camera change: the
        // retained pre-suspension snapshot keeps a latched fault latched when the same camera
        // returns.
        var approved = _approvedGeometry ?? _suspendedGeometry;
        if (approved == null || !pending.SameExtent(approved))
        {
            // A new camera or extent is a real discontinuity: advance the generation and the
            // provider history epoch together, and forget the previous frame's matrices.
            _generation++;
            _epoch++;
            _cameraId = pending.CameraId;
            _captureFailure = null;
            _nativeFault = null;
            _nativeFaultUnsupported = false;
            _captureReady = 0;
            _previousValid = false;
            _scope = false;
            _activityNeedsReset = true;
            _log($"frame generation camera accepted: id={pending.CameraId} " +
                 $"render={pending.RenderWidth}x{pending.RenderHeight} " +
                 $"output={pending.OutputWidth}x{pending.OutputHeight} " +
                 $"reversedDepth={pending.ReversedDepth} overlay={pending.RendersOverlay} " +
                 $"generation={_generation} hook=[{CaptureHookStatus}].");
        }
        pending.Generation = _generation;
        pending.HistoryEpoch = _epoch;
        _approvedGeometry = pending;
        _suspendedGeometry = null;
    }

    /// <summary>
    /// Diagnostics-only: the configured key arms one native capture of the next five pairs. A capture
    /// outside gameplay has no provider pair to record, so it is refused with a reason.
    /// </summary>
    private void PollCaptureKey(ModSettings settings)
    {
        if (!settings.Diagnostics.Value) return;
        // The presentation proof owns the same key for its own bounded sequence; arming a frame
        // generation capture from the same press would make both interfere.
        if (_xrProof) return;
        bool pressed;
        try
        {
            var key = settings.FrameCaptureKey.Value;
            if (key == KeyCode.None) return;
            pressed = Input.GetKeyDown(key);
            _captureKeyFailed = false;
        }
        catch (Exception exception)
        {
            if (_captureKeyFailed) return;
            _captureKeyFailed = true;
            _log("frame-generation capture key could not be read: " + exception.Message);
            return;
        }
        if (!pressed) return;
        if (!_gameplayActive)
        {
            _log("frame-generation capture ignored: not in gameplay.");
            return;
        }
        _log(NativeFg.RequestCapture(5) == 1
            ? "frame-generation capture armed: next 5 pairs."
            : "frame-generation capture refused: one is already in progress.");
    }

    /// <summary>
    /// Diagnostics-only inventory of the local player's own renderers. Body pixels that write no
    /// per-object motion carry the static world's camera motion instead, which is what a
    /// forward-displaced body ghost looks like in a generated frame.
    /// </summary>
    private void LogPlayerRenderers()
    {
        try
        {
            var world = WorldManager.instance;
            var character = WorldManager.localPlayerCharacter;
            if (world == null || character == null || character.gameObject == null) return;
            var references = world.playerCameraReferences;
            var camera = references != null ? references.playerCamera : null;
            var uiCamera = references != null ? references.playerUICamera : null;
            var mainMask = camera != null ? camera.cullingMask : 0;
            var uiMask = uiCamera != null && uiCamera.enabled ? uiCamera.cullingMask : 0;
            _log($"player cameras: main mask=0x{mainMask:X8} " +
                 $"ui={(uiMask != 0 ? "0x" + uiMask.ToString("X8") : "none")}");
            var renderers = character.gameObject.GetComponentsInChildren<Renderer>(true);
            var logged = 0;
            foreach (var renderer in renderers)
            {
                if (logged >= 64) break;
                logged++;
                var owner = renderer.gameObject;
                var skinned = renderer.TryCast<SkinnedMeshRenderer>();
                var material = renderer.sharedMaterial;
                var shaderName = material != null && material.shader != null ? material.shader.name : "none";
                var motionPass = material != null && material.FindPass("MotionVectors") >= 0;
                var layer = owner.layer;
                _log($"player renderer: path={RendererPath(renderer.transform, character.transform)} " +
                     $"type={(skinned != null ? "SkinnedMeshRenderer" : renderer.GetType().Name)} " +
                     $"mesh={renderer.TryCast<MeshRenderer>() != null} " +
                     $"active={renderer.enabled && owner.activeInHierarchy} layer={layer} " +
                     $"mv={renderer.motionVectorGenerationMode} " +
                     $"skinnedMv={(skinned != null ? skinned.skinnedMotionVectors.ToString() : "n/a")} " +
                     $"shader={shaderName} mvPass={motionPass} " +
                     $"main={((mainMask >> layer) & 1) != 0} ui={((uiMask >> layer) & 1) != 0}");
            }
        }
        catch (Exception exception)
        {
            _log("player renderer inventory failed: " + exception.Message);
        }
    }

    private static string RendererPath(Transform transform, Transform root)
    {
        var parts = new List<string>();
        for (var current = transform; current != null && current != root; current = current.parent)
            parts.Add(current.name);
        parts.Reverse();
        return string.Join("/", parts);
    }

    /// <summary>
    /// Samples the game's own gameplay ownership once per tick and invalidates capture exactly once
    /// per availability transition. A title, loading or menu camera can satisfy every URP property
    /// this policy checks, so gameplay is never inferred from the graph being rendered. Missing,
    /// disabled or unreadable owners fail closed.
    /// </summary>
    private void UpdateGameplayState()
    {
        var active = false;
        var cameraId = 0;
        try
        {
            var world = WorldManager.instance;
            if (world != null && world.gameObject != null && world.gameObject.activeInHierarchy &&
                world.hasStarted && world.localPlayerFullyReady)
            {
                var character = WorldManager.localPlayerCharacter;
                var references = world.playerCameraReferences;
                if (character != null && character.gameObject != null && character.isActiveAndEnabled &&
                    references != null && references.gameObject != null)
                {
                    var camera = references.playerCamera;
                    if (camera != null && camera.isActiveAndEnabled)
                    {
                        cameraId = camera.GetInstanceID();
                        active = cameraId != 0;
                    }
                }
            }
        }
        catch (Exception exception)
        {
            active = false;
            cameraId = 0;
            if (!_gameplayReadFailed)
            {
                _gameplayReadFailed = true;
                _log("frame generation cannot read the game's gameplay state; generation stays " +
                     "suspended: " + exception.Message);
            }
        }
        if (active) _gameplayReadFailed = false;
        if (active == _gameplayActive && cameraId == _gameplayCameraId) return;
        _gameplayActive = active;
        _gameplayCameraId = cameraId;
        // Entering, leaving and replacing the owned camera are one transition: the provider history,
        // the previous matrices and any pending snapshot belong to the previous owner and are
        // dropped once. A latched fault survives, because a menu round trip is not a repair.
        InvalidateGeneration(false);
        // The presenter is a window operation and must be hidden on this thread before the next
        // native tick can publish anything.
        NativeFg.SetPresenterVisible(0);
        _log(active
            ? $"frame generation gameplay camera adopted: id={cameraId}; capture re-armed."
            : "frame generation suspended: waiting for gameplay.");
    }

    /// <summary>
    /// Reports the first decision for each camera and reason once. An ineligible camera is the normal
    /// state before gameplay starts, so this is a transition diagnostic, never per-frame telemetry.
    /// </summary>
    private void LogCamera(Camera camera, string message)
    {
        var key = camera.GetInstanceID() + "|" + message;
        if (!_cameraLog.Add(key)) return;
        _log($"frame generation camera {camera.GetInstanceID()}: {message} " +
             $"[hook: {CaptureHookStatus}].");
        if (_cameraLog.Count > 32) _cameraLog.Clear();
    }

    /// <summary>
    /// Logs native frame-generation telemetry only when its identity changed or another 600 source
    /// presentations were rendered. Ordinary frames format nothing.
    /// </summary>
    private void LogDiagnostics(ModSettings settings, in NativeFg.Status status, int requested)
    {
        if (!settings.Diagnostics.Value)
        {
            _diagnosticWindowStart = 0f;
            return;
        }
        var now = Time.realtimeSinceStartup;
        if (_diagnosticWindowStart <= 0f) _diagnosticWindowStart = now;
        var reason = status.ReasonText();
        var changed = requested != _loggedProvider || status.Generation != _loggedGeneration ||
            status.State != _loggedState || !string.Equals(reason, _loggedReason, StringComparison.Ordinal);
        if (changed)
        {
            _loggedProvider = requested;
            _loggedGeneration = status.Generation;
            _loggedState = status.State;
            _loggedReason = reason;
            _log($"FG status: requested={requested} active={status.ActiveProvider} " +
                 $"generation={status.Generation} state={status.State} rendered={status.RenderedPresents} " +
                 $"generated={status.GeneratedPresents} result=0x{status.LastResult:x8} reason={reason}");
        }
        if (now - _diagnosticWindowStart < 10f) return;
        _diagnosticWindowStart = now;
        _log($"FG summary: rendered={status.RenderedPresents} generated={status.GeneratedPresents} " +
             $"state={status.State} provider={requested} generation={status.Generation}.");
    }

    private void CreatePresenterIfReady()
    {
        var parent = NativeFg.GetGameWindow();
        if (parent == IntPtr.Zero) return;
        if (parent == _presenterParent || parent == _presenterAttempt) return;
        _presenterAttempt = parent;
        if (NativeFg.CreatePresenterWindow(parent) != 0)
        {
            _presenterParent = parent;
            _log("frame-generation presenter window created on the game window's thread.");
            return;
        }
        // No retry loop: the next attempt needs an explicit provider change, which re-arms this
        // latch and retries against the same window.
        ReportCaptureFailure("frame-generation presenter window could not be created");
    }

    /// <summary>Called from the OnRecordRenderGraph prefix: decides whether this graph is eligible.</summary>
    internal void CaptureBegin(UniversalRenderer renderer)
    {
        _scope = false;
        _scopeRecorded = false;
        _scopeGeometry = null;
        if (_renderEvent == IntPtr.Zero) return;
        // Setup unavailability is terminal and is checked before any capture discovery. A latched
        // capture/provider fault is checked after the camera snapshot below, so a genuinely new
        // camera identity can re-arm while an unchanged one keeps its exact reason and captures
        // nothing.
        if (_unavailable != null) return;
        if (_provider == int.MinValue || _provider == (int)FrameGenerationKind.Off) return;
        if (_xr != 0 || _menu != 0 || VrState.Active) return;
        // No graph outside gameplay may publish a snapshot, not even one the policy would reject
        // later: the title and loading cameras never own the local player's identity.
        if (!_gameplayActive) return;
        var frameData = renderer.frameData;
        if (frameData == null) return;
        UniversalCameraData cameraData;
        try
        {
            cameraData = frameData.Get<UniversalCameraData>();
        }
        catch
        {
            return;
        }
        var camera = cameraData.camera;
        if (camera == null) return;
        if (!Eligible(cameraData, out var reason))
        {
            LogCamera(camera, reason);
            return;
        }
        var geometry = Snapshot(cameraData);
        if (geometry == null)
        {
            LogCamera(camera, "the render or output extent is not positive");
            return;
        }
        var approved = _approvedGeometry;
        if (approved == null || !geometry.SameExtent(approved))
        {
            // The first qualifying graph publishes the snapshot; Tick approves it and this camera's
            // next graph captures. Nothing is captured before the main thread owns the identity.
            Interlocked.Exchange(ref _pendingGeometry, geometry);
            return;
        }
        // A latched failure blocks recording only for this unchanged camera; Tick clears it in
        // ApprovePendingGeometry when a different eligible identity is published above.
        if (_captureFailure != null || _nativeFault != null) return;
        _scopeCamera = cameraData;
        _scopeJitter = cameraData.m_JitterMatrix;
        _scopeGeometry = approved;
        _scopeGeneration = approved.Generation;
        _scopeEpoch = approved.HistoryEpoch;
        _scopeNeedsHudless = _lastRequestedKind is FrameGenerationKind.Nvidia or FrameGenerationKind.Fsr
            or FrameGenerationKind.Anr or FrameGenerationKind.Streamline;
        _scope = true;
    }

    /// <summary>
    /// Reduces the rendered camera to the pure policy input. A value that cannot be read is a
    /// rejection with that reason, never an assumed default.
    /// </summary>
    private bool Eligible(UniversalCameraData cameraData, out string reason)
    {
        reason = "";
        var camera = cameraData.camera;
        try
        {
            var additional = camera.GetComponent<UniversalAdditionalCameraData>();
            var input = new FrameGenerationCameraInput
            {
                Live = camera.isActiveAndEnabled,
                GameCamera = cameraData.cameraType == CameraType.Game,
                // Only the camera the game itself names as the local player's gameplay camera may
                // be captured. A title camera is never gameplay, whatever its URP properties say.
                GameplayActive = _gameplayActive && camera.GetInstanceID() == _gameplayCameraId,
                Preview = cameraData.isPreviewCamera,
                SceneView = cameraData.isSceneViewCamera,
                BaseCamera = cameraData.renderType == CameraRenderType.Base,
                CameraStackCount = additional != null ? additional.cameraStack.Count : 0,
                HasTargetTexture = cameraData.targetTexture != null,
                ResolveToScreen = cameraData.resolveToScreen,
                LastBaseCamera = cameraData.isLastBaseCamera,
                HdrDisplayOutput = cameraData.isHDROutputActive,
                XrEnabled = cameraData.xr != null && cameraData.xr.enabled,
                RenderWidth = cameraData.cameraTargetDescriptor.width,
                RenderHeight = cameraData.cameraTargetDescriptor.height,
                OutputWidth = cameraData.pixelWidth,
                OutputHeight = cameraData.pixelHeight,
            };
            return FrameGenerationCameraPolicy.Accept(input, out reason);
        }
        catch (Exception exception)
        {
            reason = "the camera data could not be read: " + exception.Message;
            return false;
        }
    }

    private CaptureGeometry? Snapshot(UniversalCameraData cameraData)
    {
        try
        {
            var descriptor = cameraData.cameraTargetDescriptor;
            var outputWidth = cameraData.pixelWidth;
            var outputHeight = cameraData.pixelHeight;
            if (descriptor.width <= 0 || descriptor.height <= 0 || outputWidth <= 0 || outputHeight <= 0)
                return null;
            return new CaptureGeometry
            {
                CameraId = cameraData.camera.GetInstanceID(),
                RenderWidth = descriptor.width,
                RenderHeight = descriptor.height,
                OutputWidth = outputWidth,
                OutputHeight = outputHeight,
                ReversedDepth = SystemInfo.usesReversedZBuffer,
                RendersOverlay = cameraData.rendersOverlayUI,
            };
        }
        catch
        {
            return null;
        }
    }

    /// <summary>Called from the GetRenderPassInputs postfix: asks URP for depth and motion vectors.</summary>
    internal void RequestCaptureInputs(ref UniversalRenderer.RenderPassInputSummary summary)
    {
        if (!_scope) return;
        if (!summary.requiresDepthTexture)
        {
            summary.requiresDepthTexture = true;
            summary.requiresDepthTextureEarliestEvent = RenderPassEvent.BeforeRenderingPostProcessing;
        }
        else if (summary.requiresDepthTextureEarliestEvent > RenderPassEvent.BeforeRenderingPostProcessing)
        {
            summary.requiresDepthTextureEarliestEvent = RenderPassEvent.BeforeRenderingPostProcessing;
        }
        summary.requiresMotionVectors = true;
    }

    internal void CaptureAbort() { _scope = false; }

    /// <summary>
    /// Tells the capture whether the session can produce a reconstruction at all. A graph that
    /// recorded no pre-overlay capture while reconstruction is active is an ordinary skip; when it is
    /// not active no graph ever can, and the missing source is a fault the user can act on.
    /// </summary>
    internal void SetReconstructionActive(bool active) { _reconstructionActive = active; }

    /// <summary>
    /// Called from the DrawScreenSpaceUIPass.RenderOverlay prefix, after final tonemapping and
    /// before the game's HUD is drawn. The colour handle is the genuine HUD-less image.
    /// </summary>
    internal void CaptureBeforeUi(RenderGraph renderGraph, ContextContainer frameData, TextureHandle color)
    {
        if (!_scope || !_scopeNeedsHudless || _scopeRecorded) return;
        var cameraData = _scopeCamera;
        if (cameraData == null || frameData == null) return;
        // An absent handle at this alternate boundary is ordinary; only genuine pre-overlay sources
        // can be captured, never a backbuffer saved until graph completion.
        if (!color.IsValid()) return;
        // Managed recording lease exhaustion is transient backpressure.
        if (!_pass.HasFreeLease) return;
        try
        {
            if (Record(renderGraph, frameData, cameraData, color, HudlessRequirement.Overlay))
            {
                _scopeRecorded = true;
                return;
            }
            LogCamera(cameraData.camera, "the pre-overlay capture was not recorded; the next eligible graph retries");
        }
        catch (Exception exception)
        {
            LogCamera(cameraData.camera, "the pre-overlay capture failed (" + exception.Message +
                                        "); the next eligible graph retries");
        }
    }

    /// <summary>
    /// Records the reconstruction output immediately after post-processing and before URP records
    /// overlay UI. Saving a built-in backbuffer handle until graph completion would capture the HUD.
    /// </summary>
    internal void CaptureAfterPostProcessing(RenderGraph renderGraph, ContextContainer frameData,
        int cameraId, TextureHandle color)
    {
        if (!_scope || !_scopeNeedsHudless || _scopeRecorded) return;
        var geometry = _scopeGeometry;
        if (geometry == null) return;
        if (geometry.CameraId != cameraId)
        {
            // A reconstruction of another camera is never a HUD-less image for this one: the camera
            // keeps its own identity and history, and substituting would interpolate two views.
            var camera = _scopeCamera?.camera;
            if (camera != null)
                LogCamera(camera, $"the reconstruction published camera {cameraId}, not this camera; " +
                                  "frame generation waits for its own camera's upscaled image");
            return;
        }
        if (!color.IsValid() || !_pass.HasFreeLease) return;
        var cameraData = _scopeCamera;
        if (cameraData == null || frameData == null) return;
        if (Record(renderGraph, frameData, cameraData, color, HudlessRequirement.CameraColor))
            _scopeRecorded = true;
    }

    /// <summary>Called from the OnRecordRenderGraph postfix: submits one capture packet.</summary>
    internal unsafe void CaptureEnd(UniversalRenderer renderer, RenderGraph renderGraph)
    {
        if (!_scope) return;
        _scope = false;
        if (_renderEvent == IntPtr.Zero) return;
        if (_unavailable != null || _captureFailure != null) return;
        var cameraData = _scopeCamera;
        var geometry = _scopeGeometry;
        if (cameraData == null || geometry == null) return;
        var frameData = renderer.frameData;
        if (frameData == null) return;
        if (_scopeRecorded) return;
        if (_scopeNeedsHudless)
        {
            // No late backbuffer capture: only the two genuine pre-overlay boundaries may supply it.
            // Reconstruction can skip this graph for backpressure or camera identity.
            if (_reconstructionActive)
                LogCamera(cameraData.camera,
                    "this graph recorded no pre-overlay capture; the next eligible graph retries");
            else
                ReportCaptureFailure(
                    "the frame-generation HUD-less image is unavailable; the upscaler must be producing frames");
            return;
        }
        if (!_pass.HasFreeLease) return;
        if (!Record(renderGraph, frameData, cameraData, TextureHandle.nullHandle, HudlessRequirement.None))
            LogCamera(cameraData.camera, "the capture was not recorded for this graph; the next graph retries");
    }

    private unsafe bool Record(RenderGraph renderGraph, ContextContainer frameData, UniversalCameraData cameraData,
        TextureHandle color, HudlessRequirement hudless)
    {
        var geometry = _scopeGeometry;
        if (geometry == null) return false;
        UniversalResourceData resourceData;
        try
        {
            resourceData = frameData.Get<UniversalResourceData>();
        }
        catch
        {
            ReportCaptureFailure("the URP resource data is unavailable for frame generation");
            return false;
        }
        var depth = resourceData.cameraDepthTexture;
        var motion = resourceData.motionVectorColor;
        if (!depth.IsValid() || !motion.IsValid())
        {
            ReportCaptureFailure("frame generation needs URP depth and motion vectors; this camera did not " +
                                 "provide them");
            return false;
        }
        var depthDescriptor = depth.GetDescriptor(renderGraph);
        var motionDescriptor = motion.GetDescriptor(renderGraph);
        if (depthDescriptor.width < geometry.RenderWidth || depthDescriptor.height < geometry.RenderHeight)
        {
            ReportCaptureFailure("the URP depth texture does not contain the active render extent");
            return false;
        }
        var cameraId = geometry.CameraId;
        var epoch = geometry.HistoryEpoch;
        var view = cameraData.GetViewMatrix(0);
        // The native transport converts graph-row depth and motion into presented-backbuffer rows, so
        // the projection is the backbuffer's (not render-texture-flipped) GPU projection, and jitter Y
        // follows the negated motion axis.
        var projection = GL.GetGPUProjectionMatrix(cameraData.GetProjectionMatrixNoJitter(0), false);
        var reset = !_previousValid || _previousCameraId != cameraId || _previousEpoch != epoch;
        var frame = new NativeFg.Frame
        {
            Abi = NativeFg.AbiVersion,
            Size = (uint)NativeFg.FrameSize,
            Generation = _scopeGeneration,
            CameraId = cameraId,
            HistoryEpoch = epoch,
            FrameIndex = Time.frameCount,
            RenderWidth = geometry.RenderWidth,
            RenderHeight = geometry.RenderHeight,
            OutputWidth = geometry.OutputWidth,
            OutputHeight = geometry.OutputHeight,
            MotionWidth = motionDescriptor.width,
            MotionHeight = motionDescriptor.height,
            PresentId = _presentId,
            Flags = (geometry.ReversedDepth ? NativeFg.FlagReversedDepth : 0u) |
                (reset ? NativeFg.FlagReset : 0u),
            JitterX = _scopeJitter.m03 * geometry.RenderWidth * 0.5f,
            JitterY = -_scopeJitter.m13 * geometry.RenderHeight * 0.5f,
            DeltaMilliseconds = Time.unscaledDeltaTime * 1000f,
            CameraNear = cameraData.camera.nearClipPlane,
            CameraFar = cameraData.camera.farClipPlane,
            VerticalFovRadians = VerticalFov(projection),
            Aspect = cameraData.camera.aspect,
        };
        var transform = cameraData.camera.transform;
        WriteVector(frame.CameraPosition, transform.position);
        WriteVector(frame.CameraUp, transform.up);
        WriteVector(frame.CameraRight, transform.right);
        WriteVector(frame.CameraForward, transform.forward);
        WriteRowMajor(frame.CurrentView, view);
        WriteRowMajor(frame.CurrentProjection, projection);
        if (reset)
        {
            // A discontinuity has no meaningful history; the previous transform is the current one.
            WriteRowMajor(frame.PreviousView, view);
            WriteRowMajor(frame.PreviousProjection, projection);
        }
        else
        {
            WriteRowMajor(frame.PreviousView, _previousView);
            WriteRowMajor(frame.PreviousProjection, _previousProjection);
        }
        // Unity imports the game's CameraTarget RTHandle as UniversalResourceData.backBufferColor.
        // That handle has no graph descriptor, and IsBuiltin() alone does not identify it, so the
        // colour is classified by identity: the pass then captures it through its ordered
        // command-buffer binding instead of asking the graph to describe it.
        var boundHudless = hudless != HudlessRequirement.None &&
            (color.IsBuiltin() || color == resourceData.backBufferColor);
        if (!_pass.Record(renderGraph, color, depth, motion, frame, _renderEvent, hudless, boundHudless))
            return false;
        // Publish history only after the graph accepted the recording.
        _previousView = view;
        _previousProjection = projection;
        _previousCameraId = cameraId;
        _previousEpoch = epoch;
        _previousValid = true;
        _captureReady = 1;
        return true;
    }

    private static float VerticalFov(Matrix4x4 projection)
    {
        var m11 = Mathf.Abs(projection.m11);
        return m11 > 1e-6f ? 2f * Mathf.Atan(1f / m11) : Mathf.PI / 3f;
    }

    private static unsafe void WriteVector(float* destination, Vector3 value)
    {
        destination[0] = value.x;
        destination[1] = value.y;
        destination[2] = value.z;
    }

    // Unity stores column-vector matrices; the frame-generation contract is row-major row-vector.
    private static unsafe void WriteRowMajor(float* destination, Matrix4x4 matrix)
    {
        for (var row = 0; row < 4; row++)
            for (var column = 0; column < 4; column++)
                destination[row * 4 + column] = matrix[column, row];
    }

    private static uint NewestRetirement(in NativeFg.Status status)
    {
        var newest = 0u;
        if (status.Retirement0.Sequence > newest) newest = status.Retirement0.Sequence;
        if (status.Retirement1.Sequence > newest) newest = status.Retirement1.Sequence;
        if (status.Retirement2.Sequence > newest) newest = status.Retirement2.Sequence;
        if (status.Retirement3.Sequence > newest) newest = status.Retirement3.Sequence;
        if (status.Retirement4.Sequence > newest) newest = status.Retirement4.Sequence;
        if (status.Retirement5.Sequence > newest) newest = status.Retirement5.Sequence;
        if (status.Retirement6.Sequence > newest) newest = status.Retirement6.Sequence;
        if (status.Retirement7.Sequence > newest) newest = status.Retirement7.Sequence;
        return newest;
    }

    /// <summary>
    /// Row text before native telemetry is available. VR always reports the locked Off state, never
    /// the remembered desktop provider.
    /// </summary>
    private string ComposeUnavailable(FrameGenerationKind requested)
    {
        if (_xr != 0 || VrState.Active)
            return UpscalingStatus.FrameGeneration(UpscalingStatus.FgVrDisabled, FrameGenerationKind.Off, 1, "");
        if (requested == FrameGenerationKind.Off)
            return UpscalingStatus.FrameGeneration(UpscalingStatus.FgOff, FrameGenerationKind.Off, 1, "");
        return UpscalingStatus.FrameGeneration(UpscalingStatus.FgUnsupported, requested,
            Math.Max(2, _effectiveMultiplier),
            _unavailable ?? "the frame-generation transport is unavailable");
    }

    /// <summary>
    /// One row of the HUD from one native snapshot. The provider and multiplier shown are the ones
    /// that actually produced output for this generation; a requested value alone is never printed
    /// as Active.
    /// </summary>
    private string Compose(NativeFg.Status status, FrameGenerationKind requested, bool settingsOpen)
    {
        if (_xr != 0 || VrState.Active)
            return UpscalingStatus.FrameGeneration(UpscalingStatus.FgVrDisabled, FrameGenerationKind.Off, 1, "");
        if (requested == FrameGenerationKind.Off)
            return UpscalingStatus.FrameGeneration(UpscalingStatus.FgOff, FrameGenerationKind.Off, 1, "");
        var configured = Math.Max(2, _effectiveMultiplier);
        // A process-family refusal needs a restart the player must perform; it outranks the generic
        // Unsupported wording and is derived from the selection plus the claimed family, so it is
        // visible before the worker ever captures a frame.
        if (RestartFor(status, requested))
            return UpscalingStatus.FrameGeneration(UpscalingStatus.FgRestart, requested, configured,
                RestartReason);
        if (_sceneSuspended)
            return UpscalingStatus.FrameGeneration(UpscalingStatus.FgSuspended, requested, configured,
                "waiting for a new camera");
        if (_unavailable != null)
            return UpscalingStatus.FrameGeneration(UpscalingStatus.FgUnsupported, requested, configured,
                _unavailable);
        if (_captureFailure != null)
            return UpscalingStatus.FrameGeneration(UpscalingStatus.FgFaulted, requested, configured,
                _captureFailure);
        if (_nativeFault != null)
            return UpscalingStatus.FrameGeneration(_nativeFaultUnsupported ? UpscalingStatus.FgUnsupported
                : UpscalingStatus.FgFaulted, requested, configured, _nativeFault);

        // Terminal faults are reported above; a selected provider outside gameplay is suspended, not
        // Active and not Unsupported.
        if (!_gameplayActive)
            return UpscalingStatus.FrameGeneration(UpscalingStatus.FgSuspended, requested, configured,
                "waiting for gameplay");

        var reason = status.ReasonText();
        var confirmed = _activity.Confirmed(_generation, requested);
        var provider = confirmed ? _activity.ConfirmedProvider
            : status.ActiveProvider != 0 ? (FrameGenerationKind)status.ActiveProvider : requested;
        var observed = status.ActiveMultiplier >= 2 ? (int)status.ActiveMultiplier : configured;

        // A real SDK fault or refusal supersedes menu suspension and any historical proof.
        if (status.State == NativeFgState.Faulted)
            return UpscalingStatus.FrameGeneration(UpscalingStatus.FgFaulted, provider, observed, reason);
        if (status.State == NativeFgState.Unsupported)
            return UpscalingStatus.FrameGeneration(UpscalingStatus.FgUnsupported, provider, observed, reason);

        if (settingsOpen)
            return UpscalingStatus.FrameGeneration(UpscalingStatus.FgSuspended, provider, observed,
                "settings open");

        switch (status.State)
        {
            case NativeFgState.Active when confirmed:
                return UpscalingStatus.FrameGeneration(UpscalingStatus.FgActive, provider, observed, "");
            case NativeFgState.Active:
                // Native Active without a new generated image for this generation is not proof.
                return UpscalingStatus.FrameGeneration(UpscalingStatus.FgWaiting, requested, configured,
                    "waiting for a generated frame");
            case NativeFgState.Suspended:
                return UpscalingStatus.FrameGeneration(UpscalingStatus.FgSuspended, provider, observed,
                    reason.Length > 0 ? reason : "suspended");
            case NativeFgState.Faulted:
                return UpscalingStatus.FrameGeneration(UpscalingStatus.FgFaulted, provider, observed, reason);
            case NativeFgState.Unsupported:
                return UpscalingStatus.FrameGeneration(UpscalingStatus.FgUnsupported, provider, observed, reason);
            case NativeFgState.Off:
                return UpscalingStatus.FrameGeneration(UpscalingStatus.FgOff, FrameGenerationKind.Off, 1, "");
            default:
                return UpscalingStatus.FrameGeneration(UpscalingStatus.FgWaiting, provider, observed, reason);
        }
    }

    internal static class NativeFgState
    {
        internal const int Off = 0;
        internal const int WaitingSwapchain = 1;
        internal const int WaitingWindow = 2;
        internal const int Initializing = 3;
        internal const int WaitingFirstGenerated = 4;
        internal const int Active = 5;
        internal const int Suspended = 6;
        internal const int Draining = 7;
        internal const int Faulted = 8;
        internal const int Unsupported = 9;
    }
}
