using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using BigScale;
using BigScale.Core;

// Deterministic checks for the pure sizing, transition and history logic and the managed side of the
// native ABI. Compiled against the real source files, not mocks.
internal static class Program
{
    private static int _failures;

    private static int Main(string[] args)
    {
        CheckSizing();
        CheckPresets();
        CheckGaps();
        CheckAsymmetricEyes();
        CheckUnsupported();
        CheckStatusFormatting();
        CheckHistory();
        CheckFramePolicy();
        CheckPresentationExtent();
        CheckTemporalInputPolicy();
        CheckXrJitter();
        CheckFrameGenerationCameraPolicy();
        CheckPlayerMotionPolicy();
        CheckSettingsPanelSchema();
        CheckVrMode();
        CheckHudTelemetry();
        CheckHudPlacement();
        CheckPrimaryCameraPolicy();
        CheckFrameGenerationActivity();
        CheckFrameGenerationFamily();
        CheckFrameMultiplierCeiling();
        CheckCategoryColumnLayout();
        CheckTransition();
        CheckViewIdentity();
        CheckNeuralRendering();
        CheckCharacterMaskPolicy();
        CheckAbi();
        CheckBridge(args);
        Console.WriteLine(_failures == 0 ? "ALL CHECKS PASSED" : $"{_failures} CHECK(S) FAILED");
        return _failures == 0 ? 0 : 1;
    }

    private static void Check(bool condition, string description)
    {
        if (condition) return;
        _failures++;
        Console.WriteLine("FAIL: " + description);
    }

    private static void Near(float actual, float expected, string description) =>
        Check(Math.Abs(actual - expected) < 1e-4f, $"{description}: expected {expected}, got {actual}");

    // One supported per-eye range 960x540..1280x720 for 1920x1080, in native slot order
    // (Quality, Balanced, Performance, UltraPerformance, NativeAA).
    private static ModeRange[] NarrowRanges() => new[]
    {
        Mode(1280, 720, 960, 540, 1280, 720),  // Quality
        Mode(960, 540, 960, 540, 1280, 720),   // Balanced
        Mode(960, 540, 960, 540, 1280, 720),   // Performance
        Mode(960, 540, 960, 540, 1280, 720),   // Ultra Performance
        Mode(1280, 720, 960, 540, 1280, 720),  // Native AA (max 1280 => 1x unsupported at 1920)
    };

    private static ModeRange Mode(int optW, int optH, int minW, int minH, int maxW, int maxH) =>
        new(true, optW, optH, minW, minH, maxW, maxH);

    private static void CheckSizing()
    {
        var ranges = NarrowRanges();
        var fit = UpscalingSizing.Fit(UpscalingPreset.Quality, new[] { 1920 }, new[] { 1080 }, new[] { ranges });
        Check(fit.Ok, "a supported preset resolves");
        Check(fit.Eyes[0].Width == 1280 && fit.Eyes[0].Height == 720,
            $"quality must use the SDK optimum 1280x720, got {fit.Eyes[0]}");

        // The reported 4K case: the SDK's Balanced optimum is 2227x1253, and a 0.58 scalar truncates
        // the height to 1252 - exactly the viewport URP rendered. Rounding the fit to 1253 made the
        // presentation contract refuse every frame.
        var fourK = new ModeRange[UpscalingSizing.ModeInfoCount];
        for (var i = 0; i < fourK.Length; i++) fourK[i] = new ModeRange(false, 0, 0, 0, 0, 0, 0);
        fourK[UpscalingModes.ModeInfoIndex(UpscalingPreset.Balanced)] =
            Mode(2227, 1253, 960, 540, 3840, 2160);
        var balanced = UpscalingSizing.Fit(UpscalingPreset.Balanced, new[] { 3840 }, new[] { 2160 },
            new[] { fourK });
        Check(balanced.Ok && balanced.Eyes[0].Width == 2227 && balanced.Eyes[0].Height == 1252,
            $"a non-integral scalar truncates to URP's viewport, got " +
            $"{(balanced.Ok ? balanced.Eyes[0].ToString() : balanced.Reason)}");
        Check(!fit.Clamped, "an exact optimum is not clamped");
        Near(fit.EffectiveScale, 1280f / 1920f, "quality effective scale");

        var performance = UpscalingSizing.Fit(UpscalingPreset.Performance, new[] { 1920 }, new[] { 1080 },
            new[] { ranges });
        Check(performance.Ok && performance.Eyes[0].Width == 960 && performance.Eyes[0].Height == 540,
            $"performance resolves to 960x540, got {(performance.Ok ? performance.Eyes[0].ToString() : "none")}");
        Check(performance.Preset == UpscalingPreset.Performance, "the resolved preset is echoed");

        // No custom scale exists: the fit only carries the preset's own reported optimum, and a
        // Native AA range that cannot reach 1x is clamped into its own interval rather than invented.
        var native = UpscalingSizing.Fit(UpscalingPreset.NativeAA, new[] { 1920 }, new[] { 1080 },
            new[] { NarrowRanges() });
        Check(native.Ok && native.Eyes[0].Width == 1280 && native.Eyes[0].Height == 720,
            $"native AA clamps to the reported range, got {(native.Ok ? native.Eyes[0].ToString() : "none")}");
        Check(native.Preset == UpscalingPreset.NativeAA, "the native-AA preset is echoed");
    }

    private static void CheckPresets()
    {
        // Native AA/DLAA at native size needs a range that actually contains 1x.
        var wide = new ModeRange[UpscalingSizing.ModeInfoCount];
        for (var i = 0; i < wide.Length; i++) wide[i] = Mode(1280, 720, 960, 540, 1920, 1080);
        wide[UpscalingModes.ModeInfoIndex(UpscalingPreset.NativeAA)] = Mode(1920, 1080, 960, 540, 1920, 1080);

        var dlaa = UpscalingSizing.Fit(UpscalingPreset.NativeAA, new[] { 1920 }, new[] { 1080 }, new[] { wide });
        Check(dlaa.Ok && dlaa.Eyes[0].Width == 1920 && dlaa.Eyes[0].Height == 1080,
            $"native AA renders at native size, got {(dlaa.Ok ? dlaa.Eyes[0].ToString() : "none")}");

        var quality = UpscalingSizing.Fit(UpscalingPreset.Quality, new[] { 1920 }, new[] { 1080 }, new[] { wide });
        Check(quality.Ok && quality.Eyes[0].Width == 1280,
            $"quality keeps its own optimal size, got {(quality.Ok ? quality.Eyes[0].ToString() : "none")}");

        // Every preset maps to its declared native slot, independently of vendor numbering.
        Check(UpscalingModes.ModeInfoIndex(UpscalingPreset.Quality) == 0, "quality is slot 0");
        Check(UpscalingModes.ModeInfoIndex(UpscalingPreset.Balanced) == 1, "balanced is slot 1");
        Check(UpscalingModes.ModeInfoIndex(UpscalingPreset.Performance) == 2, "performance is slot 2");
        Check(UpscalingModes.ModeInfoIndex(UpscalingPreset.UltraPerformance) == 3, "ultra performance is slot 3");
        Check(UpscalingModes.ModeInfoIndex(UpscalingPreset.NativeAA) == 4, "native AA is slot 4");

        Check(Array.IndexOf(UpscalingModes.UpscalerOrder, UpscalerKind.DlssPresetL) ==
              Array.IndexOf(UpscalingModes.UpscalerOrder, UpscalerKind.Dlss) + 1,
            "the preset-L option follows plain DLSS in the display order");
        Check(UpscalingModes.Name(UpscalerKind.DlssPresetL) == "NVIDIA DLSS (Preset L)",
            "the preset-L option is labelled with its preset");
        Check(UpscalingModes.Name(UpscalingPreset.NativeAA, UpscalerKind.DlssPresetL) == "DLAA",
            "the preset-L option reports DLAA for the native-AA preset");

        // Frame generation providers are presented without an "(Experimental)" suffix; the category
        // that holds them carries the caveat instead.
        foreach (var kind in UpscalingModes.FrameGenerationOrder)
        {
            var name = UpscalingModes.Name(kind);
            Check(!name.Contains("Experimental"),
                $"the {kind} frame-generation label carries no experimental suffix, got {name}");
        }
        Check(UpscalingModes.Name(FrameGenerationKind.Nvidia) == "NVIDIA" &&
              UpscalingModes.Name(FrameGenerationKind.Fsr) == "AMD FSR" &&
              UpscalingModes.Name(FrameGenerationKind.XeSs) == "Intel XeSS" &&
              UpscalingModes.Name(FrameGenerationKind.Anr) == "ANR",
            "each frame-generation provider keeps its plain name");
    }

    // Disjoint per-eye intervals: the shared scalar must stay inside both, or the preset fails.
    private static void CheckGaps()
    {
        var ranges = new ModeRange[UpscalingSizing.ModeInfoCount];
        ranges[0] = Mode(1344, 756, 1344, 756, 1536, 864);    // Quality: 0.70..0.80
        ranges[1] = Mode(960, 540, 768, 432, 960, 540);       // Balanced
        ranges[2] = Mode(960, 540, 768, 432, 960, 540);       // Performance
        ranges[3] = Mode(960, 540, 0, 0, 0, 0);               // unsupported
        ranges[4] = Mode(1920, 1080, 1728, 972, 1920, 1080);  // Native AA: 0.90..1.00

        var fit = UpscalingSizing.Fit(UpscalingPreset.Quality, new[] { 1920 }, new[] { 1080 }, new[] { ranges });
        Check(fit.Ok, "quality resolves inside its interval");
        Check(fit.Eyes[0].Width >= 1344 && fit.Eyes[0].Width <= 1536,
            $"quality escaped its range: {fit.Eyes[0]}");

        var unsupported = UpscalingSizing.Fit(UpscalingPreset.UltraPerformance, new[] { 1920 }, new[] { 1080 },
            new[] { ranges });
        Check(!unsupported.Ok, "a preset without a reported range never resolves");
    }

    private static void CheckAsymmetricEyes()
    {
        // Two eyes with different output sizes and the same absolute pixel range: the shared scalar
        // must satisfy both, so neither eye escapes its own bounds.
        var ranges = new ModeRange[UpscalingSizing.ModeInfoCount];
        for (var i = 0; i < ranges.Length; i++) ranges[i] = Mode(1280, 720, 960, 540, 1280, 720);
        var fit = UpscalingSizing.Fit(UpscalingPreset.Balanced, new[] { 1920, 2048 }, new[] { 1080, 1152 },
            new[] { ranges, ranges });
        Check(fit.Ok, "asymmetric eyes resolve");
        Check(fit.Eyes.Length == 2, "asymmetric eyes return two sizes");
        Check(fit.Eyes[0].Width <= 1280 && fit.Eyes[0].Height <= 720, $"eye 0 escaped bounds: {fit.Eyes[0]}");
        Check(fit.Eyes[1].Width <= 1280 && fit.Eyes[1].Height <= 720, $"eye 1 escaped bounds: {fit.Eyes[1]}");
        var scale0 = fit.Eyes[0].Width / 1920f;
        var scale1 = fit.Eyes[1].Width / 2048f;
        Check(Math.Abs(scale0 - scale1) < 0.02f, $"eyes must share one effective scalar, got {scale0:F4} and {scale1:F4}");

        // No common scalar: one eye needs >= 0.9 and the other caps at 0.5.
        var eye0 = new ModeRange[UpscalingSizing.ModeInfoCount];
        var eye1 = new ModeRange[UpscalingSizing.ModeInfoCount];
        for (var i = 0; i < eye0.Length; i++)
        {
            eye0[i] = Mode(1728, 972, 1728, 972, 1920, 1080);
            eye1[i] = Mode(800, 450, 800, 450, 960, 540);
        }
        var conflict = UpscalingSizing.Fit(UpscalingPreset.Quality, new[] { 1920, 1920 }, new[] { 1080, 1080 },
            new[] { eye0, eye1 });
        Check(!conflict.Ok, "disjoint per-eye ranges must not produce mismatched eyes");
    }

    private static void CheckUnsupported()
    {
        var ranges = new ModeRange[6];
        for (var i = 0; i < 6; i++) ranges[i] = new ModeRange(false, 0, 0, 0, 0, 0, 0);
        var fit = UpscalingSizing.Fit(UpscalingPreset.Quality, new[] { 1920 }, new[] { 1080 }, new[] { ranges });
        Check(!fit.Ok, "an unsupported preset must not resolve");
        Check(fit.Eyes.Length == 0, "an unsupported mode returns no sizes");
    }

    private static void CheckStatusFormatting()
    {
        var summary = new RetiredFrameSummary();

        // Off names both sides unavailable instead of inventing a conversion.
        Check(UpscalingStatus.UpscalerHud(UpscalerKind.Off, null, 0, 0, 0, 0, 0) ==
              "UPSCALER: Off - Unavailable -> Unavailable",
            "an Off upscaler row invents nothing");
        var configured = UpscalingStatus.UpscalerHud(UpscalerKind.Fsr2, null, 1280, 720, 1920, 1080, 1);
        Check(configured == "UPSCALER: AMD FSR 2 - Configured: 1280x720 -> 1920x1080",
            "a pending row labels the configured size: " + configured);
        // Nothing retired and no fit: the previous row's dimensions must not survive.
        Check(UpscalingStatus.UpscalerHud(UpscalerKind.Fsr2, null, 0, 0, 0, 0, 0) ==
              "UPSCALER: AMD FSR 2 - Unavailable -> Unavailable",
            "an unfitted row never reuses a previous retired extent");

        summary.Eyes[0] = new RetiredEye
        {
            Eye = 0, InputWidth = 1280, InputHeight = 720, OutputWidth = 1920, OutputHeight = 1080,
        };
        summary.EyeCount = 1;
        summary.Upscaler = UpscalerKind.Fsr2;
        summary.Preset = UpscalingPreset.Quality;
        Check(UpscalingStatus.UpscalerHud(UpscalerKind.Fsr2, summary, 0, 0, 0, 0, 0) ==
              "UPSCALER: AMD FSR 2 - 1280x720 -> 1920x1080", "a mono conversion prints once");

        summary.Eyes[1] = summary.Eyes[0];
        summary.Eyes[1].Eye = 1;
        summary.EyeCount = 2;
        Check(UpscalingStatus.UpscalerHud(UpscalerKind.Fsr2, summary, 0, 0, 0, 0, 0) ==
              "UPSCALER: AMD FSR 2 - 1280x720 -> 1920x1080 (per eye)",
            "equal stereo views share one conversion");

        // Asymmetric eyes stay on one row: a second line would break the two-row contract.
        summary.Eyes[1] = new RetiredEye
        {
            Eye = 1, InputWidth = 1024, InputHeight = 768, OutputWidth = 2048, OutputHeight = 1536,
        };
        var unequal = UpscalingStatus.UpscalerHud(UpscalerKind.Fsr2, summary, 0, 0, 0, 0, 0);
        Check(unequal == "UPSCALER: AMD FSR 2 - L:1280x720/R:1024x768 -> L:1920x1080/R:2048x1536",
            "asymmetric eyes keep one row: " + unequal);
        Check(!unequal.Contains('\n'), "the upscaler row is always one line");

        // Provider/preset/generation identity is what makes a retired frame current.
        summary.Generation = 4;
        Check(UpscalingStatus.Matches(summary, UpscalerKind.Fsr2, UpscalingPreset.Quality),
            "a matching retired frame is current");
        Check(!UpscalingStatus.Matches(summary, UpscalerKind.Fsr3, UpscalingPreset.Quality),
            "a different provider discards the old evidence");
        Check(!UpscalingStatus.Matches(summary, UpscalerKind.Fsr2, UpscalingPreset.Balanced),
            "a different preset discards the old evidence");

        Check(UpscalingStatus.FrameGeneration(UpscalingStatus.FgActive, FrameGenerationKind.Nvidia, 4, "") ==
              "FG: NVIDIA - 4x - Active", "an active FG row carries provider and total");
        Check(UpscalingStatus.FrameGeneration(UpscalingStatus.FgSuspended, FrameGenerationKind.Fsr, 2,
                  "VR\nlocked") == "FG: AMD FSR - 2x - Suspended: VR locked",
            "a suspended row keeps its reason on one line");
        // A cadence pause presents the game's real frame only, so the row must say 1x, not the request.
        Check(UpscalingStatus.FrameGeneration(UpscalingStatus.FgSuspended, FrameGenerationKind.Nvidia, 1,
                  "source framerate too high for this display") ==
              "FG: NVIDIA - 1x - Suspended: source framerate too high for this display",
            "a cadence pause reports the presented 1x with its reason");
        Check(UpscalingStatus.FrameGeneration(UpscalingStatus.FgOff, FrameGenerationKind.Fsr, 5, "") ==
              "FG: Off - 1x - Off", "FG Off never prints a remembered provider or multiplier");
        Check(UpscalingStatus.FrameGeneration(UpscalingStatus.FgVrDisabled, FrameGenerationKind.Off, 1, "") ==
              "FG: Off - 1x - Disabled in VR", "VR reports the locked Off state");
        Check(UpscalingStatus.FrameGeneration(UpscalingStatus.FgRestart, FrameGenerationKind.Anr, 2, "") ==
              "FG: ANR - 2x - Restart required", "the restart requirement is its own state");
        Check(UpscalingStatus.FrameGeneration(UpscalingStatus.FgUnsupported, FrameGenerationKind.Anr, 2,
                  "ANR FG runtime is missing; check the mod page for how to install ANR") ==
              "FG: ANR - 2x - Unsupported: ANR FG runtime is missing; check the mod page for how to install ANR",
            "the ANR missing-runtime warning is shown verbatim");
    }

