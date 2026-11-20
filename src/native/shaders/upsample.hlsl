// Explicit non-DLSS spatial comparison and failure baseline. Produces a well-formed, full-size image
// for the current view so a failing reconstruction frame is never presented as DLSS and never black.
//
// The source is read directly, so the baseline is available before any per-eye extraction succeeds.
// It may be a scratch texture whose valid region starts at its origin, or the original Unity colour
// texture whose view region starts at (SourceX, SourceY). The sample position is clamped to that
// region: without the clamp the last output texel would blend with pixels the view does not own.
Texture2D<float4> Source : register(t0);
SamplerState LinearSampler : register(s0);
RWTexture2D<float4> Destination : register(u0);

cbuffer Params : register(b0)
{
    float ScaleX;       // inputWidth / outputWidth
    float ScaleY;       // inputHeight / outputHeight
    float SourceWidth;  // full source texture width
    float SourceHeight; // full source texture height
    float OutputWidth;
    float OutputHeight;
    float SourceX;      // left edge of the view region inside the source
    float SourceY;      // top edge of the view region inside the source
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)OutputWidth || id.y >= (uint)OutputHeight) return;
    const float2 scale = float2(ScaleX, ScaleY);
    const float2 regionOrigin = float2(SourceX, SourceY);
    const float2 regionSize = float2(OutputWidth, OutputHeight) * scale;
    const float2 sampled = (float2(id.xy) + 0.5) * scale;
    const float2 source = clamp(sampled, float2(0.5, 0.5), regionSize - 0.5) + regionOrigin;
    const float2 uv = source / float2(SourceWidth, SourceHeight);
    Destination[id.xy] = Source.SampleLevel(LinearSampler, uv, 0);
}
