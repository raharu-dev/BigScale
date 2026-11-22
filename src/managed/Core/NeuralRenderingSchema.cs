namespace BigScale.Core;

/// <summary>Control family of one Neural Rendering row.</summary>
public enum NeuralRenderingRowKind
{
    Toggle,
    Choice,
    Slider,
    Integer,
    Status,
}

/// <summary>One row of an injected Neural Rendering settings page.</summary>
public readonly struct NeuralRenderingRow
{
    public NeuralRenderingRow(string key, string caption, NeuralRenderingRowKind kind,
        float min = 0f, float max = 0f)
    {
        Key = key;
        Caption = caption;
        Kind = kind;
        Min = min;
        Max = max;
    }

    /// <summary>Stable clone name, never shown to the player.</summary>
    public string Key { get; }

    /// <summary>Literal caption written into the row's localisation-stripped label.</summary>
    public string Caption { get; }

    /// <summary>Control family the panel must build for this row.</summary>
    public NeuralRenderingRowKind Kind { get; }

    /// <summary>Slider lower bound, or zero for every other kind.</summary>
    public float Min { get; }

    /// <summary>Slider upper bound, or zero for every other kind.</summary>
    public float Max { get; }
}

/// <summary>
/// One injected Neural Rendering settings page: its tab caption, its stable object names and the
/// rows it shows in a single column, top to bottom.
/// </summary>
public readonly struct NeuralRenderingPage
{
    public NeuralRenderingPage(string label, string settingsObjectName, string categoryObjectName,
        NeuralRenderingRow[] rows)
    {
        Label = label;
        SettingsObjectName = settingsObjectName;
        CategoryObjectName = categoryObjectName;
        Rows = rows;
    }

    /// <summary>Category caption, exactly as shown after the stock categories.</summary>
    public string Label { get; }

    /// <summary>Object name of the rebuilt page; the existing Upscaling page keeps <c>BigScale.Settings</c>.</summary>
    public string SettingsObjectName { get; }

    /// <summary>Object name of the injected category tab.</summary>
    public string CategoryObjectName { get; }

    /// <summary>The page's rows in display order.</summary>
    public NeuralRenderingRow[] Rows { get; }
}

/// <summary>
/// The complete user-facing schema of the injected Neural Rendering categories, in display order.
/// This is the single source of truth for which pages and rows exist: the panel creates exactly
/// these and nothing else.
/// </summary>
public static class NeuralRenderingSchema
{
    /// <summary>First page caption, exactly as shown after the stock categories.</summary>
    public const string CategoryLabel = "Neural Rendering";

    /// <summary>Second page caption, exactly as shown after <see cref="CategoryLabel"/>.</summary>
    public const string TuningLabel = "Neural Tuning";

    public const string EnabledKey = "BigScale.Neural.Enabled";
    public const string PresetKey = "BigScale.Neural.Preset";
    public const string TimingKey = "BigScale.Neural.Timing";
    public const string StrengthKey = "BigScale.Neural.Strength";
    public const string DetailKey = "BigScale.Neural.Detail";
    public const string ToneKey = "BigScale.Neural.Tone";
    public const string PassesKey = "BigScale.Neural.Passes";
    public const string CharacterMaskKey = "BigScale.Neural.CharacterMask";
    public const string StatusKey = "BigScale.Neural.Status";

    public static readonly NeuralRenderingPage[] Pages =
    {
        new(CategoryLabel, "BigScale.NeuralSettings", "BigScale.NeuralCategory", new[]
        {
            new NeuralRenderingRow(EnabledKey, "Enabled", NeuralRenderingRowKind.Toggle),
            new NeuralRenderingRow(PresetKey, "Preset", NeuralRenderingRowKind.Choice),
            new NeuralRenderingRow(TimingKey, "Timing", NeuralRenderingRowKind.Choice),
            new NeuralRenderingRow(PassesKey, "Passes", NeuralRenderingRowKind.Integer),
            new NeuralRenderingRow(StatusKey, "Status", NeuralRenderingRowKind.Status),
        }),
        new(TuningLabel, "BigScale.NeuralTuningSettings", "BigScale.NeuralTuningCategory", new[]
        {
            new NeuralRenderingRow(StrengthKey, "Strength", NeuralRenderingRowKind.Slider, 0f, 2f),
            new NeuralRenderingRow(DetailKey, "Detail", NeuralRenderingRowKind.Slider, 0f, 2f),
            new NeuralRenderingRow(ToneKey, "Tone", NeuralRenderingRowKind.Slider, 0f, 2f),
            new NeuralRenderingRow(CharacterMaskKey, "Character Masking", NeuralRenderingRowKind.Toggle),
        }),
    };
}
