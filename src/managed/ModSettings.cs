using System;
using BepInEx.Configuration;
using BigScale.Core;
using UnityEngine;

namespace BigScale;

/// <summary>
/// Persistent, validated configuration backed by <c>BepInEx/config/raharu.bigscale.cfg</c>.
/// <para>
/// Entries stay strongly typed: BepInEx's enum binding already refuses an unknown name and falls back
/// to the default, so no string layer is needed. The only generated text this class removes is the
/// <c># Acceptable values:</c> / <c># Multiple values can be set</c> block, which is noise for a file
/// whose panel is the primary editor.
/// </para>
/// </summary>
internal sealed class ModSettings
{
    internal ConfigEntry<bool> Enabled = null!;
    internal ConfigEntry<UpscalerKind> Upscaler = null!;
    internal ConfigEntry<UpscalingPreset> Preset = null!;
    internal ConfigEntry<FrameGenerationKind> FrameGeneration = null!;
    internal ConfigEntry<FrameGenerationKind> VrFrameGeneration = null!;
    internal ConfigEntry<int> FrameMultiplier = null!;
    internal ConfigEntry<KeyCode> MenuKey = null!;
    internal ConfigEntry<bool> Diagnostics = null!;
    internal ConfigEntry<KeyCode> FrameCaptureKey = null!;
    internal ConfigEntry<bool> StageProbe = null!;
    internal ConfigEntry<bool> XrProof = null!;
    internal ConfigEntry<bool> Dx12Validation = null!;

    private ConfigFile? _config;
    private Action<string>? _log;
    private Action? _changed;
    private bool _normalizing;
    private bool _noiseReported;
    private bool _streamlineReported;

    /// <summary>Raised for any change, whatever its origin: the panel, the file, or another mod.</summary>
    internal event Action? Changed
    {
        add => _changed += value;
        remove => _changed -= value;
    }

    internal void Bind(ConfigFile config, Action<string> log)
    {
        _log = log;
        _config = config;
        var autosave = config.SaveOnConfigSet;
        config.SaveOnConfigSet = false;
        try
        {
            Enabled = config.Bind("General", "Enabled", true,
                "Master switch for super resolution and frame generation. Disabling restores the game's " +
                "own renderer without rewriting either selection.");
            Upscaler = config.Bind("Upscaling", "Upscaler", UpscalerKind.Dlss,
                "Remembered reconstruction backend. Edit it in the in-game Upscaling category.");
            Preset = config.Bind("Upscaling", "Mode", UpscalingPreset.Quality,
                "Quality preset. Edit it in the in-game Upscaling category.");
            FrameGeneration = config.Bind("FrameGeneration", "Provider", FrameGenerationKind.Off,
                "Experimental desktop frame generation. Independent of the upscaler; disabled during VR.");
            VrFrameGeneration = config.Bind("FrameGeneration", "VrProvider", FrameGenerationKind.Off,
                "VR-only frame generation. Currently experimental and not working reliably; use Off " +
                "unless you are developing it.");
            FrameMultiplier = config.Bind("FrameGeneration", "Multiplier", 2,
                "Remembered total presented frames per source frame, 2-6. The provider clamps the " +
                "effective value; this remembered choice is never rewritten by that clamp.");
            MenuKey = config.Bind("UI", "MenuKey", KeyCode.Delete,
                "Key that opens and closes the in-game Upscaling category. Use None to disable.");
            Diagnostics = config.Bind("Diagnostics", "Enabled", false,
                "Development only. Aggregate per-eye counters, history and result codes, plus one " +
                "centre-texel sample read back from the native bridge. Never writes an image and never " +
                "changes a pixel.");
            FrameCaptureKey = config.Bind("Diagnostics", "FrameCaptureKey", KeyCode.F9,
                "Development only. With Diagnostics on, captures the next five frame-generation pairs " +
                "under BepInEx/BigScale/fg-capture/. Use None to disable.");
            StageProbe = config.Bind("Diagnostics", "StageProbe", false,
                "Development only. Writes the 256x256 input/output hashes, BMP pairs at evaluations 120 " +
                "and 600, and the 8x8 guide sample under BepInEx/BigScale. Adds GPU readbacks; changes " +
                "no pixel.");
            XrProof = config.Bind("Diagnostics", "XrProof", false,
                "Development only. Restart required. Starts the bounded presentation proof with " +
                "FrameCaptureKey once a warm stereo history exists: identity band, late-pass removal and " +
                "the xr-proof/xr-tap/sr-probe BMPs.");
            Dx12Validation = config.Bind("Diagnostics", "Dx12Validation", false,
                "Development only. Restart required. Enables the D3D12 debug layer for the shared " +
                "interop device. Very slow; validation messages go to native.log.");

            Normalize(announce: false);
            config.Save();
        }
        finally
        {
            config.SaveOnConfigSet = autosave;
        }
        StripAcceptableValueNoise();

        Enabled.SettingChanged += OnSettingChanged;
        Upscaler.SettingChanged += OnSettingChanged;
        Preset.SettingChanged += OnSettingChanged;
        FrameGeneration.SettingChanged += OnSettingChanged;
        VrFrameGeneration.SettingChanged += OnSettingChanged;
        FrameMultiplier.SettingChanged += OnSettingChanged;
        Diagnostics.SettingChanged += OnSettingChanged;
        StageProbe.SettingChanged += OnSettingChanged;
        MenuKey.SettingChanged += OnSettingChanged;
    }

