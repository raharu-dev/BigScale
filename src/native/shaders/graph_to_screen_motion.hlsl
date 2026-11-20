// Converts a URP motion-vector region from graph rows into presented-backbuffer rows.
//
// On D3D11, Unity renders into render textures flipped vertically, so row 0 of a render texture is
// the bottom of the image, while row 0 of the swapchain backbuffer is the top. Frame generation
// evaluates colour captured from the backbuffer against these guides, so the rows must match before
// the provider sees them. Reversing the row order also reverses the vertical screen direction, so
// the y component of the UV displacement is negated to keep it pointing at the same image content.
Texture2D<float2> Source : register(t0);
RWTexture2D<float2> Destination : register(u0);

cbuffer Params : register(b0)
{
    int SourceX;
    int SourceY;
    int Width;
    int Height;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)Width || id.y >= (uint)Height) return;
    const float2 v = Source.Load(int3(SourceX + (int)id.x, SourceY + (Height - 1 - (int)id.y), 0));
    Destination[id.xy] = float2(v.x, -v.y);
}
