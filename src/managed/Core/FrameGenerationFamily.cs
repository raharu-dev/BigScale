namespace BigScale.Core;

/// <summary>
/// Process-lifetime frame-generation family compatibility, mirroring the native transport's rule.
/// A normal-family process may initialize ANR in place; that migration is one-way because ANR keeps
/// its D3D12 hooks installed, so every other provider (including Streamline) then needs a restart.
/// An unclaimed process may claim any family, and Off is always allowed.
/// </summary>
public static class FrameGenerationFamily
{
    public static bool RestartRequired(uint processFamily, FrameGenerationKind requested)
    {
        if (requested == FrameGenerationKind.Off || processFamily == NativeFg.FamilyUnused) return false;
        if (requested == FrameGenerationKind.Anr && processFamily == NativeFg.FamilyNormal) return false;
        var requestedFamily = requested switch
        {
            FrameGenerationKind.Anr => NativeFg.FamilyAnr,
            FrameGenerationKind.Streamline => NativeFg.FamilyStreamline,
            _ => NativeFg.FamilyNormal,
        };
        return processFamily != requestedFamily;
    }
}
