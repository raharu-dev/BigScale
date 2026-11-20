using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;

namespace BigScale;

/// <summary>
/// Owns the full-resolution reconstruction target. The texture mirrors the scene colour's
/// dimension, slice count and per-eye size so XR submission keeps its own allocation, and it is
/// only rebuilt when the descriptor actually changes.
///
/// A retired texture is never destroyed on a frame counter: every queued packet carries the tag of
/// the generation it wrote, and the generation is released only after the native side reports that
/// packet's GPU work completed.
/// </summary>
internal sealed class OutputResources
{
    private sealed class Generation
    {
        internal uint Tag;
        internal RTHandle Handle = null!;
        internal RenderTexture Texture = null!;
        internal int InFlight;
        internal bool Retired;
        internal string Name = "";
    }

    private readonly List<Generation> _generations = new();
    // Reused by Collect so a settlement pass does not allocate. Filled and cleared under the gate;
    // the release loop runs outside it.
    private readonly List<Generation> _detached = new();
    // Recording happens on the render callback while settlement happens on Update, so the generation
    // table is guarded. Unity object creation/destruction always happens outside the lock.
    private readonly object _gate = new();
    private Generation? _current;
    private Generation? _post;
    private readonly List<Generation> _retiredPosts = new();
    private int _postWidth;
    private int _postHeight;
    private int _postSlices;
    private TextureDimension _postDimension;
    private VRTextureUsage _postVrUsage;
    private GraphicsFormat _postFormat;
    private uint _nextTag = 1;

    internal RenderTexture? Texture => _current?.Texture;
    internal RTHandle? Handle => _current?.Handle;
    internal RTHandle? PostHandle => _post?.Handle;
    internal uint Tag => _current?.Tag ?? 0;
    internal int Width { get; private set; }
    internal int Height { get; private set; }
    internal int Slices { get; private set; }
    internal TextureDimension Dimension { get; private set; } = TextureDimension.Tex2D;

    internal bool Matches(int width, int height, int slices, TextureDimension dimension)
    {
        lock (_gate) return _current != null && Width == width && Height == height && Slices == slices && Dimension == dimension;
    }

    /// <summary>Creates the texture when required. Returns true when a new generation was built.</summary>
    internal bool Ensure(int width, int height, int slices, TextureDimension dimension, VRTextureUsage vrUsage,
        out string? error)
    {
        error = null;
        if (Matches(width, height, slices, dimension)) return false;
        if (width <= 0 || height <= 0 || slices <= 0)
        {
            error = $"invalid output descriptor {width}x{height}x{slices}";
            return false;
        }

        RetireCurrent();
        var descriptor = new RenderTextureDescriptor(width, height, GraphicsFormat.R16G16B16A16_SFloat, 0)
        {
            msaaSamples = 1,
            useMipMap = false,
            autoGenerateMips = false,
            enableRandomWrite = true,
            bindMS = false,
            volumeDepth = slices,
            dimension = dimension,
            vrUsage = vrUsage,
            depthStencilFormat = GraphicsFormat.None,
            sRGB = false,
        };
        var texture = new RenderTexture(descriptor)
        {
            name = "BigScale.Output",
            filterMode = FilterMode.Bilinear,
            wrapMode = TextureWrapMode.Clamp,
        };
        if (!texture.Create())
        {
            error = "RenderTexture.Create returned false";
            UnityEngine.Object.Destroy(texture);
            return false;
        }

        var generation = new Generation
        {
            Tag = 0,
            Texture = texture,
            Handle = RTHandles.Alloc(texture, false),
            Name = $"{width}x{height}x{slices}",
        };
        lock (_gate)
        {
            generation.Tag = _nextTag++;
            _generations.Add(generation);
            _current = generation;
            Width = width;
            Height = height;
            Slices = slices;
            Dimension = dimension;
        }
        return true;
    }

    /// <summary>
    /// Ensures the full-resolution stock-post destination used when URP's own post attachment was
    /// allocated at the reduced render extent. It mirrors the attachment's format, slice count,
    /// dimension and XR usage so the stock UberPost pass writes exactly the same colour semantics,
    /// but at the full output size. Returns true when a new allocation was built.
    ///
    /// This texture is engine presentation state, not BigScale output: no native packet references it,
    /// so it is released with the rest of the output resources and never waits for a retired tag.
    /// </summary>
    internal bool EnsurePostTarget(int width, int height, int slices, TextureDimension dimension,
        VRTextureUsage vrUsage, GraphicsFormat format, out string? error)
    {
        error = null;
        if (width <= 0 || height <= 0 || slices <= 0)
        {
            error = $"invalid post-processing descriptor {width}x{height}x{slices}";
            return false;
        }
        if (format == GraphicsFormat.None || GraphicsFormatUtility.IsDepthFormat(format))
            format = GraphicsFormat.R16G16B16A16_SFloat;

        lock (_gate)
        {
            if (_post != null && _postWidth == width && _postHeight == height && _postSlices == slices &&
                _postDimension == dimension && _postVrUsage == vrUsage && _postFormat == format)
                return false;
        }

        var descriptor = new RenderTextureDescriptor(width, height, format, 0)
        {
            msaaSamples = 1,
            useMipMap = false,
            autoGenerateMips = false,
            enableRandomWrite = false,
            bindMS = false,
            volumeDepth = slices,
            dimension = dimension,
            vrUsage = vrUsage,
            depthStencilFormat = GraphicsFormat.None,
        };
        var texture = new RenderTexture(descriptor)
        {
            name = "BigScale.PostColor",
            filterMode = FilterMode.Bilinear,
            wrapMode = TextureWrapMode.Clamp,
        };
        if (!texture.Create())
        {
            error = "RenderTexture.Create returned false for the full-resolution post target";
            UnityEngine.Object.Destroy(texture);
            return false;
        }

        var generation = new Generation
        {
            Texture = texture,
            Handle = RTHandles.Alloc(texture, false),
            Name = $"post {width}x{height}x{slices}",
        };
        Generation? previous;
        lock (_gate)
        {
            previous = _post;
            _post = generation;
            _postWidth = width;
            _postHeight = height;
            _postSlices = slices;
            _postDimension = dimension;
            _postVrUsage = vrUsage;
            _postFormat = format;
            // A graph recorded earlier this frame may still reference the old attachment. Detach it
            // now and let the main-thread collection release it after the frame has executed.
            if (previous != null) _retiredPosts.Add(previous);
        }
        return true;
    }

