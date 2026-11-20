// BigScale - the one piece of evidence that says which pixels SteamVR actually displayed.
//
// Only active on a launch that sets BIGSCALE_XR_PROOF=1, which the managed side derives from
// Diagnostics/XrProof. While the managed side keeps the stage
// probe flag on, each eye's output region carries an identity bar (vertical for eye 0, horizontal for
// eye 1). This module reads the compositor's undistorted mirror images through OpenVR, saves a
// bounded set of crops around that bar while the tags are stamped and again after they stop, and
// names every file with the frame it came from. A duplicated slice, a swapped eye, or an image that
// never reaches the compositor is then visible in the saved pairs instead of being inferred from
// texture names, dimensions or a successful provider call.
//
// Nothing here is on the production path: it never submits a texture, never hooks Submit, never waits
// for poses, and never enables frame generation.
#pragma once

#include <d3d11.h>

#include <cstdint>

namespace bigscale
{
namespace XrPresentationProof
{
// True when this launch asked for the presentation proof.
bool Requested();

// Called once per successfully written eye on the render thread, inside the bridge render mutex.
// `tagged` is true while the identity bars are being stamped. Starts the mirror capture on the first
// tagged eye, collects a bounded number of tagged and untagged pairs, then releases the mirror views.
void ObserveFrame(ID3D11Device* device, ID3D11DeviceContext* context, int32_t cameraId, int32_t eye,
                  int32_t frameIndex, bool tagged);

// Releases every mirror view and staging texture. The caller guarantees the GPU is idle.
void Release();
} // namespace XrPresentationProof
} // namespace bigscale
