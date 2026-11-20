namespace BigScale.Core;

/// <summary>One row of the injected Upscaling settings page.</summary>
public readonly struct SettingsPanelRow
{
    public SettingsPanelRow(string key, string caption)
    {
        Key = key;
        Caption = caption;
    }

    /// <summary>Stable clone name, never shown to the player.</summary>
    public string Key { get; }

    /// <summary>Literal caption written into the row's localisation-stripped label.</summary>
    public string Caption { get; }
}

/// <summary>
/// The complete user-facing schema of the Upscaling category, in display order. This is the single
/// source of truth for which rows exist: the panel creates exactly these and nothing else, so a
/// removed control cannot linger in the page or in the configuration.
/// </summary>
public static class SettingsPanelSchema
{
    public const string EnabledKey = "BigScale.Enabled";
    public const string UpscalerKey = "BigScale.Upscaler";
    public const string ModeKey = "BigScale.Mode";
    public const string FrameGenerationKey = "BigScale.FrameGeneration";
    public const string FrameMultiplierKey = "BigScale.FrameMultiplier";

    public static readonly SettingsPanelRow[] Rows =
    {
        new(EnabledKey, "Enabled"),
        new(UpscalerKey, "Upscaler"),
        new(ModeKey, "Mode"),
        new(FrameGenerationKey, "Frame Generation (Experimental)"),
        new(FrameMultiplierKey, "Frame Multiplier"),
    };
}
