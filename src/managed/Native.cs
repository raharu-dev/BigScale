using System;
using System.Reflection;
using System.Runtime.InteropServices;

namespace BigScale;

/// <summary>
/// Mirror of the native bridge ABI. Every shared struct is blittable and laid out sequentially so
/// the offsets can be compared against the native side once at startup.
/// </summary>
internal static class Native
{
    internal const string Dll = "BigScale.Native";

    /// <summary>Bumped whenever a shared struct changes. Both sides refuse a mismatch.</summary>
    internal const uint AbiVersion = 4;

    /// <summary>
    /// Cached blittable sizes. <c>Marshal.SizeOf</c> is not free and a packet is built every frame.
    /// </summary>
    internal static readonly int FrameSize = Marshal.SizeOf<Frame>();
    internal static readonly int StatusSize = Marshal.SizeOf<Status>();
    internal static readonly int CompletionSize = Marshal.SizeOf<Completion>();

    /// <summary>Field order of <see cref="Frame"/>, used for the native/managed layout check.</summary>
    internal static readonly string[] FrameFieldOrder =
    {
        nameof(Frame.Abi), nameof(Frame.Size), nameof(Frame.CameraId), nameof(Frame.Eye),
        nameof(Frame.FrameIndex), nameof(Frame.HistoryEpoch), nameof(Frame.Operation), nameof(Frame.Preset),
        nameof(Frame.InputWidth), nameof(Frame.InputHeight), nameof(Frame.OutputWidth), nameof(Frame.OutputHeight),
        nameof(Frame.InputX), nameof(Frame.InputY), nameof(Frame.OutputX), nameof(Frame.OutputY),
        nameof(Frame.Slice), nameof(Frame.SliceCount), nameof(Frame.Flags), nameof(Frame.Layout),
        nameof(Frame.Color), nameof(Frame.Depth), nameof(Frame.Motion), nameof(Frame.Output),
        nameof(Frame.JitterX), nameof(Frame.JitterY), nameof(Frame.MotionScaleX), nameof(Frame.MotionScaleY),
        nameof(Frame.DeltaMilliseconds), nameof(Frame.MotionWidth), nameof(Frame.MotionHeight),
        nameof(Frame.Reserved), nameof(Frame.ResourceTag), nameof(Frame.Upscaler),
        nameof(Frame.CameraNear), nameof(Frame.CameraFar), nameof(Frame.VerticalFovRadians),
        nameof(Frame.PreExposure), nameof(Frame.FgCaptureTag),
    };

    /// <summary>Field order of <see cref="Completion"/>.</summary>
    internal static readonly string[] CompletionFieldOrder =
    {
        nameof(Completion.Sequence), nameof(Completion.Kind), nameof(Completion.ResourceTag),
        nameof(Completion.CameraId), nameof(Completion.Eye), nameof(Completion.FrameIndex),
        nameof(Completion.HistoryEpoch), nameof(Completion.Operation), nameof(Completion.Result),
        nameof(Completion.InputWidth), nameof(Completion.InputHeight), nameof(Completion.OutputWidth),
        nameof(Completion.OutputHeight), nameof(Completion.Upscaler), nameof(Completion.Preset),
        nameof(Completion.Layout), nameof(Completion.Slice), nameof(Completion.SliceCount),
        nameof(Completion.Reserved),
    };

