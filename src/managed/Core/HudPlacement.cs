using System;

namespace BigScale.Core;

/// <summary>Placement policy for the two hold-TAB rows, in one view space.</summary>
public static class HudPlacement
{
    /// <summary>Gaps below this many view units are measurement noise, not a visible margin.</summary>
    public const float MinimumMeasurableInset = 4f;

    /// <summary>Margin used when the stock HUD has no usable gap.</summary>
    public const float FallbackInset = 16f;

    /// <summary>
    /// Resolves the row margin from the stock counter's four edge gaps. Each gap is the distance
    /// from the measured stock bound to the corresponding view edge; negative or non-finite gaps
    /// are ignored. The smallest usable gap wins; gaps below MinimumMeasurableInset use the
    /// fallback. The result is clamped to <paramref name="maximum"/>.
    /// </summary>
    public static bool TryResolveInset(float left, float right, float bottom, float top,
        float fallback, float maximum, out float inset)
    {
        inset = 0f;
        if (!IsFinite(fallback) || fallback <= 0f) return false;
        if (!IsFinite(maximum) || maximum <= 0f) return false;

        var smallest = float.PositiveInfinity;
        if (IsUsable(left) && left < smallest) smallest = left;
        if (IsUsable(right) && right < smallest) smallest = right;
        if (IsUsable(bottom) && bottom < smallest) smallest = bottom;
        if (IsUsable(top) && top < smallest) smallest = top;

        if (!IsFinite(smallest) || smallest < MinimumMeasurableInset) smallest = fallback;
        inset = MathF.Min(smallest, maximum);
        return IsFinite(inset) && inset > 0f;
    }

    /// <summary>
    /// Resolves the rows' bottom-left rectangle. Glyphs land exactly on the inset because the rows
    /// use a zero TMP margin; the width keeps the inset on both sides. Invalid input returns false
    /// and leaves every output at zero.
    /// </summary>
    public static bool TryGetRows(float viewMinX, float viewMinY, float viewWidth, float viewHeight,
        float inset, int lineCount, float lineHeight,
        out float left, out float bottom, out float width, out float height)
    {
        left = 0f;
        bottom = 0f;
        width = 0f;
        height = 0f;
        if (!IsFinite(viewMinX) || !IsFinite(viewMinY) ||
            !IsFinite(viewWidth) || !IsFinite(viewHeight) || viewWidth <= 0f || viewHeight <= 0f)
            return false;
        if (!IsFinite(inset) || inset < 0f) return false;
        if (lineCount <= 0 || !IsFinite(lineHeight) || lineHeight <= 0f) return false;

        var rowLeft = viewMinX + inset;
        var rowBottom = viewMinY + inset;
        var rowWidth = MathF.Max(1f, viewWidth - inset * 2f);
        var rowHeight = MathF.Max(1f, lineHeight * lineCount);
        if (!IsFinite(rowLeft) || !IsFinite(rowBottom) ||
            !IsFinite(rowWidth) || !IsFinite(rowHeight))
            return false;

        left = rowLeft;
        bottom = rowBottom;
        width = rowWidth;
        height = rowHeight;
        return true;
    }

    private static bool IsUsable(float gap) => IsFinite(gap) && gap >= 0f;

    private static bool IsFinite(float value) => !float.IsNaN(value) && !float.IsInfinity(value);
}
