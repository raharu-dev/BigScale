// Converts a device-depth region from URP graph rows into presented-backbuffer rows.
//
// On D3D11, Unity renders into render textures flipped vertically, so row 0 of a render texture is
// the bottom of the image, while row 0 of the swapchain backbuffer is the top. Frame generation
// evaluates colour captured from the backbuffer against these guides, so the rows must match before
// the provider sees them. Load, not Sample: an exact row permutation must not resample anything.
Texture2DArray<float> Source : register(t0);
RWTexture2D<float> Destination : register(u0);

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
    Destination[id.xy] = Source.Load(int4(SourceX + (int)id.x, SourceY + (Height - 1 - (int)id.y), 0, 0));
}
