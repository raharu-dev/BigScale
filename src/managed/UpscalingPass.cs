using System;
using System.Collections.Generic;
using System.Threading;
using BigScale.Core;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.RenderGraphModule;
using UnityEngine.Rendering.Universal;
using PassData = UnityEngine.Rendering.Universal.PostProcessPassRenderGraph.DoFGaussianPassData;

namespace BigScale;

/// <summary>
/// Records the reconstruction pass into the active URP render graph and hands per-eye packets to
/// the native bridge.
///
/// Each lease owns one reusable callback and an immutable packet snapshot. A lease carries a
/// generation that is copied into the recorded pass data, so a callback from a graph that was
/// recorded but never executed can never read a newer lease's packets: a generation mismatch is a
/// no-op. Allocations stay fixed at <see cref="LeaseCount"/> callbacks and packet arrays.
///
/// Every record initialises the pooled pass data it uses, and the callback validates the packet
/// against <see cref="FramePolicy"/>: a capability probe queues colour only, while an evaluation
/// must carry depth, motion and an output target. Failures are published as a pending string that
/// the main thread consumes, so coordinator policy stays on one owner.
/// </summary>
internal sealed class UpscalingPass
{
    private const int LeaseCount = 8;

    private sealed class Lease
    {
        internal readonly int Index;
        internal readonly Native.Frame[] Frames = new Native.Frame[2];
        // Snapshot copied under the gate so the callback never touches lease fields that Update may
        // be rewriting, and never allocates per frame.
        internal readonly Native.Frame[] Payload = new Native.Frame[2];
        internal readonly BaseRenderFunc<PassData, UnsafeGraphContext> Callback;
        internal int Count;
        internal IntPtr RenderEvent;
        internal uint ResourceTag;
        internal int Generation;
        internal int LeasedFrame;
        internal bool Busy;
        internal readonly UpscalingPass Owner;
        internal OutputResources? Output;
        internal Action<string>? Log;
        internal Action<string>? Failure;

        internal Lease(UpscalingPass owner, int index)
        {
            Owner = owner;
            Index = index;
            Callback = new Action<PassData, UnsafeGraphContext>(Execute);
        }

        private void Execute(PassData data, UnsafeGraphContext context)
        {
            // A stale graph cannot consume another lease's payload.
            if (data.downsample != Generation) return;
            Native.Frame[] packets;
            int count;
            lock (Owner._gate)
            {
                if (!Busy || Count <= 0) return;
                count = Count;
                Array.Copy(Frames, Payload, count);
                packets = Payload;
            }
            try
            {
                var frame = packets[0];
                var operation = frame.Operation;
                var color = ResolveTexture(data.sourceTexture);
                var depth = ResolveTexture(data.depthTexture);
                var motion = ResolveTexture(data.halfCoCTexture);
                var output = ResolveTexture(data.destination);
                if (!FramePolicy.Satisfied(operation, color != null, frame.OutputWidth, frame.OutputHeight,
                        depth != null, motion != null, output != null, out var missing))
                {
                    Failure?.Invoke($"packet rejected before the native bridge: {missing} missing " +
                                    $"(operation {operation}, camera {frame.CameraId}).");
                    return;
                }
                // Image operations were already required to carry colour by the policy above; a control
                // packet may legitimately have none and must still reach the bridge.
                if (color == null && !FramePolicy.IsControl(operation)) return;

                var commandBuffer = CommandBufferHelpers.GetNativeCommandBuffer(context.cmd);
                for (var i = 0; i < count; i++)
                {
                    var packet = packets[i];
                    packet.Color = color != null ? color.GetNativeTexturePtr() : IntPtr.Zero;
                    packet.Depth = depth != null ? depth.GetNativeTexturePtr() : IntPtr.Zero;
                    packet.Motion = motion != null ? motion.GetNativeTexturePtr() : IntPtr.Zero;
                    packet.Output = output != null ? output.GetNativeTexturePtr() : IntPtr.Zero;
                    // The output obligation was reserved when the pass was recorded; a packet the
                    // bridge refuses settles it here because no retirement will ever arrive.
                    var queued = Native.QueueFrame(ref packet);
                    if (queued == IntPtr.Zero)
                    {
                        Output?.Completed(packet.ResourceTag);
                        Failure?.Invoke($"the native bridge rejected eye {packet.Eye} of camera {packet.CameraId} " +
                                        $"(operation {packet.Operation}).");
                        continue;
                    }
                    commandBuffer.IssuePluginEventAndData(RenderEvent, 1, queued);
                }
            }
            catch (Exception exception)
            {
                Failure?.Invoke("reconstruction render function failed: " + exception.Message);
                Log?.Invoke("reconstruction render function failed: " + exception);
            }
            finally
            {
                lock (Owner._gate)
                {
                    if (data.downsample == Generation)
                    {
                        Count = 0;
                        Busy = false;
                    }
                }
            }
        }
    }

