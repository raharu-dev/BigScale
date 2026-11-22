// DLSS5 Neural Rendering composite.
//
// The model answer is blended against the upscaler's own input through the mod-owned character
// mask. White marks a character pixel, so the original image wins there and the neural answer wins
// everywhere else. Without a mask the neural answer is used verbatim, never the original.
//
// The mask is drawn at the output extent while this kernel may write the input extent (neural
// rendering before upscaling), so a bilinear sample is what downsamples it; the clamp addressing
// keeps the edge texel readable when the two extents are not an exact multiple.
Texture2D<float4> ModelAnswer : register(t0);
Texture2D<float4> Original : register(t1);
Texture2D<float> CharacterMask : register(t2);
SamplerState LinearSampler : register(s0);
RWTexture2D<float4> Destination : register(u0);

cbuffer Params : register(b0)
{
    uint Width;
    uint Height;
    float HasMask;
    float Pad;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;
    const float2 extent = float2(Width, Height);
    const float2 uv = (float2(id.xy) + 0.5) / extent;
    const float4 answer = ModelAnswer.SampleLevel(LinearSampler, uv, 0);
    if (HasMask < 0.5)
    {
        Destination[id.xy] = answer;
        return;
    }
    const float4 original = Original.SampleLevel(LinearSampler, uv, 0);
    const float mask = CharacterMask.SampleLevel(LinearSampler, uv, 0);
    const float blend = smoothstep(0.25, 0.75, mask);
    Destination[id.xy] = lerp(answer, original, blend);
}