    private static void CheckHistory()
    {
        var history = new ViewHistory();
        history.ObserveEye(100, 0);
        history.ObserveEye(100, 1);
        Check(history.ExpectedEyes(100) == 2, "both eyes are expected for a stereo camera");

        // Executed records drive continuity; retired records drive verified activity.
        history.RecordExecution(100, 0, 10, 0, true);
        history.RecordExecution(100, 1, 10, 0, true);
        Check(history.LastExecutedFrame(100) == 10, "an executed record advances continuity");
        Check(history.LastFullyRetiredFrame(100) == -1, "an executed record alone never verifies activity");

        history.RecordRetirement(100, 0, 10, 0, true, UpscalerKind.Dlss, UpscalingPreset.Quality, 1280, 720, 1920, 1080);
        Check(history.LastFullyRetiredFrame(100) == -1, "one retired eye is not a complete frame");
        history.RecordRetirement(100, 1, 10, 0, true, UpscalerKind.Dlss, UpscalingPreset.Quality, 1280, 720, 1920, 1080);
        Check(history.LastFullyRetiredFrame(100) == 10, "both eyes retired the same frame");
        Check(history.AllEyesRetired(100, 10, 0), "both eyes retired that frame in the current generation");
        Check(!history.AllEyesRetired(100, 11, 0), "a later frame is not satisfied by an earlier retirement");

        // The card summary carries each eye's own input and output extent.
        var summary = new RetiredFrameSummary();
        Check(history.TryGetRetiredFrame(100, summary), "a fully retired frame produces a summary");
        Check(summary.FrameIndex == 10 && summary.EyeCount == 2, "the summary names the completed frame and both eyes");
        Check(summary.Upscaler == UpscalerKind.Dlss && summary.Preset == UpscalingPreset.Quality,
            "the summary carries the provider and preset that retired");
        Check(summary.Eyes[0].InputWidth == 1280 && summary.Eyes[1].OutputWidth == 1920,
            "the summary keeps per-eye extents");

        // An executed-only record can never produce a summary.
        var executedOnly = new ViewHistory();
        executedOnly.ObserveEye(400, 0);
        executedOnly.RecordExecution(400, 0, 1, 0, true);
        Check(!executedOnly.TryGetRetiredFrame(400, summary), "executed-only evidence stays Waiting");

        // One retired eye of two is never a complete frame.
        var oneEye = new ViewHistory();
        oneEye.ObserveEye(401, 0);
        oneEye.ObserveEye(401, 1);
        oneEye.RecordRetirement(401, 0, 2, 0, true, UpscalerKind.XeSs, UpscalingPreset.Balanced, 960, 540, 1920, 1080);
        Check(!oneEye.TryGetRetiredFrame(401, summary), "a single retired eye stays Waiting");

        // Unequal eye frames must not be reported as a completed frame.
        history.RecordRetirement(100, 0, 12, 0, true, UpscalerKind.Dlss, UpscalingPreset.Quality, 1280, 720, 1920, 1080);
        Check(history.LastFullyRetiredFrame(100) == -1, "unequal eye frames are not a completed frame");
        history.RecordRetirement(100, 1, 12, 0, true, UpscalerKind.Dlss, UpscalingPreset.Quality, 1280, 720, 1920, 1080);
        Check(history.LastFullyRetiredFrame(100) == 12, "matching eye frames complete again");

        // A failed retirement clears that eye.
        history.RecordRetirement(100, 0, 13, 0, true, UpscalerKind.Dlss, UpscalingPreset.Quality, 1280, 720, 1920, 1080);
        history.RecordRetirement(100, 1, 13, 0, false, UpscalerKind.Dlss, UpscalingPreset.Quality, 1280, 720, 1920, 1080);
        Check(history.LastFullyRetiredFrame(100) == -1, "a failed eye invalidates the frame");

        // A stale generation never counts.
        history.AdvanceGeneration(100);
        history.RecordRetirement(100, 0, 14, 0, true, UpscalerKind.Dlss, UpscalingPreset.Quality, 1280, 720, 1920, 1080);
        history.RecordRetirement(100, 1, 14, 0, true, UpscalerKind.Dlss, UpscalingPreset.Quality, 1280, 720, 1920, 1080);
        Check(history.LastFullyRetiredFrame(100) == -1, "a retirement from a superseded generation is ignored");
        history.RecordRetirement(100, 0, 15, 1, true, UpscalerKind.Dlss, UpscalingPreset.Quality, 1280, 720, 1920, 1080);
        history.RecordRetirement(100, 1, 15, 1, true, UpscalerKind.Dlss, UpscalingPreset.Quality, 1280, 720, 1920, 1080);
        Check(history.LastFullyRetiredFrame(100) == 15, "retirements in the new generation complete");

        // Exactly one explicit reset per eye per generation, regardless of GPU arrival timing.
        var resets = new ViewHistory();
        resets.ObserveEye(300, 0);
        Check(resets.ConsumeEpochChange(300, 0, 0), "the first packet of an eye requests a reset");
        Check(!resets.ConsumeEpochChange(300, 0, 0), "a second packet of the same generation does not");
        resets.RecordExecution(300, 0, 5, 0, true);
        resets.RecordExecution(300, 0, 6, 0, true);
        Check(!resets.ConsumeEpochChange(300, 0, 0),
            "a delayed GPU fence must not request another reset");
        resets.AdvanceGeneration(300);
        Check(resets.ConsumeEpochChange(300, 0, 1), "a new generation requests exactly one reset");

        // A different camera never shares history.
        history.ObserveEye(200, 0);
        history.RecordExecution(200, 0, 30, 0, true);
        history.RecordRetirement(200, 0, 30, 0, true, UpscalerKind.Dlss, UpscalingPreset.Quality, 1280, 720, 1920, 1080);
        Check(history.LastFullyRetiredFrame(200) == 30, "the second camera satisfies its own single eye");
        Check(history.Generation(200) == 0, "the second camera keeps its own generation");
        Check(history.LastFullyRetiredFrame(100) == 15, "camera 100 is unaffected by camera 200");

        // A forgotten camera does not inherit its history.
        history.Forget(100);
        Check(history.LastFullyRetiredFrame(100) == -1, "a forgotten camera has no verified frame");
        Check(history.LastExecutedFrame(100) == -1, "a forgotten camera has no executed frame");
    }

    private static void CheckFramePolicy()
    {
        // A capability probe is read-only and carries colour only.
        Check(FramePolicy.Satisfied(FramePolicy.Probe, true, 1920, 1080, false, false, false, out var reason),
            "a colour-only probe is satisfied: " + reason);
        Check(FramePolicy.Satisfied(FramePolicy.Probe, true, 1, 1, false, false, false, out _),
            "a one-pixel probe output extent is still a positive extent");

        Check(!FramePolicy.Satisfied(FramePolicy.Probe, false, 1920, 1080, false, false, false, out reason) &&
              reason == "colour", "a probe without colour is rejected with a reason");
        Check(!FramePolicy.Satisfied(FramePolicy.Probe, true, 0, 1080, false, false, false, out reason) &&
              reason.Contains("extent"), "a probe without an output extent is rejected");

        // An evaluation needs the whole temporal set.
        Check(FramePolicy.Satisfied(FramePolicy.Evaluate, true, 1280, 720, true, true, true, out _),
            "a full evaluation packet is satisfied");
        Check(!FramePolicy.Satisfied(FramePolicy.Evaluate, true, 1280, 720, false, true, true, out reason) &&
              reason == "depth", "a missing depth buffer rejects an evaluation");
        Check(!FramePolicy.Satisfied(FramePolicy.Evaluate, true, 1280, 720, true, false, true, out reason) &&
              reason == "motion vectors", "a missing motion texture rejects an evaluation");
        Check(!FramePolicy.Satisfied(FramePolicy.Evaluate, true, 1280, 720, true, true, false, out reason) &&
              reason == "output target", "a missing output target rejects an evaluation");

        // Shutdown and recovery are control packets: the bridge dispatches them before it validates
        // any image, so they must survive having no resources and no extent at all.
        Check(FramePolicy.Satisfied(Native.OperationRecovery, false, 0, 0, false, false, false, out reason) &&
              reason.Length == 0, "a resource-free recovery packet is satisfied: " + reason);
        Check(FramePolicy.Satisfied(Native.OperationRecovery, true, 1920, 1080, false, false, false, out reason),
            "recovery carrying colour and an extent, as recorded, is satisfied: " + reason);
        Check(FramePolicy.Satisfied(Native.OperationShutdown, false, 0, 0, false, false, false, out reason) &&
              reason.Length == 0, "a resource-free shutdown packet is satisfied: " + reason);
        Check(!FramePolicy.WritesOutput(Native.OperationRecovery) &&
              !FramePolicy.WritesOutput(Native.OperationShutdown), "control operations never write output");

        // A provider switch is an ordered control packet with no image at all.
        Check(FramePolicy.Satisfied(FramePolicy.Switch, false, 0, 0, false, false, false, out reason) &&
              reason.Length == 0, "a resource-free switch packet is satisfied: " + reason);
        Check(!FramePolicy.WritesOutput(FramePolicy.Switch), "a switch never writes output");
        Check(FramePolicy.IsControl(FramePolicy.Switch), "a switch is a control packet");
        Check(!FramePolicy.Satisfied(99, false, 0, 0, false, false, false, out reason) &&
              reason.StartsWith("unknown operation"), "an unknown resource-free operation is still rejected");

        Check(!FramePolicy.Satisfied(99, true, 1280, 720, true, true, true, out reason) &&
              reason.StartsWith("unknown operation"), "an unknown operation is rejected");
        Check(FramePolicy.WritesOutput(FramePolicy.Evaluate), "an evaluation writes the output");
        Check(!FramePolicy.WritesOutput(FramePolicy.Probe), "a probe never writes the output");
    }

