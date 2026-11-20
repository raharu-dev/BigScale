// Converts an RGBA region from URP graph rows into presented-backbuffer rows, encoding the
// linear-light graph colour into the backbuffer's display transfer function.
//
// On D3D11, Unity renders into render textures flipped vertically, so row 0 of a render texture is
// the bottom of the image, while row 0 of the swapchain backbuffer is the top. Frame generation
// mixes graph-sourced colour (the HUD-less plane) with backbuffer colour, and every consumer
// compares those planes pixel by pixel to separate UI from scene colour. A URP graph colour target
// holds linear-light values while the presented backbuffer is sRGB-encoded, so the rows and the
// transfer function must both be converted before a provider sees the plane.
// Load, not Sample: an exact row permutation must not resample anything.
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Destination : register(u0);

cbuffer Params : register(b0)
{
    int SourceX;
    int SourceY;
    int Width;
    int Height;
};

float3 LinearToSrgb(float3 value)
{
    value = max(value, 0.0);
    const float3 low = value * 12.92;
    const float3 high = 1.055 * pow(value, 1.0 / 2.4) - 0.055;
    return lerp(low, high, step(0.0031308, value));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)Width || id.y >= (uint)Height) return;
    const float4 value = Source.Load(int3(SourceX + (int)id.x, SourceY + (Height - 1 - (int)id.y), 0));
    // Alpha is a coverage channel, not a colour, so it is never passed through the transfer.
    Destination[id.xy] = float4(LinearToSrgb(value.rgb), value.a);
}
