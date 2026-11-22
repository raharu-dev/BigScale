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
/// <para>
/// BepInEx also writes sections in alphabetical order, which puts the development-only
/// <c>[Diagnostics]</c> block first. It is kept as the last section instead, so the file opens on the
/// settings a player actually changes.
/// </para>
/// </summary>
public static class SettingsFileText
{
    private const string AcceptableValuesPrefix = "# Acceptable values:";
    private const string MultipleValuesPrefix = "# Multiple values can be set";
    private const string DiagnosticsHeader = "[Diagnostics]";

    /// <summary>
    /// Rewrites <paramref name="path"/> without the generated value lists and with the single
    /// <c>[Diagnostics]</c> block last. The file is only written when a line was actually removed or
    /// that block actually moved, so an already clean and ordered file is left untouched.
    /// </summary>
    /// <returns><c>true</c> when the file was rewritten.</returns>
    public static bool Normalize(string path)
    {
        if (string.IsNullOrEmpty(path) || !File.Exists(path)) return false;
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

        // Preamble first (a banner comment before any section), then one block per section header.
        var preamble = new List<string>();
        var blocks = new List<List<string>>();
        foreach (var line in kept)
        {
            var trimmed = line.Trim();
            var isHeader = trimmed.Length >= 2 && trimmed[0] == '[' && trimmed[trimmed.Length - 1] == ']';
            if (!isHeader)
            {
                if (blocks.Count == 0) preamble.Add(line);
                else blocks[blocks.Count - 1].Add(line);
                continue;
            }
            blocks.Add(new List<string> { line });
        }

        // Exactly one Diagnostics block moves last. Zero leaves nothing to move; two would move the
        // first behind the second and swap order on every save, so a duplicate file keeps its order.
        var diagnosticsIndex = -1;
        var diagnosticsCount = 0;
        for (var index = 0; index < blocks.Count; index++)
        {
            if (!string.Equals(blocks[index][0].Trim(), DiagnosticsHeader, StringComparison.Ordinal)) continue;
            diagnosticsCount++;
            if (diagnosticsCount == 1) diagnosticsIndex = index;
        }
        var moved = diagnosticsCount == 1 && diagnosticsIndex != blocks.Count - 1;
        if (moved)
        {
            var diagnostics = blocks[diagnosticsIndex];
            blocks.RemoveAt(diagnosticsIndex);
            blocks.Add(diagnostics);
        }

        if (removed == 0 && !moved) return false;

        var output = new List<string>(kept.Count);
        AppendBlock(output, preamble);
        foreach (var block in blocks) AppendBlock(output, block);
        File.WriteAllLines(path, output);
        return true;
    }

    /// <summary>
    /// Appends one block with its outer blank lines removed and exactly one blank line before it, so
    /// the file keeps a single separator no matter how the caller or BepInEx spaced the original.
    /// </summary>
    private static void AppendBlock(List<string> output, List<string> block)
    {
        var start = 0;
        var end = block.Count;
        while (start < end && block[start].Trim().Length == 0) start++;
        while (end > start && block[end - 1].Trim().Length == 0) end--;
        if (start == end) return;
        if (output.Count != 0) output.Add(string.Empty);
        for (var index = start; index < end; index++) output.Add(block[index]);
    }
}
