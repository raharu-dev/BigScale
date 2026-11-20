namespace BigScale.Core;

/// <summary>
/// Per-operation resource requirements for a native packet. A capability probe is read-only and only
/// needs the scene colour plus a positive output extent; an evaluation additionally needs the
/// temporal inputs and a destination. Shutdown and recovery are control operations that carry no
/// image at all. Keeping this in one place means the render callback cannot drift away from what the
/// native bridge accepts.
/// </summary>
public static class FramePolicy
{
    /// <summary>Mirrors <c>BIGSCALE_OP_PROBE</c>.</summary>
    public const int Probe = 0;

    /// <summary>Mirrors <c>BIGSCALE_OP_EVALUATE</c>.</summary>
    public const int Evaluate = 1;

    /// <summary>Mirrors <c>BIGSCALE_OP_SHUTDOWN</c>: drains and releases the feature without an image.</summary>
    public const int Shutdown = 3;

    /// <summary>Mirrors <c>BIGSCALE_OP_RECOVERY</c>: drains and drops faulted state before a fresh probe.</summary>
    public const int Recovery = 4;

    /// <summary>Mirrors <c>BIGSCALE_OP_SWITCH</c>: drains the old provider and acknowledges the selected one.</summary>
    public const int Switch = 5;

    /// <summary>True for an operation this build knows how to dispatch.</summary>
    public static bool IsKnown(int operation) =>
        operation is Probe or Evaluate or Shutdown or Recovery or Switch;

    /// <summary>
    /// True for a control packet: it carries no image resources and no output extent, because the
    /// bridge dispatches it before it validates any of them.
    /// </summary>
    public static bool IsControl(int operation) => operation is Shutdown or Recovery or Switch;

    /// <summary>True when the operation writes a reconstructed image.</summary>
    public static bool WritesOutput(int operation) => operation is Evaluate;

    /// <summary>
    /// True when the packet carries everything its operation needs.
    /// <paramref name="missing"/> names the first absent requirement for diagnostics.
    /// </summary>
    public static bool Satisfied(int operation, bool hasColor, int outputWidth, int outputHeight, bool hasDepth,
        bool hasMotion, bool hasOutput, out string missing)
    {
        missing = "";
        if (!IsKnown(operation))
        {
            missing = "unknown operation " + operation;
            return false;
        }
        // Shutdown and recovery are control packets: the native bridge dispatches them before it
        // looks at any texture or extent, so requiring an image here would strand a latched fault.
        if (IsControl(operation)) return true;
        if (!hasColor)
        {
            missing = "colour";
            return false;
        }
        if (outputWidth <= 0 || outputHeight <= 0)
        {
            missing = $"output extent {outputWidth}x{outputHeight}";
            return false;
        }
        if (!WritesOutput(operation)) return true;
        if (!hasDepth)
        {
            missing = "depth";
            return false;
        }
        if (!hasMotion)
        {
            missing = "motion vectors";
            return false;
        }
        if (!hasOutput)
        {
            missing = "output target";
            return false;
        }
        return true;
    }
}
