using System;
using BigScale.Core;
using BepInEx.Logging;
using TMPro;
using UnityEngine;
using UnityEngine.EventSystems;

namespace BigScale;

/// <summary>
/// The Delete-key route into the in-game <c>Upscaling</c> category. It never replaces the game's
/// pause or input policy: opening goes through <c>WorldMenuManager.OpenMenu()</c> and the stock
/// Settings button handler, closing goes through <c>SettingsMenu.SettingBackEvent()</c> and the
/// current-mode <c>PauseMenu.ActionResume()</c>, and only the screens this shortcut opened are closed
/// again. The native <c>ControlsManager</c>/<c>CursorManager</c> owners are used only for the pause
/// screen this shortcut opened; a screen the player opened stays under the stock lifecycle.
///
/// The key resolves before the category exists: Delete asks the game to open its own settings screen
/// and keeps a pending show intent for that exact menu until the screen is ready. That removes the
/// previous cycle where the shortcut refused to open a screen because the screen's panel had not
/// been built yet.
///
/// All ownership bookkeeping lives in <see cref="MenuSession"/>, which captures the close plan before
/// the stock Back action can disable the screen synchronously.
/// </summary>
internal sealed class KeyMenu
{
    private static readonly ManualLogSource Log = BepInEx.Logging.Logger.CreateLogSource("BigScale.Key");

    private readonly ModSettings _settings;
    private readonly MenuSession _session = new();
    private bool _wasDown;
    private bool _shortcutReadFailed;
    private SettingsMenu? _menu;
    private PauseMenu? _pause;
    private GameObject? _previousSelection;

    internal KeyMenu(ModSettings settings) => _settings = settings;

    /// <summary>Edge-detected poll; called every frame from the plugin behaviour.</summary>
    internal void Poll()
    {
        try
        {
            ResolvePendingShow();
        }
        catch (Exception exception)
        {
            Log.LogWarning("the BigScale pending show failed: " + exception);
        }

        var pressed = ReadShortcut();
        var edge = pressed && !_wasDown;
        _wasDown = pressed;
        if (!edge) return;
        if (TextEntryActive()) return;

        try
        {
            Toggle();
        }
        catch (Exception exception)
        {
            Log.LogWarning("the BigScale menu key failed: " + exception);
        }
    }

    /// <summary>Reads the configured key; a bad binding is reported once.</summary>
    private bool ReadShortcut()
    {
        try
        {
            _shortcutReadFailed = false;
            var key = _settings.MenuKey.Value;
            return key != KeyCode.None && Input.GetKeyDown(key);
        }
        catch (Exception exception)
        {
            if (_shortcutReadFailed) return false;
            _shortcutReadFailed = true;
            Log.LogWarning("the BigScale menu key could not be read; check UI/MenuKey: " + exception.Message);
            return false;
        }
    }

    /// <summary>Resolves an open request once the stock settings screen is ready.</summary>
    private void ResolvePendingShow()
    {
        var frame = Time.frameCount;
        if (!_session.ShouldResolve(frame, out var expired))
        {
            if (expired) Log.LogWarning("the settings screen did not become ready; the Upscaling category was not opened.");
            return;
        }
        var menu = _menu;
        if (menu == null || menu.gameObject == null || !menu.gameObject.activeInHierarchy) return;

        var panel = SettingsPanel.Ensure(menu);
        if (panel == null)
        {
            if (SettingsPanel.TryGetBuildFailure(menu, out var reason))
            {
                Log.LogError("the Upscaling category could not be opened: " + reason);
                _session.Reset();
            }
            return;
        }
        panel.Show();
        _session.PendingResolved();
        Log.LogInfo($"opened the Upscaling category (menu id={menu.GetInstanceID()}).");
    }

