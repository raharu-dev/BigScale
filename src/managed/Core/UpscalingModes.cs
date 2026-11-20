using System;

namespace BigScale.Core;

/// <summary>
/// Stable reconstruction backend identity. The numeric values are part of the native wire ABI and
/// must never be renumbered; both sides select and report the backend with these values.
/// </summary>
public enum UpscalerKind
{
    /// <summary>Stock renderer. No reconstruction backend is created and no scale is owned.</summary>
    Off = 0,

    /// <summary>NVIDIA DLSS Super Resolution through NGX on D3D11.</summary>
    Dlss = 1,

    /// <summary>AMD FidelityFX Super Resolution 2 on D3D11.</summary>
    Fsr2 = 2,

    /// <summary>AMD FidelityFX Super Resolution 3.1 on D3D11.</summary>
    Fsr3 = 3,

    /// <summary>Official AMD FidelityFX Super Resolution 4 through the DX12 interop bridge.</summary>
    Fsr4 = 4,

    /// <summary>Intel XeSS Super Resolution through the DX12 interop bridge.</summary>
    XeSs = 5,

    /// <summary>NVIDIA DLSS Super Resolution with NGX render preset L pinned (default for Ultra Performance).</summary>
    DlssPresetL = 6,
}

/// <summary>
/// User-facing quality preset shared by every provider. The wire values are provider neutral: each
/// native backend maps them onto its own SDK quality enumeration and validates the supported range.
/// </summary>
public enum UpscalingPreset
{
    Quality = 1,
    Balanced = 2,
    Performance = 3,
    UltraPerformance = 4,

    /// <summary>Native-resolution anti-aliasing: DLSS reports this as DLAA, other providers as Native AA.</summary>
    NativeAA = 5,
}

/// <summary>Optional desktop frame-generation provider. Disabled throughout a VR session.</summary>
public enum FrameGenerationKind
{
    Off = 0,
    Nvidia = 1,
    Fsr = 2,
    XeSs = 3,
    Anr = 4,
    /// <summary>
    /// Work in progress: DLSS-G through NVIDIA Streamline, the only path to multi-frame generation on
    /// Ada. The provider, its profiled patch layer and its ABI value are implemented and verified in
    /// the offline harness (`BigScaleChecks --fg streamline`), but the in-game lifecycle is not
    /// finished, so the panel never offers it and a stored value is held at Off.
    /// </summary>
    Streamline = 5,
}

/// <summary>Static display order and labels; nothing here allocates on a key press.</summary>
public static class UpscalingModes
{
    /// <summary>
    /// Every selectable upscaler, including unsupported ones so the reason stays visible. Off is
    /// deliberately absent: master enablement is the user-facing disable, and the selection is
    /// preserved across it.
    /// </summary>
    public static readonly UpscalerKind[] UpscalerOrder =
    {
        UpscalerKind.Dlss,
        UpscalerKind.DlssPresetL,
        UpscalerKind.Fsr2,
        UpscalerKind.Fsr3,
        UpscalerKind.Fsr4,
        UpscalerKind.XeSs,
    };

    /// <summary>Presets in display order. Availability is decided from the queried SDK ranges.</summary>
    public static readonly UpscalingPreset[] PresetOrder =
    {
        UpscalingPreset.Quality,
        UpscalingPreset.Balanced,
        UpscalingPreset.Performance,
        UpscalingPreset.UltraPerformance,
        UpscalingPreset.NativeAA,
    };

    /// <summary>Every frame-generation choice the menu offers. The WIP Streamline provider is
    /// deliberately absent until multi-frame generation is verified end to end in game.</summary>
    public static readonly FrameGenerationKind[] FrameGenerationOrder =
    {
        FrameGenerationKind.Off,
        FrameGenerationKind.Nvidia,
        FrameGenerationKind.Fsr,
        FrameGenerationKind.XeSs,
        FrameGenerationKind.Anr,
    };

    /// <summary>
    /// Resolves a stored selection to one the menu may request. A configuration written by the WIP
    /// Streamline build is held at Off instead of activating an unverified provider.
    /// </summary>
    public static FrameGenerationKind Selectable(FrameGenerationKind kind) =>
        kind == FrameGenerationKind.Streamline ? FrameGenerationKind.Off : kind;

    /// <summary>Menu label for an upscaler.</summary>
    public static string Name(UpscalerKind kind) => kind switch
    {
        UpscalerKind.Dlss => "NVIDIA DLSS",
        UpscalerKind.DlssPresetL => "NVIDIA DLSS (Preset L)",
        UpscalerKind.Fsr2 => "AMD FSR 2",
        UpscalerKind.Fsr3 => "AMD FSR 3",
        UpscalerKind.Fsr4 => "AMD FSR 4 (Experimental)",
        UpscalerKind.XeSs => "Intel XeSS",
        _ => "Off",
    };

    /// <summary>Menu label for a preset. Only DLSS calls the native-AA preset DLAA.</summary>
    public static string Name(UpscalingPreset preset, UpscalerKind upscaler) => preset switch
    {
        UpscalingPreset.Quality => "Quality",
        UpscalingPreset.Balanced => "Balanced",
        UpscalingPreset.Performance => "Performance",
        UpscalingPreset.UltraPerformance => "Ultra Performance",
        UpscalingPreset.NativeAA => upscaler is UpscalerKind.Dlss or UpscalerKind.DlssPresetL ? "DLAA" : "Native AA",
        _ => "Quality",
    };

    /// <summary>Menu label for a frame-generation provider.</summary>
    public static string Name(FrameGenerationKind kind) => kind switch
    {
        FrameGenerationKind.Nvidia => "NVIDIA",
        FrameGenerationKind.Fsr => "AMD FSR",
        FrameGenerationKind.XeSs => "Intel XeSS",
        FrameGenerationKind.Anr => "ANR",
        FrameGenerationKind.Streamline => "NVIDIA (Streamline)",
        _ => "Off",
    };

    /// <summary>Largest total the multiplier row may offer: the provider's proven maximum, or 2x before proof.</summary>
    public static int FrameMultiplierCeiling(FrameGenerationKind provider, int reportedMaximum) =>
        provider == FrameGenerationKind.Off ? 0 : reportedMaximum >= 2 ? Math.Min(reportedMaximum, 6) : 2;

    /// <summary>Short provider name used in the status headline.</summary>
    public static string ShortName(UpscalerKind kind) => kind switch
    {
        UpscalerKind.Dlss => "NVIDIA DLSS",
        UpscalerKind.DlssPresetL => "NVIDIA DLSS L",
        UpscalerKind.Fsr2 => "AMD FSR 2",
        UpscalerKind.Fsr3 => "AMD FSR 3",
        UpscalerKind.Fsr4 => "AMD FSR 4",
        UpscalerKind.XeSs => "Intel XeSS",
        _ => "Off",
    };

    /// <summary>Index of a preset in the native five-slot mode block.</summary>
    public static int ModeInfoIndex(UpscalingPreset preset) => preset switch
    {
        UpscalingPreset.Quality => 0,
        UpscalingPreset.Balanced => 1,
        UpscalingPreset.Performance => 2,
        UpscalingPreset.UltraPerformance => 3,
        UpscalingPreset.NativeAA => 4,
        _ => 0,
    };
}
