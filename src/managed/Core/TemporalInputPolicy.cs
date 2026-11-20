namespace BigScale.Core;

/// <summary>
/// Provider-boundary conversion of Unity's temporal inputs to the contract every upscaler in this
/// build consumes.
///
/// URP hands out one motion field and one jitter matrix for the whole session, and both are passed to
/// the selected upstream unchanged for that provider's own reasons:
/// <list type="bullet">
/// <item>the motion field is a one-unit-per-screen displacement in the previous-to-current direction,
/// stored in the motion-vector pass's own axis. Every backend that consumes it (FidelityFX on D3D11
/// and DX12, XeSS, and NGX) is given the same full-extent negation, so a full-screen move is one unit
/// on each axis and the direction becomes the current-to-previous motion all of them expect;</item>
/// <item><c>cameraData.m_JitterMatrix</c>'s translation already lives in the same coordinate and
/// direction system as that motion field, which is what the provider contracts require: DLSS states
/// that jitter must use "the same co-ordinate and direction system as motion vectors", and the AMD and
/// XeSS dispatches have always received the identical value. Converting it per provider (this build
/// used to negate Y for DLSS alone) is what produced the reported sub-pixel vertical shake and
/// combing at Ultra Performance; the shipped reference NGX integration for this engine passes
/// <c>m03 * width * 0.5f</c> and <c>m13 * height * 0.5f</c> straight through as well.</item>
/// </list>
/// The pixel basis is always the active render extent, never a pooled motion allocation.
/// </summary>
public static class TemporalInputPolicy
{
    /// <summary>Per-axis pixel scale for the packet, derived from the active render extent.</summary>
    public static (float X, float Y) MotionScale(int renderWidth, int renderHeight, bool uvStartsAtTop)
    {
        if (renderWidth <= 0 || renderHeight <= 0) return (0f, 0f);
        var y = uvStartsAtTop ? -renderHeight : renderHeight;
        return (-renderWidth, y);
    }

    /// <summary>
    /// Jitter for the packet, already expressed in render pixels. It is passed through unchanged: the
    /// matrix translation and the motion field share one axis system, so only the NDC-to-pixel factor
    /// is applied by the caller.
    /// </summary>
    public static (float X, float Y) Jitter(float clipPixelX, float clipPixelY) => (clipPixelX, clipPixelY);
}