    /// <summary>True while the player is typing, so a menu key can never steal a character.</summary>
    private static bool TextEntryActive()
    {
        try
        {
            if (SignTextInput.IsSignInputActive()) return true;
        }
        catch
        {
            // The sign input type differs or is absent; fall through to the other checks.
        }

        var manager = WorldMenuManager.instance;
        if (manager != null)
        {
            var chat = manager.textChatInput;
            if (chat != null && chat.gameObject != null && chat.gameObject.activeInHierarchy)
            {
                var field = chat.inputField;
                if (field != null && field.isActiveAndEnabled && field.isFocused) return true;
            }
        }

        var events = EventSystem.current;
        var selected = events != null ? events.currentSelectedGameObject : null;
        if (selected == null) return false;
        var direct = selected.GetComponent<TMP_InputField>();
        if (direct != null && direct.isActiveAndEnabled && direct.isFocused) return true;
        var parent = selected.GetComponentInParent<TMP_InputField>();
        return parent != null && parent.isActiveAndEnabled && parent.isFocused;
    }

    private void Toggle()
    {
        var manager = WorldMenuManager.instance;
        if (manager == null)
        {
            // Main menu, loading, or a scene without the in-game menu. The category still attaches
            // when the player opens Settings through the stock UI.
            _session.Reset();
            Log.LogInfo("no in-game menu manager in this scene; the BigScale menu key is ignored here.");
            return;
        }
        var menu = manager.settingsMenu;
        if (menu == null)
        {
            Log.LogInfo("the game's settings screen is not available yet; the BigScale menu key is ignored.");
            return;
        }
        if (_menu != null && _menu != menu)
        {
            Log.LogInfo("the settings screen was replaced; clearing shortcut state.");
            Forget();
        }
        _menu = menu;

        if (!menu.gameObject.activeInHierarchy)
        {
            OpenFromGameplay(manager, menu);
            return;
        }

        // The player's own settings screen is open. Make sure the category exists, then toggle it.
        var panel = SettingsPanel.Ensure(menu);
        if (panel == null)
        {
            if (SettingsPanel.TryGetBuildFailure(menu, out var reason))
            {
                Log.LogError("the Upscaling category could not be opened: " + reason);
                return;
            }
            _session.RequestPending(Time.frameCount);
            Log.LogInfo("the Upscaling category will open when the settings screen is ready.");
            return;
        }
        if (panel.IsVisible)
        {
            Close(menu, panel);
            return;
        }

        _previousSelection = CurrentSelection();
        _session.BeginSwitchOnOpenScreen(Token(menu.activeCatagory), _previousSelection != null);
        panel.Show();
        Log.LogInfo("switched the open settings screen to the Upscaling category.");
    }

    private void OpenFromGameplay(WorldMenuManager manager, SettingsMenu menu)
    {
        _previousSelection = CurrentSelection();
        var ownedPause = _session.OpenedPause;
        _session.BeginOpenFromGameplay(Time.frameCount, Token(menu.activeCatagory), _previousSelection != null);

        // Resolve the current-mode pause before touching any screen. A pause screen the player
        // already had open is not ours to resume later; only the one this shortcut opens is.
        var pause = manager.pauseMenuForCurrentMode;
        _pause = pause;
        var pauseWasOpen = pause != null && pause.gameObject != null && pause.gameObject.activeInHierarchy;
        var ownsPause = pause != null && (!pauseWasOpen || ownedPause);
        if (ownsPause) _session.PauseOpened();

        // The game's own pause open, then the game's own Settings button handler.
        manager.OpenMenu();
        if (!menu.gameObject.activeInHierarchy)
        {
            if (pause == null)
            {
                Log.LogInfo("no pause menu is available for this game mode; the BigScale menu key is ignored.");
                _session.Reset();
                _pause = null;
                return;
            }
            pause.ActionGoToSettings();
        }
        if (!menu.gameObject.activeInHierarchy)
        {
            Log.LogInfo("the stock settings route did not open the settings screen; closing the pause menu again.");
            ResumePause(ownsPause);
            _session.Reset();
            _pause = null;
            return;
        }

        // The stock route changes screens visually but does not publish the menu input mode for
        // this shortcut path. Apply the native menu input owners for the pause screen we opened.
        if (ownsPause) ApplyMenuInputMode();

        // Start/OnEnable may not have run yet, so the category is shown once it is ready.
        _session.RequestPending(Time.frameCount);
        Log.LogInfo("the Upscaling category will open when the settings screen is ready.");
    }

    /// <summary>
    /// Releases the cursor and suppresses gameplay look through the game's native input owners.
    /// The readback is the only evidence this build can offer; visible behavior stays user-tested.
    /// </summary>
    private static void ApplyMenuInputMode()
    {
        try
        {
            ControlsManager.SetMenuMode(true);
            CursorManager.SetFree();
        }
        catch (Exception exception)
        {
            Log.LogWarning("could not apply the native menu input mode: " + exception.Message);
            return;
        }
        LogNativeMenuMode("after opening the BigScale route");
    }

