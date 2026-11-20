using BigScale.Core;
using UnityEngine.XR;

namespace BigScale;

/// <summary>
/// One process-wide VR verdict, published independently of renderer readiness, the master switch and
/// whether a camera has been observed yet.
///
/// The explicit launcher switch is cached once: it cannot change during a process. The runtime's own
/// device signal is consulted on every read because the session can start after the plugin loaded.
/// A process launch request therefore stays authoritative even when a mirror camera reports that it
/// is not an XR camera.
/// </summary>
internal static class VrState
{
    private static readonly bool Requested = VrMode.IsRequested(System.Environment.GetCommandLineArgs());

    /// <summary>
    /// True when this process is a VR session. A runtime that cannot answer yet is treated as "not
    /// active" but never cancels an explicit launcher request.
    /// </summary>
    internal static bool Active
    {
        get
        {
            if (Requested) return true;
            try
            {
                return XRSettings.isDeviceActive;
            }
            catch
            {
                // XR may not be initialised on a pure desktop launch; that is not a VR session.
                return false;
            }
        }
    }
}
