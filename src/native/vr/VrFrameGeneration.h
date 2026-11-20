// VR frame generation in the OpenVR compositor cycle.
//
// The VR mod's XRSDKOpenVR fork owns presentation: it submits one stereo array to
// IVRCompositor_029 every Unity frame and asks WaitGetPoses for the next pair. To insert generated
// frames without a second presenter, this module takes over the real compositor cycle on a dedicated
// presenter thread and gives the game a virtual half-rate WaitGetPoses, which is the same design the
// OpenXR bridge this feature is adapted from uses.
#pragma once

#include "../Bridge.h"
#include "../Upscaler.h"
#include "../VrFrameGenerationAbi.h"

#include <d3d11.h>

namespace bigscale
{
// True while the guide textures published by the super-resolution dispatch are consumed.
bool VrFgWantsGuides();

// Publishes one eye's depth and motion for the game frame being rendered. Called on the Unity render
// thread from the super-resolution dispatch, after the eye image was written back.
void VrFgPublishGuides(ID3D11DeviceContext* context, const BIGSCALE_Frame& frame, const EvaluationRequest& request,
                       ID3D11Resource* depth, ID3D11Resource* motion);

// Managed entry points.
void VrFgConfigure(bool enabled);
void VrFgShutdown();
void VrFgFillStatus(BIGSCALE_VRFG_Status& status);

#if defined(BIGSCALE_TESTING)
void VrFgTestUseInterfaces(void* compositor, void* system, void* settings);
#endif
}
