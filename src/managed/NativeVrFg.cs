using System;
using System.Runtime.InteropServices;

namespace BigScale;

/// <summary>
/// Mirror of the VR frame-generation ABI. It is a separate version number and a single status struct:
/// the managed side only says whether the generator is wanted and reads back what the compositor path
/// proved, so no pointer and no packet crosses this boundary.
/// </summary>
internal static unsafe class NativeVrFg
{
    internal const uint AbiVersion = 1;

    /// <summary>Cached blittable size, built once instead of on every status read.</summary>
    internal static readonly int StatusSize = Marshal.SizeOf<Status>();

    internal const int StateWaiting = 1;
    internal const int StateActive = 2;
    internal const int StateUnsupported = 3;
    internal const int StateFaulted = 4;

    internal static readonly string[] StatusFieldOrder =
    {
        nameof(Status.Abi), nameof(Status.Size), nameof(Status.State), nameof(Status.LastResult),
        nameof(Status.DisplayHz), nameof(Status.MotionSmoothing), nameof(Status.RealFrames),
        nameof(Status.GeneratedFrames), nameof(Status.RepeatedSlots), nameof(Status.DroppedFrames),
        nameof(Status.PacingTimeouts), nameof(Status.Reason),
    };

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_VRFG_GetAbiVersion", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint GetAbiVersion();

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_VRFG_GetStatusSize", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint GetStatusSize();

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_VRFG_GetStatusLayout", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint GetStatusLayout([Out] uint[] offsets, uint count);

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_VRFG_Configure", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Configure(int enabled);

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_VRFG_GetStatus", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int GetStatus(ref Status status);

    [DllImport(Native.Dll, EntryPoint = "BIGSCALE_VRFG_Shutdown", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void Shutdown();

    /// <summary>Verifies the VRFG ABI, the status size and every shared field offset.</summary>
    internal static string? VerifyLayout()
    {
        if (GetAbiVersion() != AbiVersion)
            return $"VRFG native ABI {GetAbiVersion()} does not match managed ABI {AbiVersion}";
        var size = StatusSize;
        if (GetStatusSize() != size)
            return $"VRFG status size mismatch: native {GetStatusSize()} managed {size}";
        var native = new uint[StatusFieldOrder.Length];
        var count = GetStatusLayout(native, (uint)native.Length);
        if (count != StatusFieldOrder.Length)
            return $"VRFG status field count mismatch: native {count} managed {StatusFieldOrder.Length}";
        for (var i = 0; i < StatusFieldOrder.Length; i++)
        {
            var managed = Marshal.OffsetOf<Status>(StatusFieldOrder[i]).ToInt64();
            if (managed != native[i])
                return $"VRFG status field '{StatusFieldOrder[i]}' offset mismatch: native {native[i]} " +
                       $"managed {managed}";
        }
        return null;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    internal struct Status
    {
        public uint Abi;
        public uint Size;
        public int State;
        public int LastResult;
        public float DisplayHz;
        public uint MotionSmoothing;
        public ulong RealFrames;
        public ulong GeneratedFrames;
        public ulong RepeatedSlots;
        public ulong DroppedFrames;
        public ulong PacingTimeouts;
        public fixed byte Reason[256];

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
