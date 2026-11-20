#include "TexelReader.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace bigscale
{
namespace
{
float HalfToFloat(uint16_t bits)
{
    const uint32_t sign = bits >> 15;
    const uint32_t exponent = (bits >> 10) & 0x1F;
    const uint32_t mantissa = bits & 0x3FF;
    float value;
    if (exponent == 0)
        value = mantissa == 0 ? 0.0f : static_cast<float>(std::ldexp(static_cast<double>(mantissa), -24));
    else if (exponent == 31)
        value = mantissa == 0 ? INFINITY : NAN;
    else
        value = static_cast<float>(std::ldexp(static_cast<double>(0x400 | mantissa),
            static_cast<int>(exponent) - 25));
    return sign ? -value : value;
}

// The unsigned 5-bit-exponent float R11G11B10_FLOAT is built from: no sign bit, bias 15, and an
// all-ones exponent meaning inf/NaN.
float UnsignedFloat(uint32_t bits, uint32_t mantissaBits)
{
    const uint32_t exponent = bits >> mantissaBits;
    const uint32_t mantissa = bits & ((1u << mantissaBits) - 1u);
    if (exponent == 0)
        return static_cast<float>(std::ldexp(static_cast<double>(mantissa), -14 - static_cast<int>(mantissaBits)));
    if (exponent == 31) return mantissa == 0 ? INFINITY : NAN;
    const double normalized = 1.0 + static_cast<double>(mantissa) / static_cast<double>(1u << mantissaBits);
    return static_cast<float>(std::ldexp(normalized, static_cast<int>(exponent) - 15));
}

uint32_t TexelBytes(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return 4;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
            return 8;
        default:
            return 0;
    }
}

uint32_t LoadU32(const void* texel)
{
    uint32_t value = 0;
    std::memcpy(&value, texel, sizeof(value));
    return value;
}

void LoadHalfs(const void* texel, int count, float* out)
{
    for (int i = 0; i < count; i++)
    {
        uint16_t half = 0;
        std::memcpy(&half, static_cast<const uint8_t*>(texel) + i * sizeof(half), sizeof(half));
        out[i] = HalfToFloat(half);
    }
}

float LoadF32(const void* texel)
{
    float value = 0.0f;
    std::memcpy(&value, texel, sizeof(value));
    return value;
}
} // namespace

bool DecodeTexel(DXGI_FORMAT format, const void* texel, float out[4])
{
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 1.0f;
    if (texel == nullptr) return false;
    switch (format)
    {
        case DXGI_FORMAT_R11G11B10_FLOAT:
        {
            const uint32_t bits = LoadU32(texel);
            out[0] = UnsignedFloat(bits & 0x7FFu, 6);
            out[1] = UnsignedFloat((bits >> 11) & 0x7FFu, 6);
            out[2] = UnsignedFloat((bits >> 22) & 0x3FFu, 5);
            return true;
        }
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            LoadHalfs(texel, 4, out);
            return true;
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:
            LoadHalfs(texel, 2, out);
            return true;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        {
            const float value = LoadF32(texel);
            out[0] = out[1] = out[2] = value;
            return true;
        }
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        {
            const float value = static_cast<float>(LoadU32(texel) & 0xFFFFFFu) / 16777215.0f;
            out[0] = out[1] = out[2] = value;
            return true;
        }
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        {
            const uint32_t bits = LoadU32(texel);
            out[0] = static_cast<float>(bits & 0xFFu) / 255.0f;
            out[1] = static_cast<float>((bits >> 8) & 0xFFu) / 255.0f;
            out[2] = static_cast<float>((bits >> 16) & 0xFFu) / 255.0f;
            out[3] = static_cast<float>(bits >> 24) / 255.0f;
            return true;
        }
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        {
            const uint32_t bits = LoadU32(texel);
            out[0] = static_cast<float>((bits >> 16) & 0xFFu) / 255.0f;
            out[1] = static_cast<float>((bits >> 8) & 0xFFu) / 255.0f;
            out[2] = static_cast<float>(bits & 0xFFu) / 255.0f;
            out[3] = static_cast<float>(bits >> 24) / 255.0f;
            return true;
        }
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        {
            const uint32_t bits = LoadU32(texel);
            out[0] = static_cast<float>(bits & 0x3FFu) / 1023.0f;
            out[1] = static_cast<float>((bits >> 10) & 0x3FFu) / 1023.0f;
            out[2] = static_cast<float>((bits >> 20) & 0x3FFu) / 1023.0f;
            out[3] = static_cast<float>(bits >> 30) / 3.0f;
            return true;
        }
        default:
            return false;
    }
}

