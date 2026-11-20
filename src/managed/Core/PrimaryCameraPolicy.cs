namespace BigScale.Core;

/// <summary>
/// Pure primary-camera selection policy.
///
/// A candidate only becomes primary when it is a live, eligible rendered camera and either no
/// primary exists, the current primary has stopped rendering, or the candidate covers a strictly
/// larger area. Equal-area live cameras never oscillate, and the mere fact that a pre-camera
/// callback ran first is never evidence that a camera should own rendering.
/// </summary>
public static class PrimaryCameraPolicy
{
    /// <summary>
    /// True when <paramref name="candidateId"/> should replace the current primary.
    /// </summary>
    /// <param name="primaryId">Current primary instance id, or 0 when none is adopted.</param>
    /// <param name="primaryLive">The current Unity camera exists, is enabled and active.</param>
    /// <param name="primaryArea">Cached pixel area of the current primary.</param>
    /// <param name="primaryLastSeenFrame">Last frame the current primary rendered.</param>
    /// <param name="candidateId">Candidate instance id.</param>
    /// <param name="candidateArea">Candidate pixel area; non-positive is invalid.</param>
    /// <param name="frame">Current frame index.</param>
    public static bool ShouldPromote(int primaryId, bool primaryLive, long primaryArea,
        int primaryLastSeenFrame, int candidateId, long candidateArea, int frame)
    {
        if (candidateId == 0 || candidateArea <= 0) return false;
        if (candidateId == primaryId) return false;
        if (primaryId == 0) return true;
        if (!primaryLive) return true;
        if (frame - primaryLastSeenFrame > 1) return true;
        return candidateArea > primaryArea;
    }
}
