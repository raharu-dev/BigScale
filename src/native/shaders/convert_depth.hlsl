// Copies a device-depth region bit-exactly. Required when a depth-stencil sub-box copy is
// illegal because the view occupies part of a shared target.
Texture2D<float> Source : register(t0);
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
    Destination[id.xy] = Source.Load(int3(SourceX + (int)id.x, SourceY + (int)id.y, 0));
}
