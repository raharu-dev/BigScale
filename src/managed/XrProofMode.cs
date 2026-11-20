using System;
using UnityEngine;

namespace BigScale;

/// <summary>
/// Presentation proof for one VR acceptance launch.
/// <para>
/// Enabled only when <c>Diagnostics/XrProof</c> was set at startup, which also exports
/// <c>BIGSCALE_XR_PROOF=1</c> for the native mirror capture. After the renderer is
/// producing fully retired stereo pairs and the player presses the configured capture key, it runs a
/// bounded sequence: baseline frames with no tag, tagged frames whose output carries the eye identity
/// bar, and settle frames with the tag removed. The stage probe samples throughout, and the native
/// mirror capture records the compositor's image for the same windows, so one run answers whether the
/// composed eye image is the reconstruction that was written, and whether it keeps moving.
/// </para>
/// <para>
/// Nothing here runs in a normal launch: no flag is set, no file is written, and the sample rate stays
/// at the production value.
/// </para>
/// </summary>
internal sealed class XrProofMode
{
    private enum Phase
    {
        Idle,
        Baseline,
        Tagged,
        TaggedNoLate,
        TaggedWithout,
        Settle,
        Done,
    }

    // The windows are bounded so the whole sequence finishes in roughly 10 seconds at a 90 Hz pair rate.
    private const int BaselineFrames = 32;
    private const int TaggedFrames = 128;
    private const int TaggedNoLateFrames = 128;
    private const int TaggedWithoutFrames = 128;
    private const int SettleFrames = 64;
    // A key press is only accepted after this many consecutive active frames, so the tagged images
    // always come from a warmed history rather than the first reset frame.
    private const int WarmupFrames = 32;

    private readonly Action<string> _log;
    private readonly ModSettings _settings;
    private readonly bool _enabled;
    private Phase _phase = Phase.Idle;
    private int _phaseStart = -1;
    private int _readyFrames;
    private bool _keyFailed;
    private bool _warnedEarly;
    private int _notReadyFrames;
    private int _withoutIndex;
    private int _latePassCount;

    internal XrProofMode(Action<string> log, ModSettings settings)
    {
        _log = log;
        _settings = settings;
        // Startup snapshot: the coordinator is constructed during Plugin.Load, before the native
        // environment is fixed, and the proof cannot be armed or disarmed afterwards.
        _enabled = settings.XrProof.Value;
    }

    internal bool Enabled => _enabled;

    /// <summary>
    /// Packet flags for this frame. The stage probe runs for the whole sequence so the hashed stages
    /// and their crops exist; the identity bar is stamped only in the tagged window.
    /// </summary>
    internal int Flags => _phase switch
    {
        Phase.Baseline => Native.FlagStageProbe,
        Phase.Tagged => Native.FlagStageProbe | Native.FlagStageMarker,
        Phase.TaggedNoLate => Native.FlagStageProbe | Native.FlagStageMarker,
        Phase.TaggedWithout => Native.FlagStageProbe | Native.FlagStageMarker,
        Phase.Settle => Native.FlagStageProbe,
        _ => 0,
    };

    /// <summary>Late passes to remove this frame: -2 none, -1 all, k &gt;= 0 only late pass k.</summary>
    internal int LatePassRemoval => _phase switch
    {
        Phase.TaggedNoLate => -1,
        Phase.TaggedWithout => _withoutIndex,
        _ => -2,
    };

    /// <summary>The attribution window this frame belongs to.</summary>
    internal string WindowName => _phase switch
    {
        Phase.Baseline => "baseline",
        Phase.Tagged => "tagged",
        Phase.TaggedNoLate => "tagged-no-late",
        Phase.TaggedWithout => "tagged-without-" + _withoutIndex,
        Phase.Settle => "settle",
        Phase.Done => "done",
        _ => "idle",
    };

    /// <summary>The number of late passes the attribution windows will remove one at a time.</summary>
    internal void SetLatePassCount(int count)
    {
        _latePassCount = Math.Clamp(count, 0, 4);
    }

