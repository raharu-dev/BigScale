using System;
using System.Collections.Generic;
using BigScale.Core;
using Il2CppInterop.Runtime.InteropTypes.Arrays;
using UnityEngine;
using UnityEngine.Rendering;

namespace BigScale;

/// <summary>One mask draw: a renderer's submesh with the material and forward pass the game draws it with.</summary>
internal readonly struct CharacterMaskDraw
{
    internal CharacterMaskDraw(Renderer renderer, GameObject owner, Material material, int submesh, int pass)
    {
        Renderer = renderer;
        Owner = owner;
        Material = material;
        Submesh = submesh;
        Pass = pass;
    }

    internal readonly Renderer Renderer;
    internal readonly GameObject Owner;
    internal readonly Material Material;
    internal readonly int Submesh;
    internal readonly int Pass;
}

/// <summary>
/// Owns the character draw inventory and the fill resources the neural-rendering character mask uses.
///
/// The mask itself is recorded by a render-graph pass elsewhere; this class answers two questions for
/// it: which submeshes draw a character, with which material and forward pass, and what material and
/// mesh turn their depth coverage into the mask. It never changes renderer state, unlike
/// <see cref="PlayerMotionVectors"/>: the renderers are read, matched against
/// <see cref="CharacterMaskPolicy"/> and nothing else.
///
/// Everything here runs on the main thread, from the render coordinator's tick. The inventory is
/// enumerated from <c>PlayerCharacter.allPlayerCharacters</c>, the same source
/// <see cref="PlayerMotionVectors"/> uses: the player characters are the only renderers this mask can
/// cover, so walking their hierarchies is exact, where a whole-scene renderer scan was a periodic
/// main-thread spike. The result is stable for long stretches of a scene, so a scan happens at most
/// once every <see cref="RescanFrames"/> frames; a camera or scene change calls
/// <see cref="Invalidate"/> to force the next tick to rescan. While neural rendering is off or
/// character masking is disabled the cached snapshot is kept and nothing is scanned or allocated, so
/// re-enabling the switch does not wait for a new scan.
/// </summary>
internal sealed class CharacterMaskSource : IDisposable
{
    /// <summary>Frames between full renderer scans. Matches the player-motion inventory's cadence.</summary>
    private const int RescanFrames = 120;

    // Hidden/Internal-Colored is the engine's own unlit shader and exposes its depth test as the
    // _ZTest property, which the fill depends on.
    private const string ColoredShaderName = "Hidden/Internal-Colored";
    private const string MaterialName = "BigScale.CharacterMask";
    private const string FillMeshName = "BigScale.CharacterMaskFill";

    // A scan slower than this is worth a log line, reported at most once per SlowScanReportFrames so
    // a struggling scene cannot flood the log.
    private const double SlowScanMilliseconds = 2.0;
    private const int SlowScanReportFrames = 600;

    /// <summary>Per-shader pass tags; a shader's LightMode tags are fixed for its lifetime.</summary>
    private sealed class ShaderPasses
    {
        internal string?[] Modes = Array.Empty<string?>();
    }

    /// <summary>Per-material pass enablement; only the array is reused, the selected pass is not cached.</summary>
    private sealed class MaterialPass
    {
        internal bool[] Enabled = Array.Empty<bool>();
    }

    private readonly ModSettings _settings;
    private readonly Action<string> _log;
    private readonly List<CharacterMaskDraw> _drawsCollected = new();
    // Local renderers included by the last scan; read only for the inventory log.
    private readonly List<Renderer> _localCollected = new();
    private readonly List<string> _extras = new();
    private readonly Dictionary<int, MaterialPass> _materialPasses = new();
    private readonly Dictionary<int, ShaderPasses> _shaderPasses = new();
    private readonly System.Diagnostics.Stopwatch _scanWatch = new();

