namespace BigScale.Core;

/// <summary>
/// Maps a render layout to the logical view a packet belongs to. Array slices and multipass views
/// are distinct identities: an array render produces one packet per slice, a multipass render
/// produces one packet per pass, and a plain 2D camera produces a single view zero.
/// </summary>
public static class ViewIdentity
{
    /// <summary>Mirrors <c>BIGSCALE_LAYOUT_2D</c>.</summary>
    public const int Layout2D = 0;

    /// <summary>Mirrors <c>BIGSCALE_LAYOUT_ARRAY</c>.</summary>
    public const int LayoutArray = 1;

    /// <summary>Mirrors <c>BIGSCALE_LAYOUT_PACKED</c>, which is how a multipass session reaches URP.</summary>
    public const int LayoutPacked = 2;

    /// <summary>Number of logical eyes a frame of this layout must report before it is complete.</summary>
    public static int ExpectedEyes(int layout) => layout == Layout2D ? 1 : 2;

    /// <summary>Logical eye for one packet.</summary>
    public static int EyeForPacket(int layout, int slice, int multipassId) => layout switch
    {
        LayoutArray => slice,
        LayoutPacked => multipassId < 0 ? 0 : multipassId,
        _ => 0,
    };

    /// <summary>Array slice one packet reads and writes. Multipass and 2D always use slice zero.</summary>
    public static int SliceForPacket(int layout, int slice) => layout == LayoutArray ? slice : 0;

    /// <summary>True when this layout renders both eyes inside a single camera render.</summary>
    public static bool IsSinglePassStereo(int layout) => layout == LayoutArray;
}
