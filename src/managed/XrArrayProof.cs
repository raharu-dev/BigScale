using System;
using System.IO;
using Unity.Collections;
using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;
using UnityEngine.Rendering.RenderGraphModule;
using UnityEngine.Rendering.Universal;
using PassData = UnityEngine.Rendering.Universal.PostProcessPassRenderGraph.DoFGaussianPassData;

namespace BigScale;

/// <summary>
/// Verification-only tap that answers whether the reconstructed image - the one carrying the native
/// identity band - survives to the texture URP hands the XR runtime.
/// <para>
/// Three stages are sampled for both eyes: the post-processing destination right after UberPost
/// (recorded into the graph, <c>post-uber</c>), the eye array at <c>endCameraRendering</c> of the
/// same frame (<c>eye-end</c>), and the eye array at the next frame's VR camera callback
/// (<c>eye-next</c>). Comparing the three locates the writer that replaces the image.
/// </para>
/// <para>
/// A 256x256 crop per stage and eye is copied into its own plain render texture, so a copy is never
/// overwritten before its readback completes. Everything is bounded, reads only, and is skipped
/// unless this launch set the proof switch.
/// </para>
/// </summary>
internal sealed class XrArrayProof
{
    private const int Crop = 256;
    private const int MaxArms = 96;
    private const int ArmInterval = 16;
    private const int StageCount = 3;
    private const int SlotCount = StageCount * 2;
    private const int StagePostUber = 0;
    private const int StageEyeEnd = 1;
    private const int StageEyeNext = 2;

    private static readonly string[] StageNames = { "post-uber", "eye-end", "eye-next" };

    private sealed class Slot
    {
        internal RenderTexture? Target;
        internal GraphicsFormat Format;
        // The frame whose stage scheduled the copy, -1 when the slot holds no pending work.
        internal int CopyFrame = -1;
        internal AsyncGPUReadbackRequest Request;
        internal bool RequestPending;
        internal int Frame;
        internal string Window = "";
    }

    private readonly Action<string> _log;
    private readonly bool _enabled;
    private readonly Slot[]? _slots;
    private readonly BaseRenderFunc<PassData, UnsafeGraphContext>? _postUberCallback;
    // Bit per stage: the one-time "format is not decodable" notice was already written.
    private int _undecodableStages;
    private CommandBuffer? _commands;
    private RenderTargetIdentifier _eyeIdentifier;
    private int _eyeWidth;
    private int _eyeHeight;
    private GraphicsFormat _eyeFormat;
    private int _postLeft;
    private int _postTop;
    private GraphicsFormat _postFormat;
    private int _armSerial;
    private int _armFrame = -1;
    private string _armWindow = "";
    private bool _armed;
    private int _lastArm = -1000;
    private int _arms;
    private bool _unavailableLogged;
    private bool _postTargetMissingLogged;

    internal XrArrayProof(Action<string> log, bool enabled)
    {
        _log = log;
        _enabled = enabled;
        // With the proof switch absent this class owns no render target, no callback and no slot.
        if (!_enabled) return;
        _slots = new Slot[SlotCount];
        for (var i = 0; i < SlotCount; i++) _slots[i] = new Slot();
        _postUberCallback = new Action<PassData, UnsafeGraphContext>(ExecutePostUber);
    }

    /// <summary>
    /// Arms one capture for the recorded frame and records the post-processing tap. The eye taps run
    /// later in this frame and at the next camera callback, where the eye array is complete.
    /// </summary>
    internal void ArmPostUber(RenderGraph graph, UniversalResourceData resourceData,
        UniversalCameraData cameraData, TextureHandle postTarget, string window)
    {
        if (!_enabled) return;
        // An arm from an earlier frame whose next-camera callback never ran can never be consumed.
        if (_armed && Time.frameCount > _armFrame) _armed = false;
        if (_armed) return;
        if (string.IsNullOrEmpty(window) || window == "idle" || window == "done") return;
        if (Time.frameCount - _lastArm < ArmInterval) return;
        if (_arms >= MaxArms) return;
        if (AnyPending()) return;
        if (graph == null || cameraData == null) return;
        try
        {
            var xr = cameraData.xr;
            if (xr == null || !xr.enabled || !xr.singlePassEnabled)
            {
                ReportUnavailable("the camera is not a single-pass XR camera");
                return;
            }
            var descriptor = xr.renderTargetDesc;
            if (descriptor.width < Crop || descriptor.height < Crop || descriptor.volumeDepth < 2)
            {
                ReportUnavailable($"the XR array is {descriptor.width}x{descriptor.height}x{descriptor.volumeDepth}");
                return;
            }
            _eyeIdentifier = xr.renderTarget;
            _eyeWidth = descriptor.width;
            _eyeHeight = descriptor.height;
            _eyeFormat = descriptor.graphicsFormat;
            var slots = _slots!;
            // Created here rather than inside a render callback: no allocation while the graph executes.
            for (var eye = 0; eye < 2; eye++)
            {
                EnsureTarget(slots[StageEyeEnd * 2 + eye], _eyeFormat);
                EnsureTarget(slots[StageEyeNext * 2 + eye], _eyeFormat);
            }
            _armFrame = Time.frameCount;
            _armWindow = window;
            _armSerial++;
            _lastArm = _armFrame;
            _arms++;
            _armed = true;
            // UberPost wrote the eye texture directly in this window, so the eye stages already cover it.
            if (resourceData == null || !postTarget.IsValid() || postTarget == resourceData.backBufferColor) return;
            var source = postTarget.GetDescriptor(graph);
            _postFormat = source.format;
            _postLeft = Mathf.Max(0, (source.width - Crop) / 2);
            _postTop = Mathf.Max(0, (source.height - Crop) / 2);
            EnsureTarget(slots[StagePostUber * 2], _postFormat);
            EnsureTarget(slots[StagePostUber * 2 + 1], _postFormat);
            RecordPostUber(graph, postTarget, _armSerial);
        }
        catch (Exception exception)
        {
            ReportUnavailable(exception.GetType().Name + ": " + exception.Message);
        }
    }

