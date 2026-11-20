// BigScale — diagnostics that locate which stage of the XR frame stops changing.
//
// Three independent signals are produced per (camera, eye) while diagnostics are enabled:
//  * an opt-in magenta identity band stamped into the engine output region at 10% of the eye's extent:
//    a full-height vertical band for eye 0 and a full-width horizontal band for eye 1, so a compositor
//    image can be matched to the eye that produced it from pixels alone - and a band cannot hide in a
//    lens corner the way a small square could;
//  * hashes of the active texels of a 256x256 crop sampled from the reconstruction input and from
//    the written engine output, so the log can say which of the two stops changing when the image
//    freezes;
//  * for the two sampled operations at ordinals 120 and 600 only, a top-down 24-bit BMP of the
//    input crop and one of the output crop, so the pixels themselves can be inspected instead of
//    being inferred from a hash.
//
// Nothing here runs unless the packet carries BIGSCALE_FLAG_STAGE_PROBE, and every sample belongs to a
// successful image operation: a provider evaluation, or the bilinear comparison that uses the same
// input binding, output texture and write-back without invoking the provider. The identity band
// additionally needs BIGSCALE_FLAG_STAGE_MARKER and raises the sample rate to once per frame. Sampling
// is bounded (one 256-pixel crop per operation, at most four images per entry lifetime for the
// scheduled ordinals) and every failure disables the affected signal for that view instead of faulting
// the bridge.
#pragma once

#include <d3d11.h>

#include <cstdint>

namespace bigscale
{
namespace StageProbe
{
// Samples and marks one successfully written eye. `inputScratch` is the converted reconstruction
// input, `outputTexture`/`outputSlice` the engine texture and subresource slice the write-back
// filled. `outputX/Y/Width/Height` are the same region the packet declared. Must run on the render
// thread with the bridge render mutex held, after the input and the output both hold this frame.
// `marker` stamps this eye's identity bar and raises the sample rate to one frame, which is the bounded
// burst the presentation proof uses. Sampling, hashes and the crop images do not depend on it.
void AfterWriteBack(ID3D11Device* device, ID3D11DeviceContext* context, int32_t cameraId, int32_t eye,
                    ID3D11Texture2D* inputScratch, ID3D11Texture2D* outputTexture, uint32_t outputSlice,
                    uint32_t outputX, uint32_t outputY, uint32_t outputWidth, uint32_t outputHeight, bool marker);

// Identifies this eye's identity bar inside a radius around (centerX, centerY) of one RGBA image:
// 1 for the eye-0 vertical bar, 2 for the eye-1 horizontal bar, 0 when neither is present. Used by the
// offline checks to reject a slice that was duplicated into both eyes.
// Computed bounds describe the detected bar, so a caller can prove it stayed clear of a sampled region.
int ClassifyIdentityMarker(const float* rgba, uint32_t width, uint32_t height, uint32_t strideTexels,
                          float centerX, float centerY, float radius, int32_t* outLeft = nullptr,
                          int32_t* outTop = nullptr, int32_t* outRight = nullptr, int32_t* outBottom = nullptr);

// Releases every staging and marker texture. The caller guarantees the GPU has finished with them.
void Reset();
} // namespace StageProbe
} // namespace bigscale