    private void OnSettingChanged(object? sender, EventArgs args)
    {
        if (_normalizing) return;
        Normalize(announce: true);
        StripAcceptableValueNoise();
        if (_normalizing) return;
        _changed?.Invoke();
    }

    /// <summary>
    /// Removes BepInEx's generated value lists from this plugin's own file. The entries are already
    /// self-documenting, and the panel is the primary editor for everything the file does not explain.
    /// A failure here is cosmetic: it is reported once and never blocks a save.
    /// </summary>
    private void StripAcceptableValueNoise()
    {
        var path = _config?.ConfigFilePath;
        if (string.IsNullOrEmpty(path) || _noiseReported) return;
        try
        {
            SettingsFileText.StripGeneratedNoise(path!);
        }
        catch (Exception exception)
        {
            _noiseReported = true;
            Report("could not simplify the settings file: " + exception.Message);
        }
    }

    /// <summary>Reports and repairs invalid values instead of trusting the file.</summary>
    internal void Normalize() => Normalize(announce: true);

    private void Normalize(bool announce)
    {
        _normalizing = true;
        try
        {
            NormalizeCore(announce);
        }
        finally
        {
            _normalizing = false;
        }
    }

    /// <summary>
    /// Repairs the stored schema. Everything here is self-contained: no legacy key is read, and the
    /// only fallback a repair can install is the default provider with master disabled.
    /// </summary>
    private void NormalizeCore(bool announce)
    {
        if (!Enum.IsDefined(typeof(UpscalerKind), Upscaler.Value))
        {
            if (announce) Report($"invalid Upscaler '{Upscaler.Value}'; using Dlss with master disabled.");
            SetWithoutNotify(Enabled, false);
            SetWithoutNotify(Upscaler, UpscalerKind.Dlss);
        }
        else if (Upscaler.Value == UpscalerKind.Off)
        {
            // Off is a wire/control identity, never a persisted choice: it disables master once and
            // remembers the last real provider.
            if (announce) Report("Upscaler Off disables master and remembers DLSS.");
            SetWithoutNotify(Enabled, false);
            SetWithoutNotify(Upscaler, UpscalerKind.Dlss);
        }

        if (!Enum.IsDefined(typeof(UpscalingPreset), Preset.Value))
        {
            if (announce) Report($"invalid preset '{Preset.Value}'; using Quality.");
            SetWithoutNotify(Preset, UpscalingPreset.Quality);
        }

        var frameGeneration = FrameGeneration.Value;
        if (!Enum.IsDefined(typeof(FrameGenerationKind), frameGeneration))
        {
            if (announce) Report($"invalid frame-generation provider '{frameGeneration}'; using Off.");
            SetWithoutNotify(FrameGeneration, FrameGenerationKind.Off);
        }
        else if (frameGeneration == FrameGenerationKind.Streamline)
        {
            // The Streamline provider is compiled but not selectable yet: the panel cannot show it, so a
            // file that names it is repaired to Off. This is a work-in-progress note rather than a repair
            // complaint, so it is reported once even by the silent bind pass.
            if (!_streamlineReported)
            {
                _streamlineReported = true;
                Report("frame generation: the Streamline provider is work in progress (MFG unlock for " +
                       "Ada) and not selectable; the stored choice was reset to Off.");
            }
            SetWithoutNotify(FrameGeneration, FrameGenerationKind.Off);
        }

        // VR frame generation is the compositor path only: AMD FidelityFX is the single provider, and
        // every other stored value (including a desktop selection written by hand) is repaired to Off.
        var vrFrameGeneration = VrFrameGeneration.Value;
        if (vrFrameGeneration != FrameGenerationKind.Off && vrFrameGeneration != FrameGenerationKind.Fsr)
        {
            if (announce) Report($"invalid VR frame-generation provider '{vrFrameGeneration}'; using Off.");
            SetWithoutNotify(VrFrameGeneration, FrameGenerationKind.Off);
        }

        // The remembered total is a preference, not a capability claim: only the range is repaired
        // here and the provider's discovered maximum clamps the effective value at runtime.
        var multiplier = FrameMultiplier.Value;
        if (multiplier < 2) multiplier = 2;
        else if (multiplier > 6) multiplier = 6;
        if (multiplier != FrameMultiplier.Value)
        {
            if (announce)
                Report($"invalid frame-generation multiplier '{FrameMultiplier.Value}'; using {multiplier}.");
            SetWithoutNotify(FrameMultiplier, multiplier);
        }
    }

    private static void SetWithoutNotify<T>(ConfigEntry<T> entry, T value)
    {
        if (Equals(entry.Value, value)) return;
        entry.Value = value;
    }

    private void Report(string message) => _log?.Invoke(message);
}