    /// <summary>Runs the armed frame's two <c>eye-end</c> crops at the end of its camera render.</summary>
    internal void ExecuteEndCamera(ScriptableRenderContext context)
    {
        if (!_enabled || !_armed) return;
        if (Time.frameCount != _armFrame) return;
        try
        {
            if (!ScheduleEyes(StageEyeEnd)) return;
            context.ExecuteCommandBuffer(_commands!);
        }
        catch (Exception exception)
        {
            ReportUnavailable(exception.GetType().Name + ": " + exception.Message);
        }
    }

    /// <summary>Runs the armed frame's two <c>eye-next</c> crops at the next camera callback.</summary>
    internal void Execute(ScriptableRenderContext context)
    {
        if (!_enabled || !_armed) return;
        if (Time.frameCount <= _armFrame) return;
        _armed = false;
        try
        {
            if (!ScheduleEyes(StageEyeNext)) return;
            context.ExecuteCommandBuffer(_commands!);
        }
        catch (Exception exception)
        {
            ReportUnavailable(exception.GetType().Name + ": " + exception.Message);
        }
    }

    /// <summary>
    /// Requests and completes readbacks. Called once per camera callback, always a frame after the copy
    /// it reads was scheduled, so a request never races the copy in flight.
    /// </summary>
    internal void Poll()
    {
        if (!_enabled) return;
        var slots = _slots!;
        for (var i = 0; i < SlotCount; i++)
        {
            var slot = slots[i];
            if (slot.Target == null) continue;
            var stage = i / 2;
            var eye = i % 2;
            if (slot.RequestPending)
            {
                if (slot.Request.hasError)
                {
                    slot.RequestPending = false;
                    slot.CopyFrame = -1;
                    _log($"XR tap stage={StageNames[stage]} eye={eye} frame={slot.Frame}: readback failed");
                    continue;
                }
                if (!slot.Request.done) continue;
                slot.RequestPending = false;
                try
                {
                    ReportStage(stage, eye, slot, slot.Request);
                }
                catch
                {
                    // A decode or file failure must never reach the game's render loop.
                }
                slot.CopyFrame = -1;
                continue;
            }
            if (slot.CopyFrame < 0 || Time.frameCount <= slot.CopyFrame) continue;
            // Raw readback of the whole crop: mip 0, the full 256x256 region, slice 0 of one slice.
            slot.Request = AsyncGPUReadback.Request(slot.Target!, 0, 0, Crop, 0, Crop, 0, 1);
            slot.RequestPending = true;
        }
    }

    /// <summary>Records the tap on the post-processing destination into the active graph.</summary>
    private void RecordPostUber(RenderGraph graph, TextureHandle postTarget, int serial)
    {
        var builder = graph.AddUnsafePass<PassData>("BigScale XR tap post-uber", out var passData);
        try
        {
            // Every pooled field is initialised on every record, exactly as the reconstruction pass does.
            passData.sourceTexture = postTarget;
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
            // The arm serial doubles as the token checked by the callback.
            passData.downsample = serial;

            var common = builder.Cast<IBaseRenderGraphBuilder>();
            common.UseTexture(ref postTarget, AccessFlags.Read);
            common.AllowPassCulling(false);
            common.AllowGlobalStateModification(true);
            builder.SetRenderFunc(_postUberCallback!);
        }
        finally
        {
            builder.Cast<Il2CppSystem.IDisposable>().Dispose();
        }
    }