    /// <summary>Drops the sequence when ownership or the camera changes under it.</summary>
    internal void Abort(string reason)
    {
        if (!_enabled || _phase == Phase.Idle || _phase == Phase.Done) return;
        _log($"XR proof: aborted during {_phase} ({reason}).");
        _phase = Phase.Done;
    }

    /// <summary>One tick on the owning thread. <paramref name="ready"/> means a stereo pair retired.</summary>
    internal void Tick(bool ready)
    {
        if (!_enabled) return;
        if (ready) _readyFrames++;
        else _readyFrames = 0;
        if (_phase == Phase.Done) return;

        if (_phase == Phase.Idle)
        {
            var pressed = KeyPressed();
            if (_readyFrames < WarmupFrames)
            {
                if (pressed && !_warnedEarly)
                {
                    _warnedEarly = true;
                    _log("XR proof: the capture key was pressed before the renderer had a warm stereo " +
                         "history; press it again once the world is visible.");
                }
                return;
            }
            if (!pressed) return;
            _phase = Phase.Baseline;
            _phaseStart = Time.frameCount;
            LogTransition("baseline", BaselineFrames);
            return;
        }

        if (!ready)
        {
            // Opening the SteamVR dashboard, a load stall or an alt-tab stops packets for a moment. Only a
            // sustained gap means the sequence cannot describe a warm history any more.
            if (++_notReadyFrames > 300) Abort("the renderer stopped producing retired stereo pairs");
            return;
        }
        _notReadyFrames = 0;

        var elapsed = Time.frameCount - _phaseStart;
        switch (_phase)
        {
            case Phase.Baseline when elapsed >= BaselineFrames:
                _phase = Phase.Tagged;
                _phaseStart = Time.frameCount;
                LogTransition(WindowName, TaggedFrames);
                break;
            case Phase.Tagged when elapsed >= TaggedFrames:
                _phase = Phase.TaggedNoLate;
                _phaseStart = Time.frameCount;
                LogTransition(WindowName, TaggedNoLateFrames);
                break;
            case Phase.TaggedNoLate when elapsed >= TaggedNoLateFrames:
                if (_latePassCount == 0)
                {
                    _phase = Phase.Settle;
                    _phaseStart = Time.frameCount;
                    LogTransition(WindowName, SettleFrames);
                }
                else
                {
                    _withoutIndex = 0;
                    _phase = Phase.TaggedWithout;
                    _phaseStart = Time.frameCount;
                    LogTransition(WindowName, TaggedWithoutFrames);
                }
                break;
            case Phase.TaggedWithout when elapsed >= TaggedWithoutFrames:
                if (_withoutIndex + 1 < _latePassCount)
                {
                    _withoutIndex++;
                    _phaseStart = Time.frameCount;
                    LogTransition(WindowName, TaggedWithoutFrames);
                }
                else
                {
                    _phase = Phase.Settle;
                    _phaseStart = Time.frameCount;
                    LogTransition(WindowName, SettleFrames);
                }
                break;
            case Phase.Settle when elapsed >= SettleFrames:
                _phase = Phase.Done;
                _log("XR proof: sequence complete. The tagged and untagged mirror pairs and the stage probe " +
                     "crops are in the log directory; leave the headset on until the capture reports it is " +
                     "complete.");
                break;
        }
    }

    private void LogTransition(string name, int frames)
    {
        _log($"XR proof: {name} window frames {_phaseStart}..{_phaseStart + frames - 1}");
    }

    private bool KeyPressed()
    {
        try
        {
            var key = _settings.FrameCaptureKey.Value;
            if (key == KeyCode.None) return false;
            _keyFailed = false;
            return Input.GetKeyDown(key);
        }
        catch (Exception exception)
        {
            if (!_keyFailed)
            {
                _keyFailed = true;
                _log("XR proof: the capture key could not be read: " + exception.Message);
            }
            return false;
        }
    }
}
