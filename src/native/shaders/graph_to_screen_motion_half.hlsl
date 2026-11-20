// Resamples a validated motion-vector region into the active input extent, converting its rows from
// URP graph space into presented-backbuffer space on the way.
//
// On D3D11, Unity renders into render textures flipped vertically, so row 0 of a render texture is
// the bottom of the image, while row 0 of the swapchain backbuffer is the top. Frame generation
// evaluates colour captured from the backbuffer against these guides, so the rows must match before
// the provider sees them. Reversing the row order also reverses the vertical screen direction, so
// the y component of the UV displacement is negated to keep it pointing at the same image content.
// Bilinear filtering across a half-resolution field is deliberate: the values are continuous UV
// displacements, and the destination is the SDK's render-size motion input.
Texture2D<float2> Source : register(t0);
SamplerState LinearSampler : register(s0);
RWTexture2D<float2> Destination : register(u0);

cbuffer Params : register(b0)
{
    float StartU;
    float StartV;
    float StepU;
    float StepV;
    float Width;
    float Height;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)Width || id.y >= (uint)Height) return;
    const float2 uv = float2(StartU + id.x * StepU, StartV + (Height - 1.0 - id.y) * StepV);
    const float2 v = Source.SampleLevel(LinearSampler, uv, 0);
    Destination[id.xy] = float2(v.x, -v.y);
}
