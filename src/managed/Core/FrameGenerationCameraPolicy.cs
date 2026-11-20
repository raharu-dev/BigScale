namespace BigScale.Core;

/// <summary>
/// One camera's frame-generation eligibility inputs, reduced to the values the decision depends on.
/// The caller fills this from <c>UniversalCameraData</c> once per rendered graph; nothing here reads
/// a Unity object, so the decision stays deterministic and testable off-engine.
/// </summary>
public readonly struct FrameGenerationCameraInput
{
    /// <summary>The camera object exists, is enabled and is active.</summary>
    public bool Live { get; init; }

    /// <summary><c>cameraData.cameraType == CameraType.Game</c>.</summary>
    public bool GameCamera { get; init; }

    /// <summary>
    /// The game's own world/local-player ownership names this camera as the gameplay camera. URP
    /// properties alone cannot tell a title or loading camera from the one the player controls.
    /// </summary>
    public bool GameplayActive { get; init; }

    public bool Preview { get; init; }

    public bool SceneView { get; init; }

    /// <summary><c>cameraData.renderType == CameraRenderType.Base</c>.</summary>
    public bool BaseCamera { get; init; }

    /// <summary>Overlay cameras assigned to this base camera's stack.</summary>
    public int CameraStackCount { get; init; }

    /// <summary>The camera renders into its own render texture instead of the screen.</summary>
    public bool HasTargetTexture { get; init; }

    /// <summary>URP's own "this camera resolves its final target to the screen" decision.</summary>
    public bool ResolveToScreen { get; init; }

    /// <summary>No base camera renders after this one in the frame.</summary>
    public bool LastBaseCamera { get; init; }

    /// <summary>The pipeline presents real HDR output to the display.</summary>
    public bool HdrDisplayOutput { get; init; }

    public bool XrEnabled { get; init; }

    /// <summary>Active render extent from <c>cameraData.cameraTargetDescriptor</c>.</summary>
    public int RenderWidth { get; init; }

    public int RenderHeight { get; init; }

    /// <summary>Presented extent from <c>cameraData.pixelWidth/pixelHeight</c>.</summary>
    public int OutputWidth { get; init; }

    public int OutputHeight { get; init; }
}

/// <summary>
/// Pure frame-generation camera policy.
///
/// Frame generation captures the one graph that produces the image the player is looking at, and it
/// publishes that camera's identity to the native side. The decision is deliberately independent of
/// super resolution: the remembered FG provider must run while SR is off, faulted or on a different
/// camera, so "is this the SR primary" is never part of it.
///
/// An internal HDR colour buffer is not disqualifying. The HUD-less source is the post-blit
/// backbuffer, which URP has already encoded for an SDR presentation; only an actual HDR *display*
/// output changes what the swapchain holds, and that is the case this policy rejects.
/// </summary>
public static class FrameGenerationCameraPolicy
{
    /// <summary>True when this camera's graph may be captured for frame generation.</summary>
    /// <param name="camera">The snapshot taken from the rendered camera.</param>
    /// <param name="reason">The first failed requirement, or an empty string when accepted.</param>
    public static bool Accept(in FrameGenerationCameraInput camera, out string reason)
    {
        reason = "";
        if (!camera.Live)
        {
            reason = "the camera is not live and enabled";
            return false;
        }
        if (!camera.GameCamera)
        {
            reason = "the camera is not a game camera";
            return false;
        }
        if (!camera.GameplayActive)
        {
            reason = "frame generation is suspended outside gameplay";
            return false;
        }
        if (camera.Preview)
        {
            reason = "the camera is a preview camera";
            return false;
        }
        if (camera.SceneView)
        {
            reason = "the camera is the scene view";
            return false;
        }
        if (!camera.BaseCamera)
        {
            reason = "the camera is an overlay camera in a stack";
            return false;
        }
        if (camera.CameraStackCount > 0)
        {
            reason = $"the camera owns a stack of {camera.CameraStackCount} overlay camera(s)";
            return false;
        }
        if (camera.XrEnabled)
        {
            reason = "the camera renders XR views; frame generation stays suspended in VR";
            return false;
        }
        if (camera.HasTargetTexture)
        {
            reason = "the camera renders into its own render texture instead of the screen";
            return false;
        }
        if (!camera.ResolveToScreen)
        {
            reason = "the camera does not resolve its final target to the screen";
            return false;
        }
        if (!camera.LastBaseCamera)
        {
            reason = "another base camera renders after this one";
            return false;
        }
        if (camera.HdrDisplayOutput)
        {
            reason = "the camera presents HDR output to the display";
            return false;
        }
        if (camera.RenderWidth <= 0 || camera.RenderHeight <= 0)
        {
            reason = $"the render extent is {camera.RenderWidth}x{camera.RenderHeight}";
            return false;
        }
        if (camera.OutputWidth <= 0 || camera.OutputHeight <= 0)
        {
            reason = $"the output extent is {camera.OutputWidth}x{camera.OutputHeight}";
            return false;
        }
        return true;
    }
}