    /// <summary>Field order of <see cref="Status"/>.</summary>
    internal static readonly string[] StatusFieldOrder =
    {
        nameof(Status.Abi), nameof(Status.Size), nameof(Status.State), nameof(Status.LastResult),
        nameof(Status.LastError), nameof(Status.RuntimeVersion), nameof(Status.Layout), nameof(Status.Preset),
        nameof(Status.OutputWidth), nameof(Status.OutputHeight), nameof(Status.InputWidth),
        nameof(Status.InputHeight), nameof(Status.TotalEvaluations), nameof(Status.ProbeCount),
        nameof(Status.ModeCount),
        nameof(Status.Mode0), nameof(Status.Mode1), nameof(Status.Mode2), nameof(Status.Mode3),
        nameof(Status.Mode4),
        nameof(Status.View0), nameof(Status.View1),
        nameof(Status.ViewCount), nameof(Status.BilinearComparisons), nameof(Status.FaultCount),
        nameof(Status.Reserved), nameof(Status.CompletionPublished), nameof(Status.CompletionBase),
        nameof(Status.Completion0), nameof(Status.Completion1), nameof(Status.Completion2),
        nameof(Status.Completion3), nameof(Status.Completion4), nameof(Status.Completion5),
        nameof(Status.Completion6), nameof(Status.Completion7),
        nameof(Status.QueryUpscaler), nameof(Status.ActiveUpscaler), nameof(Status.ProviderVersionId),
        nameof(Status.BackendReason),
    };

