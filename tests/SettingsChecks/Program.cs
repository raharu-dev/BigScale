using System;
using System.Collections.Generic;
using System.IO;
using BigScale;
using BigScale.Core;
using BepInEx;
using BepInEx.Configuration;
using UnityEngine;

// Deterministic checks for the real settings binding: the production ModSettings.cs is compiled
// against the installed BepInEx core and the Unity key enum only. The BepInEx IL2CPP chainloader
// assembly is never referenced.
// Every case binds a real ConfigFile over a real file, so the checks exercise parsing, persistence
// and reload behavior rather than a stand-in.
internal static class Program
{
    private static int _failures;

    private static int Main()
    {
        CheckMissingFile();
        CheckMenuKeyRemoved();
        CheckFrameCaptureKeyRoundTrips();
        CheckNoneDisables();
        CheckUnparsableValues();
        CheckStreamlineNormalizes();
        CheckNeuralRenderingPersistence();
        CheckNeuralRenderingRepairs();
        CheckNoiseAfterPanelChange();
        CheckNativeLog();
        CheckSettingsFileNormalize();
        CheckReload();
        Console.WriteLine(_failures == 0 ? "ALL SETTINGS CHECKS PASSED" : $"{_failures} CHECK(S) FAILED");
        return _failures == 0 ? 0 : 1;
    }

    private static void Check(bool condition, string description)
    {
        if (condition) return;
        _failures++;
        Console.WriteLine("FAIL: " + description);
    }

