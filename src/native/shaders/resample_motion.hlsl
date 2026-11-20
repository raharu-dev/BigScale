// Resamples a validated motion-vector region into the active input extent without changing its
// sign or scale. Bilinear filtering across a half-resolution field is deliberate: the values are
// continuous UV displacements, and the destination is the SDK's render-size motion input.
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
    const float2 uv = float2(StartU + id.x * StepU, StartV + id.y * StepV);
    Destination[id.xy] = Source.SampleLevel(LinearSampler, uv, 0);
}
