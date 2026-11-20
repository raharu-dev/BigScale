using System;

namespace BigScale.Core;

/// <summary>One quality mode's SDK-reported supported input range at a fixed output size.</summary>
public readonly struct ModeRange
{
    public ModeRange(bool supported, int optimalWidth, int optimalHeight, int minWidth, int minHeight, int maxWidth, int maxHeight)
    {
        Supported = supported && optimalWidth > 0 && optimalHeight > 0 && maxWidth > 0 && maxHeight > 0;
        OptimalWidth = optimalWidth;
        OptimalHeight = optimalHeight;
        MinWidth = supported && minWidth > 0 ? minWidth : optimalWidth;
        MinHeight = supported && minHeight > 0 ? minHeight : optimalHeight;
        MaxWidth = maxWidth;
        MaxHeight = maxHeight;
    }

    public bool Supported { get; }
    public int OptimalWidth { get; }
    public int OptimalHeight { get; }
    public int MinWidth { get; }
    public int MinHeight { get; }
    public int MaxWidth { get; }
    public int MaxHeight { get; }
}

/// <summary>Integer render size for one logical eye.</summary>
public readonly struct InputSize
{
    public InputSize(int width, int height)
    {
        Width = width;
        Height = height;
    }

    public int Width { get; }
    public int Height { get; }

    public override string ToString() => Width + "x" + Height;
}

/// <summary>Outcome of resolving a requested preset against the SDK's supported ranges.</summary>
public sealed class SizeFit
{
    public bool Ok { get; init; }
    public UpscalingPreset Preset { get; init; }
    public float EffectiveScale { get; init; }
    public bool Clamped { get; init; }
    public InputSize[] Eyes { get; init; } = Array.Empty<InputSize>();
    public string Reason { get; init; } = "";
}

/// <summary>
/// Pure sizing logic: turns a requested preset into one shared effective scalar and one integer
/// input size per eye. Bounds always come from the queried SDK ranges, so an unsupported dimension
/// can never be requested. There is deliberately no custom scale: presets are the only control.
/// </summary>
public static class UpscalingSizing
{
    /// <summary>Mode-info array order, which matches the native BIGSCALE_ModeInfo order.</summary>
    public const int ModeInfoCount = 5;

    /// <summary>
    /// Resolves a preset. <paramref name="rangesPerEye"/> holds one <see cref="ModeInfoCount"/>-long
    /// array per logical eye, in native mode-info order.
    /// </summary>
    public static SizeFit Fit(UpscalingPreset preset, int[] outputWidths, int[] outputHeights,
        ModeRange[][] rangesPerEye)
    {
        if (outputWidths == null || outputHeights == null || rangesPerEye == null ||
            outputWidths.Length == 0 || outputWidths.Length != outputHeights.Length ||
            outputWidths.Length != rangesPerEye.Length)
            return Fail(preset, "view count mismatch");

        for (int eye = 0; eye < outputWidths.Length; eye++)
        {
            if (outputWidths[eye] <= 0 || outputHeights[eye] <= 0) return Fail(preset, "invalid output size");
            if (rangesPerEye[eye] == null || rangesPerEye[eye].Length < ModeInfoCount)
                return Fail(preset, "missing SDK ranges");
        }

        var index = UpscalingModes.ModeInfoIndex(preset);
        var interval = CommonInterval(index, outputWidths, outputHeights, rangesPerEye, out var supported);
        if (!supported) return Fail(preset, "preset unsupported at this output size");
        if (interval.Low > interval.High) return Fail(preset, "no common scale for every eye");

        var target = OptimalScale(index, outputWidths, outputHeights, rangesPerEye);
        if (float.IsNaN(target)) return Fail(preset, "preset reports no optimal size");
        var effective = Math.Clamp(target, interval.Low, interval.High);
        var eyes = BuildSizes(effective, outputWidths, outputHeights, index, rangesPerEye);
        if (eyes == null) return Fail(preset, "no supported input size for every eye");
        return new SizeFit
        {
            Ok = true,
            Preset = preset,
            EffectiveScale = effective,
            Clamped = Math.Abs(effective - target) > 1e-4f,
            Eyes = eyes,
            Reason = "",
        };
    }

    private static SizeFit Fail(UpscalingPreset preset, string reason) => new()
    {
        Ok = false,
        Preset = preset,
        EffectiveScale = 0f,
        Clamped = false,
        Eyes = Array.Empty<InputSize>(),
        Reason = reason,
    };

    private readonly struct Interval
    {
        public Interval(float low, float high)
        {
            Low = low;
            High = high;
        }

        public float Low { get; }
        public float High { get; }
    }

    private static Interval CommonInterval(int index, int[] widths, int[] heights, ModeRange[][] ranges,
        out bool supported)
    {
        supported = false;
        var low = 0f;
        var high = float.MaxValue;
        for (int eye = 0; eye < widths.Length; eye++)
        {
            var range = ranges[eye]![index];
            if (!range.Supported) return default;
            supported = true;
            var eyeLow = Math.Max(range.MinWidth / (float)widths[eye], range.MinHeight / (float)heights[eye]);
            var eyeHigh = Math.Min(range.MaxWidth / (float)widths[eye], range.MaxHeight / (float)heights[eye]);
            if (eyeLow > low) low = eyeLow;
            if (eyeHigh < high) high = eyeHigh;
        }
        return new Interval(low, high);
    }

    private static float OptimalScale(int index, int[] widths, int[] heights, ModeRange[][] ranges)
    {
        var total = 0f;
        for (int eye = 0; eye < widths.Length; eye++)
        {
            var range = ranges[eye]![index];
            if (!range.Supported) return float.NaN;
            total += (range.OptimalWidth / (float)widths[eye] + range.OptimalHeight / (float)heights[eye]) * 0.5f;
        }
        return total / widths.Length;
    }

    private static InputSize[]? BuildSizes(float scale, int[] widths, int[] heights, int index, ModeRange[][] ranges)
    {
        var sizes = new InputSize[widths.Length];
        for (int eye = 0; eye < widths.Length; eye++)
        {
            var range = ranges[eye]![index];
            if (!range.Supported) return null;
            // URP derives the camera's render viewport by truncating the scaled extent, so the fit must
            // truncate the identical float product: rounding produced a fit one pixel away from the
            // viewport URP really renders (2227x1253 against an active 2227x1252 at scale 0.58), and
            // the presentation contract then refused every frame.
            var width = (int)(widths[eye] * scale);
            var height = (int)(heights[eye] * scale);
            width = Math.Clamp(width, Math.Max(1, range.MinWidth), Math.Max(1, range.MaxWidth));
            height = Math.Clamp(height, Math.Max(1, range.MinHeight), Math.Max(1, range.MaxHeight));
            width = Math.Min(width, widths[eye]);
            height = Math.Min(height, heights[eye]);
            if (width <= 0 || height <= 0) return null;
            sizes[eye] = new InputSize(width, height);
        }
        return sizes;
    }

}
