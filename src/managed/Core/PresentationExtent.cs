using System;

namespace BigScale.Core;

/// <summary>
/// The validated presentation extents for one camera frame. <see cref="ActiveWidth"/> and
/// <see cref="ActiveHeight"/> are the real rendered viewport, taken before any post-processing
/// descriptor update. The motion-vector pixel basis is always that active render extent: the
/// half-resolution raw field is resampled into render-sized scratch before any provider sees it.
/// </summary>
public readonly struct PresentationBounds
{
    public PresentationBounds(int activeWidth, int activeHeight)
    {
        ActiveWidth = activeWidth;
        ActiveHeight = activeHeight;
    }

    /// <summary>Width of the active rendered viewport in pixels.</summary>
    public int ActiveWidth { get; }

    /// <summary>Height of the active rendered viewport in pixels.</summary>
    public int ActiveHeight { get; }
}

/// <summary>
/// Pure presentation contract shared by the managed renderer and its tests. A physical texture
/// allocation is evidence about memory, not about the resolution the camera actually rendered:
/// pooled allocations may be larger than the active viewport, and a larger allocation must never
/// silently redefine the input extent.
///
/// The contract only accepts an active zero-origin region that the colour/depth allocations really
/// contain, requires a motion texture that is either the active extent or exactly half of it, and
/// rejects an active viewport that no longer matches the resolved BigScale fit. Rejecting is safer than
/// cropping an arbitrary fit-sized region out of a texture that no longer has that extent.
/// </summary>
public static class PresentationExtent
{
    /// <summary>
    /// Validates one frame and resolves the active input extent. <paramref name="reason"/> explains
    /// the first broken rule; it is empty on success.
    /// </summary>
    public static bool TryResolve(int activeWidth, int activeHeight, int outputWidth, int outputHeight,
        int colorWidth, int colorHeight, int depthWidth, int depthHeight, int motionWidth, int motionHeight,
        int slices, int layout, InputSize[] eyes, out PresentationBounds bounds, out string reason)
    {
        bounds = default;
        reason = "";
        if (activeWidth <= 0 || activeHeight <= 0)
        {
            reason = $"the camera target descriptor reports no active extent ({activeWidth}x{activeHeight})";
            return false;
        }
        if (outputWidth <= 0 || outputHeight <= 0)
        {
            reason = $"the camera reports no output extent ({outputWidth}x{outputHeight})";
            return false;
        }
        if (eyes == null || eyes.Length == 0)
        {
            reason = "the resolved fit has no eyes";
            return false;
        }
        if (slices <= 0)
        {
            reason = $"the colour texture has no slices ({slices})";
            return false;
        }

        if (layout == ViewIdentity.LayoutPacked)
        {
            // Packed layouts place more than one eye inside one physical texture; each eye only has
            // to fit, because the per-eye atlas origin is owned by the layout.
            foreach (var eye in eyes)
            {
                if (eye.Width <= 0 || eye.Height <= 0)
                {
                    reason = $"the packed layout has an empty eye ({eye.Width}x{eye.Height})";
                    return false;
                }
                if (!Contains(colorWidth, colorHeight, eye.Width, eye.Height))
                {
                    reason = $"colour allocation {colorWidth}x{colorHeight} does not contain the {eye} eye";
                    return false;
                }
                if (!Contains(depthWidth, depthHeight, eye.Width, eye.Height))
                {
                    reason = $"depth allocation {depthWidth}x{depthHeight} does not contain the {eye} eye";
                    return false;
                }
                if (!SameAspect(eye.Width, eye.Height, outputWidth, outputHeight))
                {
                    reason = $"the {eye} eye and the {outputWidth}x{outputHeight} output have different aspects";
                    return false;
                }
            }
        }
        else
        {
            // A 2D camera reads its single active viewport; an array camera reads one slice per eye.
            if (eyes.Length > slices)
            {
                reason = $"layout {layout} needs {eyes.Length} slices but the colour texture has {slices}";
                return false;
            }
            foreach (var eye in eyes)
            {
                // The fit is derived from the SDK's reported optimum, and URP truncates the scaled
                // viewport, so the two can legitimately differ by a single pixel on each axis. The
                // active viewport stays authoritative: it is what the camera really rendered.
                if (Math.Abs(eye.Width - activeWidth) > 1 || Math.Abs(eye.Height - activeHeight) > 1)
                {
                    reason = $"fit eye {eye} does not match the active {activeWidth}x{activeHeight} viewport";
                    return false;
                }
            }
            if (!Contains(colorWidth, colorHeight, activeWidth, activeHeight))
            {
                reason = $"colour allocation {colorWidth}x{colorHeight} does not contain the active " +
                         $"{activeWidth}x{activeHeight} viewport";
                return false;
            }
            if (!Contains(depthWidth, depthHeight, activeWidth, activeHeight))
            {
                reason = $"depth allocation {depthWidth}x{depthHeight} does not contain the active " +
                         $"{activeWidth}x{activeHeight} viewport";
                return false;
            }
            if (!SameAspect(activeWidth, activeHeight, outputWidth, outputHeight))
            {
                reason = $"the active {activeWidth}x{activeHeight} viewport and the " +
                         $"{outputWidth}x{outputHeight} output have different aspects";
                return false;
            }
        }

        // The motion texture's pixel basis is the active render extent, whatever the stored values
        // mean (TemporalInputPolicy owns the per-provider conversion). A motion texture allocated
        // larger than that is a pooled allocation, not a resolution change.
        // Only an exactly half-resolution allocation changes the basis, and an allocation smaller
        // than the active extent without being exactly half is a wrong scale and is rejected.
        if (motionWidth > 0 && motionHeight > 0 && motionWidth * 2 == activeWidth &&
            motionHeight * 2 == activeHeight)
        {
            bounds = new PresentationBounds(activeWidth, activeHeight);
            return true;
        }
        if (motionWidth >= activeWidth && motionHeight >= activeHeight)
        {
            bounds = new PresentationBounds(activeWidth, activeHeight);
            return true;
        }
        reason = $"motion vectors {motionWidth}x{motionHeight} are smaller than the active " +
                 $"{activeWidth}x{activeHeight} extent and are not exactly half of it";
        return false;
    }

    /// <summary>
    /// True when a post-processing attachment is smaller than the full output in either axis, so
    /// publishing the reconstruction into it would force URP to rescale the image a second time.
    /// Invalid extents are reported as not reduced: the caller has no evidence to act on.
    /// </summary>
    public static bool IsReducedTarget(int targetWidth, int targetHeight, int outputWidth, int outputHeight) =>
        targetWidth > 0 && targetHeight > 0 && outputWidth > 0 && outputHeight > 0 &&
        (targetWidth < outputWidth || targetHeight < outputHeight);

    /// <summary>True when a valid attachment already has the full output extent.</summary>
    public static bool IsFullTarget(int targetWidth, int targetHeight, int outputWidth, int outputHeight) =>
        targetWidth > 0 && targetHeight > 0 && targetWidth == outputWidth && targetHeight == outputHeight;

    private static bool Contains(int allocationWidth, int allocationHeight, int width, int height) =>
        width > 0 && height > 0 && allocationWidth >= width && allocationHeight >= height;

    private static bool SameAspect(int width, int height, int outputWidth, int outputHeight)
    {
        var left = (double)width / outputWidth;
        var right = (double)height / outputHeight;
        // Allow sub-pixel rounding: the two ratios must agree within half a percent.
        return Math.Abs(left - right) <= 0.005 * Math.Max(left, right);
    }
}
