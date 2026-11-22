namespace BigScale.Core;

/// <summary>
/// Read-only status row text for Neural Rendering. This is pure policy: it never queries a backend,
/// so the panel can render the last reported native state without blocking on a frame.
/// </summary>
public static class NeuralRenderingStatusText
{
    /// <summary>
    /// Maps the gate, master switch, upscaler selection and last native state onto the exact caption
    /// the panel shows. The gate and master switch win over every native state, and an Off upscaler
    /// wins over an unknown or stale state; anything the native side has not proven yet reads
    /// <c>Waiting</c> rather than claiming success.
    /// </summary>
    public static string For(bool gate, bool master, UpscalerKind upscaler, int nativeState)
    {
        if (!gate || !master) return "Off";
        if (upscaler == UpscalerKind.Off) return "Select an Upscaler";
        return nativeState switch
        {
            NeuralRenderingState.Active => "Active",
            NeuralRenderingState.RuntimeMissing => "Runtime missing",
            NeuralRenderingState.Failed => "Failed",
            NeuralRenderingState.Unsupported => "Unsupported (desktop only)",
            _ => "Waiting",
        };
    }
}