    [DllImport(Dll, EntryPoint = "BIGSCALE_GetAbiVersion", CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint GetAbiVersion();

    [DllImport(Dll, EntryPoint = "BIGSCALE_GetFrameSize", CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint GetFrameSize();

    [DllImport(Dll, EntryPoint = "BIGSCALE_GetStatusSize", CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint GetStatusSize();

    [DllImport(Dll, EntryPoint = "BIGSCALE_GetFrameLayout", CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint GetFrameLayout([Out] uint[] offsets, uint count);

    [DllImport(Dll, EntryPoint = "BIGSCALE_GetStatusLayout", CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint GetStatusLayout([Out] uint[] offsets, uint count);

    [DllImport(Dll, EntryPoint = "BIGSCALE_GetCompletionSize", CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint GetCompletionSize();

    [DllImport(Dll, EntryPoint = "BIGSCALE_GetCompletionLayout", CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint GetCompletionLayout([Out] uint[] offsets, uint count);

    [DllImport(Dll, EntryPoint = "BIGSCALE_Initialize", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Initialize([MarshalAs(UnmanagedType.LPWStr)] string runtimeDirectory,
        [MarshalAs(UnmanagedType.LPWStr)] string dataDirectory);

    [DllImport(Dll, EntryPoint = "BIGSCALE_SetEngineVersion", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void SetEngineVersion([MarshalAs(UnmanagedType.LPUTF8Str)] string engineVersion);

    [DllImport(Dll, EntryPoint = "BIGSCALE_GetRenderEvent", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr GetRenderEvent();

    [DllImport(Dll, EntryPoint = "BIGSCALE_QueueFrame", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr QueueFrame(ref Frame frame);

    [DllImport(Dll, EntryPoint = "BIGSCALE_GetStatus", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int GetStatus(ref Status status);

    /// <summary>Queues a render-thread drain. Safe to call from the managed main thread.</summary>
    [DllImport(Dll, EntryPoint = "BIGSCALE_RequestShutdown", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void RequestShutdown();

    /// <summary>Drops every retained completion up to and including this sequence.</summary>
    [DllImport(Dll, EntryPoint = "BIGSCALE_AckCompletions", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void AckCompletions(uint throughSequence);

    /// <summary>Native operation codes.</summary>
    internal const int OperationProbe = 0;
    internal const int OperationEvaluate = 1;

    internal const int OperationShutdown = 3;

    /// <summary>Drains and clears a latched native fault so a fresh probe can run.</summary>
    internal const int OperationRecovery = 4;

    /// <summary>Drains the old provider and acknowledges the selected provider; carries no image.</summary>
    internal const int OperationSwitch = 5;

    /// <summary>Native layout kinds.</summary>
    internal const int Layout2D = 0;
    internal const int LayoutArray = 1;
    internal const int LayoutPacked = 2;

    internal const int FlagReversedDepth = 1 << 0;
    internal const int FlagHdrColor = 1 << 1;
    internal const int FlagReset = 1 << 2;
    /// <summary>Diagnostics: sample the input and output stages and count how often each one changes.</summary>
    internal const int FlagStageProbe = 1 << 3;
    /// <summary>Diagnostics: log one centre texel of the converted input and of the written output.</summary>
    internal const int FlagSampleInput = 1 << 4;
    /// <summary>Diagnostics: stamp the eye's identity bar (vertical for eye 0, horizontal for eye 1).</summary>
    internal const int FlagStageMarker = 1 << 5;

    /// <summary>Native state machine values.</summary>
    internal const int StateUnsupported = 3;
    internal const int StateFaulted = 4;
    internal const int StateShutdown = 5;

    /// <summary>An old-provider packet that was retired without executing, never a backend fault.</summary>
    internal const int ErrorStalePacket = 19;

    /// <summary>Maximum completions published per status snapshot.</summary>
    internal const int CompletionCapacity = 8;

    /// <summary>Completion kinds. Only a retired record may release a resource or verify activity.</summary>
    internal const int CompletionRetired = 1;

    /// <summary>Verifies ABI, struct sizes and every shared field offset. Returns null on success.</summary>
    internal static string? VerifyLayout()
    {
        var nativeAbi = GetAbiVersion();
        if (nativeAbi != AbiVersion) return $"native ABI {nativeAbi} does not match managed ABI {AbiVersion}";

        var frameSize = FrameSize;
        if (GetFrameSize() != frameSize) return $"frame size mismatch: native {GetFrameSize()} managed {frameSize}";
        var statusSize = StatusSize;
        if (GetStatusSize() != statusSize) return $"status size mismatch: native {GetStatusSize()} managed {statusSize}";
        var completionSize = CompletionSize;
        if (GetCompletionSize() != completionSize)
            return $"completion size mismatch: native {GetCompletionSize()} managed {completionSize}";

        var error = VerifyFields("frame", FrameFieldOrder, native => new uint[native], GetFrameLayout,
            FrameFieldOrder.Length, name => Marshal.OffsetOf<Frame>(name).ToInt64());
        if (error != null) return error;
        error = VerifyFields("completion", CompletionFieldOrder, native => new uint[native], GetCompletionLayout,
            CompletionFieldOrder.Length, name => Marshal.OffsetOf<Completion>(name).ToInt64());
        if (error != null) return error;
        return VerifyFields("status", StatusFieldOrder, native => new uint[native], GetStatusLayout,
            StatusFieldOrder.Length, name => Marshal.OffsetOf<Status>(name).ToInt64());
    }

    private static string? VerifyFields(string label, string[] order, Func<int, uint[]> allocate,
        LayoutReader reader, int expectedCount, Func<string, long> managedOffset)
    {
        var native = allocate(order.Length);
        var count = reader(native, (uint)native.Length);
        if (count != order.Length)
            return $"{label} layout field count mismatch: native {count} managed {order.Length}";
        for (var i = 0; i < order.Length; i++)
        {
            var managed = managedOffset(order[i]);
            if (managed != native[i])
                return $"{label} field '{order[i]}' offset mismatch: native {native[i]} managed {managed}";
        }
        return null;
    }

    private delegate uint LayoutReader(uint[] offsets, uint count);

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    internal struct Frame
    {
        public uint Abi;
        public uint Size;
        public int CameraId;
        public int Eye;
        public int FrameIndex;
        public int HistoryEpoch;
        public int Operation;
        public int Preset;
        public int InputWidth;
        public int InputHeight;
        public int OutputWidth;
        public int OutputHeight;
        public int InputX;
        public int InputY;
        public int OutputX;
        public int OutputY;
        public int Slice;
        public int SliceCount;
        public int Flags;
        public int Layout;
        public IntPtr Color;
        public IntPtr Depth;
        public IntPtr Motion;
        public IntPtr Output;
        /// <summary>
        /// Provider-ready sub-pixel jitter in render pixels, in the same coordinate and direction
        /// system as <see cref="MotionScaleX"/>. TemporalInputPolicy.Jitter is the single owner of the
        /// conversion, and every backend receives the identical value.
        /// </summary>
        public float JitterX;
        public float JitterY;
        /// <summary>Per-axis motion scale in pixels, resolved from the active render extent.</summary>
        public float MotionScaleX;
        public float MotionScaleY;
        public float DeltaMilliseconds;
        public int MotionWidth;
        public int MotionHeight;
        public int Reserved;
        public uint ResourceTag;
        public int Upscaler;
        public float CameraNear;
        public float CameraFar;
        public float VerticalFovRadians;
        public float PreExposure;
        public uint FgCaptureTag;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    internal struct ModeInfo
    {
        public uint Supported;
        public uint OptimalWidth;
        public uint OptimalHeight;
        public uint MinWidth;
        public uint MinHeight;
        public uint MaxWidth;
        public uint MaxHeight;
        public uint Reserved;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    internal struct ViewInfo
    {
        public uint Eye;
        public uint Evaluations;
        public int LastFrame;
        public int LastEpoch;
        public uint Failures;
        public uint Resets;
    }

    /// <summary>
    /// One immutable packet record. <see cref="Kind"/> distinguishes a packet that ran on the render
    /// thread from one whose GPU work provably finished.
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    internal struct Completion
    {
        public uint Sequence;
        public uint Kind;
        public uint ResourceTag;
        public int CameraId;
        public int Eye;
        public int FrameIndex;
        public int HistoryEpoch;
        public int Operation;
        public int Result;
        public int InputWidth;
        public int InputHeight;
        public int OutputWidth;
        public int OutputHeight;
        public int Upscaler;
        public int Preset;
        public int Layout;
        public int Slice;
        public int SliceCount;
        public uint Reserved;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    internal unsafe struct Status
    {
        public uint Abi;
        public uint Size;
        public int State;
        public int LastResult;
        public int LastError;
        public uint RuntimeVersion;
        public int Layout;
        public int Preset;
        public uint OutputWidth;
        public uint OutputHeight;
        public uint InputWidth;
        public uint InputHeight;
        public ulong TotalEvaluations;
        public uint ProbeCount;
        public uint ModeCount;

        public ModeInfo Mode0;
        public ModeInfo Mode1;
        public ModeInfo Mode2;
        public ModeInfo Mode3;
        public ModeInfo Mode4;

        public ViewInfo View0;
        public ViewInfo View1;

        public uint ViewCount;
        public uint BilinearComparisons;
        public uint FaultCount;
        public uint Reserved;

        public uint CompletionPublished;
        public uint CompletionBase;
        public Completion Completion0;
        public Completion Completion1;
        public Completion Completion2;
        public Completion Completion3;
        public Completion Completion4;
        public Completion Completion5;
        public Completion Completion6;
        public Completion Completion7;

        /// <summary>In: the upscaler whose mode block the caller wants. Out: unchanged.</summary>
        public int QueryUpscaler;

        /// <summary>The backend that actually produced the last retired frame, never the request.</summary>
        public int ActiveUpscaler;

        /// <summary>64-bit implementation identity for APIs that report one; zero otherwise.</summary>
        public ulong ProviderVersionId;

        /// <summary>Fixed UTF-8 reason. Never marshalled as a string.</summary>
        public fixed byte BackendReason[BackendReasonSize];

        public const int BackendReasonSize = 256;

        public ModeInfo ModeAt(int index) => index switch
        {
            0 => Mode0,
            1 => Mode1,
            2 => Mode2,
            3 => Mode3,
            4 => Mode4,
            _ => default,
        };

        public Completion CompletionAt(int index) => index switch
        {
            0 => Completion0,
            1 => Completion1,
            2 => Completion2,
            3 => Completion3,
            4 => Completion4,
            5 => Completion5,
            6 => Completion6,
            7 => Completion7,
            _ => default,
        };

        /// <summary>Decodes the fixed reason without allocating on an unchanged frame.</summary>
        public string BackendReasonText()
        {
            fixed (byte* pointer = BackendReason)
            {
                var length = 0;
                while (length < BackendReasonSize && pointer[length] != 0) length++;
                return length == 0 ? "" : System.Text.Encoding.UTF8.GetString(pointer, length);
            }
        }
    }
}