    private static string NewDirectory()
    {
        var directory = Path.Combine(Path.GetTempPath(), "bigscale-settings-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        return directory;
    }

    private static void Cleanup(string directory)
    {
        try
        {
            Directory.Delete(directory, true);
        }
        catch (IOException)
        {
            // Only this harness' own temporary tree; a stale handle must not mask a check result.
        }
        catch (UnauthorizedAccessException)
        {
        }
    }

    /// <summary>The real plugin identity, so the file carries the same header the game writes.</summary>
    private static readonly BepInPlugin PluginInfo = new("raharu.bigscale", "BigScale", "1.0.0");

    /// <summary>
    /// Writes the selections a player may have stored. Only the keys under test are written; every
    /// other key is created by the bind with its default.
    /// </summary>
    private static void WriteConfig(string path, string frameCaptureKey = "F9", string upscaler = "XeSs",
        string mode = "Performance", bool enabled = false, bool dlss5Enabled = false,
        string dlss5Master = "true", string dlss5Preset = "Default", string dlss5Timing = "BeforeUpscaling",
        string dlss5Strength = "1", string dlss5Detail = "1", string dlss5Tone = "1", string dlss5Passes = "1",
        string dlss5CharacterMask = "true", string dlss5Shaders = "", string nativeLog = "false")
    {
        var lines = new List<string>
        {
            "## Settings file was created by plugin BigScale v1.0.0",
            "## Plugin GUID: raharu.bigscale",
            string.Empty,
            "[General]",
            "Enabled = " + (enabled ? "true" : "false"),
            string.Empty,
            "[Upscaling]",
            "Upscaler = " + upscaler,
            "Mode = " + mode,
            string.Empty,
            "[Diagnostics]",
            string.Empty,
            "## Development only. With Diagnostics on, captures the next five frame-generation pairs.",
            "FrameCaptureKey = " + frameCaptureKey,
            "NativeLog = " + nativeLog,
            string.Empty,
            "[DLSS5]",
            "Enabled = " + (dlss5Enabled ? "true" : "false"),
            "MasterEnabled = " + dlss5Master,
            "Preset = " + dlss5Preset,
            "Timing = " + dlss5Timing,
            "Strength = " + dlss5Strength,
            "Detail = " + dlss5Detail,
            "Tone = " + dlss5Tone,
            "Passes = " + dlss5Passes,
            "CharacterMask = " + dlss5CharacterMask,
            "CharacterMaskShaders = " + dlss5Shaders,
            string.Empty,
        };
        File.WriteAllLines(path, lines);
    }

    /// <summary>True when no line carries a generated acceptable-value block or a legacy name.</summary>
    private static void CheckFileShape(string text, string label)
    {
        var lines = text.Split('\n');
        var noise = 0;
        foreach (var line in lines)
        {
            var trimmed = line.Trim();
            if (trimmed.StartsWith("# Acceptable values:", StringComparison.Ordinal) ||
                trimmed.StartsWith("# Multiple values can be set", StringComparison.Ordinal))
                noise++;
        }
        Check(noise == 0, $"{label}: no generated acceptable-value lines, found {noise}");
        foreach (var legacy in new[] { "local.bigwalk.dlss", "BigWalkDLSS", "KeyboardShortcut", "BilinearComparison", "MenuKey" })
            Check(!text.Contains(legacy, StringComparison.Ordinal), $"{label}: no '{legacy}' in the file");
        CheckDiagnosticsLast(text, label);
    }

    /// <summary>
    /// <c>[Diagnostics]</c> must be present and be the final section: BepInEx sorts sections
    /// alphabetically, so the post-save normalization is the only thing that keeps the file opening
    /// on the player-facing settings.
    /// </summary>
    private static void CheckDiagnosticsLast(string text, string label)
    {
        var lines = text.Split('\n');
        var headerIndex = -1;
        for (var index = 0; index < lines.Length; index++)
        {
            if (!string.Equals(lines[index].Trim(), "[Diagnostics]", StringComparison.Ordinal)) continue;
            headerIndex = index;
            break;
        }
        Check(headerIndex >= 0, $"{label}: [Diagnostics] section present");
        if (headerIndex < 0) return;
        for (var index = headerIndex + 1; index < lines.Length; index++)
        {
            var trimmed = lines[index].Trim();
            if (trimmed.Length < 2 || trimmed[0] != '[' || trimmed[trimmed.Length - 1] != ']') continue;
            Check(false, $"{label}: [Diagnostics] must be the last section, found {trimmed} after it");
            return;
        }
    }

    private static void CheckMissingFile()
    {
        var directory = NewDirectory();
        try
        {
            var path = Path.Combine(directory, "raharu.bigscale.cfg");
            var config = new ConfigFile(path, false, PluginInfo);
            var messages = new List<string>();
            var settings = new ModSettings();
            settings.Bind(config, messages.Add);

            Check(File.Exists(path), "binding a fresh file persists it");
            var text = File.ReadAllText(path);
            Check(text.Contains("## Plugin GUID: raharu.bigscale"),
                "a fresh config carries the plugin GUID header");
            Check(text.Contains("[Diagnostics]"), "a fresh config carries the diagnostics section");

            Check(settings.Enabled.Value, "General/Enabled defaults to true");
            Check(settings.Upscaler.Value == UpscalerKind.Dlss, "Upscaling/Upscaler defaults to Dlss");
            Check(settings.Preset.Value == UpscalingPreset.Quality, "Upscaling/Mode defaults to Quality");
            Check(settings.FrameGeneration.Value == FrameGenerationKind.Off,
                "FrameGeneration/Provider defaults to Off");
            Check(settings.VrFrameGeneration.Value == FrameGenerationKind.Off,
                "FrameGeneration/VrProvider defaults to Off");
            Check(settings.FrameMultiplier.Value == 2, "FrameGeneration/Multiplier defaults to 2");
            Check(!settings.Diagnostics.Value, "Diagnostics/Enabled defaults to false");
            Check(settings.FrameCaptureKey.Value == KeyCode.F9, "Diagnostics/FrameCaptureKey defaults to F9");
            Check(!settings.StageProbe.Value, "Diagnostics/StageProbe defaults to false");
            Check(!settings.XrProof.Value, "Diagnostics/XrProof defaults to false");
            Check(!settings.Dx12Validation.Value, "Diagnostics/Dx12Validation defaults to false");
            Check(!settings.NativeLog.Value, "Diagnostics/NativeLog defaults to false");
            Check(text.Contains("NativeLog = false"), "the fresh diagnostics section writes NativeLog = false");
            Check(!settings.DLSS5Enabled.Value, "DLSS5/Enabled defaults to false");
            Check(settings.DLSS5MasterEnabled.Value, "DLSS5/MasterEnabled defaults to true");
            Check(settings.DLSS5Preset.Value == NeuralRenderingStyle.Default,
                "DLSS5/Preset defaults to Default");
            Check(settings.DLSS5Timing.Value == NeuralRenderingTiming.BeforeUpscaling,
                "DLSS5/Timing defaults to BeforeUpscaling");
            Check(Math.Abs(settings.DLSS5Strength.Value - 1f) < 0.0001f, "DLSS5/Strength defaults to 1.0");
            Check(Math.Abs(settings.DLSS5Detail.Value - 1f) < 0.0001f, "DLSS5/Detail defaults to 1.0");
            Check(Math.Abs(settings.DLSS5Tone.Value - 1f) < 0.0001f, "DLSS5/Tone defaults to 1.0");
            Check(settings.DLSS5Passes.Value == 1, "DLSS5/Passes defaults to 1");
            Check(settings.DLSS5CharacterMask.Value, "DLSS5/CharacterMask defaults to true");
            Check((settings.DLSS5CharacterMaskShaders.Value ?? string.Empty).Length == 0,
                "DLSS5/CharacterMaskShaders defaults to empty");
            Check(text.Contains("[DLSS5]"), "a fresh config carries the DLSS5 section");
            Check(text.Contains("Preset = Default") && text.Contains("Timing = BeforeUpscaling"),
                "the fresh DLSS5 section writes its enum defaults by name");
            Check(text.Contains("Passes = 1") && text.Contains("CharacterMaskShaders ="),
                "the fresh DLSS5 section writes the pass count and empty shader list");
            Check(settings.FrameMultiplier.Value == 2 && text.Contains("Multiplier = 2"),
                "the multiplier is written with its default");

            Check(messages.Count == 0,
                "a fresh file needs no repair report, got: " + string.Join(" | ", messages));
            CheckFileShape(text, "fresh file");
        }
        finally
        {
            Cleanup(directory);
        }
    }

    private static void CheckMenuKeyRemoved()
    {
        var directory = NewDirectory();
        try
        {
            var path = Path.Combine(directory, "raharu.bigscale.cfg");
            File.WriteAllLines(path, new[]
            {
                "## Settings file was created by plugin BigScale v1.0.0",
                "## Plugin GUID: raharu.bigscale",
                string.Empty,
                "[UI]",
                "MenuKey = Delete",
                string.Empty,
            });

            var messages = new List<string>();
            var settings = new ModSettings();
            settings.Bind(new ConfigFile(path, false, PluginInfo), messages.Add);
            var text = File.ReadAllText(path);
            Check(!text.Contains("MenuKey", StringComparison.Ordinal),
                "an upgraded file loses the obsolete UI/MenuKey entry");
            CheckFileShape(text, "file after removing UI/MenuKey");
        }
        finally
        {
            Cleanup(directory);
        }
    }

    private static void CheckFrameCaptureKeyRoundTrips()
    {
        var directory = NewDirectory();
        try
        {
            var path = Path.Combine(directory, "raharu.bigscale.cfg");
            WriteConfig(path, "F8");

            var messages = new List<string>();
            var settings = new ModSettings();
            settings.Bind(new ConfigFile(path, false, PluginInfo), messages.Add);
            Check(settings.FrameCaptureKey.Value == KeyCode.F8,
                "an existing frame-capture key is honored");

            var rebound = new ModSettings();
            rebound.Bind(new ConfigFile(path, false, PluginInfo), messages.Add);
            Check(rebound.FrameCaptureKey.Value == KeyCode.F8, "F8 survives save and rebind");
            Check(!rebound.Enabled.Value && rebound.Upscaler.Value == UpscalerKind.XeSs &&
                  rebound.Preset.Value == UpscalingPreset.Performance,
                "F8 does not rewrite the other selections");
            CheckFileShape(File.ReadAllText(path), "round-tripped file");
        }
        finally
        {
            Cleanup(directory);
        }
    }

    private static void CheckNoneDisables()
    {
        var directory = NewDirectory();
        try
        {
            var path = Path.Combine(directory, "raharu.bigscale.cfg");
            WriteConfig(path, "None");

            var messages = new List<string>();
            var settings = new ModSettings();
            settings.Bind(new ConfigFile(path, false, PluginInfo), messages.Add);
            Check(settings.FrameCaptureKey.Value == KeyCode.None, "None disables the frame-capture key");

            var rebound = new ModSettings();
            rebound.Bind(new ConfigFile(path, false, PluginInfo), messages.Add);
            Check(rebound.FrameCaptureKey.Value == KeyCode.None, "None survives save and rebind");
        }
        finally
        {
            Cleanup(directory);
        }
    }

    private static void CheckUnparsableValues()
    {
        var directory = NewDirectory();
        try
        {
            var combination = Path.Combine(directory, "combination.cfg");
            WriteConfig(combination, "Delete + LeftControl");
            var messages = new List<string>();
            var settings = new ModSettings();
            settings.Bind(new ConfigFile(combination, false, PluginInfo), messages.Add);
            Check(settings.FrameCaptureKey.Value == KeyCode.F9,
                "an unparsable frame-capture key leaves the F9 default");
            Check(!settings.Enabled.Value && settings.Upscaler.Value == UpscalerKind.XeSs &&
                  settings.Preset.Value == UpscalingPreset.Performance,
                "an unparsable key does not disturb the other selections");

            var unknown = Path.Combine(directory, "unknown.cfg");
            WriteConfig(unknown, "NotAKey");
            var unknownMessages = new List<string>();
            var unknownSettings = new ModSettings();
            unknownSettings.Bind(new ConfigFile(unknown, false, PluginInfo), unknownMessages.Add);
            Check(unknownSettings.FrameCaptureKey.Value == KeyCode.F9,
                "an unknown frame-capture key name leaves the F9 default");

            var badUpscaler = Path.Combine(directory, "bad-upscaler.cfg");
            WriteConfig(badUpscaler, upscaler: "NotABackend");
            var badMessages = new List<string>();
            var badSettings = new ModSettings();
            badSettings.Bind(new ConfigFile(badUpscaler, false, PluginInfo), badMessages.Add);
            Check(badSettings.Upscaler.Value == UpscalerKind.Dlss,
                "an unknown upscaler falls back to Dlss, got " + badSettings.Upscaler.Value);
            Check(!badSettings.Enabled.Value, "an unknown upscaler leaves master disabled");
            CheckFileShape(File.ReadAllText(badUpscaler), "repaired file");
        }
        finally
        {
            Cleanup(directory);
        }
    }

    private static void CheckStreamlineNormalizes()
    {
        var directory = NewDirectory();
        try
        {
            var path = Path.Combine(directory, "raharu.bigscale.cfg");
            WriteConfig(path, enabled: true);
            File.AppendAllLines(path, new[]
            {
                string.Empty,
                "[FrameGeneration]",
                "Provider = Streamline",
                "VrProvider = Nvidia",
                "Multiplier = 9",
                string.Empty,
            });

            var messages = new List<string>();
            var settings = new ModSettings();
            settings.Bind(new ConfigFile(path, false, PluginInfo), messages.Add);
            Check(settings.FrameGeneration.Value == FrameGenerationKind.Off,
                "a stored Streamline provider is reset to Off, got " + settings.FrameGeneration.Value);
            Check(settings.VrFrameGeneration.Value == FrameGenerationKind.Off,
                "a desktop-only VR provider is reset to Off");
            Check(settings.FrameMultiplier.Value == 6,
                "an out-of-range multiplier is clamped, got " + settings.FrameMultiplier.Value);
            Check(messages.Exists(m => m.Contains("Streamline", StringComparison.Ordinal)),
                "the Streamline reset is reported once");
            CheckFileShape(File.ReadAllText(path), "normalized file");
        }
        finally
        {
            Cleanup(directory);
        }
    }

    /// <summary>
    /// A stored neural selection must survive the gate being off: the gate only decides whether the
    /// category is built, so binding with Enabled=false may not rewrite MasterEnabled or any tuning.
    /// </summary>
    private static void CheckNeuralRenderingPersistence()
    {
        var directory = NewDirectory();
        try
        {
            var path = Path.Combine(directory, "raharu.bigscale.cfg");
            WriteConfig(path, dlss5Enabled: false, dlss5Master: "false", dlss5Strength: "1.5",
                dlss5CharacterMask: "false", dlss5Shaders: "  player_a , , player_b  ");

            var messages = new List<string>();
            var settings = new ModSettings();
            settings.Bind(new ConfigFile(path, false, PluginInfo), messages.Add);
            Check(!settings.DLSS5Enabled.Value, "the DLSS5 category gate stays false");
            Check(!settings.DLSS5MasterEnabled.Value,
                "a stored MasterEnabled=false survives a disabled category gate");
            Check(!settings.DLSS5CharacterMask.Value, "a stored CharacterMask=false is honored");
            Check(Math.Abs(settings.DLSS5Strength.Value - 1.5f) < 0.0001f,
                "a stored neural strength is honored");
            Check(settings.DLSS5CharacterMaskShaders.Value == "player_a, player_b",
                "a hand-written shader list is canonicalized, got '" +
                settings.DLSS5CharacterMaskShaders.Value + "'");

            var text = File.ReadAllText(path);
            Check(text.Contains("MasterEnabled = false"),
                "the disabled gate does not rewrite MasterEnabled on disk");
            Check(text.Contains("CharacterMaskShaders = player_a, player_b"),
                "the canonical shader list is persisted");
            CheckFileShape(text, "neural settings file");

            var rebound = new ModSettings();
            rebound.Bind(new ConfigFile(path, false, PluginInfo), messages.Add);
            Check(!rebound.DLSS5MasterEnabled.Value,
                "MasterEnabled=false survives save and rebind while the gate stays off");
            Check(!rebound.DLSS5CharacterMask.Value, "CharacterMask=false survives save and rebind");
            Check(Math.Abs(rebound.DLSS5Strength.Value - 1.5f) < 0.0001f,
                "the neural strength survives save and rebind");

            var changes = 0;
            rebound.Changed += () => changes++;
            rebound.DLSS5CharacterMask.Value = true;
            Check(changes == 1, "a neural toggle raises exactly one change, got " + changes);
            Check(File.ReadAllText(path).Contains("CharacterMask = true"),
                "a neural toggle is persisted immediately");
        }
        finally
        {
            Cleanup(directory);
        }
    }

    /// <summary>
    /// Every neural value is repaired instead of trusted: enum names, the 1..5 pass range and the
    /// finite 0..2 tuning range all fall back to the documented defaults.
    /// </summary>
    private static void CheckNeuralRenderingRepairs()
    {
        var directory = NewDirectory();
        try
        {
            var clamped = Path.Combine(directory, "clamped.cfg");
            WriteConfig(clamped, dlss5Preset: "NotAStyle", dlss5Timing: "NotATiming",
                dlss5Passes: "99", dlss5Strength: "9");
            var messages = new List<string>();
            var settings = new ModSettings();
            settings.Bind(new ConfigFile(clamped, false, PluginInfo), messages.Add);
            Check(settings.DLSS5Preset.Value == NeuralRenderingStyle.Default,
                "an unknown preset name falls back to Default, got " + settings.DLSS5Preset.Value);
            Check(settings.DLSS5Timing.Value == NeuralRenderingTiming.BeforeUpscaling,
                "an unknown timing name falls back to BeforeUpscaling, got " + settings.DLSS5Timing.Value);
            Check(settings.DLSS5Passes.Value == 5,
                "Passes=99 clamps to 5, got " + settings.DLSS5Passes.Value);
            Check(Math.Abs(settings.DLSS5Strength.Value - 2f) < 0.0001f,
                "Strength=9 clamps to 2.0, got " + settings.DLSS5Strength.Value);

            // The bind pass is silent by design; a panel-style assignment is the announcing path.
            settings.DLSS5Passes.Value = 99;
            Check(settings.DLSS5Passes.Value == 5,
                "a panel Passes=99 repairs to 5, got " + settings.DLSS5Passes.Value);
            Check(messages.Exists(m => m.Contains("pass count", StringComparison.Ordinal)),
                "a clamped pass count is reported");
            settings.DLSS5Strength.Value = 9f;
            Check(Math.Abs(settings.DLSS5Strength.Value - 2f) < 0.0001f,
                "a panel Strength=9 repairs to 2.0, got " + settings.DLSS5Strength.Value);
            Check(messages.Exists(m => m.Contains("Strength", StringComparison.Ordinal)),
                "a clamped neural strength is reported");

            // The file cannot carry an undefined enum value: the binder has already replaced it with
            // the default. Assigning one directly exercises the same repair the panel would hit.
            settings.DLSS5Preset.Value = (NeuralRenderingStyle)42;
            Check(settings.DLSS5Preset.Value == NeuralRenderingStyle.Default,
                "an undefined preset wire value repairs to Default");
            settings.DLSS5Timing.Value = (NeuralRenderingTiming)42;
            Check(settings.DLSS5Timing.Value == NeuralRenderingTiming.BeforeUpscaling,
                "an undefined timing wire value repairs to BeforeUpscaling");

            var zero = Path.Combine(directory, "zero.cfg");
            WriteConfig(zero, dlss5Passes: "0");
            var zeroSettings = new ModSettings();
            zeroSettings.Bind(new ConfigFile(zero, false, PluginInfo), messages.Add);
            Check(zeroSettings.DLSS5Passes.Value == 1,
                "Passes=0 clamps to 1, got " + zeroSettings.DLSS5Passes.Value);

            var nan = Path.Combine(directory, "nan.cfg");
            WriteConfig(nan, dlss5Strength: "NaN");
            var nanSettings = new ModSettings();
            nanSettings.Bind(new ConfigFile(nan, false, PluginInfo), messages.Add);
            Check(float.IsFinite(nanSettings.DLSS5Strength.Value) &&
                  Math.Abs(nanSettings.DLSS5Strength.Value - 1f) < 0.0001f,
                "Strength=NaN repairs to 1.0, got " + nanSettings.DLSS5Strength.Value);
            nanSettings.DLSS5Detail.Value = float.PositiveInfinity;
            Check(Math.Abs(nanSettings.DLSS5Detail.Value - 1f) < 0.0001f,
                "an infinite detail repairs to 1.0, got " + nanSettings.DLSS5Detail.Value);

            var shaders = Path.Combine(directory, "shaders.cfg");
            WriteConfig(shaders, dlss5Shaders: "  a , , b  ");
            var shaderSettings = new ModSettings();
            shaderSettings.Bind(new ConfigFile(shaders, false, PluginInfo), messages.Add);
            Check(shaderSettings.DLSS5CharacterMaskShaders.Value == "a, b",
                "spaces and empty entries are trimmed, got '" +
                shaderSettings.DLSS5CharacterMaskShaders.Value + "'");
            Check(File.ReadAllText(shaders).Contains("CharacterMaskShaders = a, b"),
                "the trimmed shader list is persisted");

            var blank = Path.Combine(directory, "blank.cfg");
            WriteConfig(blank, dlss5Shaders: "   ");
            var blankSettings = new ModSettings();
            blankSettings.Bind(new ConfigFile(blank, false, PluginInfo), messages.Add);
            Check((blankSettings.DLSS5CharacterMaskShaders.Value ?? string.Empty).Length == 0,
                "a whitespace-only shader list becomes empty");
        }
        finally
        {
            Cleanup(directory);
        }
    }

    /// <summary>
    /// A panel or key press writes immediately. The generated value block must not come back, and the
    /// change must still reach a subscriber exactly once.
    /// </summary>
    private static void CheckNoiseAfterPanelChange()
    {
        var directory = NewDirectory();
        try
        {
            var path = Path.Combine(directory, "raharu.bigscale.cfg");
            var config = new ConfigFile(path, false, PluginInfo);
            var messages = new List<string>();
            var settings = new ModSettings();
            settings.Bind(config, messages.Add);

            var changes = 0;
            settings.Changed += () => changes++;

            settings.Upscaler.Value = UpscalerKind.Fsr2;
            Check(settings.Upscaler.Value == UpscalerKind.Fsr2, "a panel change is stored");
            Check(changes == 1, "a panel change raises exactly one change, got " + changes);
            var text = File.ReadAllText(path);
            Check(text.Contains("Upscaler = Fsr2"), "a panel change is persisted immediately");
            CheckFileShape(text, "file after a panel change");

            settings.StageProbe.Value = true;
            Check(settings.StageProbe.Value, "a diagnostics toggle is stored");
            CheckFileShape(File.ReadAllText(path), "file after a diagnostics change");
            CheckDiagnosticsLast(File.ReadAllText(path), "file after a diagnostics change");
        }
        finally
        {
            Cleanup(directory);
        }
    }

    /// <summary>
    /// <c>Diagnostics/NativeLog</c> is a plain persisted flag: off by default, read back from the file,
    /// and never repaired into something else.
    /// </summary>
    private static void CheckNativeLog()
    {
        var directory = NewDirectory();
        try
        {
            var path = Path.Combine(directory, "raharu.bigscale.cfg");
            WriteConfig(path, nativeLog: "true");
            var messages = new List<string>();
            var settings = new ModSettings();
            settings.Bind(new ConfigFile(path, false, PluginInfo), messages.Add);
            Check(settings.NativeLog.Value, "Diagnostics/NativeLog reads a stored true");
            Check(File.ReadAllText(path).Contains("NativeLog = true", StringComparison.Ordinal),
                "NativeLog = true is persisted");
            Check(!settings.Dx12Validation.Value, "NativeLog does not disturb Dx12Validation");

            var rebound = new ModSettings();
            rebound.Bind(new ConfigFile(path, false, PluginInfo), messages.Add);
            Check(rebound.NativeLog.Value, "NativeLog = true survives save and rebind");
        }
        finally
        {
            Cleanup(directory);
        }
    }

    /// <summary>
    /// <see cref="SettingsFileText.Normalize"/> is the post-save pass that removes BepInEx's generated
    /// value lists and keeps the single <c>[Diagnostics]</c> block last. It must be idempotent and must
    /// never reorder a file that carries two Diagnostics blocks.
    /// </summary>
    private static void CheckSettingsFileNormalize()
    {
        var directory = NewDirectory();
        try
        {
            var path = Path.Combine(directory, "ordered.cfg");
            File.WriteAllLines(path, new[]
            {
                "[Diagnostics]",
                string.Empty,
                "## Development only. This entry comment must survive.",
                "StageProbe = false",
                "# Acceptable values: false, true",
                string.Empty,
                "[General]",
                "Enabled = true",
                string.Empty,
                "[Upscaling]",
                "Upscaler = Dlss",
                string.Empty,
            });

            Check(SettingsFileText.Normalize(path),
                "Normalize reports a rewrite for noise plus a misplaced Diagnostics block");
            var text = File.ReadAllText(path);
            Check(text.Contains("## Development only. This entry comment must survive.", StringComparison.Ordinal),
                "Normalize keeps the entry comment");
            Check(text.Contains("StageProbe = false", StringComparison.Ordinal),
                "Normalize keeps the entry value");
            Check(!text.Contains("# Acceptable values:", StringComparison.Ordinal),
                "Normalize removes the generated acceptable-value line");
            CheckDiagnosticsLast(text, "normalized file");

            Check(!SettingsFileText.Normalize(path),
                "Normalize is a no-op on an already clean and ordered file");
            Check(string.Equals(File.ReadAllText(path), text, StringComparison.Ordinal),
                "Normalize leaves an already clean and ordered file byte-identical");

            var duplicate = Path.Combine(directory, "duplicate.cfg");
            File.WriteAllLines(duplicate, new[]
            {
                "[Diagnostics]",
                "StageProbe = false",
                string.Empty,
                "[General]",
                "Enabled = true",
                string.Empty,
                "[Diagnostics]",
                "Dx12Validation = false",
                string.Empty,
                "[Upscaling]",
                "Upscaler = Dlss",
                string.Empty,
            });
            SettingsFileText.Normalize(duplicate);
            var duplicateText = File.ReadAllText(duplicate);
            Check(duplicateText.Split(new[] { "[Diagnostics]" }, StringSplitOptions.None).Length == 3,
                "a duplicated [Diagnostics] block is kept, not merged");
            Check(duplicateText.IndexOf("[Diagnostics]", StringComparison.Ordinal) <
                  duplicateText.IndexOf("[General]", StringComparison.Ordinal),
                "a duplicated [Diagnostics] block keeps its order");
        }
        finally
        {
            Cleanup(directory);
        }
    }

    private static void CheckReload()
    {
        var directory = NewDirectory();
        try
        {
            var path = Path.Combine(directory, "raharu.bigscale.cfg");
            WriteConfig(path, upscaler: "XeSs");

            var config = new ConfigFile(path, false, PluginInfo);
            var messages = new List<string>();
            var settings = new ModSettings();
            settings.Bind(config, messages.Add);
            Check(settings.Upscaler.Value == UpscalerKind.XeSs, "reload starts from the persisted XeSs");

            var changes = 0;
            settings.Changed += () => changes++;

            WriteConfig(path, upscaler: "Fsr2");
            config.Reload();
            Check(settings.Upscaler.Value == UpscalerKind.Fsr2, "reload applies a valid upscaler");
            Check(changes == 1, "a reloaded upscaler raises exactly one change, got " + changes);

            WriteConfig(path, upscaler: "NotABackend");
            config.Reload();
            Check(settings.Upscaler.Value == UpscalerKind.Fsr2,
                "an unparsable reload keeps the previous upscaler");
            Check(changes == 1, "an ignored reload raises no change, got " + changes);
        }
        finally
        {
            Cleanup(directory);
        }
    }
}