    /// <summary>Resolves a graph handle to the concrete RenderTexture, or null when invalid.</summary>
    internal static RenderTexture? ResolveTexture(TextureHandle handle)
    {
        if (!handle.IsValid()) return null;
        try
        {
            var texture = (RenderTexture)handle;
            return texture == null ? null : texture;
        }
        catch
        {
            return null;
        }
    }

    private readonly Lease[] _leases = new Lease[LeaseCount];
    // Reused by AbandonStaleLeases so a dropped graph does not allocate. Main thread only, and the
    // list is cleared inside the gate before it is filled.
    private readonly List<Lease> _abandonedLeases = new();
    private readonly Action<string> _log;
    private readonly OutputResources _output;
    // Leases are acquired on Update and released by render callbacks, so lease state is guarded.
    // The gate is never held across a Unity or native call.
    private readonly object _gate = new();
    private int _cursor;
    private int _nextGeneration;
    private string? _pendingFailure;
    // Bounded notices for control leases whose graph was discarded before execution. Switch is the
    // only operation that needs cancellation feedback, so no allocation is needed here.
    private readonly UpscalerKind[] _abandonedUpscalers = new UpscalerKind[LeaseCount];
    private readonly int[] _abandonedGenerations = new int[LeaseCount];
    private int _abandonedCount;

    internal UpscalingPass(Action<string> log, OutputResources output)
    {
        _log = log;
        _output = output;
        for (var i = 0; i < LeaseCount; i++)
        {
            _leases[i] = new Lease(this, i);
            _leases[i].Log = log;
            _leases[i].Output = output;
            _leases[i].Failure = PublishFailure;
        }
    }

    /// <summary>True when a packet can still be recorded this frame.</summary>
    internal bool HasFreeLease => Array.Exists(_leases, lease => !lease.Busy);

    /// <summary>True when a render-side failure is waiting for the main thread to consume it.</summary>
    internal bool HasPendingFailure => Volatile.Read(ref _pendingFailure) != null;

    /// <summary>Consumes a failure reported by a render callback. Only the main thread calls this.</summary>
    internal string? TakeFailure() => Interlocked.Exchange(ref _pendingFailure, null);

    /// <summary>Retains the first failure; render callbacks and recording both publish.</summary>
    private void PublishFailure(string reason) =>
        Interlocked.CompareExchange(ref _pendingFailure, reason, null);

    /// <summary>
    /// Invalidates a lease whose graph was discarded before execution. The generation bump makes a
    /// late callback a no-op instead of letting it read a newer payload.
    /// </summary>
    internal void AbandonStaleLeases(int frame)
    {
        List<Lease> abandoned = _abandonedLeases;
        lock (_gate)
        {
            abandoned.Clear();
            foreach (var lease in _leases)
            {
                if (!lease.Busy) continue;
                if (frame - lease.LeasedFrame <= MaxLeaseFrames) continue;
                if (lease.Count > 0 && lease.Frames[0].Operation == FramePolicy.Switch &&
                    _abandonedCount < _abandonedUpscalers.Length)
                {
                    // Publish before invalidating the lease token so a matching reservation can be
                    // cleared; the caller ignores notices that no longer match its request.
                    _abandonedUpscalers[_abandonedCount] = (UpscalerKind)lease.Frames[0].Upscaler;
                    _abandonedGenerations[_abandonedCount] = lease.Generation;
                    _abandonedCount++;
                }
                lease.Generation = ++_nextGeneration;
                lease.Busy = false;
                lease.Count = 0;
                abandoned.Add(lease);
            }
        }
        foreach (var lease in abandoned)
            _log($"released a stale recording lease (lease {lease.Index}, frame {lease.LeasedFrame}).");
    }

