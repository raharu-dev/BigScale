namespace BigScale.Core;

/// <summary>What a close must undo. Captured before any stock call, because the stock Back action can
/// disable the settings screen synchronously and would otherwise clear the live fields.</summary>
public readonly struct ClosePlan
{
    public ClosePlan(bool openedPause, bool switchOnly, int previousCategoryToken, bool hadPreviousSelection)
    {
        OpenedPause = openedPause;
        SwitchOnly = switchOnly;
        PreviousCategoryToken = previousCategoryToken;
        HadPreviousSelection = hadPreviousSelection;
    }

    /// <summary>True when this shortcut opened the pause menu and must close it again.</summary>
    public bool OpenedPause { get; }

    /// <summary>True when the settings screen was already open and must be left open.</summary>
    public bool SwitchOnly { get; }

    /// <summary>Instance token of the category that was showing before the shortcut, or 0.</summary>
    public int PreviousCategoryToken { get; }

    /// <summary>True when a previous selection should be restored.</summary>
    public bool HadPreviousSelection { get; }
}

/// <summary>
/// Pure ownership bookkeeping for the shortcut's screens. It answers two questions that are easy to
/// get wrong: when a pending open may be resolved, and what a close must undo when the stock Back
/// action disables the screen in the middle of the close.
/// </summary>
public sealed class MenuSession
{
    /// <summary>Frames a pending open may wait for the stock settings screen.</summary>
    public const int PendingFrameLimit = 180;

    private int _pendingSince;

    /// <summary>True while an open request waits for the stock settings screen.</summary>
    public bool PendingShow { get; private set; }

    /// <summary>True when this shortcut opened the pause menu and must close it again.</summary>
    public bool OpenedPause { get; private set; }

    /// <summary>True when the settings screen was already open before the shortcut.</summary>
    public bool SwitchOnly { get; private set; }

    /// <summary>True while a close is in progress.</summary>
    public bool IsClosing { get; private set; }

    /// <summary>Instance token of the previous category, or 0.</summary>
    public int PreviousCategoryToken { get; private set; }

    /// <summary>True when a previous selection was captured.</summary>
    public bool HasPreviousSelection { get; private set; }

    /// <summary>Starts a gameplay open: this shortcut will own the pause menu it opens.</summary>
    public void BeginOpenFromGameplay(int frame, int previousCategoryToken, bool hasSelection)
    {
        OpenedPause = false;
        SwitchOnly = false;
        IsClosing = false;
        PreviousCategoryToken = previousCategoryToken;
        HasPreviousSelection = hasSelection;
        PendingShow = false;
        _pendingSince = frame;
    }

    /// <summary>Records that the stock pause menu was opened by this shortcut.</summary>
    public void PauseOpened() => OpenedPause = true;

    /// <summary>Asks for the category to be shown as soon as the settings screen is ready.</summary>
    public void RequestPending(int frame)
    {
        PendingShow = true;
        _pendingSince = frame;
    }

    /// <summary>
    /// True when a pending open should be resolved now. <paramref name="expired"/> is set once the
    /// wait exceeded <see cref="PendingFrameLimit"/>; the pending request is dropped in that case.
    /// </summary>
    public bool ShouldResolve(int frame, out bool expired)
    {
        expired = false;
        if (!PendingShow) return false;
        if (IsClosing) return false;
        if (frame - _pendingSince > PendingFrameLimit)
        {
            PendingShow = false;
            expired = true;
            return false;
        }
        return true;
    }

    /// <summary>The pending open succeeded.</summary>
    public void PendingResolved() => PendingShow = false;

    /// <summary>Starts a switch inside an already-open settings screen.</summary>
    public void BeginSwitchOnOpenScreen(int previousCategoryToken, bool hasSelection)
    {
        SwitchOnly = true;
        OpenedPause = false;
        PendingShow = false;
        IsClosing = false;
        PreviousCategoryToken = previousCategoryToken;
        HasPreviousSelection = hasSelection;
    }

    /// <summary>Captures everything the close must undo and marks the close in progress.</summary>
    public ClosePlan BeginClose()
    {
        IsClosing = true;
        return new ClosePlan(OpenedPause, SwitchOnly, PreviousCategoryToken, HasPreviousSelection);
    }

    /// <summary>
    /// The close could not complete (the stock Back action left the settings screen open or an
    /// action threw). Keep the ownership fields so a later attempt can still undo what this
    /// shortcut opened, and only clear the in-progress marker.
    /// </summary>
    public void CancelClose() => IsClosing = false;

    /// <summary>Finishes the close and clears the ownership it consumed.</summary>
    public void EndClose()
    {
        IsClosing = false;
        OpenedPause = false;
        SwitchOnly = false;
        PendingShow = false;
        PreviousCategoryToken = 0;
        HasPreviousSelection = false;
    }

    /// <summary>
    /// The settings screen was disabled. A close already in progress owns its snapshot, so this
    /// reports whether the notification was ignored.
    /// </summary>
    public bool ScreenDisabled()
    {
        if (IsClosing) return false;
        OpenedPause = false;
        SwitchOnly = false;
        PendingShow = false;
        PreviousCategoryToken = 0;
        HasPreviousSelection = false;
        return true;
    }

    /// <summary>Clears everything without touching the game's UI.</summary>
    public void Reset()
    {
        OpenedPause = false;
        SwitchOnly = false;
        PendingShow = false;
        IsClosing = false;
        PreviousCategoryToken = 0;
        HasPreviousSelection = false;
        _pendingSince = 0;
    }
}
