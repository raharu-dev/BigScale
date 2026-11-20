namespace BigScale.Core;

/// <summary>What this mod currently owns for the camera being rendered.</summary>
public enum OwnershipState
{
    /// <summary>Nothing resolved yet; the game's own renderer runs untouched.</summary>
    Idle,

    /// <summary>Capability probing only. No camera or pipeline value has been written.</summary>
    Probing,

    /// <summary>Scene scale and temporal inputs are owned and reconstruction replaces the graph.</summary>
    Owned,

    /// <summary>A defect latched. Nothing is re-acquired until an explicit enable/settings transition.</summary>
    Faulted,

    /// <summary>The user turned it off. Nothing is re-acquired until the setting changes.</summary>
    Disabled,

    /// <summary>NGX reports the feature unavailable on this device or driver.</summary>
    Unsupported,
}

/// <summary>Work that must happen at the next pre-camera boundary, before URP creates camera data.</summary>
public enum BoundaryAction
{
    None,

    /// <summary>Restore everything that was snapshotted.</summary>
    Restore,
}

/// <summary>
/// Pure ownership policy: probes never mutate anything, a resolved fit is committed exactly once at
/// the next pre-camera boundary, and a fault or a disable can never re-acquire implicitly. All the
/// Unity work stays in <c>GraphicsOwnership</c>; this decides only what is allowed to happen.
/// </summary>
public sealed class TransitionPlan
{
    private BoundaryAction _pending;
    private bool _commitRequested;

    public OwnershipState State { get; private set; } = OwnershipState.Idle;

    /// <summary>Human-readable reason for the current state; shown in the panel and the log.</summary>
    public string Reason { get; private set; } = "waiting for a supported camera";

    /// <summary>True while the mod has applied its scene-scale and temporal-input requests.</summary>
    public bool OwnsRendering => State == OwnershipState.Owned;

    /// <summary>True once a defect latched. Probes and later frames cannot clear it.</summary>
    public bool Faulted => State == OwnershipState.Faulted;

    /// <summary>True when rendering may be replaced instead of the game's own post-processing.</summary>
    public bool MayReplace => State == OwnershipState.Owned && _pending != BoundaryAction.Restore;

    /// <summary>Records that capability ranges are available, so the resolved fit may be committed.</summary>
    public void RequestCommit()
    {
        if (State is OwnershipState.Faulted or OwnershipState.Disabled or OwnershipState.Unsupported) return;
        if (State == OwnershipState.Owned) return;
        _commitRequested = true;
        State = OwnershipState.Probing;
        Reason = "capability probe answered; committing at the next camera boundary";
    }

    /// <summary>True while a fit has been resolved but its commit has not happened yet.</summary>
    public bool CommitPending => _commitRequested && State == OwnershipState.Probing;

    /// <summary>Called on the render thread boundary. Returns the action to perform now.</summary>
    public BoundaryAction OnBoundary()
    {
        var action = _pending;
        _pending = BoundaryAction.None;
        return action;
    }

    /// <summary>
    /// Consumes a pending commit request. The caller performs the commit immediately, so no second
    /// boundary action is queued: a commit must never repeat on its own.
    /// </summary>
    public bool TryBeginCommit()
    {
        if (!_commitRequested) return false;
        if (State is OwnershipState.Faulted or OwnershipState.Disabled or OwnershipState.Unsupported) return false;
        _commitRequested = false;
        return true;
    }

    /// <summary>The boundary committed the owned requests.</summary>
    public void CommitDone()
    {
        State = OwnershipState.Owned;
        Reason = "replacing the temporal upscale stage";
    }

    /// <summary>A defect. Latches and asks for a restore at the next boundary.</summary>
    public void Fail(string reason)
    {
        State = OwnershipState.Faulted;
        Reason = reason;
        _commitRequested = false;
        _pending = BoundaryAction.Restore;
    }

    /// <summary>The user disabled reconstruction. Requests a restore without latching a fault.</summary>
    public void Disable(string reason)
    {
        State = OwnershipState.Disabled;
        Reason = reason;
        _commitRequested = false;
        _pending = BoundaryAction.Restore;
    }

    /// <summary>No supported output size or NGX feature. Requests a restore and stays inactive.</summary>
    public void Unsupported(string reason)
    {
        State = OwnershipState.Unsupported;
        Reason = reason;
        _commitRequested = false;
        _pending = BoundaryAction.Restore;
    }

    /// <summary>Returns to probing after a restore, for example when the primary camera changed.</summary>
    public void Reacquire(string reason)
    {
        if (State is OwnershipState.Disabled or OwnershipState.Unsupported or OwnershipState.Faulted) return;
        State = OwnershipState.Probing;
        Reason = reason;
        _commitRequested = false;
        _pending = BoundaryAction.Restore;
    }

    /// <summary>An explicit enable or settings transition: the only way out of a latched state.</summary>
    public void Enable(string reason)
    {
        _commitRequested = false;
        _pending = BoundaryAction.Restore;
        State = OwnershipState.Probing;
        Reason = reason;
    }

    /// <summary>Records a non-committing status without changing ownership.</summary>
    public void Wait(string reason)
    {
        if (State == OwnershipState.Owned) return;
        if (State is OwnershipState.Faulted or OwnershipState.Disabled) return;
        State = OwnershipState.Probing;
        Reason = reason;
    }
}