    private const int MaxLeaseFrames = 8;

    /// <summary>
    /// Consumes one abandoned switch notice. Only the main thread calls this, so a discarded graph
    /// can re-arm the pending control request without publishing a stale native cancellation.
    /// </summary>
    internal bool TakeAbandonedSwitch(out UpscalerKind upscaler, out int leaseGeneration)
    {
        lock (_gate)
        {
            if (_abandonedCount == 0)
            {
                upscaler = UpscalerKind.Off;
                leaseGeneration = 0;
                return false;
            }
            upscaler = _abandonedUpscalers[0];
            leaseGeneration = _abandonedGenerations[0];
            for (var i = 1; i < _abandonedCount; i++)
            {
                _abandonedUpscalers[i - 1] = _abandonedUpscalers[i];
                _abandonedGenerations[i - 1] = _abandonedGenerations[i];
            }
            _abandonedCount--;
            return true;
        }
    }

    /// <summary>
    /// Records one read-only probe pass. It consumes the scene colour so the graph keeps ordering,
    /// but supplies no depth, motion or output and changes no camera or asset setting.
    /// </summary>
    internal bool RecordProbe(RenderGraph renderGraph, UniversalCameraData cameraData, TextureHandle color,
        IntPtr renderEvent, UpscalerKind upscaler, UpscalingPreset preset, float near, float far, float fov)
    {
        var lease = TakeLease();
        if (lease == null) return false;
        lock (_gate)
        {
            lease.Count = 1;
            lease.RenderEvent = renderEvent;
            lease.ResourceTag = 0;
            lease.Frames[0] = new Native.Frame
            {
                Abi = Native.AbiVersion,
                Size = (uint)Native.FrameSize,
                CameraId = cameraData.camera.GetInstanceID(),
                Eye = 0,
                FrameIndex = Time.frameCount,
                HistoryEpoch = 0,
                Operation = FramePolicy.Probe,
                Preset = (int)preset,
                OutputWidth = cameraData.pixelWidth,
                OutputHeight = cameraData.pixelHeight,
                MotionWidth = cameraData.pixelWidth,
                MotionHeight = cameraData.pixelHeight,
                Slice = 0,
                SliceCount = 1,
                Layout = Native.Layout2D,
                ResourceTag = 0,
                Upscaler = (int)upscaler,
                CameraNear = near,
                CameraFar = far,
                VerticalFovRadians = fov,
                PreExposure = 1f,
            };
        }
        return Record(renderGraph, color, TextureHandle.nullHandle, TextureHandle.nullHandle, TextureHandle.nullHandle,
            lease, "BigScale.Probe", false);
    }

    /// <summary>
    /// Records one ordered provider switch. It carries no image resources: the bridge drains the old
    /// backend, releases it and publishes a retired acknowledgement naming the new provider.
    /// </summary>
    internal bool RecordSwitch(RenderGraph renderGraph, UniversalCameraData cameraData, TextureHandle color,
        IntPtr renderEvent, UpscalerKind upscaler, out int leaseGeneration)
    {
        leaseGeneration = 0;
        var lease = TakeLease();
        if (lease == null) return false;
        leaseGeneration = lease.Generation;
        lock (_gate)
        {
            lease.Count = 1;
            lease.RenderEvent = renderEvent;
            lease.ResourceTag = 0;
            lease.Frames[0] = new Native.Frame
            {
                Abi = Native.AbiVersion,
                Size = (uint)Native.FrameSize,
                CameraId = cameraData.camera.GetInstanceID(),
                Eye = 0,
                FrameIndex = Time.frameCount,
                HistoryEpoch = 0,
                Operation = FramePolicy.Switch,
                Preset = (int)UpscalingPreset.Quality,
                OutputWidth = Mathf.Max(1, cameraData.pixelWidth),
                OutputHeight = Mathf.Max(1, cameraData.pixelHeight),
                SliceCount = 1,
                Layout = Native.Layout2D,
                Upscaler = (int)upscaler,
            };
        }
        return Record(renderGraph, color, TextureHandle.nullHandle, TextureHandle.nullHandle,
            TextureHandle.nullHandle, lease, "BigScale.Switch", false);
    }

