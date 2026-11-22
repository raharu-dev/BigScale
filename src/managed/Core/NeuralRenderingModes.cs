using System;

namespace BigScale.Core;

/// <summary>
/// Neural Rendering look. The numeric values are the NGX style wire values: 0 is the reference
/// (shown to the player as <c>Default</c>), 1 and 2 are the two authored looks.
/// </summary>
public enum NeuralRenderingStyle
{
    /// <summary>Reference look. The wire value 0 is labelled <c>Default</c> in the panel.</summary>
    Default = 0,

    /// <summary>Natural look.</summary>
    Natural = 1,

    /// <summary>Cinematic look.</summary>
    Cinematic = 2,
}

/// <summary>
/// Where the Neural Rendering stage runs relative to the selected upscaler. Both timings stay before
/// frame generation; the native bridge owns that ordering.
/// </summary>
public enum NeuralRenderingTiming
{
    /// <summary>Neural Rendering produces the colour input the upscaler reconstructs from.</summary>
    BeforeUpscaling = 0,

    /// <summary>Neural Rendering refines the upscaler's output.</summary>
    AfterUpscaling = 1,
}

/// <summary>
/// Native neural-state mirror. The values are part of the status wire ABI and must never be
/// renumbered; the managed status text and the native stage both report with these values.
/// </summary>
public static class NeuralRenderingState
{
    /// <summary>The gate or master switch is off; no neural work is configured.</summary>
    public const int Off = 0;

    /// <summary>A configuration was accepted but no frame has evaluated yet.</summary>
    public const int Waiting = 1;

    /// <summary>The last evaluation succeeded.</summary>
    public const int Active = 2;

    /// <summary>The user-supplied runtime DLL was not found.</summary>
    public const int RuntimeMissing = 3;

    /// <summary>Runtime loading, feature creation or evaluation failed.</summary>
    public const int Failed = 4;

    /// <summary>The packet layout is outside the supported desktop 2D case.</summary>
    public const int Unsupported = 5;
}

/// <summary>
/// Static display order, labels and repair helpers for Neural Rendering. Nothing here allocates or
/// touches a backend; the panel and the configuration use the same values.
/// </summary>
public static class NeuralRenderingModes
{
    /// <summary>Sequential layers the panel can request. The native stage never accepts more.</summary>
    public const int MinPasses = 1;

    /// <summary>Sequential layers the panel can request. The native stage never accepts more.</summary>
    public const int MaxPasses = 5;

    /// <summary>Styles in display order.</summary>
    public static readonly NeuralRenderingStyle[] StyleOrder =
    {
        NeuralRenderingStyle.Default,
        NeuralRenderingStyle.Natural,
        NeuralRenderingStyle.Cinematic,
    };

    /// <summary>Timings in display order.</summary>
    public static readonly NeuralRenderingTiming[] TimingOrder =
    {
        NeuralRenderingTiming.BeforeUpscaling,
        NeuralRenderingTiming.AfterUpscaling,
    };

    /// <summary>Menu label for a style. Wire value 0 is presented as <c>Default</c>.</summary>
    public static string Name(NeuralRenderingStyle style) => style switch
    {
        NeuralRenderingStyle.Natural => "Natural",
        NeuralRenderingStyle.Cinematic => "Cinematic",
        _ => "Default",
    };

    /// <summary>Menu label for a timing.</summary>
    public static string Name(NeuralRenderingTiming timing) => timing switch
    {
        NeuralRenderingTiming.AfterUpscaling => "After Upscaling",
        _ => "Before Upscaling",
    };

    /// <summary>Repairs an undefined style wire value to the reference look.</summary>
    public static NeuralRenderingStyle StyleFromWire(int wire) =>
        Enum.IsDefined(typeof(NeuralRenderingStyle), wire)
            ? (NeuralRenderingStyle)wire
            : NeuralRenderingStyle.Default;

    /// <summary>Repairs an undefined timing wire value to the pre-upscale stage.</summary>
    public static NeuralRenderingTiming TimingFromWire(int wire) =>
        Enum.IsDefined(typeof(NeuralRenderingTiming), wire)
            ? (NeuralRenderingTiming)wire
            : NeuralRenderingTiming.BeforeUpscaling;

    /// <summary>Clamps a requested layer count into the range the native stage accepts.</summary>
    public static int ClampPasses(int passes) =>
        passes < MinPasses ? MinPasses : passes > MaxPasses ? MaxPasses : passes;

    /// <summary>
    /// Repairs a strength-like value: a non-finite value becomes <paramref name="fallback"/>, then the
    /// result is clamped into the 0..2 range the native stage accepts.
    /// </summary>
    public static float ClampUnit(float value, float fallback = 1f)
    {
        if (!float.IsFinite(value)) value = fallback;
        if (!float.IsFinite(value)) value = 1f;
        if (value < 0f) return 0f;
        return value > 2f ? 2f : value;
    }

    /// <summary>Next style in display order, sharing the repair rule for unknown input.</summary>
    public static NeuralRenderingStyle NextStyle(NeuralRenderingStyle current)
    {
        var order = StyleOrder;
        for (var i = 0; i < order.Length; i++)
        {
            if (order[i] != current) continue;
            return order[(i + 1) % order.Length];
        }
        return order[0];
    }

    /// <summary>Next timing in display order, sharing the repair rule for unknown input.</summary>
    public static NeuralRenderingTiming NextTiming(NeuralRenderingTiming current)
    {
        var order = TimingOrder;
        for (var i = 0; i < order.Length; i++)
        {
            if (order[i] != current) continue;
            return order[(i + 1) % order.Length];
        }
        return order[0];
    }
}