    private void ExecutePostUber(PassData data, UnsafeGraphContext context)
    {
        if (!_enabled || !_armed || data.downsample != _armSerial) return;
        var source = UpscalingPass.ResolveTexture(data.sourceTexture);
        if (source == null)
        {
            if (!_postTargetMissingLogged)
            {
                _postTargetMissingLogged = true;
                _log("XR tap stage=post-uber: the post target is not a RenderTexture");
            }
            return;
        }
        try
        {
            var commands = CommandBufferHelpers.GetNativeCommandBuffer(context.cmd);
            var sourceIdentifier = new RenderTargetIdentifier(source);
            for (var eye = 0; eye < 2; eye++)
            {
                var slot = _slots![StagePostUber * 2 + eye];
                if (slot.CopyFrame >= 0 || slot.RequestPending) continue;
                EnsureTarget(slot, _postFormat);
                commands.CopyTexture(sourceIdentifier, eye, 0, _postLeft, _postTop, Crop, Crop,
                    new RenderTargetIdentifier(slot.Target!), 0, 0, 0, 0);
                slot.CopyFrame = Time.frameCount;
                slot.Frame = _armFrame;
                slot.Window = _armWindow;
            }
        }
        catch (Exception exception)
        {
            ReportUnavailable(exception.GetType().Name + ": " + exception.Message);
        }
    }

    private bool ScheduleEyes(int stage)
    {
        var left = Mathf.Max(0, (_eyeWidth - Crop) / 2);
        var top = Mathf.Max(0, (_eyeHeight - Crop) / 2);
        _commands ??= new CommandBuffer { name = "BigScale XR tap" };
        _commands.Clear();
        var scheduled = false;
        for (var eye = 0; eye < 2; eye++)
        {
            var slot = _slots![stage * 2 + eye];
            if (slot.CopyFrame >= 0 || slot.RequestPending) continue;
            EnsureTarget(slot, _eyeFormat);
            _commands.CopyTexture(_eyeIdentifier, eye, 0, left, top, Crop, Crop,
                new RenderTargetIdentifier(slot.Target!), 0, 0, 0, 0);
            slot.CopyFrame = Time.frameCount;
            slot.Frame = _armFrame;
            slot.Window = _armWindow;
            scheduled = true;
        }
        return scheduled;
    }

    private static void EnsureTarget(Slot slot, GraphicsFormat format)
    {
        if (slot.Target != null && slot.Format == format) return;
        if (slot.Target != null) UnityEngine.Object.Destroy(slot.Target);
        slot.Target = new RenderTexture(Crop, Crop, 0, format)
        {
            name = "BigScale XR tap",
            hideFlags = HideFlags.HideAndDontSave,
        };
        slot.Format = format;
    }

    private bool AnyPending()
    {
        foreach (var slot in _slots!)
        {
            if (slot.CopyFrame >= 0 || slot.RequestPending) return true;
        }
        return false;
    }

    private void ReportUnavailable(string reason)
    {
        if (_unavailableLogged) return;
        _unavailableLogged = true;
        _log("XR tap unavailable: " + reason);
    }

    private void ReportStage(int stage, int eye, Slot slot, AsyncGPUReadbackRequest request)
    {
        var name = StageNames[stage];
        var format = slot.Format;
        var bytesPerPixel = BytesPerPixel(format);
        if (bytesPerPixel == 0)
        {
            var bit = 1 << stage;
            if ((_undecodableStages & bit) != 0) return;
            _undecodableStages |= bit;
            _log($"XR tap stage={name} eye={eye}: format {format} is not decodable");
            return;
        }
        var data = request.GetData<byte>();
        if (data.Length < Crop * Crop * bytesPerPixel)
        {
            _log($"XR tap stage={name} eye={eye} frame={slot.Frame}: readback failed");
            return;
        }
        var bgr = new byte[Crop * Crop * 3];
        var magenta = 0;
        byte centreR = 0;
        byte centreG = 0;
        byte centreB = 0;
        for (var y = 0; y < Crop; y++)
        {
            for (var x = 0; x < Crop; x++)
            {
                var index = y * Crop + x;
                DecodePixel(data, index * bytesPerPixel, format, out var r, out var g, out var b);
                bgr[index * 3 + 0] = b;
                bgr[index * 3 + 1] = g;
                bgr[index * 3 + 2] = r;
                if (x >= 120 && x <= 135 && y >= 120 && y <= 135 && r >= 96 && b >= 96 && 2 * g < Math.Min(r, b))
                    magenta++;
                if (x == 128 && y == 128)
                {
                    centreR = r;
                    centreG = g;
                    centreB = b;
                }
            }
        }
        var band = magenta >= 200;
        var directory = Path.Combine(BepInEx.Paths.BepInExRootPath, "BigScale");
        var path = Path.Combine(directory, $"xr-tap-{name}-frame{slot.Frame}-eye{eye}-{slot.Window}.bmp");
        WriteBmp(path, bgr, Crop, Crop);
        _log($"XR tap stage={name} window={slot.Window} frame={slot.Frame} eye={eye} format={format} " +
             $"band={(band ? "yes" : "no")} magenta={magenta}/256 centre=({centreR},{centreG},{centreB})");
    }

