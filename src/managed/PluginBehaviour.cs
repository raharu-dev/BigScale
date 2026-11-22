using System;
using HarmonyLib;
using UnityEngine;

namespace BigScale;

/// <summary>Per-frame driver for <see cref="RenderCoordinator"/>; runs on the Unity main thread.</summary>
internal sealed class PluginBehaviour : MonoBehaviour
{
    private int _refreshCountdown;
    private string? _lastTickFailure;

    private void Update()
    {
        var instance = Plugin.Instance;
        if (instance == null) return;

        // A renderer defect must never take the settings UI down with it. The failure is reported
        // once per distinct message; a later successful tick only re-arms that suppression, not the
        // latched ownership fault.
        var coordinator = instance.Coordinator;
        if (coordinator != null)
        {
            try
            {
                coordinator.Tick();
                _lastTickFailure = null;
            }
            catch (Exception exception)
            {
                try
                {
                    coordinator.ReportTickFailure(exception);
                }
                catch
                {
                    // Reporting must not hide the renderer's original fault.
                }
                var message = exception.GetType().Name + ": " + exception.Message;
                if (_lastTickFailure != message)
                {
                    _lastTickFailure = message;
                    instance.LogError("the BigScale render tick failed: " + exception);
                }
            }
        }

        RefreshVisiblePanel();

        // Big Visuals may load after this plugin, so interception is retried until it sticks.
        if (!instance.Compatibility.IsReady)
            instance.Compatibility.Initialize(instance.Harmony);
    }

    /// <summary>
    /// Bounded refresh keeps the status row current without touching the game's UI every frame.
    /// Deliberately parameterless: an injected MonoBehaviour method cannot take a managed plugin type.
    /// </summary>
    private void RefreshVisiblePanel()
    {
        if (_refreshCountdown > 0)
        {
            _refreshCountdown--;
            return;
        }
        _refreshCountdown = 15;
        SettingsPanel.RefreshVisible();
    }

    private void OnApplicationQuit()
    {
        Plugin.Instance?.ShutdownFromBehaviour();
    }
}
