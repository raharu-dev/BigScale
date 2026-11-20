using System;
using System.Runtime.InteropServices;
using BigScale.Core;
using UnityEngine;

namespace BigScale;

/// <summary>
/// Managed half of VR frame generation: the requested VR provider, the arming edge and the HUD row.
///
/// The native side lives in the OpenVR compositor cycle and owns every GPU resource, the presenter
/// thread and the pacing; this class only decides whether the generator should be running and reports
/// the native state verbatim. The multiplier is not a preference here: AMD FidelityFX frame generation
/// produces exactly one interpolated frame, so VR always runs 2x and the remembered desktop
/// multiplier is never read or rewritten.
/// </summary>
internal sealed class VrFrameGenerationCoordinator
{
    private readonly Action<string> _log;
    private NativeVrFg.Status _status;
    private bool _hasStatus;
    private bool _configured;
    private string? _unavailable;
    private int _lastLoggedState = -1;
    private string _lastLoggedReason = "";
    // Re-installation retry. SteamVR and the native bridge may not be ready when the request arrives,
    // and a repeated native enable is idempotent.
    private const double ConfigureRetrySeconds = 1.0;
    private double _lastConfigure = -1.0;

    internal VrFrameGenerationCoordinator(Action<string> log) => _log = log;

    /// <summary>
    /// The row while no VR session owns the process: the same constant every frame, so the idle case
    /// never composes a string.
    /// </summary>
    private const string OffHudLine = "FG: Off - 1x - Off";

    /// <summary>The hold-TAB frame-generation row while a VR session owns the process.</summary>
    internal string HudLine { get; private set; } = OffHudLine;

    /// <summary>Set when the native ABI cannot be used at all; the selection then reports it verbatim.</summary>
    internal void SetUnavailable(string reason)
    {
        _unavailable = reason;
        _configured = false;
    }

    /// <summary>
    /// Called from the render coordinator's tick. <paramref name="upscalerActive"/> is the proof that a
    /// reconstruction output exists, which is the only source of the depth and motion guides.
    /// </summary>
    internal void Tick(ModSettings settings, bool xrActive, bool upscalerActive)
    {
        if (!xrActive && !_configured && _unavailable == null)
        {
            // Desktop path with no generator to release: the row is the constant, and nothing here can
            // change a transition this frame.
            HudLine = OffHudLine;
            LogTransition();
            return;
        }
        var provider = settings.Enabled.Value
            ? settings.VrFrameGeneration.Value
            : FrameGenerationKind.Off;
        var wanted = xrActive && _unavailable == null && provider == FrameGenerationKind.Fsr && upscalerActive;
        var now = Time.realtimeSinceStartup;
        if (wanted != _configured)
        {
            _configured = wanted;
            if (wanted) _log("VR frame generation requested: AMD FSR frame generation at 2x.");
            Apply(wanted, now);
            if (!wanted) _log("VR frame generation released.");
        }
        else if (wanted && now - _lastConfigure >= ConfigureRetrySeconds)
        {
            Apply(true, now);
        }
        HudLine = Compose(settings, xrActive, upscalerActive, provider);
        LogTransition();
    }

    private void Apply(bool wanted, double now)
    {
        _lastConfigure = now;
        try
        {
            NativeVrFg.Configure(wanted ? 1 : 0);
        }
        catch (Exception exception)
        {
            SetUnavailable("VR frame generation could not be configured: " + exception.Message);
            _log("VR frame generation unavailable: " + exception.Message);
        }
    }

    /// <summary>Re-reads the native status without waiting for the next tick.</summary>
    internal void RefreshStatus()
    {
        _hasStatus = false;
        if (_unavailable != null) return;
        try
        {
            _status = default;
            _status.Abi = NativeVrFg.AbiVersion;
            _status.Size = (uint)NativeVrFg.StatusSize;
            _hasStatus = NativeVrFg.GetStatus(ref _status) == 0;
        }
        catch
        {
            _hasStatus = false;
        }
    }

    private string Compose(ModSettings settings, bool xrActive, bool upscalerActive, FrameGenerationKind provider)
    {
        if (!xrActive) return UpscalingStatus.FrameGeneration(UpscalingStatus.FgOff, FrameGenerationKind.Off, 1, "");
        if (provider != FrameGenerationKind.Fsr)
            return UpscalingStatus.FrameGeneration(UpscalingStatus.FgOff, FrameGenerationKind.Off, 1, "");
        if (_unavailable != null)
            return UpscalingStatus.FrameGeneration(UpscalingStatus.FgUnsupported, FrameGenerationKind.Fsr, 2,
                _unavailable);
        if (!upscalerActive)
            return UpscalingStatus.FrameGeneration(UpscalingStatus.FgWaiting, FrameGenerationKind.Fsr, 2,
                "VR frame generation needs the upscaler to be producing frames");
        RefreshStatus();
        if (!_hasStatus)
            return UpscalingStatus.FrameGeneration(UpscalingStatus.FgUnsupported, FrameGenerationKind.Fsr, 2,
                "status unavailable");
        var reason = _status.ReasonText();
        return _status.State switch
        {
            NativeVrFg.StateActive => UpscalingStatus.FrameGeneration(UpscalingStatus.FgActive,
                FrameGenerationKind.Fsr, 2, reason),
            NativeVrFg.StateWaiting => UpscalingStatus.FrameGeneration(UpscalingStatus.FgWaiting,
                FrameGenerationKind.Fsr, 2, reason),
            NativeVrFg.StateUnsupported => UpscalingStatus.FrameGeneration(UpscalingStatus.FgUnsupported,
                FrameGenerationKind.Fsr, 2, reason),
            NativeVrFg.StateFaulted => UpscalingStatus.FrameGeneration(UpscalingStatus.FgFaulted,
                FrameGenerationKind.Fsr, 2, reason),
            _ => UpscalingStatus.FrameGeneration(UpscalingStatus.FgOff, FrameGenerationKind.Off, 1, ""),
        };
    }

    /// <summary>One log line per native state change, so the row never changes silently.</summary>
    private void LogTransition()
    {
        if (!_hasStatus) return;
        var reason = _status.ReasonText();
        if (_status.State == _lastLoggedState && reason == _lastLoggedReason) return;
        _lastLoggedState = _status.State;
        _lastLoggedReason = reason;
        var label = _status.State switch
        {
            NativeVrFg.StateActive => UpscalingStatus.FgActive,
            NativeVrFg.StateWaiting => UpscalingStatus.FgWaiting,
            NativeVrFg.StateUnsupported => UpscalingStatus.FgUnsupported,
            NativeVrFg.StateFaulted => UpscalingStatus.FgFaulted,
            _ => UpscalingStatus.FgOff,
        };
        _log($"VR frame generation: {label}" + (reason.Length > 0 ? ": " + reason : "") + ".");
    }
}