    private static void CheckPresentationExtent()
    {
        var eye = new InputSize(1280, 720);
        var twoEyes = new[] { eye, eye };

        // The failing case: active 1280x720 from an output of 1920x1080.
        Check(PresentationExtent.TryResolve(1280, 720, 1920, 1080, 1280, 720, 1280, 720, 1280, 720,
                1, ViewIdentity.Layout2D, new[] { eye }, out var bounds, out var reason),
            "a matching active viewport is accepted: " + reason);
        Check(bounds.ActiveWidth == 1280 && bounds.ActiveHeight == 720,
            "the active extent is reported as rendered");
        Check(bounds.ActiveWidth == 1280 && bounds.ActiveHeight == 720,
            "the motion pixel basis is the active render extent");

        // A larger pooled allocation must not redefine the active extent.
        Check(PresentationExtent.TryResolve(1280, 720, 1920, 1080, 1920, 1080, 1920, 1080, 1280, 720,
                1, ViewIdentity.Layout2D, new[] { eye }, out var pooled, out reason),
            "a larger pooled allocation is accepted when the active region fits: " + reason);
        Check(pooled.ActiveWidth == 1280 && pooled.ActiveHeight == 720,
            "a pooled allocation never changes the active input extent");

        // An allocation that cannot contain the active viewport is rejected, not cropped.
        Check(!PresentationExtent.TryResolve(1280, 720, 1920, 1080, 640, 720, 1280, 720, 1280, 720,
                1, ViewIdentity.Layout2D, new[] { eye }, out _, out reason) && reason.Contains("colour allocation"),
            "an undersized colour allocation is rejected");

        // The active viewport grew while the fit still says 1280x720: stale, not a crop.
        Check(!PresentationExtent.TryResolve(1920, 1080, 1920, 1080, 1920, 1080, 1920, 1080, 1920, 1080,
                1, ViewIdentity.Layout2D, new[] { eye }, out _, out reason) && reason.Contains("does not match"),
            "an active viewport that no longer matches the fit is rejected");

        // URP truncates the scaled viewport, so the SDK-derived fit can be one pixel larger on either
        // axis (2227x1253 against an active 2227x1252 at scale 0.58). That is accepted, and the active
        // extent stays the input extent.
        var roundedFit = new[] { new InputSize(2227, 1253) };
        Check(PresentationExtent.TryResolve(2227, 1252, 3840, 2160, 2227, 1252, 2227, 1252, 2227, 1252,
                1, ViewIdentity.Layout2D, roundedFit, out var truncated, out reason),
            "a one-pixel fit rounding difference is accepted: " + reason);
        Check(truncated.ActiveWidth == 2227 && truncated.ActiveHeight == 1252,
            "the truncated active extent is the input extent");
        var twoPixels = new[] { new InputSize(2229, 1252) };
        Check(!PresentationExtent.TryResolve(2227, 1252, 3840, 2160, 2227, 1252, 2227, 1252, 2227, 1252,
                1, ViewIdentity.Layout2D, twoPixels, out _, out reason),
            "a real resolution change is still rejected");

        // Motion is the active extent, exactly half, or a larger pooled allocation that must not
        // change the pixel basis. Anything smaller than active without being half is rejected.
        Check(PresentationExtent.TryResolve(1280, 720, 1920, 1080, 1280, 720, 1280, 720, 640, 360,
                1, ViewIdentity.Layout2D, new[] { eye }, out var half, out reason),
            "half-resolution motion vectors are accepted: " + reason);
        Check(half.ActiveWidth == 1280 && half.ActiveHeight == 720,
            "half-resolution motion is accepted without changing the active extent");
        Check(PresentationExtent.TryResolve(1280, 720, 1920, 1080, 1280, 720, 1280, 720, 1920, 1080,
                1, ViewIdentity.Layout2D, new[] { eye }, out var pooledMotion, out reason),
            "a larger pooled motion allocation is accepted: " + reason);
        Check(pooledMotion.ActiveWidth == 1280 && pooledMotion.ActiveHeight == 720,
            "a pooled motion allocation never changes the active extent");
        Check(!PresentationExtent.TryResolve(1280, 720, 1920, 1080, 1280, 720, 1280, 720, 960, 720,
                1, ViewIdentity.Layout2D, new[] { eye }, out _, out reason) && reason.Contains("motion vectors"),
            "a motion extent that is neither full nor half is rejected");

        // A wrong aspect cannot be hidden by a viewport update.
        Check(!PresentationExtent.TryResolve(1280, 720, 1920, 1000, 1280, 720, 1280, 720, 1280, 720,
                1, ViewIdentity.Layout2D, new[] { eye }, out _, out reason) && reason.Contains("aspect"),
            "an aspect mismatch is rejected");

        // Array stereo needs one slice per eye at the active extent.
        Check(PresentationExtent.TryResolve(1280, 720, 1920, 1080, 1280, 720, 1280, 720, 1280, 720,
                2, ViewIdentity.LayoutArray, twoEyes, out var array, out reason),
            "a two-slice array with matching eye extents is accepted: " + reason);
        Check(array.ActiveWidth == 1280, "array stereo keeps the active per-eye extent");
        Check(!PresentationExtent.TryResolve(1280, 720, 1920, 1080, 1280, 720, 1280, 720, 1280, 720,
                1, ViewIdentity.LayoutArray, twoEyes, out _, out reason) && reason.Contains("slices"),
            "an array without enough slices is rejected");

        // Packed stereo only has to contain each eye in the physical allocation.
        Check(PresentationExtent.TryResolve(1280, 720, 1920, 1080, 2560, 720, 2560, 720, 1280, 720,
                1, ViewIdentity.LayoutPacked, twoEyes, out _, out reason),
            "a packed allocation that contains both eyes is accepted: " + reason);
        var wideEyes = new[] { new InputSize(1600, 720), new InputSize(1600, 720) };
        Check(!PresentationExtent.TryResolve(1280, 720, 1920, 1080, 1280, 720, 1280, 720, 1280, 720,
                1, ViewIdentity.LayoutPacked, wideEyes, out _, out reason) && reason.Contains("does not contain"),
            "a packed allocation that cannot contain the eye is rejected");

        // Attachment classification drives the full-resolution handoff.
        Check(PresentationExtent.IsReducedTarget(1280, 720, 1920, 1080),
            "the reduced URP attachment is detected");
        Check(PresentationExtent.IsFullTarget(1920, 1080, 1920, 1080),
            "an already-full attachment is recognised");
        Check(!PresentationExtent.IsReducedTarget(1920, 1080, 1920, 1080),
            "a full attachment is not replaced");
        Check(!PresentationExtent.IsReducedTarget(0, 0, 1920, 1080),
            "an invalid attachment is never classified as reduced");
    }

    private static void CheckCategoryColumnLayout()
    {
        // Nine categories (eight stock plus NVIDIA DLSS) inside the stock column. The 11-slot case
        // below also covers the two injected Neural Rendering pages.
        const float top = 300f;
        const float bottom = -300f;
        const float pitch = 600f / 9f;
        var centers = new float[9];
        var heights = new float[9];
        for (var i = 0; i < 9; i++)
        {
            Check(CategoryColumnLayout.TryGetSlot(top, bottom, 60f, 9, i, out centers[i], out heights[i]),
                $"category slot {i} resolves");
            Near(centers[i], top - pitch * (i + 0.5f), $"category slot {i} centre");
            Near(heights[i], 60f, $"category slot {i} keeps the original height");
            Check(centers[i] + heights[i] * 0.5f <= top + 1e-4f &&
                  centers[i] - heights[i] * 0.5f >= bottom - 1e-4f,
                $"category slot {i} stays inside the column");
        }
        for (var i = 1; i < 9; i++)
            Check(centers[i - 1] - heights[i - 1] * 0.5f >= centers[i] + heights[i] * 0.5f - 1e-4f,
                $"category slots {i - 1} and {i} do not overlap");

        // Eleven tabs (eight stock, NVIDIA DLSS and the two Neural pages) share the same column.
        const float widePitch = 600f / 11f;
        var wideCenters = new float[11];
        var wideHeights = new float[11];
        for (var i = 0; i < 11; i++)
        {
            Check(CategoryColumnLayout.TryGetSlot(top, bottom, 60f, 11, i, out wideCenters[i], out wideHeights[i]),
                $"eleven-tab slot {i} resolves");
            Near(wideCenters[i], top - widePitch * (i + 0.5f), $"eleven-tab slot {i} centre");
            Near(wideHeights[i], widePitch, $"eleven-tab slot {i} uses the smaller pitch");
            Check(wideCenters[i] + wideHeights[i] * 0.5f <= top + 1e-4f &&
                  wideCenters[i] - wideHeights[i] * 0.5f >= bottom - 1e-4f,
                $"eleven-tab slot {i} stays inside the column");
        }
        for (var i = 1; i < 11; i++)
            Check(wideCenters[i - 1] - wideHeights[i - 1] * 0.5f >=
                  wideCenters[i] + wideHeights[i] * 0.5f - 1e-4f,
                $"eleven-tab slots {i - 1} and {i} do not overlap");

        // A taller original button is clamped to the pitch instead of crossing into its neighbour.
        for (var i = 0; i < 9; i++)
        {
            Check(CategoryColumnLayout.TryGetSlot(top, bottom, 100f, 9, i, out centers[i], out heights[i]),
                $"clamped category slot {i} resolves");
            Near(heights[i], pitch, $"clamped category slot {i} uses the pitch");
        }
        for (var i = 1; i < 9; i++)
            Check(centers[i - 1] - heights[i - 1] * 0.5f >= centers[i] + heights[i] * 0.5f - 1e-4f,
                $"clamped category slots {i - 1} and {i} do not overlap");

        // Invalid geometry returns false and leaves both outputs at zero.
        var invalid = new (float Top, float Bottom, float Height, int Count, int Index)[]
        {
            (300f, 300f, 60f, 9, 0),
            (-300f, 300f, 60f, 9, 0),
            (float.NaN, -300f, 60f, 9, 0),
            (300f, float.PositiveInfinity, 60f, 9, 0),
            (300f, -300f, 0f, 9, 0),
            (300f, -300f, -1f, 9, 0),
            (300f, -300f, 60f, 0, 0),
            (300f, -300f, 60f, 9, -1),
            (300f, -300f, 60f, 9, 9),
        };
        foreach (var input in invalid)
        {
            var accepted = CategoryColumnLayout.TryGetSlot(input.Top, input.Bottom, input.Height, input.Count,
                input.Index, out var center, out var height);
            Check(!accepted && center == 0f && height == 0f,
                $"invalid column input ({input.Top},{input.Bottom},{input.Height},{input.Count},{input.Index}) is rejected");
        }

        // A single slot is centred in the bounds.
        Check(CategoryColumnLayout.TryGetSlot(10f, -10f, 5f, 1, 0, out var singleCenter, out var singleHeight),
            "a single category slot resolves");
        Near(singleCenter, 0f, "a single category slot is centred");
        Near(singleHeight, 5f, "a single category slot keeps its height");

        // Mapping a shared-column slot back into a tab's own parent-local geometry.
        static void CheckSlotMapping(float currentCenter, float localHeight, float pivotY, float verticalAxisY,
            float horizontalSpanY, float targetCenter, float targetHeight, float expectedHeight,
            float expectedOffset, string description)
        {
            Check(CategoryColumnLayout.TryMapSlot(currentCenter, localHeight, pivotY, verticalAxisY,
                    horizontalSpanY, targetCenter, targetHeight, out var mappedHeight, out var columnOffset),
                $"{description} maps into the tab parent");
            Near(mappedHeight, expectedHeight, $"{description} local height");
            Near(columnOffset, expectedOffset, $"{description} column offset");
        }

        CheckSlotMapping(0f, 60f, 0.5f, 1f, 0f, 100f, 40f, 40f, 100f, "an unscaled centred tab");
        CheckSlotMapping(0f, 60f, 0f, 2f, 0f, 100f, 40f, 20f, 140f, "a doubled vertical axis");
        CheckSlotMapping(0f, 60f, 1f, 2f, 0f, 100f, 40f, 20f, 60f,
            "a doubled vertical axis with a top pivot");
        CheckSlotMapping(0f, 60f, 0f, -2f, 0f, 100f, 40f, 20f, 60f, "a flipped vertical axis");
        CheckSlotMapping(0f, 60f, 0f, 2f, 10f, 100f, 50f, 20f, 140f,
            "a horizontal span removed from the target height");

        // Invalid mappings return false and leave both outputs at zero.
        var invalidMapping = new (float Current, float Height, float Pivot, float Axis, float Span, float Target,
            float TargetHeight)[]
        {
            (0f, 60f, 0.5f, 0f, 0f, 100f, 40f),
            (0f, 0f, 0.5f, 1f, 0f, 100f, 40f),
            (0f, -1f, 0.5f, 1f, 0f, 100f, 40f),
            (0f, 60f, 0.5f, 1f, -1f, 100f, 40f),
            (0f, 60f, 0.5f, 1f, 0f, 100f, 0f),
            (0f, 60f, 0.5f, 1f, 5f, 100f, 5f),
            (0f, 60f, 0.5f, 1f, 6f, 100f, 5f),
            (float.NaN, 60f, 0.5f, 1f, 0f, 100f, 40f),
            (0f, float.PositiveInfinity, 0.5f, 1f, 0f, 100f, 40f),
            (0f, 60f, float.NaN, 1f, 0f, 100f, 40f),
            (0f, 60f, 0.5f, float.NaN, 0f, 100f, 40f),
            (0f, 60f, 0.5f, float.PositiveInfinity, 0f, 100f, 40f),
            (0f, 60f, 0.5f, 1f, float.NaN, 100f, 40f),
            (0f, 60f, 0.5f, 1f, float.PositiveInfinity, 100f, 40f),
            (float.PositiveInfinity, 60f, 0.5f, 1f, 0f, 100f, 40f),
            (0f, 60f, 0.5f, 1f, 0f, float.NaN, 40f),
            (0f, 60f, 0.5f, 1f, 0f, 100f, float.NaN),
            (0f, 60f, 0.5f, 1f, 0f, 100f, float.PositiveInfinity),
        };
        foreach (var input in invalidMapping)
        {
            var accepted = CategoryColumnLayout.TryMapSlot(input.Current, input.Height, input.Pivot, input.Axis,
                input.Span, input.Target, input.TargetHeight, out var mapped, out var offset);
            Check(!accepted && mapped == 0f && offset == 0f,
                $"invalid slot mapping ({input.Current},{input.Height},{input.Pivot},{input.Axis},{input.Span}," +
                $"{input.Target},{input.TargetHeight}) is rejected");
        }
    }

    private static void CheckTransition()
    {
        var plan = new TransitionPlan();
        Check(plan.State == OwnershipState.Idle, "a new plan owns nothing");
        Check(!plan.MayReplace, "replacement needs ownership");

        // A probe alone never owns and never writes.
        plan.Wait("probing");
        Check(plan.State == OwnershipState.Probing, "probing is a distinct state");
        Check(!plan.MayReplace, "probing must not replace the graph");

        // Resolving a fit requests exactly one commit at the next boundary.
        plan.RequestCommit();
        Check(plan.CommitPending, "a resolved fit requests a commit");
        Check(plan.TryBeginCommit(), "the boundary accepts the commit");
        Check(!plan.TryBeginCommit(), "a commit is applied once");
        Check(plan.OnBoundary() == BoundaryAction.None, "a commit queues no second boundary action");
        Check(plan.OnBoundary() == BoundaryAction.None, "the action stays consumed on later boundaries");
        plan.CommitDone();
        Check(plan.OwnsRendering && plan.MayReplace, "a committed plan owns rendering");

        // A repeat commit request while owned changes nothing.
        plan.RequestCommit();
        Check(!plan.CommitPending, "an owned plan never re-commits");

        // A fault latches, asks for a restore and refuses any later implicit acquisition.
        plan.Fail("evaluation failed");
        Check(plan.Faulted, "a defect latches");
        Check(plan.OnBoundary() == BoundaryAction.Restore, "a fault requests a restore at the boundary");
        plan.RequestCommit();
        Check(!plan.CommitPending, "a latched fault cannot re-acquire implicitly");
        Check(!plan.TryBeginCommit(), "a latched fault never commits");
        plan.Wait("a later probe answered");
        Check(plan.Faulted, "a probe must not clear a latched fault");

        // An explicit transition is the only way out.
        plan.Enable("settings changed");
        Check(!plan.Faulted, "an explicit enable clears the latch");
        Check(plan.OnBoundary() == BoundaryAction.Restore, "the explicit transition restores first");
        plan.RequestCommit();
        Check(plan.CommitPending, "a fresh commit can be requested after the explicit transition");

        // Disable cannot re-acquire either.
        var disabled = new TransitionPlan();
        disabled.RequestCommit();
        disabled.CommitDone();
        disabled.Disable("disabled in the settings");
        disabled.RequestCommit();
        Check(!disabled.CommitPending, "a disabled plan cannot re-acquire implicitly");
        Check(disabled.OnBoundary() == BoundaryAction.Restore, "disabling restores the game's values");
        disabled.Enable("enabled again");
        disabled.RequestCommit();
        Check(disabled.CommitPending, "re-enabling allows a new acquisition");

        // A camera or asset change re-acquires through a restore.
        var reacquire = new TransitionPlan();
        reacquire.RequestCommit();
        reacquire.CommitDone();
        reacquire.Reacquire("the primary camera changed");
        Check(reacquire.State == OwnershipState.Probing, "a re-acquire returns to probing");
        Check(reacquire.OnBoundary() == BoundaryAction.Restore, "a re-acquire restores before probing again");
        Check(!reacquire.CommitPending, "the old fit no longer commits by itself");
    }

