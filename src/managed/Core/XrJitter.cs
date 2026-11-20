using System;

namespace BigScale.Core;

/// <summary>
/// Sub-pixel jitter for XR super resolution.
///
/// URP's own <c>m_JitterMatrix</c> never reaches the rasterised VR image: the Big Walk VR mod replaces
/// the stereo input matrices with its own per-eye clip transform, so every temporal sample would land
/// on the same pixel grid. This helper produces the one offset that is injected into the mod's eye
/// matrices, expressed in three consistent forms:
/// <list type="bullet">
/// <item>a Halton(2,3) sequence of pixel offsets over the render (input) raster, the basis every
/// provider's jitter is measured in;</item>
/// <item>normalised device coordinates, which is what a clip-matrix translation measures;</item>
/// <item>the same pixel offset the temporal packet carries, by way of the matrix translation.</item>
/// </list>
/// The phase count follows the sample density: a smaller input raster needs fewer distinct offsets to
/// converge, so 8 phases are used at native resolution and up to 64 at the largest reduction.
/// </summary>
/// <summary>Why one frame's packet cannot describe the raster it was built from.</summary>
public enum JitterFrameProblem
{
    /// <summary>Both eyes carry this frame's sample on the raster extent it was measured against.</summary>
    None,

    /// <summary>No sample reached the raster, so the packet's identity jitter is the truth.</summary>
    NotSampled,

    /// <summary>One eye received the sample and the other did not.</summary>
    HalfPair,

    /// <summary>The raster extent is not the extent the sample was converted against.</summary>
    ExtentMismatch,
}

public static class XrJitter
{
    private const int MinimumPhases = 8;
    private const int MaximumPhases = 64;

    /// <summary>
    /// Number of Halton phases for one input/output pair: <c>ceil(8 * (output / input)^2)</c>,
    /// clamped to 8..64. An unusable input extent falls back to the minimum.
    /// </summary>
    public static int PhaseCount(int inputWidth, int outputWidth)
    {
        if (inputWidth <= 0 || outputWidth <= 0) return MinimumPhases;
        var ratio = outputWidth / (double)inputWidth;
        var phases = (int)Math.Ceiling(8.0 * ratio * ratio);
        if (phases < MinimumPhases) return MinimumPhases;
        if (phases > MaximumPhases) return MaximumPhases;
        return phases;
    }

    /// <summary>
    /// Pixel offset in the input raster for one frame, biased to the sample centre: both axes lie in
    /// (-0.5, 0.5) so the accumulated samples stay inside one pixel.
    /// </summary>
    public static (float X, float Y) PixelOffset(int frameIndex, int phaseCount)
    {
        if (phaseCount <= 0) phaseCount = MinimumPhases;
        var phase = ((frameIndex % phaseCount) + phaseCount) % phaseCount;
        var index = phase + 1;
        return (Halton(index, 2) - 0.5f, Halton(index, 3) - 0.5f);
    }

    /// <summary>
    /// Converts a sample measured in render pixels into the clip-space translation a projection must be
    /// left-multiplied by. The sample is expressed with texture rows (right and down positive); a D3D
    /// viewport maps clip +Y to row 0, so the Y term flips exactly when the target's rows run with the
    /// projection's Y.
    /// </summary>
    public static (float X, float Y) ClipOffset(float pixelX, float pixelY, int inputWidth, int inputHeight,
        bool uvStartsAtTop)
    {
        if (inputWidth <= 0 || inputHeight <= 0) return (0f, 0f);
        var ndcY = 2f * pixelY / inputHeight;
        return (2f * pixelX / inputWidth, uvStartsAtTop ? -ndcY : ndcY);
    }

    /// <summary>
    /// Whether both eyes of one frame received the sample. A half-sampled pair must report identity and
    /// reset history instead of claiming a displacement one eye's raster never received.
    /// </summary>
    public static JitterFrameProblem PairProblem(bool eye0Applied, bool eye1Applied)
    {
        if (eye0Applied == eye1Applied) return eye0Applied ? JitterFrameProblem.None : JitterFrameProblem.NotSampled;
        return JitterFrameProblem.HalfPair;
    }

    /// <summary>
    /// Whether this eye's raster extent is the extent the sample was converted against. A different
    /// extent means the pixel displacement on the raster is not the pixel displacement in the packet.
    /// </summary>
    public static JitterFrameProblem ExtentProblem(int rasterWidth, int rasterHeight, int basisWidth,
        int basisHeight)
    {
        if (basisWidth <= 0 || basisHeight <= 0) return JitterFrameProblem.NotSampled;
        return rasterWidth == basisWidth && rasterHeight == basisHeight
            ? JitterFrameProblem.None
            : JitterFrameProblem.ExtentMismatch;
    }

    private static float Halton(int index, int radix)
    {
        var result = 0f;
        var fraction = 1f / radix;
        var remaining = index;
        while (remaining > 0)
        {
            result += remaining % radix * fraction;
            remaining /= radix;
            fraction /= radix;
        }
        return result;
    }
}