    private CharacterMaskDraw[] _draws = Array.Empty<CharacterMaskDraw>();
    private Material? _material;
    private Mesh? _fillMesh;
    private bool _materialReported;
    private bool _meshReported;
    private bool _scanReported;
    private bool _disposed;
    private bool _rescanRequested;
    private int _nextScanFrame;
    private int _renderersIncluded;
    private int _localIncluded;
    private int _noPassSubmeshes;
    private int _lastRenderers = -1;
    private int _lastDraws = -1;
    private int _lastLocal = -1;
    private int _lastNoPass = -1;
    // The last player-character-read failure, so a repeating failure is reported once.
    private string? _lastCharacterListFailure;
    private int _lastSlowScanFrame = -SlowScanReportFrames;

    internal CharacterMaskSource(ModSettings settings, Action<string> log)
    {
        _settings = settings;
        _log = log;
    }

    /// <summary>
    /// The per-submesh character draws as of the last scan. Never null; empty until the first scan and
    /// after <see cref="Dispose"/>. A stale snapshot while masking is off is intentional: the
    /// coordinator decides whether the mask is used.
    /// </summary>
    internal CharacterMaskDraw[] Draws => _draws;

    /// <summary>The fill material the mask pass converts depth coverage with, or null when unavailable.</summary>
    internal Material? Material => _material;

    /// <summary>The fullscreen triangle the fill material is drawn with, or null when unavailable.</summary>
    internal Mesh? FillMesh => _fillMesh;

    /// <summary>
    /// Rebuilds the snapshot when neural rendering, the master switch, the upscaler and character
    /// masking are all on. <paramref name="force"/> skips the 120-frame gate but not the enabled check.
    /// </summary>
    internal void Tick(bool force = false)
    {
        if (_disposed) return;
        if (!EffectivelyEnabled()) return;
        if (!force && !_rescanRequested && Time.frameCount < _nextScanFrame) return;

        _rescanRequested = false;
        _scanWatch.Restart();
        try
        {
            Rebuild();
        }
        catch (Exception exception)
        {
            if (!_scanReported)
            {
                _scanReported = true;
                _log("character mask: scan failed: " + exception.Message);
            }
        }
        finally
        {
            _scanWatch.Stop();
            // A failure must not turn the scan into a per-frame cost; the next attempt is a full
            // rescan because the snapshot was left untouched.
            _nextScanFrame = Time.frameCount + RescanFrames;
            ReportSlowScan();
        }
    }

    /// <summary>
    /// Drops the cached draws so the next tick rescans, even before the frame gate. Called when the
    /// primary camera or scene changes and the previous inventory no longer describes the view.
    /// </summary>
    internal void Invalidate()
    {
        if (_disposed) return;
        _draws = Array.Empty<CharacterMaskDraw>();
        _nextScanFrame = 0;
        _rescanRequested = true;
    }