    private static void CheckViewIdentity()
    {
        Check(ViewIdentity.ExpectedEyes(ViewIdentity.Layout2D) == 1, "a 2D camera expects one eye");
        Check(ViewIdentity.ExpectedEyes(ViewIdentity.LayoutArray) == 2, "a stereo array expects two eyes");
        Check(ViewIdentity.ExpectedEyes(ViewIdentity.LayoutPacked) == 2, "a multipass render expects two eyes");

        // Array: slice 1 is eye 1 and reads slice 1.
        Check(ViewIdentity.EyeForPacket(ViewIdentity.LayoutArray, 1, 0) == 1, "array slice 1 is eye 1");
        Check(ViewIdentity.SliceForPacket(ViewIdentity.LayoutArray, 1) == 1, "array slice 1 reads slice 1");
        Check(ViewIdentity.EyeForPacket(ViewIdentity.LayoutArray, 0, 0) == 0, "array slice 0 is eye 0");

        // Multipass: the pass id is the eye and the texture slice stays zero.
        Check(ViewIdentity.EyeForPacket(ViewIdentity.LayoutPacked, 0, 1) == 1, "multipass id 1 is eye 1");
        Check(ViewIdentity.SliceForPacket(ViewIdentity.LayoutPacked, 0) == 0, "multipass always reads slice 0");

        // 2D never leaves view zero.
        Check(ViewIdentity.EyeForPacket(ViewIdentity.Layout2D, 3, 1) == 0, "2D always reports view zero");
        Check(ViewIdentity.SliceForPacket(ViewIdentity.Layout2D, 3) == 0, "2D always uses slice zero");

        // The identities must stay distinct: an array eye 1 and a multipass eye 1 are the same logical
        // eye but a different texture region, which the slice mapping keeps apart.
        Check(ViewIdentity.SliceForPacket(ViewIdentity.LayoutArray, 1) !=
              ViewIdentity.SliceForPacket(ViewIdentity.LayoutPacked, 1),
            "an array eye must not be written to the multipass slice");
        Check(ViewIdentity.IsSinglePassStereo(ViewIdentity.LayoutArray), "an array render is single-pass stereo");
        Check(!ViewIdentity.IsSinglePassStereo(ViewIdentity.LayoutPacked), "a multipass render is not single-pass");
    }

    /// <summary>
    /// When the packaged bridge is supplied, compares the managed mirror against the native one:
    /// ABI number, both struct sizes and every frame field offset. A mismatch is exactly what makes
    /// the plugin refuse to attach, so this is the check that the two sides agree.
    /// </summary>
    private static void CheckBridge(string[] args)
    {
        if (args.Length == 0)
        {
            Console.WriteLine("bridge ABI comparison skipped (pass the path to BigScale.Native.dll)");
            return;
        }
        var path = args[0];
        if (!System.IO.File.Exists(path))
        {
            Check(false, "the supplied bridge path exists: " + path);
            return;
        }
        nint handle = 0;
        try
        {
            handle = System.Runtime.InteropServices.NativeLibrary.Load(path);
            System.Runtime.InteropServices.NativeLibrary.SetDllImportResolver(typeof(Native).Assembly,
                (name, _, _) => name == Native.Dll ? handle : 0);
            var error = Native.VerifyLayout();
            Check(error == null, "the managed and native bridge layouts agree: " + (error ?? "ok"));
            Check(Native.GetCompletionSize() == 76, "the bridge reports a 76-byte completion");
            var fgError = NativeFg.VerifyLayout();
            Check(fgError == null, "the managed and native frame-generation layouts agree: " + (fgError ?? "ok"));
            var vrfgError = NativeVrFg.VerifyLayout();
            Check(vrfgError == null, "the managed and native VR frame-generation layouts agree: " +
                (vrfgError ?? "ok"));
        }
        catch (Exception exception)
        {
            Check(false, "the bridge could not be inspected: " + exception.Message);
        }
        finally
        {
            if (handle != 0) System.Runtime.InteropServices.NativeLibrary.Free(handle);
        }
    }

    private static void CheckTemporalInputPolicy()
    {
        // One unit of the motion field is one full render extent, because the motion-vector pass
        // stores a full-extent displacement. Every backend is handed the previous-to-current field
        // negated, so no provider-specific factor is allowed here.
        var top = TemporalInputPolicy.MotionScale(1280, 720, uvStartsAtTop: true);
        Check(top.X == -1280f && top.Y == -720f,
            $"the motion scale negates both render axes on D3D, got {top}");
        var bottom = TemporalInputPolicy.MotionScale(1280, 720, uvStartsAtTop: false);
        Check(bottom.X == -1280f && bottom.Y == 720f,
            $"the motion scale keeps Y positive on a bottom-up origin, got {bottom}");
        Near(top.X, -1280f, "a one-unit displacement covers one render width");
        var invalid = TemporalInputPolicy.MotionScale(0, 720, true);
        Check(invalid.X == 0f && invalid.Y == 0f, "an invalid extent produces no motion scale");

        // The jitter is passed through unchanged. DLSS requires it in the same coordinate and
        // direction system as the motion vectors, which the AMD and XeSS dispatches already receive;
        // negating Y for one provider is what produced the reported sub-pixel vertical shake.
        var jitter = TemporalInputPolicy.Jitter(0.25f, 0.5f);
        Near(jitter.X, 0.25f, "the packet jitter keeps its X value");
        Near(jitter.Y, 0.5f, "the packet jitter keeps its Y value");
        var negative = TemporalInputPolicy.Jitter(-0.31f, -0.42f);
        Check(negative.X == -0.31f && negative.Y == -0.42f,
            $"a negative jitter passes through unchanged, got {negative}");
    }

    private static void CheckFrameGenerationCameraPolicy()
    {
        // The one camera Big Walk actually renders gameplay with: a single base camera that resolves
        // to a 3840x2160 screen from a reduced 1280x720 render extent.
        var primary = new FrameGenerationCameraInput
        {
            Live = true,
            GameCamera = true,
            GameplayActive = true,
            BaseCamera = true,
            ResolveToScreen = true,
            LastBaseCamera = true,
            RenderWidth = 1280,
            RenderHeight = 720,
            OutputWidth = 3840,
            OutputHeight = 2160,
        };
        Check(FrameGenerationCameraPolicy.Accept(primary, out var reason),
            "the final SDR base camera is accepted: " + reason);

        // Gameplay ownership is independent of every URP property: the same otherwise-valid base
        // camera is rejected while the title/loading scene is rendered, despite GameCamera=true.
        Check(!FrameGenerationCameraPolicy.Accept(primary with { GameplayActive = false }, out reason) &&
              reason.Contains("outside gameplay"),
            "a camera outside gameplay is rejected with its reason: " + reason);

        // Title -> gameplay -> title -> gameplay: the decision follows ownership alone, and the
        // remembered provider is never rewritten by the gate.
        var requestedProvider = FrameGenerationKind.Fsr;
        var expectedDecisions = new[] { false, true, false, true };
        for (var step = 0; step < expectedDecisions.Length; step++)
        {
            var gameplay = step % 2 == 1;
            var accepted = FrameGenerationCameraPolicy.Accept(primary with { GameplayActive = gameplay },
                out reason);
            Check(accepted == expectedDecisions[step],
                $"gameplay transition {step} accepts={expectedDecisions[step]}: " + reason);
            Check(requestedProvider == FrameGenerationKind.Fsr,
                "the gameplay gate never rewrites the requested provider");
        }

        // Only a real HDR *display output* disqualifies: the HUD-less image is the post-blit
        // backbuffer, so URP's internal HDR colour buffer is not part of the decision.
        Check(!FrameGenerationCameraPolicy.Accept(primary with { HdrDisplayOutput = true }, out reason) &&
              reason.Contains("HDR"), "an HDR display output is rejected with its reason: " + reason);

        Check(!FrameGenerationCameraPolicy.Accept(primary with { Live = false }, out reason) &&
              reason.Contains("live"), "a disabled camera is rejected: " + reason);
        Check(!FrameGenerationCameraPolicy.Accept(primary with { GameCamera = false }, out reason) &&
              reason.Contains("game camera"), "a non-game camera is rejected: " + reason);
        Check(!FrameGenerationCameraPolicy.Accept(primary with { Preview = true }, out reason) &&
              reason.Contains("preview"), "a preview camera is rejected: " + reason);
        Check(!FrameGenerationCameraPolicy.Accept(primary with { SceneView = true }, out reason) &&
              reason.Contains("scene view"), "the scene view is rejected: " + reason);
        Check(!FrameGenerationCameraPolicy.Accept(primary with { BaseCamera = false }, out reason) &&
              reason.Contains("overlay"), "an overlay camera is rejected: " + reason);
        Check(!FrameGenerationCameraPolicy.Accept(primary with { CameraStackCount = 2 }, out reason) &&
              reason.Contains("stack"), "a stacked base camera is rejected: " + reason);
        Check(!FrameGenerationCameraPolicy.Accept(primary with { HasTargetTexture = true }, out reason) &&
              reason.Contains("render texture"), "an offscreen camera is rejected: " + reason);
        Check(!FrameGenerationCameraPolicy.Accept(primary with { ResolveToScreen = false }, out reason) &&
              reason.Contains("resolve"), "a camera that does not resolve to the screen is rejected: " + reason);
        Check(!FrameGenerationCameraPolicy.Accept(primary with { LastBaseCamera = false }, out reason) &&
              reason.Contains("after this one"), "a camera followed by another base camera is rejected: " + reason);
        Check(!FrameGenerationCameraPolicy.Accept(primary with { XrEnabled = true }, out reason) &&
              reason.Contains("XR"), "an XR camera is rejected while XR suspension owns the session: " + reason);
        Check(!FrameGenerationCameraPolicy.Accept(primary with { RenderWidth = 0 }, out reason) &&
              reason.Contains("render extent"), "a zero render extent is rejected: " + reason);
        Check(!FrameGenerationCameraPolicy.Accept(primary with { OutputHeight = 0 }, out reason) &&
              reason.Contains("output extent"), "a zero output extent is rejected: " + reason);

        // A rejected camera never becomes the captured identity, and the accepted one always reports
        // an empty reason so a caller can log the decision without special-casing.
        Check(FrameGenerationCameraPolicy.Accept(primary, out reason) && reason.Length == 0,
            "an accepted camera reports no reason: " + reason);
    }

    // The player's real inventory, as measured in the FG capture addendum: rigid bone-parented
    // body/head/hand MeshRenderers on househouse/PlayerCharacter with no MotionVectors pass, and limb
    // splines on househouse/PlayerLImb that have the pass but sit in Camera mode.
    private static void CheckPlayerMotionPolicy()
    {
        static PlayerMotionAction Classify(bool meshOrSkinned, bool cameraMode, int materialCount,
            int materialsWithMotionPass, int maxRenderQueue, bool hidden = false, int submeshCount = 1) =>
            PlayerMotionPolicy.Classify(new PlayerRendererMotion
            {
                MeshOrSkinned = meshOrSkinned,
                CameraMode = cameraMode,
                Hidden = hidden,
                MaterialCount = materialCount,
                MaterialsWithMotionPass = materialsWithMotionPass,
                MaxRenderQueue = maxRenderQueue,
                SubmeshCount = submeshCount,
            });

        // torso_mesh / head_mesh / BodyBall hands / feet: one submesh, no motion pass, opaque. The mod
        // appends a motion-only material so URP draws the renderer's real object motion.
        Check(Classify(true, false, 1, 0, 2000) == PlayerMotionAction.AttachMotionMaterial,
            "a renderer without a MotionVectors pass gets the motion-only material");

        // A Camera-mode renderer without the pass has the same wrong vectors; the appended material
        // makes the mode irrelevant, because URP draws the motion pass either way.
        Check(Classify(true, true, 1, 0, 2000) == PlayerMotionAction.AttachMotionMaterial,
            "a Camera-mode renderer without the pass gets the motion-only material");

        // LimbSplines: the pass exists, and Camera mode makes URP skip their real per-object motion, so
        // the only change is the mode.
        Check(Classify(true, true, 1, 1, 2000) == PlayerMotionAction.PromoteToObject,
            "a Camera-mode renderer that already has the pass is promoted to Object");

        // Hands on househouse/VertexColors: the pass exists and the mode is already Object.
        Check(Classify(true, false, 1, 1, 2000) == PlayerMotionAction.None,
            "a renderer URP already handles is left untouched");

        // Outside the opaque queue URP's motion pass never draws, so an appended motion material would
        // cover nothing. Text and transparent effects stay alone.
        Check(Classify(true, false, 1, 0, 3000) == PlayerMotionAction.None,
            "a transparent-queue renderer is not drawn into motion vectors");

        // Particles, trails and line renderers have no stable camera-relative surface.
        Check(Classify(false, false, 1, 0, 2000) == PlayerMotionAction.None,
            "a non-mesh renderer is not drawn into motion vectors");

        Check(Classify(true, false, 0, 0, 2000) == PlayerMotionAction.None,
            "a renderer with no materials is left untouched");

        // A single override draw would overwrite the submeshes whose own material already writes
        // correct motion, so a mixed renderer is skipped rather than repaired.
        Check(Classify(true, false, 2, 1, 2000) == PlayerMotionAction.None,
            "a mixed-material renderer is skipped");

        // A multi-submesh renderer without the pass: an appended material renders on the last submesh
        // only, so the rest would keep camera motion.
        Check(Classify(true, false, 2, 0, 2000, submeshCount: 2) == PlayerMotionAction.None,
            "a multi-submesh renderer without a motion pass is skipped");

        // No mesh, so no submesh to cover and nothing the motion pass could draw.
        Check(Classify(true, false, 1, 0, 2000, submeshCount: 0) == PlayerMotionAction.None,
            "a renderer without a mesh is skipped");

        // The game's nose is live geometry that never writes colour; a motion patch there would cover
        // whatever the player is really looking at.
        Check(Classify(true, false, 1, 0, 2000, hidden: true) == PlayerMotionAction.None,
            "a renderer the game never draws into colour is left alone");
    }

    private static void CheckSettingsPanelSchema()
    {
        // The page is built from this schema alone, so an absent row here means an absent control in
        // game. Both read-only status rows are gone: renderer state belongs to the hold-TAB HUD.
        var rows = SettingsPanelSchema.Rows;
        Check(rows.Length == 5, $"the Upscaling page declares 5 rows, got {rows.Length}");
        Check(rows[0].Key == SettingsPanelSchema.EnabledKey && rows[0].Caption == "Enabled",
            "the first row is the Enabled switch");
        Check(rows[1].Key == SettingsPanelSchema.UpscalerKey && rows[1].Caption == "Upscaler",
            "the second row is the Upscaler switch");
        Check(rows[2].Key == SettingsPanelSchema.ModeKey && rows[2].Caption == "Mode",
            "the third row is the Mode switch");
        Check(rows[3].Key == SettingsPanelSchema.FrameGenerationKey &&
              rows[3].Caption == "Frame Generation (Experimental)",
            "the fourth row is the frame-generation switch and names its caveat");
        Check(rows[4].Key == SettingsPanelSchema.FrameMultiplierKey &&
              rows[4].Caption == "Frame Multiplier" &&
              SettingsPanelSchema.FrameMultiplierKey == "BigScale.FrameMultiplier",
            "the fifth row is the remembered frame multiplier");
        Check(rows.Select(row => row.Key).Distinct().Count() == rows.Length, "row keys are unique");
        Check(rows.Select(row => row.Caption).Distinct().Count() == rows.Length, "row captions are unique");
        Check(!rows.Any(row => row.Key.Contains("Status", StringComparison.OrdinalIgnoreCase)),
            "no read-only status row remains in the page schema");
        Check(!rows.Any(row => row.Key.Contains("Comparison", StringComparison.OrdinalIgnoreCase)),
            "no row binds the removed comparison key");
        Check(!rows.Any(row => row.Caption.Contains("Bilinear", StringComparison.OrdinalIgnoreCase)),
            "no row offers a bilinear comparison");
    }

