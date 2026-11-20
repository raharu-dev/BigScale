// Copies a region of an RGBA-capable source into a tight RGBA16F scratch texture.
// Used only when a fast CopySubresourceRegion is illegal (format change).
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Destination : register(u0);

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
    Destination[id.xy] = Source.Load(int3(SourceX + (int)id.x, SourceY + (int)id.y, 0));
}
