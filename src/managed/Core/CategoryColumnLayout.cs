using System;

namespace BigScale.Core;

/// <summary>
/// Vertical placement for the settings category column. A single count determines the pitch, so the
/// complete column fits between the stock column's outermost edges and every hit target keeps one
/// band; the original button height caps each row instead of stretching it.
/// </summary>
public static class CategoryColumnLayout
{
    /// <summary>
    /// Returns the parent-local centre and height for one slot. Invalid input produces no placement
    /// instead of a partially computed one, so a caller never mutates a rect from bad numbers.
    /// </summary>
    public static bool TryGetSlot(float top, float bottom, float maximumHeight, int count, int index,
        out float center, out float height)
    {
        center = 0f;
        height = 0f;
        if (!IsFinite(top) || !IsFinite(bottom) || !IsFinite(maximumHeight)) return false;
        if (top <= bottom || maximumHeight <= 0f) return false;
        if (count <= 0 || index < 0 || index >= count) return false;

        var pitch = (top - bottom) / count;
        if (!IsFinite(pitch) || pitch <= 0f) return false;

        var slotHeight = MathF.Min(maximumHeight, pitch);
        if (!IsFinite(slotHeight) || slotHeight <= 0f) return false;

        var slotCenter = top - pitch * (index + 0.5f);
        if (!IsFinite(slotCenter)) return false;

        center = slotCenter;
        height = slotHeight;
        return true;
    }

    /// <summary>
    /// Maps one common-space slot into the parent-local height and vertical delta of a tab whose own
    /// parent is scaled, rotated or pivoted off-centre relative to the column. Invalid input produces
    /// no placement instead of a partially computed one.
    /// </summary>
    public static bool TryMapSlot(float currentCenter, float localHeight, float pivotY, float verticalAxisY,
        float horizontalSpanY, float targetCenter, float targetHeight, out float mappedHeight,
        out float columnOffsetY)
    {
        mappedHeight = 0f;
        columnOffsetY = 0f;
        if (!IsFinite(currentCenter) || !IsFinite(localHeight) || !IsFinite(pivotY) ||
            !IsFinite(verticalAxisY) || !IsFinite(horizontalSpanY) || !IsFinite(targetCenter) ||
            !IsFinite(targetHeight)) return false;
        if (localHeight <= 0f || verticalAxisY == 0f || horizontalSpanY < 0f) return false;
        if (targetHeight <= horizontalSpanY) return false;

        var height = (targetHeight - horizontalSpanY) / MathF.Abs(verticalAxisY);
        var offset = targetCenter - currentCenter - (height - localHeight) * (0.5f - pivotY) * verticalAxisY;
        if (!IsFinite(height) || height <= 0f || !IsFinite(offset)) return false;

        mappedHeight = height;
        columnOffsetY = offset;
        return true;
    }

    private static bool IsFinite(float value) => !float.IsNaN(value) && !float.IsInfinity(value);
}