    private static int BytesPerPixel(GraphicsFormat format) => format switch
    {
        GraphicsFormat.R8G8B8A8_SRGB => 4,
        GraphicsFormat.R8G8B8A8_UNorm => 4,
        GraphicsFormat.B8G8R8A8_SRGB => 4,
        GraphicsFormat.B8G8R8A8_UNorm => 4,
        GraphicsFormat.B10G11R11_UFloatPack32 => 4,
        GraphicsFormat.R16G16B16A16_SFloat => 8,
        _ => 0,
    };

    private static void DecodePixel(NativeArray<byte> data, int offset, GraphicsFormat format,
        out byte red, out byte green, out byte blue)
    {
        switch (format)
        {
            case GraphicsFormat.R8G8B8A8_SRGB:
            case GraphicsFormat.R8G8B8A8_UNorm:
                red = data[offset];
                green = data[offset + 1];
                blue = data[offset + 2];
                return;
            case GraphicsFormat.B8G8R8A8_SRGB:
            case GraphicsFormat.B8G8R8A8_UNorm:
                blue = data[offset];
                green = data[offset + 1];
                red = data[offset + 2];
                return;
            case GraphicsFormat.B10G11R11_UFloatPack32:
            {
                var packed = (uint)(data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16) |
                                    (data[offset + 3] << 24));
                red = EncodeSrgb(UnpackSharedExponent(packed & 0x3F, (packed >> 6) & 0x1F, 6));
                green = EncodeSrgb(UnpackSharedExponent((packed >> 11) & 0x3F, (packed >> 17) & 0x1F, 6));
                blue = EncodeSrgb(UnpackSharedExponent((packed >> 22) & 0x1F, (packed >> 27) & 0x1F, 5));
                return;
            }
            case GraphicsFormat.R16G16B16A16_SFloat:
                red = EncodeSrgb(Mathf.HalfToFloat((ushort)(data[offset] | (data[offset + 1] << 8))));
                green = EncodeSrgb(Mathf.HalfToFloat((ushort)(data[offset + 2] | (data[offset + 3] << 8))));
                blue = EncodeSrgb(Mathf.HalfToFloat((ushort)(data[offset + 4] | (data[offset + 5] << 8))));
                return;
            default:
                red = 0;
                green = 0;
                blue = 0;
                return;
        }
    }

    private static float UnpackSharedExponent(uint mantissa, uint exponent, int mantissaBits)
    {
        if (exponent == 0) return mantissa / (float)(1 << mantissaBits) * (1f / 16384f);
        if (exponent == 31) return 1f;
        return Mathf.Pow(2f, exponent - 15f) * (1f + mantissa / (float)(1 << mantissaBits));
    }

    private static byte EncodeSrgb(float linear)
    {
        var clamped = Mathf.Clamp(linear, 0f, 1f);
        var encoded = clamped <= 0.0031308f ? 12.92f * clamped : 1.055f * Mathf.Pow(clamped, 1f / 2.4f) - 0.055f;
        return (byte)Mathf.RoundToInt(encoded * 255f);
    }

    private static void WriteBmp(string path, byte[] bgr, int width, int height)
    {
        var stride = (width * 3 + 3) & ~3;
        var size = 54 + stride * height;
        var file = new byte[size];
        file[0] = (byte)'B';
        file[1] = (byte)'M';
        BitConverter.GetBytes(size).CopyTo(file, 2);
        BitConverter.GetBytes(54).CopyTo(file, 10);
        BitConverter.GetBytes(40).CopyTo(file, 14);
        BitConverter.GetBytes(width).CopyTo(file, 18);
        BitConverter.GetBytes(-height).CopyTo(file, 22);
        BitConverter.GetBytes((short)1).CopyTo(file, 26);
        BitConverter.GetBytes((short)24).CopyTo(file, 28);
        BitConverter.GetBytes(stride * height).CopyTo(file, 34);
        for (var y = 0; y < height; y++)
            Buffer.BlockCopy(bgr, y * width * 3, file, 54 + y * stride, width * 3);
        File.WriteAllBytes(path, file);
    }

    internal void Release()
    {
        if (_slots == null) return;
        foreach (var slot in _slots)
        {
            if (slot.Target != null) UnityEngine.Object.Destroy(slot.Target);
            slot.Target = null;
            slot.CopyFrame = -1;
            slot.RequestPending = false;
        }
        _armed = false;
    }
}
