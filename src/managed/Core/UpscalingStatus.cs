using System.Text;

namespace BigScale.Core;

/// <summary>
/// Pure presentation of the two hold-TAB HUD rows. Every value comes from a retired frame or an
/// explicit pending state, so a row can never claim a resolution or a provider the renderer did not
/// produce. ASCII only: the game's stock overlay font is not guaranteed to carry the unicode arrows
/// and multiplication signs the old status card used.
/// </summary>
public static class UpscalingStatus
{
    // States used by the rows. Kept as strings so this helper stays free of renderer types.
    public const string StateOff = "Off";
    public const string StateWaiting = "Waiting";
    public const string StateActive = "Active";
    public const string StateUnsupported = "Unsupported";
    public const string StateFaulted = "Faulted";

    // Frame-generation states.
    public const string FgOff = "Off";
    public const string FgWaiting = "Waiting";
    public const string FgActive = "Active";
    public const string FgSuspended = "Suspended";
    public const string FgUnsupported = "Unsupported";
    public const string FgFaulted = "Faulted";
    public const string FgRestart = "Restart required";
    public const string FgVrDisabled = "Disabled in VR";

    /// <summary>
    /// True only when a retired frame belongs to the current provider and preset in its generation.
    /// A stale summary can never be shown as the active conversion.
    /// </summary>
    public static bool Matches(RetiredFrameSummary summary, UpscalerKind upscaler, UpscalingPreset preset) =>
        summary != null && summary.EyeCount > 0 && summary.Upscaler == upscaler && summary.Preset == preset;

    /// <summary>
    /// Upscaler row: the observed provider and the actual render-to-target extent. A fully retired
    /// frame prints what it produced; before that proof a fitted configuration is prefixed
    /// <c>Configured:</c> so it is never mistaken for active scaling. An Off or unfitted state says
    /// both sides are unavailable instead of inventing an extent.
    /// </summary>
    public static string UpscalerHud(UpscalerKind provider, RetiredFrameSummary? summary, int configuredWidth,
        int configuredHeight, int outputWidth, int outputHeight, int configuredEyes)
    {
        const string arrow = " -> ";
        var name = provider == UpscalerKind.Off ? "Off" : UpscalingModes.ShortName(provider);
        if (summary != null && summary.EyeCount > 0)
        {
            var left = summary.Eyes[0];
            if (summary.EyeCount == 1)
                return "UPSCALER: " + name + " - " + Convert(left, arrow);
            var right = summary.Eyes[1];
            var identical = left.InputWidth == right.InputWidth && left.InputHeight == right.InputHeight &&
                            left.OutputWidth == right.OutputWidth && left.OutputHeight == right.OutputHeight;
            if (identical) return "UPSCALER: " + name + " - " + Convert(left, arrow) + " (per eye)";
            return "UPSCALER: " + name + " - L:" + Size(left.InputWidth, left.InputHeight) +
                   "/R:" + Size(right.InputWidth, right.InputHeight) + arrow +
                   "L:" + Size(left.OutputWidth, left.OutputHeight) + "/R:" +
                   Size(right.OutputWidth, right.OutputHeight);
        }
        if (configuredWidth > 0 && configuredHeight > 0 && outputWidth > 0 && outputHeight > 0)
        {
            var pending = Size(configuredWidth, configuredHeight) + arrow + Size(outputWidth, outputHeight) +
                          (configuredEyes > 1 ? " (per eye)" : "");
            return "UPSCALER: " + name + " - Configured: " + pending;
        }
        return "UPSCALER: " + name + " - Unavailable" + arrow + "Unavailable";
    }

    private static string Convert(RetiredEye eye, string arrow) =>
        Size(eye.InputWidth, eye.InputHeight) + arrow + Size(eye.OutputWidth, eye.OutputHeight);

    private static string Size(int width, int height) => width + "x" + height;

    /// <summary>
    /// Frame-generation row: provider, total multiplier and state. The multiplier is the one the
    /// provider actually applied; Off always prints 1x because no extra frame exists. A non-empty
    /// reason is appended after <c>": "</c> with newlines flattened, so a multi-line SDK message
    /// cannot break the two-row HUD.
    /// </summary>
    public static string FrameGeneration(string state, FrameGenerationKind provider, int multiplier, string reason)
    {
        // Off has no provider and no extra frame: neither a remembered selection nor a stale
        // multiplier may be printed beside it.
        if (state == FgOff) provider = FrameGenerationKind.Off;
        var total = state == FgOff || multiplier < 1 ? 1 : multiplier;
        var builder = new StringBuilder(96);
        builder.Append("FG: ").Append(UpscalingModes.Name(provider));
        builder.Append(" - ").Append(total).Append("x - ").Append(state);
        if (!string.IsNullOrEmpty(reason))
        {
            builder.Append(": ");
            foreach (var character in reason)
                builder.Append(character is '\r' or '\n' ? ' ' : character);
        }
        return builder.ToString();
    }
}
