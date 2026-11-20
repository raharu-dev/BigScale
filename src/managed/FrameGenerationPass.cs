using System;
using System.Threading;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.RenderGraphModule;
using UnityEngine.Rendering.Universal;
using PassData = UnityEngine.Rendering.Universal.PostProcessPassRenderGraph.DoFGaussianPassData;

namespace BigScale;

/// <summary>
/// Where the provider's HUD-less colour comes from, which decides how strictly its extent is checked.
/// </summary>
internal enum HudlessRequirement
{
    /// <summary>The provider keeps its own UI handling (XeSS): no HUD-less plane is supplied.</summary>
    None,

    /// <summary>The pre-overlay boundary colour, which is always at the output extent.</summary>
    Overlay,

    /// <summary>
    /// The reconstruction output published by the post-processing path. It is output sized, like the
    /// boundary colour: the native conversion copies texels into an output-sized plane.
    /// </summary>
    CameraColor,
}

/// <summary>
/// Records one frame-generation capture into the active URP render graph.
///
/// The pass data is the game's own compiled <c>DoFGaussianPassData</c>, exactly like the
/// reconstruction pass: IL2CPP can only instantiate a pass-data type the graph knows, so a managed
/// type declared here would fail before the callback ever ran. The generation token travels in
/// <c>downsample</c>, depth in <c>depthTexture</c> and motion in <c>halfCoCTexture</c>; every other
/// field is reset on each recording because the graph pools the instance.
///
/// Each lease owns one cached callback plus the captured frame and render event. The callback takes
/// the frame snapshot under the pass gate, submits one native capture packet and issues the native
/// render event, so depth and motion are copied on the render thread while the resources are still
/// alive. The matching Present supplies the colour; nothing here touches the swapchain.
/// </summary>
internal sealed class FrameGenerationPass
{
    private const int LeaseCount = 8;
    private const int MaxLeaseFrames = 8;

    private sealed class Lease
    {
        internal readonly int Index;
        // Built on first successful recording, never in the constructor: an IL2CPP delegate
        // conversion at plugin load must not be able to abort the plugin before a single frame.
        internal BaseRenderFunc<PassData, UnsafeGraphContext>? Callback;
        // Snapshot copied under the pass gate so the callback never reads fields Record may rewrite.
        internal NativeFg.Frame Frame;
        internal IntPtr RenderEvent;
        internal int Generation;
        internal int LeasedFrame;
        internal bool Busy;
        internal HudlessRequirement Hudless;
        // Classified by the caller: an imported game backbuffer has no graph descriptor and must be
        // captured through its command-buffer binding, never through a texture pointer.
        internal bool BoundHudless;
        internal Action<string>? Failure;

        private readonly FrameGenerationPass _owner;

        internal Lease(FrameGenerationPass owner, int index)
        {
            _owner = owner;
            Index = index;
        }

        /// <summary>
        /// Builds the render callback once, on first use. The conversion is the operation that could
        /// fail during IL2CPP delegate marshalling, so the caller invokes this inside its protected
        /// recording scope rather than in the constructor at plugin load.
        /// </summary>
        internal BaseRenderFunc<PassData, UnsafeGraphContext> EnsureCallback() =>
            Callback ??= new Action<PassData, UnsafeGraphContext>(Execute);