    /// <summary>Destroys the cached material and mesh. Idempotent; the source is inert afterwards.</summary>
    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        if (_material != null)
        {
            UnityEngine.Object.Destroy(_material);
            _material = null;
        }
        if (_fillMesh != null)
        {
            UnityEngine.Object.Destroy(_fillMesh);
            _fillMesh = null;
        }
        _draws = Array.Empty<CharacterMaskDraw>();
        _drawsCollected.Clear();
        _localCollected.Clear();
        _extras.Clear();
    }

    private bool EffectivelyEnabled()
    {
        try
        {
            return _settings.DLSS5Enabled.Value &&
                   _settings.DLSS5MasterEnabled.Value &&
                   _settings.Upscaler.Value != UpscalerKind.Off &&
                   _settings.DLSS5CharacterMask.Value;
        }
        catch (Exception)
        {
            // The entries are not bound yet during plugin load; masking is simply off.
            return false;
        }
    }

    private void Rebuild()
    {
        // Instance IDs are recycled when materials and shaders are destroyed, so a long session
        // accumulates dead keys. A character scene uses a handful of each, so dropping the caches is
        // cheaper than tracking destruction; the next rebuild repopulates them.
        if (_materialPasses.Count > 256 || _shaderPasses.Count > 64)
        {
            _materialPasses.Clear();
            _shaderPasses.Clear();
        }

        ParseExtras();
        _drawsCollected.Clear();
        _localCollected.Clear();
        _renderersIncluded = 0;
        _localIncluded = 0;
        _noPassSubmeshes = 0;

        // The local body is masked like any player. Coverage comes from each material's own forward
        // pass, so geometry the game hides in its shader, or that writes no depth, never reaches the
        // mask. A flat silhouette of the local body drew a pill.
        Transform? localRoot = null;
        try
        {
            var character = WorldManager.localPlayerCharacter;
            var body = character != null ? character.gameObject : null;
            if (body != null) localRoot = body.transform;
        }
        catch (Exception)
        {
            // The world may be loading, unloading or between scenes; the local body is found again by
            // the next rescan.
        }

        // The player characters are the only renderers the mask can cover, so the inventory walks
        // their hierarchies: the same source PlayerMotionVectors uses. Keeping the previous inventory
        // and retrying in RescanFrames is preferable to scanning the whole scene on the main thread.
        Il2CppSystem.Collections.Generic.List<PlayerCharacter> characters;
        try
        {
            characters = PlayerCharacter.allPlayerCharacters;
        }
        catch (Exception exception)
        {
            ReportCharacterListUnavailable(exception.Message);
            _nextScanFrame = Time.frameCount + RescanFrames;
            return;
        }
        if (characters == null || characters.Count == 0)
        {
            // An empty list is the normal case during a load, and there is nothing to mask without one.
            ReportCharacterListUnavailable("empty list");
            _nextScanFrame = Time.frameCount + RescanFrames;
            return;
        }

        for (var i = 0; i < characters.Count; i++)
        {
            var character = characters[i];
            if (character == null) continue;
            try
            {
                var body = character.gameObject;
                if (body == null) continue;
                var renderers = body.GetComponentsInChildren<Renderer>(true);
                if (renderers == null) continue;
                foreach (var renderer in renderers)
                {
                    if (renderer != null) ScanRenderer(renderer, localRoot);
                }
            }
            catch (Exception)
            {
                // A character destroyed or unreadable mid-scan contributes nothing; the next scan
                // picks up whatever replaced it.
            }
        }

        _draws = _drawsCollected.ToArray();
        EnsureResources();
        Report();
    }

    /// <summary>
    /// Reports a scan that could not read the player character list. Each distinct failure is logged
    /// once; the published inventory is kept and the next attempt is a full rescan.
    /// </summary>
    private void ReportCharacterListUnavailable(string message)
    {
        if (string.Equals(_lastCharacterListFailure, message, StringComparison.Ordinal)) return;
        _lastCharacterListFailure = message;
        _log("character mask: player character list unavailable: " + message);
    }

    /// <summary>
    /// Applies the inclusion rules to one renderer of a player character: visible, opaque, in the
    /// scene, and either a character shader or part of the local player's own hierarchy.
    /// </summary>
    private void ScanRenderer(Renderer renderer, Transform? localRoot)
    {
        GameObject? owner;
        try
        {
            owner = renderer.gameObject;
        }
        catch (Exception)
        {
            return;
        }
        if (owner == null) return;

        bool inScene;
        try
        {
            inScene = owner.scene.IsValid();
        }
        catch (Exception)
        {
            return;
        }

        var maskable = inScene && IsMaskRenderer(renderer);
        var local = maskable && localRoot != null && IsDescendant(renderer.transform, localRoot);
        var include = maskable;

        if (include)
        {
            // One broken material or shader reference must not abort the whole scan.
            string? shaderName;
            try
            {
                var material = renderer.sharedMaterial;
                var shader = material != null ? material.shader : null;
                shaderName = shader != null ? shader.name : null;
            }
            catch (Exception)
            {
                shaderName = null;
            }
            include = CharacterMaskPolicy.IsCharacterShader(shaderName, _extras);
        }
        // The local body bypasses the shader-name filter: every visible mesh under the local player is
        // the player.
        if (!include && local) include = true;

        if (include) Collect(renderer, owner, local);
    }

    /// <summary>
    /// Logs a scan that took long enough to matter, at most once per <see cref="SlowScanReportFrames"/>.
    /// The stopwatch is only read here; the scan path itself carries no timing work.
    /// </summary>
    private void ReportSlowScan()
    {
        if (_scanWatch.Elapsed.TotalMilliseconds < SlowScanMilliseconds) return;
        if (Time.frameCount < _lastSlowScanFrame + SlowScanReportFrames) return;
        _lastSlowScanFrame = Time.frameCount;
        _log($"character mask: scan took {_scanWatch.Elapsed.TotalMilliseconds:F1} ms for {_renderersIncluded} renderer(s)");
    }

    /// <summary>Adds one draw per drawable submesh of an included renderer.</summary>
    private void Collect(Renderer renderer, GameObject owner, bool local)
    {
        try
        {
            var materials = renderer.sharedMaterials;
            // Materials past the submesh count are extra passes over the last submesh, such as the
            // motion-only material PlayerMotionVectors appends; they are not character coverage.
            var count = Math.Min(SubmeshCount(renderer), materials.Length);
            for (var submesh = 0; submesh < count; submesh++)
            {
                var material = materials[submesh];
                if (material == null || !IsOpaqueMaskMaterial(material)) continue;
                if (local && IsLocalNose(material)) continue;
                var pass = ForwardPass(material);
                if (pass < 0)
                {
                    _noPassSubmeshes++;
                    continue;
                }
                _drawsCollected.Add(new CharacterMaskDraw(renderer, owner, material, submesh, pass));
            }
        }
        catch (Exception)
        {
            // A renderer destroyed or broken mid-scan contributes nothing.
            return;
        }
        _renderersIncluded++;
        if (local)
        {
            _localIncluded++;
            _localCollected.Add(renderer);
        }
    }

    /// <summary>
    /// Reads the user's extra shader names from the config on every rebuild: the file may be edited
    /// while the game runs, and this is not a per-frame path.
    /// </summary>
    private void ParseExtras()
    {
        _extras.Clear();
        string? raw;
        try
        {
            raw = _settings.DLSS5CharacterMaskShaders.Value;
        }
        catch (Exception)
        {
            return;
        }
        if (string.IsNullOrEmpty(raw)) return;
        var parts = raw.Split(',');
        for (var i = 0; i < parts.Length; i++)
        {
            var candidate = parts[i].Trim();
            if (candidate.Length == 0) continue;
            _extras.Add(candidate);
        }
    }

    /// <summary>
    /// True only for a visible, opaque character mesh. Inactive LODs, shadow-only proxies,
    /// transparent blob shadows, decals, effects, disabled renderers and non-mesh renderers must not
    /// contribute coverage to the mask.
    /// </summary>
    private static bool IsMaskRenderer(Renderer renderer)
    {
        try
        {
            if (renderer == null || !renderer.enabled || renderer.forceRenderingOff) return false;
            var owner = renderer.gameObject;
            if (owner == null || !owner.activeInHierarchy) return false;
            if (renderer.shadowCastingMode == ShadowCastingMode.ShadowsOnly) return false;
            if (renderer.TryCast<MeshRenderer>() == null &&
                renderer.TryCast<SkinnedMeshRenderer>() == null) return false;
            return IsOpaqueMaskMaterial(renderer.sharedMaterial);
        }
        catch (Exception)
        {
            return false;
        }
    }

    /// <summary>
    /// The local player hierarchy also contains a transparent blob shadow and occasional decals or
    /// effect quads. Rasterising those into the coverage mask leaves unedited pill-shaped patches on
    /// the ground, so only opaque character materials may enter the mask.
    /// </summary>
    private static bool IsOpaqueMaskMaterial(Material? material)
    {
        if (material == null) return false;
        if (material.renderQueue > PlayerMotionPolicy.OpaqueQueueMax) return false;
        var shader = material.shader;
        var shaderName = shader != null ? shader.name : string.Empty;
        return shaderName.IndexOf("shadow", StringComparison.OrdinalIgnoreCase) < 0 &&
               shaderName.IndexOf("blob", StringComparison.OrdinalIgnoreCase) < 0 &&
               shaderName.IndexOf("decal", StringComparison.OrdinalIgnoreCase) < 0 &&
               shaderName.IndexOf("projector", StringComparison.OrdinalIgnoreCase) < 0;
    }

    private static bool IsLocalNose(Material material)
    {
        var shader = material.shader;
        return shader != null &&
               string.Equals(shader.name, CharacterMaskPolicy.LocalNoseShaderName, StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// The pass URP's forward rendering draws this material with, or -1.
    ///
    /// The per-renderer scan used to allocate a tag and an enabled array per material; both are cached
    /// here instead. Only the arrays are reused: enablement can change at runtime, so the pass index is
    /// resolved on every call and never cached.
    /// </summary>
    private int ForwardPass(Material material)
    {
        try
        {
            var shader = material.shader;
            if (shader == null) return -1;
            var lightMode = new ShaderTagId("LightMode");
            var passes = shader.passCount;
            if (passes <= 0) return -1;

            var shaderKey = shader.GetInstanceID();
            _shaderPasses.TryGetValue(shaderKey, out var shaderPasses);
            if (shaderPasses == null || shaderPasses.Modes.Length != passes)
            {
                // Built completely before it is cached, so a failure below cannot store a partial entry.
                var modes = new string?[passes];
                for (var i = 0; i < passes; i++)
                {
                    var name = shader.FindPassTagValue(i, lightMode).name;
                    modes[i] = string.IsNullOrEmpty(name) ? null : name;
                }
                shaderPasses = new ShaderPasses { Modes = modes };
                _shaderPasses[shaderKey] = shaderPasses;
            }

            var materialKey = material.GetInstanceID();
            _materialPasses.TryGetValue(materialKey, out var materialPass);
            if (materialPass == null)
            {
                materialPass = new MaterialPass();
                _materialPasses[materialKey] = materialPass;
            }
            var enabled = materialPass.Enabled;
            if (enabled.Length != passes)
            {
                enabled = new bool[passes];
                materialPass.Enabled = enabled;
            }

            var tags = shaderPasses.Modes;
            for (var i = 0; i < passes; i++)
            {
                var mode = tags[i];
                var untagged = mode == null;
                enabled[i] = material.GetShaderPassEnabled(untagged ? "SRPDefaultUnlit" : mode!);
            }
            return CharacterMaskPolicy.SelectForwardPass(tags, enabled);
        }
        catch (Exception)
        {
            return -1;
        }
    }

    /// <summary>The mesh submesh count for a renderer, without allocating a materials array.</summary>
    private static int SubmeshCount(Renderer renderer)
    {
        var skinned = renderer.TryCast<SkinnedMeshRenderer>();
        if (skinned != null)
        {
            Mesh? mesh = skinned.sharedMesh;
            return mesh != null ? mesh.subMeshCount : 0;
        }
        var filter = renderer.GetComponent<MeshFilter>();
        Mesh? shared = filter != null ? filter.sharedMesh : null;
        return shared != null ? shared.subMeshCount : 0;
    }

    private static bool IsDescendant(Transform? transform, Transform root)
    {
        try
        {
            for (var current = transform; current != null; current = current.parent)
            {
                if (current == root) return true;
            }
        }
        catch (Exception)
        {
            // A transform destroyed mid-scan is not part of the character.
        }
        return false;
    }

    /// <summary>
    /// Creates the fill material and fullscreen triangle lazily. Each failure is reported once, so a
    /// build without the shader degrades to no mask instead of a log line per scan.
    /// </summary>
    private void EnsureResources()
    {
        EnsureMaterial();
        EnsureFillMesh();
    }

    private void EnsureMaterial()
    {
        if (_material != null || _materialReported) return;
        Material? material = null;
        try
        {
            var shader = Shader.Find(ColoredShaderName);
            if (shader != null)
            {
                material = new Material(shader)
                {
                    name = MaterialName,
                    hideFlags = HideFlags.HideAndDontSave,
                };
            }
            // Without a _ZTest property the fill would use the shader's default LEqual and invert the
            // mask, so no other shader can stand in.
            if (material == null || !material.HasProperty("_ZTest"))
            {
                if (material != null) UnityEngine.Object.Destroy(material);
                _materialReported = true;
                _log("character mask: Hidden/Internal-Colored is unavailable; character masking is unavailable");
                return;
            }

            material.SetColor("_Color", Color.white);
            // The fill passes only where the private depth differs from the far value it was cleared
            // to, which is where a character fragment wrote depth.
            material.SetFloat("_Cull", (float)CullMode.Off);
            material.SetFloat("_ZWrite", 0f);
            material.SetFloat("_ZTest", (float)CompareFunction.NotEqual);
            material.SetFloat("_SrcBlend", (float)BlendMode.One);
            material.SetFloat("_DstBlend", (float)BlendMode.Zero);
            material.SetFloat("_ZBias", 0f);
            _material = material;
        }
        catch (Exception exception)
        {
            if (material != null) UnityEngine.Object.Destroy(material);
            _materialReported = true;
            _log("character mask: material creation failed: " + exception.Message);
        }
    }

    private void EnsureFillMesh()
    {
        if (_fillMesh != null || _meshReported) return;
        try
        {
            // One triangle covering the viewport under identity view and projection. z = 1 is the far
            // plane after Unity's GPU projection conversion (0 with reversed-Z, 1 otherwise), the same
            // value ClearRenderTarget writes.
            var mesh = new Mesh
            {
                name = FillMeshName,
                hideFlags = HideFlags.HideAndDontSave,
            };
            mesh.vertices = new Il2CppStructArray<Vector3>(new[]
            {
                new Vector3(-1f, -1f, 1f), new Vector3(3f, -1f, 1f), new Vector3(-1f, 3f, 1f),
            });
            mesh.triangles = new Il2CppStructArray<int>(new[] { 0, 1, 2 });
            mesh.bounds = new Bounds(Vector3.zero, new Vector3(8f, 8f, 8f));
            _fillMesh = mesh;
        }
        catch (Exception exception)
        {
            _meshReported = true;
            _log("character mask: fill mesh creation failed: " + exception.Message);
        }
    }

    private void Report()
    {
        var draws = _draws.Length;
        if (_renderersIncluded == _lastRenderers && draws == _lastDraws && _localIncluded == _lastLocal &&
            _noPassSubmeshes == _lastNoPass)
        {
            return;
        }
        _lastRenderers = _renderersIncluded;
        _lastDraws = draws;
        _lastLocal = _localIncluded;
        _lastNoPass = _noPassSubmeshes;
        _log($"character mask: {_renderersIncluded} renderer(s), {draws} draw(s), local player {_localIncluded}, {_noPassSubmeshes} submesh(es) without a forward pass");

        for (var i = 0; i < _localCollected.Count; i++)
        {
            try
            {
                _log(DescribeLocal(_localCollected[i]));
            }
            catch (Exception)
            {
                _log("character mask local renderer: unreadable");
            }
        }
    }

    private string DescribeLocal(Renderer renderer)
    {
        var skinned = renderer.TryCast<SkinnedMeshRenderer>() != null;
        var materials = renderer.sharedMaterials;
        var count = Math.Min(SubmeshCount(renderer), materials.Length);
        var details = new string[count];
        for (var submesh = 0; submesh < count; submesh++)
        {
            var material = materials[submesh];
            if (material == null)
            {
                details[submesh] = "null";
                continue;
            }
            var shader = material.shader;
            var shaderName = shader != null ? shader.name : "<no shader>";
            details[submesh] = IsLocalNose(material)
                ? $"{shaderName} excluded (local nose)"
                : $"{shaderName} q{material.renderQueue} pass {ForwardPass(material)}";
        }
        return $"character mask local renderer: '{renderer.name}' {(skinned ? "skinned" : "mesh")} layer {renderer.gameObject.layer}: {string.Join("; ", details)}";
    }
}