bool ReadTexels(ID3D11DeviceContext* context, ID3D11Texture2D* texture, UINT slice,
                const UINT (*points)[2], UINT count, float (*out)[4], const char** reason)
{
    if (out != nullptr)
        for (UINT i = 0; i < count; i++)
            out[i][0] = out[i][1] = out[i][2] = out[i][3] = 0.0f;
    if (reason != nullptr) *reason = "null";
    if (context == nullptr || texture == nullptr || points == nullptr || out == nullptr) return false;

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (reason != nullptr) *reason = "format";
    const uint32_t texelBytes = TexelBytes(desc.Format);
    if (texelBytes == 0) return false;
    if (reason != nullptr) *reason = "msaa";
    if (desc.SampleDesc.Count > 1) return false;
    if (reason != nullptr) *reason = "slice";
    if (slice >= desc.ArraySize) return false;
    if (reason != nullptr) *reason = "point";
    for (UINT i = 0; i < count; i++)
        if (points[i][0] >= desc.Width || points[i][1] >= desc.Height) return false;
    if (count == 0) return true;

    if (reason != nullptr) *reason = "device";
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device) return false;

    if (reason != nullptr) *reason = "create";
    D3D11_TEXTURE2D_DESC staging{};
    staging.Format = desc.Format;
    staging.MipLevels = 1;
    staging.ArraySize = 1;
    staging.SampleDesc.Count = 1;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    const UINT sourceSubresource = D3D11CalcSubresource(0, slice, desc.MipLevels);
    ComPtr<ID3D11Texture2D> copy;

    // D3D11 rejects a source box for a depth-stencil subresource, so that case copies the whole
    // subresource and reads the wanted points out of the mapped rows instead.
    if ((desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0)
    {
        staging.Width = desc.Width;
        staging.Height = desc.Height;
        if (FAILED(device->CreateTexture2D(&staging, nullptr, &copy))) return false;
        context->CopySubresourceRegion(copy.Get(), 0, 0, 0, 0, texture, sourceSubresource, nullptr);
        if (reason != nullptr) *reason = "map";
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        bool decoded = true;
        for (UINT i = 0; i < count && decoded; i++)
        {
            const auto* texel = static_cast<const uint8_t*>(mapped.pData) +
                static_cast<size_t>(points[i][1]) * mapped.RowPitch + static_cast<size_t>(points[i][0]) * texelBytes;
            decoded = DecodeTexel(desc.Format, texel, out[i]);
        }
        context->Unmap(copy.Get(), 0);
        if (!decoded && reason != nullptr) *reason = "format";
        return decoded;
    }

    staging.Width = count;
    staging.Height = 1;
    if (FAILED(device->CreateTexture2D(&staging, nullptr, &copy))) return false;
    for (UINT i = 0; i < count; i++)
    {
        const D3D11_BOX box{points[i][0], points[i][1], 0, points[i][0] + 1, points[i][1] + 1, 1};
        context->CopySubresourceRegion(copy.Get(), 0, i, 0, 0, texture, sourceSubresource, &box);
    }
    if (reason != nullptr) *reason = "map";
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    bool decoded = true;
    for (UINT i = 0; i < count && decoded; i++)
    {
        const auto* texel = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(i) * texelBytes;
        decoded = DecodeTexel(desc.Format, texel, out[i]);
    }
    context->Unmap(copy.Get(), 0);
    if (!decoded && reason != nullptr) *reason = "format";
    return decoded;
}
} // namespace bigscale