        private void Execute(PassData data, UnsafeGraphContext context)
        {
            NativeFg.Frame frame;
            IntPtr renderEvent;
            HudlessRequirement hudless;
            bool boundHudless;
            lock (_owner._gate)
            {
                // A graph recorded for an abandoned lease must not read its replacement's packet.
                if (!Busy || data.downsample != Generation) return;
                frame = Frame;
                renderEvent = RenderEvent;
                hudless = Hudless;
                boundHudless = BoundHudless;
            }
            try
            {
                var depth = Resolve(data.depthTexture);
                var motion = Resolve(data.halfCoCTexture);
                if (depth == IntPtr.Zero || motion == IntPtr.Zero)
                {
                    Failure?.Invoke("frame generation needs URP depth and motion vectors; this camera " +
                                    "did not provide them.");
                    return;
                }
                if (hudless != HudlessRequirement.None)
                {
                    frame.Hudless = boundHudless ? IntPtr.Zero : Resolve(data.sourceTexture);
                    if (boundHudless)
                        frame.Flags |= NativeFg.FlagBoundRenderTarget;
                    else if (frame.Hudless == IntPtr.Zero)
                    {
                        Failure?.Invoke("pre-overlay colour was not captured");
                        return;
                    }
                }
                frame.Depth = depth;
                frame.Motion = motion;
                var commandBuffer = CommandBufferHelpers.GetNativeCommandBuffer(context.cmd);
                var queued = NativeFg.SubmitFrame(ref frame);
                if (queued == IntPtr.Zero)
                {
                    // A full-but-healthy transport drops the capture; only a reported native fault
                    // is a capture failure.
                    var status = new NativeFg.Status();
                    if (NativeFg.GetStatus(ref status) != 0 &&
                        status.State is FrameGenerationCoordinator.NativeFgState.Faulted
                            or FrameGenerationCoordinator.NativeFgState.Unsupported)
                    {
                        Failure?.Invoke($"the native frame-generation bridge rejected camera {frame.CameraId}: " +
                                        status.ReasonText());
                    }
                    return;
                }
                // The bind executes before the event on this same command buffer, preserving the
                // pre-overlay colour even when Unity exposes only a built-in target identifier.
                if (boundHudless)
                    context.cmd.SetRenderTarget((RenderTargetIdentifier)data.sourceTexture,
                        RenderBufferLoadAction.Load, RenderBufferStoreAction.Store);
                commandBuffer.IssuePluginEventAndData(renderEvent, 2, queued);
            }
            catch (Exception exception)
            {
                Failure?.Invoke("frame-generation capture failed: " + exception.Message);
            }
            finally
            {
                // Only the recording that still owns the token may release the lease; a stale lease
                // has already been handed to a newer recording.
                lock (_owner._gate)
                {
                    if (data.downsample == Generation) Busy = false;
                }
            }
        }

        /// <summary>
        /// Resolves a graph handle to its native texture, or zero when the handle owns no resource. Any
        /// texture type is accepted: the capture needs the D3D11 resource, and restricting this to
        /// RenderTexture dropped handles URP wraps in another type.
        /// </summary>
        private static IntPtr Resolve(TextureHandle handle)
        {
            if (!handle.IsValid()) return IntPtr.Zero;
            try
            {
                Texture? texture = handle;
                return texture == null ? IntPtr.Zero : texture.GetNativeTexturePtr();
            }
            catch
            {
                return IntPtr.Zero;
            }
        }
    }

    private readonly Lease[] _leases = new Lease[LeaseCount];
    private readonly Action<string> _log;
    // Leases are acquired on Update and released by render callbacks, so lease state is guarded.
    // The gate is never held across a Unity or native call.
    private readonly object _gate = new();
    private int _cursor;
    private int _nextGeneration;
    private string? _pendingFailure;

    internal FrameGenerationPass(Action<string> log)
    {
        _log = log;
        for (var i = 0; i < LeaseCount; i++)
        {
            _leases[i] = new Lease(this, i) { Failure = PublishFailure };
        }
    }

    /// <summary>True when a capture packet can still be recorded this frame.</summary>
    internal bool HasFreeLease
    {
        get
        {
            lock (_gate)
            {
                foreach (var lease in _leases)
                    if (!lease.Busy) return true;
                return false;
            }
        }
    }

    internal string? TakeFailure() => Interlocked.Exchange(ref _pendingFailure, null);

    private void PublishFailure(string reason) =>
        Interlocked.CompareExchange(ref _pendingFailure, reason, null);

    internal void AbandonStaleLeases(int frame)
    {
        lock (_gate)
        {
            foreach (var lease in _leases)
            {
                if (!lease.Busy) continue;
                if (frame - lease.LeasedFrame <= MaxLeaseFrames) continue;
                lease.Generation = ++_nextGeneration;
                lease.Busy = false;
                _log($"released a stale frame-generation lease (lease {lease.Index}, frame {lease.LeasedFrame}).");
            }
        }
    }

