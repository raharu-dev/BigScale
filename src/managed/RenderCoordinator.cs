using System;
using System.Collections.Generic;
using BigScale.Core;
using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;
using UnityEngine.Rendering.RenderGraphModule;
using UnityEngine.Rendering.Universal;

namespace BigScale;

/// <summary>Renderer-neutral status shown in the panel and the BepInEx log.</summary>
internal enum RendererState
{
    Disabled,
    Waiting,
    Active,
    Unsupported,
    Faulted,
}

/// <summary>
/// Decides, per camera and frame, whether reconstruction may run, keeps the mode/size fit current,
/// and drives probe-first transitions so a half-configured eye is never presented.
///
/// Ownership is never mutated while a render graph is being recorded: the coordinator only asks for
/// a commit or a restore, and <see cref="OnCameraBoundary"/> applies it before URP creates the
/// camera's frame data. Everything before and after a fault keeps the game's own renderer.
/// </summary>
internal sealed class RenderCoordinator
{
    private readonly Plugin _plugin;
    private readonly ModSettings _settings;
    private readonly BigVisualsCompatibility _compatibility;
    private readonly GraphicsOwnership _ownership;
    private readonly OutputResources _output = new();
    private readonly UpscalingPass _pass;
    private readonly StockPostProcessing _stock;
    private readonly ViewHistory _history = new();
    private readonly TransitionPlan _plan = new();
    private readonly FrameGenerationCoordinator _frameGeneration;
    private readonly VrFrameGenerationCoordinator _vrFrameGeneration;
    private readonly XrProofMode _proof;
    private readonly XrArrayProof _xrArrayProof;
    private readonly PlayerMotionVectors _playerMotion;

    private readonly int[] _eyeWidths = new int[2];
    private readonly int[] _eyeHeights = new int[2];
    private readonly ModeRange[][] _eyeRanges = { Array.Empty<ModeRange>(), Array.Empty<ModeRange>() };
    private readonly Native.Frame[] _frames = new Native.Frame[2];
    private readonly Dictionary<long, ModeRange[]> _modeCache = new();

    private IntPtr _renderEvent;
    private bool _nativeReady;
    private RendererState _state = RendererState.Waiting;
    private string _stateReason = "waiting for a supported camera";
    private SizeFit? _fit;
    private int _fitWidth;
    private int _fitHeight;
    private int _primaryCameraId;
    private Camera? _primaryCamera;
    private int _primarySceneHandle;
    private long _primaryArea;
    private int _lastPrimarySeenFrame;
    private int _lastActiveFrame = -1;
    private bool _announcedActive;
    private bool _probeQueuedLogged;
    private bool _probeExecutedLogged;
    // Last frame at which the temporal packet was logged; once per 600 frames at most.
    private int _lastPacketLogFrame = -1;
    private bool _recoveryPending;
    private int _recoveryRequestedFrame;
    private uint _lastAcknowledgedSequence;
    private string? _lastFailure;
    private readonly HashSet<string> _skipReasons = new();
    // Diagnostics-only counters for URP's own image transfer: first few occurrences, then periodic.
    private int _uberPostCalls;
    private int _postExitCalls;
    private int _postExitSignature = -1;
    private int _finalPostCalls;
    private int _finalBlitCalls;

    // Provider identity. The requested value comes from the settings; the selected value is only
    // advanced by a retired switch acknowledgement from the bridge.
    private readonly RetiredFrameSummary _retired = new();
    private UpscalerKind _requestedUpscaler = UpscalerKind.Off;
    private UpscalerKind _selectedUpscaler = UpscalerKind.Off;
    private bool _switchPending;
    private bool _switchAcked;
    private float _switchReservedAt;
    private int _switchLeaseGeneration;
    private bool _switchQueuedLogged;
    private bool _xrActive;

    // XR: the reduced intermediate extent declared through the URP camera-data hook, and the jitter
    // injected into the VR mod's per-eye matrices. Both are per-frame facts; nothing here is derived
    // from a value another mod may rewrite.
    private bool _xrExtentAvailable;
    private int _xrExtentFrame = -1;
    private int _xrJitterFrame = -1;
    private float _xrJitterPixelX;
    private float _xrJitterPixelY;
    private int _xrJitterInputWidth;
    private int _xrJitterInputHeight;
    private int _xrJitterAppliedFrame = -1;
    private int _xrJitterResetFrame = -1;
    private int _xrJitterAnomalyFrame = -600;
    private string _xrJitterAnomaly = "";
    private int _xrEyeReads0;
    private int _xrEyeReads1;
    private bool _xrEye0Applied;
    private bool _xrEye1Applied;
    private bool _vrJitterHookOk;
    private string _vrJitterHookReason = "the VR eye-matrix hook has not reported its state";
    private readonly Matrix4x4[] _xrSavedProjection = new Matrix4x4[2];
    private readonly int[] _xrSavedProjectionFrame = { -1, -1 };
    private int _xrMismatchFrame = -600;
    private readonly bool _xrVerify;
    private Matrix4x4 _xrPrevViewProj;
    private int _xrPrevCameraId;
    private int _xrMotionLoggedCamera;
    private CommandBuffer? _xrMotionCommands;
    private static readonly bool InjectXrJitter = true;

    // XR UberPost seam. Armed by the outer post prefix, consumed by the nested UberPost prefix and
    // postfix, and cleared by the outer finalizer. Only pointers and value types are retained, so no
    // graph or frame-data reference survives the outer invocation.
    private int _xrScopeTokens;
    private int _xrScopeToken;
    private IntPtr _xrScopePass;
    private IntPtr _xrScopeGraph;
    private IntPtr _xrScopeFrameData;
    private int _xrScopeCameraId;
    private int _xrScopeInputWidth;
    private int _xrScopeInputHeight;
    private int _xrScopeOutputWidth;
    private int _xrScopeOutputHeight;
    private int _xrScopeLayout;
    private int _xrScopeSlices;
    private TextureDimension _xrScopeDimension;
    private VRTextureUsage _xrScopeVrUsage;
    private bool _xrScopeRecorded;
    private bool _xrScopeFinished;
    private TextureHandle _xrScopePostDestination;
    private TextureHandle _xrScopeReconstruction;
    private TextureHandle _xrScopeActiveColor;
    // True only while the substituted post chain runs, so its re-entrant invocation of the patched
    // method executes URP's stock body instead of arming a second scope.
    private bool _xrScopeReplaying;
    private string? _xrScopeFailure;
    private int _xrScopeLogs;

    // Last render-affecting values applied. Diagnostics and the menu key are deliberately absent:
    // changing them must never reset rendering or clear a latched fault.
    private bool _lastEnabled;
    private UpscalerKind _lastUpscaler = UpscalerKind.Off;
    private UpscalingPreset _lastPreset;
    private float _sceneLostAt = -1f;
    // Last stock FPS label, the measured total it was decorated with, and the result. The stock label
    // is rewritten by the game every frame, so this keeps a stable decoration from re-formatting.
    private string _decorateStock = "";
    private int? _decorateTotal;
    private string _decorateResult = "";
    private bool _decorateCached;
    private long _diagnosticExecuted;
    private long _diagnosticRetired;
    private float _diagnosticWindowStart;
    private FrameGenerationKind _lastFrameGeneration = FrameGenerationKind.Off;
    private FrameGenerationKind _lastVrFrameGeneration = FrameGenerationKind.Off;

    internal RenderCoordinator(Plugin plugin, ModSettings settings, BigVisualsCompatibility compatibility)
    {
        _plugin = plugin;
        _settings = settings;
        _compatibility = compatibility;
        _ownership = new GraphicsOwnership(plugin.LogInfo);
        // Startup snapshot: Diagnostics/XrProof is read once, here, and never on the render path.
        _xrVerify = settings.XrProof.Value;
        _pass = new UpscalingPass(plugin.LogInfo, _output);
        _stock = new StockPostProcessing { Log = plugin.LogInfo };
        _frameGeneration = new FrameGenerationCoordinator(plugin.LogInfo, _xrVerify);
        _vrFrameGeneration = new VrFrameGenerationCoordinator(plugin.LogInfo);
        _proof = new XrProofMode(plugin.LogInfo, settings);
        _xrArrayProof = new XrArrayProof(plugin.LogInfo, _xrVerify);
        _playerMotion = new PlayerMotionVectors(plugin.LogInfo);
        _lastEnabled = settings.Enabled.Value;
        _lastUpscaler = settings.Upscaler.Value;
        _lastPreset = settings.Preset.Value;
        _lastFrameGeneration = settings.FrameGeneration.Value;
        _lastVrFrameGeneration = settings.VrFrameGeneration.Value;
    }

    internal FrameGenerationCoordinator FrameGeneration => _frameGeneration;
    internal VrFrameGenerationCoordinator VrFrameGeneration => _vrFrameGeneration;
    internal PlayerMotionVectors PlayerMotion => _playerMotion;
    internal GraphicsOwnership Ownership => _ownership;
    internal RendererState State => _state;
    internal bool NativeReady => _nativeReady;
    internal SizeFit? Fit => _fit;
    internal ViewHistory History => _history;
    internal BigVisualsCompatibility Compatibility => _compatibility;
    internal bool OwnsRendering => _ownership.Owned;

    internal string StateLabel => _state switch
    {
        RendererState.Active => UpscalingStatus.StateActive,
        RendererState.Disabled => UpscalingStatus.StateOff,
        RendererState.Unsupported => UpscalingStatus.StateUnsupported,
        RendererState.Faulted => UpscalingStatus.StateFaulted,
        _ => UpscalingStatus.StateWaiting,
    };

    /// <summary>
    /// Builds the two hold-TAB rows from one consistent rendering snapshot. The upscaler shown is the
    /// one that actually retired the displayed frame; before that proof the selected provider is
    /// shown with an explicitly pending extent. The frame-generation row comes from the coordinator's
    /// cached native status, never from a second native read.
    /// </summary>
    internal void BuildHudText(out string upscalerLine, out string frameGenerationLine)
    {
        var active = _state is RendererState.Active;
        var matches = active && UpscalingStatus.Matches(_retired, _selectedUpscaler, _settings.Preset.Value);
        RetiredFrameSummary? summary = matches ? _retired : null;
        var provider = summary != null ? summary.Upscaler : _selectedUpscaler;
        if (!_settings.Enabled.Value) provider = UpscalerKind.Off;

        var configuredWidth = 0;
        var configuredHeight = 0;
        var outputWidth = 0;
        var outputHeight = 0;
        var eyes = 0;
        if (_state != RendererState.Disabled && _fit is { Ok: true } fit)
        {
            configuredWidth = fit.Eyes[0].Width;
            configuredHeight = fit.Eyes[0].Height;
            outputWidth = _fitWidth;
            outputHeight = _fitHeight;
            eyes = fit.Eyes.Length;
        }
        upscalerLine = UpscalingStatus.UpscalerHud(provider, summary, configuredWidth, configuredHeight,
            outputWidth, outputHeight, eyes);
        frameGenerationLine = _xrActive ? _vrFrameGeneration.HudLine : _frameGeneration.HudLine;
    }

    /// <summary>
    /// Decorates the stock FPS value with the measured presented total, or returns it unchanged while
    /// frame generation is effectively off. The measurement is held by the frame-generation
    /// coordinator, so a rate from another generation can never be shown. Called after every stock
    /// counter update, so the formatted result is reused while the label and the measured total are
    /// both unchanged.
    /// </summary>
    internal string DecorateStockFps(string stockText)
    {
        if (!_frameGeneration.FpsDecorationActive)
        {
            _decorateCached = false;
            return stockText;
        }
        var total = _frameGeneration.SamplePresentedFps(Time.unscaledTimeAsDouble);
        if (_decorateCached && _decorateTotal == total &&
            string.Equals(_decorateStock, stockText, StringComparison.Ordinal))
            return _decorateResult;
        var decorated = HudTelemetry.DecorateFps(stockText, total);
        _decorateStock = stockText ?? "";
        _decorateTotal = total;
        _decorateResult = decorated;
        _decorateCached = true;
        return decorated;
    }

    internal void AttachNative(IntPtr renderEvent)
    {
        _renderEvent = renderEvent;
        _nativeReady = true;
    }

    internal void SetSupportMessage(string message)
    {
        _plan.Unsupported(message);
        if (_nativeReady) return;
        _state = RendererState.Unsupported;
        _stateReason = message;
    }

    /// <summary>
    /// Called when <see cref="Tick"/> throws. The ownership plan latches the defect so the panel shows
    /// the real reason instead of a stale Active state; only an explicit settings transition clears it.
    /// </summary>
    internal void ReportTickFailure(Exception exception)
    {
        var reason = "render tick failed: " + exception.GetType().Name + ": " + exception.Message;
        _plan.Fail(reason);
        _state = RendererState.Faulted;
        _stateReason = reason;
        _lastFailure = reason;
    }

    /// <summary>
    /// The pre-camera-data boundary. This is the only place that writes a camera or pipeline value,
    /// and it runs before URP reads the scale and the antialiasing mode.
    /// </summary>
    internal void OnCameraBoundary(Camera camera, string source)
    {
        if (!_nativeReady) return;
        if (camera == null) return;
        if (!IsRenderCamera(camera, out var additional)) return;

        var id = camera.GetInstanceID();
        // Adoption happens only in OnPostProcess, after the camera proved it is an eligible
        // rendered base camera. A pre-camera callback running first is never evidence of primacy.
        if (id != _primaryCameraId) return;
        _lastPrimarySeenFrame = Time.frameCount;

        if (_plan.OnBoundary() == BoundaryAction.Restore) RestoreNow("boundary restore");

        // A commit requested by the last recorded frame is applied at this boundary, exactly once.
        if (_plan.TryBeginCommit()) CommitNow(camera, additional, source);
        else if (_plan.OwnsRendering && _fit != null)
        {
            // The scene may rewrite the global scale or MSAA after a hidden graphics reset. Reapply
            // the values this mod already owns instead of capturing the drifted state.
            var asset = GraphicsSettings.currentRenderPipeline?.TryCast<UniversalRenderPipelineAsset>();
            if (asset != null && _ownership.IsAsset(asset))
                _ownership.EnsureApplied(asset, camera, additional, _fit.EffectiveScale, !_xrActive,
                    !_xrActive);
        }

        // The VR mod reads its eye matrices later in this same camera render, so the jitter of this
        // frame has to be chosen here, once, before it is read.
        if (_xrActive && _plan.OwnsRendering && _fit is { Eyes.Length: > 0 } fit && _xrJitterFrame != Time.frameCount)
        {
            _xrJitterFrame = Time.frameCount;
            _xrEye0Applied = false;
            _xrEye1Applied = false;
            _xrEyeReads0 = 0;
            _xrEyeReads1 = 0;
            var input = fit.Eyes[0];
            var phases = XrJitter.PhaseCount(input.Width, _fitWidth);
            var (pixelX, pixelY) = XrJitter.PixelOffset(Time.frameCount, phases);
            // The sample is kept in render pixels and shared by both eyes. Each eye converts it with the
            // extent its own raster uses, so an asymmetric multipass pair can never inherit eye 0's basis.
            _xrJitterPixelX = pixelX;
            _xrJitterPixelY = pixelY;
            _xrJitterInputWidth = input.Width;
            _xrJitterInputHeight = input.Height;
        }
    }

