using System;
using System.Collections.Generic;
using BepInEx.Configuration;
using BigScale.Core;
using UnityEngine;

namespace BigScale;

/// <summary>
/// Persistent, validated configuration backed by <c>BepInEx/config/raharu.bigscale.cfg</c>.
/// <para>
/// Entries stay strongly typed: BepInEx's enum binding already refuses an unknown name and falls back
/// to the default, so no string layer is needed. After every save this class removes BepInEx's
/// generated <c># Acceptable values:</c> / <c># Multiple values can be set</c> lists and keeps the
/// single <c>[Diagnostics]</c> block last, because the panel is the primary editor for the values
/// themselves and BepInEx's alphabetical section order would otherwise open the file on development
/// switches.
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
    internal ConfigEntry<bool> Diagnostics = null!;
    internal ConfigEntry<KeyCode> FrameCaptureKey = null!;
    internal ConfigEntry<bool> StageProbe = null!;
    internal ConfigEntry<bool> XrProof = null!;
    internal ConfigEntry<bool> Dx12Validation = null!;
    internal ConfigEntry<bool> NativeLog = null!;

    internal ConfigEntry<bool> DLSS5Enabled = null!;
    internal ConfigEntry<bool> DLSS5MasterEnabled = null!;
    internal ConfigEntry<NeuralRenderingStyle> DLSS5Preset = null!;
    internal ConfigEntry<NeuralRenderingTiming> DLSS5Timing = null!;
    internal ConfigEntry<float> DLSS5Strength = null!;
    internal ConfigEntry<float> DLSS5Detail = null!;
    internal ConfigEntry<float> DLSS5Tone = null!;
    internal ConfigEntry<int> DLSS5Passes = null!;
    internal ConfigEntry<bool> DLSS5CharacterMask = null!;
    internal ConfigEntry<string> DLSS5CharacterMaskShaders = null!;

    private ConfigFile? _config;
    private Action<string>? _log;
    private Action? _changed;
    private bool _normalizing;
    private bool _normalizeReported;
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
            // A previous release bound this shortcut; this build has no keybind. Consume the orphan and
            // remove it before the first save so an upgraded file does not keep a dead key.
            var obsoleteMenuKey = config.Bind("UI", "MenuKey", string.Empty);
            config.Remove(obsoleteMenuKey.Definition);

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
                "interop device. Very slow; validation messages go to native.log when " +
                "Diagnostics/NativeLog is on.");
            NativeLog = config.Bind("Diagnostics", "NativeLog", false,
                "Writes BepInEx/BigScale/native.log for the native bridge. Off by default; enable it when " +
                "reporting a backend problem. The managed BepInEx log is always written. A change made in " +
                "the file applies on the next launch.");

            // Experimental Neural Rendering. Enabled only decides whether the settings categories are
            // built; the in-game switch is MasterEnabled and the category gate never rewrites it.
            DLSS5Enabled = config.Bind("DLSS5", "Enabled", false,
                "Exposes the experimental Neural Rendering settings categories when the settings page is " +
                "built. Independent of MasterEnabled, which is never changed by this gate.");
            DLSS5MasterEnabled = config.Bind("DLSS5", "MasterEnabled", true,
                "In-game Neural Rendering switch. Effective only while the category gate is on and an " +
                "upscaler other than Off is selected.");
            DLSS5Preset = config.Bind("DLSS5", "Preset", NeuralRenderingStyle.Default,
                "Neural Rendering look: Default, Natural or Cinematic.");
            DLSS5Timing = config.Bind("DLSS5", "Timing", NeuralRenderingTiming.BeforeUpscaling,
                "Run Neural Rendering before or after super resolution; always before frame generation.");
            DLSS5Strength = config.Bind("DLSS5", "Strength", 1f,
                "Overall Neural Rendering mix, 0.0-2.0. 1.0 is the reference; non-finite repairs to 1.0.");
            DLSS5Detail = config.Bind("DLSS5", "Detail", 1f,
                "Local structure and skin detail strength, 0.0-2.0. Non-finite repairs to 1.0.");
            DLSS5Tone = config.Bind("DLSS5", "Tone", 1f,
                "Local tone strength, 0.0-2.0. Later passes always evaluate with tone off, whatever " +
                "this value is.");
            DLSS5Passes = config.Bind("DLSS5", "Passes", 1,
                "Sequential Neural Rendering layers, 1-5. Each layer is a full evaluation; out-of-range " +
                "values are clamped.");
            DLSS5CharacterMask = config.Bind("DLSS5", "CharacterMask", true,
                "Keep the mod's character mask out of Neural Rendering edits.");
            DLSS5CharacterMaskShaders = config.Bind("DLSS5", "CharacterMaskShaders", string.Empty,
                "Optional comma-separated shader-name substrings added to the built-in character mask. " +
                "Empty means the built-in player shaders only.");

            Normalize(announce: false);
            config.Save();
        }
        finally
        {
            config.SaveOnConfigSet = autosave;
        }
        NormalizeSettingsFile();

        Enabled.SettingChanged += OnSettingChanged;
        Upscaler.SettingChanged += OnSettingChanged;
        Preset.SettingChanged += OnSettingChanged;
        FrameGeneration.SettingChanged += OnSettingChanged;
        VrFrameGeneration.SettingChanged += OnSettingChanged;
        FrameMultiplier.SettingChanged += OnSettingChanged;
        Diagnostics.SettingChanged += OnSettingChanged;
        StageProbe.SettingChanged += OnSettingChanged;
        NativeLog.SettingChanged += OnSettingChanged;
        DLSS5Enabled.SettingChanged += OnSettingChanged;
        DLSS5MasterEnabled.SettingChanged += OnSettingChanged;
        DLSS5Preset.SettingChanged += OnSettingChanged;
        DLSS5Timing.SettingChanged += OnSettingChanged;
        DLSS5Strength.SettingChanged += OnSettingChanged;
        DLSS5Detail.SettingChanged += OnSettingChanged;
        DLSS5Tone.SettingChanged += OnSettingChanged;
        DLSS5Passes.SettingChanged += OnSettingChanged;
        DLSS5CharacterMask.SettingChanged += OnSettingChanged;
        DLSS5CharacterMaskShaders.SettingChanged += OnSettingChanged;
    }

    private void OnSettingChanged(object? sender, EventArgs args)
    {
        if (_normalizing) return;
        Normalize(announce: true);
        NormalizeSettingsFile();
        if (_normalizing) return;
        _changed?.Invoke();
    }

    /// <summary>
    /// Rewrites this plugin's own file: BepInEx's generated value lists are removed and the single
    /// <c>[Diagnostics]</c> block is kept last. The entries are already self-documenting, and the panel
    /// is the primary editor for everything the file does not explain. A failure here is cosmetic: it
    /// is reported once and never blocks a save.
    /// </summary>
    private void NormalizeSettingsFile()
    {
        var path = _config?.ConfigFilePath;
        if (string.IsNullOrEmpty(path) || _normalizeReported) return;
        try
        {
            SettingsFileText.Normalize(path!);
        }
        catch (Exception exception)
        {
            _normalizeReported = true;
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

        // Neural Rendering repairs. The category gate and the in-game master switch are deliberately
        // absent: each is a user decision, and neither may be rewritten by normalization.
        if (!Enum.IsDefined(typeof(NeuralRenderingStyle), DLSS5Preset.Value))
        {
            if (announce) Report($"invalid Neural Rendering preset '{DLSS5Preset.Value}'; using Default.");
            SetWithoutNotify(DLSS5Preset, NeuralRenderingStyle.Default);
        }

        if (!Enum.IsDefined(typeof(NeuralRenderingTiming), DLSS5Timing.Value))
        {
            if (announce) Report($"invalid Neural Rendering timing '{DLSS5Timing.Value}'; using BeforeUpscaling.");
            SetWithoutNotify(DLSS5Timing, NeuralRenderingTiming.BeforeUpscaling);
        }

        var passes = NeuralRenderingModes.ClampPasses(DLSS5Passes.Value);
        if (passes != DLSS5Passes.Value)
        {
            if (announce) Report($"invalid Neural Rendering pass count '{DLSS5Passes.Value}'; using {passes}.");
            SetWithoutNotify(DLSS5Passes, passes);
        }

        NormalizeUnit(DLSS5Strength, "Strength", announce);
        NormalizeUnit(DLSS5Detail, "Detail", announce);
        NormalizeUnit(DLSS5Tone, "Tone", announce);

        var storedShaders = DLSS5CharacterMaskShaders.Value;
        var shaders = storedShaders ?? string.Empty;
        var normalizedShaders = NormalizeShaderList(shaders);
        if (!string.Equals(storedShaders, normalizedShaders, StringComparison.Ordinal))
        {
            if (announce)
                Report($"Neural Rendering character mask shaders '{shaders}' normalized to '{normalizedShaders}'.");
            SetWithoutNotify(DLSS5CharacterMaskShaders, normalizedShaders);
        }
    }

    /// <summary>Repairs one 0..2 strength entry, reporting only when the value actually changed.</summary>
    private void NormalizeUnit(ConfigEntry<float> entry, string name, bool announce)
    {
        var value = NeuralRenderingModes.ClampUnit(entry.Value, 1f);
        if (value.Equals(entry.Value)) return;
        if (announce) Report($"invalid Neural Rendering {name} '{entry.Value}'; using {value}.");
        SetWithoutNotify(entry, value);
    }

    /// <summary>
    /// Splits an optional comma list, trims each entry and drops empty ones. The stored form is the
    /// canonical <c>a, b</c> text so a hand-written file and the panel agree byte for byte.
    /// </summary>
    private static string NormalizeShaderList(string value)
    {
        var parts = value.Split(',');
        var kept = new List<string>(parts.Length);
        foreach (var part in parts)
        {
            var trimmed = part.Trim();
            if (trimmed.Length != 0) kept.Add(trimmed);
        }
        return string.Join(", ", kept);
    }

    private static void SetWithoutNotify<T>(ConfigEntry<T> entry, T value)
    {
        if (Equals(entry.Value, value)) return;
        entry.Value = value;
    }

    private void Report(string message) => _log?.Invoke(message);
}