    /// <summary>Records one capture pass. Returns false when no lease or handle is available.</summary>
    internal bool Record(RenderGraph renderGraph, TextureHandle color, TextureHandle depth, TextureHandle motion,
        NativeFg.Frame frame, IntPtr renderEvent, HudlessRequirement hudless, bool boundHudless)
    {
        if (hudless == HudlessRequirement.None) boundHudless = false;
        if (!depth.IsValid() || !motion.IsValid())
        {
            PublishFailure("frame generation needs URP depth and motion vectors; this camera did not provide them.");
            return false;
        }
        if (hudless != HudlessRequirement.None)
        {
            if (!color.IsValid())
            {
                PublishFailure("pre-overlay colour was not captured");
                return false;
            }
            // Imported game backbuffers (including UniversalResourceData.backBufferColor) have no
            // graph descriptor: they are captured through their ordered command-buffer binding, and
            // native conversion validates the actual D3D11 extent and format against this frame's
            // approved output extent. Only graph-owned colour is described here.
            if (!boundHudless)
            {
                TextureDesc description;
                try
                {
                    description = color.GetDescriptor(renderGraph);
                }
                catch (Exception exception)
                {
                    PublishFailure("the frame-generation HUD-less colour could not be described: " + exception.Message);
                    return false;
                }
                if (description.width <= 0 || description.height <= 0)
                {
                    PublishFailure("the frame-generation HUD-less colour has no extent");
                    return false;
                }
                // Conversion copies texels one for one. Larger pooled allocations are legal because
                // their active region starts at the origin, exactly like the depth texture's.
                if (description.width < frame.OutputWidth || description.height < frame.OutputHeight)
                {
                    PublishFailure($"the HUD-less colour is {description.width}x{description.height}, smaller " +
                                   $"than the {frame.OutputWidth}x{frame.OutputHeight} output");
                    return false;
                }
            }
        }
        Lease? lease = null;
        int generation;
        lock (_gate)
        {
            for (var i = 0; i < LeaseCount; i++)
            {
                var candidate = _leases[(_cursor + i) % LeaseCount];
                if (candidate.Busy) continue;
                _cursor = (candidate.Index + 1) % LeaseCount;
                candidate.Generation = ++_nextGeneration;
                candidate.Busy = true;
                candidate.LeasedFrame = Time.frameCount;
                candidate.Frame = frame;
                candidate.RenderEvent = renderEvent;
                candidate.Hudless = hudless;
                candidate.BoundHudless = boundHudless;
                lease = candidate;
                break;
            }
            generation = lease?.Generation ?? 0;
        }
        if (lease == null)
        {
            _log("all frame-generation capture leases are busy; skipping this frame.");
            return false;
        }
        try
        {
            // The IL2CPP delegate conversion is the operation that failed at plugin load in 1.0.4.
            // It runs here, inside the recording scope, so a failure releases this lease and reports
            // the exact exception instead of escaping construction. Once built it is cached: no
            // per-frame delegate is created after a successful initialization.
            var callback = lease.EnsureCallback();
            var builder = renderGraph.AddUnsafePass<PassData>("BigScale.FrameGenerationCapture", out var passData);
            try
            {
                // Every pooled field is initialised on every record; an unused handle must be invalid,
                // never a leftover from another pass or from this graph's earlier use.
                passData.sourceTexture = TextureHandle.nullHandle;
                passData.depthTexture = TextureHandle.nullHandle;
                passData.halfCoCTexture = TextureHandle.nullHandle;
                passData.fullCoCTexture = TextureHandle.nullHandle;
                passData.pingTexture = TextureHandle.nullHandle;
                passData.pongTexture = TextureHandle.nullHandle;
                passData.destination = TextureHandle.nullHandle;
                passData.material = null;
                passData.materialCoC = null;
                passData.cocParams = Vector3.zero;
                passData.highQualitySamplingValue = false;
                // The generation doubles as the lease token checked by the callback. `renderingData`
                // is never written: it is a generated IL2CPP wrapper this pass does not use.
                passData.downsample = generation;
                passData.sourceTexture = color;
                passData.depthTexture = depth;
                passData.halfCoCTexture = motion;

                var common = builder.Cast<IBaseRenderGraphBuilder>();
                if (color.IsValid())
                    common.UseTexture(ref color, boundHudless ? AccessFlags.ReadWrite : AccessFlags.Read);
                common.UseTexture(ref depth, AccessFlags.Read);
                common.UseTexture(ref motion, AccessFlags.Read);
                common.AllowPassCulling(false);
                // Built-in HUD-less targets are explicitly bound before the native render event.
                common.AllowGlobalStateModification(true);
                builder.SetRenderFunc(callback);
                return true;
            }
            finally
            {
                builder.Cast<Il2CppSystem.IDisposable>().Dispose();
            }
        }
        catch (Exception exception)
        {
            lock (_gate)
            {
                if (lease.Generation == generation) lease.Busy = false;
            }
            PublishFailure($"recording the frame-generation capture failed: {exception.GetType().Name}: " +
                           exception.Message);
            return false;
        }
    }
}
