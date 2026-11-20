using System;

namespace BigScale.Core;

/// <summary>What the mod has to change on one player renderer for its motion to be correct.</summary>
[Flags]
public enum PlayerMotionAction
{
    /// <summary>URP's own motion-vector pass already handles this renderer, or it does not draw into it.</summary>
    None = 0,

    /// <summary>The renderer is in <c>Camera</c> mode; set it to <c>Object</c> so URP draws its real motion.</summary>
    PromoteToObject = 1,

    /// <summary>
    /// No material has a <c>MotionVectors</c> pass; the mod appends a motion-only material so URP draws
    /// the renderer's real object motion.
    /// </summary>
    AttachMotionMaterial = 2,
}

public readonly struct PlayerRendererMotion
{
    /// <summary>The renderer is a <c>MeshRenderer</c> or <c>SkinnedMeshRenderer</c>.</summary>
    public bool MeshOrSkinned { get; init; }

    /// <summary><c>renderer.motionVectorGenerationMode == MotionVectorGenerationMode.Camera</c>.</summary>
    public bool CameraMode { get; init; }

    /// <summary>
    /// The game draws this renderer as a first-person decoration and never writes its colour, even
    /// though the geometry is live and sits in front of the camera. Correcting its motion would paint a
    /// motion patch over whatever the player is actually looking at.
    /// </summary>
    public bool Hidden { get; init; }

    /// <summary>Non-null entries in <c>renderer.sharedMaterials</c>, excluding the mod's own material.</summary>
    public int MaterialCount { get; init; }

    /// <summary>Materials whose shader has a pass tagged <c>MotionVectors</c>.</summary>
    public int MaterialsWithMotionPass { get; init; }

    /// <summary>Highest <c>Material.renderQueue</c> across the renderer's materials.</summary>
    public int MaxRenderQueue { get; init; }

    /// <summary>The mesh's <c>subMeshCount</c>, or 0 when the renderer has no mesh.</summary>
    public int SubmeshCount { get; init; }
}

/// <summary>
/// Pure classification of one player renderer against the motion URP will actually draw for it.
///
/// URP's motion-vector pass draws every renderer in the camera's culled opaque set whose material has a
/// <c>MotionVectors</c> pass. The game's player shaders (<c>househouse/PlayerCharacter</c>,
/// <c>househouse/PlayerNose</c>) have no such pass, so those pixels keep the full-screen camera motion
/// and reproject as if they were static world geometry: a forward-displaced body ghosts in every
/// generated frame, and a remote player walking past does the same.
///
/// The repair has two halves. Renderers that already have the pass but sit in <c>Camera</c> mode are
/// promoted to <c>Object</c>, which is all URP needs for their real per-object motion. Renderers
/// without the pass get one extra material appended: a stock Lit instance with every pass but
/// <c>MotionVectors</c> disabled. The renderer then has a motion pass, so URP draws it from its own
/// culled list with real previous matrices, the camera's culling mask, LOD selection and shadow
/// visibility, and the appended material draws nothing in any other pass.
///
/// Unity draws materials beyond the submesh count on the last submesh, so an appended material covers
/// exactly one submesh. A renderer with several materials or several submeshes would get motion on one
/// part and camera motion on the rest, which is worse than leaving the whole thing alone.
/// </summary>
public static class PlayerMotionPolicy
{
    /// <summary>Upper bound of <c>RenderQueueRange.opaque</c> in URP.</summary>
    public const int OpaqueQueueMax = 2500;

    public static PlayerMotionAction Classify(in PlayerRendererMotion renderer)
    {
        if (renderer.Hidden || !renderer.MeshOrSkinned)
        {
            return PlayerMotionAction.None;
        }
        if (renderer.MaterialCount <= 0)
        {
            return PlayerMotionAction.None;
        }
        if (renderer.MaxRenderQueue > OpaqueQueueMax)
        {
            return PlayerMotionAction.None;
        }
        if (renderer.MaterialsWithMotionPass == renderer.MaterialCount)
        {
            return renderer.CameraMode ? PlayerMotionAction.PromoteToObject : PlayerMotionAction.None;
        }
        if (renderer.MaterialsWithMotionPass != 0)
        {
            // Some submeshes already write correct motion; appending could only cover the last one.
            return PlayerMotionAction.None;
        }
        if (renderer.MaterialCount != 1 || renderer.SubmeshCount != 1)
        {
            // The appended material renders on the last submesh only.
            return PlayerMotionAction.None;
        }
        return PlayerMotionAction.AttachMotionMaterial;
    }
}