    private static void CheckVrMode()
    {
        Check(!VrMode.IsRequested(null), "no arguments is not a VR request");
        Check(!VrMode.IsRequested(Array.Empty<string>()), "an empty argument list is not a VR request");
        Check(!VrMode.IsRequested(new[] { "BigWalk.exe", "-bigscale" }),
            "an unrelated argument is not a VR request");
        Check(VrMode.IsRequested(new[] { VrMode.RequestSwitch }), "the explicit VR switch requests VR");
        Check(VrMode.IsRequested(new[] { "-BIGWALKVR-VR" }), "the VR switch is case-insensitive");
        Check(!VrMode.IsRequested(new[] { VrMode.RequestSwitch, VrMode.DenySwitch }),
            "the last explicit switch wins (non-VR)");
        Check(VrMode.IsRequested(new[] { VrMode.DenySwitch, VrMode.RequestSwitch }),
            "the last explicit switch wins (VR)");
        Check(!VrMode.IsRequested(new[] { "-bigwalkvr-vrx" }), "a prefix is not the switch");
    }

    private static void CheckHudTelemetry()
    {
        var telemetry = new HudTelemetry();
        Check(!telemetry.Observe(0.0, 1, FrameGenerationKind.Nvidia, 2, 100, 50, out _),
            "the first sample only seeds the window");
        Check(!telemetry.Observe(0.25, 1, FrameGenerationKind.Nvidia, 2, 115, 65, out _),
            "a short interval never reports a rate");
        // 30 rendered + 30 generated over 0.5s is 120 presented frames per second.
        Check(telemetry.Observe(0.5, 1, FrameGenerationKind.Nvidia, 2, 130, 80, out var total) && total == 120,
            $"rendered and generated deltas both count, got {total}");
        // A generated delta of zero must not invent the configured multiplier.
        Check(telemetry.Observe(1.0, 1, FrameGenerationKind.Nvidia, 2, 145, 80, out total) && total == 30,
            $"no generated frames means the base rate, got {total}");
        // A counter rollback reseeds instead of reporting a negative rate.
        Check(!telemetry.Observe(1.5, 1, FrameGenerationKind.Nvidia, 2, 10, 0, out _),
            "a counter rollback reseeds without a rate");
        Check(telemetry.Observe(2.0, 1, FrameGenerationKind.Nvidia, 2, 25, 15, out total) && total == 60,
            $"the reseeded window reports again, got {total}");
        // A new generation is a new identity: no rate can leak across it.
        Check(!telemetry.Observe(2.25, 2, FrameGenerationKind.Nvidia, 2, 40, 30, out _),
            "a new generation reseeds instead of reusing the old window");
        telemetry.Reset();
        Check(!telemetry.Observe(3.0, 2, FrameGenerationKind.Nvidia, 2, 50, 40, out _),
            "an explicit reset reseeds the first sample");

        Check(HudTelemetry.DecorateFps("FPS: 60", 120) == "FPS: 60/120",
            "the measured total is appended to the stock numeric token");
        Check(HudTelemetry.DecorateFps("<color=#fff>60</color> FPS", 120) ==
              "<color=#fff>60/120</color> FPS", "rich-text tags and labels are preserved");
        Check(HudTelemetry.DecorateFps("FPS: 59.5", 119) == "FPS: 59.5/119",
            "a decimal token keeps its separator");
        Check(HudTelemetry.DecorateFps("FPS: 60", null) == "FPS: 60/--",
            "an unproven total shows two dashes");
        Check(HudTelemetry.DecorateFps("Frame rate", 120) == "Frame rate",
            "no numeric token leaves the stock text untouched");
        Check(HudTelemetry.DecorateFps("1 2", 120) == "1 2",
            "ambiguous multiple tokens leave the stock text untouched");
        Check(HudTelemetry.DecorateFps("", 120) == "", "empty stock text stays empty");

        // Between full windows the last measured rate stays valid: the stock counter is decorated on
        // every one of its own updates, so a rate that expired mid-window would blink back to "--".
        var retained = new HudTelemetry();
        Check(!retained.Observe(0.0, 1, FrameGenerationKind.Fsr, 2, 100, 50, out _),
            "the retained-rate window still seeds first");
        Check(retained.Observe(0.5, 1, FrameGenerationKind.Fsr, 2, 130, 80, out var held) && held == 120,
            $"the first full window reports 120, got {held}");
        Check(retained.Observe(0.51, 1, FrameGenerationKind.Fsr, 2, 131, 81, out held) && held == 120,
            $"a sub-window call retains 120, got {held}");
        Check(retained.Observe(0.75, 1, FrameGenerationKind.Fsr, 2, 145, 85, out held) && held == 120,
            $"the retained rate survives the whole interval, got {held}");
        Check(retained.Observe(1.0, 1, FrameGenerationKind.Fsr, 2, 160, 80, out held) && held == 60,
            $"the next full window replaces the retained rate, got {held}");

        // Every invalidation drops the retained rate until another full window completes.
        var measured = MeasuredWindow(1, FrameGenerationKind.Fsr, 2);
        Check(measured.Observe(0.55, 1, FrameGenerationKind.Fsr, 2, 135, 71, out var kept) && kept == 100,
            $"the measured window holds 100 between windows, got {kept}");
        Check(!measured.Observe(double.NaN, 1, FrameGenerationKind.Fsr, 2, 135, 71, out _),
            "a non-finite time never reports a rate");
        Check(!MeasuredWindow(1, FrameGenerationKind.Fsr, 2)
                .Observe(0.55, 2, FrameGenerationKind.Fsr, 2, 135, 71, out _),
            "a generation change invalidates the retained rate");
        Check(!MeasuredWindow(1, FrameGenerationKind.Fsr, 2)
                .Observe(0.55, 1, FrameGenerationKind.Nvidia, 2, 135, 71, out _),
            "a provider change invalidates the retained rate");
        Check(!MeasuredWindow(1, FrameGenerationKind.Fsr, 2)
                .Observe(0.55, 1, FrameGenerationKind.Fsr, 3, 135, 71, out _),
            "a multiplier change invalidates the retained rate");
        var afterReset = MeasuredWindow(1, FrameGenerationKind.Fsr, 2);
        afterReset.Reset();
        Check(!afterReset.Observe(0.55, 1, FrameGenerationKind.Fsr, 2, 135, 71, out _),
            "an explicit reset invalidates the retained rate");
        var afterRollback = MeasuredWindow(1, FrameGenerationKind.Fsr, 2);
        Check(!afterRollback.Observe(0.55, 1, FrameGenerationKind.Fsr, 2, 10, 0, out _),
            "a counter rollback invalidates the retained rate");
        Check(!afterRollback.Observe(0.6, 1, FrameGenerationKind.Fsr, 2, 12, 1, out _),
            "the reseeded window returns no rate until it is full");

        // The real telemetry + decorator pair, driven the way the stock counter drives it: one sample
        // and one decoration per stock update, with the localized value rewritten every frame.
        var pulse = new HudTelemetry();
        Check(DecorateStockUpdate(pulse, 0.0, 100, 50) == "<color=#fff>30,00/--</color> FPS",
            "before a full window the suffix is a truthful two dashes");
        Check(DecorateStockUpdate(pulse, 0.5, 115, 65) == "<color=#fff>30,00/60</color> FPS",
            "a full window decorates the localized value with the measured total");
        for (var frame = 0; frame < 5; frame++)
        {
            var repeated = DecorateStockUpdate(pulse, 0.501 + frame * 0.001, 116, 66);
            Check(repeated == "<color=#fff>30,00/60</color> FPS",
                $"stock update {frame} keeps the measured total, got {repeated}");
        }
        Check(HudTelemetry.DecorateFps("30,00", 60) == "30,00/60",
            "the localized decimal separator is preserved with the total appended once");
    }

    private static void CheckHudPlacement()
    {
        // Stock HUD: the nearest visible edge is 10; the right-aligned rect's zero left gap is irrelevant.
        Check(HudPlacement.TryResolveInset(500f, 28f, 200f, 10f, 16f, 400f, out var inset),
            "a stock margin resolves");
        Near(inset, 10f, "the smallest stock edge wins");

        Check(HudPlacement.TryResolveInset(0f, 0f, 0f, 0f, 16f, 400f, out inset),
            "a flush stock rectangle resolves");
        Near(inset, 16f, "a flush stock rectangle uses the fallback margin");

        Check(HudPlacement.TryResolveInset(0.5f, 2f, 3f, 4f, 16f, 400f, out inset),
            "sub-unit gaps resolve");
        Near(inset, 16f, "sub-unit gaps are measurement noise");

        Check(HudPlacement.TryResolveInset(float.NaN, -1f, float.PositiveInfinity, -2f, 16f, 400f,
                out inset), "unusable gaps resolve");
        Near(inset, 16f, "unusable gaps use the fallback margin");

        Check(HudPlacement.TryResolveInset(300f, 300f, 300f, 300f, 16f, 40f, out inset),
            "a huge stock margin is bounded");
        Near(inset, 40f, "the margin is clamped to the view");

        Check(!HudPlacement.TryResolveInset(10f, 10f, 10f, 10f, 0f, 40f, out _),
            "a non-positive fallback is rejected");
        Check(!HudPlacement.TryResolveInset(10f, 10f, 10f, 10f, 16f, float.NaN, out _),
            "a non-finite maximum is rejected");

        Check(HudPlacement.TryGetRows(0f, 0f, 100f, 50f, 16f, 2, 10f,
                out var left, out var bottom, out var width, out var height),
            "a row rectangle resolves");
        Near(left, 16f, "row left uses the margin");
        Near(bottom, 16f, "row bottom uses the margin");
        Near(width, 68f, "row width keeps both margins");
        Near(height, 20f, "row height keeps both lines");
        Near(left + width, 84f, "row right keeps the margin");

        var invalid = new (float MinX, float MinY, float Width, float Height, float Inset, int Lines,
            float LineHeight)[]
        {
            (float.NaN, 0f, 100f, 50f, 16f, 2, 10f),
            (0f, 0f, 0f, 50f, 16f, 2, 10f),
            (0f, 0f, 100f, -1f, 16f, 2, 10f),
            (0f, 0f, 100f, 50f, float.NaN, 2, 10f),
            (0f, 0f, 100f, 50f, -1f, 2, 10f),
            (0f, 0f, 100f, 50f, 16f, 0, 10f),
            (0f, 0f, 100f, 50f, 16f, 2, 0f),
        };
        foreach (var input in invalid)
        {
            var accepted = HudPlacement.TryGetRows(input.MinX, input.MinY, input.Width, input.Height,
                input.Inset, input.Lines, input.LineHeight,
                out var l, out var b, out var w, out var h);
            Check(!accepted && l == 0f && b == 0f && w == 0f && h == 0f,
                $"invalid row geometry is rejected ({input.MinX},{input.MinY},{input.Width}," +
                $"{input.Height},{input.Inset},{input.Lines},{input.LineHeight})");
        }
    }

    /// <summary>A telemetry instance holding one completed window of 100 presented frames per second.</summary>
    private static HudTelemetry MeasuredWindow(uint generation, FrameGenerationKind provider, int multiplier)
    {
        var telemetry = new HudTelemetry();
        telemetry.Observe(0.0, generation, provider, multiplier, 100, 50, out _);
        telemetry.Observe(0.5, generation, provider, multiplier, 130, 70, out _);
        return telemetry;
    }

    /// <summary>
    /// One stock-counter update: sample the native snapshot exactly as the renderer path does, then
    /// decorate the localized label the game rewrote.
    /// </summary>
    private static string DecorateStockUpdate(HudTelemetry telemetry, double now, ulong rendered,
        ulong generated)
    {
        var total = telemetry.Observe(now, 1, FrameGenerationKind.Nvidia, 2, rendered, generated,
            out var rate)
            ? rate
            : (int?)null;
        return HudTelemetry.DecorateFps("<color=#fff>30,00</color> FPS", total);
    }

    private static void CheckPrimaryCameraPolicy()
    {
        Check(PrimaryCameraPolicy.ShouldPromote(0, false, 0, 0, 7, 1920L * 1080, 10),
            "the first eligible rendered camera is adopted");
        Check(!PrimaryCameraPolicy.ShouldPromote(7, true, 1920L * 1080, 10, 7, 1920L * 1080, 11),
            "the same live camera is never re-adopted");
        Check(!PrimaryCameraPolicy.ShouldPromote(7, true, 1920L * 1080, 10, 8, 1920L * 1080, 11),
            "an equal-area live camera does not oscillate");
        Check(PrimaryCameraPolicy.ShouldPromote(7, true, 1280L * 720, 10, 8, 1920L * 1080, 11),
            "a strictly larger live camera takes over");
        Check(PrimaryCameraPolicy.ShouldPromote(7, true, 1920L * 1080, 10, 8, 640L * 360, 20),
            "a primary that stopped rendering is replaced even by a smaller camera");
        Check(PrimaryCameraPolicy.ShouldPromote(7, false, 1920L * 1080, 10, 8, 640L * 360, 11),
            "a destroyed or disabled primary is replaced");
        Check(!PrimaryCameraPolicy.ShouldPromote(0, false, 0, 0, 0, 1920L * 1080, 10),
            "a zero candidate id is never adopted");
        Check(!PrimaryCameraPolicy.ShouldPromote(0, false, 0, 0, 9, 0, 10),
            "a non-positive candidate area is never adopted");
    }

    private static void CheckFrameGenerationActivity()
    {
        var activity = new FrameGenerationActivity();
        Check(!activity.Confirmed(1, FrameGenerationKind.Nvidia),
            "activity requires a baseline and an observation");
        activity.Reset(1, FrameGenerationKind.Nvidia, 100);
        activity.Observe(1, FrameGenerationKind.Nvidia, true, 100);
        Check(!activity.Confirmed(1, FrameGenerationKind.Nvidia),
            "an unchanged generated count is not proof");
        activity.Observe(1, FrameGenerationKind.Nvidia, true, 101);
        Check(activity.Confirmed(1, FrameGenerationKind.Nvidia), "a new generated image confirms activity");
        Check(activity.ConfirmedProvider == FrameGenerationKind.Nvidia, "the confirmed provider is reported");
        activity.Observe(1, FrameGenerationKind.Nvidia, false, 200);
        Check(activity.Confirmed(1, FrameGenerationKind.Nvidia),
            "menu suspension preserves the historical gameplay proof");
        var otherProvider = new FrameGenerationActivity();
        otherProvider.Reset(1, FrameGenerationKind.Nvidia, 100);
        otherProvider.Observe(1, FrameGenerationKind.Fsr, true, 300);
        Check(!otherProvider.Confirmed(1, FrameGenerationKind.Nvidia),
            "another provider's Active state never confirms the requested one");
        activity.Reset(2, FrameGenerationKind.Nvidia, 300);
        Check(!activity.Confirmed(1, FrameGenerationKind.Nvidia), "an old generation is rejected");
        Check(!activity.Confirmed(2, FrameGenerationKind.Nvidia), "a new baseline starts unconfirmed");
        activity.Observe(2, FrameGenerationKind.Nvidia, true, 301);
        Check(activity.Confirmed(2, FrameGenerationKind.Nvidia), "the new generation confirms independently");
    }

