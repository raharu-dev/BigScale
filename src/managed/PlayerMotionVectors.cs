using System;
using System.Collections.Generic;
using BigScale.Core;
using Il2CppInterop.Runtime.InteropTypes.Arrays;
using UnityEngine;
using UnityEngine.Rendering;

namespace BigScale;

/// <summary>
/// Gives every player character real per-object motion vectors, which the game's own shaders never
/// draw.
///
/// The game's player shaders (<c>househouse/PlayerCharacter</c>, <c>househouse/PlayerNose</c>) have no
/// <c>MotionVectors</c> pass, so URP's motion pass leaves the full-screen camera motion on body and
/// hand pixels. A body locked to the camera then reprojects as static world geometry, and at arm's
/// length that camera parallax is the largest vector in the frame, so every generated frame ghosts it.
/// A remote player is wrong the other way round: it is real world geometry, so the unrepaired camera
/// motion also ghosts it whenever it moves.
///
/// The repair is two-sided. Camera-mode renderers that already have the pass are promoted to
/// <c>Object</c>, which is all URP needs. Renderers without the pass get one extra material appended: a
/// stock Lit instance with every pass but <c>MotionVectors</c> disabled. Their material list then has a
/// motion pass, so URP's own motion pass draws them from the camera's culled list, with real previous
/// model matrices, the camera's culling mask, LOD selection and shadow visibility. Nothing is drawn by
/// this class: the appended material renders nothing in any other pass, and a direct draw would bypass
/// the culling that keeps the game's hidden body set off screen.
/// </summary>
internal sealed class PlayerMotionVectors
{
    private const int RescanFrames = 120;
    private const string MotionPassName = "MotionVectors";
    private const string ForwardShader = "Universal Render Pipeline/Lit";
    // The game's first-person nose decoration: live geometry that never writes colour.
    private const string HiddenShader = CharacterMaskPolicy.LocalNoseShaderName;
    private const string MaterialName = "BigScale.ObjectMotion";

    private sealed class MotionRecord
    {
        internal int Key;
        internal Renderer Renderer = null!;
        internal bool Attached;
        internal bool Promoted;
        internal MotionVectorGenerationMode OriginalMode;
    }

    private readonly Action<string> _log;
    private readonly Dictionary<int, MotionRecord> _records = new();
    private readonly List<int> _deadKeys = new();

    private Material? _material;
    private bool _materialReported;
    private int _nextScanFrame;
    private int _scannedSkipped;
    private int _lastAttached = -1;
    private int _lastPromoted = -1;
    private int _lastSkipped = -1;
    private int _lastCharacters = -1;
    private bool _faulted;

    internal PlayerMotionVectors(Action<string> log)
    {
        _log = log;
    }

    internal void Shutdown()
    {
        RestoreAll("unloaded");
        if (_material != null)
        {
            UnityEngine.Object.Destroy(_material);
            _material = null;
        }
    }

    /// <summary>Keeps the game's renderer state in sync with the master switch.</summary>
    internal void Tick(bool enabled)
    {
        if (!enabled)
        {
            RestoreAll("disabled");
            return;
        }
        if (_faulted) return;
        if (Time.frameCount < _nextScanFrame) return;

        Il2CppSystem.Collections.Generic.List<PlayerCharacter> characters;
        try
        {
            characters = PlayerCharacter.allPlayerCharacters;
        }
        catch (Exception exception)
        {
            Fault("reading the player characters failed: " + exception);
            return;
        }
        if (characters == null || characters.Count == 0)
        {
            // An empty list is the normal case during a load; without rescheduling here the scan would
            // run every frame until a character exists.
            _nextScanFrame = Time.frameCount + RescanFrames;
            return;
        }

        _scannedSkipped = 0;
        var scanned = 0;
        for (var i = 0; i < characters.Count; i++)
        {
            var character = characters[i];
            if (character == null) continue;
            var body = character.gameObject;
            if (body == null) continue;
            try
            {
                Scan(body);
                scanned++;
            }
            catch (Exception exception)
            {
                Fault("scanning player renderers failed: " + exception);
                return;
            }
        }

        _nextScanFrame = Time.frameCount + RescanFrames;
        Prune();
        Report(scanned);
    }