    /// <summary>
    /// Applies this frame's sub-pixel sample to one freshly built eye projection. The caller inserts
    /// this between the VR mod's own <c>GL.GetGPUProjectionMatrix</c> call and everything it derives
    /// from that result, so the remap, the packed view-projection and its inverse all describe the
    /// same shifted projection. Nothing is written to a stored matrix, so a repeated read in one
    /// frame re-derives the same values instead of stacking the offset.
    /// </summary>
    internal Matrix4x4 ApplyXrProjection(Matrix4x4 gpuProjection, int eye, Camera camera)
    {
        if (eye < 0 || eye > 1) return gpuProjection;
        if (!TryGetXrJitter(camera, out var width, out var height)) return gpuProjection;
        NoteXrEyeRead(eye);
        var (ndcX, gpuY) = XrJitter.ClipOffset(_xrJitterPixelX, _xrJitterPixelY, width, height,
            SystemInfo.graphicsUVStartsAtTop);
        _xrSavedProjection[eye] = gpuProjection;
        _xrSavedProjectionFrame[eye] = Time.frameCount;
        MarkXrJitterApplied(eye);
        return Matrix4x4.Translate(new Vector3(ndcX, gpuY, 0f)) * gpuProjection;
    }

    /// <summary>
    /// True when this camera's next eye projection must carry this frame's sample. False means no
    /// sample was chosen for this frame or camera, so the packet must report identity rather than a
    /// URP jitter that never reached the raster.
    /// </summary>
    private bool TryGetXrJitter(Camera camera, out int width, out int height)
    {
        width = _xrJitterInputWidth;
        height = _xrJitterInputHeight;
        if (!InjectXrJitter) return false;
        if (camera == null) return false;
        if (_xrJitterFrame != Time.frameCount) return false;
        if (camera.GetInstanceID() != _primaryCameraId) return false;
        return width > 0 && height > 0;
    }

    /// <summary>Counts sampled eye reads per frame so the packet log shows whether the mod reads twice.</summary>
    private void NoteXrEyeRead(int eye)
    {
        if (eye == 0) _xrEyeReads0++;
        else _xrEyeReads1++;
    }

    /// <summary>Records that one eye's projection of this frame carries the sample.</summary>
    private void MarkXrJitterApplied(int eye)
    {
        if (eye == 0) _xrEye0Applied = true;
        else _xrEye1Applied = true;
        if (_xrEye0Applied && _xrEye1Applied) _xrJitterAppliedFrame = Time.frameCount;
    }

    /// <summary>Reports whether the VR eye-matrix hook installed and can be trusted.</summary>
    internal void MarkVrJitterHook(bool installed, string reason)
    {
        _vrJitterHookOk = installed;
        _vrJitterHookReason = reason;
    }

    /// <summary>
    /// Verification-only: proves the VR mod derived its packed view-projection and inverse from the
    /// projection this mod shifted. Compares the stored blocks against the saved unjittered projection
    /// and the view matrix the same call wrote, then measures the displacement of three world points.
    /// </summary>
    internal void VerifyXrEyeMatrices(int eye, Camera camera, float[]? matrices)
    {
        if (!_xrVerify || !_xrActive || matrices == null || eye < 0 || eye > 1) return;
        if (!_vrJitterHookOk || camera == null) return;
        if (_xrSavedProjectionFrame[eye] != Time.frameCount) return;
        var view = Block(matrices, eye * 16);
        var stored = Block(matrices, 32 + eye * 16);
        var inverse = Block(matrices, 64 + eye * 16);
        if (!view.HasValue || !stored.HasValue || !inverse.HasValue) return;
        var (ndcX, gpuY) = XrJitter.ClipOffset(_xrJitterPixelX, _xrJitterPixelY, _xrJitterInputWidth,
            _xrJitterInputHeight, SystemInfo.graphicsUVStartsAtTop);
        var expected = Matrix4x4.Translate(new Vector3(ndcX, gpuY, 0f)) * _xrSavedProjection[eye] * view.Value;
        // Tolerances follow the evidence: the packed view-projection is a float32 round trip through the
        // mod's own array, and its inverse is a product of two float32 inverses, so the comparison is in
        // the unit that matters (input pixels) with a margin a real producer mismatch cannot hide behind.
        var derived = MaxDelta(expected, stored.Value);
        var roundTrip = MaxDelta(Matrix4x4.identity, stored.Value * inverse.Value);
        var unjittered = _xrSavedProjection[eye] * view.Value;
        var worstPixels = 0f;
        var measuredX = 0f;
        var measuredY = 0f;
        var measured = 0;
        for (var i = 0; i < 3; i++)
        {
            var local = new Vector3((i - 1) * 0.3f, (i % 2) * 0.2f, 1.5f + i);
            var world = camera.transform.localToWorldMatrix.MultiplyPoint3x4(local);
            if (!Project(unjittered, world, out var ax, out var ay)) continue;
            if (!Project(stored.Value, world, out var bx, out var by)) continue;
            var pixelX = (bx - ax) * _xrJitterInputWidth * 0.5f;
            var pixelY = -(by - ay) * _xrJitterInputHeight * 0.5f;
            measuredX += pixelX;
            measuredY += pixelY;
            measured++;
            worstPixels = Mathf.Max(worstPixels,
                Mathf.Max(Mathf.Abs(pixelX - _xrJitterPixelX), Mathf.Abs(pixelY - _xrJitterPixelY)));
        }
        if (measured > 0)
        {
            var measuredPixelX = measuredX / measured;
            var measuredPixelY = measuredY / measured;
            // The one line that would prove a sign or axis mistake: what was asked for versus what the
            // matrix the GPU consumes really does.
            var mismatch = Mathf.Max(Mathf.Abs(measuredPixelX - _xrJitterPixelX),
                Mathf.Abs(measuredPixelY - _xrJitterPixelY));
            if (mismatch > 0.05f && Time.frameCount - _xrMismatchFrame >= 600)
            {
                _xrMismatchFrame = Time.frameCount;
                _plugin.LogInfo($"XR jitter mismatches the packed matrix eye={eye} " +
                                   $"requested=({_xrJitterPixelX:F4},{_xrJitterPixelY:F4}) " +
                                   $"measured=({measuredPixelX:F4},{measuredPixelY:F4}); the packet " +
                                   "reports the requested value.");
            }
        }
        var ok = derived <= 2e-3f && roundTrip <= 2e-2f && worstPixels <= 0.05f;
        if (_xrVerify && !ok)
        {
            _plugin.LogError($"XR projection invariant failed eye={eye}: pixels={worstPixels:F4} " +
                             $"derived={derived:E2} inverse={roundTrip:E2} " +
                             $"delta=({_xrJitterPixelX:F4},{_xrJitterPixelY:F4})");
            return;
        }
        if (_xrVerify && Time.frameCount % 600 == 0)
            _plugin.LogInfo($"XR projection invariant ok eye={eye} pixels={worstPixels:F4} " +
                            $"derived={derived:E2} inverse={roundTrip:E2} " +
                            $"delta=({_xrJitterPixelX:F4},{_xrJitterPixelY:F4})");
    }

    /// <summary>Reads one 16-float column-major matrix block from the packed eye-matrix array.</summary>
    private static Matrix4x4? Block(float[] matrices, int offset)
    {
        if (offset < 0 || offset + 16 > matrices.Length) return null;
        var m = new Matrix4x4();
        for (var column = 0; column < 4; column++)
            for (var row = 0; row < 4; row++)
                m[row, column] = matrices[offset + column * 4 + row];
        return m;
    }

    /// <summary>Largest absolute element difference between two matrices.</summary>
    private static float MaxDelta(Matrix4x4 a, Matrix4x4 b)
    {
        var max = 0f;
        for (var row = 0; row < 4; row++)
            for (var column = 0; column < 4; column++)
                max = Mathf.Max(max, Mathf.Abs(a[row, column] - b[row, column]));
        return max;
    }

    /// <summary>Projects one world point to normalised device coordinates.</summary>
    private static bool Project(Matrix4x4 m, Vector3 point, out float x, out float y)
    {
        x = y = 0f;
        var clip = m * new Vector4(point.x, point.y, point.z, 1f);
        if (clip.w <= 1e-5f) return false;
        x = clip.x / clip.w;
        y = clip.y / clip.w;
        return true;
    }

    /// <summary>Throws away every XR-only fact whose camera identity changed.</summary>
    private void ResetXrIdentity()
    {
        _xrPrevCameraId = 0;
        _xrMotionLoggedCamera = 0;
        _xrJitterFrame = -1;
        _xrJitterAppliedFrame = -1;
        _xrExtentFrame = -1;
        _xrEye0Applied = false;
        _xrEye1Applied = false;
        _xrPassInventoryCamera = 0;
    }

    internal void MarkXrExtentHook(bool available) => _xrExtentAvailable = available;

    // XR reconstruction still needs depth and motion guides after the owned camera AA became None.
    // The static GetRenderPassInputs postfix cannot see the renderer, so the instance
    // OnRecordRenderGraph prefix publishes whether the graph being recorded belongs to the owned XR
    // camera. Restored by the matching finalizer on success and on exceptions.
    private bool _xrInputsRequested;

    /// <summary>Camera whose late passes and renderer features were last logged.</summary>
    private int _xrPassInventoryCamera;
    /// <summary>Set once the late-pass inspection failed, so its failure is logged once.</summary>
    private bool _xrInspectFailed;
    /// <summary>Proof window whose late-pass removal was last logged.</summary>
    private string _xrRemovalWindow = "";

    /// <summary>
    /// Marks the guide request for one OnRecordRenderGraph scope. Returns the previous value so a
    /// nested camera recording can mask this scope and the finalizer can restore it.
    /// </summary>
    internal bool BeginXrReconstructionInputs(UniversalRenderer renderer)
    {
        var previous = _xrInputsRequested;
        _xrInputsRequested = false;
        if (renderer == null || !_nativeReady || !_xrActive) return previous;
        if (_requestedUpscaler == UpscalerKind.Off) return previous;
        if (!_plan.OwnsRendering) return previous;
        var frameData = renderer.frameData;
        if (frameData == null) return previous;
        UniversalCameraData cameraData;
        try
        {
            cameraData = frameData.Get<UniversalCameraData>();
        }
        catch
        {
            return previous;
        }
        var camera = cameraData.camera;
        if (camera == null) return previous;
        if (camera.GetInstanceID() != _primaryCameraId) return previous;
        var xr = cameraData.xr;
        if (xr == null || !xr.enabled) return previous;
        _xrInputsRequested = true;
        InspectXrLatePasses(renderer);
        return previous;
    }

    /// <summary>
    /// Inventories the passes and renderer features that run after post-processing, and removes the
    /// late passes the proof window selects. Never throws, because it runs while a graph is recorded.
    /// </summary>
    private void InspectXrLatePasses(UniversalRenderer renderer)
    {
        // Neither the inventory nor the removal can be observed with Diagnostics and XrProof both off,
        // so the scan (and its per-frame lists) is skipped entirely in normal play.
        if (!_settings.Diagnostics.Value && !_xrVerify) return;
        try
        {
            var queue = renderer.activeRenderPassQueue;
            var latePasses = new List<ScriptableRenderPass>();
            if (queue != null)
            {
                for (var i = 0; i < queue.Count; i++)
                {
                    var pass = queue[i];
                    if (pass == null) continue;
                    if (pass.renderPassEvent >= RenderPassEvent.AfterRenderingPostProcessing)
                        latePasses.Add(pass);
                }
            }
            if (_xrPassInventoryCamera != _primaryCameraId && (_settings.Diagnostics.Value || _xrVerify))
            {
                var passes = new List<string>(latePasses.Count);
                for (var i = 0; i < latePasses.Count; i++)
                    passes.Add(i + "=" + DescribePass(latePasses[i]));
                _plugin.LogInfo(
                    $"XR late passes camera={_primaryCameraId} count={latePasses.Count}: {string.Join("; ", passes)}");
                var features = new List<string>();
                var rendererFeatures = renderer.rendererFeatures;
                if (rendererFeatures != null)
                {
                    for (var i = 0; i < rendererFeatures.Count; i++)
                    {
                        var feature = rendererFeatures[i];
                        if (feature == null) continue;
                        var entry =
                            $"{feature.GetIl2CppType().FullName}/'{feature.name}'/active={feature.isActive}";
                        var fullscreen = feature.TryCast<FullScreenPassRendererFeature>();
                        if (fullscreen != null)
                            entry +=
                                $" [ fullscreen injection={fullscreen.injectionPoint} fetchColor={fullscreen.fetchColorBuffer} requirements={fullscreen.requirements} shader={fullscreen.passMaterial?.shader?.name ?? "none"} pass={fullscreen.passIndex}]";
                        features.Add(entry);
                    }
                }
                _plugin.LogInfo($"XR renderer features camera={_primaryCameraId}: {string.Join("; ", features)}");
                _xrPassInventoryCamera = _primaryCameraId;
            }
            _proof.SetLatePassCount(latePasses.Count);
            var mode = _proof.LatePassRemoval;
            if (mode != -2)
            {
                var removed = RemoveLatePasses(renderer, mode);
                var window = _proof.WindowName;
                if (_xrRemovalWindow != window)
                {
                    _xrRemovalWindow = window;
                    _plugin.LogInfo(
                        $"XR proof: removed {removed.Count} late pass(es) for window {window}: {removed.Descriptions}");
                }
            }
        }
        catch (Exception exception)
        {
            NoteXrInspectFailure(exception);
        }
    }

    /// <summary>Logs the late-pass inspection failure once per launch.</summary>
    private void NoteXrInspectFailure(Exception exception)
    {
        if (_xrInspectFailed) return;
        _xrInspectFailed = true;
        _plugin.LogInfo($"XR late-pass inspection failed: {exception.GetType().Name}: {exception.Message}");
    }

