using System;
using System.Collections.Generic;
using System.IO;

namespace BigScale.Core;

/// <summary>
/// Post-save text cleanup for this plugin's own configuration file.
/// <para>
/// BepInEx appends a generated list of every accepted enum value to each entry it writes. The file is
/// already self-documenting without it, so those lines are removed after each save. Only the two exact
/// generated prefixes are dropped; <c># Setting type:</c> and <c># Default value:</c> stay, and no
/// other file is ever read.
/// </para>
/// </summary>
public static class SettingsFileText
{
    private const string AcceptableValuesPrefix = "# Acceptable values:";
    private const string MultipleValuesPrefix = "# Multiple values can be set";

    /// <summary>
    /// Rewrites <paramref name="path"/> without the generated value lists. The file is only written
    /// when a line was actually removed, so an unchanged save leaves it untouched.
    /// </summary>
    /// <returns>The number of removed lines.</returns>
    public static int StripGeneratedNoise(string path)
    {
        if (string.IsNullOrEmpty(path) || !File.Exists(path)) return 0;
        var lines = File.ReadAllLines(path);
        var kept = new List<string>(lines.Length);
        var removed = 0;
        foreach (var line in lines)
        {
            var trimmed = line.Trim();
            if (trimmed.StartsWith(AcceptableValuesPrefix, StringComparison.Ordinal) ||
                trimmed.StartsWith(MultipleValuesPrefix, StringComparison.Ordinal))
            {
                removed++;
                continue;
            }
            kept.Add(line);
        }
        if (removed == 0) return 0;
        File.WriteAllLines(path, kept);
        return removed;
    }
}