    /// <summary>
    /// Records that a recorded pass references this generation for <paramref name="packets"/> queued
    /// packet(s). The obligation is created at record time, so a resize cannot destroy a texture the
    /// graph already imported even if the callback is delayed.
    /// </summary>
    internal void Reserve(uint tag, int packets)
    {
        if (tag == 0 || packets <= 0) return;
        lock (_gate)
        {
            foreach (var generation in _generations)
            {
                if (generation.Tag != tag) continue;
                generation.InFlight += packets;
                return;
            }
        }
    }

    /// <summary>Undoes a reservation when the pass could not be recorded.</summary>
    internal void Rollback(uint tag, int packets)
    {
        if (tag == 0 || packets <= 0) return;
        // Decrement only: this can run on the recording thread, so Unity destruction is left to
        // <see cref="Collect"/> on the owning thread.
        lock (_gate)
        {
            foreach (var generation in _generations)
            {
                if (generation.Tag != tag) continue;
                generation.InFlight = System.Math.Max(0, generation.InFlight - packets);
                return;
            }
        }
    }

    /// <summary>Settles one obligation: a retired packet, or a packet the bridge refused.</summary>
    internal void Completed(uint tag)
    {
        if (tag == 0) return;
        Generation? detached = null;
        lock (_gate)
        {
            foreach (var generation in _generations)
            {
                if (generation.Tag != tag) continue;
                if (generation.InFlight > 0) generation.InFlight--;
                detached = DetachIfIdle(generation);
                break;
            }
        }
        ReleaseUnity(detached);
    }

    /// <summary>Removes an idle, retired generation from the table. Caller holds the gate.</summary>
    private Generation? DetachIfIdle(Generation generation)
    {
        if (!generation.Retired || generation.InFlight > 0) return null;
        if (!_generations.Remove(generation)) return null;
        if (_current == generation) _current = null;
        return generation;
    }

    /// <summary>Frees a detached generation. Never called with the gate held.</summary>
    private static void ReleaseUnity(Generation? generation)
    {
        if (generation == null) return;
        generation.Handle.Release();
        if (generation.Texture != null) UnityEngine.Object.Destroy(generation.Texture);
    }

    private void ReleaseIfIdle(Generation generation) => ReleaseUnity(DetachIdle(generation));

    private Generation? DetachIdle(Generation generation)
    {
        lock (_gate) return DetachIfIdle(generation);
    }

    private void RetireCurrent()
    {
        Generation? retired = null;
        lock (_gate)
        {
            if (_current == null) return;
            _current.Retired = true;
            retired = _current;
            _current = null;
        }
        ReleaseIfIdle(retired);
    }

    /// <summary>Releases generations whose GPU work already completed. Never uses a frame counter.</summary>
    internal void Collect()
    {
        var detached = _detached;
        lock (_gate)
        {
            detached.Clear();
            for (var i = _generations.Count - 1; i >= 0; i--)
            {
                var idle = DetachIfIdle(_generations[i]);
                if (idle != null) detached.Add(idle);
            }
            detached.AddRange(_retiredPosts);
            _retiredPosts.Clear();
        }
        foreach (var generation in detached) ReleaseUnity(generation);
        detached.Clear();
    }

    /// <summary>
    /// Full release, only after the native side confirmed the GPU finished. Retains anything still
    /// reported in flight rather than freeing memory queued work may still read.
    /// </summary>
    internal void Release(bool force)
    {
        RetireCurrent();
        Generation? post;
        List<Generation> retiredPosts;
        lock (_gate)
        {
            post = _post;
            _post = null;
            _postWidth = 0;
            _postHeight = 0;
            _postSlices = 0;
            retiredPosts = new List<Generation>(_retiredPosts);
            _retiredPosts.Clear();
        }
        ReleaseUnity(post);
        foreach (var retiredPost in retiredPosts) ReleaseUnity(retiredPost);
        var detached = new List<Generation>();
        lock (_gate)
        {
            for (var i = _generations.Count - 1; i >= 0; i--)
            {
                var generation = _generations[i];
                if (!force && generation.InFlight > 0) continue;
                generation.Retired = true;
                generation.InFlight = 0;
                var idle = DetachIfIdle(generation);
                if (idle != null) detached.Add(idle);
            }
            _current = null;
            Width = 0;
            Height = 0;
            Slices = 0;
        }
        foreach (var generation in detached) ReleaseUnity(generation);
    }
}
