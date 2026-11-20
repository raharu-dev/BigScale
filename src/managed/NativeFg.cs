using System;
using System.Runtime.InteropServices;

namespace BigScale;

/// <summary>
/// Mirror of the frame-generation ABI. FG has its own version number and structs; every offset is
/// compared against the native exports once at startup, exactly like the SR bridge.
/// </summary>
internal static unsafe class NativeFg
{
    internal const uint AbiVersion = 7;

    /// <summary>Cached blittable sizes, built once instead of on every packet.</summary>
    internal static readonly int ConfigSize = Marshal.SizeOf<Config>();
    internal static readonly int FrameSize = Marshal.SizeOf<Frame>();
    internal static readonly int StatusSize = Marshal.SizeOf<Status>();
    /// <summary>Process family the native transport has not claimed yet.</summary>
    internal const uint FamilyUnused = 0;
    /// <summary>Process family claimed by every provider except ANR.</summary>
    internal const uint FamilyNormal = 1;
    /// <summary>Process family claimed by ANR, whose hooks stay installed for the whole process.</summary>
    internal const uint FamilyAnr = 2;
    // Streamline initializes once per process and cannot be unloaded, so it owns its own family: a
    // session that loaded either generator never loads the other.
    internal const uint FamilyStreamline = 3;
    internal const uint FlagReversedDepth = 1u << 0;
    internal const uint FlagReset = 1u << 1;
    /// <summary>
    /// Asks the later render event to snapshot the bound render target when no explicit HUD-less
    /// pointer is supplied. SubmitFrame copies metadata only; preceding command-buffer binds execute first.
    /// </summary>
    internal const uint FlagBoundRenderTarget = 1u << 2;

    internal static readonly string[] ConfigFieldOrder =
    {
        nameof(Config.Abi), nameof(Config.Size), nameof(Config.Provider), nameof(Config.Generation),
        nameof(Config.PrimaryCameraId), nameof(Config.PrimaryHistoryEpoch), nameof(Config.CaptureReady),
        nameof(Config.XrActive), nameof(Config.MenuVisible), nameof(Config.Multiplier),
    };

    internal static readonly string[] FrameFieldOrder =
    {
        nameof(Frame.Abi), nameof(Frame.Size), nameof(Frame.Generation), nameof(Frame.SlotTag),
        nameof(Frame.CameraId), nameof(Frame.HistoryEpoch), nameof(Frame.FrameIndex),
        nameof(Frame.RenderWidth), nameof(Frame.RenderHeight), nameof(Frame.OutputWidth),
        nameof(Frame.OutputHeight), nameof(Frame.Flags), nameof(Frame.JitterX), nameof(Frame.JitterY),
        nameof(Frame.DeltaMilliseconds), nameof(Frame.CameraNear), nameof(Frame.CameraFar),
        nameof(Frame.VerticalFovRadians), nameof(Frame.Aspect), nameof(Frame.CameraPosition),
        nameof(Frame.CameraUp), nameof(Frame.CameraRight), nameof(Frame.CameraForward),
        nameof(Frame.CurrentView), nameof(Frame.CurrentProjection), nameof(Frame.PreviousView),
        nameof(Frame.PreviousProjection), nameof(Frame.Depth), nameof(Frame.Motion),
        nameof(Frame.PresentId), nameof(Frame.MotionWidth), nameof(Frame.MotionHeight),
        nameof(Frame.Hudless),
    };

    internal static readonly string[] StatusFieldOrder =
    {
        nameof(Status.Abi), nameof(Status.Size), nameof(Status.State), nameof(Status.RequestedProvider),
        nameof(Status.ActiveProvider), nameof(Status.Generation), nameof(Status.LastResult),
        nameof(Status.LastSourceFrame), nameof(Status.ParentHwnd), nameof(Status.RenderedPresents),
        nameof(Status.GeneratedPresents), nameof(Status.ActiveMultiplier), nameof(Status.MaxMultiplier),
        nameof(Status.RestartRequired), nameof(Status.ProcessFamily), nameof(Status.Reason),
        nameof(Status.RetirementCount), nameof(Status.Reserved), nameof(Status.Retirement0),
    };

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_FG_GetAbiVersion", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint GetAbiVersion();

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_FG_GetFrameSize", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint GetFrameSize();

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_FG_GetConfigSize", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint GetConfigSize();

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_FG_GetStatusSize", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint GetStatusSize();

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_FG_GetConfigLayout", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint GetConfigLayout([Out] uint[] offsets, uint count);

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_FG_GetFrameLayout", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint GetFrameLayout([Out] uint[] offsets, uint count);

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_FG_GetStatusLayout", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint GetStatusLayout([Out] uint[] offsets, uint count);

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_FG_Configure", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Configure(ref Config config);

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_FG_GetStatus", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int GetStatus(ref Status status);

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_FG_GetGameWindow", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr GetGameWindow();

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_FG_CreatePresenterWindow", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int CreatePresenterWindow(IntPtr parent);

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_FG_GetRenderEvent", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr GetRenderEvent();

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_FG_SubmitFrame", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr SubmitFrame(ref Frame frame);

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_FG_BeginFrame", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void BeginFrame(ulong presentId);

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_FG_EndSimulation", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void EndSimulation(ulong presentId);

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_FG_SetPresenterVisible", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SetPresenterVisible(int visible);

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_FG_AckCaptures", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void AckCaptures(uint throughSequence);

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_FG_RequestCapture", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int RequestCapture(uint pairs);

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_FG_Shutdown", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void Shutdown();