    /// <summary>
    /// Records one resource-free recovery packet. It carries colour only so the bridge can bind the
    /// device if it needs to, then drain and clear a latched fault on the render thread.
    /// </summary>
    internal bool RecordRecovery(RenderGraph renderGraph, UniversalCameraData cameraData, TextureHandle color,
        IntPtr renderEvent)
    {
        var lease = TakeLease();
        if (lease == null) return false;
        lock (_gate)
        {
            lease.Count = 1;
            lease.RenderEvent = renderEvent;
            lease.ResourceTag = 0;
            lease.Frames[0] = new Native.Frame
        {
            Abi = Native.AbiVersion,
            Size = (uint)Native.FrameSize,
            CameraId = cameraData.camera.GetInstanceID(),
            Eye = 0,
            FrameIndex = Time.frameCount,
            HistoryEpoch = 0,
            Operation = Native.OperationRecovery,
            Preset = (int)UpscalingPreset.Quality,
            OutputWidth = Mathf.Max(1, cameraData.pixelWidth),
            OutputHeight = Mathf.Max(1, cameraData.pixelHeight),
            SliceCount = 1,
            Layout = Native.Layout2D,
        };
        }
        return Record(renderGraph, color, TextureHandle.nullHandle, TextureHandle.nullHandle,
            TextureHandle.nullHandle, lease, "BigScale.Recovery", false);
    }

    /// <summary>Records the reconstruction pass and queues one packet per logical eye.</summary>
    internal bool RecordReconstruction(RenderGraph renderGraph, TextureHandle color, TextureHandle depth,
        TextureHandle motion, TextureHandle output, Native.Frame[] frames, int count, IntPtr renderEvent,
        uint resourceTag)
    {
        var lease = TakeLease();
        if (lease == null) return false;
        lock (_gate)
        {
            lease.Count = count;
            lease.RenderEvent = renderEvent;
            lease.ResourceTag = resourceTag;
            Array.Copy(frames, lease.Frames, count);
        }
        return Record(renderGraph, color, depth, motion, output, lease, "BigScale.Reconstruction", true);
    }

    private Lease? TakeLease()
    {
        lock (_gate)
        {
            for (var i = 0; i < LeaseCount; i++)
            {
                var lease = _leases[(_cursor + i) % LeaseCount];
                if (lease.Busy) continue;
                _cursor = (lease.Index + 1) % LeaseCount;
                lease.Generation = ++_nextGeneration;
                lease.Count = 0;
                lease.Busy = true;
                lease.LeasedFrame = Time.frameCount;
                return lease;
            }
        }
        _log("all reconstruction recording leases are busy; skipping this frame.");
        return null;
    }

    private bool Record(RenderGraph renderGraph, TextureHandle color, TextureHandle depth, TextureHandle motion,
        TextureHandle output, Lease lease, string label, bool writesOutput)
    {
        var builder = renderGraph.AddUnsafePass<PassData>(label, out var passData);
        try
        {
            // Every pooled field is initialised on every record; an unused handle must be invalid,
            // never a leftover from a previous frame.
            passData.sourceTexture = color;
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
            // The generation doubles as the lease token checked by the callback.
            passData.downsample = lease.Generation;

            var common = builder.Cast<IBaseRenderGraphBuilder>();
            common.UseTexture(ref color, AccessFlags.Read);
            if (depth.IsValid())
            {
                passData.depthTexture = depth;
                common.UseTexture(ref depth, AccessFlags.Read);
            }
            if (motion.IsValid())
            {
                passData.halfCoCTexture = motion;
                common.UseTexture(ref motion, AccessFlags.Read);
            }
            if (writesOutput && output.IsValid())
            {
                passData.destination = output;
                common.UseTexture(ref output, AccessFlags.Write);
            }
            common.AllowPassCulling(false);
            common.AllowGlobalStateModification(true);
            builder.SetRenderFunc(lease.Callback);
            return true;
        }
        catch (Exception exception)
        {
            lock (_gate)
            {
                lease.Busy = false;
                lease.Count = 0;
            }
            PublishFailure($"recording '{label}' failed: {exception.GetType().Name}: {exception.Message}");
            return false;
        }
        finally
        {
            builder.Cast<Il2CppSystem.IDisposable>().Dispose();
        }
    }
}