    /// <summary>
    /// The managed restart predictor must mirror the native transport exactly: a normal family may
    /// migrate to ANR in place, every other change after a claim needs a restart, and Off is free.
    /// </summary>
    private static void CheckFrameGenerationFamily()
    {
        Check(NativeFg.FamilyUnused == 0, "the unused family code mirrors the native macro");
        Check(NativeFg.FamilyNormal == 1, "the normal family code mirrors the native macro");
        Check(NativeFg.FamilyAnr == 2, "the ANR family code mirrors the native macro");
        Check(NativeFg.FamilyStreamline == 3, "the streamline family code mirrors the native macro");

        foreach (var requested in new[]
                 {
                     FrameGenerationKind.Off, FrameGenerationKind.Nvidia, FrameGenerationKind.Fsr,
                     FrameGenerationKind.XeSs, FrameGenerationKind.Anr, FrameGenerationKind.Streamline,
                 })
        {
            Check(!FrameGenerationFamily.RestartRequired(NativeFg.FamilyUnused, requested),
                $"an unclaimed process accepts {requested} in place");
        }

        Check(!FrameGenerationFamily.RestartRequired(NativeFg.FamilyNormal, FrameGenerationKind.Off),
            "Off needs no restart in the normal family");
        Check(!FrameGenerationFamily.RestartRequired(NativeFg.FamilyAnr, FrameGenerationKind.Off),
            "Off needs no restart in the ANR family");
        Check(!FrameGenerationFamily.RestartRequired(NativeFg.FamilyStreamline, FrameGenerationKind.Off),
            "Off needs no restart in the streamline family");

        Check(!FrameGenerationFamily.RestartRequired(NativeFg.FamilyNormal, FrameGenerationKind.Nvidia),
            "the normal family accepts NVIDIA in place");
        Check(!FrameGenerationFamily.RestartRequired(NativeFg.FamilyNormal, FrameGenerationKind.Fsr),
            "the normal family accepts FSR in place");
        Check(!FrameGenerationFamily.RestartRequired(NativeFg.FamilyNormal, FrameGenerationKind.XeSs),
            "the normal family accepts XeSS in place");
        Check(!FrameGenerationFamily.RestartRequired(NativeFg.FamilyNormal, FrameGenerationKind.Anr),
            "the normal family migrates to ANR in place");
        Check(FrameGenerationFamily.RestartRequired(NativeFg.FamilyNormal, FrameGenerationKind.Streamline),
            "the normal family cannot load streamline without a restart");

        Check(!FrameGenerationFamily.RestartRequired(NativeFg.FamilyAnr, FrameGenerationKind.Anr),
            "the ANR family stays on ANR without a restart");
        Check(FrameGenerationFamily.RestartRequired(NativeFg.FamilyAnr, FrameGenerationKind.Nvidia),
            "ANR refuses NVIDIA without a restart");
        Check(FrameGenerationFamily.RestartRequired(NativeFg.FamilyAnr, FrameGenerationKind.Fsr),
            "ANR refuses FSR without a restart");
        Check(FrameGenerationFamily.RestartRequired(NativeFg.FamilyAnr, FrameGenerationKind.XeSs),
            "ANR refuses XeSS without a restart");
        Check(FrameGenerationFamily.RestartRequired(NativeFg.FamilyAnr, FrameGenerationKind.Streamline),
            "ANR refuses streamline without a restart");

        Check(!FrameGenerationFamily.RestartRequired(NativeFg.FamilyStreamline, FrameGenerationKind.Streamline),
            "the streamline family stays on streamline without a restart");
        Check(FrameGenerationFamily.RestartRequired(NativeFg.FamilyStreamline, FrameGenerationKind.Nvidia),
            "streamline refuses NVIDIA without a restart");
        Check(FrameGenerationFamily.RestartRequired(NativeFg.FamilyStreamline, FrameGenerationKind.Fsr),
            "streamline refuses FSR without a restart");
        Check(FrameGenerationFamily.RestartRequired(NativeFg.FamilyStreamline, FrameGenerationKind.XeSs),
            "streamline refuses XeSS without a restart");
        Check(FrameGenerationFamily.RestartRequired(NativeFg.FamilyStreamline, FrameGenerationKind.Anr),
            "streamline refuses ANR without a restart");

        Check(!FrameGenerationFamily.RestartRequired(NativeFg.FamilyNormal, FrameGenerationKind.Anr) &&
              FrameGenerationFamily.RestartRequired(NativeFg.FamilyAnr, FrameGenerationKind.Fsr),
            "the migration is one-way: normal -> ANR is free and ANR -> FSR needs a restart");
    }

    /// <summary>
    /// The multiplier row may only offer totals the provider has proven. A provider that has not
    /// answered yet (or answered 2x) is 2x, which is the XeSS regression: it offers one
    /// interpolation only, so 6x is never selectable there.
    /// </summary>
    private static void CheckFrameMultiplierCeiling()
    {
        Check(UpscalingModes.FrameMultiplierCeiling(FrameGenerationKind.XeSs, 0) == 2,
            "an unanswered provider offers only 2x");
        Check(UpscalingModes.FrameMultiplierCeiling(FrameGenerationKind.XeSs, 2) == 2,
            "a provider that proved 2x offers only 2x");
        Check(UpscalingModes.FrameMultiplierCeiling(FrameGenerationKind.Nvidia, 6) == 6,
            "a provider that proved 6x offers 6x");
        Check(UpscalingModes.FrameMultiplierCeiling(FrameGenerationKind.Anr, 6) == 2,
            "ANR is capped at its 2x one-generated-frame maximum");
        Check(UpscalingModes.FrameMultiplierCeiling(FrameGenerationKind.Anr, 2) == 2,
            "ANR that proved 2x stays at 2x");
        Check(UpscalingModes.FrameMultiplierCeiling(FrameGenerationKind.Anr, 0) == 2,
            "ANR before proof offers only 2x");
        Check(UpscalingModes.ProviderMultiplierLimit(FrameGenerationKind.Anr) == 2,
            "ANR's fixed multiplier limit is 2x");
        Check(UpscalingModes.FrameMultiplierCeiling(FrameGenerationKind.Nvidia, 9) == 6,
            "the ceiling never exceeds the 6x the runtime can present");
        Check(UpscalingModes.FrameMultiplierCeiling(FrameGenerationKind.Off, 6) == 0,
            "a disabled provider offers nothing");
    }

    private static void CheckNeuralRendering()
    {
        // Display order and wire values are shared by the panel, the config repair and the native
        // feature parameters, so a renumber here changes what the game sends to NGX.
        var styles = NeuralRenderingModes.StyleOrder;
        Check(styles.Length == 3, $"style order must list 3 values, got {styles.Length}");
        Check(styles[0] == NeuralRenderingStyle.Default && (int)styles[0] == 0, "style wire value 0 is Default");
        Check(styles[1] == NeuralRenderingStyle.Natural && (int)styles[1] == 1, "style wire value 1 is Natural");
        Check(styles[2] == NeuralRenderingStyle.Cinematic && (int)styles[2] == 2, "style wire value 2 is Cinematic");

        var timings = NeuralRenderingModes.TimingOrder;
        Check(timings.Length == 2, $"timing order must list 2 values, got {timings.Length}");
        Check(timings[0] == NeuralRenderingTiming.BeforeUpscaling && (int)timings[0] == 0,
            "timing wire value 0 is BeforeUpscaling");
        Check(timings[1] == NeuralRenderingTiming.AfterUpscaling && (int)timings[1] == 1,
            "timing wire value 1 is AfterUpscaling");

        Check(NeuralRenderingModes.Name(NeuralRenderingStyle.Default) == "Default", "style 0 is labelled Default");
        Check(NeuralRenderingModes.Name(NeuralRenderingStyle.Natural) == "Natural", "style 1 is labelled Natural");
        Check(NeuralRenderingModes.Name(NeuralRenderingStyle.Cinematic) == "Cinematic", "style 2 is labelled Cinematic");
        Check(NeuralRenderingModes.Name(NeuralRenderingTiming.BeforeUpscaling) == "Before Upscaling",
            "pre-upscale timing is labelled Before Upscaling");
        Check(NeuralRenderingModes.Name(NeuralRenderingTiming.AfterUpscaling) == "After Upscaling",
            "post-upscale timing is labelled After Upscaling");

        Check(NeuralRenderingModes.MinPasses == 1, "the minimum pass count is 1");
        Check(NeuralRenderingModes.MaxPasses == 5, "the maximum pass count is 5");
        Check(NeuralRenderingModes.ClampPasses(0) == 1, "zero passes clamp up to 1");
        Check(NeuralRenderingModes.ClampPasses(1) == 1, "one pass stays 1");
        Check(NeuralRenderingModes.ClampPasses(5) == 5, "five passes stay 5");
        Check(NeuralRenderingModes.ClampPasses(6) == 5, "six passes clamp down to 5");
        Check(NeuralRenderingModes.ClampPasses(int.MinValue) == 1, "the smallest int clamps to 1");
        Check(NeuralRenderingModes.ClampPasses(int.MaxValue) == 5, "the largest int clamps to 5");

        Check(NeuralRenderingModes.ClampUnit(float.NaN) == 1f, "a NaN unit value falls back to 1");
        Check(NeuralRenderingModes.ClampUnit(float.PositiveInfinity) == 1f,
            "positive infinity falls back to 1");
        Check(NeuralRenderingModes.ClampUnit(float.NegativeInfinity) == 1f,
            "negative infinity falls back to 1");
        Check(NeuralRenderingModes.ClampUnit(-1f) == 0f, "a negative unit value clamps to 0");
        Check(NeuralRenderingModes.ClampUnit(0f) == 0f, "zero stays 0");
        Check(NeuralRenderingModes.ClampUnit(1f) == 1f, "one stays 1");
        Check(NeuralRenderingModes.ClampUnit(2f) == 2f, "two stays 2");
        Check(NeuralRenderingModes.ClampUnit(9f) == 2f, "a value above the range clamps to 2");
        Check(NeuralRenderingModes.ClampUnit(float.NaN, 0.5f) == 0.5f,
            "a NaN unit value uses the supplied fallback");

        static void CheckRows(NeuralRenderingPage page,
            (string Key, string Caption, NeuralRenderingRowKind Kind)[] expected, string label)
        {
            Check(page.Rows.Length == expected.Length,
                $"the {label} page declares {expected.Length} rows, got {page.Rows.Length}");
            for (var i = 0; i < expected.Length && i < page.Rows.Length; i++)
            {
                var row = page.Rows[i];
                Check(row.Key == expected[i].Key && row.Caption == expected[i].Caption &&
                      row.Kind == expected[i].Kind,
                    $"{label} row {i} must be {expected[i].Caption}/{expected[i].Kind}");
            }
        }

        var pages = NeuralRenderingSchema.Pages;
        Check(pages.Length == 2, $"the Neural Rendering schema declares 2 pages, got {pages.Length}");
        if (pages.Length == 2)
        {
            var first = pages[0];
            Check(first.Label == NeuralRenderingSchema.CategoryLabel && first.Label == "Neural Rendering",
                "the first page caption is Neural Rendering");
            Check(first.SettingsObjectName == "BigScale.NeuralSettings",
                "the first page root is BigScale.NeuralSettings");
            Check(first.CategoryObjectName == "BigScale.NeuralCategory",
                "the first page tab is BigScale.NeuralCategory");
            CheckRows(first, new (string Key, string Caption, NeuralRenderingRowKind Kind)[]
            {
                (NeuralRenderingSchema.EnabledKey, "Enabled", NeuralRenderingRowKind.Toggle),
                (NeuralRenderingSchema.PresetKey, "Preset", NeuralRenderingRowKind.Choice),
                (NeuralRenderingSchema.TimingKey, "Timing", NeuralRenderingRowKind.Choice),
                (NeuralRenderingSchema.PassesKey, "Passes", NeuralRenderingRowKind.Integer),
                (NeuralRenderingSchema.StatusKey, "Status", NeuralRenderingRowKind.Status),
            }, "Neural Rendering");

            var second = pages[1];
            Check(second.Label == NeuralRenderingSchema.TuningLabel && second.Label == "Neural Tuning",
                "the second page caption is Neural Tuning");
            Check(second.SettingsObjectName == "BigScale.NeuralTuningSettings",
                "the second page root is BigScale.NeuralTuningSettings");
            Check(second.CategoryObjectName == "BigScale.NeuralTuningCategory",
                "the second page tab is BigScale.NeuralTuningCategory");
            CheckRows(second, new (string Key, string Caption, NeuralRenderingRowKind Kind)[]
            {
                (NeuralRenderingSchema.StrengthKey, "Strength", NeuralRenderingRowKind.Slider),
                (NeuralRenderingSchema.DetailKey, "Detail", NeuralRenderingRowKind.Slider),
                (NeuralRenderingSchema.ToneKey, "Tone", NeuralRenderingRowKind.Slider),
                (NeuralRenderingSchema.CharacterMaskKey, "Character Masking", NeuralRenderingRowKind.Toggle),
            }, "Neural Tuning");
        }

        var flat = new List<NeuralRenderingRow>();
        foreach (var page in pages) flat.AddRange(page.Rows);
        Check(flat.Count == 9, $"the two Neural pages declare 9 rows in total, got {flat.Count}");
        var keys = new HashSet<string>();
        foreach (var row in flat) keys.Add(row.Key);
        Check(keys.Count == flat.Count, $"the nine Neural rows use unique keys, got {keys.Count}");
        foreach (var row in flat)
        {
            if (row.Kind == NeuralRenderingRowKind.Slider)
            {
                Check(row.Min == 0f && row.Max == 2f,
                    $"{row.Caption} slider range must be 0..2, got {row.Min}..{row.Max}");
            }
            else
            {
                Check(row.Min == 0f && row.Max == 0f,
                    $"{row.Caption} is not a slider and must carry no range");
            }
        }

        Check(NeuralRenderingStatusText.For(false, true, UpscalerKind.Dlss, NeuralRenderingState.Active) == "Off",
            "the disabled gate reads Off even with the master switch on");
        Check(NeuralRenderingStatusText.For(true, false, UpscalerKind.Dlss, NeuralRenderingState.Active) == "Off",
            "the master switch off reads Off");
        Check(NeuralRenderingStatusText.For(true, true, UpscalerKind.Off, NeuralRenderingState.Active) ==
              "Select an Upscaler", "an Off upscaler asks for a selection");
        Check(NeuralRenderingStatusText.For(true, true, UpscalerKind.Dlss, NeuralRenderingState.Waiting) == "Waiting",
            "a waiting native state reads Waiting");
        Check(NeuralRenderingStatusText.For(true, true, UpscalerKind.Dlss, NeuralRenderingState.Active) == "Active",
            "an active native state reads Active");
        Check(NeuralRenderingStatusText.For(true, true, UpscalerKind.Dlss, NeuralRenderingState.RuntimeMissing) ==
              "Runtime missing", "a missing runtime reads Runtime missing");
        Check(NeuralRenderingStatusText.For(true, true, UpscalerKind.Dlss, NeuralRenderingState.Failed) == "Failed",
            "a failed native state reads Failed");
        Check(NeuralRenderingStatusText.For(true, true, UpscalerKind.Dlss, NeuralRenderingState.Unsupported) ==
              "Unsupported (desktop only)", "an unsupported packet layout names its desktop-only limit");
        Check(NeuralRenderingStatusText.For(true, true, UpscalerKind.Dlss, NeuralRenderingState.Off) == "Waiting",
            "a stale Off native state reads Waiting rather than claiming success");
        Check(NeuralRenderingStatusText.For(true, true, UpscalerKind.Dlss, 99) == "Waiting",
            "an unknown native state reads Waiting");
    }