    /// <summary>Classifies one character's renderers and re-asserts the ones the game may have reset.</summary>
    private void Scan(GameObject body)
    {
        var renderers = body.GetComponentsInChildren<Renderer>(true);
        foreach (var renderer in renderers)
        {
            if (renderer == null) continue;
            var key = renderer.GetInstanceID();
            if (_records.TryGetValue(key, out var existing))
            {
                Reassert(existing);
                continue;
            }

            var materials = renderer.sharedMaterials;
            var count = 0;
            var withPass = 0;
            var queue = int.MinValue;
            var hidden = false;
            for (var i = 0; i < materials.Length; i++)
            {
                var material = materials[i];
                if (material == null || IsOwnMaterial(material)) continue;
                count++;
                if (material.FindPass(MotionPassName) >= 0) withPass++;
                if (material.renderQueue > queue) queue = material.renderQueue;
                var shader = material.shader;
                if (shader != null && shader.name == HiddenShader) hidden = true;
            }

            var meshOrSkinned = renderer.TryCast<MeshRenderer>() != null ||
                                renderer.TryCast<SkinnedMeshRenderer>() != null;
            var mode = renderer.motionVectorGenerationMode;
            var submeshCount = SubmeshCount(renderer);
            var action = PlayerMotionPolicy.Classify(new PlayerRendererMotion
            {
                MeshOrSkinned = meshOrSkinned,
                CameraMode = mode == MotionVectorGenerationMode.Camera,
                Hidden = hidden,
                MaterialCount = count,
                MaterialsWithMotionPass = withPass,
                MaxRenderQueue = queue,
                SubmeshCount = submeshCount,
            });

            if (action == PlayerMotionAction.None)
            {
                // Only a renderer the mod could otherwise repair is worth reporting as skipped.
                var eligible = !hidden && meshOrSkinned && count > 0 &&
                               queue <= PlayerMotionPolicy.OpaqueQueueMax;
                if (eligible && ((withPass != 0 && withPass != count) ||
                                 (withPass == 0 && (count != 1 || submeshCount != 1))))
                    _scannedSkipped++;
                continue;
            }

            var attach = (action & PlayerMotionAction.AttachMotionMaterial) != 0;
            if (attach)
            {
                EnsureMaterial();
                if (_material == null) continue;
            }
            if (mode != MotionVectorGenerationMode.Object)
                renderer.motionVectorGenerationMode = MotionVectorGenerationMode.Object;
            if (attach) Append(renderer);

            _records[key] = new MotionRecord
            {
                Key = key,
                Renderer = renderer,
                Attached = attach,
                Promoted = (action & PlayerMotionAction.PromoteToObject) != 0,
                OriginalMode = mode,
            };
        }
    }

    /// <summary>The mesh's submesh count, or 0 when the renderer has no mesh.</summary>
    private static int SubmeshCount(Renderer renderer)
    {
        Mesh? mesh = renderer.TryCast<SkinnedMeshRenderer>()?.sharedMesh;
        if (mesh == null)
        {
            var filter = renderer.GetComponent<MeshFilter>();
            if (filter != null) mesh = filter.sharedMesh;
        }
        return mesh != null ? mesh.subMeshCount : 0;
    }

    /// <summary>
    /// True for the mod's own material, identified by name prefix so a <c>" (Instance)"</c> copy the
    /// game makes still counts. The prefix is compared character by character: this Unity build's
    /// mscorlib throws a method-not-found from <c>string.StartsWith(string, StringComparison)</c>
    /// because its <c>ReadOnlySpan&lt;T&gt;</c> has no generic pinnable reference.
    /// </summary>
    private static bool IsOwnMaterial(Material? material)
    {
        if (material == null) return false;
        var name = material.name;
        if (name == null || name.Length < MaterialName.Length) return false;
        for (var i = 0; i < MaterialName.Length; i++)
        {
            if (name[i] != MaterialName[i]) return false;
        }
        return true;
    }

    /// <summary>Puts the motion-only material last, so Unity draws it on the last submesh.</summary>
    private void Append(Renderer renderer)
    {
        if (_material == null) return;
        var current = renderer.sharedMaterials;
        for (var i = 0; i < current.Length; i++)
        {
            if (IsOwnMaterial(current[i])) return;
        }
        var materials = new Material[current.Length + 1];
        for (var i = 0; i < current.Length; i++) materials[i] = current[i];
        materials[current.Length] = _material;
        renderer.sharedMaterials = new Il2CppReferenceArray<Material>(materials);
    }

    private void Reassert(MotionRecord record)
    {
        var renderer = record.Renderer;
        if (renderer == null) return;
        if (renderer.motionVectorGenerationMode != MotionVectorGenerationMode.Object)
            renderer.motionVectorGenerationMode = MotionVectorGenerationMode.Object;
        // The game can replace the array when its look changes; re-append keeps the motion pass.
        if (record.Attached) Append(renderer);
    }

