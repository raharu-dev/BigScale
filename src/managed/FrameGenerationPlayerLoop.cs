using System;
using System.Collections.Generic;
using UnityEngine.LowLevel;

namespace BigScale;

/// <summary>
/// Inserts the two XeLL frame boundaries into Unity's player loop.
///
/// The frame id must be assigned once per frame before input sampling, and the simulation phase must
/// close after late update. Both callbacks are cached and rooted; uninstalling removes only this
/// mod's nodes from the current loop, never a saved copy that would clobber other mods' changes.
/// </summary>
internal sealed class FrameGenerationPlayerLoop
{
    private readonly Action _beginFrame;
    private readonly Action _endSimulation;
    private bool _installed;

    internal FrameGenerationPlayerLoop()
    {
        _beginFrame = BeginFrame;
        _endSimulation = EndSimulation;
    }

    internal bool Installed => _installed;
    internal string Status { get; private set; } = "not installed";

    internal bool Install()
    {
        if (_installed) return true;
        var loop = PlayerLoop.GetCurrentPlayerLoop();
        if (loop.subSystemList == null || loop.subSystemList.Length == 0)
        {
            Status = "the Unity player loop has no subsystems";
            return false;
        }
        var systems = new List<PlayerLoopSystem>(loop.subSystemList);
        // The interop layer hands out il2cpp type proxies, so identity against typeof(...) fails even
        // though the subsystem is present. Match by name and report what the loop actually contains.
        var early = systems.FindIndex(system => IsSubsystem(system, "EarlyUpdate"));
        var late = systems.FindIndex(system => IsSubsystem(system, "PreLateUpdate"));
        if (early < 0 || late < 0)
        {
            var names = new List<string>(systems.Count);
            foreach (var system in systems) names.Add(system.type?.Name ?? "<anonymous>");
            Status = "the Unity player loop is missing EarlyUpdate or PreLateUpdate (" +
                     string.Join(", ", names) + ")";
            return false;
        }
        var begin = new PlayerLoopSystem
        {
            type = null,
            updateDelegate = _beginFrame,
            subSystemList = Array.Empty<PlayerLoopSystem>(),
        };
        systems.Insert(early, begin);
        // Re-find PreLateUpdate: the begin node shifted every later index.
        late = systems.FindIndex(system => IsSubsystem(system, "PreLateUpdate"));
        var end = new PlayerLoopSystem
        {
            type = null,
            updateDelegate = _endSimulation,
            subSystemList = Array.Empty<PlayerLoopSystem>(),
        };
        systems.Insert(late + 1, end);
        loop.subSystemList = systems.ToArray();
        PlayerLoop.SetPlayerLoop(loop);
        _installed = true;
        Status = "frame-generation player loop installed";
        return true;
    }

    internal void Uninstall()
    {
        if (!_installed) return;
        try
        {
            var loop = PlayerLoop.GetCurrentPlayerLoop();
            if (loop.subSystemList == null) return;
            var systems = new List<PlayerLoopSystem>(loop.subSystemList.Length);
            foreach (var system in loop.subSystemList)
            {
                if (system.type != null || system.subSystemList == null || system.subSystemList.Length != 0)
                {
                    systems.Add(system);
                    continue;
                }
                if (IsOurNode(system)) continue;
                systems.Add(system);
            }
            loop.subSystemList = systems.ToArray();
            PlayerLoop.SetPlayerLoop(loop);
        }
        catch
        {
            // Best effort during shutdown.
        }
        _installed = false;
        Status = "uninstalled";
    }

    private static bool IsSubsystem(in PlayerLoopSystem system, string name) =>
        system.type != null && system.type.Name == name;

    private bool IsOurNode(in PlayerLoopSystem system)
    {
        var update = system.updateDelegate;
        if (update == null) return false;
        return update.Equals((PlayerLoopSystem.UpdateFunction)_beginFrame) ||
            update.Equals((PlayerLoopSystem.UpdateFunction)_endSimulation);
    }

    private static void BeginFrame() => Plugin.Instance?.Coordinator?.FrameGeneration?.BeginFrame();

    private static void EndSimulation() => Plugin.Instance?.Coordinator?.FrameGeneration?.EndSimulation();
}
