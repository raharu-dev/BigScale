using System;

namespace BigScale.Core;

/// <summary>
/// The VR mod's own explicit process switches. They are the only evidence that a VR session was
/// requested before the XR runtime reports a live device, and they take precedence over the runtime
/// because the user may have launched the VR session headlessly or without a headset attached.
/// </summary>
public static class VrMode
{
    public const string RequestSwitch = "-bigwalkvr-vr";
    public const string DenySwitch = "-bigwalkvr-non-vr";

    /// <summary>
    /// Scans the process arguments in order; the last explicit switch wins. Unrelated arguments do
    /// nothing and the comparison is ordinal-ignore-case, because the VR launcher's own casing is
    /// not part of the contract.
    /// </summary>
    public static bool IsRequested(string[]? args)
    {
        if (args == null) return false;
        var requested = false;
        foreach (var argument in args)
        {
            if (string.Equals(argument, RequestSwitch, StringComparison.OrdinalIgnoreCase))
                requested = true;
            else if (string.Equals(argument, DenySwitch, StringComparison.OrdinalIgnoreCase))
                requested = false;
        }
        return requested;
    }
}