    /// <summary>
    /// Builds the one motion-only material this owner needs. URP's motion pass draws any renderer whose
    /// material has a <c>MotionVectors</c> pass, so a stock Lit instance with every other pass disabled
    /// makes URP draw the renderer's own motion and nothing else.
    /// </summary>
    private void EnsureMaterial()
    {
        if (_material != null || _materialReported) return;
        var shader = Shader.Find(ForwardShader);
        if (shader == null)
        {
            _materialReported = true;
            _log($"shader '{ForwardShader}' is not loaded; player motion cannot be corrected");
            return;
        }
        var material = new Material(shader)
        {
            name = MaterialName,
            hideFlags = HideFlags.HideAndDontSave,
        };
        // The body and hand meshes are opaque. Alpha clipping needs a texture this material does not
        // have, and the variant is compiled only when the keyword is on.
        material.DisableKeyword("_ALPHATEST_ON");
        material.renderQueue = 2000;

        var disabled = new List<string>();
        var lightMode = new ShaderTagId("LightMode");
        var motion = new ShaderTagId(MotionPassName);
        var passes = shader.passCount;
        for (var i = 0; i < passes; i++)
        {
            var tag = shader.FindPassTagValue(i, lightMode);
            // Tag ids are case-insensitive and are what SetShaderPassEnabled resolves: URP Lit tags the
            // same pass "MOTIONVECTORS" in one subshader and "MotionVectors" in another.
            if (tag == motion) continue;
            var name = tag.name;
            if (string.IsNullOrEmpty(name)) continue;
            material.SetShaderPassEnabled(name, false);
            var known = false;
            for (var j = 0; j < disabled.Count && !known; j++) known = disabled[j] == name;
            if (!known) disabled.Add(name);
        }
        if (material.FindPass(MotionPassName) < 0)
        {
            _materialReported = true;
            UnityEngine.Object.Destroy(material);
            _log($"shader '{ForwardShader}' has no usable MotionVectors pass; player motion cannot be corrected");
            return;
        }
        _material = material;
        _log($"player motion material: '{ForwardShader}' with only MotionVectors enabled " +
             $"(disabled: {string.Join(", ", disabled)})");
    }

    private void Prune()
    {
        _deadKeys.Clear();
        foreach (var record in _records.Values)
        {
            if (record.Renderer == null) _deadKeys.Add(record.Key);
        }
        foreach (var key in _deadKeys) _records.Remove(key);
    }

    private void Report(int characters)
    {
        var attached = 0;
        var promoted = 0;
        foreach (var record in _records.Values)
        {
            if (record.Attached) attached++;
            if (record.Promoted) promoted++;
        }
        if (attached == _lastAttached && promoted == _lastPromoted &&
            _scannedSkipped == _lastSkipped && characters == _lastCharacters) return;
        _lastAttached = attached;
        _lastPromoted = promoted;
        _lastSkipped = _scannedSkipped;
        _lastCharacters = characters;
        _log($"player motion vectors: attached the motion-only material to {attached} renderer(s) " +
             $"across {characters} character(s), promoted {promoted} from Camera to Object, " +
             $"skipped {_scannedSkipped} mixed or multi-submesh renderer(s).");
    }

    /// <summary>Reverts every renderer this owner touched.</summary>
    private void RestoreAll(string reason)
    {
        var restored = 0;
        foreach (var record in _records.Values)
        {
            var renderer = record.Renderer;
            if (renderer == null) continue;
            try
            {
                if (record.Attached) Detach(renderer);
                if (renderer.motionVectorGenerationMode != record.OriginalMode)
                    renderer.motionVectorGenerationMode = record.OriginalMode;
                restored++;
            }
            catch (Exception exception)
            {
                Fault("restoring a player renderer failed: " + exception);
            }
        }
        _records.Clear();
        _lastAttached = -1;
        _lastPromoted = -1;
        _lastSkipped = -1;
        _lastCharacters = -1;
        _nextScanFrame = 0;
        if (restored > 0) _log($"player motion vectors restored {restored} renderer(s): {reason}");
    }

    /// <summary>
    /// Rebuilds the material list from the current array, so changes the game made since the material
    /// was appended survive.
    /// </summary>
    private static void Detach(Renderer renderer)
    {
        var current = renderer.sharedMaterials;
        var kept = 0;
        for (var i = 0; i < current.Length; i++)
        {
            if (!IsOwnMaterial(current[i])) kept++;
        }
        if (kept == current.Length) return;
        var materials = new Material[kept];
        var index = 0;
        for (var i = 0; i < current.Length; i++)
        {
            var material = current[i];
            if (IsOwnMaterial(material)) continue;
            materials[index++] = material;
        }
        renderer.sharedMaterials = new Il2CppReferenceArray<Material>(materials);
    }

    private void Fault(string reason)
    {
        if (_faulted) return;
        _faulted = true;
        _log("player motion vectors stopped for this session: " + reason);
    }
}