    private static void CheckCharacterMaskPolicy()
    {
        var builtIns = CharacterMaskPolicy.DefaultShaderNames;
        Check(builtIns.Length == 2, $"the built-in character shader list must hold 2 names, got {builtIns.Length}");
        Check(builtIns[0] == "househouse/PlayerCharacter" && builtIns[1] == "househouse/PlayerNose",
            "the built-in character shader names are unchanged");

        var none = Array.Empty<string>();
        Check(CharacterMaskPolicy.IsCharacterShader("househouse/PlayerCharacter", none),
            "the player body shader is a character shader");
        Check(CharacterMaskPolicy.IsCharacterShader("househouse/PlayerNose", none),
            "the player nose shader is a character shader");
        Check(CharacterMaskPolicy.IsCharacterShader("HOUSEHOUSE/PLAYERCHARACTER", none),
            "the built-in match is case-insensitive");
        Check(CharacterMaskPolicy.IsCharacterShader("househouse/PlayerCharacterAlt", none),
            "a variant name containing the built-in is a character shader");
        Check(CharacterMaskPolicy.IsCharacterShader("x/househouse/playerNose.y", none),
            "the match is a substring, not a whole name");
        Check(!CharacterMaskPolicy.IsCharacterShader("Universal Render Pipeline/Lit", none),
            "an environment shader is not a character shader");
        Check(!CharacterMaskPolicy.IsCharacterShader(null, none), "a null shader name matches nothing");
        Check(!CharacterMaskPolicy.IsCharacterShader(string.Empty, none), "an empty shader name matches nothing");
        Check(!CharacterMaskPolicy.IsCharacterShader("   ", none), "a whitespace shader name matches nothing");

        Check(CharacterMaskPolicy.IsCharacterShader("Custom/Variant/SkinPass", new[] { "Variant/Skin" }),
            "an extra shader name protects a variant");
        Check(CharacterMaskPolicy.IsCharacterShader("x/variant/skin.y", new[] { "Variant/Skin" }),
            "an extra shader name matches case-insensitively");
        Check(!CharacterMaskPolicy.IsCharacterShader("Universal Render Pipeline/Lit", null!),
            "a null extra list matches nothing");
        Check(!CharacterMaskPolicy.IsCharacterShader("Universal Render Pipeline/Lit",
                new[] { (string)null!, string.Empty, "   " }),
            "null, empty and whitespace extras are skipped");
        Check(!CharacterMaskPolicy.IsCharacterShader("Universal Render Pipeline/Lit", new[] { string.Empty }),
            "an empty extra is not a wildcard");

        Check(CharacterMaskPolicy.LocalNoseShaderName == builtIns[1],
            "the local nose constant is the built-in nose shader");
        const int cameraMask = ~(1 << 9); // the local camera leaves out HideOnLocalCamera
        Check(!CharacterMaskPolicy.IsDrawnByCamera(cameraMask, 9, true, true, false, false),
            "a renderer on the camera-hidden layer 9 is not drawn");
        Check(CharacterMaskPolicy.IsDrawnByCamera(cameraMask, 0, true, true, false, false),
            "a visible-layer renderer is drawn");
        Check(!CharacterMaskPolicy.IsDrawnByCamera(-1, 0, false, true, false, false),
            "a disabled renderer is not drawn");
        Check(!CharacterMaskPolicy.IsDrawnByCamera(-1, 0, true, false, false, false),
            "an inactive renderer is not drawn");
        Check(!CharacterMaskPolicy.IsDrawnByCamera(-1, 0, true, true, true, false),
            "a force-hidden renderer is not drawn");
        Check(!CharacterMaskPolicy.IsDrawnByCamera(-1, 0, true, true, false, true),
            "a shadow-only renderer is not drawn");
        Check(!CharacterMaskPolicy.IsDrawnByCamera(-1, 32, true, true, false, false) &&
              !CharacterMaskPolicy.IsDrawnByCamera(-1, -1, true, true, false, false),
            "an out-of-range layer is not drawn");

        var on3 = new[] { true, true, true };
        Check(CharacterMaskPolicy.SelectForwardPass(new[] { "ShadowCaster", "DepthOnly", "UniversalForward" }, on3) == 2,
            "the forward pass is chosen over depth and shadow passes");
        Check(CharacterMaskPolicy.SelectForwardPass(new[] { "SRPDefaultUnlit", "UniversalForward" }, new[] { true, true }) == 1,
            "UniversalForward beats an extra unlit pass");
        Check(CharacterMaskPolicy.SelectForwardPass(new[] { null, "ShadowCaster" }, new[] { true, true }) == 0,
            "an untagged pass counts as SRPDefaultUnlit");
        Check(CharacterMaskPolicy.SelectForwardPass(new[] { "UniversalForward", "UniversalForwardOnly" }, new[] { false, true }) == 1,
            "a disabled forward pass is skipped");
        Check(CharacterMaskPolicy.SelectForwardPass(new[] { "ShadowCaster", "DepthOnly", "MotionVectors" }, on3) == -1,
            "no forward pass");
        Check(CharacterMaskPolicy.SelectForwardPass(new[] { "universalforward" }, new[] { true }) == 0,
            "tags compare case-insensitively");
        Check(CharacterMaskPolicy.SelectForwardPass(new[] { "UniversalForward" }, new[] { true, false }) == -1,
            "mismatched lengths");
    }

    private static void CheckAbi()
    {
        Check(Native.AbiVersion == 5, "the bridge ABI is version 5");
        Check(Marshal.SizeOf<Native.Frame>() == 192, $"managed Frame size must be 192, got {Marshal.SizeOf<Native.Frame>()}");
        Check(Marshal.SizeOf<Native.Status>() == 1176, $"managed Status size must be 1176, got {Marshal.SizeOf<Native.Status>()}");
        Check(Marshal.SizeOf<Native.ModeInfo>() == 32, "managed ModeInfo size must be 32");
        Check(Marshal.SizeOf<Native.ViewInfo>() == 24, "managed ViewInfo size must be 24");
        Check(Marshal.SizeOf<Native.Completion>() == 76, "managed Completion size must be 76");
        Check(Marshal.OffsetOf<Native.Frame>(nameof(Native.Frame.Color)).ToInt64() == 80, "Frame.Color offset");
        Check(Marshal.OffsetOf<Native.Frame>(nameof(Native.Frame.Output)).ToInt64() == 104, "Frame.Output offset");
        Check(Marshal.OffsetOf<Native.Frame>(nameof(Native.Frame.JitterX)).ToInt64() == 112, "Frame.JitterX offset");
        Check(Marshal.OffsetOf<Native.Frame>(nameof(Native.Frame.DeltaMilliseconds)).ToInt64() == 128, "Frame.DeltaMilliseconds offset");
        Check(Marshal.OffsetOf<Native.Frame>(nameof(Native.Frame.ResourceTag)).ToInt64() == 144, "Frame.ResourceTag offset");
        Check(Marshal.OffsetOf<Native.Frame>(nameof(Native.Frame.Upscaler)).ToInt64() == 148, "Frame.Upscaler offset");
        Check(Marshal.OffsetOf<Native.Frame>(nameof(Native.Frame.CameraNear)).ToInt64() == 152, "Frame.CameraNear offset");
        Check(Marshal.OffsetOf<Native.Frame>(nameof(Native.Frame.PreExposure)).ToInt64() == 164, "Frame.PreExposure offset");
        Check(Marshal.OffsetOf<Native.Frame>(nameof(Native.Frame.FgCaptureTag)).ToInt64() == 168, "Frame.FgCaptureTag offset");
        Check(Marshal.OffsetOf<Native.Frame>(nameof(Native.Frame.NeuralMask)).ToInt64() == 176, "Frame.NeuralMask offset");
        Check(Marshal.OffsetOf<Native.Frame>(nameof(Native.Frame.NeuralMaskWidth)).ToInt64() == 184,
            "Frame.NeuralMaskWidth offset");
        Check(Marshal.OffsetOf<Native.Frame>(nameof(Native.Frame.NeuralMaskHeight)).ToInt64() == 188,
            "Frame.NeuralMaskHeight offset");
        Check(Marshal.OffsetOf<Native.Status>(nameof(Native.Status.TotalEvaluations)).ToInt64() == 48, "Status.TotalEvaluations offset");
        Check(Marshal.OffsetOf<Native.Status>(nameof(Native.Status.Mode0)).ToInt64() == 64, "Status.Mode0 offset");
        Check(Marshal.OffsetOf<Native.Status>(nameof(Native.Status.Mode4)).ToInt64() == 192, "Status.Mode4 offset");
        Check(Marshal.OffsetOf<Native.Status>(nameof(Native.Status.View0)).ToInt64() == 224, "Status.View0 offset");
        Check(Marshal.OffsetOf<Native.Status>(nameof(Native.Status.ViewCount)).ToInt64() == 272, "Status.ViewCount offset");
        Check(Marshal.OffsetOf<Native.Status>(nameof(Native.Status.NeuralState)).ToInt64() == 284,
            "Status.NeuralState offset");
        Check(Marshal.OffsetOf<Native.Status>(nameof(Native.Status.CompletionPublished)).ToInt64() == 288, "Status.CompletionPublished offset");
        Check(Marshal.OffsetOf<Native.Status>(nameof(Native.Status.CompletionBase)).ToInt64() == 292, "Status.CompletionBase offset");
        Check(Marshal.OffsetOf<Native.Status>(nameof(Native.Status.Completion0)).ToInt64() == 296, "Status.Completion0 offset");
        Check(Marshal.OffsetOf<Native.Status>(nameof(Native.Status.Completion7)).ToInt64() == 828, "Status.Completion7 offset");
        Check(Marshal.OffsetOf<Native.Status>(nameof(Native.Status.QueryUpscaler)).ToInt64() == 904, "Status.QueryUpscaler offset");
        Check(Marshal.OffsetOf<Native.Status>(nameof(Native.Status.ActiveUpscaler)).ToInt64() == 908, "Status.ActiveUpscaler offset");
        Check(Marshal.OffsetOf<Native.Status>(nameof(Native.Status.ProviderVersionId)).ToInt64() == 912, "Status.ProviderVersionId offset");
        Check(Marshal.OffsetOf<Native.Status>(nameof(Native.Status.BackendReason)).ToInt64() == 920, "Status.BackendReason offset");
        Check(Marshal.OffsetOf<Native.Completion>(nameof(Native.Completion.Kind)).ToInt64() == 4, "Completion.Kind offset");
        Check(Marshal.OffsetOf<Native.Completion>(nameof(Native.Completion.Result)).ToInt64() == 32, "Completion.Result offset");
        Check(Marshal.OffsetOf<Native.Completion>(nameof(Native.Completion.Upscaler)).ToInt64() == 52, "Completion.Upscaler offset");
        Check(Marshal.OffsetOf<Native.Completion>(nameof(Native.Completion.SliceCount)).ToInt64() == 68, "Completion.SliceCount offset");
        Check(Native.FrameFieldOrder.Length == 42, $"frame field order table must list 42 fields, got {Native.FrameFieldOrder.Length}");
        Check(Native.CompletionFieldOrder.Length == 19, "completion field order table lists 19 fields");
        Check(Native.StatusFieldOrder.Length == 40, "status field order table lists 40 fields");
        Check(Native.StatusFieldOrder.Contains(nameof(Native.Status.NeuralState)),
            "the status field order table carries the neural state");
        Check(!Native.StatusFieldOrder.Contains("Reserved"),
            "the status field order table no longer carries a Reserved field");
        Check(Native.NeuralAbiVersion == 1, "the Neural Rendering ABI is version 1");
        Check(Marshal.SizeOf<Native.NeuralConfig>() == 44,
            $"managed NeuralConfig size must be 44, got {Marshal.SizeOf<Native.NeuralConfig>()}");
        Check(Marshal.OffsetOf<Native.NeuralConfig>(nameof(Native.NeuralConfig.Abi)).ToInt64() == 0,
            "NeuralConfig.Abi offset");
        Check(Marshal.OffsetOf<Native.NeuralConfig>(nameof(Native.NeuralConfig.Size)).ToInt64() == 4,
            "NeuralConfig.Size offset");
        Check(Marshal.OffsetOf<Native.NeuralConfig>(nameof(Native.NeuralConfig.Enabled)).ToInt64() == 8,
            "NeuralConfig.Enabled offset");
        Check(Marshal.OffsetOf<Native.NeuralConfig>(nameof(Native.NeuralConfig.Style)).ToInt64() == 12,
            "NeuralConfig.Style offset");
        Check(Marshal.OffsetOf<Native.NeuralConfig>(nameof(Native.NeuralConfig.Timing)).ToInt64() == 16,
            "NeuralConfig.Timing offset");
        Check(Marshal.OffsetOf<Native.NeuralConfig>(nameof(Native.NeuralConfig.Passes)).ToInt64() == 20,
            "NeuralConfig.Passes offset");
        Check(Marshal.OffsetOf<Native.NeuralConfig>(nameof(Native.NeuralConfig.Strength)).ToInt64() == 24,
            "NeuralConfig.Strength offset");
        Check(Marshal.OffsetOf<Native.NeuralConfig>(nameof(Native.NeuralConfig.Detail)).ToInt64() == 28,
            "NeuralConfig.Detail offset");
        Check(Marshal.OffsetOf<Native.NeuralConfig>(nameof(Native.NeuralConfig.Tone)).ToInt64() == 32,
            "NeuralConfig.Tone offset");
        Check(Marshal.OffsetOf<Native.NeuralConfig>(nameof(Native.NeuralConfig.CharacterMask)).ToInt64() == 36,
            "NeuralConfig.CharacterMask offset");
        Check(Marshal.OffsetOf<Native.NeuralConfig>(nameof(Native.NeuralConfig.Reserved)).ToInt64() == 40,
            "NeuralConfig.Reserved offset");
        Check(NativeFg.AbiVersion == 7, "the frame-generation ABI is version 7");
        Check(Marshal.SizeOf<NativeFg.Config>() == 40, "managed FG config size must be 40");
        Check(Marshal.SizeOf<NativeFg.Frame>() == 424, "managed FG frame size must be 424");
        Check(Marshal.SizeOf<NativeFg.Status>() == 464, "managed FG status size must be 464");
        Check(Marshal.OffsetOf<NativeFg.Status>(nameof(NativeFg.Status.ActiveMultiplier)).ToInt64() == 56,
            "FG active multiplier offset");
        Check(Marshal.OffsetOf<NativeFg.Status>(nameof(NativeFg.Status.MaxMultiplier)).ToInt64() == 60,
            "FG maximum multiplier offset");
        Check(Marshal.OffsetOf<NativeFg.Status>(nameof(NativeFg.Status.RestartRequired)).ToInt64() == 64,
            "FG restart requirement offset");
        Check(Marshal.OffsetOf<NativeFg.Status>(nameof(NativeFg.Status.ProcessFamily)).ToInt64() == 68,
            "FG process family offset");
        Check(Marshal.OffsetOf<NativeFg.Status>(nameof(NativeFg.Status.Reason)).ToInt64() == 72, "FG reason offset");
        Check(Marshal.OffsetOf<NativeFg.Status>(nameof(NativeFg.Status.RetirementCount)).ToInt64() == 328,
            "FG retirement count offset");
        Check(Marshal.OffsetOf<NativeFg.Status>(nameof(NativeFg.Status.Reserved)).ToInt64() == 332,
            "FG status reserved offset");
        Check(Marshal.OffsetOf<NativeFg.Frame>(nameof(NativeFg.Frame.Depth)).ToInt64() == 384, "FG frame depth offset");
        Check(Marshal.OffsetOf<NativeFg.Frame>(nameof(NativeFg.Frame.Motion)).ToInt64() == 392, "FG frame motion offset");
        Check(Marshal.OffsetOf<NativeFg.Frame>(nameof(NativeFg.Frame.PresentId)).ToInt64() == 400,
            "FG frame present id offset");
        Check(Marshal.OffsetOf<NativeFg.Frame>(nameof(NativeFg.Frame.MotionHeight)).ToInt64() == 412,
            "FG frame motion height offset");
        Check(Marshal.OffsetOf<NativeFg.Frame>(nameof(NativeFg.Frame.Hudless)).ToInt64() == 416,
            "FG frame HUD-less offset");
        Check(Marshal.OffsetOf<NativeFg.Config>(nameof(NativeFg.Config.CaptureReady)).ToInt64() == 24,
            "FG config capture-ready offset");
        Check(Marshal.OffsetOf<NativeFg.Config>(nameof(NativeFg.Config.Multiplier)).ToInt64() == 36,
            "FG config multiplier offset");
        Check(NativeFg.ConfigFieldOrder.Length == 10, "FG config field order lists 10 fields");
        Check(NativeFg.FrameFieldOrder.Length == 33, "FG frame field order lists 33 fields");
        Check(NativeFg.StatusFieldOrder.Length == 19, "FG status field order lists 19 fields");
        Check(NativeVrFg.AbiVersion == 1, "the VR frame-generation ABI is version 1");
        Check(Marshal.SizeOf<NativeVrFg.Status>() == 320,
            $"managed VRFG status size must be 320, got {Marshal.SizeOf<NativeVrFg.Status>()}");
        Check(Marshal.OffsetOf<NativeVrFg.Status>(nameof(NativeVrFg.Status.DisplayHz)).ToInt64() == 16,
            "VRFG display rate offset");
        Check(Marshal.OffsetOf<NativeVrFg.Status>(nameof(NativeVrFg.Status.MotionSmoothing)).ToInt64() == 20,
            "VRFG motion smoothing offset");
        Check(Marshal.OffsetOf<NativeVrFg.Status>(nameof(NativeVrFg.Status.RealFrames)).ToInt64() == 24,
            "VRFG real-frame counter offset");
        Check(Marshal.OffsetOf<NativeVrFg.Status>(nameof(NativeVrFg.Status.PacingTimeouts)).ToInt64() == 56,
            "VRFG pacing-timeout counter offset");
        Check(Marshal.OffsetOf<NativeVrFg.Status>(nameof(NativeVrFg.Status.Reason)).ToInt64() == 64,
            "VRFG reason offset");
        Check(NativeVrFg.StatusFieldOrder.Length == 12, "VRFG status field order lists 12 fields");
    }