    /// <summary>
    /// Removes the late passes the given mode selects (-1 all, k &gt;= 0 only late pass k) and returns
    /// the removed count with their descriptions in queue order.
    /// </summary>
    private (int Count, string Descriptions) RemoveLatePasses(UniversalRenderer renderer, int mode)
    {
        var queue = renderer.activeRenderPassQueue;
        if (queue == null) return (0, "");
        var late = 0;
        for (var i = 0; i < queue.Count; i++)
        {
            var pass = queue[i];
            if (pass != null && pass.renderPassEvent >= RenderPassEvent.AfterRenderingPostProcessing) late++;
        }
        if (mode >= 0 && mode >= late) return (0, "");
        var removed = new List<string>();
        var index = late - 1;
        for (var i = queue.Count - 1; i >= 0; i--)
        {
            var pass = queue[i];
            if (pass == null) continue;
            if (pass.renderPassEvent < RenderPassEvent.AfterRenderingPostProcessing) continue;
            if (mode < 0 || index == mode)
            {
                removed.Insert(0, DescribePass(pass));
                queue.RemoveAt(i);
            }
            index--;
        }
        return (removed.Count, string.Join("; ", removed));
    }

    /// <summary>Describes one render pass by event, type and pass name; falls back to the event only.</summary>
    private string DescribePass(ScriptableRenderPass pass)
    {
        try
        {
            return $"{pass.renderPassEvent}({(int)pass.renderPassEvent}) {pass.GetIl2CppType().FullName} '{pass.passName}'";
        }
        catch (Exception exception)
        {
            NoteXrInspectFailure(exception);
            return $"{pass.renderPassEvent}({(int)pass.renderPassEvent})";
        }
    }