    /// <summary>Verifies the FG ABI, sizes and every shared field offset. Returns null on success.</summary>
    internal static string? VerifyLayout()
    {
        if (GetAbiVersion() != AbiVersion)
            return $"FG native ABI {GetAbiVersion()} does not match managed ABI {AbiVersion}";
        var configSize = ConfigSize;
        if (GetConfigSize() != configSize) return $"FG config size mismatch: native {GetConfigSize()} managed {configSize}";
        var frameSize = FrameSize;
        if (GetFrameSize() != frameSize) return $"FG frame size mismatch: native {GetFrameSize()} managed {frameSize}";
        var statusSize = StatusSize;
        if (GetStatusSize() != statusSize) return $"FG status size mismatch: native {GetStatusSize()} managed {statusSize}";

        var error = Verify(nameof(Config), ConfigFieldOrder, GetConfigLayout, n => Marshal.OffsetOf<Config>(n).ToInt64());
        if (error != null) return error;
        error = Verify(nameof(Frame), FrameFieldOrder, GetFrameLayout, n => Marshal.OffsetOf<Frame>(n).ToInt64());
        if (error != null) return error;
        return Verify(nameof(Status), StatusFieldOrder, GetStatusLayout, n => Marshal.OffsetOf<Status>(n).ToInt64());
    }

    private delegate uint LayoutReader(uint[] offsets, uint count);

    private static string? Verify(string label, string[] order, LayoutReader reader, Func<string, long> offset)
    {
        var native = new uint[order.Length];
        var count = reader(native, (uint)native.Length);
        if (count != order.Length) return $"FG {label} field count mismatch: native {count} managed {order.Length}";
        for (var i = 0; i < order.Length; i++)
        {
            var managed = offset(order[i]);
            if (managed != native[i])
                return $"FG {label} field '{order[i]}' offset mismatch: native {native[i]} managed {managed}";
        }
        return null;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    internal struct Config
    {
        public uint Abi;
        public uint Size;
        public int Provider;
        public uint Generation;
        public int PrimaryCameraId;
        public int PrimaryHistoryEpoch;
        public int CaptureReady;
        public int XrActive;
        public int MenuVisible;

        /// <summary>Requested total presented frames per source frame: one real plus M-1 generated.</summary>
        public uint Multiplier;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    internal unsafe struct Frame
    {
        public uint Abi;
        public uint Size;
        public uint Generation;
        public uint SlotTag;
        public int CameraId;
        public int HistoryEpoch;
        public int FrameIndex;
        public int RenderWidth;
        public int RenderHeight;
        public int OutputWidth;
        public int OutputHeight;
        public uint Flags;
        public float JitterX;
        public float JitterY;
        public float DeltaMilliseconds;
        public float CameraNear;
        public float CameraFar;
        public float VerticalFovRadians;
        public float Aspect;
        public fixed float CameraPosition[3];
        public fixed float CameraUp[3];
        public fixed float CameraRight[3];
        public fixed float CameraForward[3];
        public fixed float CurrentView[16];
        public fixed float CurrentProjection[16];
        public fixed float PreviousView[16];
        public fixed float PreviousProjection[16];
        public IntPtr Depth;
        public IntPtr Motion;
        public ulong PresentId;
        public int MotionWidth;
        public int MotionHeight;
        /// <summary>Pre-overlay scene colour, used by providers that extract the HUD.</summary>
        public IntPtr Hudless;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    internal struct Retirement
    {
        public uint Sequence;
        public uint SlotTag;
        public uint Generation;
        public int Result;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    internal unsafe struct Status
    {
        public uint Abi;
        public uint Size;
        public int State;
        public int RequestedProvider;
        public int ActiveProvider;
        public uint Generation;
        public int LastResult;
        public int LastSourceFrame;
        public IntPtr ParentHwnd;
        public ulong RenderedPresents;
        public ulong GeneratedPresents;

        /// <summary>Total multiplier in force for this generation, or 0 while inactive.</summary>
        public uint ActiveMultiplier;

        /// <summary>Maximum total multiplier the selected provider proved, or 0 while unknown.</summary>
        public uint MaxMultiplier;

        /// <summary>Typed restart requirement for the selected provider/generation. Not a reason string.</summary>
        public uint RestartRequired;

        /// <summary>
        /// Process family the native transport claimed: FamilyUnused, FamilyNormal or FamilyAnr. The
        /// first SDK initialization attempt claims it for the rest of the process.
        /// </summary>
        public uint ProcessFamily;
        public fixed byte Reason[256];
        public uint RetirementCount;
        public uint Reserved;
        public Retirement Retirement0;
        public Retirement Retirement1;
        public Retirement Retirement2;
        public Retirement Retirement3;
        public Retirement Retirement4;
        public Retirement Retirement5;
        public Retirement Retirement6;
        public Retirement Retirement7;

        internal string ReasonText()
        {
            fixed (byte* pointer = Reason)
            {
                var length = 0;
                while (length < 256 && pointer[length] != 0) length++;
                return length == 0 ? "" : System.Text.Encoding.UTF8.GetString(pointer, length);
            }
        }
    }
}
