using System;
using System.Collections.Generic;

namespace BigScale.Core;

/// <summary>
/// Decides which renderers draw a character and must therefore be excluded from neural-rendering
/// edits.
///
/// The decision is a shader-name match, not a layer or object-name match: the game's character bodies
/// use the dedicated <c>househouse/PlayerCharacter</c> and <c>househouse/PlayerNose</c> shaders, while
/// props, terrain and the environment use unrelated ones, and neither layers nor hierarchy paths are
/// stable across the game's streaming. A shader name is what the user can add to
/// <c>CharacterMaskShaders</c> without a code change when a character variant uses a shader this list
/// does not know.
///
/// Matching is an ordinal, case-insensitive substring so a variant shader such as
/// <c>househouse/PlayerCharacterAlt</c> is covered by the base name. That is intentionally broad: a
/// false positive protects a little extra geometry from the neural renderer, where a false negative
/// would let it paint over a character.
/// </summary>
public static class CharacterMaskPolicy
{
    /// <summary>
    /// The nose shader. On the local player it is first-person decoration that never writes colour, so
    /// it must not enter the mask there; remote noses are visible and stay masked.
    /// </summary>
    public const string LocalNoseShaderName = "househouse/PlayerNose";

    /// <summary>
    /// The shaders the game's local and remote player bodies use. Kept as a mutable array because it
    /// is part of the frozen interface; callers must treat it as read-only.
    /// </summary>
    public static readonly string[] DefaultShaderNames =
    {
        "househouse/PlayerCharacter",
        LocalNoseShaderName,
    };

    /// <summary>
    /// True when the camera draws this renderer: enabled, active, not force-hidden, not shadow-only,
    /// and its layer is in the camera's culling mask. <c>CommandBuffer.DrawRenderer</c> ignores the
    /// culling mask and renderer state, so the mask pass applies them itself.
    /// </summary>
    public static bool IsDrawnByCamera(int cullingMask, int layer, bool enabled, bool activeInHierarchy,
        bool forceRenderingOff, bool shadowsOnly)
        => enabled && activeInHierarchy && !forceRenderingOff && !shadowsOnly &&
           layer >= 0 && layer < 32 && (cullingMask & (1 << layer)) != 0;

    /// <summary>
    /// Index of the pass URP's opaque forward rendering draws for a material, given each pass's
    /// LightMode tag (null or empty = untagged, which URP draws as SRPDefaultUnlit) and whether the
    /// material has that pass enabled. An enabled UniversalForward or UniversalForwardOnly pass wins;
    /// otherwise the first enabled SRPDefaultUnlit/untagged pass; otherwise -1. Tags compare ordinal
    /// case-insensitively. Mismatched list lengths return -1.
    /// </summary>
    public static int SelectForwardPass(IReadOnlyList<string?> lightModes, IReadOnlyList<bool> enabled)
    {
        if (lightModes == null || enabled == null || lightModes.Count != enabled.Count) return -1;
        var unlit = -1;
        for (var i = 0; i < lightModes.Count; i++)
        {
            if (!enabled[i]) continue;
            var mode = lightModes[i];
            if (string.Equals(mode, "UniversalForward", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(mode, "UniversalForwardOnly", StringComparison.OrdinalIgnoreCase))
                return i;
            if (unlit < 0 && (string.IsNullOrEmpty(mode) ||
                              string.Equals(mode, "SRPDefaultUnlit", StringComparison.OrdinalIgnoreCase)))
                unlit = i;
        }
        return unlit;
    }

    /// <summary>
    /// True when <paramref name="name"/> contains one of <see cref="DefaultShaderNames"/>, or one of
    /// <paramref name="extras"/>, as an ordinal case-insensitive substring.
    /// A null or empty name matches nothing; null, empty and whitespace-only extras are skipped.
    /// </summary>
    public static bool IsCharacterShader(string? name, IReadOnlyList<string> extras)
    {
        if (string.IsNullOrEmpty(name)) return false;
        for (var i = 0; i < DefaultShaderNames.Length; i++)
        {
            if (name.IndexOf(DefaultShaderNames[i], StringComparison.OrdinalIgnoreCase) >= 0) return true;
        }
        if (extras == null) return false;
        for (var i = 0; i < extras.Count; i++)
        {
            var candidate = extras[i];
            if (string.IsNullOrWhiteSpace(candidate)) continue;
            if (name.IndexOf(candidate, StringComparison.OrdinalIgnoreCase) >= 0) return true;
        }
        return false;
    }
}
