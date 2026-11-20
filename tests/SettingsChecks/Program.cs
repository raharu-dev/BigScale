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
        CheckSingleKeyRoundTrips();
        CheckNoneDisables();
        CheckUnparsableValues();
        CheckStreamlineNormalizes();
        CheckNoiseAfterPanelChange();
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
    private static void WriteConfig(string path, string menuKey, string upscaler = "XeSs",
        string mode = "Performance", bool enabled = false)
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
            "[UI]",
            string.Empty,
            "## Key that opens and closes the in-game Upscaling category. Use None to disable.",
            "MenuKey = " + menuKey,
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
        foreach (var legacy in new[] { "local.bigwalk.dlss", "BigWalkDLSS", "KeyboardShortcut", "BilinearComparison" })
            Check(!text.Contains(legacy, StringComparison.Ordinal), $"{label}: no '{legacy}' in the file");
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
            Check(settings.MenuKey.Value == KeyCode.Delete, "UI/MenuKey defaults to Delete");
            Check(!settings.Diagnostics.Value, "Diagnostics/Enabled defaults to false");
            Check(settings.FrameCaptureKey.Value == KeyCode.F9, "Diagnostics/FrameCaptureKey defaults to F9");
            Check(!settings.StageProbe.Value, "Diagnostics/StageProbe defaults to false");
            Check(!settings.XrProof.Value, "Diagnostics/XrProof defaults to false");
            Check(!settings.Dx12Validation.Value, "Diagnostics/Dx12Validation defaults to false");
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

    private static void CheckSingleKeyRoundTrips()
    {
        var directory = NewDirectory();
        try
        {
            var path = Path.Combine(directory, "raharu.bigscale.cfg");
            WriteConfig(path, "F8");

            var messages = new List<string>();
            var settings = new ModSettings();
            settings.Bind(new ConfigFile(path, false, PluginInfo), messages.Add);
            Check(settings.MenuKey.Value == KeyCode.F8, "an existing single key is honored");

            var rebound = new ModSettings();
            rebound.Bind(new ConfigFile(path, false, PluginInfo), messages.Add);
            Check(rebound.MenuKey.Value == KeyCode.F8, "F8 survives save and rebind");
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
            Check(settings.MenuKey.Value == KeyCode.None, "None disables the menu key");

            var rebound = new ModSettings();
            rebound.Bind(new ConfigFile(path, false, PluginInfo), messages.Add);
            Check(rebound.MenuKey.Value == KeyCode.None, "None survives save and rebind");
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
            Check(settings.MenuKey.Value == KeyCode.Delete,
                "a removed modifier combination leaves the Delete default");
            Check(!settings.Enabled.Value && settings.Upscaler.Value == UpscalerKind.XeSs &&
                  settings.Preset.Value == UpscalingPreset.Performance,
                "an unparsable key does not disturb the other selections");

            var unknown = Path.Combine(directory, "unknown.cfg");
            WriteConfig(unknown, "NotAKey");
            var unknownMessages = new List<string>();
            var unknownSettings = new ModSettings();
            unknownSettings.Bind(new ConfigFile(unknown, false, PluginInfo), unknownMessages.Add);
            Check(unknownSettings.MenuKey.Value == KeyCode.Delete,
                "an unknown key name leaves the Delete default");

            var badUpscaler = Path.Combine(directory, "bad-upscaler.cfg");
            WriteConfig(badUpscaler, "Delete", upscaler: "NotABackend");
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
            WriteConfig(path, "Delete", enabled: true);
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
            WriteConfig(path, "F8");

            var config = new ConfigFile(path, false, PluginInfo);
            var messages = new List<string>();
            var settings = new ModSettings();
            settings.Bind(config, messages.Add);
            Check(settings.MenuKey.Value == KeyCode.F8, "reload starts from the persisted F8");

            var changes = 0;
            settings.Changed += () => changes++;

            WriteConfig(path, "Delete");
            config.Reload();
            Check(settings.MenuKey.Value == KeyCode.Delete, "reload applies a valid key");
            Check(changes == 1, "a reloaded key raises exactly one change, got " + changes);

            WriteConfig(path, "NotAKey");
            config.Reload();
            Check(settings.MenuKey.Value == KeyCode.Delete, "an unparsable reload keeps the previous key");
            Check(changes == 1, "an ignored reload raises no change, got " + changes);
        }
        finally
        {
            Cleanup(directory);
        }
    }
}