    private static void LogNativeMenuMode(string when)
    {
        try
        {
            var active = ControlsManager.menuModeActive;
            Log.LogInfo($"native menu mode={(active ? "active" : "inactive")} {when}.");
        }
        catch (Exception exception)
        {
            Log.LogWarning("could not read the native menu mode: " + exception.Message);
        }
    }

    private void Close(SettingsMenu menu, SettingsPanel panel)
    {
        if (_session.IsClosing) return;
        var plan = _session.BeginClose();
        var selection = _previousSelection;
        var completed = false;
        try
        {
            if (plan.SwitchOnly)
            {
                panel.RestoreCategory(FindCategory(plan.PreviousCategoryToken));
                RestoreSelection(selection);
                completed = true;
                return;
            }

            panel.RestoreCategory(FindCategory(plan.PreviousCategoryToken));
            for (var i = 0; i < 3 && menu.gameObject.activeInHierarchy; i++) menu.SettingBackEvent();
            if (menu.gameObject.activeInHierarchy)
            {
                Log.LogInfo("the settings screen did not close through its Back action; leaving it open.");
                return;
            }
            ResumePause(plan.OpenedPause);
            RestoreSelection(selection);
            completed = true;
        }
        finally
        {
            if (completed)
            {
                _previousSelection = null;
                _pause = null;
                _session.EndClose();
            }
            else if (_session.IsClosing)
            {
                // The screen or an action failed midway. Keep the ownership fields so a later
                // shortcut attempt can still leave the screens it opened.
                _session.CancelClose();
            }
        }
    }

    /// <summary>
    /// Leaves the pause screen through the current-mode <see cref="PauseMenu"/> action the game
    /// uses for Resume; the native action owns input/cursor restoration.
    /// </summary>
    private void ResumePause(bool opened)
    {
        if (!opened) return;
        var pause = _pause;
        if (pause == null || pause.gameObject == null)
        {
            Log.LogInfo("the pause menu this shortcut opened is already gone; no resume action is needed.");
            return;
        }
        try
        {
            pause.ActionResume();
        }
        catch (Exception exception)
        {
            Log.LogWarning("could not resume the pause menu: " + exception.Message);
            return;
        }
        LogNativeMenuMode("after closing the BigScale route");
    }

    /// <summary>Resolves a remembered category token to the live category component, or null.</summary>
    private SettingsCatagory? FindCategory(int token)
    {
        if (token == 0 || _menu == null) return null;
        foreach (var candidate in new[]
                 {
                     _menu.catagoryGeneral, _menu.catagoryInterface, _menu.catagoryAudio, _menu.catagoryMicrophone,
                     _menu.catagoryGraphics, _menu.catagoryDisplay, _menu.catagoryControls, _menu.catagoryRebind,
                 })
        {
            if (candidate == null) continue;
            if (candidate.GetInstanceID() == token) return candidate;
        }
        return null;
    }

    private static int Token(SettingsCatagory? catagory) =>
        catagory == null ? 0 : catagory.GetInstanceID();

    private static GameObject? CurrentSelection()
    {
        var events = EventSystem.current;
        return events != null ? events.currentSelectedGameObject : null;
    }

    private static void RestoreSelection(GameObject? target)
    {
        if (target == null) return;
        try
        {
            var events = EventSystem.current;
            if (events != null && target.activeInHierarchy) events.SetSelectedGameObject(target);
        }
        catch
        {
            // The remembered selection may belong to a screen that is gone.
        }
    }

    /// <summary>
    /// The settings screen was disabled. A close already in progress owns its plan, so this only
    /// clears the state that belongs to an open screen.
    /// </summary>
    internal void SettingsClosed()
    {
        if (!_session.ScreenDisabled()) return;
        _previousSelection = null;
        _pause = null;
    }

    /// <summary>Clears every remembered screen without touching the game's UI.</summary>
    internal void Forget()
    {
        _session.Reset();
        _previousSelection = null;
        _menu = null;
        _pause = null;
    }
}