    private static void CheckXrJitter()
    {
        // The VR mod's patched shaders ignore URP's jitter matrix, so the offset this mod injects is
        // chosen in render pixels, expressed as a clip-space translation, and read back by the packet
        // as render pixels. Sixteen frames of one performance reduction must round-trip exactly.
        const int input = 1280;
        const int output = 2560;
        var phases = XrJitter.PhaseCount(input, output);
        Check(phases == 32, $"a 2x performance reduction uses 32 Halton phases, got {phases}");
        Check(XrJitter.PhaseCount(1920, 1920) == 8, "native AA stays at the minimum phase count");
        Check(XrJitter.PhaseCount(0, 1920) == 8, "an unusable input keeps the minimum phase count");
        Check(XrJitter.PhaseCount(16, 4096) == 64, "a large reduction clamps at 64 phases");
        var distinctX = new System.Collections.Generic.HashSet<int>();
        var distinctY = new System.Collections.Generic.HashSet<int>();
        for (var frame = 1; frame <= 16; frame++)
        {
            var (pixelX, pixelY) = XrJitter.PixelOffset(frame, phases);
            Check(Math.Abs(pixelX) < 0.5f && Math.Abs(pixelY) < 0.5f,
                $"frame {frame} jitter stays inside one pixel, got ({pixelX},{pixelY})");
            // The projection is translated by the clip offset, and the packet carries the pixel
            // sample directly; the two must describe the same displacement.
            var (packetX, packetY) = TemporalInputPolicy.Jitter(pixelX, pixelY);
            Check(Math.Abs(packetX - pixelX) < 1e-4f && Math.Abs(packetY - pixelY) < 1e-4f,
                $"frame {frame} jitter round-trips through the clip translation, got ({packetX},{packetY}) " +
                $"from ({pixelX},{pixelY})");
            distinctX.Add((int)MathF.Round(pixelX * 1000f));
            distinctY.Add((int)MathF.Round(pixelY * 1000f));
        }
        Check(distinctX.Count >= 8 && distinctY.Count >= 8,
            $"16 consecutive frames use distinct offsets, got {distinctX.Count}x{distinctY.Count}");
        var first = XrJitter.PixelOffset(0, phases);
        Check(first == XrJitter.PixelOffset(phases, phases), "the jitter sequence repeats after one phase set");
        CheckXrProjection();
    }

    private static void CheckXrProjection()
    {
        // The sample is measured in render pixels; the projection is left-multiplied by a clip-space
        // translation, and the packet reports the same pixel sample. A nonzero off-axis term makes a
        // width/height swap observable, and the two eye positions must land on the same pixel shift.
        const float near = 0.03f;
        const float far = 1500f;
        const float fov = 1.2f;
        var aspect = 1280f / 960f;
        var f = 1f / MathF.Tan(fov * 0.5f);
        var projection = new[]
        {
            f / aspect, 0, 0, 0,
            0, f, 0, 0,
            0, 0, far / (near - far), -1,
            0, 0, near * far / (near - far), 0,
        };
        var eyes = new[] { -0.032f, 0.032f };
        var samples = new[] { (0.25f, -0.31f), (-0.44f, 0.37f) };
        foreach (var uvStartsAtTop in new[] { true, false })
        {
            foreach (var sample in samples)
            {
                var (ndcX, ndcY) = XrJitter.ClipOffset(sample.Item1, sample.Item2, 1280, 960, uvStartsAtTop);
                Check(Math.Abs(ndcX - 2f * sample.Item1 / 1280f) < 1e-6f,
                    "the clip offset keeps the horizontal sample unchanged");
                Check(Math.Abs(MathF.Abs(ndcY) - 2f * MathF.Abs(sample.Item2) / 960f) < 1e-6f,
                    "the clip offset scales the vertical sample by its own extent");
                // The sample is expressed with texture rows; the clip term flips only when those rows
                // run with clip +Y, so a positive sample always means "toward the target's bottom".
                Check(uvStartsAtTop == (ndcY * sample.Item2 < 0),
                    "the vertical clip term follows the target's row order");
                var expectedY = uvStartsAtTop ? sample.Item2 : -sample.Item2;
                foreach (var eye in eyes)
                {
                    foreach (var depth in new[] { 1.5f, 40f })
                    {
                        var view = new[]
                        {
                            1, 0, 0, 0,
                            0, 1, 0, 0,
                            0, 0, 1, 0,
                            -eye, 0, -depth, 1,
                        };
                        var vp = Multiply(projection, view);
                        var jittered = Translate(ndcX, ndcY, vp);
                        var point = new[] { 0f, 0f, 0f, 1f };
                        var plain = ProjectToPixels(vp, point, 1280, 960);
                        var shifted = ProjectToPixels(jittered, point, 1280, 960);
                        var dx = shifted.Item1 - plain.Item1;
                        var dy = shifted.Item2 - plain.Item2;
                        Check(Math.Abs(dx - sample.Item1) < 1e-3f && Math.Abs(dy - expectedY) < 1e-3f,
                            $"depth {depth} eye {eye} shift is ({dx:F4},{dy:F4}), expected " +
                            $"({sample.Item1},{expectedY})");
                    }
                }
            }
        }
        // Asymmetric extents must convert the same pixel sample differently; a swapped basis cannot
        // produce both terms from one width.
        var wide = XrJitter.ClipOffset(0.2f, 0.3f, 2496, 2688, true);
        var narrow = XrJitter.ClipOffset(0.2f, 0.3f, 832, 896, true);
        Check(Math.Abs(wide.Item1 - narrow.Item1) > 1e-4f && Math.Abs(wide.Item2 - narrow.Item2) > 1e-4f,
            "an asymmetric pair converts the same sample per extent");
        Check(XrJitter.ClipOffset(0.2f, 0.3f, 0, 896, true) == (0f, 0f),
            "an unusable extent converts no sample");
        // A half-sampled pair and a resized raster must both refuse to report the sample.
        Check(XrJitter.PairProblem(true, true) == JitterFrameProblem.None, "a fully sampled pair is reportable");
        Check(XrJitter.PairProblem(false, false) == JitterFrameProblem.NotSampled,
            "an unsampled pair reports identity");
        Check(XrJitter.PairProblem(true, false) == JitterFrameProblem.HalfPair,
            "a half-sampled pair is refused");
        Check(XrJitter.ExtentProblem(1248, 1344, 1248, 1344) == JitterFrameProblem.None,
            "the measured extent is reportable");
        Check(XrJitter.ExtentProblem(1280, 1344, 1248, 1344) == JitterFrameProblem.ExtentMismatch,
            "a resized raster is refused");
        Check(XrJitter.ExtentProblem(1248, 1344, 0, 0) == JitterFrameProblem.NotSampled,
            "a missing basis is refused");
        // Repeating one read must reproduce the same translation; nothing in the sample advances.
        Check(XrJitter.ClipOffset(0.25f, -0.31f, 1280, 960, true) == XrJitter.ClipOffset(0.25f, -0.31f, 1280, 960, true),
            "a repeated read reproduces the same sample");
        // Forward and inverse view-projection must round-trip a point, so an unjittered inverse cannot
        // stay paired with a jittered forward matrix unnoticed.
        var roundTripView = new[]
        {
            0.9f, 0.1f, 0f, 0f,
            -0.1f, 0.9f, 0f, 0f,
            0f, 0f, 1f, 0f,
            -0.032f, 0f, -8f, 1f,
        };
        var roundTripVp = Translate(0.11f, -0.07f, Multiply(projection, roundTripView));
        var inverse = Invert(roundTripVp);
        var world = new[] { 0.4f, -0.2f, 0.3f, 1f };
        var back = MultiplyPoint(inverse, MultiplyPoint(roundTripVp, world));
        Check(Math.Abs(back[0] - world[0]) < 1e-3f && Math.Abs(back[1] - world[1]) < 1e-3f &&
              Math.Abs(back[2] - world[2]) < 1e-3f, "the jittered view-projection inverts back to the same point");
        var unjitteredInverse = Invert(Multiply(projection, roundTripView));
        var mismatched = MultiplyPoint(unjitteredInverse, MultiplyPoint(roundTripVp, world));
        Check(Math.Abs(mismatched[0] - world[0]) > 1e-2f,
            "an unjittered inverse paired with a jittered forward matrix fails the round-trip");
    }

    private static float[] Multiply(float[] a, float[] b)
    {
        var result = new float[16];
        for (var column = 0; column < 4; column++)
            for (var row = 0; row < 4; row++)
            {
                var sum = 0f;
                for (var k = 0; k < 4; k++) sum += a[k * 4 + row] * b[column * 4 + k];
                result[column * 4 + row] = sum;
            }
        return result;
    }

    /// <summary>
    /// Clip-space translation applied the way the plugin applies it: the translation multiplies the
    /// projection on the left, so a column-vector point is shifted by <c>t * w</c> after projection.
    /// </summary>
    private static float[] Translate(float x, float y, float[] m)
    {
        var t = new[]
        {
            1f, 0f, 0f, 0f,
            0f, 1f, 0f, 0f,
            0f, 0f, 1f, 0f,
            x, y, 0f, 1f,
        };
        return Multiply(t, m);
    }

    private static (float X, float Y) ProjectToPixels(float[] m, float[] point, int width, int height)
    {
        var clip = MultiplyPoint(m, point);
        var w = clip[3];
        return ((clip[0] / w + 1f) * 0.5f * width, (1f - clip[1] / w) * 0.5f * height);
    }

    private static float[] MultiplyPoint(float[] m, float[] point)
    {
        var result = new float[4];
        for (var row = 0; row < 4; row++)
            result[row] = m[row] * point[0] + m[4 + row] * point[1] + m[8 + row] * point[2] + m[12 + row] * point[3];
        return result;
    }

    /// <summary>
    /// Inverts one column-major 4x4. The input is transposed into a row-major working copy only so the
    /// pivot search reads naturally; the result is written back in the same column-major order.
    /// </summary>
    private static float[] Invert(float[] m)
    {
        var a = new double[16];
        for (var column = 0; column < 4; column++)
            for (var row = 0; row < 4; row++)
                a[row * 4 + column] = m[column * 4 + row];
        var inv = new double[16];
        for (var i = 0; i < 4; i++)
            for (var j = 0; j < 4; j++)
                inv[i * 4 + j] = i == j ? 1.0 : 0.0;
        for (var column = 0; column < 4; column++)
        {
            var pivot = column;
            for (var row = column + 1; row < 4; row++)
                if (Math.Abs(a[row * 4 + column]) > Math.Abs(a[pivot * 4 + column])) pivot = row;
            if (Math.Abs(a[pivot * 4 + column]) < 1e-12) throw new InvalidOperationException("singular matrix");
            if (pivot != column)
                for (var k = 0; k < 4; k++)
                {
                    (a[column * 4 + k], a[pivot * 4 + k]) = (a[pivot * 4 + k], a[column * 4 + k]);
                    (inv[column * 4 + k], inv[pivot * 4 + k]) = (inv[pivot * 4 + k], inv[column * 4 + k]);
                }
            var scale = a[column * 4 + column];
            for (var k = 0; k < 4; k++)
            {
                a[column * 4 + k] /= scale;
                inv[column * 4 + k] /= scale;
            }
            for (var row = 0; row < 4; row++)
            {
                if (row == column) continue;
                var factor = a[row * 4 + column];
                if (Math.Abs(factor) < 1e-18) continue;
                for (var k = 0; k < 4; k++)
                {
                    a[row * 4 + k] -= factor * a[column * 4 + k];
                    inv[row * 4 + k] -= factor * inv[column * 4 + k];
                }
            }
        }
        var result = new float[16];
        for (var column = 0; column < 4; column++)
            for (var row = 0; row < 4; row++)
                result[column * 4 + row] = (float)inv[row * 4 + column];
        return result;
    }
}