    /// <summary>
    /// Called from the GetRenderPassInputs postfix. Asks URP to allocate depth and motion vectors for
    /// the owned XR camera without enabling Unity TAA, which would resolve the image again.
    /// </summary>
    internal void RequestXrReconstructionInputs(ref UniversalRenderer.RenderPassInputSummary summary)
    {
        if (!_xrInputsRequested) return;
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

    internal void EndXrReconstructionInputs(bool previousRequested) => _xrInputsRequested = previousRequested;

    /// <summary>Latches a real failure from the guide-request path without blaming frame generation.</summary>
    internal void ReportXrInputFailure(Exception exception)
    {
        var reason = "the XR temporal input request failed: " + exception.GetType().Name + ": " +
                     exception.Message;
        if (_lastFailure == reason) return;
        _lastFailure = reason;
        _plugin.LogError("BigScale cannot render: " + reason);
        _plan.Fail(reason);
    }

    /// <summary>
    /// Postfix on URP's XR camera-data pass. <c>cameraTargetDescriptor</c> is the per-eye intermediate
    /// extent there, so this is where the reconstruction input size is declared. The eye textures
    /// themselves keep their SteamVR size because <c>asset.renderScale</c> is never written in XR.
    /// </summary>
    internal void OnXrCameraData(UniversalCameraData cameraData)
    {
        if (!_nativeReady || !_xrActive || cameraData == null) return;
        var camera = cameraData.camera;
        if (camera == null) return;
        if (camera.GetInstanceID() != _primaryCameraId) return;
        if (!_plan.OwnsRendering || _fit is not { Eyes.Length: > 0 } fit) return;
        var xr = cameraData.xr;
        if (xr == null || !xr.enabled || !xr.singlePassEnabled) return;
        // The fit was resolved against the previous frame's output extent. Any other value means a
        // resize is in progress; the replacement path refits before touching this camera again.
        if (cameraData.pixelWidth != _fitWidth || cameraData.pixelHeight != _fitHeight) return;
        var eye = fit.Eyes[0];
        if (eye.Width <= 0 || eye.Height <= 0 || eye.Width > cameraData.pixelWidth) return;
        var descriptor = cameraData.cameraTargetDescriptor;
        descriptor.width = eye.Width;
        descriptor.height = eye.Height;
        descriptor.useDynamicScale = false;
        cameraData.cameraTargetDescriptor = descriptor;
        cameraData.scaledWidth = eye.Width;
        cameraData.scaledHeight = eye.Height;
        // URP derives several sizes from pixelWidth * renderScale (for example the GPU occlusion
        // depth size), so the scalar must describe the same reduced extent as the descriptor. The
        // image scaling mode is deliberately left as URP computed it: declaring Upscaling here routes
        // XR post through the non-linear final-pass branch, whose transfer never reaches the eye
        // textures in this build. BigScale performs the upscale; URP keeps its linear FinalBlitPass.
        cameraData.renderScale = eye.Width / (float)cameraData.pixelWidth;
        _xrExtentFrame = Time.frameCount;
    }

    /// <summary>
    /// Publishes the mono non-jittered view-projection and its predecessor while the VR mod's
    /// non-stereo shaders own the raster. Those shaders read <c>_NonJitteredViewProjMatrix</c> and
    /// <c>_PrevViewProjMatrix</c>, which URP only supplies as stereo arrays here.
    /// </summary>
    internal void OnVrCameraRendered(ScriptableRenderContext context, Camera camera)
    {
        // These globals are the only whole matrices this mod writes before rasterisation; without
        // them the motion-vector pass emits a zero field and BigScale has no camera motion to reproject.
        if (camera == null || !_nativeReady) return;
        if (!_plan.OwnsRendering) return;
        if (camera.GetInstanceID() != _primaryCameraId) return;
        Matrix4x4 current;
        try
        {
            current = GL.GetGPUProjectionMatrix(camera.projectionMatrix, true) * camera.worldToCameraMatrix;
        }
        catch
        {
            return;
        }
        _xrArrayProof.Poll();
        _xrArrayProof.Execute(context);
        var previous = _xrPrevCameraId == _primaryCameraId ? _xrPrevViewProj : current;
        try
        {
            _xrMotionCommands ??= new CommandBuffer { name = "BigScale XR motion matrices" };
            _xrMotionCommands.Clear();
            _xrMotionCommands.SetGlobalMatrix("_NonJitteredViewProjMatrix", current);
            _xrMotionCommands.SetGlobalMatrix("_PrevViewProjMatrix", previous);
            context.ExecuteCommandBuffer(_xrMotionCommands);
        }
        catch (Exception exception)
        {
            if (_lastFailure != "XR motion matrices")
            {
                _lastFailure = "XR motion matrices";
                _plugin.LogError("XR motion matrices could not be published: " + exception.Message);
            }
            return;
        }
        _xrPrevViewProj = current;
        if (_xrPrevCameraId != _primaryCameraId)
        {
            _xrPrevCameraId = _primaryCameraId;
            _xrMotionLoggedCamera = _primaryCameraId;
            _plugin.LogInfo($"XR motion matrices published for camera id={_primaryCameraId} (mono, non-jittered).");
        }
    }

    /// <summary>
    /// Runs after the graph is submitted and before the XR mirror/submit, so the eye array it samples
    /// is this frame's finished image.
    /// </summary>
    internal void OnEndCameraRendering(ScriptableRenderContext context, Camera camera)
    {
        if (!_xrActive || camera == null) return;
        if (camera.GetInstanceID() != _primaryCameraId) return;
        _xrArrayProof.ExecuteEndCamera(context);
    }

    private void CommitNow(Camera camera, UniversalAdditionalCameraData additional, string source)
    {
        var fit = _fit;
        if (fit == null)
        {
            _plugin.LogInfo("commit deferred: no resolved size fit yet.");
            return;
        }
        var asset = GraphicsSettings.currentRenderPipeline?.TryCast<UniversalRenderPipelineAsset>();
        if (asset == null)
        {
            _plan.Fail("no UniversalRenderPipelineAsset is active");
            RestoreNow("commit without a pipeline asset");
            return;
        }
        var cameraId = camera.GetInstanceID();
        // In an XR session URP forwards renderScale to XRSystem.SetRenderScale, which resizes the
        // SteamVR eye textures; there the scale stays scene-owned and only the reduced intermediate
        // is this mod's to size (see OnXrCameraData).
        _ownership.Acquire(asset, camera, additional, fit.EffectiveScale, !_xrActive, !_xrActive);
        BigVisualsCompatibility.DeferScaleWrites = !_xrActive;
        BigVisualsCompatibility.OwnedCamera = camera;
        // The expected eyes are declared when the first packet set is built, so a single-eye frame
        // can never be declared complete before the second eye exists.
        _history.AdvanceGeneration(cameraId);
        _plan.CommitDone();
        var ownedAntialiasing = _xrActive ? AntialiasingMode.None : AntialiasingMode.TemporalAntiAliasing;
        if (_xrActive && fit.Eyes.Length > 0)
        {
            _plugin.LogInfo($"committed at the {source} boundary: camera={camera.name} id={cameraId} " +
                            $"XR render extent={fit.Eyes[0].Width}x{fit.Eyes[0].Height} (SteamVR eye textures " +
                            $"untouched) msaa=1 aa={ownedAntialiasing} depth=true " +
                            $"upscalingFilter={asset.upscalingFilter} (untouched).");
        }
        else
        {
            _plugin.LogInfo($"committed at the {source} boundary: camera={camera.name} id={cameraId} " +
                            $"scale={fit.EffectiveScale:F4} msaa=1 aa={ownedAntialiasing} depth=true " +
                            $"upscalingFilter={asset.upscalingFilter} (untouched).");
        }
    }

    private void RestoreNow(string reason)
    {
        if (!_ownership.Owned && _fit == null)
        {
            // Nothing was written, so there is nothing to give back.
            BigVisualsCompatibility.DeferScaleWrites = false;
            BigVisualsCompatibility.OwnedCamera = null;
            return;
        }
        _ownership.Restore();
        BigVisualsCompatibility.DeferScaleWrites = false;
        BigVisualsCompatibility.OwnedCamera = null;
        _compatibility.Reapply();
        _history.Clear();
        _output.Release(false);
        _fit = null;
        _fitWidth = 0;
        _fitHeight = 0;
        _modeCache.Clear();
        _retired.Clear();
        _plugin.LogInfo($"{UpscalingModes.ShortName(_selectedUpscaler)} released rendering: " + reason);
    }

    /// <summary>
    /// Drops every derived size/activity value. Called when the provider, preset or output extent
    /// changes, so an old frame can never be presented as the current conversion.
    /// </summary>
    private void ClearFit(string reason)
    {
        _fit = null;
        _fitWidth = 0;
        _fitHeight = 0;
        _modeCache.Clear();
        _announcedActive = false;
        if (reason.Length > 0) _plugin.LogInfo("fit cleared: " + reason + ".");
    }

    private static string NativeFailure(string prefix, Native.Status status)
    {
        var reason = status.BackendReasonText();
        var code = $"error {status.LastError}, result 0x{status.LastResult:x8}";
        return string.IsNullOrEmpty(reason) ? $"{prefix} ({code})" : $"{prefix}: {reason} [{code}]";
    }

    /// <summary>Per-frame bookkeeping on the main thread.</summary>
    internal void Tick()
    {
        // VR evidence is process-wide and must be current before any early return below: the
        // settings page and frame generation both lock to it even when the renderer is not ready.
        _xrActive = VrState.Active;
        // The player's own renderer state follows the master switch, exactly like ownership does:
        // disabling the mod must restore the game's material lists and motion-vector modes, not leave
        // them corrected. XR stays excluded because the motion material disables XRMotionVectors.
        _playerMotion.Tick(_settings.Enabled.Value && !_xrActive);
        _pass.AbandonStaleLeases(Time.frameCount);
        ConsumeAbandonedSwitches();
        _output.Collect();

        // A render callback cannot mutate coordinator policy, so it publishes a failure string that
        // is consumed here, on the owning thread.
        var renderFailure = _pass.TakeFailure();
        if (renderFailure != null && _nativeReady && _plan.State != OwnershipState.Faulted)
        {
            _plan.Fail(renderFailure);
            _lastFailure = renderFailure;
            _plugin.LogError("BigScale cannot render: " + renderFailure);
        }

        if (!_nativeReady)
        {
            // A pre-native tick fault has no ownership to restore; an explicit settings transition
            // clears it instead of leaving the panel faulted for the rest of the session.
            if (_state == RendererState.Faulted && _plan.State != OwnershipState.Faulted)
            {
                _state = RendererState.Waiting;
                _stateReason = _plan.Reason;
            }
            return;
        }

        if (_recoveryPending && Time.frameCount - _recoveryRequestedFrame > 240)
        {
            _recoveryPending = false;
            _plugin.LogError("the recovery drain did not complete; BigScale stays faulted until the next settings change.");
        }

        // Master enablement is independent of the remembered provider. Disabling maps to the Off
        // wire identity without rewriting the selection, and frame generation follows it.
        var enabled = _settings.Enabled.Value;
        var requested = enabled ? _settings.Upscaler.Value : UpscalerKind.Off;
        if (requested != _requestedUpscaler)
        {
            _requestedUpscaler = requested;
            _switchPending = true;
            _switchAcked = false;
            _switchLeaseGeneration = 0;
            _switchQueuedLogged = false;
            ClearFit(enabled ? "the upscaler changed" : "master disabled");
            _retired.Clear();
            if (requested == UpscalerKind.Off)
            {
                // No native image work exists to drain for a master disable; ownership is restored at
                // the next boundary and the remembered provider stays in the settings.
                _switchPending = false;
                _selectedUpscaler = UpscalerKind.Off;
                _plan.Disable("master disabled");
                if (_state != RendererState.Disabled)
                {
                    _state = RendererState.Disabled;
                    _stateReason = "master disabled";
                }
            }
            else
            {
                _plan.Wait("switching the upscaler to " + UpscalingModes.Name(requested));
            }
            _plugin.LogInfo(enabled
                ? $"upscaler requested: {UpscalingModes.Name(requested)}."
                : "master disabled; the selected providers are preserved.");
        }

        // The watchdog exists only after a control pass actually reserved a lease. Before that the
        // title/loading screen may simply have no eligible camera, which is not a fault.
        if (_switchPending && _switchAcked &&
            Time.realtimeSinceStartup - _switchReservedAt > 10f)
        {
            // Never wedge in a switch: an acknowledgement that never retires is a renderer fault.
            _switchPending = false;
            _switchAcked = false;
            _plan.Fail("the upscaler switch did not complete");
        }

        var status = ReadStatus(0, 0, (int)_requestedUpscaler);
        ProcessCompletions(status);

        // Frame generation follows master enablement independently of super resolution; its rows
        // still report truthfully while SR is Off, faulted or unsupported. The reconstruction output is
        // its only HUD-less source, so the capture is told whether one can exist at all.
        _frameGeneration.SetReconstructionActive(requested != UpscalerKind.Off);
        _frameGeneration.Tick(_settings, _xrActive, SettingsPanel.AnyVisible);
        // The VR generator is inserted at the compositor, so it follows the same enablement and needs
        // the upscaler to be producing the depth and motion it interpolates from.
        _vrFrameGeneration.Tick(_settings, _xrActive, _state == RendererState.Active);
        if (_proof.Enabled)
        {
            if (!_xrActive || !_plan.OwnsRendering) _proof.Abort("XR ownership is not active");
            _proof.Tick(_state == RendererState.Active);
        }

        if (status.State == Native.StateShutdown)
        {
            // The bridge proved the GPU idle before publishing shutdown, so nothing is in flight.
            _output.Release(true);
            return;
        }

        if (requested == UpscalerKind.Off)
        {
            if (_state != RendererState.Disabled)
            {
                _plan.Disable(enabled ? "the upscaler is Off" : "master disabled");
                _state = RendererState.Disabled;
                _stateReason = enabled ? "the upscaler is Off" : "master disabled";
                _retired.Clear();
            }
            return;
        }

        if (status.State == Native.StateFaulted)
        {
            if (_plan.State != OwnershipState.Faulted)
                _plan.Fail(NativeFailure("native renderer fault", status));
        }
        else if (status.State == Native.StateUnsupported && _plan.State != OwnershipState.Unsupported)
        {
            _plan.Unsupported(NativeFailure("the selected upscaler is unavailable", status));
        }

        // A destroyed or disabled primary camera is detected from the live object on the next
        // eligible graph; it does not need a multi-second disappearance wait here.
        if (_primaryCameraId != 0 && !PrimaryAlive())
        {
            ReleasePrimary("the primary camera is no longer live");
        }

        UpdateDisplayState(status);
        LogDiagnosticSummary(status);
        // Every latching path (native result, completion, recording pass, render tick) ends here;
        // log the exact reason once instead of once per frame.
        if (_plan.State == OwnershipState.Faulted && _plan.Reason != _lastFailure)
        {
            _lastFailure = _plan.Reason;
            _plugin.LogError("BigScale cannot render: " + _plan.Reason);
        }
    }

    /// <summary>
    /// Refreshes the frame-generation row from the cached native snapshot. Called synchronously
    /// when the panel opens or closes so a cached Active label never outlives the suspension.
    /// </summary>
    internal void RefreshFrameGenerationStatus(bool settingsOpen)
    {
        _frameGeneration.RefreshStatusLine(settingsOpen);
    }

    private bool PrimaryAlive() => _primaryCamera != null && _primaryCamera.isActiveAndEnabled;

    /// <summary>
    /// One aggregate line per ten seconds of monotonic time, and only when diagnostics is enabled.
    /// Ordinary frames format no telemetry strings.
    /// </summary>
    private void LogDiagnosticSummary(Native.Status status)
    {
        if (!_settings.Diagnostics.Value)
        {
            _diagnosticWindowStart = 0f;
            return;
        }
        var now = Time.realtimeSinceStartup;
        if (_diagnosticWindowStart <= 0f) _diagnosticWindowStart = now;
        if (now - _diagnosticWindowStart < 10f) return;
        _plugin.LogInfo($"upscaling summary: executed={_diagnosticExecuted} retired={_diagnosticRetired} " +
                        $"evaluations={status.TotalEvaluations} state={_state} provider={_selectedUpscaler} " +
                        $"preset={_settings.Preset.Value}.");
        _diagnosticExecuted = 0;
        _diagnosticRetired = 0;
        _diagnosticWindowStart = now;
    }

    private void ConsumeAbandonedSwitches()
    {
        while (_pass.TakeAbandonedSwitch(out var upscaler, out var leaseGeneration))
        {
            // A notice only cancels the reservation it belongs to; a late superseded cancellation
            // must never cancel a newer switch.
            if (!_switchPending || !_switchAcked) continue;
            if (leaseGeneration != _switchLeaseGeneration || upscaler != _requestedUpscaler) continue;
            _switchAcked = false;
            _switchLeaseGeneration = 0;
            _plan.Wait("waiting to record the upscaler switch");
            _plugin.LogInfo("the queued upscaler switch was never executed; it will be recorded again.");
        }
    }

    private void UpdateDisplayState(Native.Status status)
    {
        _stateReason = _plan.Reason;
        _state = _plan.State switch
        {
            OwnershipState.Owned => RendererState.Waiting,
            OwnershipState.Faulted => RendererState.Faulted,
            OwnershipState.Disabled => RendererState.Disabled,
            OwnershipState.Unsupported => RendererState.Unsupported,
            _ => RendererState.Waiting,
        };
        if (!_plan.OwnsRendering) return;

        var cameraId = _primaryCameraId;
        if (cameraId == 0) return;
        if (_switchPending)
        {
            _state = RendererState.Waiting;
            _stateReason = "waiting for the upscaler switch to retire";
            return;
        }
        // Only a fully retired frame from the selected provider and preset proves activity.
        if (!_history.TryGetRetiredFrame(cameraId, _retired) ||
            !UpscalingStatus.Matches(_retired, _selectedUpscaler, _settings.Preset.Value))
        {
            _state = RendererState.Waiting;
            _stateReason = "waiting for the first fully retired frame from " +
                           UpscalingModes.ShortName(_selectedUpscaler);
            return;
        }
        if (Time.frameCount - _lastActiveFrame > 120 && _state == RendererState.Active)
        {
            _state = RendererState.Waiting;
            _stateReason = "no fully retired evaluation in the last 120 frames";
            return;
        }
        var age = Time.frameCount - _retired.FrameIndex;
        if (age > 4)
        {
            _state = RendererState.Waiting;
            _stateReason = $"last fully completed evaluation was frame {_retired.FrameIndex}";
            return;
        }

        _state = RendererState.Active;
        _lastActiveFrame = Time.frameCount;
        var eyes = _history.ExpectedEyes(cameraId);
        _stateReason = status.Layout switch
        {
            Native.LayoutArray => $"stereo array, all {eyes} eye(s) retired",
            Native.LayoutPacked => $"multipass views, all {eyes} eye(s) retired",
            _ => "single view retired",
        };
        if (_announcedActive) return;
        _announcedActive = true;
        _plugin.LogInfo($"{UpscalingModes.ShortName(_retired.Upscaler)} active after a fully retired frame: " +
                        $"camera={cameraId} " +
                        $"input={_retired.Eyes[0].InputWidth}x{_retired.Eyes[0].InputHeight} " +
                        $"output={_retired.Eyes[0].OutputWidth}x{_retired.Eyes[0].OutputHeight} " +
                        $"preset={_retired.Preset} views={status.ViewCount} " +
                        $"completions={status.TotalEvaluations} runtime=0x{status.RuntimeVersion:x8} " +
                        $"provider=0x{status.ProviderVersionId:x16}.");
    }

    /// <summary>
    /// Consumes the retained completion snapshot and acknowledges exactly what it processed.
    /// History is only ever updated here, so the latch, lifetime and panel state stay on one owner.
    /// </summary>
    private void ProcessCompletions(Native.Status status)
    {
        var count = (int)Math.Min(status.CompletionPublished, (uint)Native.CompletionCapacity);
        if (count == 0) return;

        uint highest = 0;
        var retiredProbe = false;
        for (var i = 0; i < count; i++)
        {
            var completion = status.CompletionAt(i);
            if (completion.Sequence == 0 || completion.Sequence <= _lastAcknowledgedSequence) continue;
            highest = Math.Max(highest, completion.Sequence);
            var ok = completion.Result == 0;
            // A stale packet is an ordered control artefact, not a fault of the new provider: its
            // work was retired without executing and must keep settling the resource tag.
            var stale = completion.Result == Native.ErrorStalePacket;
            if (completion.Kind == Native.CompletionRetired && completion.ResourceTag != 0)
                _output.Completed(completion.ResourceTag);
            if (completion.Operation == Native.OperationSwitch)
            {
                CompleteSwitch(completion, ok);
                continue;
            }
            if (completion.Operation is Native.OperationProbe or Native.OperationRecovery)
            {
                if (completion.Operation == Native.OperationProbe)
                {
                    if (completion.Kind == Native.CompletionRetired && ok) retiredProbe = true;
                    else if (!ok && !stale) _plan.Fail($"capability probe failed (result 0x{completion.Result:x8})");
                }
                else if (ok) CompleteRecovery();
                continue;
            }

            if (completion.Kind == Native.CompletionRetired)
                _history.RecordRetirement(completion.CameraId, completion.Eye, completion.FrameIndex,
                    completion.HistoryEpoch, ok, (UpscalerKind)completion.Upscaler,
                    (UpscalingPreset)completion.Preset, completion.InputWidth, completion.InputHeight,
                    completion.OutputWidth, completion.OutputHeight);
            else
                _history.RecordExecution(completion.CameraId, completion.Eye, completion.FrameIndex,
                    completion.HistoryEpoch, ok);

            if (completion.Kind == Native.CompletionRetired) _diagnosticRetired++;
            else _diagnosticExecuted++;
            if (ok || stale || completion.CameraId != _primaryCameraId) continue;
            _plan.Fail($"native completion failed for eye {completion.Eye} (result 0x{completion.Result:x8})");
        }

        if (highest != 0)
        {
            _lastAcknowledgedSequence = highest;
            Native.AckCompletions(highest);
        }

        if (retiredProbe && !_probeExecutedLogged)
        {
            _probeExecutedLogged = true;
            _plugin.LogInfo("capability probe executed on the render thread.");
        }
    }

    /// <summary>
    /// A retired switch is the only event that advances the selected provider. A queued request that
    /// changed again mid-flight simply stays pending and is re-issued on the next frame.
    /// </summary>
    private void CompleteSwitch(Native.Completion completion, bool ok)
    {
        if (completion.Kind != Native.CompletionRetired) return;
        if (!ok)
        {
            _switchPending = false;
            _switchAcked = false;
            _switchLeaseGeneration = 0;
            _plan.Fail($"the upscaler switch failed (result 0x{completion.Result:x8})");
            return;
        }
        _switchAcked = false;
        var switched = (UpscalerKind)completion.Upscaler;
        _selectedUpscaler = switched;
        _switchPending = switched != _requestedUpscaler;
        if (!_switchPending) _switchLeaseGeneration = 0;
        ClearFit("the upscaler switch retired");
        _retired.Clear();
        if (_primaryCameraId != 0) _history.AdvanceGeneration(_primaryCameraId);
        if (switched == UpscalerKind.Off)
            _plan.Disable("the upscaler is Off");
        else
            _plan.Enable("upscaler switched to " + UpscalingModes.Name(switched));
        _plugin.LogInfo($"upscaler switch retired: active={UpscalingModes.Name(switched)}.");
    }

    /// <summary>The bridge reported that the recovery drain finished.</summary>
    private void CompleteRecovery()
    {
        if (!_recoveryPending) return;
        _recoveryPending = false;
        ClearFit("recovery");
        _retired.Clear();
        _lastFailure = null;
        _probeQueuedLogged = false;
        _probeExecutedLogged = false;
        _history.Clear();
        _plan.Enable("recovery drain completed");
        _plugin.LogInfo("recovery drain completed; re-probing the capability set.");
    }

    /// <summary>Called from the Harmony prefix. Returns true to run URP's original post-processing.</summary>
    internal bool OnPostProcess(PostProcessPassRenderGraph pass, RenderGraph graph, ContextContainer frameData,
        ref TextureHandle activeCameraColorTexture, ref TextureHandle lutTexture, ref TextureHandle overlayTexture,
        ref TextureHandle postProcessingTarget, bool hasFinalPass, bool resolveToDebugScreen,
        bool enableColorEndingIfNeeded, out int xrScopeToken)
    {
        xrScopeToken = 0;
        // The substituted chain re-enters this patched method with the reconstruction as its source.
        // That inner invocation executes URP's stock body: it already carries the right operands.
        if (_xrScopeReplaying) return true;
        if (!_nativeReady) return true;

        var cameraData = frameData.Get<UniversalCameraData>();
        if (cameraData.camera == null) return true;
        if (!Eligible(cameraData, out var ineligible)) { LogSkip(cameraData, ineligible); return true; }

        var area = (long)cameraData.pixelWidth * cameraData.pixelHeight;
        var camera = cameraData.camera;
        var candidateId = camera.GetInstanceID();
        // Adoption and promotion happen only here, after the camera proved it is an eligible
        // rendered base camera. Pre-camera callbacks never choose the primary.
        if (PrimaryCameraPolicy.ShouldPromote(_primaryCameraId, PrimaryAlive(), _primaryArea,
                _lastPrimarySeenFrame, candidateId, area, Time.frameCount))
        {
            Promote(camera, area);
            return true;
        }
        var cameraId = _primaryCameraId;
        if (candidateId != cameraId)
        {
            LogSkip(cameraData, "not the primary camera");
            return true;
        }
        _primaryCamera = camera;
        _primaryArea = area;
        _lastPrimarySeenFrame = Time.frameCount;

        var asset = GraphicsSettings.currentRenderPipeline?.TryCast<UniversalRenderPipelineAsset>();
        if (asset == null) return true;
        if (_ownership.Owned && !_ownership.IsAsset(asset))
        {
            _plan.Reacquire("the pipeline asset was replaced");
            return true;
        }

        var resourceData = frameData.Get<UniversalResourceData>();
        var color = activeCameraColorTexture.IsValid() ? activeCameraColorTexture : resourceData.activeColorTexture;
        if (!color.IsValid()) return true;

        if (_recoveryPending)
        {
            // The drain is handled on the render thread; nothing else may run until it reports back.
            ProbeRecovery(graph, cameraData, color);
            return true;
        }

        if (_switchPending && !_switchAcked)
        {
            // One ordered control packet per requested value. It drains the old provider before any
            // packet of the new one can exist. The watchdog starts only when this actually reserved
            // a control lease.
            if (_pass.RecordSwitch(graph, cameraData, color, _renderEvent, _requestedUpscaler,
                    out var leaseGeneration))
            {
                _switchAcked = true;
                _switchLeaseGeneration = leaseGeneration;
                _switchReservedAt = Time.realtimeSinceStartup;
                if (!_switchQueuedLogged)
                {
                    _switchQueuedLogged = true;
                    _plugin.LogInfo($"upscaler switch queued: {UpscalingModes.Name(_requestedUpscaler)}.");
                }
            }
            return true;
        }

        if (_requestedUpscaler == UpscalerKind.Off) return true;
        if (_plan.State is OwnershipState.Faulted or OwnershipState.Disabled or OwnershipState.Unsupported)
            return true;

        var outputWidth = cameraData.pixelWidth;
        var outputHeight = cameraData.pixelHeight;
        if (outputWidth <= 0 || outputHeight <= 0) return true;

        // An XR camera is additional evidence, never a veto: a desktop mirror camera inside a VR
        // process must not be able to erase the process-wide verdict.
        if (cameraData.xr != null && cameraData.xr.enabled) _xrActive = true;
        var colorDescriptor = color.GetDescriptor(graph);
        var slices = Mathf.Max(1, colorDescriptor.slices);
        var viewCount = ViewCount(cameraData);
        var layout = DetermineLayout(cameraData, colorDescriptor, viewCount);

        if (_fit == null || _fitWidth != outputWidth || _fitHeight != outputHeight)
        {
            var eyeCount = ViewIdentity.ExpectedEyes(layout);
            var ranges = GetModeRanges(outputWidth, outputHeight, _selectedUpscaler);
            if (ranges == null)
            {
                Probe(graph, cameraData, color, "querying upscaler capability and supported sizes");
                return true;
            }
            for (var i = 0; i < eyeCount; i++)
            {
                _eyeWidths[i] = outputWidth;
                _eyeHeights[i] = outputHeight;
                _eyeRanges[i] = ranges;
            }
            // NativeAA renders at the full eye extent and then upscales by one: in VR that costs the
            // whole native raster plus a native-resolution BigScale pass and recovers no detail the eye
            // texture does not already have. Quality is the smallest supported XR reduction.
            var preset = _settings.Preset.Value;
            if (cameraData.xr is { enabled: true } && preset == UpscalingPreset.NativeAA)
            {
                preset = UpscalingPreset.Quality;
                LogOnce("NativeAA needs the full eye raster and upscales nothing in VR; using Quality.");
            }
            var fit = UpscalingSizing.Fit(preset,
                _eyeWidths[..eyeCount], _eyeHeights[..eyeCount], _eyeRanges[..eyeCount]);
            if (!fit.Ok)
            {
                _plan.Unsupported(fit.Reason);
                _fit = null;
                _plugin.LogError($"{UpscalingModes.ShortName(_selectedUpscaler)} cannot be configured: " +
                                 $"{fit.Reason} (preset {_settings.Preset.Value}, " +
                                 $"output {outputWidth}x{outputHeight}).");
                return true;
            }
            _fit = fit;
            _fitWidth = outputWidth;
            _fitHeight = outputHeight;
            if (_history.Generation(cameraId) != 0) _history.AdvanceGeneration(cameraId);
            _plugin.LogInfo($"resolved {fit.Preset} effective={fit.EffectiveScale:F4} clamped={fit.Clamped} " +
                            $"eyes={string.Join(" ", Array.ConvertAll(fit.Eyes, size => size.ToString()))} " +
                            $"output={outputWidth}x{outputHeight} layout={layout} views={viewCount} " +
                            $"slices={slices} provider={UpscalingModes.ShortName(_selectedUpscaler)}.");
        }

        if (!_plan.OwnsRendering)
        {
            if (_fit != null) _plan.RequestCommit();
            Probe(graph, cameraData, color, "committing the resolved scene scale at the next camera boundary");
            return true;
        }

        // In XR the reduced input exists only through the camera-data hook: URP's own render scale is
        // never written, so without it the camera renders (and the fit describes) the full eye extent.
        var ownedFit = _fit;
        if (ownedFit == null) return true;
        if (cameraData.xr is { enabled: true })
        {
            if (!_vrJitterHookOk)
            {
                var reason = "the VR eye-matrix hook is unavailable: " + _vrJitterHookReason;
                if (_lastFailure != reason)
                {
                    _lastFailure = reason;
                    _plugin.LogError("BigScale cannot render: " + reason);
                }
                _plan.Unsupported(reason);
                return true;
            }
            if (!_xrExtentAvailable)
            {
                const string unsupported = "the XR render extent hook is unavailable in this URP build";
                if (_lastFailure != unsupported)
                {
                    _lastFailure = unsupported;
                    _plugin.LogError("BigScale cannot render: " + unsupported);
                }
                _plan.Unsupported(unsupported);
                return true;
            }
            if (_xrExtentFrame != Time.frameCount)
                return Probe(graph, cameraData, color, "applying the XR render extent at the next camera");
            // URP's own orchestration stays in charge of every effect and of the presentation
            // transfer. BigScale is substituted only at the UberPost seam, from the HDR image URP really
            // hands that pass.
            return ArmXrPostScope(pass, graph, frameData, cameraData, cameraId, layout, slices, outputWidth,
                outputHeight, colorDescriptor, ownedFit, color, lutTexture, overlayTexture,
                postProcessingTarget, hasFinalPass, resolveToDebugScreen, enableColorEndingIfNeeded,
                out xrScopeToken);
        }

        return Replace(graph, pass, frameData, resourceData, cameraData, ref color, colorDescriptor,
            ref lutTexture, ref overlayTexture, ref postProcessingTarget, hasFinalPass, resolveToDebugScreen,
            enableColorEndingIfNeeded, cameraId, layout, slices, viewCount, outputWidth, outputHeight);
    }

    /// <summary>
    /// Arms the per-invocation XR scope, records the reconstruction and runs URP's own post chain with
    /// the reconstruction as its active colour, so every effect samples a full-size image at 1:1.
    /// Returns false when the chain already ran: URP's arguments are <c>in</c> handles, so a prefix
    /// cannot rewrite them in place, only supply different ones to a re-entrant call.
    /// </summary>
    private bool ArmXrPostScope(PostProcessPassRenderGraph pass, RenderGraph graph, ContextContainer frameData,
        UniversalCameraData cameraData, int cameraId, int layout, int slices, int outputWidth, int outputHeight,
        TextureDesc colorDescriptor, SizeFit fit, TextureHandle activeColor, TextureHandle lutTexture,
        TextureHandle overlayTexture, TextureHandle target, bool hasFinalPass, bool resolveToDebugScreen,
        bool enableColorEndingIfNeeded, out int token)
    {
        token = 0;
        if (_xrScopeToken != 0)
        {
            // Never overwrite an enclosing scope: a nested invocation keeps the stock path and the
            // finalizer still clears only the scope it was given.
            LogOnce("an owned XR post scope is already active; this invocation keeps URP's stock path.");
            return true;
        }
        int activeWidth;
        int activeHeight;
        try
        {
            var descriptor = cameraData.cameraTargetDescriptor;
            activeWidth = descriptor.width;
            activeHeight = descriptor.height;
        }
        catch
        {
            return true;
        }
        var passPointer = pass.Pointer;
        var graphPointer = graph.Pointer;
        var frameDataPointer = frameData.Pointer;
        _xrScopeTokens++;
        if (_xrScopeTokens == 0) _xrScopeTokens = 1;
        _xrScopePass = passPointer;
        _xrScopeGraph = graphPointer;
        _xrScopeFrameData = frameDataPointer;
        _xrScopeCameraId = cameraId;
        _xrScopeInputWidth = activeWidth;
        _xrScopeInputHeight = activeHeight;
        _xrScopeOutputWidth = outputWidth;
        _xrScopeOutputHeight = outputHeight;
        _xrScopeLayout = layout;
        _xrScopeSlices = slices;
        _xrScopeDimension = colorDescriptor.dimension;
        _xrScopeVrUsage = colorDescriptor.vrUsage;
        _xrScopeRecorded = false;
        _xrScopeFinished = false;
        _xrScopePostDestination = TextureHandle.nullHandle;
        _xrScopeReconstruction = TextureHandle.nullHandle;
        _xrScopeActiveColor = activeColor;
        _xrScopeFailure = null;
        // Published last: a throw before this point leaves the previous (unowned) state intact.
        _xrScopeToken = _xrScopeTokens;
        token = _xrScopeToken;

        try
        {
            RecordXrReconstruction(pass, graph, frameData, cameraData, fit, activeColor, target);
        }
        catch (Exception exception)
        {
            FailXrScope("the XR reconstruction threw: " + exception.GetType().Name + ": " + exception.Message);
        }
        // URP's post methods take their handles as `in`, which the interop surface exposes as `ref`:
        // rewriting the original arguments never reaches the body. The chain therefore runs again from
        // here with the reconstruction as its active colour, and the outer call is skipped.
        var reconstruction = _xrScopeReconstruction;
        var lut = lutTexture;
        var overlay = overlayTexture;
        var postTarget = _xrScopePostDestination;
        _xrScopeReplaying = true;
        try
        {
            pass.RenderPostProcessingRenderGraph(graph, frameData, ref reconstruction, ref lut, ref overlay,
                ref postTarget, hasFinalPass, resolveToDebugScreen, enableColorEndingIfNeeded);
        }
        catch (Exception exception)
        {
            _xrScopeRecorded = false;
            _xrScopePostDestination = TextureHandle.nullHandle;
            FailXrScope("the XR post chain failed: " + exception.GetType().Name + ": " + exception.Message);
            return true;
        }
        finally
        {
            _xrScopeReplaying = false;
        }
        return false;
    }

    /// <summary>
    /// Reconstructs from the HDR image URP actually handed UberPost and substitutes the full-size
    /// result. Every resource is validated before the first pass is recorded, so a rejection leaves
    /// the stock operands untouched.
    /// </summary>
    private void RecordXrReconstruction(PostProcessPassRenderGraph pass, RenderGraph graph,
        ContextContainer frameData, UniversalCameraData cameraData, SizeFit fit, TextureHandle source,
        TextureHandle destination)
    {
        var resourceData = frameData.Get<UniversalResourceData>();
        var depth = resourceData.cameraDepthTexture;
        var motion = resourceData.motionVectorColor;
        if (!source.IsValid() || !destination.IsValid())
        {
            FailXrScope("URP did not hand the owned XR uber post a valid source and destination");
            return;
        }
        _xrScopeLogs++;
        if (_settings.Diagnostics.Value && (_xrScopeLogs <= 3 || _xrScopeLogs % 600 == 0))
        {
            _plugin.LogInfo($"XR uber source: {DescribeTarget(graph, source, resourceData.backBufferColor)} " +
                            $"dest={DescribeTarget(graph, destination, resourceData.backBufferColor)} " +
                            $"depth={DescribeTarget(graph, depth, resourceData.backBufferColor)} " +
                            $"motion={DescribeTarget(graph, motion, resourceData.backBufferColor)} " +
                            $"activeBackBuffer={resourceData.isActiveTargetBackBuffer} " +
                            $"sourceIsActiveColor={source == _xrScopeActiveColor} " +
                            $"input={_xrScopeInputWidth}x{_xrScopeInputHeight} " +
                            $"output={_xrScopeOutputWidth}x{_xrScopeOutputHeight}.");
        }
        if (!depth.IsValid() || !motion.IsValid())
        {
            FailXrScope("URP did not provide readable depth and motion vectors for the owned XR camera");
            return;
        }
        TextureDesc sourceDescriptor;
        TextureDesc depthDescriptor;
        TextureDesc motionDescriptor;
        try
        {
            sourceDescriptor = source.GetDescriptor(graph);
            depthDescriptor = depth.GetDescriptor(graph);
            motionDescriptor = motion.GetDescriptor(graph);
        }
        catch (Exception exception)
        {
            FailXrScope("the XR uber post inputs could not be described: " + exception.Message);
            return;
        }
        if (!PresentationExtent.TryResolve(_xrScopeInputWidth, _xrScopeInputHeight, _xrScopeOutputWidth,
                _xrScopeOutputHeight, sourceDescriptor.width, sourceDescriptor.height, depthDescriptor.width,
                depthDescriptor.height, motionDescriptor.width, motionDescriptor.height, _xrScopeSlices,
                _xrScopeLayout, fit.Eyes, out var bounds, out var extentReason))
        {
            FailXrScope(extentReason);
            return;
        }

        _output.Ensure(_xrScopeOutputWidth, _xrScopeOutputHeight, _xrScopeSlices, _xrScopeDimension,
            _xrScopeVrUsage, out var outputError);
        if (outputError != null)
        {
            FailXrScope("output texture: " + outputError);
            return;
        }
        if (_output.Handle == null || _output.Texture == null)
        {
            FailXrScope("output texture unavailable");
            return;
        }
        var resourceTag = _output.Tag;

        var finalDestination = destination;
        var fullDestination = resourceData.isActiveTargetBackBuffer ||
                              destination == resourceData.backBufferColor || destination.IsBuiltin();
        if (!fullDestination)
        {
            TextureDesc targetDescriptor;
            try
            {
                targetDescriptor = destination.GetDescriptor(graph);
            }
            catch (Exception exception)
            {
                FailXrScope("the XR post-processing target could not be described: " + exception.Message);
                return;
            }
            if (PresentationExtent.IsFullTarget(targetDescriptor.width, targetDescriptor.height,
                    _xrScopeOutputWidth, _xrScopeOutputHeight))
            {
                fullDestination = true;
            }
            else if (PresentationExtent.IsReducedTarget(targetDescriptor.width, targetDescriptor.height,
                         _xrScopeOutputWidth, _xrScopeOutputHeight))
            {
                _output.EnsurePostTarget(_xrScopeOutputWidth, _xrScopeOutputHeight, _xrScopeSlices,
                    targetDescriptor.dimension, targetDescriptor.vrUsage, targetDescriptor.format, out var postError);
                if (postError != null)
                {
                    FailXrScope("post-processing target: " + postError);
                    return;
                }
                if (_output.PostHandle == null)
                {
                    FailXrScope("post-processing target unavailable");
                    return;
                }
                finalDestination = graph.ImportTexture(_output.PostHandle);
                fullDestination = true;
                LogOnce($"promoted the reduced XR {targetDescriptor.width}x{targetDescriptor.height} uber " +
                        $"post target to {_xrScopeOutputWidth}x{_xrScopeOutputHeight}; downstream image " +
                        "scaling disabled.");
            }
            else
            {
                FailXrScope($"the XR post-processing target {targetDescriptor.width}x{targetDescriptor.height} " +
                            "could not be classified");
                return;
            }
        }

        var output = graph.ImportTexture(_output.Handle);
        var outputDescriptor = output.GetDescriptor(graph);
        if (outputDescriptor.width != _xrScopeOutputWidth || outputDescriptor.height != _xrScopeOutputHeight)
        {
            FailXrScope("imported output descriptor does not match the camera");
            return;
        }
        if (!_pass.HasFreeLease)
        {
            FailXrScope("every reconstruction recording lease is still busy");
            return;
        }

        // The active viewport is authoritative; the fit only chose the scalar and URP truncates it.
        var renderedEye = _xrScopeLayout == ViewIdentity.LayoutPacked
            ? default
            : new InputSize(bounds.ActiveWidth, bounds.ActiveHeight);
        var count = BuildFrames(cameraData, _xrScopeLayout, _xrScopeSlices, _xrScopeOutputWidth,
            _xrScopeOutputHeight, fit, renderedEye, sourceDescriptor, motionDescriptor, resourceTag, out var buildError);
        if (count == 0)
        {
            FailXrScope(buildError ?? "no logical views to reconstruct");
            return;
        }

        _output.Reserve(resourceTag, count);
        var recorded = false;
        try
        {
            if (!_pass.RecordReconstruction(graph, source, depth, motion, output, _frames, count, _renderEvent,
                    resourceTag))
            {
                _output.Rollback(resourceTag, count);
                if (_pass.HasPendingFailure)
                {
                    // The recording path published the exact exception; Tick latches that reason.
                    _xrScopeFailure = "the XR reconstruction recording failed";
                }
                else
                {
                    FailXrScope("the render graph rejected the XR reconstruction pass");
                }
                return;
            }
            recorded = true;
            if (fullDestination)
            {
                pass.UpdateCameraResolution(graph, cameraData,
                    new Vector2Int(_xrScopeOutputWidth, _xrScopeOutputHeight));
                cameraData.imageScalingMode = ImageScalingMode.None;
            }
            _xrScopeReconstruction = output;
            _xrScopePostDestination = finalDestination;
            _xrScopeRecorded = true;
            if (_settings.Diagnostics.Value && (_xrScopeLogs <= 3 || _xrScopeLogs % 600 == 0))
            {
                _plugin.LogInfo($"XR uber substitute: source={DescribeTarget(graph, _xrScopeReconstruction, resourceData.backBufferColor)} " +
                                $"dest={DescribeTarget(graph, _xrScopePostDestination, resourceData.backBufferColor)}.");
            }
        }
        catch (Exception exception)
        {
            if (!recorded) _output.Rollback(resourceTag, count);
            FailXrScope("the XR reconstruction threw: " + exception.GetType().Name + ": " + exception.Message);
        }
    }

    /// <summary>
    /// The nested UberPost postfix. Marks the matching scope finished only after the original pass
    /// returned normally, then publishes the post-processed destination the rest of URP must read.
    /// </summary>
    internal void OnXrUberPostReturned(PostProcessPassRenderGraph pass, RenderGraph graph,
        ContextContainer frameData, UniversalCameraData cameraData)
    {
        if (_xrScopeToken == 0 || !_xrScopeRecorded || _xrScopeFinished) return;
        if (pass == null || graph == null || frameData == null || cameraData == null) return;
        if (pass.Pointer != _xrScopePass || graph.Pointer != _xrScopeGraph ||
            frameData.Pointer != _xrScopeFrameData) return;
        if (cameraData.camera == null || cameraData.camera.GetInstanceID() != _xrScopeCameraId) return;
        _xrScopeFinished = true;
        var destination = _xrScopePostDestination;
        if (!destination.IsValid()) return;
        try
        {
            frameData.Get<UniversalResourceData>().cameraColor = destination;
        }
        catch (Exception exception)
        {
            ReportXrPostFailure(exception);
        }
    }

    /// <summary>
    /// The outer post postfix. Reaffirms the published destination and performs the one
    /// frame-generation capture at the original post-completion boundary.
    /// </summary>
    internal void OnPostProcessReturned(PostProcessPassRenderGraph pass, RenderGraph graph,
        ContextContainer frameData, ref TextureHandle postProcessingTarget, int scopeToken)
    {
        if (scopeToken == 0 || scopeToken != _xrScopeToken) return;
        if (!_xrScopeFinished)
        {
            if (_xrScopeFailure == null && _xrScopeRecorded)
                FailXrScope("URP did not complete the owned XR UberPost");
            else if (_xrScopeFailure == null)
                FailXrScope("URP did not record the owned XR UberPost");
            return;
        }
        var destination = _xrScopePostDestination;
        if (!destination.IsValid()) return;
        try
        {
            var resourceData = frameData.Get<UniversalResourceData>();
            postProcessingTarget = destination;
            resourceData.cameraColor = destination;
            _frameGeneration.CaptureAfterPostProcessing(graph, frameData, _xrScopeCameraId, destination);
            // Verification only: sample the post-processing destination and the eye array the final
            // transfer writes, so the presented pixels can be checked instead of inferred.
            try
            {
                _xrArrayProof.ArmPostUber(graph, resourceData, frameData.Get<UniversalCameraData>(),
                    destination, _proof.WindowName);
            }
            catch
            {
                // A missing camera data here is not a render failure; the tap simply stays idle.
            }
        }
        catch (Exception exception)
        {
            ReportXrPostFailure(exception);
        }
    }

    /// <summary>Clears the scope this invocation owns; a mismatch is another invocation's state.</summary>
    internal void OnPostProcessExited(int scopeToken)
    {
        if (scopeToken == 0 || scopeToken != _xrScopeToken) return;
        if (!_xrScopeFinished && _xrScopeFailure == null)
            FailXrScope(_xrScopeRecorded
                ? "URP did not complete the owned XR UberPost"
                : "URP did not record the owned XR UberPost");
        _xrScopeToken = 0;
        _xrScopePass = IntPtr.Zero;
        _xrScopeGraph = IntPtr.Zero;
        _xrScopeFrameData = IntPtr.Zero;
        _xrScopeCameraId = 0;
        _xrScopeRecorded = false;
        _xrScopeFinished = false;
        _xrScopePostDestination = TextureHandle.nullHandle;
        _xrScopeReconstruction = TextureHandle.nullHandle;
        _xrScopeActiveColor = TextureHandle.nullHandle;
        _xrScopeFailure = null;
    }

    /// <summary>Latches one exact XR seam failure and keeps the stock operands for this frame.</summary>
    private void FailXrScope(string reason)
    {
        if (_xrScopeFailure != null) return;
        _xrScopeFailure = reason;
        KeepStock(reason);
    }

    internal void ReportXrPostFailure(Exception exception)
    {
        var reason = "the XR post-processing seam failed: " + exception.GetType().Name + ": " + exception.Message;
        if (_lastFailure == reason) return;
        _lastFailure = reason;
        _plugin.LogError("BigScale cannot render: " + reason);
        _plan.Fail(reason);
    }

    private void Promote(Camera camera, long area)
    {
        var previous = _primaryCameraId;
        _primaryCamera = camera;
        _primaryCameraId = camera.GetInstanceID();
        _primaryArea = area;
        try
        {
            _primarySceneHandle = camera.gameObject.scene.handle;
        }
        catch
        {
            _primarySceneHandle = 0;
        }
        _lastPrimarySeenFrame = Time.frameCount;
        _history.Clear();
        _retired.Clear();
        ClearFit(previous == 0 ? "adopting the primary camera" : "the primary camera changed");
        ResetXrIdentity();
        _plan.Reacquire(previous == 0 ? "waiting for the first camera boundary" : "primary camera changed");
        _frameGeneration.ResumeAfterScene();
        if (_sceneLostAt >= 0f)
        {
            _plugin.LogInfo($"frame generation re-armed for a new primary camera after " +
                            $"{(Time.realtimeSinceStartup - _sceneLostAt) * 1000f:F0} ms.");
            _sceneLostAt = -1f;
        }
        _plugin.LogInfo(previous == 0
            ? $"primary camera adopted after rendering: {camera.name} id={_primaryCameraId}."
            : $"primary camera changed to id={_primaryCameraId}; restarting the capability probe.");
    }

    /// <summary>
    /// Drops the primary camera and every derived identity. Ownership is restored at the next
    /// boundary; the remembered SR/FG selections are never rewritten.
    /// </summary>
    private void ReleasePrimary(string reason)
    {
        if (_primaryCameraId == 0 && _primaryCamera == null) return;
        _primaryCamera = null;
        _primaryCameraId = 0;
        _primaryArea = 0;
        _primarySceneHandle = 0;
        _history.Clear();
        _retired.Clear();
        ClearFit(reason);
        ResetXrIdentity();
        _plan.Reacquire(reason);
        _frameGeneration.SuspendForScene();
        _sceneLostAt = Time.realtimeSinceStartup;
        _plugin.LogInfo("primary camera released: " + reason + "; the selected providers are preserved.");
    }

    /// <summary>
    /// Called by the scene-unloaded callback when the primary camera's scene is unloaded. Unrelated
    /// additive unloads are ignored.
    /// </summary>
    internal void OnPrimarySceneUnloaded(int sceneHandle)
    {
        if (_primaryCameraId == 0) return;
        if (_primarySceneHandle == 0 || sceneHandle != _primarySceneHandle) return;
        ReleasePrimary("the primary scene unloaded");
    }

    /// <summary>
    /// The replacement path. Everything is validated before the first pass is recorded, so a
    /// preflight failure can still fall back to the untouched stock orchestration.
    /// </summary>
    private bool Replace(RenderGraph graph, PostProcessPassRenderGraph pass, ContextContainer frameData,
        UniversalResourceData resourceData, UniversalCameraData cameraData, ref TextureHandle color,
        TextureDesc colorDescriptor, ref TextureHandle lutTexture, ref TextureHandle overlayTexture,
        ref TextureHandle postProcessingTarget, bool hasFinalPass, bool resolveToDebugScreen,
        bool enableColorEndingIfNeeded, int cameraId, int layout, int slices, int viewCount, int outputWidth,
        int outputHeight)
    {
        var fit = _fit;
        if (fit == null) return KeepStock("no resolved size fit");
        var additional = cameraData.camera.GetComponent<UniversalAdditionalCameraData>();
        if (additional == null) return KeepStock("camera has no UniversalAdditionalCameraData");

        var depth = resourceData.cameraDepthTexture;
        var motion = resourceData.motionVectorColor;
        if (!depth.IsValid() || !motion.IsValid())
            return KeepStock("URP did not provide readable depth and motion vectors for this camera");

        // The active viewport is what the camera actually rendered this frame. Read it before any
        // post-upscale descriptor write, and never let a larger physical allocation redefine it.
        int activeWidth;
        int activeHeight;
        try
        {
            var activeDescriptor = cameraData.cameraTargetDescriptor;
            activeWidth = activeDescriptor.width;
            activeHeight = activeDescriptor.height;
        }
        catch (Exception exception)
        {
            return KeepStock("the camera target descriptor could not be read: " + exception.Message);
        }

        var depthDescriptor = depth.GetDescriptor(graph);
        var motionDescriptor = motion.GetDescriptor(graph);
        if (!PresentationExtent.TryResolve(activeWidth, activeHeight, outputWidth, outputHeight,
                colorDescriptor.width, colorDescriptor.height, depthDescriptor.width, depthDescriptor.height,
                motionDescriptor.width, motionDescriptor.height, slices, layout, fit.Eyes,
                out var bounds, out var extentReason))
            return KeepStock(extentReason);

        _output.Ensure(outputWidth, outputHeight, slices, colorDescriptor.dimension, colorDescriptor.vrUsage,
            out var outputError);
        if (outputError != null) return KeepStock("output texture: " + outputError);
        if (_output.Handle == null || _output.Texture == null) return KeepStock("output texture unavailable");
        var resourceTag = _output.Tag;

        var output = graph.ImportTexture(_output.Handle);
        var outputDescriptor = output.GetDescriptor(graph);
        if (outputDescriptor.width != outputWidth || outputDescriptor.height != outputHeight)
            return KeepStock("imported output descriptor does not match the camera");

        if (!_stock.EnsureHelpers(out var helperError)) return KeepStock(helperError ?? "URP helpers unavailable");
        if (!_stock.Prepare(pass, frameData, cameraData, hasFinalPass, enableColorEndingIfNeeded,
                lutTexture.IsValid(), out var prepareReason))
            return KeepStock(prepareReason ?? "URP post-process materials are incomplete for this build");
        if (!_pass.HasFreeLease) return KeepStock("every reconstruction recording lease is still busy");

        // Fullscreen features URP records after post-processing still read the descriptor-backed
        // intermediate it allocated, so an XR camera keeps that target as well: URP owns the final
        // transfer into the back buffer, and publishing into the back buffer here makes those later
        // consumers throw.
        var destination = postProcessingTarget;
        var promoted = false;
        var publishFullResolution = resourceData.isActiveTargetBackBuffer;

        // Without an XR target the stock post pass writes into an attachment URP allocated at the scene
        // render extent. When it is the reduced intermediate, writing the full BigScale result into it would
        // force a second spatial scale in URP's own final stage. Publish an explicit full-size
        // destination instead; backbuffers and already-full intermediates keep their existing path.
        if (!publishFullResolution)
        {
            if (!postProcessingTarget.IsValid())
            {
                LogOnce("the post-processing target handle is invalid; keeping its existing extent.");
            }
            else
            {
                var targetDescriptor = postProcessingTarget.GetDescriptor(graph);
                if (PresentationExtent.IsFullTarget(targetDescriptor.width, targetDescriptor.height, outputWidth,
                        outputHeight))
                {
                    publishFullResolution = true;
                }
                else if (PresentationExtent.IsReducedTarget(targetDescriptor.width, targetDescriptor.height,
                             outputWidth, outputHeight))
                {
                    // The Boolean reports whether a new allocation was built, not success: every later
                    // frame at the same descriptor reuses the existing target and returns false with a
                    // null error. Only the error channel may keep stock rendering here.
                    _output.EnsurePostTarget(outputWidth, outputHeight, slices, targetDescriptor.dimension,
                        targetDescriptor.vrUsage, targetDescriptor.format, out var postError);
                    if (postError != null) return KeepStock("post-processing target: " + postError);
                    if (_output.PostHandle == null) return KeepStock("post-processing target unavailable");
                    destination = graph.ImportTexture(_output.PostHandle);
                    promoted = true;
                    publishFullResolution = true;
                    LogOnce($"promoted the reduced {targetDescriptor.width}x{targetDescriptor.height} URP post " +
                            $"target to {outputWidth}x{outputHeight} for the BigScale handoff; downstream image " +
                            "scaling disabled.");
                }
                else
                {
                    LogOnce($"the post-processing target {targetDescriptor.width}x{targetDescriptor.height} " +
                            "could not be classified; no downstream resolution change is applied.");
                }
            }
        }

        // Diagnostics: the targets URP will present from, recorded once so a frozen headset image can
        // be read against the passes the plugin actually handed on.
        if (_settings.Diagnostics.Value && cameraData.xr is { enabled: true })
        {
            LogOnce("XR handoff: " +
                    $"backBuffer={resourceData.isActiveTargetBackBuffer} hasFinalPass={hasFinalPass} " +
                    $"debugScreen={resolveToDebugScreen} " +
                    $"post={DescribeTarget(graph, postProcessingTarget, resourceData.backBufferColor)} " +
                    $"color={DescribeTarget(graph, color, resourceData.backBufferColor)} " +
                    $"cameraColor={DescribeTarget(graph, resourceData.cameraColor, resourceData.backBufferColor)} " +
                    $"published={publishFullResolution} promoted={promoted} " +
                    $"scaling={cameraData.imageScalingMode} filter={cameraData.upscalingFilter} " +
                    $"resolve={cameraData.resolveFinalTarget} overlayUI={cameraData.rendersOverlayUI} " +
                    $"desc={cameraData.cameraTargetDescriptor.width}x{cameraData.cameraTargetDescriptor.height} " +
                    $"pixel={cameraData.pixelWidth}x{cameraData.pixelHeight} " +
                    $"renderScale={cameraData.renderScale:F3}");
        }

        // The active viewport is the authoritative input extent: the fit only chose the scalar, and
        // URP's own truncation decides the integer size. A packed atlas keeps its per-eye layout.
        var renderedEye = layout == ViewIdentity.LayoutPacked
            ? default
            : new InputSize(bounds.ActiveWidth, bounds.ActiveHeight);
        var count = BuildFrames(cameraData, layout, slices, outputWidth, outputHeight, fit, renderedEye,
            colorDescriptor, motionDescriptor, resourceTag, out var buildError);
        if (count == 0) return KeepStock(buildError ?? "no logical views to reconstruct");

        var recording = false;
        // The generation is committed to the graph the moment this pass is recorded, not when the
        // callback eventually runs: a resize in between must not destroy the imported texture.
        _output.Reserve(resourceTag, count);
        try
        {
            recording = true;
            StockPostProcessing.RecordProjectionSetup(graph);
            _stock.BeforeReconstruction(pass, graph, resourceData, cameraData, ref color);
            if (!_pass.RecordReconstruction(graph, color, depth, motion, output, _frames, count, _renderEvent,
                    resourceTag))
            {
                _output.Rollback(resourceTag, count);
                // A published recording failure already carries the pass label and exception; the next
                // Tick latches and logs that exact reason. A plain rejection gets its own fallback.
                if (!_pass.HasPendingFailure) _plan.Fail("render graph rejected the reconstruction pass");
                return false;
            }

            if (publishFullResolution)
            {
                // Only inflate the descriptor when the attachment really is full size: otherwise
                // URP's scaled viewport would draw beyond the reduced attachment and sample a crop.
                pass.UpdateCameraResolution(graph, cameraData, new Vector2Int(outputWidth, outputHeight));
                cameraData.imageScalingMode = ImageScalingMode.None;
            }
            color = output;
            if (promoted)
            {
                // The caller's own local target keeps the reduced handle; every downstream pass reads
                // resourceData.cameraColor, so publish the same full-size texture through both.
                resourceData.cameraColor = destination;
            }
            _stock.AfterReconstruction(pass, graph, frameData, resourceData, cameraData, resolveToDebugScreen,
                ref color, ref destination, ref lutTexture, ref overlayTexture);
            // Record now: later overlay passes may write directly into this same built-in backbuffer.
            _frameGeneration.CaptureAfterPostProcessing(graph, frameData, cameraId, destination);
            return false;
        }
        catch (Exception exception)
        {
            _plan.Fail("reconstruction threw: " + exception.Message);
            if (!recording) return true;
            // Passes are already recorded, so the stock orchestration must not run a second time on
            // top of them. Whatever colour handle is current is a valid image for this frame.
            return false;
        }
    }

    /// <summary>Logs a presentation handoff fact once per session; repeated frames stay quiet.</summary>
    private void LogOnce(string message)
    {
        var key = "presentation:" + message;
        if (!_skipReasons.Add(key)) return;
        _plugin.LogInfo(message);
        if (_skipReasons.Count > 64) _skipReasons.Clear();
    }

    /// <summary>Formats one render target for a diagnostics line; an unassigned handle is named.</summary>
    private static string DescribeTarget(RenderGraph graph, TextureHandle handle, TextureHandle backBuffer)
    {
        if (!handle.IsValid()) return "invalid";
        // Unity imports the game's CameraTarget RTHandle as UniversalResourceData.backBufferColor.
        // That handle has no graph descriptor and IsBuiltin() alone does not identify it, so the
        // identity is classified first and the graph is never asked to describe it.
        if (handle == backBuffer) return "backBuffer (descriptor unavailable)";
        if (handle.IsBuiltin()) return "builtin (descriptor unavailable)";
        try
        {
            var descriptor = handle.GetDescriptor(graph);
            return $"{descriptor.width}x{descriptor.height} slices={descriptor.slices} " +
                   $"name={descriptor.name ?? "<none>"} format={descriptor.format}";
        }
        catch (Exception exception)
        {
            return "descriptor unavailable (" + exception.GetType().Name + ": " + exception.Message + ")";
        }
    }

    /// <summary>Diagnostics: the operands of the uber post that produces the presented image.</summary>
    internal void OnUrpUberPost(RenderGraph graph, ContextContainer frameData, TextureHandle source,
        TextureHandle destination)
    {
        if (!_settings.Diagnostics.Value) return;
        _uberPostCalls++;
        if (_uberPostCalls > 3 && _uberPostCalls % 600 != 0) return;
        var backBuffer = frameData.Get<UniversalResourceData>().backBufferColor;
        _plugin.LogInfo($"URP uber post #{_uberPostCalls}: " +
                        $"source={DescribeTarget(graph, source, backBuffer)} " +
                        $"dest={DescribeTarget(graph, destination, backBuffer)}.");
    }

    /// <summary>Diagnostics: where URP leaves the camera colour once post-processing returned.</summary>
    internal void OnUrpPostExit(RenderGraph graph, ContextContainer frameData)
    {
        if (!_settings.Diagnostics.Value) return;
        _postExitCalls++;
        var resourceData = frameData.Get<UniversalResourceData>();
        var cameraData = frameData.Get<UniversalCameraData>();
        var backBuffer = resourceData.backBufferColor;
        var color = resourceData.cameraColor;
        // A master toggle must produce a comparable line in each mode, not wait for a multiple of 600.
        var signature = (cameraData.resolveFinalTarget ? 1 : 0) |
                        (cameraData.isLastBaseCamera ? 2 : 0) |
                        (resourceData.isActiveTargetBackBuffer ? 4 : 0) |
                        (color == backBuffer ? 8 : 0);
        var changed = signature != _postExitSignature;
        _postExitSignature = signature;
        if (!changed && _postExitCalls > 3 && _postExitCalls % 600 != 0) return;
        var camera = cameraData.camera;
        _plugin.LogInfo($"URP post exit #{_postExitCalls}: " +
                        $"camera={camera?.name ?? "<none>"} id={camera?.GetInstanceID() ?? 0} " +
                        $"resolveFinalTarget={cameraData.resolveFinalTarget} " +
                        $"lastBase={cameraData.isLastBaseCamera} overlayUI={cameraData.rendersOverlayUI} " +
                        $"resolveToScreen={cameraData.resolveToScreen} type={cameraData.renderType} " +
                        $"scaling={cameraData.imageScalingMode} filter={cameraData.upscalingFilter} " +
                        $"desc={cameraData.cameraTargetDescriptor.width}x" +
                        $"{cameraData.cameraTargetDescriptor.height} " +
                        $"pixel={cameraData.pixelWidth}x{cameraData.pixelHeight} " +
                        $"rect=({cameraData.pixelRect.x:F0},{cameraData.pixelRect.y:F0}," +
                        $"{cameraData.pixelRect.width:F0},{cameraData.pixelRect.height:F0}) " +
                        $"xrMesh={cameraData.xr?.hasValidVisibleMesh} " +
                        $"cameraColor={DescribeTarget(graph, color, backBuffer)} " +
                        $"backBuffer={DescribeTarget(graph, backBuffer, backBuffer)} " +
                        $"activeBackBuffer={resourceData.isActiveTargetBackBuffer}.");
    }

    /// <summary>Diagnostics: URP's own camera-target blit, the last hop of the presented image.</summary>
    internal void OnUrpFinalBlit(RenderGraph graph, ContextContainer frameData, TextureHandle source,
        TextureHandle destination)
    {
        if (!_settings.Diagnostics.Value) return;
        _finalBlitCalls++;
        if (_finalBlitCalls > 3 && _finalBlitCalls % 60 != 0) return;
        var backBuffer = frameData.Get<UniversalResourceData>().backBufferColor;
        _plugin.LogInfo($"URP final blit #{_finalBlitCalls}: " +
                        $"source={DescribeTarget(graph, source, backBuffer)} " +
                        $"dest={DescribeTarget(graph, destination, backBuffer)}.");
    }

    /// <summary>Diagnostics: the graph-based final transfer and the encoding flag it received.</summary>
    internal void OnUrpFinalPostGraph(RenderGraph graph, ContextContainer frameData, TextureHandle source,
        TextureHandle destination)
    {
        if (!_settings.Diagnostics.Value) return;
        _finalPostCalls++;
        if (_finalPostCalls > 3 && _finalPostCalls % 60 != 0) return;
        var resourceData = frameData.Get<UniversalResourceData>();
        var backBuffer = resourceData.backBufferColor;
        _plugin.LogInfo($"URP final post #{_finalPostCalls}: " +
                        $"source={DescribeTarget(graph, source, backBuffer)} " +
                        $"dest={DescribeTarget(graph, destination, backBuffer)} " +
                        $"activeBackBuffer={resourceData.isActiveTargetBackBuffer}.");
    }

    /// <summary>Falls back to the stock path for this frame and latches a fault behind a restore.</summary>
    private bool KeepStock(string reason)
    {
        _plan.Fail(reason);
        if (_lastFailure != reason)
        {
            _lastFailure = reason;
            _plugin.LogError("BigScale cannot render: " + reason);
        }
        return true;
    }

    private int BuildFrames(UniversalCameraData cameraData, int layout, int slices, int outputWidth,
        int outputHeight, SizeFit fit, InputSize renderedEye, TextureDesc colorDescriptor,
        TextureDesc motionDescriptor, uint resourceTag, out string? error)
    {
        error = null;
        var cameraId = cameraData.camera.GetInstanceID();
        NoteXrJitterAnomaly();
        var flags = (SystemInfo.usesReversedZBuffer ? Native.FlagReversedDepth : 0) |
                    (GraphicsFormatUtility.IsHDRFormat(colorDescriptor.format) ? Native.FlagHdrColor : 0) |
                    (_settings.Diagnostics.Value ? Native.FlagSampleInput : 0) |
                    (_settings.StageProbe.Value ? Native.FlagStageProbe : 0) |
                    (_xrActive ? _proof.Flags : 0);
        // Diagnostics stays non-visual for super resolution: the centre sample goes to the native log
        // for the first evaluations and then every 120th; it never stamps a marker or writes a file.
        // The stage probe is its own switch and is what writes the hashed stages and BMP pairs.
        const int operation = Native.OperationEvaluate;

        // Both logical eyes are always declared, so a single-eye completion can never make the frame
        // look complete.
        var eyeCount = ViewIdentity.ExpectedEyes(layout);
        for (var i = 0; i < eyeCount; i++) _history.ObserveEye(cameraId, i);

        var count = 0;
        if (ViewIdentity.IsSinglePassStereo(layout))
        {
            for (var slice = 0; slice < 2 && slice < slices; slice++)
            {
                var eye = ViewIdentity.EyeForPacket(layout, slice, 0);
                var packetSlice = ViewIdentity.SliceForPacket(layout, slice);
                var size = renderedEye.Width > 0 ? renderedEye : fit.Eyes[Mathf.Min(eye, fit.Eyes.Length - 1)];
                _frames[count++] = CreateFrame(cameraData, eye, eye, packetSlice, size, outputWidth, outputHeight,
                    layout, slices, fit, motionDescriptor, flags, operation, resourceTag);
            }
        }
        else
        {
            var multipassId = ResolveEye(cameraData, layout);
            var eye = ViewIdentity.EyeForPacket(layout, 0, multipassId);
            var size = renderedEye.Width > 0 ? renderedEye : fit.Eyes[Mathf.Min(eye, fit.Eyes.Length - 1)];
            _frames[count++] = CreateFrame(cameraData, eye, eye, ViewIdentity.SliceForPacket(layout, 0), size,
                outputWidth, outputHeight, layout, slices, fit, motionDescriptor, flags, operation,
                resourceTag);
        }

        if (_settings.Diagnostics.Value && count > 0 && Time.frameCount - _lastPacketLogFrame >= 600)
        {
            _lastPacketLogFrame = Time.frameCount;
            var packet = _frames[0];
            _plugin.LogInfo($"temporal packet: camera={packet.CameraId} " +
                            $"input={packet.InputWidth}x{packet.InputHeight} output={packet.OutputWidth}x{packet.OutputHeight} " +
                            $"motion={packet.MotionWidth}x{packet.MotionHeight} " +
                            $"jitter=({packet.JitterX:F3},{packet.JitterY:F3}) eyeReads={_xrEyeReads0}/{_xrEyeReads1} " +
                            $"mvScale=({packet.MotionScaleX:F1},{packet.MotionScaleY:F1}) " +
                            $"layout={packet.Layout} preset={packet.Preset} upscaler={packet.Upscaler} " +
                            $"operation={packet.Operation}.");
        }
        if (count == 0) error = "no logical views to reconstruct";
        return count;
    }

    private Native.Frame CreateFrame(UniversalCameraData cameraData, int eye, int viewIndex, int slice,
        InputSize size, int outputWidth, int outputHeight, int layout, int slices, SizeFit fit,
        TextureDesc motionDescriptor, int flags, int operation, uint resourceTag)
    {
        // TemporalInputPolicy is the single owner of the provider contract. Both inputs reach every
        // backend through this one packet, so neither may be converted per provider here.
        var (motionScaleX, motionScaleY) =
            TemporalInputPolicy.MotionScale(size.Width, size.Height, SystemInfo.graphicsUVStartsAtTop);
        var (jitterX, jitterY) = AppliedJitter(cameraData, size);
        var cameraId = cameraData.camera.GetInstanceID();
        var frameIndex = Time.frameCount;
        var epoch = _history.Generation(cameraId);
        // Only an explicit generation change requests a reset. Frame-gap continuity is decided
        // natively from the executed sequence, so a delayed GPU fence never resets history.
        var reset = _history.ConsumeEpochChange(cameraId, eye, epoch) ||
                    _xrJitterResetFrame == frameIndex;
        var camera = cameraData.camera;
        return new Native.Frame
        {
            Abi = Native.AbiVersion,
            Size = (uint)Native.FrameSize,
            CameraId = cameraId,
            Eye = eye,
            FrameIndex = frameIndex,
            HistoryEpoch = epoch,
            Operation = operation,
            Preset = (int)fit.Preset,
            InputWidth = size.Width,
            InputHeight = size.Height,
            OutputWidth = outputWidth,
            OutputHeight = outputHeight,
            Slice = slice,
            SliceCount = Mathf.Max(1, slices),
            Flags = flags | (reset ? Native.FlagReset : 0),
            Layout = layout,
            JitterX = jitterX,
            JitterY = jitterY,
            MotionScaleX = motionScaleX,
            MotionScaleY = motionScaleY,
            DeltaMilliseconds = Time.unscaledDeltaTime * 1000f,
            MotionWidth = motionDescriptor.width,
            MotionHeight = motionDescriptor.height,
            ResourceTag = resourceTag,
            Upscaler = (int)_selectedUpscaler,
            CameraNear = camera.nearClipPlane,
            CameraFar = camera.farClipPlane,
            VerticalFovRadians = VerticalFov(cameraData, viewIndex, camera),
            // This URP path is not pre-exposed; the color-grading postExposure EV is a different
            // quantity and must never be used here.
            PreExposure = 1f,
        };
    }

    /// <summary>
    /// Per-view vertical field of view in radians, derived from the real projection matrix. Falls
    /// back to the camera's serialized FOV only if the projection cannot be read.
    /// </summary>
    private static float VerticalFov(UniversalCameraData cameraData, int viewIndex, Camera camera)
    {
        try
        {
            var projection = cameraData.GetProjectionMatrix(viewIndex);
            var m11 = Mathf.Abs(projection.m11);
            if (m11 > 1e-6f) return 2f * Mathf.Atan(1f / m11);
        }
        catch
        {
            // A missing per-view projection is reported by the backend's own validation.
        }
        return camera.fieldOfView * Mathf.Deg2Rad;
    }

    /// <summary>
    /// The jitter this frame's raster was displaced by, already in render pixels. Outside XR the packet
    /// describes URP's own jitter matrix. In XR the packet carries the requested sample injected into both
    /// eye projections - identity when the pair did not both receive it, or when the raster extent was
    /// not the extent the sample was measured against. Measuring the packed matrix is verification only.
    /// </summary>
    private (float X, float Y) AppliedJitter(UniversalCameraData cameraData, InputSize size)
    {
        if (!_xrActive) return TemporalInputPolicy.Jitter(cameraData.m_JitterMatrix.m03 * size.Width * 0.5f,
            cameraData.m_JitterMatrix.m13 * size.Height * 0.5f);
        if (_xrJitterAppliedFrame != Time.frameCount) return (0f, 0f);
        if (XrJitter.ExtentProblem(size.Width, size.Height, _xrJitterInputWidth, _xrJitterInputHeight) !=
            JitterFrameProblem.None)
        {
            NoteXrJitterAnomaly(
                $"the raster extent {size.Width}x{size.Height} is not the {_xrJitterInputWidth}x" +
                $"{_xrJitterInputHeight} the eye projection was measured against");
            return (0f, 0f);
        }
        return TemporalInputPolicy.Jitter(_xrJitterPixelX, _xrJitterPixelY);
    }

    /// <summary>
    /// Decides whether this frame's two eyes disagree about the sample, and records the reason. A
    /// half-sampled pair is evaluated with identity jitter and an explicit history reset, so BigScale
    /// never accumulates a frame whose displacement it was not told about; the log names the cause
    /// once instead of every frame.
    /// </summary>
    private void NoteXrJitterAnomaly()
    {
        if (!_xrActive) return;
        if (XrJitter.PairProblem(_xrEye0Applied, _xrEye1Applied) != JitterFrameProblem.HalfPair)
        {
            _xrJitterAnomaly = "";
            return;
        }
        NoteXrJitterAnomaly(
            $"only eye {(_xrEye0Applied ? 1 : 0)} received the sample (reads={_xrEyeReads0}/{_xrEyeReads1})");
    }

    /// <summary>Latches one exact reason why this frame's jitter cannot be reported as sampled.</summary>
    private void NoteXrJitterAnomaly(string reason)
    {
        _xrJitterResetFrame = Time.frameCount;
        _xrJitterAnomaly = reason;
        if (Time.frameCount - _xrJitterAnomalyFrame < 600) return;
        _xrJitterAnomalyFrame = Time.frameCount;
        _plugin.LogError("XR jitter anomaly: " + reason + "; this frame reports identity jitter and resets history.");
    }

    private static int ResolveEye(UniversalCameraData cameraData, int layout)
    {
        if (layout == Native.LayoutArray) return 0;
        var xr = cameraData.xr;
        if (xr != null && xr.enabled) return xr.multipassId;
        return 0;
    }

    private static int DetermineLayout(UniversalCameraData cameraData, TextureDesc descriptor, int viewCount)
    {
        if (descriptor.dimension == TextureDimension.Tex2DArray && descriptor.slices > 1)
            return ViewIdentity.LayoutArray;
        if (viewCount > 1) return ViewIdentity.LayoutPacked;
        return ViewIdentity.Layout2D;
    }

    /// <summary>
    /// View count from the XR pass that is actually rendering. <c>Camera.stereoEnabled</c> is never
    /// used as a proxy: it stays true for a stereo-configured camera that renders no views.
    /// </summary>
    private static int ViewCount(UniversalCameraData cameraData)
    {
        var xr = cameraData.xr;
        if (xr == null || !xr.enabled) return 1;
        return Mathf.Max(1, xr.viewCount);
    }

    private bool Probe(RenderGraph graph, UniversalCameraData cameraData, TextureHandle color, string reason)
    {
        _plan.Wait(reason);
        var camera = cameraData.camera;
        if (!_pass.RecordProbe(graph, cameraData, color, _renderEvent, _selectedUpscaler, _settings.Preset.Value,
                camera.nearClipPlane, camera.farClipPlane, VerticalFov(cameraData, 0, camera)))
        {
            // A recording defect has already published its reason. Exhausting the leases is not a
            // defect; report only the current wait.
            if (!_pass.HasFreeLease) _plan.Wait("waiting for a free render-pass lease");
            return true;
        }
        if (!_probeQueuedLogged)
        {
            _probeQueuedLogged = true;
            _plugin.LogInfo($"capability probe queued: camera={cameraData.camera.GetInstanceID()} " +
                            $"upscaler={UpscalingModes.ShortName(_selectedUpscaler)} " +
                            $"output={cameraData.pixelWidth}x{cameraData.pixelHeight} (colour only).");
        }
        return true;
    }

    /// <summary>Records a resource-free recovery packet so the bridge can drain and clear a fault.</summary>
    private void ProbeRecovery(RenderGraph graph, UniversalCameraData cameraData, TextureHandle color)
    {
        if (_pass.RecordRecovery(graph, cameraData, color, _renderEvent)) return;
        if (!_pass.HasFreeLease) _plan.Wait("waiting for a free render-pass lease");
    }

    /// <summary>
    /// Called when the panel or the config file changed a setting. Only render-affecting changes
    /// reset the renderer; Diagnostics and the menu key must never invalidate history, clear a
    /// latched fault or recreate frame generation.
    /// </summary>
    internal void NotifySettingsChanged()
    {
        _settings.Normalize();
        var enabled = _settings.Enabled.Value;
        var upscaler = _settings.Upscaler.Value;
        var preset = _settings.Preset.Value;
        var frameGeneration = _settings.FrameGeneration.Value;
        var vrFrameGeneration = _settings.VrFrameGeneration.Value;
        var renderChanged = enabled != _lastEnabled || upscaler != _lastUpscaler || preset != _lastPreset;
        var frameGenerationChanged = frameGeneration != _lastFrameGeneration;
        var vrFrameGenerationChanged = vrFrameGeneration != _lastVrFrameGeneration;
        _lastEnabled = enabled;
        _lastUpscaler = upscaler;
        _lastPreset = preset;
        _lastFrameGeneration = frameGeneration;
        _lastVrFrameGeneration = vrFrameGeneration;
        if (!renderChanged)
        {
            if (vrFrameGenerationChanged)
                _plugin.LogInfo($"VR frame generation changed to {UpscalingModes.Name(vrFrameGeneration)}; " +
                                "the upscaler is untouched.");
            else
                _plugin.LogInfo(frameGenerationChanged
                    ? $"frame generation changed to {UpscalingModes.Name(frameGeneration)}; the upscaler is untouched."
                    : "a configuration-only setting changed; rendering is untouched.");
            return;
        }
        if (_nativeReady && _plan.State == OwnershipState.Faulted)
        {
            // A latched native fault owns device state; only a render-thread drain may clear it.
            _recoveryPending = true;
            _recoveryRequestedFrame = Time.frameCount;
            _plugin.LogInfo("settings changed while faulted: a recovery drain is required before re-probing.");
        }
        else
        {
            _plan.Enable("settings changed");
        }
        ClearFit("settings changed");
        _retired.Clear();
        _lastFailure = null;
        _probeQueuedLogged = false;
        _probeExecutedLogged = false;
        _switchQueuedLogged = false;
        if (_primaryCameraId != 0) _history.AdvanceGeneration(_primaryCameraId);
        _plugin.LogInfo($"settings changed: enabled={enabled} upscaler={UpscalingModes.Name(upscaler)} " +
                        $"preset={preset} frameGeneration={frameGeneration}.");
    }

    private ModeRange[]? GetModeRanges(int width, int height, UpscalerKind upscaler)
    {
        // The provider is part of the identity: another backend's sizes must never be reused.
        var key = ((long)(int)upscaler << 48) | ((long)width << 16) | (uint)(height & 0xFFFF);
        if (_modeCache.TryGetValue(key, out var cached)) return cached;
        var status = ReadStatus(width, height, (int)upscaler);
        if (status.ModeCount == 0) return null;
        var ranges = new ModeRange[UpscalingSizing.ModeInfoCount];
        for (var i = 0; i < ranges.Length; i++)
        {
            var info = status.ModeAt(i);
            ranges[i] = new ModeRange(info.Supported != 0, (int)info.OptimalWidth, (int)info.OptimalHeight,
                (int)info.MinWidth, (int)info.MinHeight, (int)info.MaxWidth, (int)info.MaxHeight);
        }
        _modeCache[key] = ranges;
        var supported = 0;
        foreach (var range in ranges) if (range.Supported) supported++;
        _plugin.LogInfo($"capability checkpoint: provider={UpscalingModes.ShortName(upscaler)} " +
                        $"output={width}x{height} modes={supported}/{ranges.Length} " +
                        $"runtime=0x{status.RuntimeVersion:x8} provider=0x{status.ProviderVersionId:x16}.");
        return ranges;
    }

    private Native.Status ReadStatus(int width, int height, int queryUpscaler)
    {
        var status = new Native.Status
        {
            OutputWidth = (uint)width,
            OutputHeight = (uint)height,
            QueryUpscaler = queryUpscaler,
        };
        Native.GetStatus(ref status);
        return status;
    }

    /// <summary>Camera eligibility. Overlay, preview, scene and stacked cameras keep the stock path.</summary>
    private static bool Eligible(UniversalCameraData cameraData, out string reason)
    {
        reason = "";
        var camera = cameraData.camera;
        if (cameraData.cameraType != CameraType.Game) { reason = "camera type " + cameraData.cameraType; return false; }
        if (cameraData.isPreviewCamera || cameraData.isSceneViewCamera) { reason = "preview or scene camera"; return false; }
        if (cameraData.renderType != CameraRenderType.Base) { reason = "overlay camera in a stack"; return false; }
        // UI and helper cameras clear depth or nothing; the world camera clears colour.
        if (camera.clearFlags is CameraClearFlags.Depth or CameraClearFlags.Nothing)
        {
            reason = "camera clears " + camera.clearFlags;
            return false;
        }
        var additional = camera.GetComponent<UniversalAdditionalCameraData>();
        if (additional == null) { reason = "no UniversalAdditionalCameraData"; return false; }
        if (additional.cameraStack.Count > 0)
        {
            reason = $"camera stack of {additional.cameraStack.Count}; temporal input is only valid without one";
            return false;
        }
        // In a VR session only the XR camera renders the game: a desktop mirror camera is a second
        // render of the same view and must never take the reconstruction path. Both rules report a
        // skip, never a fault, so a mirror-only frame leaves the session's real verdict untouched.
        var xr = cameraData.xr;
        if (VrState.Active && xr is not { enabled: true })
        {
            reason = "not the XR camera in a VR session";
            return false;
        }
        if (xr is { enabled: true } xrPass && !xrPass.singlePassEnabled)
        {
            reason = "XR multi-pass rendering is not supported; run Big Walk VR in single-pass instanced mode";
            return false;
        }
        return true;
    }

    /// <summary>Cheap boundary-time gate: the camera must be one this mod may ever own.</summary>
    private static bool IsRenderCamera(Camera camera, out UniversalAdditionalCameraData additional)
    {
        additional = null!;
        if (camera == null) return false;
        if (camera.cameraType != CameraType.Game) return false;
        if (camera.clearFlags is CameraClearFlags.Depth or CameraClearFlags.Nothing) return false;
        var data = camera.GetComponent<UniversalAdditionalCameraData>();
        if (data == null) return false;
        if (data.renderType != CameraRenderType.Base) return false;
        additional = data;
        return true;
    }

    private void LogSkip(UniversalCameraData cameraData, string reason)
    {
        var key = cameraData.camera.GetInstanceID() + ":" + reason;
        if (_skipReasons.Contains(key)) return;
        _skipReasons.Add(key);
        _plugin.LogInfo($"camera {cameraData.camera.name} keeps the stock path: {reason}.");
        if (_skipReasons.Count > 32) _skipReasons.Clear();
    }
}
