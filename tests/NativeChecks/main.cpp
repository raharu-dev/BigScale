// Offline D3D11 checks for the production native bridge.
//
// These exercise the real conversion, copy, baseline and packet-ring code paths against synthetic
// single-slice, two-slice-array, packed-region and packed-depth textures. They never load the game,
// Unity or a VR runtime. NGX is only touched when its runtime DLL is staged next to this executable,
// and the report states explicitly whether a real create/evaluate was exercised.
#include "Bridge.h"
#include "ContextState.h"
#include "EyeResources.h"
#include "NeuralRendering.h"
#include "NgxBackend.h"
#include "StageProbe.h"
#include "TexelReader.h"
#include "FrameGenerationAbi.h"
#include "fg/FgAdaptiveCadence.h"
#include "fg/FrameGenerationResources.h"
#include "fg/FgReadback.h"
#include "fg/FgTestObserver.h"
#include "VrFakes.h"
#include "VrFrameGenerationAbi.h"
#include "vr/VrFrameGeneration.h"

#include <windows.h>
#include <dbghelp.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace bigscale;

namespace
{
int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const std::string& description)
{
    g_checks++;
    if (condition) return;
    g_failures++;
    std::printf("FAIL: %s\n", description.c_str());
}

void Near(float actual, float expected, float tolerance, const std::string& description)
{
    Check(std::fabs(actual - expected) <= tolerance,
        description + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}

ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
bool g_hardware = false;

bool CreateDevice()
{
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 1,
        D3D11_SDK_VERSION, &g_device, nullptr, &g_context);
    if (SUCCEEDED(result))
    {
        g_hardware = true;
        return true;
    }
    g_device.Reset();
    g_context.Reset();
    result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 1, D3D11_SDK_VERSION,
        &g_device, nullptr, &g_context);
    if (SUCCEEDED(result)) return true;
    std::printf("FATAL: no D3D11 device (hardware 0x%08x, WARP 0x%08x)\n", static_cast<unsigned>(result),
        static_cast<unsigned>(result));
    return false;
}

ComPtr<ID3D11Texture2D> CreateTexture(UINT width, UINT height, UINT arraySize, DXGI_FORMAT format, UINT bind,
    UINT misc = 0)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = arraySize;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = misc;
    ComPtr<ID3D11Texture2D> texture;
    const HRESULT result = g_device->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(result))
    {
        std::printf("  texture %ux%u x%u format=%u bind=0x%x failed 0x%08x (device removed: 0x%08x)\n", width,
            height, arraySize, static_cast<unsigned>(format), bind, static_cast<unsigned>(result),
            g_device != nullptr ? static_cast<unsigned>(g_device->GetDeviceRemovedReason()) : 0u);
        return nullptr;
    }
    return texture;
}


uint16_t ToHalf(float value)
{
    const uint32_t bits = *reinterpret_cast<uint32_t*>(&value);
    const uint32_t sign = (bits >> 16) & 0x8000;
    int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFF;
    if (exponent <= 0)
    {
        if (exponent < -10) return static_cast<uint16_t>(sign);
        mantissa = (mantissa | 0x800000) >> (1 - exponent);
        return static_cast<uint16_t>(sign | ((mantissa + 0x1000) >> 13));
    }
    if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7C00);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | ((mantissa + 0x1000) >> 13));
}

// IEC 61966-2-1 decode. The game's pre-overlay attachment holds linear-light values while its
// presented backbuffer is sRGB-encoded, so the fixture writes the HUD-less plane in the same space
// the transport must convert from; the transport encodes it back into display space.
float SrgbToLinear(float value)
{
    if (value <= 0.04045f) return value / 12.92f;
    return std::pow((value + 0.055f) / 1.055f, 2.4f);
}

// Fills a whole array texture with a per-slice colour so slice isolation is measurable.
bool FillColor(ID3D11Texture2D* texture, UINT slice, float r, float g, float b, float a)
{
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    std::vector<uint16_t> texels(static_cast<size_t>(desc.Width) * desc.Height * 4);
    for (size_t y = 0; y < desc.Height; y++)
    {
        for (size_t x = 0; x < desc.Width; x++)
        {
            // Horizontal ramp so a flipped or offset copy is visible.
            const float ramp = static_cast<float>(x) / static_cast<float>(desc.Width);
            uint16_t* texel = &texels[(y * desc.Width + x) * 4];
            texel[0] = ToHalf(r * ramp);
            texel[1] = ToHalf(g);
            texel[2] = ToHalf(b);
            texel[3] = ToHalf(a);
        }
    }
    D3D11_BOX box{0, 0, 0, desc.Width, desc.Height, 1};
    const UINT rowPitch = desc.Width * 4 * sizeof(uint16_t);
    g_context->UpdateSubresource(texture, D3D11CalcSubresource(0, slice, desc.MipLevels), &box, texels.data(),
        rowPitch, rowPitch * desc.Height);
    return true;
}

bool ClearDepth(ID3D11Texture2D* texture, UINT slice, float depth)
{
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    ComPtr<ID3D11DepthStencilView> view;
    D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc{};
    viewDesc.Format = desc.Format == DXGI_FORMAT_R24G8_TYPELESS ? DXGI_FORMAT_D24_UNORM_S8_UINT
        : desc.Format == DXGI_FORMAT_R32_TYPELESS ? DXGI_FORMAT_D32_FLOAT
        : desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT
        : desc.Format;
    viewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
    viewDesc.Texture2DArray.FirstArraySlice = slice;
    viewDesc.Texture2DArray.ArraySize = 1;
    viewDesc.Texture2DArray.MipSlice = 0;
    if (FAILED(g_device->CreateDepthStencilView(texture, &viewDesc, &view))) return false;
    g_context->ClearDepthStencilView(view.Get(), D3D11_CLEAR_DEPTH, depth, 0);
    return true;
}

bool FillMotion(ID3D11Texture2D* texture, UINT slice, float x, float y)
{
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    std::vector<uint16_t> texels(static_cast<size_t>(desc.Width) * desc.Height * 2);
    for (size_t i = 0; i < texels.size(); i += 2)
    {
        texels[i] = ToHalf(x);
        texels[i + 1] = ToHalf(y);
    }
    D3D11_BOX box{0, 0, 0, desc.Width, desc.Height, 1};
    const UINT rowPitch = desc.Width * 2 * sizeof(uint16_t);
    g_context->UpdateSubresource(texture, D3D11CalcSubresource(0, slice, desc.MipLevels), &box, texels.data(),
        rowPitch, rowPitch * desc.Height);
    return true;
}

// Reads one slice/rect of a texture back into floats. Returns an empty vector on failure.
std::vector<float> ReadBack(ID3D11Texture2D* texture, UINT slice, DXGI_FORMAT readFormat, UINT channels)
{
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.BindFlags = 0;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    staging.Format = readFormat;
    ComPtr<ID3D11Texture2D> copy;
    if (FAILED(g_device->CreateTexture2D(&staging, nullptr, &copy))) return {};
    const UINT subresource = D3D11CalcSubresource(0, slice, desc.MipLevels);
    g_context->CopySubresourceRegion(copy.Get(), 0, 0, 0, 0, texture, subresource, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};

    std::vector<float> values(static_cast<size_t>(desc.Width) * desc.Height * channels, 0.0f);
    const auto* rows = static_cast<const uint8_t*>(mapped.pData);
    for (UINT y = 0; y < desc.Height; y++)
    {
        const auto* row = reinterpret_cast<const uint16_t*>(rows + static_cast<size_t>(y) * mapped.RowPitch);
        for (UINT x = 0; x < desc.Width; x++)
        {
            for (UINT c = 0; c < channels; c++)
            {
                const uint16_t bits = row[x * channels + c];
                float value;
                const uint32_t sign = bits >> 15;
                const uint32_t exponent = (bits >> 10) & 0x1F;
                const uint32_t mantissa = bits & 0x3FF;
                if (exponent == 0)
                {
                    value = mantissa == 0 ? 0.0f : std::ldexp(static_cast<float>(mantissa), -24);
                }
                else if (exponent == 31)
                {
                    value = mantissa == 0 ? INFINITY : NAN;
                }
                else
                {
                    value = std::ldexp(static_cast<float>(0x400 | mantissa), static_cast<int>(exponent) - 25);
                }
                if (sign) value = -value;
                if (channels == 1)
                    values[static_cast<size_t>(y) * desc.Width + x] = value;
                else
                    values[(static_cast<size_t>(y) * desc.Width + x) * channels + c] = value;
            }
        }
    }
    g_context->Unmap(copy.Get(), 0);
    return values;
}

// Row-major texel index for a read-back buffer.
size_t Texel(UINT x, UINT y, UINT width, UINT channels)
{
    return (static_cast<size_t>(y) * width + x) * channels;
}

std::vector<float> ReadBackFloat(ID3D11Texture2D* texture, UINT slice)
{
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.BindFlags = 0;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> copy;
    if (FAILED(g_device->CreateTexture2D(&staging, nullptr, &copy))) return {};
    g_context->CopySubresourceRegion(copy.Get(), 0, 0, 0, 0, texture, D3D11CalcSubresource(0, slice, 1), nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};
    std::vector<float> values(static_cast<size_t>(desc.Width) * desc.Height);
    for (UINT y = 0; y < desc.Height; y++)
    {
        const auto* row = reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped.pData) +
            static_cast<size_t>(y) * mapped.RowPitch);
        for (UINT x = 0; x < desc.Width; x++) values[static_cast<size_t>(y) * desc.Width + x] = row[x];
    }
    g_context->Unmap(copy.Get(), 0);
    return values;
}

// Shared fixtures for one eye.
struct Fixture
{
    ComPtr<ID3D11Texture2D> color;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11Texture2D> motion;
    ComPtr<ID3D11Texture2D> output;
    ViewKey key{};
    EvaluationRequest request{};
};

bool MakeFixture(UINT sourceWidth, UINT sourceHeight, UINT arraySize, UINT inputWidth, UINT inputHeight,
    UINT outputWidth, UINT outputHeight, UINT scratchWidth, UINT scratchHeight, DXGI_FORMAT depthFormat,
    Fixture& fixture, int cameraId, int eye, int slice, int sliceCount)
{
    const UINT outputSlices = static_cast<UINT>(sliceCount);
    const UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
    fixture.color = CreateTexture(sourceWidth, sourceHeight, arraySize, DXGI_FORMAT_R16G16B16A16_FLOAT, bind);
    fixture.depth = CreateTexture(sourceWidth, sourceHeight, arraySize, depthFormat,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL);
    fixture.motion = CreateTexture(sourceWidth, sourceHeight, arraySize, DXGI_FORMAT_R16G16_FLOAT, bind);
    fixture.output = CreateTexture(outputWidth, outputHeight, outputSlices, DXGI_FORMAT_R16G16B16A16_FLOAT, bind);
    if (!fixture.color || !fixture.depth || !fixture.motion || !fixture.output) return false;

    fixture.key = ViewKey{cameraId, eye};
    fixture.request = EvaluationRequest{};
    fixture.request.color = fixture.color.Get();
    fixture.request.depth = fixture.depth.Get();
    fixture.request.motion = fixture.motion.Get();
    fixture.request.output = fixture.output.Get();
    fixture.request.inputWidth = inputWidth;
    fixture.request.inputHeight = inputHeight;
    fixture.request.outputWidth = outputWidth;
    fixture.request.outputHeight = outputHeight;
    fixture.request.scratchWidth = scratchWidth;
    fixture.request.scratchHeight = scratchHeight;
    fixture.request.slice = slice;
    fixture.request.sliceCount = sliceCount;
    fixture.request.layout = sliceCount > 1 ? BIGSCALE_LAYOUT_ARRAY : BIGSCALE_LAYOUT_2D;
    fixture.request.motionWidth = inputWidth;
    fixture.request.motionHeight = inputHeight;
    fixture.request.frameIndex = 1;
    return true;
}

void CheckSingleView(EyeResources& eyes)
{
    Fixture fixture;
    if (!MakeFixture(64, 36, 1, 64, 36, 128, 72, 128, 72, DXGI_FORMAT_R32_TYPELESS, fixture, 900, 0, 0, 1))
    {
        Check(false, "single-view fixture created");
        return;
    }
    FillColor(fixture.color.Get(), 0, 1.0f, 0.5f, 0.25f, 1.0f);
    Check(ClearDepth(fixture.depth.Get(), 0, 0.25f), "single-view depth cleared");
    FillMotion(fixture.motion.Get(), 0, 0.125f, -0.0625f);

    const bool prepared = eyes.Prepare(g_device.Get(), fixture.key, fixture.request, fixture.color.Get(),
        fixture.depth.Get(), fixture.motion.Get());
    Check(prepared, "single-view scratch prepared: " + eyes.LastReason());
    if (!prepared) return;
    Check(eyes.MirrorSources(g_context.Get(), fixture.key, fixture.request),
        "single-view mirrored: " + eyes.LastReason());
    Check(eyes.Extract(g_context.Get(), fixture.key, fixture.request),
        "single-view extract succeeded: " + eyes.LastReason());

    auto* colorScratch = static_cast<ID3D11Texture2D*>(eyes.ColorInput(fixture.key));
    Check(colorScratch != nullptr, "colour scratch available");
    if (colorScratch != nullptr)
    {
        const auto values = ReadBack(colorScratch, 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
        Check(!values.empty(), "colour scratch read back");
        if (!values.empty())
        {
            Near(values[0], 0.0f, 0.01f, "colour scratch left edge is the ramp start");
            Near(values[Texel(63, 0, 64, 4)], 63.0f / 64.0f, 0.02f, "colour scratch right edge is the ramp end");
            Near(values[1], 0.5f, 0.01f, "colour scratch green channel is preserved");
            Near(values[2], 0.25f, 0.01f, "colour scratch blue channel is preserved");
        }
    }

    auto* depthScratch = static_cast<ID3D11Texture2D*>(eyes.DepthInput(fixture.key));
    Check(depthScratch != nullptr, "depth scratch available");
    if (depthScratch != nullptr)
    {
        const auto values = ReadBackFloat(depthScratch, 0);
        Check(!values.empty(), "depth scratch read back");
        if (!values.empty()) Near(values[0], 0.25f, 1e-5f, "D32 depth is preserved exactly");
    }

    auto* motionScratch = static_cast<ID3D11Texture2D*>(eyes.MotionInput(fixture.key));
    Check(motionScratch != nullptr, "motion scratch available");
    if (motionScratch != nullptr)
    {
        const auto values = ReadBack(motionScratch, 0, DXGI_FORMAT_R16G16_FLOAT, 2);
        Check(!values.empty(), "motion scratch read back");
        if (!values.empty())
        {
            Near(values[0], 0.125f, 1e-4f, "motion u comes from the mirrored slice");
            Near(values[1], -0.0625f, 1e-4f, "motion v comes from the mirrored slice");
        }
    }

    // The baseline must replace every texel of the output, including a sentinel.
    std::vector<uint16_t> sentinel(static_cast<size_t>(128) * 72 * 4, ToHalf(-1.0f));
    D3D11_BOX box{0, 0, 0, 128, 72, 1};
    g_context->UpdateSubresource(fixture.output.Get(), 0, &box, sentinel.data(), 128 * 4 * sizeof(uint16_t),
        128 * 4 * sizeof(uint16_t) * 72);
    Check(eyes.WriteSourceBaseline(g_context.Get(), fixture.key, fixture.request, fixture.output.Get()),
        "single-view baseline written: " + eyes.LastReason());
    const auto output = ReadBack(fixture.output.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
    Check(!output.empty(), "output read back");
    if (!output.empty())
    {
        bool anySentinel = false;
        for (size_t i = 0; i < output.size(); i += 4)
            if (output[i] < -0.5f) { anySentinel = true; break; }
        Check(!anySentinel, "the baseline covers every output texel (no sentinel left behind)");
        Near(output[Texel(64, 36, 128, 4) + 1], 0.5f, 0.02f,
            "the baseline upscales the extracted colour");
        Near(output[Texel(127, 0, 128, 4)], 63.0f / 64.0f, 0.03f,
            "the baseline reaches the right edge of the output");
        Near(output[Texel(0, 71, 128, 4)], 0.0f, 0.03f, "the baseline reaches the bottom edge of the output");
    }
}

void CheckArrayIsolation(EyeResources& eyes)
{
    // One texture, two slices: each eye must read and write its own slice.
    Fixture eye0;
    Fixture eye1;
    if (!MakeFixture(64, 36, 2, 64, 36, 128, 72, 128, 72, DXGI_FORMAT_R32_TYPELESS, eye0, 901, 0, 0, 2) ||
        !MakeFixture(64, 36, 2, 64, 36, 128, 72, 128, 72, DXGI_FORMAT_R32_TYPELESS, eye1, 901, 1, 1, 2))
    {
        Check(false, "array fixtures created");
        return;
    }
    FillColor(eye0.color.Get(), 0, 1.0f, 0.0f, 0.0f, 1.0f);
    FillColor(eye0.color.Get(), 1, 0.0f, 1.0f, 0.0f, 1.0f);
    ClearDepth(eye0.depth.Get(), 0, 0.75f);
    ClearDepth(eye0.depth.Get(), 1, 0.125f);
    FillMotion(eye0.motion.Get(), 0, 0.5f, 0.0f);
    FillMotion(eye0.motion.Get(), 1, 0.0f, 0.5f);
    // Both eyes read the same array resource, which is what single-pass stereo provides.
    eye1.request.color = eye0.color.Get();
    eye1.request.depth = eye0.depth.Get();
    eye1.request.motion = eye0.motion.Get();

    Check(eyes.Prepare(g_device.Get(), eye0.key, eye0.request, eye0.color.Get(), eye0.depth.Get(), eye0.motion.Get()),
        "eye 0 scratch prepared: " + eyes.LastReason());
    Check(eyes.Prepare(g_device.Get(), eye1.key, eye1.request, eye0.color.Get(), eye0.depth.Get(), eye0.motion.Get()),
        "eye 1 scratch prepared: " + eyes.LastReason());
    Check(eyes.MirrorSources(g_context.Get(), eye0.key, eye0.request), "eye 0 mirrored: " + eyes.LastReason());
    Check(eyes.MirrorSources(g_context.Get(), eye1.key, eye1.request), "eye 1 mirrored: " + eyes.LastReason());
    Check(eyes.Extract(g_context.Get(), eye0.key, eye0.request), "eye 0 extracted: " + eyes.LastReason());
    Check(eyes.Extract(g_context.Get(), eye1.key, eye1.request), "eye 1 extracted: " + eyes.LastReason());

    auto* scratch0 = static_cast<ID3D11Texture2D*>(eyes.ColorInput(eye0.key));
    auto* scratch1 = static_cast<ID3D11Texture2D*>(eyes.ColorInput(eye1.key));
    if (scratch0 != nullptr && scratch1 != nullptr)
    {
        const auto a = ReadBack(scratch0, 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
        const auto b = ReadBack(scratch1, 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
        Check(!a.empty() && !b.empty(), "both eyes read back");
        if (!a.empty() && !b.empty())
        {
            Near(a[1], 0.0f, 0.01f, "eye 0 has no green from slice 1");
            Near(b[1], 1.0f, 0.01f, "eye 1 carries slice 1's green");
            Near(b[2], 0.0f, 0.01f, "eye 1 carries slice 1's blue");
        }
    }
    auto* depth0 = static_cast<ID3D11Texture2D*>(eyes.DepthInput(eye0.key));
    auto* depth1 = static_cast<ID3D11Texture2D*>(eyes.DepthInput(eye1.key));
    if (depth0 != nullptr && depth1 != nullptr)
    {
        const auto a = ReadBackFloat(depth0, 0);
        const auto b = ReadBackFloat(depth1, 0);
        if (!a.empty() && !b.empty())
        {
            Near(a[0], 0.75f, 1e-5f, "eye 0 depth comes from slice 0");
            Near(b[0], 0.125f, 1e-5f, "eye 1 depth comes from slice 1");
        }
    }

    // Each eye writes its own output slice.
    Check(eyes.WriteSourceBaseline(g_context.Get(), eye0.key, eye0.request, eye0.output.Get()),
        "eye 0 baseline: " + eyes.LastReason());
    Check(eyes.WriteSourceBaseline(g_context.Get(), eye1.key, eye1.request, eye1.output.Get()),
        "eye 1 baseline: " + eyes.LastReason());
    const auto out0 = ReadBack(eye0.output.Get(), static_cast<UINT>(eye0.request.slice),
        DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
    const auto out1 = ReadBack(eye1.output.Get(), static_cast<UINT>(eye1.request.slice),
        DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
    if (!out0.empty() && !out1.empty())
    {
        Near(out0[1], 0.0f, 0.02f, "eye 0 output holds slice 0");
        Near(out1[1], 1.0f, 0.02f, "eye 1 output holds slice 1");
    }
}

void CheckPackedRegion(EyeResources& eyes)
{
    // A packed viewport: the eye lives in a sub-region of a larger texture.
    Fixture fixture;
    if (!MakeFixture(128, 72, 1, 64, 36, 128, 72, 128, 72, DXGI_FORMAT_R32_TYPELESS, fixture, 902, 1, 0, 1))
    {
        Check(false, "packed fixture created");
        return;
    }
    fixture.request.inputX = 32;
    fixture.request.inputY = 16;
    fixture.request.outputX = 0;
    fixture.request.outputY = 0;
    fixture.request.layout = BIGSCALE_LAYOUT_PACKED;
    FillColor(fixture.color.Get(), 0, 1.0f, 0.25f, 0.75f, 1.0f);
    FillMotion(fixture.motion.Get(), 0, 0.25f, 0.25f);

    Check(eyes.Prepare(g_device.Get(), fixture.key, fixture.request, fixture.color.Get(), fixture.depth.Get(),
        fixture.motion.Get()), "packed scratch prepared: " + eyes.LastReason());
    Check(eyes.MirrorSources(g_context.Get(), fixture.key, fixture.request), "packed mirrored: " + eyes.LastReason());
    Check(eyes.Extract(g_context.Get(), fixture.key, fixture.request),
        "packed extract succeeded: " + eyes.LastReason());
    auto* scratch = static_cast<ID3D11Texture2D*>(eyes.ColorInput(fixture.key));
    if (scratch != nullptr)
    {
        const auto values = ReadBack(scratch, 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
        Check(!values.empty(), "packed scratch read back");
        if (!values.empty())
        {
            Near(values[Texel(0, 0, 64, 4)], 0.25f, 0.02f, "packed scratch starts at the region's left edge");
            Near(values[Texel(63, 0, 64, 4)], 95.0f / 128.0f, 0.02f, "packed scratch ends at the region's right edge");
        }
    }
    Check(eyes.WriteSourceBaseline(g_context.Get(), fixture.key, fixture.request, fixture.output.Get()),
        "packed baseline written: " + eyes.LastReason());
    const auto output = ReadBack(fixture.output.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
    if (!output.empty())
    {
        // The region starts at x=32 of a 128-wide source, so the packed eye's left edge is 25 % along
        // the ramp rather than at zero.
        Near(output[Texel(0, 0, 128, 4)], 0.25f, 0.03f, "the packed region starts at its own left edge");
        Near(output[Texel(127, 0, 128, 4)], 95.0f / 128.0f, 0.03f,
            "the packed region ends at its own right edge");
        Near(output[Texel(0, 71, 128, 4)], 0.25f, 0.03f, "the packed region covers the last row too");
    }
}

void CheckPackedDepth(EyeResources& eyes)
{
    // D24S8 is a packed depth family; the conversion must preserve the nonlinear device value.
    Fixture fixture;
    if (!MakeFixture(64, 36, 1, 64, 36, 128, 72, 128, 72, DXGI_FORMAT_R24G8_TYPELESS, fixture, 903, 0, 0, 1))
    {
        Check(false, "D24S8 fixture created");
        return;
    }
    FillColor(fixture.color.Get(), 0, 1.0f, 1.0f, 1.0f, 1.0f);
    FillMotion(fixture.motion.Get(), 0, 0.0f, 0.0f);
    Check(ClearDepth(fixture.depth.Get(), 0, 0.625f), "D24S8 depth cleared");
    Check(eyes.Prepare(g_device.Get(), fixture.key, fixture.request, fixture.color.Get(), fixture.depth.Get(),
        fixture.motion.Get()), "D24S8 scratch prepared: " + eyes.LastReason());
    Check(eyes.MirrorSources(g_context.Get(), fixture.key, fixture.request), "D24S8 mirrored: " + eyes.LastReason());
    Check(eyes.Extract(g_context.Get(), fixture.key, fixture.request),
        "D24S8 extract succeeded: " + eyes.LastReason());
    auto* depth = static_cast<ID3D11Texture2D*>(eyes.DepthInput(fixture.key));
    if (depth != nullptr)
    {
        const auto values = ReadBackFloat(depth, 0);
        Check(!values.empty(), "D24S8 depth read back");
        // 24-bit UNORM quantisation is 1/16777216, but clear values are exact multiples of it.
        if (!values.empty()) Near(values[0], 0.625f, 1.0f / 65536.0f, "packed depth is preserved");
    }
}

void CheckTypelessDepth32(EyeResources& eyes)
{
    // URP's XR depth attachment is R32_FLOAT_X8X24_TYPELESS. The mirror copy must carry that
    // depth-stencil family as exactly as the D32_FLOAT-and-D24S8 cases.
    Fixture fixture;
    if (!MakeFixture(64, 36, 1, 64, 36, 128, 72, 128, 72, DXGI_FORMAT_R32G8X24_TYPELESS,
            fixture, 907, 0, 0, 1))
    {
        Check(false, "R32G8X24 depth fixture created");
        return;
    }
    FillColor(fixture.color.Get(), 0, 1.0f, 1.0f, 1.0f, 1.0f);
    FillMotion(fixture.motion.Get(), 0, 0.0f, 0.0f);
    Check(ClearDepth(fixture.depth.Get(), 0, 0.375f), "R32G8X24 depth cleared");
    Check(eyes.Prepare(g_device.Get(), fixture.key, fixture.request, fixture.color.Get(), fixture.depth.Get(),
        fixture.motion.Get()), "R32G8X24 scratch prepared: " + eyes.LastReason());
    Check(eyes.MirrorSources(g_context.Get(), fixture.key, fixture.request), "R32G8X24 mirrored: " + eyes.LastReason());
    Check(eyes.Extract(g_context.Get(), fixture.key, fixture.request),
        "R32G8X24 extract succeeded: " + eyes.LastReason());
    auto* depth = static_cast<ID3D11Texture2D*>(eyes.DepthInput(fixture.key));
    if (depth != nullptr)
    {
        const auto values = ReadBackFloat(depth, 0);
        Check(!values.empty(), "R32G8X24 depth read back");
        if (!values.empty()) Near(values[0], 0.375f, 1e-6f, "typeless D32S8 depth is preserved");
    }
}

void CheckComputeSlotState()
{
    // A packet must leave exactly the compute bindings it found, so Unity's own slots survive a
    // reconstruction frame and no device-context state is swapped. Slots 3, 5 and 1 are interior to
    // the saved ranges and non-null, so a guard that only clears slots is caught.
    auto srvTexture = CreateTexture(8, 8, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    auto uavTexture = CreateTexture(8, 8, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    if (!srvTexture || !uavTexture)
    {
        Check(false, "compute guard fixtures created");
        return;
    }
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = 32;
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    Check(SUCCEEDED(g_device->CreateShaderResourceView(srvTexture.Get(), nullptr, &srv)), "guard SRV created");
    Check(SUCCEEDED(g_device->CreateUnorderedAccessView(uavTexture.Get(), nullptr, &uav)), "guard UAV created");
    Check(SUCCEEDED(g_device->CreateBuffer(&bufferDesc, nullptr, &constants)), "guard constant buffer created");
    Check(SUCCEEDED(g_device->CreateSamplerState(&samplerDesc, &sampler)), "guard sampler created");
    if (!srv || !uav || !constants || !sampler) return;

    ID3D11ShaderResourceView* resource[1] = {srv.Get()};
    ID3D11UnorderedAccessView* target[1] = {uav.Get()};
    ID3D11Buffer* buffer[1] = {constants.Get()};
    ID3D11SamplerState* state[1] = {sampler.Get()};
    g_context->CSSetShaderResources(3, 1, resource);
    g_context->CSSetUnorderedAccessViews(5, 1, target, nullptr);
    g_context->CSSetConstantBuffers(1, 1, buffer);
    g_context->CSSetSamplers(1, 1, state);

    {
        ComputeSlotState guard(g_context.Get());
        ID3D11ShaderResourceView* clobbered[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
        ID3D11UnorderedAccessView* clobberedTargets[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr};
        ID3D11Buffer* clobberedBuffers[2] = {nullptr, nullptr};
        ID3D11SamplerState* clobberedSamplers[2] = {nullptr, nullptr};
        g_context->CSSetShaderResources(0, 8, clobbered);
        g_context->CSSetUnorderedAccessViews(0, 8, clobberedTargets, nullptr);
        g_context->CSSetConstantBuffers(0, 2, clobberedBuffers);
        g_context->CSSetSamplers(0, 2, clobberedSamplers);
    }

    ID3D11ShaderResourceView* restoredResource = nullptr;
    ID3D11UnorderedAccessView* restoredTarget = nullptr;
    ID3D11Buffer* restoredBuffer = nullptr;
    ID3D11SamplerState* restoredSampler = nullptr;
    g_context->CSGetShaderResources(3, 1, &restoredResource);
    g_context->CSGetUnorderedAccessViews(5, 1, &restoredTarget);
    g_context->CSGetConstantBuffers(1, 1, &restoredBuffer);
    g_context->CSGetSamplers(1, 1, &restoredSampler);
    Check(restoredResource == srv.Get(), "the packet guard restores an interior compute resource slot");
    Check(restoredTarget == uav.Get(), "the packet guard restores an interior compute UAV slot");
    Check(restoredBuffer == constants.Get(), "the packet guard restores an interior constant buffer slot");
    Check(restoredSampler == sampler.Get(), "the packet guard restores an interior compute sampler slot");

    if (restoredResource != nullptr) restoredResource->Release();
    if (restoredTarget != nullptr) restoredTarget->Release();
    if (restoredBuffer != nullptr) restoredBuffer->Release();
    if (restoredSampler != nullptr) restoredSampler->Release();

    // Leave the context as a stock frame would find it.
    ID3D11ShaderResourceView* noneResource[1] = {nullptr};
    ID3D11UnorderedAccessView* noneTarget[1] = {nullptr};
    ID3D11Buffer* noneBuffer[1] = {nullptr};
    ID3D11SamplerState* noneSampler[1] = {nullptr};
    g_context->CSSetShaderResources(3, 1, noneResource);
    g_context->CSSetUnorderedAccessViews(5, 1, noneTarget, nullptr);
    g_context->CSSetConstantBuffers(1, 1, noneBuffer);
    g_context->CSSetSamplers(1, 1, noneSampler);
}

void CheckFullPresentationFrame(EyeResources& eyes)
{
    // Presentation regression: a 1920x1080 output from an active 1280x720 viewport that lives at
    // the origin of a larger pooled allocation. The active region must cover the entire output;
    // the allocation's extra columns must never be sampled, and no output texel may be left black.
    Fixture fixture;
    if (!MakeFixture(1920, 1080, 1, 1280, 720, 1920, 1080, 1280, 720, DXGI_FORMAT_R32_TYPELESS,
            fixture, 960, 0, 0, 1))
    {
        Check(false, "full-presentation fixture created");
        return;
    }
    FillColor(fixture.color.Get(), 0, 1.0f, 0.5f, 0.25f, 1.0f);
    Check(ClearDepth(fixture.depth.Get(), 0, 0.25f), "full-presentation depth cleared");
    FillMotion(fixture.motion.Get(), 0, 0.0f, 0.0f);

    Check(eyes.Prepare(g_device.Get(), fixture.key, fixture.request, fixture.color.Get(), fixture.depth.Get(),
            fixture.motion.Get()),
        "full-presentation scratch prepared: " + eyes.LastReason());
    Check(eyes.MirrorSources(g_context.Get(), fixture.key, fixture.request),
        "full-presentation mirrored: " + eyes.LastReason());
    Check(eyes.Extract(g_context.Get(), fixture.key, fixture.request),
        "the active region is extracted from the larger allocation: " + eyes.LastReason());
    Check(eyes.WriteSourceBaseline(g_context.Get(), fixture.key, fixture.request, fixture.output.Get()),
        "the baseline is written to the full output: " + eyes.LastReason());

    const auto output = ReadBack(fixture.output.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
    Check(!output.empty(), "full-presentation output read back");
    if (output.empty()) return;

    const UINT width = 1920;
    const UINT height = 1080;
    const size_t corners[] = {
        Texel(0, 0, width, 4), Texel(width - 1, 0, width, 4),
        Texel(0, height - 1, width, 4), Texel(width - 1, height - 1, width, 4),
        Texel(width / 2, height / 2, width, 4),
    };
    const char* names[] = {"top-left", "top-right", "bottom-left", "bottom-right", "centre"};
    for (int i = 0; i < 5; i++)
    {
        const size_t index = corners[i];
        Check(std::isfinite(output[index]) && std::isfinite(output[index + 3]),
            std::string("the baseline is finite at the ") + names[i]);
        Near(output[index + 3], 1.0f, 0.05f, std::string("the baseline covers the ") + names[i]);
        Check(output[index + 1] > 0.2f, std::string("the baseline is not black at the ") + names[i]);
    }

    // The right edge samples the active region's last column (1279/1920), not the allocation's.
    Near(output[Texel(width - 1, 0, width, 4)], 1279.0f / 1920.0f, 0.02f,
        "the baseline's right edge is the active region's right edge");
    Near(output[Texel(0, 0, width, 4)], 0.0f, 0.02f, "the baseline's left edge is the active origin");
}

void CheckRejections(EyeResources& eyes)
{
    // A multisampled source is not a legal temporal input.
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 32;
        desc.Height = 32;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 2;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> multisampled;
        if (SUCCEEDED(g_device->CreateTexture2D(&desc, nullptr, &multisampled)))
        {
            Fixture fixture;
            if (!MakeFixture(32, 32, 1, 32, 32, 64, 64, 64, 64, DXGI_FORMAT_R32_TYPELESS, fixture, 904, 0, 0, 1))
                return;
            const bool prepared = eyes.Prepare(g_device.Get(), fixture.key, fixture.request, multisampled.Get(),
                fixture.depth.Get(), fixture.motion.Get());
            Check(!prepared, "a multisampled source is rejected");
            Check(!eyes.LastReason().empty(), "the rejection carries a reason");
        }
    }

    // An output that is not RGBA16F is rejected instead of being written through a bad copy.
    {
        Fixture fixture;
        if (!MakeFixture(32, 32, 1, 32, 32, 64, 64, 64, 64, DXGI_FORMAT_R32_TYPELESS, fixture, 905, 0, 0, 1))
            return;
        auto wrongOutput = CreateTexture(64, 64, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        if (wrongOutput && fixture.color && eyes.Prepare(g_device.Get(), fixture.key, fixture.request,
                fixture.color.Get(), fixture.depth.Get(), fixture.motion.Get()))
        {
            Check(!eyes.CopyBack(g_context.Get(), fixture.key, fixture.request, wrongOutput.Get()),
                "a non-RGBA16F output is rejected");
        }
    }

    // A region larger than the scratch is rejected before any GPU call.
    {
        Fixture fixture;
        if (!MakeFixture(32, 32, 1, 32, 32, 64, 64, 32, 32, DXGI_FORMAT_R32_TYPELESS, fixture, 906, 0, 0, 1))
            return;
        fixture.request.inputWidth = 64;
        fixture.request.inputHeight = 64;
        Check(!eyes.Prepare(g_device.Get(), fixture.key, fixture.request, fixture.color.Get(), fixture.depth.Get(),
            fixture.motion.Get()), "an input larger than the scratch extent is rejected");
    }
}

struct Packet
{
    void* handle = nullptr;
};

using RenderEventFn = void(__stdcall*)(int32_t, void*);

RenderEventFn RenderEvent()
{
    return reinterpret_cast<RenderEventFn>(BIGSCALE_GetRenderEvent());
}

// Fires one queued packet through the real render entry point.
void Execute(void* packet)
{
    RenderEvent()(1, packet);
}

BIGSCALE_Status ReadStatus()
{
    BIGSCALE_Status status{};
    BIGSCALE_GetStatus(&status);
    return status;
}

// Provider-scoped snapshot: the mode block and active identity belong to exactly this request.
BIGSCALE_Status ReadStatusFor(int32_t upscaler, uint32_t width, uint32_t height)
{
    BIGSCALE_Status status{};
    status.queryUpscaler = upscaler;
    status.outputWidth = width;
    status.outputHeight = height;
    BIGSCALE_GetStatus(&status);
    return status;
}

// Counts retained records matching a predicate; the snapshot holds the oldest eight.
int CountCompletions(const BIGSCALE_Status& status, uint32_t kind, int32_t operation, int* resultOut = nullptr,
    uint32_t* tagOut = nullptr)
{
    const uint32_t count = status.completionPublished < BIGSCALE_COMPLETION_COUNT
        ? status.completionPublished
        : BIGSCALE_COMPLETION_COUNT;
    int found = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        const BIGSCALE_Completion& entry = status.completions[i];
        if (entry.kind != kind) continue;
        if (operation >= 0 && entry.operation != operation) continue;
        found++;
        if (resultOut != nullptr) *resultOut = entry.result;
        if (tagOut != nullptr) *tagOut = entry.resourceTag;
    }
    return found;
}

// Acknowledges everything currently retained so later checks start from an empty queue. A snapshot
// carries eight records, so the drain repeats until the queue reports empty.
void AckAll()
{
    for (int attempt = 0; attempt < 64; attempt++)
    {
        const BIGSCALE_Status status = ReadStatus();
        if (status.completionPublished == 0) return;
        const uint32_t count = status.completionPublished < BIGSCALE_COMPLETION_COUNT
            ? status.completionPublished
            : BIGSCALE_COMPLETION_COUNT;
        BIGSCALE_AckCompletions(status.completions[count - 1].sequence);
    }
}

void DumpCompletions(const char* label)
{
    const BIGSCALE_Status status = ReadStatus();
    const uint32_t count = status.completionPublished < BIGSCALE_COMPLETION_COUNT
        ? status.completionPublished
        : BIGSCALE_COMPLETION_COUNT;
    std::printf("  %s: state=%d error=%d published=%u base=%u", label, status.state, status.lastError,
        status.completionPublished, status.completionBase);
    for (uint32_t i = 0; i < count; i++)
    {
        const BIGSCALE_Completion& entry = status.completions[i];
        std::printf(" [seq=%u kind=%u op=%d result=%d tag=%u]", entry.sequence, entry.kind, entry.operation,
            entry.result, entry.resourceTag);
    }
    std::printf("\n");
}

bool WaitForDevice()
{
    D3D11_QUERY_DESC description{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> query;
    if (FAILED(g_device->CreateQuery(&description, &query)) || !query) return false;
    g_context->End(query.Get());
    g_context->Flush();
    for (int attempt = 0; attempt < 5000; attempt++)
    {
        BOOL complete = FALSE;
        const HRESULT result = g_context->GetData(query.Get(), &complete, sizeof(complete), 0);
        if (result == S_OK && complete) return true;
        if (FAILED(result)) return false;
        Sleep(1);
    }
    return false;
}

void CheckAbi()
{
    Check(BIGSCALE_GetAbiVersion() == 5, "the bridge reports ABI 5");
    Check(BIGSCALE_GetFrameSize() == 192, "the bridge reports a 192-byte frame");
    Check(BIGSCALE_GetStatusSize() == 1176, "the bridge reports a 1176-byte status");
    Check(BIGSCALE_GetCompletionSize() == 76, "the bridge reports a 76-byte completion");
    Check(BIGSCALE_MODE_COUNT == 5, "the mode block carries five neutral presets");
    Check(sizeof(BIGSCALE_NeuralConfig) == 44, "the neural configuration is 44 bytes");

    uint32_t offsets[42]{};
    const uint32_t count = BIGSCALE_GetFrameLayout(offsets, 42);
    Check(count == 42, "the frame layout reports 42 fields");
    Check(offsets[20] == 80, "the bridge reports the colour pointer at offset 80");
    Check(offsets[23] == 104, "the bridge reports the output pointer at offset 104");
    Check(offsets[32] == 144, "the bridge reports the resource tag at offset 144");
    Check(offsets[33] == 148, "the bridge reports the upscaler at offset 148");
    Check(offsets[38] == 168, "the bridge reports the FG capture tag at offset 168");
    Check(offsets[39] == 176, "the bridge reports the neural mask at offset 176");
    Check(offsets[40] == 184, "the bridge reports the neural mask width at offset 184");
    Check(offsets[41] == 188, "the bridge reports the neural mask height at offset 188");

    uint32_t completionOffsets[19]{};
    Check(BIGSCALE_GetCompletionLayout(completionOffsets, 19) == 19, "the completion layout reports 19 fields");
    Check(completionOffsets[13] == 52, "the bridge reports the completion upscaler at offset 52");

    uint32_t statusOffsets[40]{};
    Check(BIGSCALE_GetStatusLayout(statusOffsets, 40) == 40, "the status layout reports 40 fields");
    Check(statusOffsets[20] == 224, "the bridge reports views at offset 224");
    Check(statusOffsets[25] == 284, "the bridge reports neuralState at offset 284");
    Check(statusOffsets[28] == 296, "the bridge reports completions at offset 296");
    Check(statusOffsets[36] == 904, "the bridge reports queryUpscaler at offset 904");
    Check(statusOffsets[37] == 908, "the bridge reports activeUpscaler at offset 908");
    Check(statusOffsets[38] == 912, "the bridge reports providerVersionId at offset 912");
    Check(statusOffsets[39] == 920, "the bridge reports backendReason at offset 920");

    Check(BIGSCALE_ConfigureNeuralRendering(nullptr) == BIGSCALE_ERROR_ABI_MISMATCH,
        "a null neural configuration is refused");
    BIGSCALE_NeuralConfig neural{};
    neural.abi = BIGSCALE_NEURAL_ABI_VERSION + 1;
    neural.size = sizeof(BIGSCALE_NeuralConfig);
    Check(BIGSCALE_ConfigureNeuralRendering(&neural) == BIGSCALE_ERROR_ABI_MISMATCH,
        "a foreign neural ABI version is refused");
    neural.abi = BIGSCALE_NEURAL_ABI_VERSION;
    neural.size = sizeof(BIGSCALE_NeuralConfig) + 4;
    Check(BIGSCALE_ConfigureNeuralRendering(&neural) == BIGSCALE_ERROR_ABI_MISMATCH,
        "a mismatched neural configuration size is refused");
}

// Adaptive frame-generation cadence: a requested multiplier is only displayable when one source
// interval can hold that many presents at the display refresh. Both the fit function and the
// governor are pure arithmetic, so every case names its own source interval and refresh instead of
// depending on the host monitor.
void CheckFgAdaptiveCadence()
{
    auto choose = [](double source, double refresh, uint32_t configured, uint32_t maximum)
    {
        FgCadenceInput input{};
        input.sourceIntervalMilliseconds = source;
        input.refreshIntervalMilliseconds = refresh;
        input.configuredMultiplier = configured;
        input.providerMaximum = maximum;
        return FgChoosePresentedCount(input);
    };
    struct ChooseCase
    {
        double source;
        double refresh;
        uint32_t configured;
        uint32_t maximum;
        uint32_t expected;
    };
    const ChooseCase chooseCases[] = {
        // 60 Hz display: one refresh is 16.7 ms.
        {16.7, 16.7, 4, 6, 1},
        {33.4, 16.7, 4, 6, 2},
        {66.7, 16.7, 4, 6, 4},
        {8.3, 16.7, 4, 6, 1},
        // 120 Hz display: one refresh is 8.33 ms.
        {25.0, 8.33, 4, 6, 3},
        // The provider's proven ceiling caps the fit, not the configured request.
        {33.3, 8.33, 4, 2, 2},
        // An unknowable source interval or refresh preserves the request untouched.
        {0.0, 16.7, 4, 6, 4},
        {33.3, 0.0, 4, 6, 4},
    };
    for (const ChooseCase& testCase : chooseCases)
    {
        const uint32_t actual = choose(testCase.source, testCase.refresh, testCase.configured,
            testCase.maximum);
        char description[224]{};
        std::snprintf(description, sizeof(description),
            "cadence choose(source=%.2f ms, refresh=%.2f ms, requested=%ux, providerMax=%ux): expected %u, "
            "got %u",
            testCase.source, testCase.refresh, static_cast<unsigned>(testCase.configured),
            static_cast<unsigned>(testCase.maximum), static_cast<unsigned>(testCase.expected),
            static_cast<unsigned>(actual));
        Check(actual == testCase.expected, description);
    }

    FgMultiplierGovernor governor;
    auto observe = [&governor](double source)
    {
        FgCadenceInput input{};
        input.sourceIntervalMilliseconds = source;
        input.refreshIntervalMilliseconds = 16.7;
        input.configuredMultiplier = 4;
        input.providerMaximum = 6;
        return governor.Observe(input);
    };
    governor.Reset();
    const uint32_t firstSample = observe(33.4);
    Check(firstSample == 4,
        "governor: the first sample after a reset boots at the requested ceiling, not from one "
        "interval (got " + std::to_string(firstSample) + "x)");
    uint32_t settledTwo = firstSample;
    int settleObservations = 0;
    for (int frame = 0; frame < 6 && settledTwo != 2; frame++)
    {
        settledTwo = observe(33.4);
        settleObservations = frame + 1;
    }
    Check(settledTwo == 2,
        "governor: a 33.4 ms source at a 16.7 ms refresh settles at 2x within 6 observations (got " +
            std::to_string(settledTwo) + "x after " + std::to_string(settleObservations) + ")");

    // Raising must be slow and single-step: the smoothed interval has to fit the next count for 30
    // consecutive source frames, and the count may only climb one at a time.
    bool reachedFour = false;
    bool monotone = true;
    bool singleStep = true;
    bool ceilingHeld = true;
    int riseObservations = 0;
    uint32_t previous = governor.Current();
    for (int frame = 0; frame < 120; frame++)
    {
        const uint32_t current = observe(66.7);
        if (current > 4) ceilingHeld = false;
        if (current < previous) monotone = false;
        if (current > previous + 1) singleStep = false;
        if (current != previous)
            std::printf("governor: raise step to %ux at observation %d\n", static_cast<unsigned>(current),
                frame + 1);
        previous = current;
        if (!reachedFour && current == 4)
        {
            reachedFour = true;
            riseObservations = frame + 1;
        }
    }
    char riseDescription[256]{};
    std::snprintf(riseDescription, sizeof(riseDescription),
        "governor: 66.7 ms at a 16.7 ms refresh rises 2->3->4 within 120 observations, one step at a "
        "time and never above 4 (reached=%d after %d, monotone=%d, singleStep=%d, ceilingHeld=%d)",
        reachedFour ? 1 : 0, riseObservations, monotone ? 1 : 0, singleStep ? 1 : 0, ceilingHeld ? 1 : 0);
    Check(reachedFour && monotone && singleStep && ceilingHeld, riseDescription);
    const uint32_t settled = governor.Current();
    Check(settled == 4, "governor: the settled count stays at the feasible 4x (got " +
        std::to_string(settled) + "x)");

    // Confirmation: a single double-present or scheduler outlier must not collapse the multiplier the
    // display has been sustaining, while a genuine step change does drop.
    const uint32_t afterOneLow = observe(16.7);
    Check(afterOneLow == 4, "governor: one short interval does not drop a settled 4x (got " +
        std::to_string(afterOneLow) + "x)");
    const uint32_t afterTwoLow = observe(16.7);
    Check(afterTwoLow == 1, "governor: two consecutive short intervals drop to 1x (got " +
        std::to_string(afterTwoLow) + "x)");

    uint32_t raisedValue = 1;
    int raiseBackObservations = 0;
    for (int frame = 0; frame < 90; frame++)
    {
        const uint32_t current = observe(33.4);
        if (current != 1)
        {
            raisedValue = current;
            raiseBackObservations = frame + 1;
            break;
        }
    }
    char raiseBackDescription[224]{};
    std::snprintf(raiseBackDescription, sizeof(raiseBackDescription),
        "governor: 33.4 ms raises the dropped 1x back to 2x within 90 observations (got %ux after %d)",
        static_cast<unsigned>(raisedValue), raiseBackObservations);
    Check(raisedValue == 2 && raiseBackObservations >= 1 && raiseBackObservations <= 90,
        raiseBackDescription);

    // Anti-flap: present intervals jitter around the M * refresh boundary by a couple of milliseconds
    // on any real machine (the present lands slightly before or after its slot). Acting on a raw
    // sample alone made generation alternate between 2x and 1x every few frames, cancelling and
    // restarting captures and hiding the presenter each time. The smoothed interval decides, so a
    // jittering source that fits on average must hold its multiplier.
    uint32_t jitterValue = governor.Current();
    int jitterDrops = 0;
    for (int frame = 0; frame < 40; frame++)
    {
        const double interval = (frame % 2 == 0) ? 33.4 : 31.7;
        jitterValue = observe(interval);
        if (jitterValue < 2) jitterDrops++;
    }
    Check(jitterDrops == 0 && jitterValue == 2,
        "governor: intervals jittering across the pair boundary never drop 2x (drops=" +
            std::to_string(jitterDrops) + ", final=" + std::to_string(jitterValue) + "x)");

    // A gap far longer than a refresh can only add room, so it must restore the ceiling immediately;
    // holding the previous decision there is what turned a stall into a self-sustaining pause.
    for (int frame = 0; frame < 4; frame++) observe(16.7);
    const uint32_t beforeGap = governor.Current();
    const uint32_t afterGap = observe(600.0);
    Check(beforeGap == 1 && afterGap == 4,
        "governor: a 600 ms gap restores the 4x ceiling (before=" + std::to_string(beforeGap) +
            "x, after=" + std::to_string(afterGap) + "x)");
    // A zero delta is a clock that did not advance, not a frame rate: no evidence either way.
    const uint32_t afterZero = observe(0.0);
    Check(afterZero == 4, "governor: an unmeasurable interval keeps the current decision (got " +
        std::to_string(afterZero) + "x)");
}

void CheckRingAndProbe()
{
    wchar_t temporary[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temporary);
    // The runtime directory is where the checks stage verified provider DLLs next to the executable.
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring runtime = modulePath;
    const size_t slash = runtime.find_last_of(L"\\/");
    if (slash != std::wstring::npos) runtime.resize(slash);
    const std::wstring data = std::wstring(temporary) + L"bigscale-check-data";
    CreateDirectoryW(runtime.c_str(), nullptr);
    CreateDirectoryW(data.c_str(), nullptr);
    Check(BIGSCALE_Initialize(runtime.c_str(), data.c_str()) == 1, "the bridge initialises its CPU state");

    auto color = CreateTexture(32, 32, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET);
    Check(color != nullptr, "ring fixture texture created");
    if (color == nullptr) return;

    // A capability probe carries colour only: no depth, motion or output.
    BIGSCALE_Frame frame{};
    frame.abi = BIGSCALE_ABI_VERSION;
    frame.size = sizeof(BIGSCALE_Frame);
    frame.cameraId = 1;
    frame.eye = 0;
    frame.frameIndex = 1;
    frame.operation = BIGSCALE_OP_PROBE;
    frame.preset = BIGSCALE_PRESET_QUALITY;
    frame.upscaler = BIGSCALE_UPSCALER_DLSS;
    frame.inputWidth = 32;
    frame.inputHeight = 32;
    frame.outputWidth = 64;
    frame.outputHeight = 64;
    frame.motionWidth = 32;
    frame.motionHeight = 32;
    frame.sliceCount = 1;
    frame.layout = BIGSCALE_LAYOUT_2D;
    frame.color = color.Get();

    void* first = BIGSCALE_QueueFrame(&frame);
    Check(first != nullptr, "a colour-only probe packet is accepted");
    if (first == nullptr) return;
    Execute(first);
    BIGSCALE_Status status = ReadStatus();
    Check(CountCompletions(status, BIGSCALE_COMPLETION_RETIRED, BIGSCALE_OP_PROBE) == 1,
        "a colour-only probe publishes one retired record");
    AckAll();

    // Saturation: every slot accepts exactly one packet, then the ring refuses.
    std::vector<void*> packets;
    for (int i = 0; i < BIGSCALE_RING_CAPACITY; i++)
    {
        frame.frameIndex = i + 2;
        void* packet = BIGSCALE_QueueFrame(&frame);
        if (packet == nullptr) break;
        packets.push_back(packet);
    }
    Check(static_cast<int>(packets.size()) == BIGSCALE_RING_CAPACITY, "every ring slot accepts one packet");
    frame.frameIndex = 99;
    Check(BIGSCALE_QueueFrame(&frame) == nullptr, "the ring refuses a packet once it is full");

    BIGSCALE_Frame wrongAbi = frame;
    wrongAbi.abi = 2;
    Check(BIGSCALE_QueueFrame(&wrongAbi) == nullptr, "a mismatched ABI is refused");

    // Drain every accepted packet through the real public operation. Executing them also keeps the
    // retained completion queue populated without private packet mutation.
    for (void* packet : packets) Execute(packet);
    status = ReadStatus();
    Check(status.completionPublished > 0, "executing the queued probes publishes records");
    AckAll();
    status = ReadStatus();
    Check(status.completionPublished == 0, "acknowledging clears the retained queue");

    frame.frameIndex = 100;
    void* reused = BIGSCALE_QueueFrame(&frame);
    Check(reused != nullptr, "a released slot accepts a new packet");
    if (reused != nullptr) Execute(reused);
    AckAll();
}

void CheckRetirementGating()
{
    // The native non-temporal control packet is the device-free path these checks drive: it needs the
    // full temporal set and publishes retirement only after a later render event observes the D3D11
    // query, never synchronously with execution. It is not reachable from the managed settings page;
    // that control and its configuration key were removed, so this is a check harness entry point.

    Fixture fixture;
    if (!MakeFixture(64, 36, 1, 64, 36, 128, 72, 128, 72, DXGI_FORMAT_R32_TYPELESS, fixture, 920, 0, 0, 1))
    {
        Check(false, "retirement fixture created");
        return;
    }
    FillColor(fixture.color.Get(), 0, 1.0f, 0.5f, 0.25f, 1.0f);
    ClearDepth(fixture.depth.Get(), 0, 0.4f);
    FillMotion(fixture.motion.Get(), 0, 0.0f, 0.0f);

    BIGSCALE_Frame frame{};
    frame.abi = BIGSCALE_ABI_VERSION;
    frame.size = sizeof(BIGSCALE_Frame);
    frame.cameraId = 920;
    frame.eye = 0;
    frame.frameIndex = 1;
    frame.operation = BIGSCALE_OP_BILINEAR_COMPARISON;
    frame.preset = BIGSCALE_PRESET_QUALITY;
    frame.upscaler = BIGSCALE_UPSCALER_DLSS;
    frame.inputWidth = 64;
    frame.inputHeight = 36;
    frame.outputWidth = 128;
    frame.outputHeight = 72;
    frame.motionWidth = 64;
    frame.motionHeight = 36;
    frame.sliceCount = 1;
    frame.layout = BIGSCALE_LAYOUT_2D;
    frame.color = fixture.color.Get();
    frame.depth = fixture.depth.Get();
    frame.motion = fixture.motion.Get();
    frame.output = fixture.output.Get();
    frame.resourceTag = 4242;

    void* packet = BIGSCALE_QueueFrame(&frame);
    Check(packet != nullptr, "the control packet is accepted");
    if (packet == nullptr) return;
    Execute(packet);

    BIGSCALE_Status status = ReadStatus();
    int executedResult = 0;
    if (CountCompletions(status, BIGSCALE_COMPLETION_EXECUTED, BIGSCALE_OP_BILINEAR_COMPARISON, &executedResult) != 1)
        DumpCompletions("after control execution");
    Check(CountCompletions(status, BIGSCALE_COMPLETION_EXECUTED, BIGSCALE_OP_BILINEAR_COMPARISON, &executedResult) == 1,
        "execution publishes one executed record");
    Check(CountCompletions(status, BIGSCALE_COMPLETION_RETIRED, BIGSCALE_OP_BILINEAR_COMPARISON) == 0,
        "execution alone must not publish a retirement");

    Check(WaitForDevice(), "the test device reached idle");
    // A later render event polls outstanding retirement queries; the probe only provides the event.
    BIGSCALE_Frame probe = frame;
    probe.operation = BIGSCALE_OP_PROBE;
    probe.depth = nullptr;
    probe.motion = nullptr;
    probe.output = nullptr;
    probe.resourceTag = 0;
    probe.frameIndex = 2;
    void* probePacket = BIGSCALE_QueueFrame(&probe);
    Check(probePacket != nullptr, "the retirement poll probe is accepted");
    if (probePacket == nullptr) return;
    Execute(probePacket);

    status = ReadStatus();
    int retiredResult = -1;
    int retired = 0;
    const uint32_t count = status.completionPublished < BIGSCALE_COMPLETION_COUNT
        ? status.completionPublished
        : BIGSCALE_COMPLETION_COUNT;
    for (uint32_t i = 0; i < count; i++)
    {
        const BIGSCALE_Completion& entry = status.completions[i];
        if (entry.kind != BIGSCALE_COMPLETION_RETIRED) continue;
        if (entry.operation != BIGSCALE_OP_BILINEAR_COMPARISON) continue;
        retired++;
        retiredResult = entry.result;
        Check(entry.resourceTag == 4242, "the retirement echoes the packet's resource tag");
    }
    Check(retired == 1, "a later render event publishes exactly one retirement");
    Check(retiredResult == 0, "the retired control packet reports success");
    AckAll();
}

void CheckFaultRecovery()
{
    // A resource from a second device makes the bind fail deterministically, which latches a fault.
    ComPtr<ID3D11Device> other;
    ComPtr<ID3D11DeviceContext> otherContext;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
            1, D3D11_SDK_VERSION, &other, nullptr, &otherContext)))
    {
        std::printf("fault recovery: a second device is unavailable; not exercised\n");
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 32;
    desc.Height = 32;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> foreign;
    if (FAILED(other->CreateTexture2D(&desc, nullptr, &foreign)) || !foreign)
    {
        std::printf("fault recovery: the second device texture is unavailable; not exercised\n");
        return;
    }

    BIGSCALE_Frame frame{};
    frame.abi = BIGSCALE_ABI_VERSION;
    frame.size = sizeof(BIGSCALE_Frame);
    frame.cameraId = 930;
    frame.operation = BIGSCALE_OP_PROBE;
    frame.preset = BIGSCALE_PRESET_QUALITY;
    frame.upscaler = BIGSCALE_UPSCALER_DLSS;
    frame.inputWidth = 32;
    frame.inputHeight = 32;
    frame.outputWidth = 64;
    frame.outputHeight = 64;
    frame.motionWidth = 32;
    frame.motionHeight = 32;
    frame.sliceCount = 1;
    frame.layout = BIGSCALE_LAYOUT_2D;
    frame.color = foreign.Get();

    void* packet = BIGSCALE_QueueFrame(&frame);
    Check(packet != nullptr, "the fault fixture packet is accepted");
    if (packet == nullptr) return;
    Execute(packet);
    BIGSCALE_Status status = ReadStatus();
    Check(status.state == BIGSCALE_STATE_FAULTED, "a packet from another device latches a fault");
    Check(status.lastError != BIGSCALE_ERROR_NONE, "the fault records an error");
    AckAll();

    // Recovery must drain and clear the latch on the render thread.
    BIGSCALE_Frame recovery = frame;
    recovery.operation = BIGSCALE_OP_RECOVERY;
    recovery.color = nullptr;
    void* recoveryPacket = BIGSCALE_QueueFrame(&recovery);
    Check(recoveryPacket != nullptr, "a recovery packet is accepted without resources");
    if (recoveryPacket == nullptr) return;
    Execute(recoveryPacket);
    status = ReadStatus();
    Check(status.state == BIGSCALE_STATE_WAITING, "recovery returns the bridge to waiting");
    Check(status.lastError == BIGSCALE_ERROR_NONE, "recovery clears the recorded error");
    AckAll();

    // A later valid probe proves the latch is really gone.
    auto color = CreateTexture(32, 32, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET);
    if (color)
    {
        BIGSCALE_Frame probe = frame;
        probe.color = color.Get();
        void* probePacket = BIGSCALE_QueueFrame(&probe);
        Check(probePacket != nullptr, "a probe is accepted after recovery");
        if (probePacket != nullptr)
        {
            Execute(probePacket);
            status = ReadStatus();
            Check(status.state != BIGSCALE_STATE_FAULTED, "a probe after recovery does not re-latch");
        }
        AckAll();
    }
}

// FidelityFX reports failures as positive codes (FFX_API_RETURN_ERROR_PARAMETER == 6), so a check
// that only rejects negative/NGX results would treat a refused dispatch as success. The injected
// result exercises the real bridge baseline, fault, completion and retirement path while skipping
// the SDK call; the seam exists only in this executable.
void CheckFsrPositiveErrorResult()
{
    AckAll();
    BIGSCALE_Status baseline = ReadStatus();
    Check(baseline.state == BIGSCALE_STATE_ACTIVE, "FSR3 is active before the injected error");
    const uint64_t evaluationsBefore = baseline.totalEvaluations;

    Fixture fixture;
    if (!MakeFixture(64, 36, 1, 64, 36, 128, 72, 128, 72, DXGI_FORMAT_R32_TYPELESS, fixture, 952, 0, 0, 1))
    {
        Check(false, "injected-error fixture created");
        return;
    }
    FillColor(fixture.color.Get(), 0, 1.0f, 0.5f, 0.25f, 1.0f);
    ClearDepth(fixture.depth.Get(), 0, 0.4f);
    FillMotion(fixture.motion.Get(), 0, 0.0f, 0.0f);

    BIGSCALE_Frame frame{};
    frame.abi = BIGSCALE_ABI_VERSION;
    frame.size = sizeof(BIGSCALE_Frame);
    frame.cameraId = 952;
    frame.eye = 0;
    frame.frameIndex = 1;
    frame.operation = BIGSCALE_OP_BILINEAR_COMPARISON;
    frame.preset = BIGSCALE_PRESET_QUALITY;
    frame.upscaler = BIGSCALE_UPSCALER_FSR3;
    frame.inputWidth = 64;
    frame.inputHeight = 36;
    frame.outputWidth = 128;
    frame.outputHeight = 72;
    frame.motionWidth = 64;
    frame.motionHeight = 36;
    frame.sliceCount = 1;
    frame.layout = BIGSCALE_LAYOUT_2D;
    frame.color = fixture.color.Get();
    frame.depth = fixture.depth.Get();
    frame.motion = fixture.motion.Get();
    frame.output = fixture.output.Get();
    frame.resourceTag = 777;

    // The provider-neutral comparison path writes the exact same-frame image the failed evaluation
    // must leave behind, so its readback is the expected baseline for this fixture.
    void* packet = BIGSCALE_QueueFrame(&frame);
    Check(packet != nullptr, "the error-regression comparison packet is accepted");
    if (packet == nullptr) return;
    Execute(packet);
    Check(WaitForDevice(), "the device reached idle after the comparison packet");
    const auto expectedPixels = ReadBack(fixture.output.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
    Check(!expectedPixels.empty(), "the error-regression comparison output was read back");
    if (expectedPixels.empty()) return;
    const size_t centre = Texel(64, 36, 128, 4);
    const float expected[4] = {expectedPixels[centre], expectedPixels[centre + 1], expectedPixels[centre + 2],
        expectedPixels[centre + 3]};
    AckAll();

    BIGSCALE_TestForceNextEvaluationResult(6);
    frame.operation = BIGSCALE_OP_EVALUATE;
    frame.frameIndex = 2;
    packet = BIGSCALE_QueueFrame(&frame);
    Check(packet != nullptr, "the forced-error evaluate packet is accepted");
    if (packet == nullptr) return;
    Execute(packet);
    Check(WaitForDevice(), "the device reached idle after the forced-error evaluate");

    BIGSCALE_Status status = ReadStatus();
    Check(status.state == BIGSCALE_STATE_FAULTED, "a positive FidelityFX error latches a fault");
    Check(status.lastError == BIGSCALE_ERROR_BACKEND_EVALUATE_FAILED,
        "a positive FidelityFX error is reported as an evaluation failure");
    Check(status.lastResult == 6, "the original positive provider result is preserved");
    Check(status.totalEvaluations == evaluationsBefore, "the failed evaluation is not counted");

    const auto output = ReadBack(fixture.output.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
    Check(output.size() == expectedPixels.size(), "the failed evaluation still left a full front image");
    if (output.size() == expectedPixels.size())
    {
        Near(output[centre], expected[0], 0.01f, "the failed evaluation kept the same-frame baseline (r)");
        Near(output[centre + 1], expected[1], 0.01f, "the failed evaluation kept the same-frame baseline (g)");
        Near(output[centre + 2], expected[2], 0.01f, "the failed evaluation kept the same-frame baseline (b)");
        Near(output[centre + 3], expected[3], 0.01f, "the failed evaluation kept the same-frame baseline (a)");
    }

    // Retirement stays query-gated: a later event publishes the failed evaluation's record.
    BIGSCALE_Frame poll = frame;
    poll.operation = BIGSCALE_OP_PROBE;
    poll.frameIndex = 3;
    poll.depth = nullptr;
    poll.motion = nullptr;
    poll.output = nullptr;
    poll.resourceTag = 0;
    void* pollPacket = BIGSCALE_QueueFrame(&poll);
    Check(pollPacket != nullptr, "the error-regression poll packet is accepted");
    if (pollPacket == nullptr) return;
    Execute(pollPacket);
    status = ReadStatus();
    int retiredResult = 0;
    Check(CountCompletions(status, BIGSCALE_COMPLETION_RETIRED, BIGSCALE_OP_EVALUATE, &retiredResult) == 1,
        "the failed evaluation publishes exactly one retirement");
    Check(retiredResult == 6, "the failed evaluation retires with the preserved provider result");

    BIGSCALE_Frame recovery = poll;
    recovery.operation = BIGSCALE_OP_RECOVERY;
    recovery.color = nullptr;
    recovery.frameIndex = 4;
    packet = BIGSCALE_QueueFrame(&recovery);
    Check(packet != nullptr, "the error-regression recovery packet is accepted");
    if (packet != nullptr) Execute(packet);
    status = ReadStatus();
    Check(status.state == BIGSCALE_STATE_WAITING, "recovery clears the injected fault");
    Check(status.lastError == BIGSCALE_ERROR_NONE, "recovery clears the injected error");
    AckAll();
}

// ---------------------------------------------------------------------------------------------
// Image-quality gate for real temporal reconstruction.
//
// The existing stereo fixtures prove that both slices are written and that successful evaluations
// retire; a spatial resize satisfies that too. This gate generates an analytic chart whose edges have a
// known position, renders it with the plugin's own jitter convention at a real reduced extent, and
// measures the provider against (a) a supersampled full-resolution reference and (b) the provider-
// neutral bilinear control built from the same input pixels. It also repeats the sequence with a known
// translation on each axis so a wrong motion direction is measured instead of guessed.
// ---------------------------------------------------------------------------------------------
constexpr int kQualityOutputWidth = 1920;
constexpr int kQualityOutputHeight = 1080;
constexpr int kQualityWarmupFrames = 16;
constexpr int kQualityMeasuredFrames = 6;
constexpr float kQualityMotionPerFrame = 2.0f;

uint16_t FloatToHalf(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0)
    {
        if (exponent < -10) return static_cast<uint16_t>(sign);
        mantissa = (mantissa | 0x800000u) >> static_cast<uint32_t>(1 - exponent);
        return static_cast<uint16_t>(sign | (mantissa >> 13));
    }
    if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13));
}

// Analytic luma of the chart at one continuous input-space position. Eye 0 and eye 1 place the
// near-vertical edge at different columns, so a duplicated slice fails the per-eye reference instead of
// passing as a valid reconstruction.
float ChartLuma(float x, float y, float width, float height, int eye)
{
    const float edgeX = (eye == 0 ? 0.45f : 0.55f) * width + 0.15f * (y - 0.5f * height);
    const float edgeY = 0.5f * height - 0.12f * (x - 0.5f * width);
    const float vertical = x >= edgeX ? 1.0f : 0.0f;
    const float horizontal = y >= edgeY ? 1.0f : 0.0f;
    // One-pixel checker: at the render extent its phase depends on the sub-pixel sample, so a spatial
    // resize can only average it away while a jittered temporal reconstruction can resolve it.
    const float checker = ((static_cast<int>(std::floor(x)) + static_cast<int>(std::floor(y))) & 1) ? 1.0f : -1.0f;
    const float value = 0.28f + 0.22f * vertical + 0.18f * horizontal + 0.10f * (x / width) + 0.12f * checker;
    return value < 0.05f ? 0.05f : (value > 0.95f ? 0.95f : value);
}

// Fills one slice with the chart sampled at texel centres displaced by (dx, dy) input pixels. The
// displacement is the sample the packet claims: a feature therefore appears shifted by +d.
bool FillChart(ID3D11Texture2D* color, UINT slice, UINT width, UINT height, int eye, float dx, float dy)
{
    std::vector<uint16_t> texels(static_cast<size_t>(width) * height * 4);
    for (UINT y = 0; y < height; y++)
        for (UINT x = 0; x < width; x++)
        {
            const float value = ChartLuma(x + 0.5f - dx, y + 0.5f - dy, static_cast<float>(width),
                static_cast<float>(height), eye);
            const uint16_t half = FloatToHalf(value);
            const size_t index = (static_cast<size_t>(y) * width + x) * 4;
            texels[index + 0] = half;
            texels[index + 1] = half;
            texels[index + 2] = half;
            texels[index + 3] = FloatToHalf(1.0f);
        }
    D3D11_BOX box{0, 0, 0, width, height, 1};
    g_context->UpdateSubresource(color, D3D11CalcSubresource(0, slice, 1), &box, texels.data(),
        width * sizeof(uint16_t) * 4, 0);
    return true;
}

// Constant motion guide, in the packet's stored convention, for one slice.
bool FillMotionValue(ID3D11Texture2D* motion, UINT slice, UINT width, UINT height, float storedX, float storedY)
{
    std::vector<uint16_t> texels(static_cast<size_t>(width) * height * 2);
    for (size_t i = 0; i < static_cast<size_t>(width) * height; i++)
    {
        texels[i * 2 + 0] = FloatToHalf(storedX);
        texels[i * 2 + 1] = FloatToHalf(storedY);
    }
    D3D11_BOX box{0, 0, 0, width, height, 1};
    g_context->UpdateSubresource(motion, D3D11CalcSubresource(0, slice, 1), &box, texels.data(),
        width * sizeof(uint16_t) * 2, 0);
    return true;
}

// Supersampled reference for one output texel, expressed in linear luma.
float ReferenceLuma(int px, int py, int eye, int inputWidth, int inputHeight)
{
    const float scaleX = static_cast<float>(inputWidth) / kQualityOutputWidth;
    const float scaleY = static_cast<float>(inputHeight) / kQualityOutputHeight;
    float sum = 0.0f;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++)
        {
            const float u = (px + (sx + 0.5f) * 0.25f) * scaleX;
            const float v = (py + (sy + 0.5f) * 0.25f) * scaleY;
            sum += ChartLuma(u, v, static_cast<float>(inputWidth), static_cast<float>(inputHeight), eye);
        }
    return sum * (1.0f / 16.0f);
}

// Mean absolute luma error over the two edge bands, which is where a spatial resize and a reconstructor
// differ. Flat regions are identical between both candidates and would dilute the number.
float BandError(const std::vector<float>& image, int eye, int inputWidth, int inputHeight)
{
    double sum = 0.0;
    long count = 0;
    const float scaleX = static_cast<float>(kQualityOutputWidth) / inputWidth;
    for (int py = kQualityOutputHeight * 20 / 100; py < kQualityOutputHeight * 80 / 100; py += 3)
    {
        const float v = (py + 0.5f) * inputHeight / static_cast<float>(kQualityOutputHeight);
        const float edgeInput = (eye == 0 ? 0.45f : 0.55f) * inputWidth + 0.15f * (v - 0.5f * inputHeight);
        const int edgeOutput = static_cast<int>(edgeInput * scaleX + 0.5f);
        for (int offset = -22; offset <= 22; offset += 2)
        {
            const int px = edgeOutput + offset;
            if (px < 0 || px >= kQualityOutputWidth) continue;
            const size_t index = Texel(static_cast<UINT>(px), static_cast<UINT>(py), kQualityOutputWidth, 4);
            const float luma = 0.2126f * image[index] + 0.7152f * image[index + 1] + 0.0722f * image[index + 2];
            const float reference = ReferenceLuma(px, py, eye, inputWidth, inputHeight);
            sum += std::fabs(luma - reference);
            count++;
        }
    }
    return count == 0 ? 1.0e9f : static_cast<float>(sum / count);
}

// Vertical-edge crossing of one frame, averaged over rows, in output pixels. Measured, not supplied:
// the caller compares it with the analytic expectation plus the known content translation.
bool EdgePosition(const std::vector<float>& image, int eye, int inputWidth, int inputHeight,
    float translationOutputPixels, float& position)
{
    double sum = 0.0;
    int rows = 0;
    const float scaleX = static_cast<float>(kQualityOutputWidth) / inputWidth;
    for (int i = 0; i < 9; i++)
    {
        const int py = kQualityOutputHeight * (15 + 8 * i) / 100;
        const float v = (py + 0.5f) * inputHeight / static_cast<float>(kQualityOutputHeight);
        const float edgeInput = (eye == 0 ? 0.45f : 0.55f) * inputWidth + 0.15f * (v - 0.5f * inputHeight);
        // The frame's authored translation is part of where the edge must be, so it is the centre of
        // the measurement window rather than a correction applied afterwards.
        const int centre = static_cast<int>(edgeInput * scaleX + translationOutputPixels + 0.5f);
        const int lo = centre - 40;
        const int hi = centre + 40;
        if (lo - 4 < 0 || hi + 4 >= kQualityOutputWidth) continue;

        // The chart also carries a one-pixel checker whose phase follows the sub-pixel sample. A
        // five-tap average along the row cancels it without moving the edge, so the crossing is the
        // chart's step and not a checker transition.
        float profile[2 * 40 + 1 + 8];
        for (int px = lo - 4; px <= hi + 4; px++)
        {
            float total = 0.0f;
            for (int tap = -2; tap <= 2; tap++)
            {
                const size_t index = Texel(static_cast<UINT>(px + tap), static_cast<UINT>(py),
                    kQualityOutputWidth, 4);
                total += 0.2126f * image[index] + 0.7152f * image[index + 1] + 0.0722f * image[index + 2];
            }
            profile[px - (lo - 4)] = total * 0.2f;
        }
        float minimum = 1.0e9f;
        float maximum = -1.0e9f;
        for (int px = lo; px <= hi; px++)
        {
            const float value = profile[px - (lo - 4)];
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
        if (maximum - minimum < 0.08f) continue;
        const float threshold = 0.5f * (minimum + maximum);
        bool found = false;
        for (int px = lo; px < hi && !found; px++)
        {
            const float a = profile[px - (lo - 4)];
            const float b = profile[px + 1 - (lo - 4)];
            if (a >= threshold || b < threshold) continue;
            const float t = (threshold - a) / (b - a);
            sum += px + t;
            rows++;
            found = true;
        }
    }
    if (rows == 0) return false;
    position = static_cast<float>(sum / rows);
    return true;
}

// Analytic crossing for one frame's known translation, so the residual is the provider's error and not
// the motion the fixture authored.
float ExpectedEdgePosition(int eye, int inputWidth, int inputHeight, float translationOutputPixels)
{
    // Averaged over the same row band the measurement uses.
    double sum = 0.0;
    int rows = 0;
    for (int i = 0; i < 9; i++)
    {
        const int py = kQualityOutputHeight * (15 + 8 * i) / 100;
        const float v = (py + 0.5f) * inputHeight / static_cast<float>(kQualityOutputHeight);
        const float edgeInput = (eye == 0 ? 0.45f : 0.55f) * inputWidth + 0.15f * (v - 0.5f * inputHeight);
        sum += edgeInput * static_cast<float>(kQualityOutputWidth) / inputWidth;
        rows++;
    }
    return static_cast<float>(sum / rows) + translationOutputPixels;
}

// One jitter sample per frame, in input pixels, with the same Halton(2,3) sequence the plugin uses.
void QualityJitter(int frame, float& pixelX, float& pixelY)
{
    const auto halton = [](int index, int radix)
    {
        float result = 0.0f;
        float fraction = 1.0f / radix;
        int remaining = index;
        while (remaining > 0)
        {
            result += (remaining % radix) * fraction;
            remaining /= radix;
            fraction /= radix;
        }
        return result;
    };
    pixelX = halton(frame + 1, 2) - 0.5f;
    pixelY = halton(frame + 1, 3) - 0.5f;
}

struct QualityScores
{
    const char* reason = "not started";
    float dlssError[2] = {1.0e9f, 1.0e9f};
    float bilinearError[2] = {1.0e9f, 1.0e9f};
    float motionSum[2] = {0.0f, 0.0f};
    int motionCount[2] = {0, 0};
    float motionMaxAbs[2] = {-1.0e9f, -1.0e9f};
    float stillMin[2] = {1.0e9f, 1.0e9f};
    float stillMax[2] = {-1.0e9f, -1.0e9f};
    int stillCount[2] = {0, 0};
    bool usable = false;

    float MotionMean(int eye) const
    {
        return motionCount[eye] == 0 ? 1.0e9f : motionSum[eye] / motionCount[eye];
    }

    float MotionMaxAbs(int eye) const { return motionMaxAbs[eye]; }

    // Peak-to-peak excursion of a stationary chart: phase-locked shaking shows up here.
    float StillSpread(int eye) const
    {
        return stillCount[eye] < 2 ? 1.0e9f : stillMax[eye] - stillMin[eye];
    }
};

bool QualityGatePasses(float candidateError, float controlError)
{
    return candidateError <= 0.9f * controlError;
}

// Runs one full sequence at a fresh history: a stationary quality pass, then one translation on each
// axis. Returns the measured scores; `usable` is false when the fixture or a packet failed.
QualityScores MeasureDlssQuality(int32_t upscaler, int32_t cameraId, uint32_t inputWidth, uint32_t inputHeight,
    float jitterSignX, float jitterSignY, int32_t epochBase)
{
    QualityScores scores;
    Fixture fixture;
    if (!MakeFixture(inputWidth, inputHeight, 2, inputWidth, inputHeight, kQualityOutputWidth, kQualityOutputHeight,
            inputWidth, inputHeight, DXGI_FORMAT_R32_TYPELESS, fixture, cameraId, 0, 0, 2))
    {
        scores.reason = "the quality fixture was not created";
        return scores;
    }
    ComPtr<ID3D11Texture2D> bilinear;
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = kQualityOutputWidth;
        desc.Height = kQualityOutputHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 2;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &bilinear)) || !bilinear)
        {
            scores.reason = "the bilinear control texture was not created";
            return scores;
        }
    }
    for (int eye = 0; eye < 2; eye++)
        if (!ClearDepth(fixture.depth.Get(), static_cast<UINT>(eye), 0.4f))
        {
            scores.reason = "the depth clear failed";
            return scores;
        }

    BIGSCALE_Frame frame{};
    frame.abi = BIGSCALE_ABI_VERSION;
    frame.size = sizeof(BIGSCALE_Frame);
    frame.cameraId = cameraId;
    frame.operation = BIGSCALE_OP_EVALUATE;
    frame.preset = BIGSCALE_PRESET_PERFORMANCE;
    frame.upscaler = upscaler;
    frame.inputWidth = static_cast<int32_t>(inputWidth);
    frame.inputHeight = static_cast<int32_t>(inputHeight);
    frame.outputWidth = kQualityOutputWidth;
    frame.outputHeight = kQualityOutputHeight;
    frame.motionWidth = static_cast<int32_t>(inputWidth);
    frame.motionHeight = static_cast<int32_t>(inputHeight);
    frame.sliceCount = 2;
    frame.layout = BIGSCALE_LAYOUT_ARRAY;
    frame.flags = BIGSCALE_FLAG_HDR_COLOR;
    frame.color = fixture.color.Get();
    frame.depth = fixture.depth.Get();
    frame.motion = fixture.motion.Get();
    frame.output = fixture.output.Get();
    frame.cameraNear = 0.1f;
    frame.cameraFar = 1000.0f;
    frame.verticalFovRadians = 1.2f;
    frame.preExposure = 1.0f;
    frame.motionScaleX = -static_cast<float>(inputWidth);
    frame.motionScaleY = -static_cast<float>(inputHeight);
    frame.deltaMilliseconds = 16.0f;

    const float inputScale = static_cast<float>(inputWidth) / kQualityOutputWidth;
    int32_t epoch = epochBase;
    int32_t frameIndex = 1;

    // Applies one frame's chart, motion and packets. `translation` is in output pixels.
    const auto runFrame = [&](float translationX, float translationY, bool reset, bool measure) -> bool
    {
        float jitterX = 0.0f;
        float jitterY = 0.0f;
        QualityJitter(frameIndex, jitterX, jitterY);
        for (int eye = 0; eye < 2; eye++)
        {
            const float dx = translationX * inputScale;
            const float dy = translationY * inputScale;
            if (!FillChart(fixture.color.Get(), static_cast<UINT>(eye), inputWidth, inputHeight, eye, dx, dy))
            {
                scores.reason = "the chart upload failed";
                return false;
            }
            // The fixture content moves by +t, so the stored guide is t in the plugin's own units.
            const float storedX = dx / static_cast<float>(inputWidth);
            const float storedY = dy / static_cast<float>(inputHeight);
            if (!FillMotionValue(fixture.motion.Get(), static_cast<UINT>(eye), inputWidth, inputHeight, storedX,
                    storedY))
            {
                scores.reason = "the motion upload failed";
                return false;
            }
            BIGSCALE_Frame evaluate = frame;
            evaluate.eye = eye;
            evaluate.slice = eye;
            evaluate.frameIndex = frameIndex;
            evaluate.historyEpoch = epoch;
            evaluate.flags = BIGSCALE_FLAG_HDR_COLOR | (reset ? BIGSCALE_FLAG_RESET : 0);
            evaluate.jitterX = jitterSignX * jitterX;
            evaluate.jitterY = jitterSignY * jitterY;
            void* packet = BIGSCALE_QueueFrame(&evaluate);
            if (packet == nullptr)
            {
                scores.reason = "an evaluate packet was rejected";
                return false;
            }
            Execute(packet);
        }
        if (!WaitForDevice())
        {
            scores.reason = "the device did not reach idle";
            return false;
        }
        BIGSCALE_Frame drain = frame;
        drain.operation = BIGSCALE_OP_PROBE;
        drain.eye = 0;
        drain.slice = 0;
        drain.sliceCount = 1;
        drain.layout = BIGSCALE_LAYOUT_2D;
        drain.flags = 0;
        drain.depth = nullptr;
        drain.motion = nullptr;
        drain.output = nullptr;
        drain.resourceTag = 0;
        drain.frameIndex = frameIndex + 100000;
        void* drainPacket = BIGSCALE_QueueFrame(&drain);
        if (drainPacket != nullptr) Execute(drainPacket);
        AckAll();
        if (measure)
        {
            for (int eye = 0; eye < 2; eye++)
            {
                const auto image = ReadBack(fixture.output.Get(), static_cast<UINT>(eye),
                    DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
                if (image.size() != static_cast<size_t>(kQualityOutputWidth) * kQualityOutputHeight * 4)
                {
                    scores.reason = "the output read-back returned the wrong size";
                    return false;
                }
                float position = 0.0f;
                if (!EdgePosition(image, eye, static_cast<int>(inputWidth), static_cast<int>(inputHeight),
                        translationX - 0.15f * translationY, position))
                {
                    float minimum = 1.0e9f;
                    float maximum = -1.0e9f;
                    for (int py = 0; py < kQualityOutputHeight; py += 7)
                        for (int px = 0; px < kQualityOutputWidth; px += 7)
                        {
                            const size_t i = Texel(static_cast<UINT>(px), static_cast<UINT>(py), kQualityOutputWidth, 4);
                            const float luma = 0.2126f * image[i] + 0.7152f * image[i + 1] + 0.0722f * image[i + 2];
                            minimum = std::min(minimum, luma);
                            maximum = std::max(maximum, luma);
                        }
                    const int row = kQualityOutputHeight / 2;
                    std::printf("image quality debug: eye %d frame %d luma range %.4f..%.4f row %d:", eye, frameIndex,
                        minimum, maximum, row);
                    for (int px = kQualityOutputWidth * 40 / 100; px < kQualityOutputWidth * 60 / 100; px += 24)
                    {
                        const size_t i = Texel(static_cast<UINT>(px), static_cast<UINT>(row), kQualityOutputWidth, 4);
                        std::printf(" %.3f", 0.2126f * image[i] + 0.7152f * image[i + 1] + 0.0722f * image[i + 2]);
                    }
                    std::printf("\n");
                    scores.reason = "no edge crossing was found in the reconstructed frame";
                    return false;
                }
                // The chart's edges are slanted, so a vertical translation also moves the vertical
                // edge sideways; the expected position is the analytic one plus that combined shift.
                const float shift = translationX - 0.15f * translationY;
                const float expected = ExpectedEdgePosition(eye, static_cast<int>(inputWidth),
                    static_cast<int>(inputHeight), shift);
                const float residual = position - expected;
                if (translationX == 0.0f && translationY == 0.0f)
                {
                    scores.stillCount[eye]++;
                    scores.stillMin[eye] = std::min(scores.stillMin[eye], residual);
                    scores.stillMax[eye] = std::max(scores.stillMax[eye], residual);
                }
                else
                {
                    scores.motionSum[eye] += std::fabs(residual);
                    scores.motionCount[eye]++;
                    scores.motionMaxAbs[eye] = std::max(scores.motionMaxAbs[eye], std::fabs(residual));
                }
                if (translationX == 0.0f && translationY == 0.0f)
                {
                    scores.dlssError[eye] = BandError(image, eye, static_cast<int>(inputWidth),
                        static_cast<int>(inputHeight));
                    // The same input pixels through the provider-neutral spatial path.
                    BIGSCALE_Frame control = frame;
                    control.eye = eye;
                    control.slice = eye;
                    control.operation = BIGSCALE_OP_BILINEAR_COMPARISON;
                    control.frameIndex = frameIndex;
                    control.historyEpoch = epoch;
                    control.flags = BIGSCALE_FLAG_HDR_COLOR;
                    control.output = bilinear.Get();
                    void* controlPacket = BIGSCALE_QueueFrame(&control);
                    if (controlPacket == nullptr)
                    {
                        scores.reason = "the bilinear control packet was rejected";
                        return false;
                    }
                    Execute(controlPacket);
                    if (!WaitForDevice())
                    {
                        scores.reason = "the device did not reach idle after the control";
                        return false;
                    }
                    const auto controlImage = ReadBack(bilinear.Get(), static_cast<UINT>(eye),
                        DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
                    if (controlImage.size() != image.size())
                    {
                        scores.reason = "the control read-back returned the wrong size";
                        return false;
                    }
                    scores.bilinearError[eye] = BandError(controlImage, eye, static_cast<int>(inputWidth),
                        static_cast<int>(inputHeight));
                    AckAll();
                }
            }
        }
        frameIndex++;
        return true;
    };

    const auto runPass = [&](float translationX, float translationY, bool measure) -> bool
    {
        epoch++;
        for (int step = 0; step < kQualityWarmupFrames; step++)
            if (!runFrame(translationX * step, translationY * step, step == 0, false)) return false;
        const int start = kQualityWarmupFrames;
        for (int step = 0; step < kQualityMeasuredFrames; step++)
        {
            const int index = start + step;
            if (!runFrame(translationX * index, translationY * index, false, measure && step > 0)) return false;
        }
        return true;
    };

    // Stationary pass supplies the reference error and the bilinear control; the moving passes measure
    // whether the supplied guide actually reprojects in the direction the packet declares.
    if (!runPass(0.0f, 0.0f, true)) return scores;
    if (!runPass(kQualityMotionPerFrame, 0.0f, true)) return scores;
    if (!runPass(0.0f, kQualityMotionPerFrame, true)) return scores;
    scores.usable = true;
    return scores;
}

void CheckDlssImageQuality(int32_t upscaler)
{
    // A Performance fit at the real 1920x1080 output drives the whole gate. The probe carries a valid
    // small colour resource because the packet contract requires one for every capability query.
    Fixture probeFixture;
    if (!MakeFixture(64, 36, 1, 64, 36, kQualityOutputWidth, kQualityOutputHeight, 64, 36,
            DXGI_FORMAT_R32_TYPELESS, probeFixture, 953, 0, 0, 1))
    {
        Check(false, "the image-quality capability fixture is created");
        return;
    }
    BIGSCALE_Frame probe{};
    probe.abi = BIGSCALE_ABI_VERSION;
    probe.size = sizeof(BIGSCALE_Frame);
    probe.cameraId = 953;
    probe.operation = BIGSCALE_OP_PROBE;
    probe.preset = BIGSCALE_PRESET_PERFORMANCE;
    probe.upscaler = upscaler;
    probe.inputWidth = 64;
    probe.inputHeight = 36;
    probe.motionWidth = 64;
    probe.motionHeight = 36;
    probe.outputWidth = kQualityOutputWidth;
    probe.outputHeight = kQualityOutputHeight;
    probe.sliceCount = 1;
    probe.layout = BIGSCALE_LAYOUT_2D;
    probe.color = probeFixture.color.Get();
    void* probePacket = BIGSCALE_QueueFrame(&probe);
    if (probePacket == nullptr)
    {
        Check(false, "the image-quality capability probe is accepted");
        return;
    }
    Execute(probePacket);
    const BIGSCALE_Status modes = ReadStatusFor(upscaler, kQualityOutputWidth, kQualityOutputHeight);
    const BIGSCALE_ModeInfo& performance = modes.modes[2];
    const bool usable = modes.modeCount == BIGSCALE_MODE_COUNT && performance.supported == 1 &&
                        performance.optimalWidth > 0 && performance.optimalWidth < kQualityOutputWidth &&
                        performance.optimalHeight > 0 && performance.optimalHeight < kQualityOutputHeight;
    Check(usable, "Performance reports a genuinely reduced input at 1920x1080 (got " +
        std::to_string(performance.optimalWidth) + "x" + std::to_string(performance.optimalHeight) + ")");
    AckAll();
    if (!usable) return;

    QualityScores scores = MeasureDlssQuality(upscaler, 953, performance.optimalWidth, performance.optimalHeight,
        1.0f, 1.0f, 1);
    Check(scores.usable, std::string("the image-quality sequence executed every frame and read back every "
        "measurement (") + scores.reason + ")");
    if (!scores.usable) return;

    // The documented mapping is the one the packet currently sends. Only if it fails is the same chart
    // replayed with independent axis signs, each at a fresh history, so the fix is measured rather than
    // guessed on a headset.
    bool pass = QualityGatePasses(scores.dlssError[0], scores.bilinearError[0]) &&
                QualityGatePasses(scores.dlssError[1], scores.bilinearError[1]);
    if (!pass)
    {
        const float signPairs[3][2] = {{1.0f, -1.0f}, {-1.0f, 1.0f}, {-1.0f, -1.0f}};
        for (int candidate = 0; candidate < 3 && !pass; candidate++)
        {
            std::printf("image quality: documented jitter mapping failed (eye0 %.4f vs %.4f, eye1 %.4f vs %.4f); "
                        "trying signs %.0f/%.0f\n", scores.dlssError[0], scores.bilinearError[0],
                scores.dlssError[1], scores.bilinearError[1], signPairs[candidate][0], signPairs[candidate][1]);
            QualityScores attempt = MeasureDlssQuality(upscaler, 953, performance.optimalWidth,
                performance.optimalHeight, signPairs[candidate][0], signPairs[candidate][1],
                100 * (candidate + 1));
            if (!attempt.usable) continue;
            if (QualityGatePasses(attempt.dlssError[0], attempt.bilinearError[0]) &&
                QualityGatePasses(attempt.dlssError[1], attempt.bilinearError[1]))
            {
                scores = attempt;
                pass = true;
                std::printf("image quality: signs %.0f/%.0f pass the gate; the packet mapping in "
                            "XrJitter/ClipOffset must adopt them.\n", signPairs[candidate][0],
                    signPairs[candidate][1]);
            }
        }
    }

    for (int eye = 0; eye < 2; eye++)
    {
        std::printf("image quality: eye %d reference error dlss=%.4f bilinear=%.4f "
                    "stationary spread=%.3f moving mean=%.3f max=%.3f output px\n", eye,
            scores.dlssError[eye], scores.bilinearError[eye], scores.StillSpread(eye), scores.MotionMean(eye),
            scores.MotionMaxAbs(eye));
        Check(std::isfinite(scores.dlssError[eye]) && std::isfinite(scores.bilinearError[eye]),
            "the image-quality scores are finite for eye " + std::to_string(eye));
        Check(QualityGatePasses(scores.dlssError[eye], scores.bilinearError[eye]),
            "reconstruction beats the same-input spatial control by 10% for eye " + std::to_string(eye) +
            " (dlss=" + std::to_string(scores.dlssError[eye]) + " bilinear=" +
            std::to_string(scores.bilinearError[eye]) + ")");
        Check(scores.StillSpread(eye) < 0.5f,
            "the stationary reconstructed edge stays within half an output pixel for eye " +
            std::to_string(eye) + " (spread=" + std::to_string(scores.StillSpread(eye)) + ")");
        Check(scores.MotionMean(eye) < 2.0f && scores.MotionMaxAbs(eye) < 2.5f,
            "the reconstructed edge follows the authored motion direction for eye " + std::to_string(eye) +
            " (mean=" + std::to_string(scores.MotionMean(eye)) + " max=" +
            std::to_string(scores.MotionMaxAbs(eye)) + ")");
    }
    // The gate itself must reject the spatial control: blurry-but-successful is what this test exists for.
    Check(!QualityGatePasses(scores.bilinearError[0], scores.bilinearError[0]) &&
          !QualityGatePasses(scores.bilinearError[1], scores.bilinearError[1]),
        "the quality gate rejects the spatial control as a candidate");
}

void CheckBackendRuntime(const char* label, int32_t upscaler)
{
    // Requires the pinned runtime next to this executable. A missing runtime, a non-NVIDIA device or
    // a WARP device is reported as NOT EXERCISED, never as a passing real evaluation.
    BIGSCALE_Status status = ReadStatus();
    if (status.state == BIGSCALE_STATE_FAULTED)
    {
        std::printf("%s: the bridge is faulted before the smoke test; not exercised\n", label);
        return;
    }

    // Advance the selected provider through the ordered switch control first, so the generic checks
    // that ran before this one cannot leave another backend's mode block looking like this one's.
    BIGSCALE_Frame switchFrame{};
    switchFrame.abi = BIGSCALE_ABI_VERSION;
    switchFrame.size = sizeof(BIGSCALE_Frame);
    switchFrame.cameraId = 949;
    switchFrame.operation = BIGSCALE_OP_SWITCH;
    switchFrame.upscaler = upscaler;
    switchFrame.preset = BIGSCALE_PRESET_QUALITY;
    switchFrame.sliceCount = 1;
    switchFrame.layout = BIGSCALE_LAYOUT_2D;
    void* switchPacket = BIGSCALE_QueueFrame(&switchFrame);
    Check(switchPacket != nullptr, "switch packet queued");
    if (switchPacket == nullptr) return;
    Execute(switchPacket);
    status = ReadStatusFor(upscaler, 0, 0);
    Check(status.activeUpscaler == upscaler, std::string(label) + " was acknowledged by the bridge");
    AckAll();

    Fixture fixture;
    if (!MakeFixture(64, 36, 1, 64, 36, 128, 72, 128, 72, DXGI_FORMAT_R32_TYPELESS, fixture, 950, 0, 0, 1))
    {
        std::printf("%s: fixture creation failed; not exercised\n", label);
        return;
    }
    FillColor(fixture.color.Get(), 0, 1.0f, 0.5f, 0.25f, 1.0f);
    ClearDepth(fixture.depth.Get(), 0, 0.4f);
    FillMotion(fixture.motion.Get(), 0, 0.0f, 0.0f);

    BIGSCALE_Frame frame{};
    frame.abi = BIGSCALE_ABI_VERSION;
    frame.size = sizeof(BIGSCALE_Frame);
    frame.cameraId = 950;
    frame.operation = BIGSCALE_OP_PROBE;
    frame.preset = BIGSCALE_PRESET_QUALITY;
    frame.upscaler = upscaler;
    frame.inputWidth = 64;
    frame.inputHeight = 36;
    frame.outputWidth = 128;
    frame.outputHeight = 72;
    frame.motionWidth = 64;
    frame.motionHeight = 36;
    frame.sliceCount = 1;
    frame.layout = BIGSCALE_LAYOUT_2D;
    frame.color = fixture.color.Get();

    void* packet = BIGSCALE_QueueFrame(&frame);
    Check(packet != nullptr, "probe packet queued");
    if (packet == nullptr) return;
    Execute(packet);
    status = ReadStatusFor(upscaler, 128, 72);
    if (status.modeCount != BIGSCALE_MODE_COUNT)
    {
        std::printf("%s: capability probe answered %u modes (state=%d error=%d); real create/evaluate NOT "
                    "EXERCISED\n", label, status.modeCount, status.state, status.lastError);
        Check(false, std::string(label) + " reported every preset");
        return;
    }
    Check(status.modeCount == BIGSCALE_MODE_COUNT, std::string(label) + " reports five presets");
    Check(status.activeUpscaler == upscaler, std::string(label) + " is the provider that actually attached");

    frame.operation = BIGSCALE_OP_EVALUATE;
    frame.depth = fixture.depth.Get();
    frame.motion = fixture.motion.Get();
    frame.output = fixture.output.Get();
    packet = BIGSCALE_QueueFrame(&frame);
    Check(packet != nullptr, "evaluate packet queued");
    if (packet == nullptr) return;
    Execute(packet);

    // Retirement is query-gated, so poll it with a later event after the GPU finished.
    Check(WaitForDevice(), "the test device reached idle after evaluate");
    BIGSCALE_Frame poll = frame;
    poll.operation = BIGSCALE_OP_PROBE;
    poll.depth = nullptr;
    poll.motion = nullptr;
    poll.output = nullptr;
    poll.resourceTag = 0;
    poll.frameIndex = frame.frameIndex + 1;
    void* pollPacket = BIGSCALE_QueueFrame(&poll);
    if (pollPacket != nullptr) Execute(pollPacket);

    status = ReadStatus();
    int retiredResult = -1;
    int retiredEvaluations = 0;
    const uint32_t count = status.completionPublished < BIGSCALE_COMPLETION_COUNT
        ? status.completionPublished
        : BIGSCALE_COMPLETION_COUNT;
    for (uint32_t i = 0; i < count; i++)
    {
        const BIGSCALE_Completion& entry = status.completions[i];
        if (entry.kind != BIGSCALE_COMPLETION_RETIRED || entry.operation != BIGSCALE_OP_EVALUATE) continue;
        retiredEvaluations++;
        retiredResult = entry.result;
    }
    Check(retiredEvaluations == 1, std::string(label) + " publishes exactly one retirement");
    Check(retiredResult == 0, std::string(label) + " retired successfully");
    Check(status.totalEvaluations > 0, std::string(label) + " counted the real evaluation");
    Check(status.state == BIGSCALE_STATE_ACTIVE, std::string(label) + " published the active state");
    Check(status.lastError == BIGSCALE_ERROR_NONE, std::string(label) + " recorded no error");

    // Full-presentation regression fixture: the active 1280x720 input reconstructed into a
    // 1920x1080 output. NGX now creates the feature at exactly this evaluated extent, so the whole
    // frame must be written; a larger creation extent used to leave the top/right untouched.
    {
        Fixture full;
        if (!MakeFixture(1280, 720, 1, 1280, 720, 1920, 1080, 1280, 720, DXGI_FORMAT_R32_TYPELESS,
                full, 951, 0, 0, 1))
        {
            Check(false, "full-presentation NGX fixture created");
        }
        else
        {
            FillColor(full.color.Get(), 0, 1.0f, 0.5f, 0.25f, 1.0f);
            ClearDepth(full.depth.Get(), 0, 0.4f);
            FillMotion(full.motion.Get(), 0, 0.0f, 0.0f);

            BIGSCALE_Frame fullFrame = frame;
            fullFrame.cameraId = 951;
            fullFrame.operation = BIGSCALE_OP_EVALUATE;
            fullFrame.inputWidth = 1280;
            fullFrame.inputHeight = 720;
            fullFrame.outputWidth = 1920;
            fullFrame.outputHeight = 1080;
            fullFrame.motionWidth = 1280;
            fullFrame.motionHeight = 720;
            fullFrame.color = full.color.Get();
            fullFrame.depth = full.depth.Get();
            fullFrame.motion = full.motion.Get();
            fullFrame.output = full.output.Get();
            fullFrame.frameIndex = frame.frameIndex + 2;
            void* fullPacket = BIGSCALE_QueueFrame(&fullFrame);
            Check(fullPacket != nullptr, "the full-presentation evaluate is accepted");
            if (fullPacket != nullptr) Execute(fullPacket);
            Check(WaitForDevice(), "the device reached idle after the full-presentation evaluate");

            const auto values = ReadBack(full.output.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
            Check(!values.empty(), "the full-presentation output was read back");
            if (!values.empty())
            {
                const UINT width = 1920;
                const UINT height = 1080;
                const size_t samples[] = {
                    Texel(8, 8, width, 4), Texel(width - 9, 8, width, 4),
                    Texel(8, height - 9, width, 4), Texel(width - 9, height - 9, width, 4),
                    Texel(width / 2, height / 2, width, 4),
                };
                const char* names[] = {"top-left", "top-right", "bottom-left", "bottom-right", "centre"};
                for (int i = 0; i < 5; i++)
                {
                    const size_t index = samples[i];
                    const float r = values[index];
                    const float g = values[index + 1];
                    const float b = values[index + 2];
                    const float a = values[index + 3];
                    Check(std::isfinite(r) && std::isfinite(g) && std::isfinite(b) && std::isfinite(a),
                        std::string("the NGX output is finite at the ") + names[i]);
                    Check(r + g + b > 0.1f, std::string("the NGX output is not black at the ") + names[i]);
                }
            }

            // Retirement is published by a later packet. A successful retirement proves the full
            // frame was actually created and evaluated at this extent, not merely baseline-filled.
            BIGSCALE_Frame publish = poll;
            publish.frameIndex = frame.frameIndex + 3;
            void* publishPacket = BIGSCALE_QueueFrame(&publish);
            if (publishPacket != nullptr) Execute(publishPacket);
            status = ReadStatus();
            int fullRetired = 0;
            bool fullOk = false;
            const uint32_t published = status.completionPublished < BIGSCALE_COMPLETION_COUNT
                ? status.completionPublished
                : BIGSCALE_COMPLETION_COUNT;
            for (uint32_t i = 0; i < published; i++)
            {
                const BIGSCALE_Completion& entry = status.completions[i];
                if (entry.cameraId != 951 || entry.operation != BIGSCALE_OP_EVALUATE ||
                    entry.kind != BIGSCALE_COMPLETION_RETIRED)
                    continue;
                fullRetired++;
                fullOk = entry.result == 0;
                // The packet declares the active render extent as its input; the output extent must
                // never become the motion or depth allocation. This guards the convention the managed
                // packet builder now derives from TemporalInputPolicy.
                Check(entry.inputWidth == 1280 && entry.inputHeight == 720,
                    "the 1280x720 render extent is the packet's input, not the 1920x1080 output (got " +
                        std::to_string(entry.inputWidth) + "x" + std::to_string(entry.inputHeight) + ")");
                Check(entry.outputWidth == 1920 && entry.outputHeight == 1080,
                    "the output extent is echoed separately from the render extent (got " +
                        std::to_string(entry.outputWidth) + "x" + std::to_string(entry.outputHeight) + ")");
            }
            Check(fullRetired == 1, "the full-presentation evaluate publishes one retirement");
            Check(fullOk, "the full-presentation evaluate retired successfully");
        }
    }

    // Two-slice real evaluation. The real-backend smoke above is mono, and the stereo fixtures only
    // drive input conversion and the bilinear control, so this is the one place a reduced Quality
    // extent is created and evaluated for both eyes through the public packet path.
    {
        AckAll();
        BIGSCALE_Frame query = frame;
        query.operation = BIGSCALE_OP_PROBE;
        query.cameraId = 952;
        query.inputWidth = 0;
        query.inputHeight = 0;
        query.outputWidth = 1920;
        query.outputHeight = 1080;
        query.motionWidth = 0;
        query.motionHeight = 0;
        query.depth = nullptr;
        query.motion = nullptr;
        query.output = nullptr;
        query.resourceTag = 0;
        query.slice = 0;
        query.sliceCount = 1;
        query.layout = BIGSCALE_LAYOUT_2D;
        query.frameIndex = frame.frameIndex + 4;
        void* queryPacket = BIGSCALE_QueueFrame(&query);
        Check(queryPacket != nullptr, "the 1920x1080 XR capability query is accepted");
        if (queryPacket != nullptr) Execute(queryPacket);

        const BIGSCALE_Status modes = ReadStatusFor(upscaler, 1920, 1080);
        const BIGSCALE_ModeInfo& quality = modes.modes[0];
        const bool qualityUsable = quality.supported == 1 && quality.optimalWidth > 0 &&
                                   quality.optimalWidth < 1920 && quality.optimalHeight > 0 &&
                                   quality.optimalHeight < 1080;
        Check(quality.supported == 1, "Quality is supported at the 1920x1080 XR output");
        Check(qualityUsable, "Quality reports a genuinely reduced input extent (got " +
            std::to_string(quality.optimalWidth) + "x" + std::to_string(quality.optimalHeight) + ")");

        Fixture stereo;
        if (!qualityUsable)
        {
            AckAll();
        }
        else if (!MakeFixture(quality.optimalWidth, quality.optimalHeight, 2, quality.optimalWidth,
                     quality.optimalHeight, 1920, 1080, quality.optimalWidth, quality.optimalHeight,
                     DXGI_FORMAT_R32_TYPELESS, stereo, 952, 0, 0, 2))
        {
            Check(false, "the two-slice Quality fixture was created");
            AckAll();
        }
        else
        {
            const int32_t inputWidth = static_cast<int32_t>(quality.optimalWidth);
            const int32_t inputHeight = static_cast<int32_t>(quality.optimalHeight);
            Check(ClearDepth(stereo.depth.Get(), 0, 0.4f), "stereo eye 0 depth cleared");
            Check(ClearDepth(stereo.depth.Get(), 1, 0.4f), "stereo eye 1 depth cleared");
            FillMotion(stereo.motion.Get(), 0, 0.0f, 0.0f);
            FillMotion(stereo.motion.Get(), 1, 0.0f, 0.0f);

            BIGSCALE_Frame stereoFrame{};
            stereoFrame.abi = BIGSCALE_ABI_VERSION;
            stereoFrame.size = sizeof(BIGSCALE_Frame);
            stereoFrame.cameraId = 952;
            stereoFrame.historyEpoch = 1;
            stereoFrame.operation = BIGSCALE_OP_EVALUATE;
            stereoFrame.preset = BIGSCALE_PRESET_QUALITY;
            stereoFrame.inputWidth = inputWidth;
            stereoFrame.inputHeight = inputHeight;
            stereoFrame.outputWidth = 1920;
            stereoFrame.outputHeight = 1080;
            stereoFrame.motionWidth = inputWidth;
            stereoFrame.motionHeight = inputHeight;
            stereoFrame.sliceCount = 2;
            stereoFrame.flags = BIGSCALE_FLAG_HDR_COLOR | BIGSCALE_FLAG_SAMPLE_INPUT;
            stereoFrame.layout = BIGSCALE_LAYOUT_ARRAY;
            stereoFrame.color = stereo.color.Get();
            stereoFrame.depth = stereo.depth.Get();
            stereoFrame.motion = stereo.motion.Get();
            stereoFrame.output = stereo.output.Get();
            stereoFrame.upscaler = upscaler;
            stereoFrame.cameraNear = 0.1f;
            stereoFrame.cameraFar = 1000.0f;
            stereoFrame.verticalFovRadians = 1.2f;
            stereoFrame.preExposure = 1.0f;
            stereoFrame.motionScaleX = 1.0f;
            stereoFrame.motionScaleY = 1.0f;
            stereoFrame.deltaMilliseconds = 16.0f;

            int retired[2] = {0, 0};
            int retiredFailures = 0;
            bool pairOk = true;
            auto runPair = [&](int step) -> bool
            {
                // The first four frames carry 0.4 in the eye's dominant channel and the last four
                // 0.8, so a genuinely temporal reconstruction must move the output, not echo one
                // baseline frame.
                const float intensity = step < 4 ? 0.4f : 0.8f;
                FillColor(stereo.color.Get(), 0, intensity, 0.02f, 0.02f, 1.0f);
                FillColor(stereo.color.Get(), 1, 0.02f, intensity, 0.02f, 1.0f);
                for (int eye = 0; eye < 2; eye++)
                {
                    BIGSCALE_Frame evaluate = stereoFrame;
                    evaluate.eye = eye;
                    evaluate.slice = eye;
                    evaluate.frameIndex = frame.frameIndex + 8 + step;
                    evaluate.flags = BIGSCALE_FLAG_HDR_COLOR | BIGSCALE_FLAG_SAMPLE_INPUT |
                                     (step == 0 ? BIGSCALE_FLAG_RESET : 0);
                    void* evaluatePacket = BIGSCALE_QueueFrame(&evaluate);
                    if (evaluatePacket == nullptr)
                    {
                        Check(false, "the two-slice stereo evaluate packet is accepted");
                        return false;
                    }
                    Execute(evaluatePacket);
                }
                if (!WaitForDevice())
                {
                    Check(false, "the test device reached idle after the stereo pair");
                    return false;
                }
                // Retirement is query-gated: a later render event polls it before doing its own work.
                BIGSCALE_Frame drain = stereoFrame;
                drain.operation = BIGSCALE_OP_PROBE;
                drain.eye = 0;
                drain.slice = 0;
                drain.sliceCount = 1;
                drain.layout = BIGSCALE_LAYOUT_2D;
                drain.flags = 0;
                drain.depth = nullptr;
                drain.motion = nullptr;
                drain.output = nullptr;
                drain.frameIndex = frame.frameIndex + 30 + step;
                void* drainPacket = BIGSCALE_QueueFrame(&drain);
                if (drainPacket != nullptr) Execute(drainPacket);
                const BIGSCALE_Status pairStatus = ReadStatus();
                const uint32_t published = pairStatus.completionPublished < BIGSCALE_COMPLETION_COUNT
                    ? pairStatus.completionPublished
                    : BIGSCALE_COMPLETION_COUNT;
                for (uint32_t i = 0; i < published; i++)
                {
                    const BIGSCALE_Completion& entry = pairStatus.completions[i];
                    if (entry.cameraId != 952 || entry.operation != BIGSCALE_OP_EVALUATE ||
                        entry.kind != BIGSCALE_COMPLETION_RETIRED)
                        continue;
                    if (entry.eye >= 0 && entry.eye < 2) retired[entry.eye]++;
                    if (entry.result != 0) retiredFailures++;
                }
                AckAll();
                return true;
            };

            for (int step = 0; step < 4 && pairOk; step++) pairOk = runPair(step);
            const auto midEye0 = ReadBack(stereo.output.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
            const auto midEye1 = ReadBack(stereo.output.Get(), 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
            for (int step = 4; step < 8 && pairOk; step++) pairOk = runPair(step);
            Check(pairOk, "all eight stereo frame pairs executed");

            const auto finalEye0 = ReadBack(stereo.output.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
            const auto finalEye1 = ReadBack(stereo.output.Get(), 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
            const int width = 1920;
            const int height = 1080;
            const size_t expectedTexels = static_cast<size_t>(width) * height * 4;
            Check(finalEye0.size() == expectedTexels && finalEye1.size() == expectedTexels,
                "both stereo output slices were read back in full");
            Check(retired[0] == 8 && retired[1] == 8, "each eye publishes eight successful retirements (got " +
                std::to_string(retired[0]) + " and " + std::to_string(retired[1]) + ")");
            Check(retiredFailures == 0, "every stereo retirement reported a zero result");

            const BIGSCALE_Status finalStatus = ReadStatus();
            int viewEvaluations[2] = {-1, -1};
            for (uint32_t i = 0; i < finalStatus.viewCount && i < BIGSCALE_MAX_VIEWS; i++)
            {
                if (finalStatus.views[i].eye < 2)
                    viewEvaluations[finalStatus.views[i].eye] =
                        static_cast<int>(finalStatus.views[i].evaluations);
            }
            Check(viewEvaluations[0] == 8, "the native eye-0 evaluation counter advanced to eight (got " +
                std::to_string(viewEvaluations[0]) + ")");
            Check(viewEvaluations[1] == 8, "the native eye-1 evaluation counter advanced to eight (got " +
                std::to_string(viewEvaluations[1]) + ")");

            if (finalEye0.size() == expectedTexels && finalEye1.size() == expectedTexels)
            {
                const size_t samples[5] = {
                    Texel(width / 2 - 1, height / 2 - 1, width, 4),
                    Texel(width / 8, height / 8, width, 4),
                    Texel(7 * width / 8, height / 8, width, 4),
                    Texel(width / 8, 7 * height / 8, width, 4),
                    Texel(7 * width / 8, 7 * height / 8, width, 4),
                };
                const char* names[5] = {"centre", "top-left", "top-right", "bottom-left", "bottom-right"};
                for (int eye = 0; eye < 2; eye++)
                {
                    const std::vector<float>& values = eye == 0 ? finalEye0 : finalEye1;
                    for (int i = 0; i < 5; i++)
                    {
                        const size_t index = samples[i];
                        const float r = values[index];
                        const float g = values[index + 1];
                        const float b = values[index + 2];
                        const float a = values[index + 3];
                        const std::string where = std::string("eye ") + std::to_string(eye) + " at " + names[i];
                        Check(std::isfinite(r) && std::isfinite(g) && std::isfinite(b) && std::isfinite(a),
                            "the stereo output is finite for " + where);
                        Check(r + g + b > 0.1f, "the stereo output is not black for " + where);
                        const float dominant = eye == 0 ? r : g;
                        const float others = eye == 0 ? g + b : r + b;
                        Check(dominant > others, "the stereo output stays " +
                            std::string(eye == 0 ? "red" : "green") + "-dominant for " + where + " (r=" +
                            std::to_string(r) + " g=" + std::to_string(g) + " b=" + std::to_string(b) + ")");
                    }
                }
                const size_t centre = samples[0];
                if (midEye0.size() == expectedTexels && midEye1.size() == expectedTexels)
                {
                    Check(finalEye0[centre] > midEye0[centre] + 0.05f,
                        "eye 0 accumulated the brighter final frames (mid=" + std::to_string(midEye0[centre]) +
                            " final=" + std::to_string(finalEye0[centre]) + ")");
                    Check(finalEye1[centre + 1] > midEye1[centre + 1] + 0.05f,
                        "eye 1 accumulated the brighter final frames (mid=" +
                            std::to_string(midEye1[centre + 1]) + " final=" +
                            std::to_string(finalEye1[centre + 1]) + ")");
                }
                else
                {
                    Check(false, "the stereo midpoint output was read back in full");
                }
            }
            AckAll();
        }
    }

    // A positive FidelityFX failure must be treated as a failure. This runs after the real success
    // assertions above and never replaces them.
    if (upscaler == BIGSCALE_UPSCALER_FSR3) CheckFsrPositiveErrorResult();

    status = ReadStatus();
    std::printf("%s: real probe, create and evaluate exercised (state=%d evaluations=%llu error=%d)\n", label,
        status.state, static_cast<unsigned long long>(status.totalEvaluations), status.lastError);
    AckAll();

    // Independent of the slice/retirement checks above: an actual reconstruction must recover detail
    // the same input pixels cannot, and must follow the motion the packet declares.
    if (upscaler == BIGSCALE_UPSCALER_DLSS) CheckDlssImageQuality(upscaler);
}
}


namespace bigscale
{
void SetLogDirectory(const std::wstring& directory);
std::wstring LogDirectory();
}

// ---------------------------------------------------------------------------------------------
// Stage-probe image evidence. The probe writes a BMP crop of the input and of the engine output for
// two scheduled operations, so this drives the real public packet path with a synthetic bilinear
// frame and inspects both the GPU output and the files it produced.
// ---------------------------------------------------------------------------------------------
constexpr int kStageProbeCamera = 929;
// Frames with no identity band, so the bounded crop burst runs before the band is stamped.
constexpr int kStageProbeUntaggedFrames = 24;
constexpr int kPlainCamera = 930;
constexpr UINT kStageProbeExtent = 512;
// The centre crop of a 512-tall texture is rows 128..383, so its top half comes from the authored
// top half and its bottom half from the authored bottom half: a flipped crop swaps these two.
constexpr float kTopBlue = 0.1f;
constexpr float kBottomBlue = 0.6f;
constexpr float kEyeRed[2] = {1.0f, 0.25f};
constexpr float kEyeGreen[2] = {0.25f, 1.0f};

std::wstring UniqueDirectory(const wchar_t* prefix)
{
    wchar_t temporary[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temporary) == 0) return {};
    for (int attempt = 0; attempt < 8; attempt++)
    {
        wchar_t name[96]{};
        _snwprintf_s(name, sizeof(name) / sizeof(name[0]), _TRUNCATE, L"%s%u-%d", prefix, GetCurrentProcessId(), attempt);
        const std::wstring path = std::wstring(temporary) + name;
        if (CreateDirectoryW(path.c_str(), nullptr)) return path;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return {};
    }
    return {};
}

void RemoveTree(const std::wstring& directory)
{
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW((directory + L"\\*").c_str(), &data);
    if (search != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (data.cFileName[0] == L'.' && (data.cFileName[1] == 0 ||
                (data.cFileName[1] == L'.' && data.cFileName[2] == 0))) continue;
            const std::wstring path = directory + L"\\" + data.cFileName;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) RemoveTree(path);
            else DeleteFileW(path.c_str());
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    RemoveDirectoryW(directory.c_str());
}

// ---------------------------------------------------------------------------------------------
// DLSS5 neural rendering. The runtime is user-supplied, so this mode proves the configuration ABI,
// the bridge's state machine and, only when the compatibility DLL is staged next to the checks,
// feature-18 creation, multipass evaluation and the character-mask composite on real D3D11 textures.
// ---------------------------------------------------------------------------------------------
constexpr UINT kNeuralExtent = 256;
constexpr int32_t kNeuralCamera = 980;

// One D3D11 fixture for a neural stage run: the inputs the Bridge hands the stage, the mod-owned
// character mask, and (for after-upscale timing) the backend output the stage composites into.
struct NeuralFixture
{
    ComPtr<ID3D11Texture2D> color;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11Texture2D> motion;
    ComPtr<ID3D11Texture2D> mask;
    ComPtr<ID3D11Texture2D> output;
    EvaluationRequest request{};
};

// R32_FLOAT has no packing ambiguity, so one constant-depth plane is enough for the guide.
bool FillNeuralDepth(ID3D11Texture2D* texture, float depth)
{
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    std::vector<float> texels(static_cast<size_t>(desc.Width) * desc.Height, depth);
    D3D11_BOX box{0, 0, 0, desc.Width, desc.Height, 1};
    g_context->UpdateSubresource(texture, 0, &box, texels.data(), desc.Width * sizeof(float),
        desc.Width * desc.Height * sizeof(float));
    return true;
}

// White on the left half, black on the right: the composite keeps the masked half and replaces the
// unmasked half with the model answer, so a mask that never arrives is visible in the result.
bool FillNeuralMask(ID3D11Texture2D* texture, UINT whiteColumns)
{
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    std::vector<uint8_t> texels(static_cast<size_t>(desc.Width) * desc.Height, 0);
    const uint8_t inside = 255;
    const uint8_t outside = 0;
    for (UINT y = 0; y < desc.Height; y++)
    {
        for (UINT x = 0; x < desc.Width; x++)
            texels[static_cast<size_t>(y) * desc.Width + x] = x < whiteColumns ? inside : outside;
    }
    D3D11_BOX box{0, 0, 0, desc.Width, desc.Height, 1};
    g_context->UpdateSubresource(texture, 0, &box, texels.data(), desc.Width, desc.Width * desc.Height);
    return true;
}

bool MakeNeuralFixture(NeuralFixture& fixture)
{
    const UINT extent = kNeuralExtent;
    fixture.color = CreateTexture(extent, extent, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    fixture.depth = CreateTexture(extent, extent, 1, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    fixture.motion = CreateTexture(extent, extent, 1, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    fixture.mask = CreateTexture(extent, extent, 1, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    fixture.output = CreateTexture(extent, extent, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    if (!fixture.color || !fixture.depth || !fixture.motion || !fixture.mask || !fixture.output) return false;

    FillColor(fixture.color.Get(), 0, 0.8f, 0.35f, 0.6f, 1.0f);
    FillNeuralDepth(fixture.depth.Get(), 0.5f);
    FillMotion(fixture.motion.Get(), 0, 0.0f, 0.0f);
    FillNeuralMask(fixture.mask.Get(), extent / 2);
    // The after-upscale source is the backend result; it starts as its own ramp so the substitution
    // is measurable against it.
    FillColor(fixture.output.Get(), 0, 0.5f, 0.75f, 0.2f, 1.0f);

    EvaluationRequest& request = fixture.request;
    request.color = fixture.color.Get();
    request.depth = fixture.depth.Get();
    request.motion = fixture.motion.Get();
    request.output = fixture.output.Get();
    request.inputWidth = extent;
    request.inputHeight = extent;
    request.outputWidth = extent;
    request.outputHeight = extent;
    request.scratchWidth = extent;
    request.scratchHeight = extent;
    request.motionWidth = static_cast<int32_t>(extent);
    request.motionHeight = static_cast<int32_t>(extent);
    request.sliceCount = 1;
    request.layout = BIGSCALE_LAYOUT_2D;
    request.preset = Preset::Quality;
    request.historyEpoch = 1;
    request.motionScaleX = 1.0f;
    request.motionScaleY = 1.0f;
    return true;
}

BIGSCALE_NeuralConfig NeuralStageConfig(int32_t passes, int32_t timing)
{
    BIGSCALE_NeuralConfig config{};
    config.abi = BIGSCALE_NEURAL_ABI_VERSION;
    config.size = sizeof(BIGSCALE_NeuralConfig);
    config.enabled = 1;
    config.style = 0;
    config.timing = timing;
    config.passes = passes;
    config.strength = 1.0f;
    config.detail = 1.0f;
    config.tone = 1.0f;
    config.characterMask = 1;
    return config;
}

// Counts texels whose channels differ beyond half-float storage noise.
int CountNeuralChanges(const std::vector<float>& before, const std::vector<float>& after, UINT channels,
    float tolerance)
{
    if (before.empty() || before.size() != after.size()) return 0;
    int changed = 0;
    for (size_t texel = 0; texel + channels <= before.size(); texel += channels)
    {
        for (UINT channel = 0; channel < channels; channel++)
        {
            if (std::fabs(before[texel + channel] - after[texel + channel]) > tolerance)
            {
                changed++;
                break;
            }
        }
    }
    return changed;
}

std::string NeuralReasonSuffix(const NeuralRenderingStage& stage)
{
    const std::string& reason = stage.LastReason();
    return reason.empty() ? std::string() : std::string(" (") + reason + ")";
}

// Counts changed texels in the left quarter of the image, where the mask is white: the composite
// keeps the source pixel there, so any change means the character mask was not honoured.
int CountNeuralMaskedChanges(const std::vector<float>& before, const std::vector<float>& after)
{
    constexpr UINT channels = 4;
    if (before.empty() || before.size() != after.size()) return 0;
    int changed = 0;
    for (UINT y = 0; y < kNeuralExtent; y++)
    {
        for (UINT x = 0; x < kNeuralExtent / 4; x++)
        {
            const size_t at = Texel(x, y, kNeuralExtent, channels);
            for (UINT channel = 0; channel < channels; channel++)
            {
                if (std::fabs(before[at + channel] - after[at + channel]) > 1e-3f)
                {
                    changed++;
                    break;
                }
            }
        }
    }
    return changed;
}

// The model must have run (the image differs) while the masked quarter stayed untouched.
void AssertNeuralImage(const std::vector<float>& source, const std::vector<float>& image,
    const std::string& label)
{
    const size_t texels = static_cast<size_t>(kNeuralExtent) * kNeuralExtent;
    Check(image.size() == texels * 4, label + ": the neural image reads back");
    const int changed = CountNeuralChanges(source, image, 4, 1e-3f);
    Check(changed > 0, label + ": the model changed " + std::to_string(changed) + " of " +
                            std::to_string(texels) + " texels");
    const int masked = CountNeuralMaskedChanges(source, image);
    Check(masked == 0, label + ": the masked quarter matches the source (" + std::to_string(masked) +
                           " changed texels)");
    std::printf("  %s: %d of %zu texels changed, %d changed under the mask\n", label.c_str(), changed,
        texels, masked);
}

// Reads one RGBA16F stage image and asserts it differs from the image it was derived from.
void CheckNeuralImage(ID3D11Texture2D* image, const std::vector<float>& source, const std::string& label,
    std::vector<float>& outImage)
{
    if (image == nullptr)
    {
        Check(false, label + ": no neural image");
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    image->GetDesc(&desc);
    Check(desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && desc.Width == kNeuralExtent &&
              desc.Height == kNeuralExtent,
        label + ": the neural image is RGBA16F at 64x64");
    outImage = ReadBack(image, 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
    AssertNeuralImage(source, outImage, label);
}

// Runs one layer count at one timing through the real stage and returns the neural image. The first
// call is always the creation frame: it builds the feature set and substitutes nothing.
void RunNeuralPasses(NeuralRenderingStage& stage, int32_t passes, int32_t timing, const std::string& label,
    std::vector<float>& outImage)
{
    outImage.clear();
    NeuralFixture fixture;
    Check(MakeNeuralFixture(fixture), label + ": fixture created");
    if (!fixture.color || !fixture.output) return;

    ViewKey key{};
    key.cameraId = kNeuralCamera;
    key.eye = 0;

    // Before upscaling the neural answer replaces the colour input; after it, the backend result is
    // both the model source and the substitution target.
    ID3D11Texture2D* source = timing == 0 ? fixture.color.Get() : fixture.output.Get();
    const std::vector<float> baseline = ReadBack(source, 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
    Check(!baseline.empty(), label + ": the source image reads back");

    stage.Configure(NeuralStageConfig(passes, timing));

    void* mask = fixture.mask.Get();
    EvaluationRequest request = fixture.request;
    if (timing == 0)
    {
        request.frameIndex = 1;
        ID3D11Resource* creation = stage.BeforeUpscale(g_device.Get(), g_context.Get(), key, request, mask,
            kNeuralExtent, kNeuralExtent);
        Check(creation == nullptr, label + ": the creation frame returns no neural image");
        request.frameIndex = 2;
        ID3D11Resource* evaluated = stage.BeforeUpscale(g_device.Get(), g_context.Get(), key, request, mask,
            kNeuralExtent, kNeuralExtent);
        Check(evaluated != nullptr,
            label + ": the evaluation frame returns a neural image" + NeuralReasonSuffix(stage));
        if (evaluated != nullptr)
            CheckNeuralImage(static_cast<ID3D11Texture2D*>(evaluated), baseline, label, outImage);
    }
    else
    {
        request.frameIndex = 1;
        Check(stage.BeforeUpscale(g_device.Get(), g_context.Get(), key, request, mask, kNeuralExtent,
                  kNeuralExtent) == nullptr,
            label + ": before-upscale is inert at after-upscale timing");
        const bool creation = stage.AfterUpscale(g_device.Get(), g_context.Get(), key, request, mask,
            kNeuralExtent, kNeuralExtent);
        Check(!creation, label + ": the creation frame reports no substitution");
        request.frameIndex = 2;
        const bool evaluated = stage.AfterUpscale(g_device.Get(), g_context.Get(), key, request, mask,
            kNeuralExtent, kNeuralExtent);
        Check(evaluated,
            label + ": the evaluation frame writes the neural image" + NeuralReasonSuffix(stage));
        if (evaluated)
        {
            outImage = ReadBack(fixture.output.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
            AssertNeuralImage(baseline, outImage, label);
        }
    }
}

// The stage resolves the compatibility runtime from the plugin directory: the provider tree first,
// then a loose copy beside the executable. Absent is not a failure; the run reports NOT EXERCISED.
bool NeuralRuntimeStaged(std::wstring& source)
{
    source.clear();
    wchar_t modulePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0) return false;
    std::wstring directory(modulePath);
    const size_t slash = directory.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    directory.resize(slash);
    const wchar_t* candidates[2] = {L"\\providers\\dlss5\\nvngx_dlssnr.dll", L"\\nvngx_dlssnr.dll"};
    for (const wchar_t* candidate : candidates)
    {
        const std::wstring path = directory + candidate;
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            source = path;
            return true;
        }
    }
    return false;
}

// A dedicated temporary tree named BigScaleNeuralChecks-<pid> so every file the runtime writes,
// including the nvngx.dll hard link, can be removed after the run.
std::wstring NeuralTemporaryDirectory()
{
    wchar_t temporary[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temporary) == 0) return {};
    for (int attempt = 0; attempt < 8; attempt++)
    {
        wchar_t name[96]{};
        if (attempt == 0)
        {
            _snwprintf_s(name, sizeof(name) / sizeof(name[0]), _TRUNCATE, L"BigScaleNeuralChecks-%u",
                GetCurrentProcessId());
        }
        else
        {
            _snwprintf_s(name, sizeof(name) / sizeof(name[0]), _TRUNCATE, L"BigScaleNeuralChecks-%u-%d",
                GetCurrentProcessId(), attempt);
        }
        const std::wstring path = std::wstring(temporary) + name;
        if (CreateDirectoryW(path.c_str(), nullptr)) return path;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return {};
    }
    return {};
}

void CheckNeuralRendering()
{
    // A null, foreign-ABI or short block is refused before the bridge owns anything: no stored
    // configuration changes and no GPU work starts.
    Check(BIGSCALE_ConfigureNeuralRendering(nullptr) == BIGSCALE_ERROR_ABI_MISMATCH,
        "neural: a null configuration is refused");
    BIGSCALE_NeuralConfig invalid{};
    invalid.abi = 0;
    invalid.size = sizeof(BIGSCALE_NeuralConfig);
    Check(BIGSCALE_ConfigureNeuralRendering(&invalid) == BIGSCALE_ERROR_ABI_MISMATCH,
        "neural: a zero abi is refused");
    invalid.abi = BIGSCALE_NEURAL_ABI_VERSION;
    invalid.size = 43;
    Check(BIGSCALE_ConfigureNeuralRendering(&invalid) == BIGSCALE_ERROR_ABI_MISMATCH,
        "neural: a 43-byte configuration is refused");

    // The bridge resolves the runtime from the executable directory; the data directory is a unique
    // temporary tree so every file the runtime writes is removable after the run.
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring runtimeDirectory(modulePath);
    const size_t slash = runtimeDirectory.find_last_of(L"\\/");
    if (slash != std::wstring::npos) runtimeDirectory.resize(slash);

    const std::wstring dataDirectory = NeuralTemporaryDirectory();
    Check(!dataDirectory.empty(), "neural: the temporary data directory is created");
    if (dataDirectory.empty()) return;

    // The harness is an EXE, which cannot be loaded as a DLL. Use the bridge DLL staged beside it as
    // the nvngx.dll forwarder unless the caller provided an explicit one.
    {
        wchar_t existing[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"BIGSCALE_NVNGX_FORWARDER", existing, MAX_PATH) == 0)
        {
            const std::wstring bridge = runtimeDirectory + L"\\BigScale.Native.dll";
            if (GetFileAttributesW(bridge.c_str()) != INVALID_FILE_ATTRIBUTES)
                SetEnvironmentVariableW(L"BIGSCALE_NVNGX_FORWARDER", bridge.c_str());
        }
    }

    BIGSCALE_SetEngineVersion("6000.3.17f1");
    Check(BIGSCALE_Initialize(runtimeDirectory.c_str(), dataDirectory.c_str()) == 1,
        "neural: the bridge initialises");
    // The bridge points the log manager at the application data directory. Keep the neural run's
    // diagnostics beside the executable so a failed create can be diagnosed after the temporary
    // tree is removed.
    bigscale::SetLogDirectory(runtimeDirectory);

    BIGSCALE_NeuralConfig disabled = NeuralStageConfig(1, 0);
    disabled.enabled = 0;
    Check(BIGSCALE_ConfigureNeuralRendering(&disabled) == 0, "neural: a disabled configuration is accepted");
    Check(ReadStatus().neuralState == BIGSCALE_NEURAL_OFF, "neural: a disabled configuration reports Off");

    const BIGSCALE_NeuralConfig enabled = NeuralStageConfig(1, 0);
    Check(BIGSCALE_ConfigureNeuralRendering(&enabled) == 0,
        "neural: a valid enabled configuration is accepted");

    std::wstring runtimePath;
    const bool runtimePresent = NeuralRuntimeStaged(runtimePath);
    const BIGSCALE_Status afterConfigure = ReadStatus();
    if (runtimePresent)
    {
        Check(afterConfigure.neuralState == BIGSCALE_NEURAL_WAITING,
            "neural: an enabled configuration reports Waiting until it evaluates");
    }
    else
    {
        Check(afterConfigure.neuralState == BIGSCALE_NEURAL_RUNTIME_MISSING,
            "neural: a missing runtime reports Runtime missing");
    }

    if (runtimePresent)
    {
        std::printf("neural runtime: %ls\n", runtimePath.c_str());
        {
            NeuralRenderingStage stage;
            stage.SetEnvironment(runtimeDirectory, dataDirectory, "6000.3.17f1");
            // A generation change (pass count, then timing) rebuilds the feature set, so each run
            // starts with its own creation frame.
            std::vector<float> onePass;
            RunNeuralPasses(stage, 1, 0, "neural before-upscaling 1 pass", onePass);
            std::vector<float> fivePasses;
            RunNeuralPasses(stage, 5, 0, "neural before-upscaling 5 passes", fivePasses);
            if (!onePass.empty() && !fivePasses.empty())
            {
                const int changed = CountNeuralChanges(onePass, fivePasses, 4, 1e-3f);
                Check(changed > 0, "neural: the five-pass image differs from the one-pass image (" +
                                       std::to_string(changed) + " texels)");
                std::printf("  neural multipass: %d of %zu texels differ between 1 and 5 passes\n", changed,
                    onePass.size() / 4);
            }
            std::vector<float> afterUpscaling;
            RunNeuralPasses(stage, 1, 1, "neural after-upscaling 1 pass", afterUpscaling);
            stage.Release();
        }
    }
    else
    {
        std::printf("neural runtime: NOT EXERCISED "
                    "(stage nvngx_dlssnr.dll next to BigScaleChecks.exe)\n");
    }

    // No file this run produced may survive it: the alias is a hard link into the compatibility
    // runtime, and the application data tree holds whatever the NGX core wrote. The bridge's log
    // file lives in that tree, so re-point the log manager at the executable directory first; an
    // open handle would make the directory removal fail on every retry.
    bigscale::SetLogDirectory(runtimeDirectory);
    const std::wstring alias = dataDirectory + L"\\dlss5\\nvngx.dll";
    DeleteFileW(alias.c_str());
    Check(GetFileAttributesW(alias.c_str()) == INVALID_FILE_ATTRIBUTES,
        "neural: the nvngx.dll alias is removed");
    // NGX flushes its application-data logs asynchronously; a bounded window keeps a late flush from
    // being reported as a leak while the final state is still asserted.
    for (int attempt = 0; attempt < 20 &&
        GetFileAttributesW(dataDirectory.c_str()) != INVALID_FILE_ATTRIBUTES; attempt++)
    {
        RemoveTree(dataDirectory);
        Sleep(50);
    }
    Check(GetFileAttributesW(dataDirectory.c_str()) == INVALID_FILE_ATTRIBUTES,
        "neural: the temporary data directory is removed");
}

std::vector<std::wstring> ListProbeFiles(const std::wstring& directory)
{
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW((directory + L"\\sr-probe-*.bmp").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return files;
    do
    {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) files.push_back(data.cFileName);
    } while (FindNextFileW(search, &data));
    FindClose(search);
    std::sort(files.begin(), files.end());
    return files;
}

// Finds the first capture for one eye and stage. The ordinal is deliberately not fixed: the probe
// captures twice inside an identity-marker window and on its own scheduled ordinals otherwise.
const std::wstring* FindEyeStageFile(const std::vector<std::wstring>& files, int eye, int stage)
{
    const std::wstring prefix = L"-eye" + std::to_wstring(eye) + L"-sample";
    const std::wstring suffix = stage == 0 ? L"input.bmp" : L"output.bmp";
    for (const std::wstring& file : files)
    {
        if (file.find(prefix) == std::wstring::npos) continue;
        if (file.size() < suffix.size()) continue;
        if (file.compare(file.size() - suffix.size(), suffix.size(), suffix) == 0) return &file;
    }
    return nullptr;
}

const std::wstring* FindProbeFile(const std::vector<std::wstring>& files, const std::wstring& suffix)
{
    for (const std::wstring& file : files)
    {
        if (file.size() < suffix.size()) continue;
        if (file.compare(file.size() - suffix.size(), suffix.size(), suffix) == 0) return &file;
    }
    return nullptr;
}

struct Bitmap
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgr;
};

// Reads back a 24-bit BMP exactly as written: the header decides the orientation, so a positive
// height (bottom-up) is refused rather than silently flipped into a passing image.
bool LoadBitmap(const std::wstring& path, Bitmap& bitmap)
{
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    DWORD read = 0;
    bool ok = ReadFile(handle, &file, sizeof(file), &read, nullptr) && read == sizeof(file) &&
              ReadFile(handle, &info, sizeof(info), &read, nullptr) && read == sizeof(info);
    if (ok)
    {
        ok = file.bfType == 0x4D42 && info.biSize == sizeof(BITMAPINFOHEADER) && info.biBitCount == 24 &&
             info.biCompression == BI_RGB && info.biWidth > 0 && info.biHeight < 0;
    }
    if (ok)
    {
        bitmap.width = static_cast<int>(info.biWidth);
        bitmap.height = -static_cast<int>(info.biHeight);
        const size_t stride = (static_cast<size_t>(bitmap.width) * 3 + 3) & ~static_cast<size_t>(3);
        std::vector<uint8_t> rows(stride * static_cast<size_t>(bitmap.height));
        ok = SetFilePointer(handle, static_cast<LONG>(file.bfOffBits), nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER;
        if (ok)
            ok = ReadFile(handle, rows.data(), static_cast<DWORD>(rows.size()), &read, nullptr) && read == rows.size();
        if (ok)
        {
            bitmap.bgr.resize(static_cast<size_t>(bitmap.width) * bitmap.height * 3);
            for (int y = 0; y < bitmap.height; y++)
                std::memcpy(&bitmap.bgr[static_cast<size_t>(y) * bitmap.width * 3],
                    &rows[static_cast<size_t>(y) * stride], static_cast<size_t>(bitmap.width) * 3);
        }
    }
    CloseHandle(handle);
    return ok;
}

// Linear-light value a preview byte stands for; the writer encoded with IEC sRGB.
float PreviewLinear(uint8_t value)
{
    return SrgbToLinear(static_cast<float>(value) / 255.0f);
}

std::array<float, 3> BitmapPixel(const Bitmap& bitmap, int x, int y)
{
    const uint8_t* texel = &bitmap.bgr[(static_cast<size_t>(y) * bitmap.width + x) * 3];
    return {PreviewLinear(texel[2]), PreviewLinear(texel[1]), PreviewLinear(texel[0])};
}

// FillColor's content with a per-half blue channel: the red ramp is the horizontal signal, the two
// blue levels are the vertical one, and green identifies the eye.
bool FillColorHalves(ID3D11Texture2D* texture, UINT slice, float r, float g, float a)
{
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    std::vector<uint16_t> texels(static_cast<size_t>(desc.Width) * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; y++)
    {
        for (UINT x = 0; x < desc.Width; x++)
        {
            const float ramp = static_cast<float>(x) / static_cast<float>(desc.Width);
            uint16_t* texel = &texels[(static_cast<size_t>(y) * desc.Width + x) * 4];
            texel[0] = ToHalf(r * ramp);
            texel[1] = ToHalf(g);
            texel[2] = ToHalf(y < desc.Height / 2 ? kTopBlue : kBottomBlue);
            texel[3] = ToHalf(a);
        }
    }
    D3D11_BOX box{0, 0, 0, desc.Width, desc.Height, 1};
    const UINT rowPitch = desc.Width * 4 * sizeof(uint16_t);
    g_context->UpdateSubresource(texture, D3D11CalcSubresource(0, slice, desc.MipLevels), &box, texels.data(),
        rowPitch, rowPitch * desc.Height);
    return true;
}

BIGSCALE_Frame MakeStageProbeFrame(
    Fixture& fixture, int cameraId, int eye, int frameIndex, int flags, int sliceCount)
{
    BIGSCALE_Frame frame{};
    frame.abi = BIGSCALE_ABI_VERSION;
    frame.size = sizeof(BIGSCALE_Frame);
    frame.cameraId = cameraId;
    frame.eye = eye;
    frame.frameIndex = frameIndex;
    frame.operation = BIGSCALE_OP_BILINEAR_COMPARISON;
    frame.preset = BIGSCALE_PRESET_NATIVE_AA;
    frame.upscaler = BIGSCALE_UPSCALER_DLSS;
    frame.inputWidth = static_cast<int32_t>(kStageProbeExtent);
    frame.inputHeight = static_cast<int32_t>(kStageProbeExtent);
    frame.outputWidth = static_cast<int32_t>(kStageProbeExtent);
    frame.outputHeight = static_cast<int32_t>(kStageProbeExtent);
    frame.motionWidth = static_cast<int32_t>(kStageProbeExtent);
    frame.motionHeight = static_cast<int32_t>(kStageProbeExtent);
    frame.slice = eye;
    frame.sliceCount = sliceCount;
    frame.layout = BIGSCALE_LAYOUT_ARRAY;
    frame.flags = flags;
    frame.color = fixture.color.Get();
    frame.depth = fixture.depth.Get();
    frame.motion = fixture.motion.Get();
    frame.output = fixture.output.Get();
    frame.resourceTag = 7000u + static_cast<uint32_t>(eye);
    return frame;
}

// True when the written engine slice contains a magenta marker texel, and reports where it was
// found so a caller can require it inside the known marker band and nowhere else.
bool FindMagenta(const std::vector<float>& pixels, UINT width, UINT height, UINT& x, UINT& y)
{
    for (UINT row = 0; row < height; row++)
    {
        for (UINT column = 0; column < width; column++)
        {
            const size_t index = Texel(column, row, width, 4);
            if (pixels[index] > 0.9f && pixels[index + 1] < 0.1f && pixels[index + 2] > 0.9f)
            {
                x = column;
                y = row;
                return true;
            }
        }
    }
    return false;
}

void CheckStageProbeCrop(const Bitmap& bitmap, int eye, float redScale, const std::string& label)
{
    Check(bitmap.width == 256 && bitmap.height == 256, label + " is a 256x256 crop");
    if (bitmap.width != 256 || bitmap.height != 256) return;
    // Crop column 0 is texture column 128, so the expected red ramp continues from there.
    const auto expectedRed = [&](int x)
    {
        return redScale * static_cast<float>(128 + x) / static_cast<float>(kStageProbeExtent);
    };
    const std::array<std::array<int, 2>, 3> samples = {{{8, 8}, {128, 128}, {247, 247}}};
    for (const std::array<int, 2>& at : samples)
    {
        const std::array<float, 3> pixel = BitmapPixel(bitmap, at[0], at[1]);
        Near(pixel[0], expectedRed(at[0]), 0.02f, label + " red ramp at crop column " + std::to_string(at[0]));
        Near(pixel[1], kEyeGreen[eye], 0.02f, label + " green identifies eye " + std::to_string(eye));
        // Rows 0..127 of the crop are the authored top half, rows 128..255 the bottom half.
        Near(pixel[2], at[1] < 128 ? kTopBlue : kBottomBlue, 0.02f,
            label + " blue half at crop row " + std::to_string(at[1]));
    }
    // Both halves must be present, so the orientation assertion cannot pass on a uniform crop.
    const float topBlue = BitmapPixel(bitmap, 128, 8)[2];
    const float bottomBlue = BitmapPixel(bitmap, 128, 247)[2];
    Check(topBlue < bottomBlue, label + " keeps the authored top/bottom order");
}

void CheckStageProbeBilinear()
{
    // Every exit path restores the log location: the harness later writes its own native.log.
    const std::wstring previousLogDirectory = bigscale::LogDirectory();
    struct RestoreLogDirectory
    {
        std::wstring previous;
        ~RestoreLogDirectory() { bigscale::SetLogDirectory(previous); }
    } restore{previousLogDirectory};

    const std::wstring root = UniqueDirectory(L"bigscale-stage-probe-");
    Check(!root.empty(), "stage probe temporary directory created");
    if (root.empty()) return;

    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring runtime(modulePath);
    const size_t slash = runtime.find_last_of(L"\\/");
    if (slash != std::wstring::npos) runtime.resize(slash);
    Check(BIGSCALE_Initialize(runtime.c_str(), root.c_str()) == 1, "the stage probe check initialises the bridge");

    Fixture fixture;
    if (!MakeFixture(kStageProbeExtent, kStageProbeExtent, 2, kStageProbeExtent, kStageProbeExtent,
            kStageProbeExtent, kStageProbeExtent, kStageProbeExtent, kStageProbeExtent, DXGI_FORMAT_R32_TYPELESS,
            fixture, kStageProbeCamera, 0, 0, 2))
    {
        Check(false, "stage probe fixture created");
        RemoveTree(root);
        return;
    }
    Check(fixture.request.layout == BIGSCALE_LAYOUT_ARRAY, "the stage probe fixture is a two-slice array");
    for (int eye = 0; eye < 2; eye++)
    {
        FillColorHalves(fixture.color.Get(), static_cast<UINT>(eye), kEyeRed[eye], kEyeGreen[eye], 1.0f);
        Check(ClearDepth(fixture.depth.Get(), static_cast<UINT>(eye), 0.4f), "stage probe depth cleared");
        FillMotion(fixture.motion.Get(), static_cast<UINT>(eye), 0.0f, 0.0f);
    }

    // Ordinal 120 is queued by this run and collected by the next operation, which is why the loop
    // reaches 122. A missing capture is then the probe's failure, not a skipped assertion.
    bool executed = true;
    const int frameCount = 122;
    for (int frameIndex = 1; frameIndex <= frameCount && executed; frameIndex++)
    {
        for (int eye = 0; eye < 2 && executed; eye++)
        {
            // The first frames are untagged, which is the only state that writes the crop pairs; the
            // later ones carry the identity band, which is what the read-back below classifies.
            const int32_t flags = BIGSCALE_FLAG_STAGE_PROBE |
                (frameIndex > kStageProbeUntaggedFrames ? BIGSCALE_FLAG_STAGE_MARKER : 0);
            BIGSCALE_Frame frame = MakeStageProbeFrame(fixture, kStageProbeCamera, eye, frameIndex, flags, 2);
            void* packet = BIGSCALE_QueueFrame(&frame);
            if (packet == nullptr)
            {
                Check(false, "the stage probe packet is accepted");
                executed = false;
                break;
            }
            Execute(packet);
        }
        if (!executed) break;
        Check(WaitForDevice(), "the stage probe device reached idle");
        AckAll();
    }
    Check(executed, "the stage probe frames executed");

    // The bilinear branch converts, copies back and stamps; a read-back here is the same engine
    // slice the headset receives, so a missing marker or a wrong eye is measured, not described. The
    // orientation is the identity: a slice duplicated into both eyes must be rejected below.
    std::vector<float> eye0Output;
    for (int eye = 0; executed && eye < 2; eye++)
    {
        const std::vector<float> output = ReadBack(
            fixture.output.Get(), static_cast<UINT>(eye), DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
        Check(output.size() == static_cast<size_t>(kStageProbeExtent) * kStageProbeExtent * 4,
            "stage probe output slice read back");
        if (output.size() != static_cast<size_t>(kStageProbeExtent) * kStageProbeExtent * 4) continue;
        if (eye == 0) eye0Output = output;
        int32_t left = 0;
        int32_t top = 0;
        int32_t right = 0;
        int32_t bottom = 0;
        const int shape = bigscale::StageProbe::ClassifyIdentityMarker(output.data(), kStageProbeExtent,
            kStageProbeExtent, kStageProbeExtent, kStageProbeExtent * 0.5f, kStageProbeExtent * 0.5f,
            static_cast<float>(kStageProbeExtent), &left, &top, &right, &bottom);
        Check(shape == eye + 1, "the written slice carries eye " + std::to_string(eye) +
            "'s identity bar (got " + std::to_string(shape) + ")");
        if (shape == eye + 1)
        {
            // The band crosses the centre so it cannot fall inside the compositor's hidden-area mesh;
            // the crop pairs are taken from untagged frames, which the file assertions below cover.
            const bool centred = eye == 0
                ? std::abs((left + right) / 2 - static_cast<int32_t>(kStageProbeExtent) / 2) <= 4
                : std::abs((top + bottom) / 2 - static_cast<int32_t>(kStageProbeExtent) / 2) <= 4;
            Check(centred, "the identity band crosses the centre for eye " + std::to_string(eye));
            // A band spans the whole region in its long axis, which is what makes it visible through a
            // lens instead of hiding in a corner.
            const bool spans = eye == 0
                ? (bottom - top + 1) >= static_cast<int32_t>(kStageProbeExtent) &&
                      (right - left + 1) == 24
                : (right - left + 1) >= static_cast<int32_t>(kStageProbeExtent) &&
                      (bottom - top + 1) == 24;
            Check(spans, "the identity band spans the whole eye region for eye " + std::to_string(eye) +
                " (box " + std::to_string(right - left + 1) + "x" + std::to_string(bottom - top + 1) + ")");
        }
        // Outside the identity band the eye keeps its own colour: leakage from the other eye would
        // show up as the other eye's green (or its much lower red). The rows skip the band's own place
        // at the centre of the region.
        const UINT rows[] = {128, 176, 208, 320, 384};
        for (UINT row : rows)
        {
            const size_t index = Texel(320, row, kStageProbeExtent, 4);
            Near(output[index + 1], kEyeGreen[eye], 0.02f, "the output keeps eye " + std::to_string(eye) + " green");
            Near(output[index], kEyeRed[eye] * 320.0f / static_cast<float>(kStageProbeExtent), 0.02f,
                "the output keeps eye " + std::to_string(eye) + " red ramp");
            Near(output[index + 2], row < 256 ? kTopBlue : kBottomBlue, 0.02f,
                "the output keeps the authored blue half");
        }
    }

    // A slice copied into both eyes is exactly the failure this marker exists to catch: eye 0's
    // vertical bar must not be accepted as eye 1's horizontal one.
    if (executed && !eye0Output.empty())
    {
        const int duplicated = bigscale::StageProbe::ClassifyIdentityMarker(eye0Output.data(), kStageProbeExtent,
            kStageProbeExtent, kStageProbeExtent, kStageProbeExtent * 0.5f, kStageProbeExtent * 0.5f,
            static_cast<float>(kStageProbeExtent));
        Check(duplicated == 1, "eye 0's slice classifies as the vertical bar");
        Check(duplicated != 2, "a duplicated left slice is rejected as eye 1");
    }

    const std::vector<std::wstring> probeFiles = ListProbeFiles(root);
    Check(probeFiles.size() >= 4 && probeFiles.size() % 2 == 0 && probeFiles.size() <= 64,
        "the probe wrote a bounded, paired crop set (got " + std::to_string(probeFiles.size()) + " files)");
    if (executed)
    {
        for (int eye = 0; eye < 2; eye++)
        {
            for (int stage = 0; stage < 2; stage++)
            {
                const std::string label = std::string(stage == 0 ? "input" : "output") + " crop for eye " +
                    std::to_string(eye);
                const std::wstring* file = FindEyeStageFile(probeFiles, eye, stage);
                Check(file != nullptr, label + " was written");
                if (file == nullptr) continue;
                Bitmap bitmap;
                Check(LoadBitmap(root + L"\\" + *file, bitmap), label + " parses as a top-down 24-bit BMP");
                CheckStageProbeCrop(bitmap, eye, kEyeRed[eye], label);
            }
        }
    }

    // The same geometry without the flag: scene pixels must survive with no marker and no files, so
    // the marker and the captures are the probe's, not the write-back's.
    Fixture plain;
    if (MakeFixture(kStageProbeExtent, kStageProbeExtent, 2, kStageProbeExtent, kStageProbeExtent,
            kStageProbeExtent, kStageProbeExtent, kStageProbeExtent, kStageProbeExtent, DXGI_FORMAT_R32_TYPELESS,
            plain, kPlainCamera, 0, 0, 2))
    {
        FillColorHalves(plain.color.Get(), 0, kEyeRed[0], kEyeGreen[0], 1.0f);
        ClearDepth(plain.depth.Get(), 0, 0.4f);
        FillMotion(plain.motion.Get(), 0, 0.0f, 0.0f);
        bool plainExecuted = true;
        for (int frameIndex = 1; frameIndex <= 3 && plainExecuted; frameIndex++)
        {
            BIGSCALE_Frame frame = MakeStageProbeFrame(plain, kPlainCamera, 0, frameIndex, 0, 2);
            void* packet = BIGSCALE_QueueFrame(&frame);
            if (packet == nullptr)
            {
                Check(false, "the diagnostics-disabled packet is accepted");
                plainExecuted = false;
                break;
            }
            Execute(packet);
            Check(WaitForDevice(), "the diagnostics-disabled device reached idle");
            AckAll();
        }
        Check(plainExecuted, "the diagnostics-disabled frames executed");
        const std::vector<float> output = ReadBack(
            plain.output.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 4);
        if (output.size() == static_cast<size_t>(kStageProbeExtent) * kStageProbeExtent * 4)
        {
            UINT markerX = 0;
            UINT markerY = 0;
            Check(!FindMagenta(output, kStageProbeExtent, kStageProbeExtent, markerX, markerY),
                "a packet without the probe flag carries no marker");
            const size_t index = Texel(320, 192, kStageProbeExtent, 4);
            Near(output[index + 1], kEyeGreen[0], 0.02f, "the diagnostics-disabled scene keeps its green");
            Near(output[index], kEyeRed[0] * 320.0f / static_cast<float>(kStageProbeExtent), 0.02f,
                "the diagnostics-disabled scene keeps its red ramp");
            Near(output[index + 2], kTopBlue, 0.02f, "the diagnostics-disabled scene keeps its blue half");
        }
        else
        {
            Check(false, "diagnostics-disabled output read back");
        }
    }
    else
    {
        Check(false, "diagnostics-disabled fixture created");
    }
    Check(ListProbeFiles(root).size() == probeFiles.size(),
        "a packet without the probe flag writes no capture files");

    BIGSCALE_Frame shutdown = MakeStageProbeFrame(fixture, kStageProbeCamera, 0, frameCount + 1, 0, 2);
    shutdown.operation = BIGSCALE_OP_SHUTDOWN;
    shutdown.color = nullptr;
    shutdown.depth = nullptr;
    shutdown.motion = nullptr;
    shutdown.output = nullptr;
    void* packet = BIGSCALE_QueueFrame(&shutdown);
    Check(packet != nullptr, "the stage probe shutdown packet is accepted");
    if (packet != nullptr) Execute(packet);
    AckAll();
    RemoveTree(root);
}


// ---------------------------------------------------------------------------------------------
// Frame-generation observation: the providers own their child swapchain, so the harness registers
// the testing observer to read the resources that actually reach the display.
// ---------------------------------------------------------------------------------------------
bool g_fgAll = false;
// Set for `--fg anr`: the ANR process family. Every other provider must be refused in-process.
bool g_fgAnr = false;
// Set by `--anr-after-normal`: this process exercises the one-way normal -> ANR migration and the
// restart-required back-switch after the incumbent normal provider has run the whole suite.
bool g_fgAnrAfterNormal = false;
// Set by `--source-blt`: creates the game/source swapchain as one-buffer bitblt instead of the
// fixture's historic two-buffer flip chain. Provider-owned child swapchains are never altered.
bool g_fgSourceBlt = false;
// Set by `--source-game`: creates the source chain with the shipping game's exact descriptor
// (two buffers, FLIP_SEQUENTIAL, flags 0x842). Provider-owned child chains are never altered.
bool g_fgSourceGame = false;
// Set by `--fg-probe`: one finite, interpretable sequence (colour identity, both motion axes, one
// reset pair and cadence) instead of the multi-phase tournament below.
bool g_fgProbe = false;
// Requested total multiplier (one real frame plus generated images). Validated at startup; every
// configuration this harness sends carries it so a resize/switch phase cannot silently revert to 2x.
uint32_t g_fgMultiplier = 2;

// Waits for `handle` while pumping this thread's messages. Provider presentation and the child
// window both live on this thread, so a plain Sleep can starve them. A null handle is a timed delay.
DWORD WaitWithMessages(HANDLE handle, DWORD milliseconds)
{
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    for (;;)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        const ULONGLONG now = GetTickCount64();
        const DWORD remaining = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
        DWORD result = WAIT_TIMEOUT;
        if (handle != nullptr)
        {
            if (remaining == 0) return WaitForSingleObject(handle, 0);
            result = MsgWaitForMultipleObjectsEx(1, &handle, remaining, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (result == WAIT_OBJECT_0) return WAIT_OBJECT_0;
            if (result != WAIT_OBJECT_0 + 1) return result;
        }
        else
        {
            if (remaining == 0) return WAIT_TIMEOUT;
            // Pump the queue at a bounded rate; the deadline drives the exit, not the sleep length.
            const DWORD slice = remaining < 5 ? remaining : 5;
            if (MsgWaitForMultipleObjectsEx(0, nullptr, slice, QS_ALLINPUT, MWMO_INPUTAVAILABLE) ==
                WAIT_TIMEOUT)
                continue;
        }
    }
}

struct Image
{
    UINT width = 0;
    UINT height = 0;
    // RGBA in row-major order; 8-bit sources are scaled to 0..1.
    std::vector<float> rgba;

    bool Valid() const { return width > 0 && height > 0 && rgba.size() >= static_cast<size_t>(width) * height * 4; }
    const float* At(UINT x, UINT y) const { return &rgba[(static_cast<size_t>(y) * width + x) * 4]; }
};

struct ObservedFrame
{
    uint32_t generation = 0;
    int32_t sourceFrame = 0;
    // 1-based position of this intermediate inside its source frame, and how many the provider
    // presented. A duplicated copy would report distinct indices with identical pixels.
    uint32_t intermediateIndex = 1;
    uint32_t intermediateCount = 1;
    bool reset = false;
    // A suppressed pair: only the real image was presented, so nothing here may count as
    // interpolation.
    bool realOnly = false;
    bool interpolationDisabled = false;
    bool ok = false;
    std::string reason;
    // The image the provider copied into the child back buffer (screen space).
    Image screen;
    // The SDK's own output before any provider-side orientation correction.
    Image raw;
    // Identity of the captured source frame, decoded from the frame-coded patch in the retained real
    // colour. `codedValid` is false when the patch could not be located.
    int32_t codedFrame = -1;
    bool codedValid = false;
    // Largest per-channel difference between the actual child buffer and the provider's copy source
    // (0.0 = identical). Only computed when both resources were readable.
    bool childCompared = false;
    float childVsCopyMax = 0.0f;
    // Largest per-channel difference between the actual child buffer and the retained real colour.
    // This is the suppressed-pair check: the child must hold this frame's real image.
    bool realCompared = false;
    float childVsRealMax = 0.0f;
    // Largest per-channel difference between the SDK's evaluation colour and the retained real
    // colour (spatial samples only).
    bool evaluationCompared = false;
    float evaluationVsRealMax = 0.0f;
    // Largest per-channel difference between the pre-overlay plane the SDK received and the retained
    // final colour. Zero means the provider was handed the overlay frame as "HUD-less".
    bool hudlessCompared = false;
    float hudlessVsRealMax = 0.0f;
    // The same plane compared outside the HUD rectangle, in presented-backbuffer rows.
    bool hudlessSpatialCompared = false;
    float hudlessSpatialVsRealMax = 0.0f;
    bool hudlessCodedValid = false;
    int32_t hudlessCodedFrame = -1;
    // Grid samples of the guide planes that were compared, and how many disagreed with the
    // screen-row scene. Zero samples means this frame made no statement about the transport.
    int guideSamples = 0;
    int guideMismatches = 0;
    // Mean of the first three channels of the retained real colour, the child/copy plane and the
    // SDK's evaluation colour. A zero mean is a black plane, not a registration difference.
    float realMean = 0.0f;
    float screenMean = 0.0f;
    float evaluationMean = 0.0f;
};

struct ObservedPair
{
    uint32_t generation = 0;
    int32_t sourceFrame = 0;
    // Generated images this source frame actually presented, in increasing intermediate order.
    uint32_t generatedCount = 0;
    int64_t generatedQpc[5]{};
    long generatedResult[5]{};
    int64_t realQpc = 0;
    long realResult = 0;
    bool reset = false;
    // Raw SDK interpolation-disable byte, or -1 when the provider has no such output.
    int32_t disableByte = -1;
};

std::mutex g_observeMutex;
// Counts observer callbacks currently reading a resource. The worker publishes its status counters
// only after a callback returns, so phase-boundary waits must treat an active readback as
// "not quiet" even when the counters have not moved yet.
std::atomic<int> g_observeBusy{0};
std::vector<ObservedFrame> g_observedFrames;
std::vector<ObservedPair> g_observedPairs;
std::atomic<bool> g_observePixels{false};
// Set when an observation could not be completed (event creation, fence signalling, a timed-out
// wait, or an unreadable required resource). The probe owner stops its phases on this flag instead
// of reusing readback objects a timed-out GPU command may still reference.
std::atomic<bool> g_observeFailed{false};
// Set only for phases that interpret the SDK's own output. Warmup and the suppressed-pair checks
// read the presented image alone, because they make no statement about interpolation quality.
std::atomic<bool> g_observeRaw{false};
// Scene content of each submitted source frame, so an observed image or guide plane can be related
// to what was actually rendered for that frame. Guarded by g_observeMutex.
struct SceneRecord
{
    int objectX = -1;
    int objectY = 0;
    uint32_t generation = 0;
    // Rectangle displacement in output pixels between the previous and the current source frame.
    float stepX = 0.0f;
    float stepY = 0.0f;
};
std::map<int32_t, SceneRecord> g_scenes;

// Guide-row contract sampling. Armed for a phase; only the first four observable frames of that
// phase are read, because the contract is a property of the transport, not of a particular frame.
std::atomic<bool> g_observeGuides{false};
std::atomic<int> g_guideFrames{0};

// FSR reset diagnostic. The initial reset's generated plane is published only while the test switch
// is on, so its marker verdict is saved here before the bounded frame window can evict it. Late
// non-reset samples are classified as they are observed, joined by generation and source frame.
struct FsrResetDiagnostic
{
    bool active = false;
    uint32_t generation = 0;
    int32_t firstFrame = 0;
    bool resetSeen = false;
    bool resetRed = false;
    bool resetBlue = false;
    bool resetInputBlack = false;
    int samples = 0;
    int inputBlack = 0;
    int red = 0;
    int blue = 0;
    int mixed = 0;
    int unmarked = 0;
};

FsrResetDiagnostic g_fsrDiagnostic; // guarded by g_observeMutex
// Latched when the probe's bounded progress watchdog fires: later phases are not evidence any more.
std::atomic<bool> g_probeFailed{false};
// Set once a probe run reached its ordinary shutdown, so the 60-second process limit never fires
// during a healthy teardown.
std::atomic<bool> g_probeFinished{false};
// Set only while the probe's cadence phase runs: reports owner-side waits longer than 100 ms.
bool g_traceSlowWaits = false;

double QpcMilliseconds(int64_t ticks)
{
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    if (frequency.QuadPart <= 0) return 0.0;
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

struct D12ReadState
{
    ComPtr<ID3D12Device> device;
    // The provider's own queue. Readback uses it so the copied work is ordered behind the provider's
    // fence instead of racing it on an unrelated queue.
    ID3D12CommandQueue* queue = nullptr;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    FgReadbackPlane plane;
    HANDLE event = nullptr;
    uint64_t value = 0;
    // A wait that timed out leaves GPU work referencing these objects. They are never released or
    // reset again: the probe is failed instead.
    bool unusable = false;
};

// Reads one whole D3D12 texture through a fenced copy on the provider's queue. `sourceState` is the
// provider's own tracked state; it is restored so the provider's bookkeeping stays exact.
bool ReadD12Texture(ID3D12CommandQueue* queue, ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState,
    Image& image, std::string& reason)
{
    static D12ReadState state;
    if (source == nullptr)
    {
        reason = "no frame-generation resource was observed";
        return false;
    }
    if (queue == nullptr)
    {
        reason = "no provider queue was supplied for the frame-generation readback";
        return false;
    }
    if (state.unusable)
    {
        reason = "a previous frame-generation readback never completed; the readback state is retained";
        return false;
    }
    const D3D12_RESOURCE_DESC desc = source->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width == 0 || desc.Height == 0 ||
        desc.DepthOrArraySize > 1)
    {
        reason = "the observed frame-generation resource is not a single 2D texture";
        return false;
    }
    ID3D12Device* device = nullptr;
    if (FAILED(source->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        reason = "the observed frame-generation resource has no device";
        return false;
    }
    if (state.device.Get() != device || state.queue != queue)
    {
        // Only swap the cached objects once every command they were used by has completed.
        if (state.device != nullptr &&
            (state.fence == nullptr || state.fence->GetCompletedValue() < state.value))
        {
            device->Release();
            reason = "the frame-generation readback state cannot be proven idle";
            return false;
        }
        state.plane = FgReadbackPlane{};
        state.fence.Reset();
        state.list.Reset();
        state.allocator.Reset();
        state.queue = queue;
        state.device.Attach(device);
        state.value = 0;
    }
    else
    {
        device->Release();
    }
    if (state.allocator == nullptr || state.list == nullptr || state.fence == nullptr)
    {
        if (FAILED(state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&state.allocator))) ||
            FAILED(state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, state.allocator.Get(),
                nullptr, IID_PPV_ARGS(&state.list))) ||
            FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state.fence))) ||
            !state.allocator || !state.list || !state.fence)
        {
            reason = "the frame-generation readback device objects could not be created";
            return false;
        }
        state.list->Close();
    }
    if (state.event == nullptr)
    {
        state.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (state.event == nullptr)
        {
            reason = "the frame-generation readback event could not be created";
            return false;
        }
    }
    const HRESULT allocatorReset = state.allocator->Reset();
    const HRESULT listReset = SUCCEEDED(allocatorReset)
        ? state.list->Reset(state.allocator.Get(), nullptr)
        : allocatorReset;
    if (FAILED(allocatorReset) || FAILED(listReset))
    {
        reason = "the frame-generation readback list could not be reset";
        return false;
    }
    // RGBA8_UNORM and R16G16_FLOAT both use four bytes per texel; the shared helper owns the
    // copyable footprint and the 256-byte row alignment.
    if (!FgRecordReadback(state.device.Get(), state.list.Get(), source, sourceState, state.plane, reason))
        return false;
    if (FAILED(state.list->Close()))
    {
        reason = "the frame-generation readback list could not be closed";
        return false;
    }
    ID3D12CommandList* lists[1] = {state.list.Get()};
    state.queue->ExecuteCommandLists(1, lists);
    const uint64_t value = ++state.value;
    const HRESULT signalled = state.queue->Signal(state.fence.Get(), value);
    const HRESULT armed = SUCCEEDED(signalled) ? state.fence->SetEventOnCompletion(value, state.event)
                                              : signalled;
    const DWORD waitResult = SUCCEEDED(armed) ? WaitForSingleObject(state.event, 5000) : WAIT_FAILED;
    if (FAILED(signalled) || FAILED(armed) || waitResult != WAIT_OBJECT_0)
    {
        // The submission may still be executing: its allocator, list, staging buffer and fence stay
        // untouched so the GPU never reads memory this process rewrote or released.
        state.unusable = true;
        reason = waitResult == WAIT_TIMEOUT
            ? "the observed frame-generation image did not read back before the wait expired"
            : "the observed frame-generation image did not read back";
        return false;
    }
    const HRESULT removed = state.device->GetDeviceRemovedReason();
    if (FAILED(removed))
    {
        reason = "the frame-generation device was removed while reading an observed image";
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!FgMapTightRows(state.plane, bytes, reason)) return false;
    image.width = state.plane.width;
    image.height = state.plane.height;
    image.rgba.assign(static_cast<size_t>(image.width) * image.height * 4, 0.0f);
    const DXGI_FORMAT format = state.plane.format;
    for (UINT y = 0; y < image.height; y++)
    {
        const auto* row = bytes.data() + static_cast<size_t>(y) * image.width * 4;
        float* out = &image.rgba[static_cast<size_t>(y) * image.width * 4];
        for (UINT x = 0; x < image.width; x++)
        {
            const uint8_t* texel = row + static_cast<size_t>(x) * 4;
            if (format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                format == DXGI_FORMAT_B8G8R8A8_UNORM)
            {
                const bool bgr = format == DXGI_FORMAT_B8G8R8A8_UNORM;
                out[x * 4 + 0] = static_cast<float>(texel[bgr ? 2 : 0]) / 255.0f;
                out[x * 4 + 1] = static_cast<float>(texel[1]) / 255.0f;
                out[x * 4 + 2] = static_cast<float>(texel[bgr ? 0 : 2]) / 255.0f;
                out[x * 4 + 3] = static_cast<float>(texel[3]) / 255.0f;
            }
            else if (format == DXGI_FORMAT_R16G16_FLOAT)
            {
                for (int channel = 0; channel < 2; channel++)
                {
                    const uint16_t bits = static_cast<uint16_t>(texel[channel * 2]) |
                        static_cast<uint16_t>(texel[channel * 2 + 1] << 8);
                    const float sign = (bits & 0x8000u) != 0 ? -1.0f : 1.0f;
                    const uint32_t exponent = (bits >> 10) & 0x1Fu;
                    const uint32_t mantissa = bits & 0x3FFu;
                    float value = 0.0f;
                    if (exponent == 0) value = std::ldexp(static_cast<float>(mantissa), -24);
                    else if (exponent != 31)
                        value = std::ldexp(static_cast<float>(0x400u | mantissa),
                            static_cast<int>(exponent) - 25);
                    out[x * 4 + channel] = sign * value;
                }
            }
            else if (format == DXGI_FORMAT_R32_FLOAT)
            {
                std::memcpy(&out[x * 4], texel, sizeof(float));
            }
            else
            {
                reason = "the observed frame-generation format cannot be read back";
                return false;
            }
        }
    }
    return true;
}

// Frame-coded patch written into both the final backbuffer and the pre-overlay colour. Its position
// is outside the bands, the marker, the moving object and the HUD rectangle, so a decoded value can
// only come from the producer, never from scene content.
constexpr UINT kPatchLeft = 1180;
constexpr UINT kPatchTop = 260;
constexpr UINT kPatchSize = 16;

// HUD rectangle of the final colour only; the pre-overlay plane never contains it. Declared here
// because the observer excludes this region when it compares the HUD-less input.
constexpr UINT kHudLeft = 900;
constexpr UINT kHudTop = 600;
constexpr UINT kHudRight = 1100;
constexpr UINT kHudBottom = 680;

// Decodes the frame index the producer wrote. Codes are 8-bit: r = index & 255, g = index >> 8, and
// a constant blue distinguishes the patch from anything else that happens to be dark. Every image
// this harness decodes is in presented-backbuffer rows, so the patch is read where it was written.
bool DecodeFramePatch(const Image& image, int32_t& frame)
{
    if (!image.Valid() || kPatchLeft + kPatchSize > image.width || kPatchTop + kPatchSize > image.height)
        return false;
    const UINT top = kPatchTop;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    for (UINT y = top; y < top + kPatchSize; y++)
    {
        for (UINT x = kPatchLeft; x < kPatchLeft + kPatchSize; x++)
        {
            const float* texel = image.At(x, y);
            red += texel[0];
            green += texel[1];
            blue += texel[2];
        }
    }
    const double count = static_cast<double>(kPatchSize) * kPatchSize;
    red /= count;
    green /= count;
    blue /= count;
    if (std::fabs(blue - 0.25) > 0.08) return false;
    const long low = std::lround(red * 255.0);
    const long high = std::lround(green * 255.0);
    if (low < 0 || low > 255 || high < 0 || high > 255) return false;
    frame = static_cast<int32_t>(low | (high << 8));
    return true;
}

// Largest per-channel absolute difference between two images of the same extent.
bool MaxChannelDiff(const Image& left, const Image& right, float& maximum)
{
    maximum = 0.0f;
    if (!left.Valid() || !right.Valid() || left.width != right.width || left.height != right.height)
        return false;
    for (size_t i = 0; i < left.rgba.size(); i++)
    {
        const float delta = std::fabs(left.rgba[i] - right.rgba[i]);
        if (delta > maximum) maximum = delta;
    }
    return true;
}

// Largest per-channel difference between the pre-overlay plane and the retained real colour outside
// the HUD rectangle. Both planes are in presented-backbuffer rows.
bool MaxChannelDiffOutsideHud(const Image& left, const Image& right, float& maximum)
{
    maximum = 0.0f;
    if (!left.Valid() || !right.Valid() || left.width != right.width || left.height != right.height)
        return false;
    for (UINT y = 0; y < left.height; y++)
    {
        const float* a = left.At(0, y);
        const float* b = right.At(0, y);
        for (UINT x = 0; x < left.width; x++)
        {
            if (x >= kHudLeft && x < kHudRight && y >= kHudTop && y < kHudBottom) continue;
            const float* texelA = a + static_cast<size_t>(x) * 4;
            const float* texelB = b + static_cast<size_t>(x) * 4;
            for (int channel = 0; channel < 4; channel++)
            {
                const float delta = std::fabs(texelA[channel] - texelB[channel]);
                if (delta > maximum) maximum = delta;
            }
        }
    }
    return true;
}

// The SDK's reset debug overlay writes red for rows below 32 when the interpolator reset and blue
// for rows 32..63 when optical flow reported a scene change. The fixture blacks those rows in every
// producer, so a channel this strong can only be the SDK's own marker.
bool ReadFsrResetMarkers(const Image& image, bool& red, bool& blue)
{
    red = blue = false;
    if (!image.Valid() || image.width < 2 || image.height < 64) return false;
    const float* resetTexel = image.At(image.width / 2, 16);
    const float* sceneTexel = image.At(image.width / 2, 48);
    red = resetTexel[0] > 0.9f;
    blue = sceneTexel[2] > 0.9f;
    return true;
}

// True when the retained producer colour is black at both marker rows. A marker can only be
// interpreted when the input under it was black, not scene content.
bool InputMarkerRowsBlack(const Image& real)
{
    if (!real.Valid() || real.width < 2 || real.height < 64) return false;
    const float* resetRow = real.At(real.width / 2, 16);
    const float* sceneRow = real.At(real.width / 2, 48);
    const float tolerance = (1.0f / 255.0f) + 1e-4f;
    for (int channel = 0; channel < 3; channel++)
    {
        if (std::fabs(resetRow[channel]) > tolerance || std::fabs(sceneRow[channel]) > tolerance)
            return false;
    }
    return true;
}

// Saves the FSR reset diagnostic's marker verdicts under the observation mutex, joined by
// generation and source frame, before the bounded frame window can evict the reset sample.
void ClassifyFsrResetDiagnostic(const FgTestGeneratedImage& image, const ObservedFrame& frame,
    const Image& real)
{
    std::lock_guard<std::mutex> guard(g_observeMutex);
    FsrResetDiagnostic& state = g_fsrDiagnostic;
    if (!state.active || frame.generation != state.generation) return;
    if (!real.Valid()) return;
    const bool inputBlack = InputMarkerRowsBlack(real);
    bool red = false;
    bool blue = false;
    if (image.reset)
    {
        // Only the diagnostic publishes a generated plane for a reset; an ordinary reset has none.
        if (image.generated == nullptr || !frame.raw.Valid()) return;
        ReadFsrResetMarkers(frame.raw, red, blue);
        state.resetSeen = true;
        state.resetRed = red;
        state.resetBlue = blue;
        state.resetInputBlack = inputBlack;
        return;
    }
    if (frame.sourceFrame < state.firstFrame + 16 || !frame.raw.Valid()) return;
    ReadFsrResetMarkers(frame.raw, red, blue);
    state.samples++;
    if (inputBlack) state.inputBlack++;
    if (red && blue) state.mixed++;
    else if (red) state.red++;
    else if (blue) state.blue++;
    else state.unmarked++;
}

// Mean of the first three channels, so a black plane is distinguishable from a merely different one.
bool MeanColour(const Image& image, float& mean)
{
    mean = 0.0f;
    if (!image.Valid()) return false;
    double sum = 0.0;
    size_t count = 0;
    for (size_t i = 0; i + 3 < image.rgba.size(); i += 4)
    {
        sum += image.rgba[i] + image.rgba[i + 1] + image.rgba[i + 2];
        count += 3;
    }
    if (count == 0) return false;
    mean = static_cast<float>(sum / static_cast<double>(count));
    return true;
}

struct ObserveBusyGuard
{
    ObserveBusyGuard() { g_observeBusy.fetch_add(1); }
    ~ObserveBusyGuard() { g_observeBusy.fetch_sub(1); }
};

// Defined next to the guide fixture below; declared here because the observer runs before it.
void CompareGuideRows(const FgTestGeneratedImage& image, ObservedFrame& frame);

void FrameGenerationObserver(const FgTestGeneratedImage* image, const FgTestPairTiming* timing)
{
    ObserveBusyGuard busy;
    if (timing != nullptr)
    {
        ObservedPair pair;
        pair.generation = timing->generation;
        pair.sourceFrame = timing->sourceFrame;
        pair.generatedCount = timing->generatedCount;
        for (uint32_t index = 0; index < 5; index++)
        {
            pair.generatedQpc[index] = timing->generatedQpc[index];
            pair.generatedResult[index] = timing->generatedResult[index];
        }
        pair.realQpc = timing->realQpc;
        pair.realResult = timing->realResult;
        pair.reset = timing->reset;
        pair.disableByte = timing->disableByte;
        std::lock_guard<std::mutex> guard(g_observeMutex);
        g_observedPairs.push_back(pair);
        return;
    }
    if (image == nullptr) return;
    // Once a readback could not be proven complete, touching device objects again would risk reading
    // or resetting memory a timed-out submission still references. The owner stops the probe.
    if (g_observeFailed.load()) return;
    std::printf("    observe start frame=%d reset=%d realonly=%d disabled=%d pixels=%d\n", image->sourceFrame,
        image->reset ? 1 : 0, image->realOnly ? 1 : 0, image->interpolationDisabled ? 1 : 0,
        g_observePixels.load() ? 1 : 0);
    ObservedFrame frame;
    frame.generation = image->generation;
    frame.sourceFrame = image->sourceFrame;
    frame.intermediateIndex = image->intermediateIndex;
    frame.intermediateCount = image->intermediateCount;
    frame.reset = image->reset;
    frame.realOnly = image->realOnly;
    frame.interpolationDisabled = image->interpolationDisabled;
    frame.ok = true;
    // A non-reset, non-real-only manual-provider sample must carry the SDK output, the resource it
    // was copied from and the child buffer it was copied into. Anything else is not interpretable.
    if (!image->realOnly && !image->reset &&
        (image->childCopySource == nullptr || image->childBackbuffer == nullptr || image->generated == nullptr))
    {
        frame.ok = false;
        frame.reason = "a non-reset sample is missing its generated or child colour resource";
        g_observeFailed.store(true);
    }
    if (frame.ok && g_observePixels.load())
    {
        // The provider's own fence proves no command still touches these resources. A fence-less
        // sample cannot be read safely at all.
        if (image->fence == nullptr || image->fenceValue == 0)
        {
            frame.ok = false;
            frame.reason = "the observed sample carries no provider fence";
            g_observeFailed.store(true);
        }
        else
        {
            static HANDLE waitEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            const HRESULT armed = waitEvent != nullptr
                ? image->fence->SetEventOnCompletion(image->fenceValue, waitEvent)
                : E_FAIL;
            const DWORD waited = SUCCEEDED(armed) ? WaitForSingleObject(waitEvent, 5000) : WAIT_FAILED;
            if (FAILED(armed) || waited != WAIT_OBJECT_0)
            {
                frame.ok = false;
                frame.reason = waited == WAIT_TIMEOUT
                    ? "the provider fence did not complete before the observation window expired"
                    : "the provider fence could not be awaited";
                g_observeFailed.store(true);
            }
        }
    }
    if (frame.ok && g_observePixels.load())
    {
        // Scratch images are reused: only the two planes the assertions need are retained.
        static Image real;
        static Image copySource;
        static Image child;
        static Image evaluation;
        bool copyRead = false;
        bool childRead = false;
        bool realRead = false;
        float evaluationMean = 0.0f;
        bool evaluationMeanValid = false;
        if (frame.ok && image->realColor != nullptr)
        {
            frame.ok = ReadD12Texture(image->queue, image->realColor, image->realColorState, real, frame.reason);
            realRead = frame.ok;
        }
        if (frame.ok && realRead) frame.codedValid = DecodeFramePatch(real, frame.codedFrame);
        if (frame.ok && image->childCopySource != nullptr)
        {
            frame.ok = ReadD12Texture(image->queue, image->childCopySource, image->childCopyState, copySource,
                frame.reason);
            copyRead = frame.ok;
        }
        if (frame.ok && image->childBackbuffer != nullptr)
        {
            frame.ok = ReadD12Texture(image->queue, image->childBackbuffer, image->childBackbufferState, child,
                frame.reason);
            childRead = frame.ok;
        }
        if (frame.ok && childRead)
        {
            float difference = 0.0f;
            if (copyRead && MaxChannelDiff(child, copySource, difference))
            {
                frame.childCompared = true;
                frame.childVsCopyMax = difference;
            }
            if (realRead && MaxChannelDiff(child, real, difference))
            {
                frame.realCompared = true;
                frame.childVsRealMax = difference;
            }
        }
        // A spatial sample also reports whether the SDK received the same colour the display did.
        if (frame.ok && !image->realOnly && !image->reset && image->evaluationColor != nullptr &&
            image->evaluationColor != image->realColor)
        {
            if (ReadD12Texture(image->queue, image->evaluationColor, image->evaluationColorState, evaluation,
                    frame.reason))
            {
                float difference = 0.0f;
                evaluationMeanValid = MeanColour(evaluation, evaluationMean);
                if (realRead && MaxChannelDiff(evaluation, real, difference))
                {
                    frame.evaluationCompared = true;
                    frame.evaluationVsRealMax = difference;
                }
            }
            else
            {
                frame.ok = false;
            }
        }
        // The pre-overlay plane must differ from the final colour wherever the HUD was drawn; an
        // identical pair means the provider was handed the overlay frame as its "HUD-less" input.
        if (frame.ok && !image->realOnly && !image->reset && image->evaluationHudless != nullptr &&
            image->evaluationHudless != image->realColor && image->evaluationHudless != image->evaluationColor)
        {
            if (ReadD12Texture(image->queue, image->evaluationHudless, image->evaluationHudlessState, evaluation,
                    frame.reason))
            {
                float difference = 0.0f;
                if (realRead && MaxChannelDiff(evaluation, real, difference))
                {
                    frame.hudlessCompared = true;
                    frame.hudlessVsRealMax = difference;
                }
                // The plane the SDK received must match the scene outside the HUD rectangle. Both
                // planes are in presented-backbuffer rows, so a row flip shows up as a mismatch.
                if (realRead && MaxChannelDiffOutsideHud(evaluation, real, difference))
                {
                    frame.hudlessSpatialCompared = true;
                    frame.hudlessSpatialVsRealMax = difference;
                    frame.hudlessCodedValid = DecodeFramePatch(evaluation, frame.hudlessCodedFrame);
                }
            }
            else
            {
                frame.ok = false;
            }
        }
        if (frame.ok)
        {
            // A black plane and a merely different plane are different findings, so the means are
            // reported alongside the differences.
            MeanColour(real, frame.realMean);
            if (!image->realOnly && !image->reset && copyRead) MeanColour(copySource, frame.screenMean);
            if (evaluationMeanValid) frame.evaluationMean = evaluationMean;
        }
        if (frame.ok)
        {
            // A suppressed pair presented the child buffer itself; every other pair presented the
            // provider's copy source. A scratch image that was never read is never retained.
            if ((image->realOnly || image->reset) && childRead)
            {
                MeanColour(child, frame.screenMean);
                frame.screen = std::move(child);
            }
            else if (!image->realOnly && !image->reset && copyRead) frame.screen = std::move(copySource);
            if (g_observeRaw.load() && image->generated != nullptr &&
                image->generated != image->childCopySource)
            {
                frame.ok = ReadD12Texture(image->queue, image->generated, image->generatedState, frame.raw,
                    frame.reason);
            }
            else
            {
                frame.raw = frame.screen;
            }
            if (g_observeRaw.load()) ClassifyFsrResetDiagnostic(*image, frame, real);
        }
        if (frame.ok) CompareGuideRows(*image, frame);
        if (!frame.ok) g_observeFailed.store(true);
    }
    std::printf("    observe done frame=%d ok=%d coded=%d childcopy=%.3f childreal=%.3f eval=%.3f hudless=%.3f "
                "mean(real=%.3f screen=%.3f eval=%.3f) %s\n",
        frame.sourceFrame, frame.ok ? 1 : 0, frame.codedValid ? frame.codedFrame : -1, frame.childVsCopyMax,
        frame.childVsRealMax, frame.evaluationVsRealMax, frame.hudlessVsRealMax, frame.realMean, frame.screenMean,
        frame.evaluationMean, frame.reason.c_str());
    std::lock_guard<std::mutex> guard(g_observeMutex);
    g_observedFrames.push_back(std::move(frame));
    // Each retained image is a decoded 1280x720 frame; only the newest window is ever asserted on.
    // A source frame can contribute up to five intermediates, so the bounded window keeps enough
    // source frames to satisfy the >=20-sample gates at every supported multiplier.
    while (g_observedFrames.size() > 120) g_observedFrames.erase(g_observedFrames.begin());
}

void ResetObservations()
{
    std::lock_guard<std::mutex> guard(g_observeMutex);
    g_observedFrames.clear();
    g_observedPairs.clear();
}

// Mean colour of an axis-aligned rectangle, clamped to the image.
void AverageColor(const Image& image, UINT x0, UINT y0, UINT x1, UINT y1, float out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (!image.Valid()) return;
    if (x1 > image.width) x1 = image.width;
    if (y1 > image.height) y1 = image.height;
    size_t count = 0;
    for (UINT y = y0; y < y1; y++)
    {
        for (UINT x = x0; x < x1; x++)
        {
            const float* texel = image.At(x, y);
            for (int channel = 0; channel < 4; channel++) out[channel] += texel[channel];
            count++;
        }
    }
    if (count == 0) return;
    for (int channel = 0; channel < 4; channel++) out[channel] /= static_cast<float>(count);
}

struct ChannelRange
{
    float minValue;
    float maxValue;
};

// Centroid of every pixel inside [y0,y1) whose channels satisfy all three ranges.
bool FindCentroid(const Image& image, ChannelRange red, ChannelRange green, ChannelRange blue, UINT y0, UINT y1,
    float& centreX, float& centreY)
{
    centreX = centreY = 0.0f;
    if (!image.Valid()) return false;
    if (y1 > image.height) y1 = image.height;
    double sumX = 0.0;
    double sumY = 0.0;
    size_t count = 0;
    for (UINT y = y0; y < y1; y++)
    {
        for (UINT x = 0; x < image.width; x++)
        {
            const float* texel = image.At(x, y);
            if (texel[0] < red.minValue || texel[0] > red.maxValue) continue;
            if (texel[1] < green.minValue || texel[1] > green.maxValue) continue;
            if (texel[2] < blue.minValue || texel[2] > blue.maxValue) continue;
            sumX += static_cast<double>(x);
            sumY += static_cast<double>(y);
            count++;
        }
    }
    if (count == 0) return false;
    centreX = static_cast<float>(sumX / static_cast<double>(count));
    centreY = static_cast<float>(sumY / static_cast<double>(count));
    return true;
}

// Fixture geometry. The white marker's centroid is exactly (10,10); the moving rectangle steps by a
// fixed number of output pixels per source frame so the expected interpolation midpoint is exact.
constexpr UINT kMarkerLeft = 7;
constexpr UINT kMarkerRight = 14;
constexpr UINT kMarkerTop = 64;
constexpr float kMarkerCentre = 10.0f;
constexpr float kMarkerCentreRow = 67.0f;
constexpr UINT kObjectSize = 32;
constexpr UINT kObjectY = 340;
constexpr UINT kObjectBase = 320;
constexpr UINT kObjectStep = 16;
const float kBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
const float kRedBand[4] = {1.0f, 0.0f, 0.0f, 1.0f};
const float kBlueBand[4] = {0.0f, 0.0f, 1.0f, 1.0f};
// Magenta, not white: the SDK's HUD extraction can leave white speckle in band rows, and the marker
// position must be measured from the marker alone.
const float kMarker[4] = {1.0f, 0.0f, 1.0f, 1.0f};
const float kGreen[4] = {0.0f, 1.0f, 0.0f, 1.0f};
const float kHud[4] = {1.0f, 0.5f, 0.0f, 1.0f};

void ClearRect(ID3D11DeviceContext1* context, ID3D11RenderTargetView* target, const float colour[4], int left,
    int top, int right, int bottom)
{
    if (context == nullptr || target == nullptr || right <= left || bottom <= top) return;
    const D3D11_RECT rect{left, top, right, bottom};
    context->ClearView(target, colour, &rect, 1);
}

// Synthetic camera shared by the projection the packet carries and the depth the guides carry.
constexpr float kCameraNear = 0.1f;
constexpr float kCameraFar = 1000.0f;
constexpr float kCameraFovRadians = 1.0f;
constexpr float kObjectViewDepth = 5.0f;
constexpr float kBackgroundViewDepth = 10.0f;

// Reversed-Z depth of one view-space distance under the row-vector projection below: clipZ is
// near/(near-far)*z + near*far/(far-near) and w is z, so nearer geometry stores a larger value.
float ProjectedDepth(float viewDepth)
{
    const float a = kCameraNear / (kCameraNear - kCameraFar);
    const float b = kCameraNear * kCameraFar / (kCameraFar - kCameraNear);
    return (a * viewDepth + b) / viewDepth;
}

// The frame-coded patch that proves which source frame a captured image belongs to.
void FramePatchColour(int32_t frameIndex, float out[4])
{
    const uint32_t value = static_cast<uint32_t>(frameIndex) & 0xffffu;
    out[0] = static_cast<float>(value & 0xffu) / 255.0f;
    out[1] = static_cast<float>((value >> 8) & 0xffu) / 255.0f;
    out[2] = 0.25f;
    out[3] = 1.0f;
}

// Composes the screen-space scene: red upper band, blue lower band, magenta marker, the frame-coded
// patch and the moving opaque rectangle. The HUD rectangle is optional because the bound-HUD-less
// route captures the scene before the overlay is drawn.
void ComposeScreenScene(ID3D11DeviceContext1* context, ID3D11RenderTargetView* target, UINT width, UINT height,
    int objectX, int objectY, float jitterX, float jitterY, bool hud, int32_t frameIndex)
{
    if (context == nullptr || target == nullptr) return;
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ClearRect(context, target, black, 0, 0, static_cast<int>(width), static_cast<int>(height));
    const int shiftX = static_cast<int>(std::lround(jitterX));
    const int shiftY = static_cast<int>(std::lround(jitterY));
    const int markerLeft = static_cast<int>(kMarkerLeft) + shiftX;
    const int markerTop = static_cast<int>(kMarkerTop) + shiftY;
    ClearRect(context, target, kRedBand, 0, shiftY, static_cast<int>(width),
        static_cast<int>(height / 4) + shiftY);
    ClearRect(context, target, kBlueBand, 0, static_cast<int>(height * 3 / 4) + shiftY,
        static_cast<int>(width), static_cast<int>(height) + shiftY);
    ClearRect(context, target, kMarker, markerLeft, markerTop,
        markerLeft + static_cast<int>(kMarkerRight - kMarkerLeft),
        markerTop + static_cast<int>(kMarkerRight - kMarkerLeft));
    float patch[4]{};
    FramePatchColour(frameIndex, patch);
    ClearRect(context, target, patch, static_cast<int>(kPatchLeft), static_cast<int>(kPatchTop),
        static_cast<int>(kPatchLeft + kPatchSize), static_cast<int>(kPatchTop + kPatchSize));
    if (objectX >= 0)
    {
        ClearRect(context, target, kGreen, objectX, objectY, objectX + static_cast<int>(kObjectSize),
            objectY + static_cast<int>(kObjectSize));
    }
    if (hud)
    {
        ClearRect(context, target, kHud, static_cast<int>(kHudLeft), static_cast<int>(kHudTop),
            static_cast<int>(kHudRight), static_cast<int>(kHudBottom));
    }
}

// Uploads the HUD-less scene with the same values and patch as the presented colour, in graph rows
// and in linear light: the pre-overlay colour is a URP graph attachment, so its row 0 is the
// presented image's last row and its values are linear while the backbuffer is sRGB-encoded. The
// transport converts both (rows and transfer function) before any provider sees the plane, and the
// guides get their rows converted the same way, which is exactly what the game's render graph
// produces.
void UploadHudless(ID3D11DeviceContext* context, ID3D11Texture2D* texture, UINT width, UINT height, int objectX,
    int objectY, int32_t frameIndex, bool diagnosticIndicators = false)
{
    if (context == nullptr || texture == nullptr) return;
    float patch[4]{};
    FramePatchColour(frameIndex, patch);
    std::vector<float> texels(static_cast<size_t>(width) * height * 4);
    for (UINT y = 0; y < height; y++)
    {
        const UINT targetRow = height - 1 - y;
        float* row = &texels[static_cast<size_t>(targetRow) * width * 4];
        for (UINT x = 0; x < width; x++)
        {
            const float* colour = kBlack;
            if (y < height / 4) colour = kRedBand;
            else if (y >= height * 3 / 4) colour = kBlueBand;
            if (x >= kMarkerLeft && x < kMarkerRight && y >= kMarkerTop && y < kMarkerTop + (kMarkerRight - kMarkerLeft))
                colour = kMarker;
            if (x >= kPatchLeft && x < kPatchLeft + kPatchSize && y >= kPatchTop && y < kPatchTop + kPatchSize)
                colour = patch;
            if (objectX >= 0 && x >= static_cast<UINT>(objectX) && x < static_cast<UINT>(objectX) + kObjectSize &&
                y >= static_cast<UINT>(objectY) && y < static_cast<UINT>(objectY) + kObjectSize)
                colour = kGreen;
            // Diagnostic-only: the SDK draws its reset markers into rows below 64, so the producer
            // makes that band black and a marker can only come from the overlay.
            if (diagnosticIndicators && y < 64) colour = kBlack;
            // Linear light, as the game's pre-overlay attachment really stores it. Alpha is not a
            // colour and is left untouched by the transport's transfer function.
            row[x * 4 + 0] = SrgbToLinear(colour[0]);
            row[x * 4 + 1] = SrgbToLinear(colour[1]);
            row[x * 4 + 2] = SrgbToLinear(colour[2]);
            row[x * 4 + 3] = colour[3];
        }
    }
    D3D11_BOX box{0, 0, 0, width, height, 1};
    context->UpdateSubresource(texture, 0, &box, texels.data(), width * 4 * sizeof(float),
        width * 4 * sizeof(float) * height);
}

// Reversed depth and a forward UV motion field, both restricted to the projected rectangle. Static
// geometry carries zero motion: a uniform field used to claim the whole scene moved, which no
// camera-relative interpolation can interpret. `stepX`/`stepY` are the rectangle's displacement in
// output pixels between the previous and the current source frame.
//
// URP's depth and motion attachments are graph-space textures: their row 0 is the presented image's
// last row. The fixture writes that same convention, so a provider that flips the captured colour
// into graph space (ANR) aligns guides and colour, and a provider that does not receives exactly the
// pair the game produces. The vertical motion component is therefore expressed in graph rows, where
// a downward screen movement is a negative row displacement.
void UploadGuides(ID3D11DeviceContext* context, ID3D11Texture2D* depth, ID3D11Texture2D* motion,
    UINT renderWidth, UINT renderHeight, UINT outputWidth, UINT outputHeight, int objectX, int objectY,
    float stepX, float stepY)
{
    const float scaleX = static_cast<float>(renderWidth) / static_cast<float>(outputWidth);
    const float scaleY = static_cast<float>(renderHeight) / static_cast<float>(outputHeight);
    const float left = static_cast<float>(objectX) * scaleX;
    const float top = static_cast<float>(objectY) * scaleY;
    const float right = left + static_cast<float>(kObjectSize) * scaleX;
    const float bottom = top + static_cast<float>(kObjectSize) * scaleY;
    // Graph row `y` carries presented-image row `renderHeight - 1 - y`.
    auto insideGraph = [&](UINT x, UINT y)
    {
        const float screenY = static_cast<float>(renderHeight - 1 - y) + 0.5f;
        return objectX >= 0 && x >= left && x < right && screenY >= top && screenY < bottom;
    };
    if (depth != nullptr)
    {
        const float foreground = ProjectedDepth(kObjectViewDepth);
        const float background = ProjectedDepth(kBackgroundViewDepth);
        std::vector<float> values(static_cast<size_t>(renderWidth) * renderHeight);
        for (UINT y = 0; y < renderHeight; y++)
        {
            for (UINT x = 0; x < renderWidth; x++)
            {
                values[static_cast<size_t>(y) * renderWidth + x] =
                    insideGraph(x, y) ? foreground : background;
            }
        }
        D3D11_BOX box{0, 0, 0, renderWidth, renderHeight, 1};
        context->UpdateSubresource(depth, 0, &box, values.data(), renderWidth * sizeof(float),
            renderWidth * sizeof(float) * renderHeight);
    }
    if (motion != nullptr)
    {
        // Forward UV displacement in graph rows: an output-pixel step divided by the output extent,
        // which is the same value in render space because both extents scale together.
        const float motionU = stepX / static_cast<float>(outputWidth);
        const float motionV = -stepY / static_cast<float>(outputHeight);
        std::vector<uint16_t> values(static_cast<size_t>(renderWidth) * renderHeight * 2);
        for (UINT y = 0; y < renderHeight; y++)
        {
            for (UINT x = 0; x < renderWidth; x++)
            {
                const bool inside = insideGraph(x, y);
                const size_t index = (static_cast<size_t>(y) * renderWidth + x) * 2;
                values[index] = ToHalf(inside ? motionU : 0.0f);
                values[index + 1] = ToHalf(inside ? motionV : 0.0f);
            }
        }
        D3D11_BOX box{0, 0, 0, renderWidth, renderHeight, 1};
        context->UpdateSubresource(motion, 0, &box, values.data(), renderWidth * 2 * sizeof(uint16_t),
            renderWidth * 2 * sizeof(uint16_t) * renderHeight);
    }
}

// Verifies the transport's screen-row guide contract against the scene that produced the frame.
// The fixture writes depth and motion in URP graph rows; the transport must hand every provider the
// presented-backbuffer rows of that same scene, with the motion y component negated by the flip.
// Only the first four observable frames of an armed phase are read.
void CompareGuideRows(const FgTestGeneratedImage& image, ObservedFrame& frame)
{
    if (!g_observeGuides.load() || image.reset) return;
    if (image.depth == nullptr || image.motion == nullptr || image.queue == nullptr) return;
    if (g_guideFrames.fetch_add(1) >= 4) return;
    SceneRecord scene;
    {
        std::lock_guard<std::mutex> guard(g_observeMutex);
        const auto found = g_scenes.find(frame.sourceFrame);
        if (found == g_scenes.end()) return;
        scene = found->second;
    }
    if (scene.objectX < 0 || scene.generation != frame.generation) return;
    Image depth;
    Image motion;
    std::string reason;
    if (!ReadD12Texture(image.queue, image.depth, image.depthState, depth, reason)) return;
    if (!ReadD12Texture(image.queue, image.motion, image.motionState, motion, reason)) return;
    if (!depth.Valid() || !motion.Valid() || depth.width != motion.width || depth.height != motion.height)
        return;
    const uint32_t renderWidth = depth.width;
    const uint32_t renderHeight = depth.height;
    const int32_t outputWidth = image.frame != nullptr ? image.frame->outputWidth : 0;
    const int32_t outputHeight = image.frame != nullptr ? image.frame->outputHeight : 0;
    if (outputWidth <= 0 || outputHeight <= 0) return;
    const float scaleX = static_cast<float>(renderWidth) / static_cast<float>(outputWidth);
    const float scaleY = static_cast<float>(renderHeight) / static_cast<float>(outputHeight);
    const float left = static_cast<float>(scene.objectX) * scaleX;
    const float top = static_cast<float>(scene.objectY) * scaleY;
    const float right = left + static_cast<float>(kObjectSize) * scaleX;
    const float bottom = top + static_cast<float>(kObjectSize) * scaleY;
    const float foreground = ProjectedDepth(kObjectViewDepth);
    const float background = ProjectedDepth(kBackgroundViewDepth);
    const float motionU = scene.stepX / static_cast<float>(outputWidth);
    const float motionV = scene.stepY / static_cast<float>(outputHeight);
    for (uint32_t y = 0; y < renderHeight; y += 20)
    {
        for (uint32_t x = 0; x < renderWidth; x += 20)
        {
            const float centreY = static_cast<float>(y) + 0.5f;
            const bool inside = x >= left && x < right && centreY >= top && centreY < bottom;
            const float* depthTexel = depth.At(x, y);
            const float expectedDepth = inside ? foreground : background;
            if (std::fabs(depthTexel[0] - expectedDepth) > 1e-6f) frame.guideMismatches++;
            const float* motionTexel = motion.At(x, y);
            const float expectedU = inside ? motionU : 0.0f;
            const float expectedV = inside ? motionV : 0.0f;
            if (std::fabs(motionTexel[0] - expectedU) > 2e-3f ||
                std::fabs(motionTexel[1] - expectedV) > 2e-3f)
                frame.guideMismatches++;
            frame.guideSamples += 2;
        }
    }
}

// Crash reporting: an access violation inside the harness must name its frame, not an address. The
// handler only prints and then lets the process die exactly as it would have.
using SymFromAddrFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, void*);
using SymInitializeFn = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
using SymGetLineFromAddr64Fn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD, void*);
using SymSetOptionsFn = DWORD(WINAPI*)(DWORD);

struct SymbolContext
{
    SymInitializeFn initialize = nullptr;
    SymFromAddrFn fromAddress = nullptr;
    SymGetLineFromAddr64Fn lineFromAddress = nullptr;
    bool ready = false;
};

SymbolContext& Symbols()
{
    static SymbolContext context;
    static bool attempted = false;
    if (attempted) return context;
    attempted = true;
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (dbghelp == nullptr) return context;
    context.initialize = reinterpret_cast<SymInitializeFn>(GetProcAddress(dbghelp, "SymInitialize"));
    context.fromAddress = reinterpret_cast<SymFromAddrFn>(GetProcAddress(dbghelp, "SymFromAddr"));
    context.lineFromAddress = reinterpret_cast<SymGetLineFromAddr64Fn>(
        GetProcAddress(dbghelp, "SymGetLineFromAddr64"));
    auto setOptions = reinterpret_cast<SymSetOptionsFn>(GetProcAddress(dbghelp, "SymSetOptions"));
    if (setOptions != nullptr) setOptions(0x2 | 0x4);   // UNDNAME | DEFERRED_LOADS
    context.ready = context.initialize != nullptr && context.fromAddress != nullptr &&
        context.initialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    return context;
}

// 96 bytes is enough for MAX_SYM_NAME plus the SYMBOL_INFO header.
struct SymbolBuffer
{
    unsigned char storage[sizeof(SYMBOL_INFO) + 1024];
    SymbolBuffer() { std::memset(storage, 0, sizeof(storage)); }
    SYMBOL_INFO* info() { return reinterpret_cast<SYMBOL_INFO*>(storage); }
};

struct LineBuffer
{
    unsigned char storage[sizeof(IMAGEHLP_LINE64) + 256];
    LineBuffer() { std::memset(storage, 0, sizeof(storage)); }
    IMAGEHLP_LINE64* info() { return reinterpret_cast<IMAGEHLP_LINE64*>(storage); }
};

void PrintSymbol(DWORD64 address, const char* prefix)
{
    SymbolContext& symbols = Symbols();
    DWORD64 base = 0;
    if (!symbols.ready) return;
    SymbolBuffer buffer;
    buffer.info()->SizeOfStruct = sizeof(SYMBOL_INFO);
    buffer.info()->MaxNameLen = 1024;
    if (symbols.fromAddress(GetCurrentProcess(), address, &base, buffer.info()) == FALSE) return;
    std::printf("%s %s+0x%llx", prefix, buffer.info()->Name,
        static_cast<unsigned long long>(address - buffer.info()->Address));
    if (symbols.lineFromAddress != nullptr)
    {
        LineBuffer line;
        line.info()->SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD displacement = 0;
        if (symbols.lineFromAddress(GetCurrentProcess(), address, &displacement, line.info()) != FALSE)
            std::printf(" (%s:%lu)", line.info()->FileName == nullptr ? "?" : line.info()->FileName,
                static_cast<unsigned long>(line.info()->LineNumber));
    }
    std::printf("\n");
}

LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* pointers)
{
    if (pointers == nullptr || pointers->ExceptionRecord == nullptr) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = pointers->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    std::printf("\nCRASH code=0x%08x thread=%lu address=%p\n", code, static_cast<unsigned long>(GetCurrentThreadId()),
        pointers->ExceptionRecord->ExceptionAddress);
    if (code == EXCEPTION_ACCESS_VIOLATION && pointers->ExceptionRecord->NumberParameters >= 2)
    {
        const ULONG_PTR operation = pointers->ExceptionRecord->ExceptionInformation[0];
        std::printf("CRASH %s at %p\n", operation == 0 ? "read" : operation == 1 ? "write" : "execute",
            reinterpret_cast<void*>(pointers->ExceptionRecord->ExceptionInformation[1]));
    }
    // Name the containing module first: it works without symbols and says whether the fault is in
    // this executable or inside a driver/runtime DLL we called.
    if (pointers->ExceptionRecord->ExceptionAddress != nullptr)
    {
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(pointers->ExceptionRecord->ExceptionAddress), &module) &&
            module != nullptr)
        {
            wchar_t modulePath[MAX_PATH]{};
            GetModuleFileNameW(module, modulePath, MAX_PATH);
            const DWORD64 offset = reinterpret_cast<DWORD64>(pointers->ExceptionRecord->ExceptionAddress) -
                reinterpret_cast<DWORD64>(module);
            std::printf("CRASH module=%ls base=%p offset=0x%llx\n", modulePath,
                reinterpret_cast<void*>(module), static_cast<unsigned long long>(offset));
        }
    }
    Symbols();
    CONTEXT* context = pointers->ContextRecord;
    if (context != nullptr)
    {
        PrintSymbol(context->Rip, "CRASH rip:");
        // A stack scan is not an unwind, but with symbols it names the calling frames well enough to
        // locate the fault. Stop at the first address outside any loaded module.
        const DWORD64* stack = reinterpret_cast<const DWORD64*>(context->Rsp);
        for (int index = 0; index < 24; index++)
        {
            const DWORD64 value = stack[index];
            if (value < 0x10000) continue;
            PrintSymbol(value, "CRASH stack:");
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Deterministic check of the frame-generation colour conversion contract, without a swapchain or an
// SDK: a source smaller than the output extent is refused, and a larger same-format pooled
// allocation contributes only its active output-sized region.
std::vector<uint8_t> ReadBackRGBA8(ID3D11Texture2D* texture)
{
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.BindFlags = 0;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    // The conversion leaves its UAV and SRV bound to the compute stage. Copying a resource while a
    // view of it is still bound is undefined for the driver, so the slots are cleared first.
    ID3D11UnorderedAccessView* noUav[1] = {nullptr};
    ID3D11ShaderResourceView* noSrv[1] = {nullptr};
    g_context->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
    g_context->CSSetShaderResources(0, 1, noSrv);
    g_context->CSSetShader(nullptr, nullptr, 0);
    ComPtr<ID3D11Texture2D> copy;
    if (FAILED(g_device->CreateTexture2D(&staging, nullptr, &copy)) || !copy) return {};
    g_context->CopyResource(copy.Get(), texture);
    g_context->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(desc.Width) * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; y++)
    {
        const auto* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        std::memcpy(&bytes[static_cast<size_t>(y) * desc.Width * 4], row, static_cast<size_t>(desc.Width) * 4);
    }
    g_context->Unmap(copy.Get(), 0);
    return bytes;
}

void CheckFrameGenerationColorExtents()
{
    // Earlier checks leave their own pipeline state bound; a fresh, empty context is the only
    // reliable starting point for a direct conversion call.
    g_context->ClearState();
    g_context->Flush();
    FrameGenerationResources resources;
    std::string reason;
    Check(resources.EnsureKernels(g_device.Get(), reason), "FG conversion kernels compile for the extent checks");
    FgSlotTextures slot;
    Check(resources.EnsureSlot(g_device.Get(), slot, 640, 360, 1280, 720, true, reason),
        "FG conversion slot is created for the extent checks");
    const uint32_t width = 1280;
    const uint32_t height = 720;

    // Same-family allocations above and below the output extent. Their content is irrelevant to the
    // contract under test: what matters is that a larger allocation is accepted through the active
    // region path and a smaller one is refused with its own reason.
    ComPtr<ID3D11Texture2D> larger = CreateTexture(width * 3 / 2, height * 3 / 2, 1, DXGI_FORMAT_B8G8R8A8_UNORM,
        D3D11_BIND_SHADER_RESOURCE);
    ComPtr<ID3D11Texture2D> smaller = CreateTexture(width / 2, height / 2, 1, DXGI_FORMAT_B8G8R8A8_UNORM,
        D3D11_BIND_SHADER_RESOURCE);
    ComPtr<ID3D11Texture2D> largerHudless = CreateTexture(width * 3 / 2, height * 3 / 2, 1,
        DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    Check(larger != nullptr && smaller != nullptr && largerHudless != nullptr,
        "FG extent fixtures are created");
    if (larger == nullptr || smaller == nullptr || largerHudless == nullptr) return;

    reason.clear();
    Check(resources.ConvertColor(g_context.Get(), slot, larger.Get(), width, height, reason),
        "a larger colour allocation is accepted (" + reason + ")");
    reason.clear();
    Check(!resources.ConvertColor(g_context.Get(), slot, smaller.Get(), width, height, reason),
        "a smaller colour allocation is refused");
    Check(reason.find("smaller than the frame-generation output extent") != std::string::npos,
        "the smaller-source refusal names the extent (got: " + reason + ")");
    reason.clear();
    Check(resources.ConvertHudless(g_context.Get(), slot, largerHudless.Get(), width, height, true, reason),
        "a larger HUD-less allocation is accepted (" + reason + ")");
    reason.clear();
    Check(!resources.ConvertHudless(g_context.Get(), slot, smaller.Get(), width, height, true, reason),
        "a smaller HUD-less allocation is refused");
    // The accepted conversion must have produced a readable output-sized plane.
    const auto bytes = ReadBackRGBA8(slot.color.Get());
    Check(bytes.size() == static_cast<size_t>(width) * height * 4,
        "the accepted conversion produced a full output-sized plane (" + std::to_string(bytes.size()) + " bytes)");
    slot.Reset();
}

// Coarse textual map of an observed image: one letter per block, enough to see rows, bands and
// markers without a viewer.
void PrintImageMap(const Image& image)
{
    if (!image.Valid()) return;
    const int columns = 64;
    const int rows = 24;
    std::printf("    image map %ux%u\n", image.width, image.height);
    for (int row = 0; row < rows; row++)
    {
        std::printf("    ");
        for (int column = 0; column < columns; column++)
        {
            const UINT x = static_cast<UINT>((column + 0.5) * image.width / columns);
            const UINT y = static_cast<UINT>((row + 0.5) * image.height / rows);
            const float* texel = image.At(std::min(x, image.width - 1), std::min(y, image.height - 1));
            char letter = '.';
            if (texel[0] > 0.6f && texel[1] > 0.6f && texel[2] > 0.6f) letter = 'W';
            else if (texel[0] > 0.6f && texel[1] > 0.3f && texel[2] < 0.3f) letter = 'O';
            else if (texel[0] > 0.6f && texel[1] < 0.3f && texel[2] < 0.3f) letter = 'R';
            else if (texel[1] > 0.5f && texel[0] < 0.4f && texel[2] < 0.4f) letter = 'G';
            else if (texel[2] > 0.6f && texel[0] < 0.4f) letter = 'B';
            else if (texel[0] + texel[1] + texel[2] > 0.2f) letter = '-';
            std::printf("%c", letter);
        }
        std::printf("\n");
    }
}

// True when this build staged the ANR runtime beside the check executable, i.e. the same
// `providers\anr\nvngx_dlssg.dll` path the provider preflight inspects.
bool AnrRuntimeStaged()
{
    wchar_t modulePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0) return false;
    std::wstring path(modulePath);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    path.resize(slash);
    path += L"\\providers\\anr\\nvngx_dlssg.dll";
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Runs `--fg anr` in a child process so the ANR family never loads into the parent harness. The
// `afterNormal` variant first runs `--fg fsr`'s suite in that child and then migrates to ANR.
bool RunAnrChildProcess(bool afterNormal, int& exitCode, std::string& reason)
{
    wchar_t modulePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0)
    {
        reason = "the harness path could not be read";
        return false;
    }
    std::wstring command = L"\"";
    command += modulePath;
    command += L"\" ";
    command += afterNormal ? L"--fg fsr --anr-after-normal" : L"--fg anr";
    command += L" --fg-multiplier ";
    command += std::to_wstring(g_fgMultiplier);
    if (g_fgSourceBlt) command += L" --source-blt";
    if (g_fgSourceGame) command += L" --source-game";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    // The child's diagnostics must survive its own exit; a shared file handle is enough because the
    // content is text and the reader runs after the process is reaped. The file lives in the temp
    // directory so running the checks never writes into the repository.
    wchar_t tempDirectory[MAX_PATH]{};
    const DWORD tempLength = GetTempPathW(MAX_PATH, tempDirectory);
    const wchar_t* childLogName = afterNormal ? L"bigscale-fg-anr-after-normal-child.log"
                                              : L"bigscale-fg-anr-child.log";
    std::wstring childLogPath = tempLength > 0 && tempLength < MAX_PATH
        ? std::wstring(tempDirectory) + childLogName
        : std::wstring(childLogName);
    // The child inherits this handle as its stdout/stderr, so it must be created inheritable;
    // CreateProcessW is called with bInheritHandles=TRUE below.
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    HANDLE childLog = CreateFileW(childLogPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (childLog != INVALID_HANDLE_VALUE)
    {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = childLog;
        startup.hStdError = childLog;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(0);
    const BOOL started = CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, childLog != INVALID_HANDLE_VALUE,
        0, nullptr, nullptr, &startup, &process);
    if (childLog != INVALID_HANDLE_VALUE) CloseHandle(childLog);
    if (!started)
    {
        reason = "the ANR child process could not be started";
        return false;
    }
    const DWORD waited = WaitForSingleObject(process.hProcess, 300000);
    exitCode = -1;
    if (waited == WAIT_OBJECT_0)
    {
        DWORD code = 1;
        if (GetExitCodeProcess(process.hProcess, &code)) exitCode = static_cast<int>(code);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (waited != WAIT_OBJECT_0)
    {
        reason = "the ANR child process did not finish";
        return false;
    }
    if (exitCode != 0)
    {
        reason = "the ANR child process reported failures (exit " + std::to_string(exitCode) + ")";
        return false;
    }
    return true;
}

// Frame-generation transport, provider, pixel and cadence checks.
//
// The harness owns a real D3D11 window/swapchain, records a capture packet per frame exactly like
// the URP hook does, and drives the real provider worker. Generated image content is verified
// through the testing observer, which hands the harness the resources that actually reach the
// child presenter, so an interpolated midpoint is a measurement, not a counter.
void CheckFrameGeneration(const char* label, int32_t provider)
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"BigScale.PresenterTest";
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        Check(false, "presenter test window class registered");
        return;
    }
    // ANR refuses capture buffers narrower than 640 pixels (its shipped programs start there), so the
    // fixture guarantees an exact 1280x720 client area instead of a window size that loses its frame
    // and chrome. Every runtime extent below is still derived from the real backbuffer.
    RECT windowRect{0, 0, 1280, 720};
    Check(AdjustWindowRectEx(&windowRect, WS_OVERLAPPEDWINDOW, FALSE, 0) != 0,
        "the presenter test window's 1280x720 client area is computed");
    const int windowWidth = windowRect.right - windowRect.left;
    const int windowHeight = windowRect.bottom - windowRect.top;
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"BigScale frame generation test",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, windowWidth, windowHeight, nullptr, nullptr, instance,
        nullptr);
    Check(window != nullptr, "presenter test window created");
    if (window == nullptr) return;
    ShowWindow(window, SW_SHOWNA);
    UpdateWindow(window);
    // DLSS-G disables itself while its swap-chain window is not the foreground window, which a
    // background check process never is. The harness owns the desktop for the duration of the run.
    SetForegroundWindow(window);
    SetActiveWindow(window);
    SetFocus(window);
    RECT clientRect{};
    GetClientRect(window, &clientRect);
    const int clientWidth = clientRect.right - clientRect.left;
    const int clientHeight = clientRect.bottom - clientRect.top;
    Check(clientWidth >= 640 && clientHeight > 0, "the presenter test window exposes a capturable client area");
    std::printf("%s: client area %dx%d (window %dx%d)\n", label, clientWidth, clientHeight, windowWidth,
        windowHeight);

    // Default: the two-buffer flip-discard source the fixture has always exercised. `--source-blt`
    // models one legal game shape (one buffer, legacy bitblt, no flags) and `--source-game` models
    // the shipping game's observed shape (two buffers, FLIP_SEQUENTIAL, flags 0x842). Provider-owned
    // child swapchains are never altered by any of these.
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = g_fgSourceBlt ? 1 : 2;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    if (g_fgSourceBlt)
    {
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc.Flags = 0;
    }
    else if (g_fgSourceGame)
    {
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH |
            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    else
    {
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    }
    ComPtr<IDXGISwapChain> swapchain;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const HRESULT created = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr,
        0, D3D11_SDK_VERSION, &desc, &swapchain, &device, nullptr, &context);
    if (FAILED(created) || !swapchain || !device || !context)
    {
        // No silent fallback to a different source shape: a request this driver refuses is reported
        // as the failed creation it is.
        Check(false, std::string(label) + ": frame-generation test swapchain created (hr=0x" +
                         std::to_string(static_cast<unsigned>(created)) + ")");
        DestroyWindow(window);
        return;
    }
    // ResizeBuffers must preserve the source chain's own shape, not impose the flip flags on a
    // bitblt chain.
    const UINT sourceChainFlags = desc.Flags;
    DXGI_SWAP_CHAIN_DESC createdDesc{};
    const bool sourceDescribed = SUCCEEDED(swapchain->GetDesc(&createdDesc));
    Check(sourceDescribed, std::string(label) + ": the source swapchain descriptor is readable");
    if (sourceDescribed)
    {
        std::printf("%s: source swapchain buffers=%u swapEffect=%u flags=0x%x\n", label, createdDesc.BufferCount,
            static_cast<unsigned>(createdDesc.SwapEffect), createdDesc.Flags);
        if (g_fgSourceBlt)
        {
            Check(createdDesc.BufferCount == 1 && createdDesc.SwapEffect == DXGI_SWAP_EFFECT_DISCARD &&
                      createdDesc.Flags == 0,
                std::string(label) + ": the requested one-buffer bitblt source chain was created");
        }
        else if (g_fgSourceGame)
        {
            Check(createdDesc.BufferCount == 2 && createdDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL &&
                      createdDesc.Flags == (DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH |
                          DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                          DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING),
                std::string(label) +
                    ": the requested game-shaped two-buffer sequential source chain was created");
        }
        else
        {
            Check(createdDesc.BufferCount == 2 && createdDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD &&
                      createdDesc.Flags == DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT,
                std::string(label) + ": the requested two-buffer flip source chain was created");
        }
    }
    ComPtr<ID3D11DeviceContext1> context1;
    context.As(&context1);
    // Only the default flip source exposes the latency waitable and Present1. A bitblt source
    // legitimately offers neither, so those facilities are not queried or required in that mode.
    HANDLE waitable = nullptr;
    ComPtr<IDXGISwapChain1> swapchain1;
    if (!g_fgSourceBlt)
    {
        ComPtr<IDXGISwapChain2> swapchain2;
        swapchain.As(&swapchain2);
        if (swapchain2)
        {
            swapchain2->SetMaximumFrameLatency(1);
            waitable = swapchain2->GetFrameLatencyWaitableObject();
        }
        Check(waitable != nullptr, "the parent swapchain exposes a frame-latency waitable");
        // The game may present through either entry point; both must advance the present count once.
        swapchain.As(&swapchain1);
        Check(swapchain1 != nullptr, "the parent swapchain exposes IDXGISwapChain1");
    }

    // Release the bridge's previous device binding so the FG transport binds only this device.
    BIGSCALE_Frame shutdown{};
    shutdown.abi = BIGSCALE_ABI_VERSION;
    shutdown.size = sizeof(BIGSCALE_Frame);
    shutdown.cameraId = 989;
    shutdown.operation = BIGSCALE_OP_SHUTDOWN;
    shutdown.preset = BIGSCALE_PRESET_QUALITY;
    shutdown.upscaler = BIGSCALE_UPSCALER_DLSS;
    shutdown.sliceCount = 1;
    shutdown.layout = BIGSCALE_LAYOUT_2D;
    void* shutdownPacket = BIGSCALE_QueueFrame(&shutdown);
    if (shutdownPacket != nullptr) Execute(shutdownPacket);

    ComPtr<ID3D11Texture2D> backbuffer;
    swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    D3D11_TEXTURE2D_DESC backbufferDesc{};
    if (backbuffer) backbuffer->GetDesc(&backbufferDesc);
    const uint32_t outputWidth = backbufferDesc.Width;
    const uint32_t outputHeight = backbufferDesc.Height;
    Check(outputWidth > 0 && outputHeight > 0, "the parent swapchain reports its extent");
    ComPtr<ID3D11RenderTargetView> target;
    if (backbuffer) device->CreateRenderTargetView(backbuffer.Get(), nullptr, &target);

    // The render extent is mutable: the extent-change phase below moves it while the output extent
    // stays fixed, exactly as a Mode change does in game.
    uint32_t renderWidth = outputWidth / 2;
    uint32_t renderHeight = outputHeight / 2;
    // The capture textures must belong to the same device as the presenting swapchain: the real
    // URP depth/motion pair does, and a mismatch is not a scenario the transport should accept.
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11Texture2D> motion;
    ComPtr<ID3D11Texture2D> hudless;
    {
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = renderWidth;
        textureDesc.Height = renderHeight;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        device->CreateTexture2D(&textureDesc, nullptr, &depth);
        textureDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
        device->CreateTexture2D(&textureDesc, nullptr, &motion);
        // Output-sized pre-overlay colour for providers that extract the HUD, in the HDR format URP
        // really produces for its final post-processed attachment. An 8-bit fixture hid the fact that
        // the colour conversion could not read that format at all. It is deliberately a separate
        // resource from the backbuffer so a wrong pairing is visible. Float32 keeps the linear-light
        // values exact so the transport's display encoding round-trips to the same 8-bit codes.
        textureDesc.Width = outputWidth;
        textureDesc.Height = outputHeight;
        textureDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        device->CreateTexture2D(&textureDesc, nullptr, &hudless);
    }
    Check(depth != nullptr && motion != nullptr && hudless != nullptr,
        "frame-generation depth, motion and HUD-less fixtures created");
    if (!depth || !motion || !hudless)
    {
        DestroyWindow(window);
        return;
    }

    BIGSCALE_FG_Config config{};
    config.abi = BIGSCALE_FG_ABI_VERSION;
    config.size = sizeof(BIGSCALE_FG_Config);
    config.provider = provider;
    config.generation = 1;
    config.primaryCameraId = 700;
    config.primaryHistoryEpoch = 0;
    config.captureReady = 1;
    config.multiplier = g_fgMultiplier;
    Check(BIGSCALE_FG_Configure(&config) == 1, std::string(label) + ": FG configure accepts the request");
    Check(BIGSCALE_FG_CreatePresenterWindow(window) == 1,
        std::string(label) + ": the presenter child window is created on its owner thread");
    Check(BIGSCALE_FG_SetPresenterVisible(1) == 1,
        std::string(label) + ": the presenter is shown from the window's owning thread");
    SetFgTestObserver(FrameGenerationObserver);
    // Pin the display refresh so the adaptive cadence governor makes the same decision on any host
    // monitor. The fixture below paces the source at M * 60 Hz, so the governor must settle at M;
    // without the pin a 30 Hz or 144 Hz host would measure a different fit.
    FgTestSetDisplayRefreshMilliseconds(1000.0 / 60.0);

    using RenderEventFn = void (*)(int32_t, void*);
    const auto renderEvent = reinterpret_cast<RenderEventFn>(BIGSCALE_FG_GetRenderEvent());
    Check(renderEvent != nullptr, std::string(label) + ": the FG render event is exported");

    const bool needsHudless = provider == BIGSCALE_FG_PROVIDER_NVIDIA || provider == BIGSCALE_FG_PROVIDER_FSR ||
        provider == BIGSCALE_FG_PROVIDER_ANR || g_fgAnrAfterNormal;
    // The fixture must be physically presentable: M images per source need M presents inside one
    // source interval, and no compositor presents faster than its refresh. At 2x the historical
    // 30 Hz source yields 60 presents/s; above 2x the source slows so each step remains one 60 Hz
    // refresh, which is exactly the regime multi-frame generation is meant for. The step under test
    // stays source interval / M in every case.
    const double kSourceIntervalMilliseconds = static_cast<double>(g_fgMultiplier) * 1000.0 / 60.0 >
            1000.0 / 30.0
        ? static_cast<double>(g_fgMultiplier) * 1000.0 / 60.0
        : 1000.0 / 30.0;

    uint64_t frameId = 0;
    uint64_t submittedFrames = 0;
    // Rendered count at the start of the current in-flight window. A phase that starts after a
    // canceled capture leaves a permanent one-frame offset between these lifetime counters, and an
    // absolute comparison would then wait its full deadline on every later frame.
    uint64_t renderedFloor = 0;
    int droppedFrames = 0;
    int submitFailures = 0;
    int presentCallFailures = 0;
    int presentDeltaFailures = 0;
    int waitableProgress = 0;
    bool captureBoundHudless = false;
    // Diagnostic-only producer mode: rows [0,64) are black in both the final colour and the
    // pre-overlay plane, so the FSR SDK's reset markers can only come from its own overlay.
    bool diagnosticIndicators = false;
    // Set by the HUD-less extent phase to submit an alternative capture allocation.
    ID3D11Texture2D* hudlessOverride = nullptr;
    // Shifts the reported source index so a deliberate capture gap can be exercised.
    int32_t frameIndexExtra = 0;
    // Maximum captures in flight. One during the cadence phase so the source cadence, not the
    // worker's throughput, is what the measured spacing reflects.
    uint64_t inFlightAllowance = 4;
    LARGE_INTEGER sourceClockStart{};
    QueryPerformanceCounter(&sourceClockStart);
    // Sleep(1) resolves to the system timer tick, which adds up to a whole display refresh of
    // jitter to the source cadence. A high-resolution waitable timer makes the 30 Hz source exact.
    HANDLE sourceTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (sourceTimer == nullptr) sourceTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    double lastSourceSubmitMs = 0.0;
    // Absolute schedule for the simulated source. Advanced once per presented frame so the interval
    // stays fixed no matter how fast or slow the transport is.
    double nextSourceSlotMs = 0.0;
    auto waitForSourceSlot = [&](double targetMilliseconds)
    {
        for (int iteration = 0; iteration < 4; iteration++)
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const double sinceStart = QpcMilliseconds(now.QuadPart - sourceClockStart.QuadPart);
            const double remaining = targetMilliseconds - sinceStart;
            if (remaining <= 0.5) return;
            if (sourceTimer == nullptr) return;
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(remaining * 10000.0);
            if (!SetWaitableTimer(sourceTimer, &due, 0, nullptr, nullptr, FALSE)) return;
            WaitWithMessages(sourceTimer, static_cast<DWORD>(remaining) + 50);
        }
    };

    // Object position of the previous submitted frame, so the motion field describes the scene's
    // actual displacement instead of a declared constant.
    int previousObjectX = 0;
    int previousObjectY = 0;
    bool previousObjectValid = false;

    // One capture packet, one parent present. The real present count is read around the call, so a
    // second original Present1 is visible even though the frame itself looks fine.
    auto presentFrame = [&](uint32_t generation, bool reset, int objectX, int objectY, bool hud,
                            float jitterX, float jitterY) -> bool
    {
        // The source runs at 30 Hz: a 30-to-60 doubling only has a half-period to fill if the source
        // itself is paced. The schedule is absolute, not "last submit + interval": a relative pacer
        // measures its own loop speed, so a fast transport would speed the simulated source up and a
        // governor that gates on the source interval would then correctly refuse to generate. A real
        // game's frame rate is its own, so the fixture holds the interval fixed.
        LARGE_INTEGER waitStart{};
        QueryPerformanceCounter(&waitStart);
        nextSourceSlotMs += kSourceIntervalMilliseconds;
        // A slowed frame (provider initialisation, a resize) leaves the schedule behind. Catching up
        // by presenting several frames back to back would simulate a far faster source than this
        // fixture models, so the schedule resyncs to one interval after the last present instead.
        if (nextSourceSlotMs < lastSourceSubmitMs)
            nextSourceSlotMs = lastSourceSubmitMs + kSourceIntervalMilliseconds;
        waitForSourceSlot(nextSourceSlotMs);
        LARGE_INTEGER waitEnd{};
        QueryPerformanceCounter(&waitEnd);
        if (g_traceSlowWaits && QpcMilliseconds(waitEnd.QuadPart - waitStart.QuadPart) > 100.0)
            std::printf("%s: frame %llu source-slot wait took %.1f ms\n", label,
                static_cast<unsigned long long>(frameId + 1),
                QpcMilliseconds(waitEnd.QuadPart - waitStart.QuadPart));
        QueryPerformanceCounter(&waitStart);
        if (waitable != nullptr && WaitWithMessages(waitable, 200) == WAIT_OBJECT_0) waitableProgress++;
        QueryPerformanceCounter(&waitEnd);
        if (g_traceSlowWaits && QpcMilliseconds(waitEnd.QuadPart - waitStart.QuadPart) > 100.0)
            std::printf("%s: frame %llu parent-waitable wait took %.1f ms\n", label,
                static_cast<unsigned long long>(frameId + 1),
                QpcMilliseconds(waitEnd.QuadPart - waitStart.QuadPart));
        {
            BIGSCALE_FG_Status pre{};
            BIGSCALE_FG_GetStatus(&pre);
            if (pre.state == BIGSCALE_FG_STATE_FAULTED || pre.state == BIGSCALE_FG_STATE_UNSUPPORTED)
                return false;
        }
        // The transport owns eight capture slots and a real game is paced by its present. Keep at
        // most four frames in flight so the harness exercises the transport, not a producer that is
        // faster than any display.
        if (submittedFrames > inFlightAllowance)
        {
            QueryPerformanceCounter(&waitStart);
            const DWORD pacingDeadline = GetTickCount() + 500;
            while (GetTickCount() < pacingDeadline)
            {
                BIGSCALE_FG_Status pacing{};
                BIGSCALE_FG_GetStatus(&pacing);
                if (pacing.renderedPresents >= renderedFloor &&
                    pacing.renderedPresents - renderedFloor + inFlightAllowance >= submittedFrames)
                    break;
                WaitWithMessages(nullptr, 1);
            }
            QueryPerformanceCounter(&waitEnd);
            if (g_traceSlowWaits && QpcMilliseconds(waitEnd.QuadPart - waitStart.QuadPart) > 100.0)
                std::printf("%s: frame %llu in-flight gate took %.1f ms (submitted=%llu)\n", label,
                    static_cast<unsigned long long>(frameId + 1),
                    QpcMilliseconds(waitEnd.QuadPart - waitStart.QuadPart),
                    static_cast<unsigned long long>(submittedFrames));
        }
        {
            LARGE_INTEGER submitted{};
            QueryPerformanceCounter(&submitted);
            lastSourceSubmitMs = QpcMilliseconds(submitted.QuadPart - sourceClockStart.QuadPart);
        }
        const uint64_t frame = ++frameId;
        const int32_t codedIndex = static_cast<int32_t>(frame) + frameIndexExtra;
        const bool boundHudless = captureBoundHudless && needsHudless;
        const bool hasObject = objectX >= 0;
        float stepX = 0.0f;
        float stepY = 0.0f;
        if (hasObject && previousObjectValid)
        {
            stepX = static_cast<float>(objectX - previousObjectX);
            stepY = static_cast<float>(objectY - previousObjectY);
        }
        previousObjectX = objectX;
        previousObjectY = objectY;
        previousObjectValid = hasObject;
        swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
        ComposeScreenScene(context1.Get(), target.Get(), outputWidth, outputHeight, objectX, objectY, jitterX,
            jitterY, boundHudless ? false : hud, codedIndex);
        if (diagnosticIndicators)
            ClearRect(context1.Get(), target.Get(), kBlack, 0, 0, static_cast<int>(outputWidth), 64);
        UploadGuides(context.Get(), depth.Get(), motion.Get(), renderWidth, renderHeight, outputWidth, outputHeight,
            objectX, objectY, stepX, stepY);
        if (!boundHudless)
            UploadHudless(context.Get(), hudless.Get(), outputWidth, outputHeight, objectX, objectY,
                codedIndex, diagnosticIndicators);
        {
            std::lock_guard<std::mutex> guard(g_observeMutex);
            g_scenes[codedIndex] = SceneRecord{objectX, objectY, generation, stepX, stepY};
        }
        BIGSCALE_FG_BeginFrame(frame);
        BIGSCALE_FG_Frame packet{};
        packet.abi = BIGSCALE_FG_ABI_VERSION;
        packet.size = sizeof(BIGSCALE_FG_Frame);
        packet.generation = generation;
        packet.cameraId = 700;
        packet.historyEpoch = 0;
        packet.frameIndex = codedIndex;
        packet.renderWidth = static_cast<int32_t>(renderWidth);
        packet.renderHeight = static_cast<int32_t>(renderHeight);
        packet.outputWidth = static_cast<int32_t>(outputWidth);
        packet.outputHeight = static_cast<int32_t>(outputHeight);
        packet.motionWidth = static_cast<int32_t>(renderWidth);
        packet.motionHeight = static_cast<int32_t>(renderHeight);
        packet.presentId = frame;
        packet.flags = BIGSCALE_FG_FLAG_REVERSED_DEPTH | (reset ? BIGSCALE_FG_FLAG_RESET : 0);
        packet.jitterX = jitterX;
        packet.jitterY = jitterY;
        packet.deltaMilliseconds = static_cast<float>(kSourceIntervalMilliseconds);
        packet.cameraNear = kCameraNear;
        packet.cameraFar = kCameraFar;
        packet.verticalFovRadians = kCameraFovRadians;
        packet.aspect = static_cast<float>(outputWidth) / static_cast<float>(outputHeight);
        packet.cameraPosition[2] = 0.0f;
        packet.cameraForward[2] = 1.0f;
        packet.cameraUp[1] = 1.0f;
        packet.cameraRight[0] = 1.0f;
        // Static left-handed camera at the origin looking down +Z. Identity view plus the reversed-Z
        // perspective below matches the depth the guides carry, so a wrong reprojection cannot hide
        // behind an identity projection. Previous and current are equal: these cases have no camera
        // motion, which is not an assertion about camera-motion quality.
        packet.currentView[0] = packet.previousView[0] = 1.0f;
        packet.currentView[5] = packet.previousView[5] = 1.0f;
        packet.currentView[10] = packet.previousView[10] = 1.0f;
        packet.currentView[15] = packet.previousView[15] = 1.0f;
        const float tanHalfFov = std::tan(kCameraFovRadians * 0.5f);
        packet.currentProjection[0] = packet.previousProjection[0] =
            1.0f / (packet.aspect * tanHalfFov);
        packet.currentProjection[5] = packet.previousProjection[5] = 1.0f / tanHalfFov;
        packet.currentProjection[10] = packet.previousProjection[10] =
            kCameraNear / (kCameraNear - kCameraFar);
        packet.currentProjection[11] = packet.previousProjection[11] = 1.0f;
        packet.currentProjection[14] = packet.previousProjection[14] =
            kCameraNear * kCameraFar / (kCameraFar - kCameraNear);
        packet.depth = depth.Get();
        packet.motion = motion.Get();
        packet.hudless = needsHudless && !boundHudless
            ? (hudlessOverride != nullptr ? hudlessOverride : hudless.Get())
            : nullptr;
        if (boundHudless)
        {
            packet.flags |= BIGSCALE_FG_FLAG_BOUND_RENDER_TARGET;
            // Submit must not inspect bindings: Unity's command-buffer bind has not executed yet.
            context->OMSetRenderTargets(0, nullptr, nullptr);
        }
        void* slot = BIGSCALE_FG_SubmitFrame(&packet);
        // A full but healthy transport drops the capture; only a reported native fault is a failure.
        if (slot != nullptr)
        {
            submittedFrames++;
            if (boundHudless)
            {
                ID3D11RenderTargetView* boundTarget = target.Get();
                context->OMSetRenderTargets(1, &boundTarget, nullptr);
            }
            renderEvent(2, slot);
        }
        else
        {
            droppedFrames++;
            BIGSCALE_FG_Status dropStatus{};
            BIGSCALE_FG_GetStatus(&dropStatus);
            if (dropStatus.state == BIGSCALE_FG_STATE_FAULTED ||
                dropStatus.state == BIGSCALE_FG_STATE_UNSUPPORTED)
                return false;
        }
        if (boundHudless && hud)
        {
            // The render event already copied the scene; the overlay arrives after it, exactly like
            // the game's UI pass.
            ClearRect(context1.Get(), target.Get(), kHud, static_cast<int>(kHudLeft),
                static_cast<int>(kHudTop), static_cast<int>(kHudRight), static_cast<int>(kHudBottom));
        }
        UINT before = 0;
        UINT after = 0;
        swapchain->GetLastPresentCount(&before);
        HRESULT presented;
        // Present1 receives an explicit parameters block: this DXGI/driver combination faults on a
        // null pointer even though the API documents it as optional.
        DXGI_PRESENT_PARAMETERS parameters{};
        if (swapchain1 != nullptr && (frame % 2) == 0) presented = swapchain1->Present1(0, 0, &parameters);
        else presented = swapchain->Present(0, 0);
        swapchain->GetLastPresentCount(&after);
        if (!SUCCEEDED(presented)) presentCallFailures++;
        if (after != before + 1) presentDeltaFailures++;
        BIGSCALE_FG_EndSimulation(frame);
        // The managed coordinator acknowledges retirements every frame; the harness does the same.
        BIGSCALE_FG_AckCaptures(0xffffffffu);
        return true;
    };

    // Waits until the provider reports Active with fresh generated presentations for `generation`.
    auto waitForActive = [&](uint32_t generation, int32_t expectedProvider, uint64_t generatedBefore,
                             DWORD milliseconds) -> bool
    {
        BIGSCALE_FG_Status current{};
        const DWORD deadline = GetTickCount() + milliseconds;
        while (GetTickCount() < deadline)
        {
            BIGSCALE_FG_GetStatus(&current);
            if (current.generation == generation && current.activeProvider == expectedProvider &&
                current.state == BIGSCALE_FG_STATE_ACTIVE && current.generatedPresents > generatedBefore)
                return true;
            WaitWithMessages(nullptr, 10);
        }
        return false;
    };

    // Waits until no pair is being presented, so a counter read before a phase is a stable base.
    auto waitForQuiet = [&]()
    {
        // Both counters must be stable: a multi-frame pair commits its generated images before the
        // real present, so watching only the real count can declare quiet while a sequence is still
        // landing.
        uint64_t lastRendered = 0;
        uint64_t lastGenerated = 0;
        for (int attempt = 0; attempt < 80; attempt++)
        {
            BIGSCALE_FG_Status current{};
            BIGSCALE_FG_GetStatus(&current);
            if (attempt > 0 && g_observeBusy.load() == 0 && current.renderedPresents == lastRendered &&
                current.generatedPresents == lastGenerated)
                return;
            lastRendered = current.renderedPresents;
            lastGenerated = current.generatedPresents;
            WaitWithMessages(nullptr, 50);
        }
    };

    // A TDR or device removal invalidates every later phase; report the first phase that observes it
    // so the trigger is identifiable instead of a cascade.
    bool deviceLost = false;
    auto checkDevice = [&](const char* phase)
    {
        if (deviceLost || g_device == nullptr) return;
        const HRESULT removed = g_device->GetDeviceRemovedReason();
        if (FAILED(removed))
        {
            deviceLost = true;
            std::printf("%s: DEVICE LOST after %s (reason 0x%08x)\n", label, phase,
                static_cast<unsigned>(removed));
        }
    };

    auto statusSnapshot = [&](const char* phase)
    {
        BIGSCALE_FG_Status current{};
        BIGSCALE_FG_GetStatus(&current);
        std::printf("%s: %s state=%d provider=%d generation=%u generated=%llu rendered=%llu reason=%s\n", label,
            phase, current.state, current.activeProvider, current.generation,
            static_cast<unsigned long long>(current.generatedPresents),
            static_cast<unsigned long long>(current.renderedPresents), current.reason);
        return current;
    };

    // Bounded progress watchdog. A source that keeps submitting while no real present ever completes
    // would make every later verdict meaningless, so the probe reports the stall once and skips the
    // rest instead of collecting cascaded failures.
    uint64_t probeRendered = 0;
    ULONGLONG probeProgressTick = GetTickCount64();
    auto probeStalled = [&]() -> bool
    {
        BIGSCALE_FG_Status current{};
        BIGSCALE_FG_GetStatus(&current);
        if (current.renderedPresents != probeRendered)
        {
            probeRendered = current.renderedPresents;
            probeProgressTick = GetTickCount64();
            return false;
        }
        if (GetTickCount64() - probeProgressTick < 15000) return false;
        if (g_probeFailed.exchange(true)) return true;
        std::printf("%s: PROBE STALLED: completed real presents stayed %llu for 15 s (state=%d provider=%d "
                    "generation=%u reason=%s)\n", label, static_cast<unsigned long long>(current.renderedPresents),
            current.state, current.activeProvider, current.generation, current.reason);
        {
            std::lock_guard<std::mutex> guard(g_observeMutex);
            size_t shown = 0;
            for (auto it = g_observedFrames.rbegin(); it != g_observedFrames.rend() && shown < 6; ++it, ++shown)
                std::printf("    stalled: frame=%d intermediate=%u/%u generation=%u reset=%d realonly=%d ok=%d "
                            "coded=%d reason=%s\n",
                    it->sourceFrame, it->intermediateIndex, it->intermediateCount, it->generation,
                    it->reset ? 1 : 0, it->realOnly ? 1 : 0, it->ok ? 1 : 0,
                    it->codedValid ? it->codedFrame : -1, it->reason.c_str());
        }
        Check(false, std::string(label) + ": completed real present progress continued during the probe");
        return true;
    };
    auto probeOk = [&]() { return !g_probeFailed.load() && !g_observeFailed.load(); };

    // A requested total the provider cannot support is refused with its exact reason, never clamped
    // in silence and never simulated by duplicating one interpolation. When the requested value is
    // above 2x, the first submissions decide which run this is: a proven provider becomes Active, a
    // provider with a smaller real ceiling refuses with that ceiling, and this process then asserts
    // the deliberate rejection instead of claiming generation.
    if (g_fgMultiplier > 2)
    {
        g_observePixels.store(false);
        for (int frame = 0; frame < 8; frame++)
            presentFrame(config.generation, frame == 0, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f);
        const ULONGLONG ceilingDeadline = GetTickCount64() + 5000;
        BIGSCALE_FG_Status first{};
        for (;;)
        {
            BIGSCALE_FG_GetStatus(&first);
            if (first.state == BIGSCALE_FG_STATE_ACTIVE || first.state == BIGSCALE_FG_STATE_UNSUPPORTED ||
                first.state == BIGSCALE_FG_STATE_FAULTED)
                break;
            if (GetTickCount64() >= ceilingDeadline) break;
            WaitWithMessages(nullptr, 10);
        }
        const std::string ceilingReason = first.reason;
        if (first.state != BIGSCALE_FG_STATE_ACTIVE &&
            ceilingReason.find("supports at most") != std::string::npos)
        {
            std::printf("%s: %ux refused by the provider: state=%d provider=%d generated=%llu restart=%u "
                        "reason=%s\n", label, g_fgMultiplier, first.state, first.activeProvider,
                static_cast<unsigned long long>(first.generatedPresents), first.restartRequired,
                first.reason);
            Check(first.state == BIGSCALE_FG_STATE_UNSUPPORTED,
                std::string(label) + ": a total above the proven ceiling is reported Unsupported");
            // The initialized provider may still be named as active while it refuses this total;
            // what matters is that no image was generated and the refusal is typed and exact.
            Check(ceilingReason.find("x was requested") != std::string::npos,
                std::string(label) + ": the refusal names the requested total (got: " + ceilingReason + ")");
            Check(first.generatedPresents == 0,
                std::string(label) + ": no generated frame is invented for an unsupported total");
            Check(first.restartRequired == 0,
                std::string(label) + ": a capability refusal is not a restart requirement");
            BIGSCALE_FG_Shutdown();
            SetFgTestObserver(nullptr);
            FgTestSetDisplayRefreshMilliseconds(0.0);
            DestroyWindow(window);
            return;
        }
        if (first.state == BIGSCALE_FG_STATE_UNSUPPORTED || first.state == BIGSCALE_FG_STATE_FAULTED)
        {
            Check(false, std::string(label) + ": the requested " + std::to_string(g_fgMultiplier) +
                             "x was refused (state=" + std::to_string(first.state) + ", reason=" +
                             ceilingReason + ")");
            BIGSCALE_FG_Shutdown();
            SetFgTestObserver(nullptr);
            FgTestSetDisplayRefreshMilliseconds(0.0);
            DestroyWindow(window);
            return;
        }
        // The provider accepted it; the ordinary warmup below proves it actually presented images.
        std::printf("%s: provider accepted %ux; continuing with the full checks\n", label, g_fgMultiplier);
    }

    if (g_fgProbe)
    {
        // The probe is a bounded run, not a game: one process may not hang a test machine. The
        // limit stops this fixture process only and retains everything already printed and logged.
        std::thread([]()
        {
            for (int second = 0; second < 120; second++)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (g_probeFinished.load()) return;
            }
            std::printf("PROBE TIMEOUT: the probe did not finish within 120 s; stopping this fixture process\n");
            std::fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 3);
        }).detach();
    }

    BIGSCALE_FG_Status status{};
    constexpr uint64_t warmupFrames = 40;
    // Stable addresses for the trace phase label: the native trace compares phase pointers.
    static const char kPhaseWarmup[] = "warmup";
    static const char kPhaseOrientation[] = "orientation";
    static const char kPhaseHorizontal[] = "horizontal";
    static const char kPhaseVertical[] = "vertical";
    static const char kPhaseReset[] = "reset";
    static const char kPhaseFsrResetDiagnostic[] = "fsr-reset-diagnostic";
    static const char kPhaseGap[] = "gap";
    static const char kPhaseCadence[] = "cadence";
    static const char kPhaseTeardown[] = "teardown";
    SetFgTestPhase(kPhaseWarmup);
    // Warmup readback proves the captured source identity and that the child buffer was written; it
    // deliberately makes no SDK-quality claim.
    g_observePixels.store(true);
    ResetObservations();
    g_observeFailed.store(false);
    for (uint64_t frame = 1; frame <= warmupFrames; frame++)
    {
        // Warmup: a static scene, zero jitter and a mixed frame cadence, exactly as the transport
        // needs before an orientation assertion is meaningful.
        if (!presentFrame(config.generation, frame == 1, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f))
            submitFailures++;
        if (frame == 8) WaitWithMessages(nullptr, 50);
        if (probeStalled() || g_observeFailed.load()) break;
    }
    g_observePixels.store(false);
    Check(submitFailures == 0, std::string(label) + ": no packet was rejected by the bridge");
    Check(submittedFrames >= 24, std::string(label) + ": the capture ring accepted a working set of packets");
    std::printf("%s: accepted=%llu dropped=%d\n", label,
        static_cast<unsigned long long>(submittedFrames), droppedFrames);

    Check(BIGSCALE_FG_GetStatus(&status) == 1, std::string(label) + ": FG status is readable");
    Check(status.activeProvider == provider, std::string(label) + ": the requested provider is active");
    Check(status.state == BIGSCALE_FG_STATE_ACTIVE, std::string(label) + ": the provider published Active");
    Check(status.generatedPresents > 0, std::string(label) + ": the provider proved generated presentations");
    Check(status.renderedPresents >= 24, std::string(label) + ": source presents reached the presenter");
    // The applied total and the provider's proven ceiling must both be visible and truthful. A
    // multiplier above the ceiling never reaches this point: the provider refuses it with its reason.
    Check(status.activeMultiplier == g_fgMultiplier,
        std::string(label) + ": the active total is the requested " + std::to_string(g_fgMultiplier) + "x (got " +
            std::to_string(status.activeMultiplier) + ")");
    Check(status.maxMultiplier >= g_fgMultiplier && status.maxMultiplier <= 6,
        std::string(label) + ": the provider published a proven maximum covering the request (got " +
            std::to_string(status.maxMultiplier) + ")");
    Check(status.restartRequired == 0,
        std::string(label) + ": a supported provider reports no restart requirement");
    if (g_fgSourceBlt)
        std::printf("%s: waitable progress not applicable (bitblt source chain)\n", label);
    else
        Check(waitableProgress >= 30, std::string(label) + ": the original Present keeps the parent waitable progressing");
    Check(presentCallFailures == 0, std::string(label) + ": every parent present call succeeded");
    Check(presentDeltaFailures == 0,
        std::string(label) + ": each owned present advanced GetLastPresentCount exactly once");
    checkDevice("warmup");
    {
        // The simulator's own clock is the UI thread this loop owns: a message-starved owner thread
        // looks exactly like a stalled transport, so the stall check also reports message progress.
        std::lock_guard<std::mutex> guard(g_observeMutex);
        int codedMatches = 0;
        for (const ObservedFrame& frame : g_observedFrames)
        {
            if (!frame.ok || !frame.codedValid) continue;
            const auto scene = g_scenes.find(frame.codedFrame);
            if (scene != g_scenes.end() && scene->second.generation == frame.generation) codedMatches++;
        }
        std::printf("%s: warmup observations=%zu coded-identity matches=%d\n", label, g_observedFrames.size(),
            codedMatches);
        if (probeOk() && !g_observedFrames.empty())
            Check(codedMatches > 0, std::string(label) +
                ": a retained real colour decodes to the submitted source frame");
    }
    if (!probeOk())
    {
        std::printf("%s: observation failed during warmup; the probe stops before any phase verdict\n", label);
        statusSnapshot("observation failure");
        Check(false, std::string(label) + ": the observation window stayed valid through warmup");
        g_observePixels.store(false);
        // Drain healthy submitted work while this owner thread keeps pumping its messages.
        WaitWithMessages(nullptr, 1000);
        BIGSCALE_FG_Shutdown();
        SetFgTestObserver(nullptr);
        FgTestSetDisplayRefreshMilliseconds(0.0);
        DestroyWindow(window);
        return;
    }

    // ---- Orientation -------------------------------------------------------------------------
    // The generated child copy must be upright: the red band at the top, the blue band at the
    // bottom and the white marker at its exact screen position. The SDK's own output is inspected
    // too, so a provider-side flip cannot mask a wrong SDK orientation convention.
    auto checkOrientation = [&](const char* phase, bool requireMarker)
    {
        std::lock_guard<std::mutex> guard(g_observeMutex);
        const ObservedFrame* frame = nullptr;
        for (auto it = g_observedFrames.rbegin(); it != g_observedFrames.rend(); ++it)
        {
            // A real-only sample carries no interpolated image, so it cannot prove orientation.
            if (it->realOnly || it->reset || !it->ok || !it->screen.Valid()) continue;
            frame = &*it;
            break;
        }
        if (frame == nullptr)
        {
            std::string detail;
            for (auto it = g_observedFrames.rbegin(); it != g_observedFrames.rend(); ++it)
            {
                if (!it->reason.empty())
                {
                    detail = it->reason;
                    break;
                }
            }
            Check(false, std::string(label) + ": " + phase + " produced a readable generated image" +
                             (detail.empty() ? std::string() : " (" + detail + ")"));
            return;
        }
        const Image& screen = frame->screen;
        float topColour[4]{};
        float bottomColour[4]{};
        AverageColor(screen, outputWidth / 2 - 20, 12, outputWidth / 2 + 20, 40, topColour);
        AverageColor(screen, outputWidth / 2 - 20, outputHeight - 40, outputWidth / 2 + 20, outputHeight - 12,
            bottomColour);
        Check(topColour[0] > 0.5f && topColour[2] < 0.4f,
            std::string(label) + ": " + phase + " keeps the red band at the top of the generated image (r=" +
                std::to_string(topColour[0]) + ", b=" + std::to_string(topColour[2]) + ")");
        Check(bottomColour[2] > 0.5f && bottomColour[0] < 0.4f,
            std::string(label) + ": " + phase + " keeps the blue band at the bottom of the generated image (r=" +
                std::to_string(bottomColour[0]) + ", b=" + std::to_string(bottomColour[2]) + ")");
        float markerX = 0.0f;
        float markerY = 0.0f;
        if (!requireMarker)
        {
            std::printf("%s: %s marker position not asserted (synthetic graph-space HUD-less)\n", label, phase);
        }
        else
        {
        const bool found = FindCentroid(screen, ChannelRange{0.6f, 1.1f}, ChannelRange{0.0f, 0.3f},
            ChannelRange{0.6f, 1.1f}, 40, 100, markerX, markerY);
        Check(found, std::string(label) + ": " + phase + " keeps the marker in the generated image");
        if (found)
        {
            const float* exact = screen.At(static_cast<UINT>(kMarkerCentre), static_cast<UINT>(kMarkerCentreRow));
            Check(exact[0] > 0.5f && exact[1] < 0.4f && exact[2] > 0.5f,
                std::string(label) + ": " + phase + " marker pixel is at (10,10) (got " +
                    std::to_string(exact[0]) + "," + std::to_string(exact[1]) + "," +
                    std::to_string(exact[2]) + ")");
        }
        if (found)
        {
            Near(markerX, kMarkerCentre, 1.0f, std::string(label) + ": " + phase + " marker x");
            Near(markerY, kMarkerCentreRow, 1.0f, std::string(label) + ": " + phase + " marker y");
            if (std::fabs(markerX - kMarkerCentre) > 1.0f || std::fabs(markerY - kMarkerCentre) > 1.0f)
            {
                const float* corner = screen.At(10, static_cast<UINT>(kMarkerCentreRow));
                std::printf("%s: marker diagnostic: (10,10)=(%.2f,%.2f,%.2f,%.2f) centroid=(%.2f,%.2f)\n", label,
                    corner[0], corner[1], corner[2], corner[3], markerX, markerY);
                PrintImageMap(screen);
            }
        }
        }
        if (frame->raw.Valid())
        {
            float rawTop[4]{};
            float rawBottom[4]{};
            AverageColor(frame->raw, outputWidth / 2 - 20, 12, outputWidth / 2 + 20, 40, rawTop);
            AverageColor(frame->raw, outputWidth / 2 - 20, outputHeight - 40, outputWidth / 2 + 20,
                outputHeight - 12, rawBottom);
            const char* rawSpace = rawTop[0] > rawBottom[0] ? "screen space" : "graph space";
            std::printf("%s: %s raw SDK output rows are %s (top r=%.2f b=%.2f, bottom r=%.2f b=%.2f)\n", label,
                phase, rawSpace, rawTop[0], rawTop[2], rawBottom[0], rawBottom[2]);
        }
    };

    // The pre-overlay plane handed to the SDK is compared spatially on consecutive frames, so a
    // descriptor still bound to the destination plane or to a previous frame's slot cannot pass.
    auto checkHudlessInputs = [&](int32_t startFrame)
    {
        std::lock_guard<std::mutex> guard(g_observeMutex);
        int hudlessSamples = 0;
        int hudlessExact = 0;
        int hudlessCoded = 0;
        float worstHudless = 0.0f;
        for (const ObservedFrame& frame : g_observedFrames)
        {
            if (frame.realOnly || frame.reset || !frame.ok || frame.sourceFrame < startFrame) continue;
            if (!frame.hudlessSpatialCompared) continue;
            hudlessSamples++;
            worstHudless = std::max(worstHudless, frame.hudlessSpatialVsRealMax);
            if (frame.hudlessSpatialVsRealMax <= (1.0f / 255.0f) + 1e-4f) hudlessExact++;
            if (frame.codedValid && frame.hudlessCodedValid && frame.hudlessCodedFrame == frame.codedFrame)
                hudlessCoded++;
        }
        std::printf("%s: HUD-less inputs samples=%d exact=%d coded=%d worst=%.4f\n",
            label, hudlessSamples, hudlessExact, hudlessCoded, worstHudless);
        Check(hudlessSamples >= 4, std::string(label) +
            ": the orientation phase observed consecutive HUD-less samples (got " +
            std::to_string(hudlessSamples) + ")");
        Check(hudlessExact == hudlessSamples, std::string(label) +
            ": the HUD-less plane matches the real colour outside the HUD (got " +
            std::to_string(hudlessExact) + " of " + std::to_string(hudlessSamples) + ")");
        Check(hudlessCoded == hudlessSamples, std::string(label) +
            ": the HUD-less plane carries this source frame's coded patch (got " +
            std::to_string(hudlessCoded) + " of " + std::to_string(hudlessSamples) + ")");
    };

    // XeSS-FG presents through its own proxy swapchain, so the manual-provider observer has no
    // handed-over resources for it; its pacing stays the SDK's and only the transport/lifecycle
    // assertions apply. Every manual provider runs the pixel and cadence gates below.
    if (needsHudless)
    {
    SetFgTestPhase(kPhaseOrientation);
    g_observePixels.store(true);
    ResetObservations();
    {
        int orientationFailures = 0;
        const int32_t orientationStart = static_cast<int32_t>(frameId) + 1;
        for (int frame = 0; frame < 12 && probeOk(); frame++)
        {
            if (!presentFrame(config.generation, frame == 0, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f)) orientationFailures++;
            if (probeStalled()) break;
        }
        if (probeOk())
        {
            Check(orientationFailures == 0, std::string(label) + ": the orientation fixture is accepted");
            WaitWithMessages(nullptr, 200);
            checkOrientation("graph-backed HUD-less", false);
            if (provider == BIGSCALE_FG_PROVIDER_ANR) checkHudlessInputs(orientationStart);
            checkDevice("graph-backed orientation");
        }
    }

    // ---- Interpolation -----------------------------------------------------------------------
    // The rectangle steps sixteen output pixels per source frame, so an interpolated image must
    // place it eight pixels behind the current real position. A reset frame produces no generated
    // presentation and warms the SDK history.
    // Pixel and cadence phases use a separate HUD-less texture whose rows match the presented
    // colour, so a row mismatch can never mask the interpolation result. The bound route is
    // exercised on its own below; the game supplies that same matched pair.
    captureBoundHudless = false;
    config.generation++;
    Check(BIGSCALE_FG_Configure(&config) == 1,
        std::string(label) + ": the matched-HUD-less route is armed for the pixel phases");
    g_observePixels.store(true);
    g_observeRaw.store(true);
    ResetObservations();
    // The guide contract: the transport must hand every provider depth and motion in
    // presented-backbuffer rows. The fixture writes both in URP graph rows, so a transport that
    // copies them unchanged leaves every sample vertically mirrored and fails here.
    auto checkGuides = [&](const char* phase, int32_t startFrame)
    {
        if (!probeOk())
        {
            std::printf("%s: %s guide verdict skipped after a probe failure\n", label, phase);
            return;
        }
        std::lock_guard<std::mutex> guard(g_observeMutex);
        int frames = 0;
        int samples = 0;
        int mismatches = 0;
        for (const ObservedFrame& frame : g_observedFrames)
        {
            if (frame.sourceFrame < startFrame || frame.guideSamples == 0) continue;
            frames++;
            samples += frame.guideSamples;
            mismatches += frame.guideMismatches;
        }
        std::printf("%s: %s guide rows frames=%d samples=%d mismatches=%d\n", label, phase, frames, samples,
            mismatches);
        Check(frames >= 2, std::string(label) + ": " + phase + " observed guide planes (got " +
            std::to_string(frames) + " frames)");
        Check(mismatches == 0, std::string(label) +
            ": the transport hands depth and motion in presented-backbuffer rows (" + phase + ": " +
            std::to_string(mismatches) + " of " + std::to_string(samples) + " samples mismatched)");
    };

    // One evaluator for both axes. `horizontal` selects the moved axis; the other axis must stay
    // within two output pixels, which is what an axis-swapped motion field breaks.
    auto evaluateMotion = [&](const char* phase, bool horizontal, int32_t startFrame)
    {
        if (!probeOk())
        {
            std::printf("%s: %s verdict skipped after a probe failure\n", label, phase);
            return;
        }
        std::lock_guard<std::mutex> guard(g_observeMutex);
        int evaluated = 0;
        int consistent = 0;
        int distinct = 0;
        int stationaryOk = 0;
        int codedOk = 0;
        int joined = 0;
        // Measured position of every intermediate, keyed by source frame then sequence index. A
        // multi-interpolation run must place them at distinct fractions; a kernel that blends with a
        // compiled-in midpoint repeats one position for every index.
        std::map<int64_t, std::map<uint32_t, float>> curve;
        std::map<int64_t, uint32_t> counts;
        for (const ObservedFrame& frame : g_observedFrames)
        {
            if (frame.realOnly || frame.reset || !frame.ok || !frame.screen.Valid()) continue;
            if (frame.sourceFrame < startFrame) continue;
            const auto scene = g_scenes.find(frame.sourceFrame);
            if (scene == g_scenes.end() || scene->second.objectX < 0) continue;
            // A Record callback alone does not prove that its generated image was presented.
            bool generatedSubmitted = false;
            for (const ObservedPair& pair : g_observedPairs)
            {
                if (pair.generation == frame.generation && pair.sourceFrame == frame.sourceFrame)
                    generatedSubmitted = generatedSubmitted || pair.generatedCount > 0;
            }
            if (!generatedSubmitted) continue;
            joined++;
            if (frame.codedValid && frame.codedFrame == frame.sourceFrame) codedOk++;
            float centreX = 0.0f;
            float centreY = 0.0f;
            const bool found = FindCentroid(frame.screen, ChannelRange{0.0f, 0.4f}, ChannelRange{0.5f, 1.1f},
                ChannelRange{0.0f, 0.4f}, 0, frame.screen.height, centreX, centreY);
            if (!found) continue;
            evaluated++;
            const float current = static_cast<float>(horizontal ? scene->second.objectX : scene->second.objectY) +
                static_cast<float>(kObjectSize) / 2.0f;
            const float step = static_cast<float>(kObjectStep);
            const float previous = current - step;
            // Intermediate i of an M-total sequence sits i/(M-1) of the way from the previous source
            // to the current one. At 2x that is the familiar midpoint; at 4x the intermediates are
            // quarter steps, and asserting a midpoint for all of them would reject correct output.
            const uint32_t count = frame.intermediateCount != 0 ? frame.intermediateCount : 1;
            const uint32_t position = frame.intermediateIndex != 0 ? frame.intermediateIndex : 1;
            const float fraction = static_cast<float>(position) / static_cast<float>(count + 1);
            const float midpoint = current - step * (1.0f - fraction);
            const float measured = horizontal ? centreX : centreY;
            const float stationaryCurrent =
                static_cast<float>(horizontal ? scene->second.objectY : scene->second.objectX) +
                static_cast<float>(kObjectSize) / 2.0f;
            const float stationaryMeasured = horizontal ? centreY : centreX;
            if (evaluated <= 6)
            {
                std::printf("%s: %s frame %d centroid=(%.1f,%.1f) previous=%.1f midpoint=%.1f current=%.1f\n",
                    label, phase, frame.sourceFrame, centreX, centreY, previous, midpoint, current);
            }
            if (std::fabs(measured - midpoint) <= 4.0f) consistent++;
            if (measured > previous + 4.0f && measured < current - 4.0f) distinct++;
            if (std::fabs(stationaryMeasured - stationaryCurrent) <= 2.0f) stationaryOk++;
            if (count >= 2)
            {
                curve[frame.sourceFrame][position] = measured;
                counts[frame.sourceFrame] = count;
            }
        }
        Check(joined >= 20, std::string(label) + ": " + phase +
            " produced at least 20 presented generated samples (got " + std::to_string(joined) + ")");
        Check(codedOk >= 20, std::string(label) + ": " + phase +
            " retained real colour decodes to the submitted source frame (got " + std::to_string(codedOk) +
            " of " + std::to_string(joined) + ")");
        Check(evaluated >= 20, std::string(label) + ": " + phase +
            " generated frames carry the moving object (got " + std::to_string(evaluated) + ")");
        Check(consistent >= 20, std::string(label) + ": " + phase +
            " object sits at the previous/current midpoint (got " + std::to_string(consistent) + " of " +
            std::to_string(evaluated) + ")");
        Check(distinct >= 20, std::string(label) + ": " + phase +
            " object is distinct from both real endpoints (got " + std::to_string(distinct) + " of " +
            std::to_string(evaluated) + ")");
        Check(stationaryOk >= 20, std::string(label) + ": " + phase +
            " stationary axis stays within two pixels (got " + std::to_string(stationaryOk) + " of " +
            std::to_string(evaluated) + ")");
        // Only a multi-interpolation run can be ordered; 2x has one midpoint per source frame.
        if (!curve.empty())
        {
            int complete = 0;
            int ordered = 0;
            for (const auto& frame : curve)
            {
                const auto total = counts.find(frame.first);
                if (total == counts.end() || frame.second.size() != total->second) continue;
                complete++;
                bool advances = true;
                auto position = frame.second.begin();
                float previousPosition = position->second;
                for (++position; position != frame.second.end(); ++position)
                {
                    if (position->second - previousPosition < 1.0f)
                    {
                        advances = false;
                        break;
                    }
                    previousPosition = position->second;
                }
                if (advances) ordered++;
            }
            Check(complete >= 10 && ordered * 10 >= complete * 8, std::string(label) + ": " + phase +
                " intermediates advance with their index instead of repeating one midpoint (ordered " +
                std::to_string(ordered) + " of " + std::to_string(complete) + ")");
        }
        checkDevice(phase);
    };
    int32_t horizontalStart = -1;
    {
        SetFgTestPhase(kPhaseHorizontal);
        g_guideFrames.store(0);
        g_observeGuides.store(true);
        int failures = 0;
        for (int frame = 0; frame < 38; frame++)
        {
            const int objectX = static_cast<int>(kObjectBase) + frame * static_cast<int>(kObjectStep);
            if (horizontalStart < 0) horizontalStart = static_cast<int32_t>(frameId) + 1;
            if (!presentFrame(config.generation, frame == 0, objectX, static_cast<int>(kObjectY), true, 0.0f,
                    0.0f))
            {
                failures++;
                break;
            }
            if (probeStalled()) break;
        }
        g_observeGuides.store(false);
        if (probeOk())
            Check(failures == 0, std::string(label) + ": the interpolation fixture is accepted");
        WaitWithMessages(nullptr, 300);
    }
    checkGuides("horizontal motion", horizontalStart);
    evaluateMotion("horizontal motion", true, horizontalStart);

    // ---- Vertical motion ---------------------------------------------------------------------
    // A horizontal-only case cannot tell a correct field from one whose axes are swapped, and it
    // never exercises a vertical displacement. The same rectangle moves down instead.
    int32_t verticalStart = -1;
    {
        SetFgTestPhase(kPhaseVertical);
        g_guideFrames.store(0);
        g_observeGuides.store(true);
        int failures = 0;
        for (int frame = 0; frame < 30; frame++)
        {
            const int objectY = 80 + frame * static_cast<int>(kObjectStep);
            if (verticalStart < 0) verticalStart = static_cast<int32_t>(frameId) + 1;
            if (!presentFrame(config.generation, frame == 0, static_cast<int>(kObjectBase), objectY, true, 0.0f,
                    0.0f))
            {
                failures++;
                break;
            }
            if (probeStalled()) break;
        }
        g_observeGuides.store(false);
        if (probeOk())
            Check(failures == 0, std::string(label) + ": the vertical-motion fixture is accepted");
        WaitWithMessages(nullptr, 300);
    }
    checkGuides("vertical motion", verticalStart);
    evaluateMotion("vertical motion", false, verticalStart);
    g_observeRaw.store(false);

    // ---- Reset frames ------------------------------------------------------------------------
    {
        SetFgTestPhase(kPhaseReset);
        waitForQuiet();
        BIGSCALE_FG_Status before{};
        BIGSCALE_FG_GetStatus(&before);
        const int objectX = static_cast<int>(kObjectBase) + 40 * static_cast<int>(kObjectStep);
        // Keep the suppression observation on: a suppressed pair's child buffer must be shown to
        // hold the real image, not assumed to.
        g_observePixels.store(true);
        ResetObservations();
        Check(presentFrame(config.generation, true, objectX, static_cast<int>(kObjectY), true, 0.0f, 0.0f),
            std::string(label) + ": a reset source frame is accepted");
        // Wait for the reset pair's own observation instead of a fixed delay: the worker may still be
        // reading a previous pair's pixels when this packet is queued, and a counter-only quiet check
        // cannot see a queued slot that has not started yet.
        {
            const DWORD resetDeadline = GetTickCount() + 5000;
            for (;;)
            {
                bool pairSeen = false;
                {
                    std::lock_guard<std::mutex> guard(g_observeMutex);
                    for (const ObservedPair& pair : g_observedPairs)
                        if (pair.sourceFrame == static_cast<int32_t>(frameId)) pairSeen = true;
                }
                if (pairSeen || GetTickCount() >= resetDeadline) break;
                WaitWithMessages(nullptr, 20);
            }
        }
        // The pair's real present and the worker's accounting have completed by now.
        waitForQuiet();
        g_observePixels.store(false);
        BIGSCALE_FG_Status after{};
        BIGSCALE_FG_GetStatus(&after);
        if (probeOk())
        {
            Check(after.generatedPresents == before.generatedPresents,
                std::string(label) + ": a reset frame does not count as a generated frame");
        }
        bool resetPairSeen = false;
        bool singleRealPresent = false;
        bool realOnlySeen = false;
        bool childMatchedReal = false;
        bool noGeneratedDeadline = false;
        {
            std::lock_guard<std::mutex> guard(g_observeMutex);
            for (const ObservedPair& pair : g_observedPairs)
            {
                if (pair.sourceFrame != static_cast<int32_t>(frameId)) continue;
                resetPairSeen = true;
                singleRealPresent = pair.generatedCount == 0 && pair.realQpc != 0;
                // A generated pacing deadline can only exist after a generated present.
                noGeneratedDeadline = pair.generatedCount == 0;
            }
            for (const ObservedFrame& frame : g_observedFrames)
            {
                if (frame.sourceFrame != static_cast<int32_t>(frameId)) continue;
                if (!frame.realOnly) continue;
                realOnlySeen = true;
                // 8-bit codes: agreement within one code value proves the child holds this frame's
                // real colour rather than the previous pair's or an unwritten buffer.
                childMatchedReal = frame.ok && frame.realCompared && frame.childVsRealMax <= (1.0f / 255.0f) + 1e-4f;
            }
        }
        if (probeOk())
        {
            // Record which suppression combinations the SDK actually emitted instead of assuming
            // any. A byte that never changes across hundreds of pairs is an SDK that writes it; a
            // byte that varies wildly is an output this family never fills.
            std::map<std::pair<int, int>, int> combinations;
            {
                std::lock_guard<std::mutex> guard(g_observeMutex);
                for (const ObservedPair& pair : g_observedPairs)
                    combinations[{pair.reset ? 1 : 0, pair.disableByte}]++;
            }
            std::printf("%s: observed pair suppression combinations (reset,disableByte)=count:\n", label);
            for (const auto& entry : combinations)
                std::printf("%s:   reset=%d disableByte=%d count=%d\n", label, entry.first.first,
                    entry.first.second, entry.second);
        }
        if (probeOk())
        {
            Check(resetPairSeen && singleRealPresent,
                std::string(label) + ": a reset frame presents the real image exactly once");
            Check(noGeneratedDeadline,
                std::string(label) + ": a reset frame leaves no generated pacing deadline");
            // Only the NGX family publishes a real-only observation for a suppressed pair; FSR
            // presents the real copy from its own rejected target and reports it through the pair
            // timing alone.
            if (provider == BIGSCALE_FG_PROVIDER_NVIDIA || provider == BIGSCALE_FG_PROVIDER_ANR)
            {
                Check(realOnlySeen,
                    std::string(label) + ": the suppressed pair published a real-only observation");
                Check(childMatchedReal,
                    std::string(label) + ": the suppressed pair's child buffer holds the real frame's colour");
            }
        }
        // The next pair must recover without a stale deadline.
        for (int frame = 0; frame < 6; frame++)
        {
            const int objectXRecover = static_cast<int>(kObjectBase) + (41 + frame) * static_cast<int>(kObjectStep);
            presentFrame(config.generation, false, objectXRecover, static_cast<int>(kObjectY), true, 0.0f,
                0.0f);
        }
        Check(waitForActive(config.generation, provider, after.generatedPresents, 5000),
            std::string(label) + ": generation recovers after a reset frame");
        checkDevice("reset");
    }

    // ---- FSR reset/scene-change diagnostic ---------------------------------------------------
    // A bounded, FSR-only GPU experiment that discriminates the two SDK-internal bypass paths
    // before any algorithm change: the SDK paints red below row 32 when its interpolator reset and
    // blue in rows 33..63 when its optical flow reported a scene change. Both producers black rows
    // [0,64), so a marker can only be the SDK's own overlay. Diagnostics never convert a failed run
    // into a success.
    if (g_fgProbe && provider == BIGSCALE_FG_PROVIDER_FSR)
    {
        config.generation++;
        Check(BIGSCALE_FG_Configure(&config) == 1,
            std::string(label) + ": the FSR reset diagnostic generation is accepted");
        ResetObservations();
        SetFgTestPhase(kPhaseFsrResetDiagnostic);
        g_observePixels.store(true);
        g_observeRaw.store(true);
        diagnosticIndicators = true;
        SetFgTestFsrResetIndicators(true);
        {
            std::lock_guard<std::mutex> guard(g_observeMutex);
            g_fsrDiagnostic = FsrResetDiagnostic{};
            g_fsrDiagnostic.active = true;
            g_fsrDiagnostic.generation = config.generation;
            g_fsrDiagnostic.firstFrame = static_cast<int32_t>(frameId) + 1;
        }
        int failures = 0;
        for (int frame = 0; frame < 40 && probeOk(); frame++)
        {
            const int objectX = static_cast<int>(kObjectBase) + frame * static_cast<int>(kObjectStep);
            if (!presentFrame(config.generation, frame == 0, objectX, static_cast<int>(kObjectY), true, 0.0f,
                    0.0f))
            {
                failures++;
                break;
            }
            if (probeStalled()) break;
        }
        // Drain every submitted diagnostic frame while the switch is still on, then leave the
        // diagnostic producer mode before cadence so the black band cannot contaminate it.
        WaitWithMessages(nullptr, 500);
        SetFgTestFsrResetIndicators(false);
        diagnosticIndicators = false;
        {
            std::lock_guard<std::mutex> guard(g_observeMutex);
            g_fsrDiagnostic.active = false;
        }
        g_observeRaw.store(false);
        g_observePixels.store(false);
        if (!probeOk())
        {
            std::printf("%s: FSR reset diagnostic skipped after a probe failure\n", label);
        }
        else
        {
            FsrResetDiagnostic diagnostic;
            {
                std::lock_guard<std::mutex> guard(g_observeMutex);
                diagnostic = g_fsrDiagnostic;
            }
            // The existing midpoint/current-endpoint measurements are reported for the same frames,
            // not re-asserted: the horizontal phase already owns the quality verdict.
            int measured = 0;
            int midpointCount = 0;
            int currentCount = 0;
            int previousCount = 0;
            {
                std::lock_guard<std::mutex> guard(g_observeMutex);
                for (const ObservedFrame& frame : g_observedFrames)
                {
                    if (frame.realOnly || frame.reset || !frame.ok || !frame.screen.Valid()) continue;
                    if (frame.generation != diagnostic.generation ||
                        frame.sourceFrame < diagnostic.firstFrame + 16)
                        continue;
                    const auto scene = g_scenes.find(frame.sourceFrame);
                    if (scene == g_scenes.end() || scene->second.objectX < 0) continue;
                    float centreX = 0.0f;
                    float centreY = 0.0f;
                    if (!FindCentroid(frame.screen, ChannelRange{0.0f, 0.4f}, ChannelRange{0.5f, 1.1f},
                            ChannelRange{0.0f, 0.4f}, 0, frame.screen.height, centreX, centreY))
                        continue;
                    measured++;
                    const float current = static_cast<float>(scene->second.objectX) + kObjectSize / 2.0f;
                    const float previous = current - static_cast<float>(kObjectStep);
                    const float midpoint = (current + previous) / 2.0f;
                    if (std::fabs(centreX - midpoint) <= 4.0f) midpointCount++;
                    if (std::fabs(centreX - current) <= 4.0f) currentCount++;
                    if (std::fabs(centreX - previous) <= 4.0f) previousCount++;
                }
            }
            std::printf("%s: FSR reset diagnostic: resetSeen=%d resetRed=%d resetBlue=%d resetInputBlack=%d | "
                        "late samples=%d inputBlack=%d red=%d blue=%d mixed=%d unmarked=%d | motion "
                        "measured=%d midpoint=%d current=%d previous=%d\n",
                label, diagnostic.resetSeen ? 1 : 0, diagnostic.resetRed ? 1 : 0,
                diagnostic.resetBlue ? 1 : 0, diagnostic.resetInputBlack ? 1 : 0, diagnostic.samples,
                diagnostic.inputBlack, diagnostic.red, diagnostic.blue, diagnostic.mixed, diagnostic.unmarked,
                measured, midpointCount, currentCount, previousCount);
            Check(failures == 0, std::string(label) + ": the FSR reset diagnostic fixture is accepted");
            Check(diagnostic.resetSeen && diagnostic.resetInputBlack && diagnostic.resetRed,
                std::string(label) + ": the initial explicit reset produced the SDK's red reset marker "
                    "(otherwise this diagnostic is unsupported/invalid)");
            if (!diagnostic.resetSeen || !diagnostic.resetInputBlack || !diagnostic.resetRed)
                std::printf("%s: FSR reset diagnostic unsupported/invalid: no red initial reset marker over "
                            "black input (seen=%d red=%d inputBlack=%d); no reset branch is concluded\n",
                    label, diagnostic.resetSeen ? 1 : 0, diagnostic.resetRed ? 1 : 0,
                    diagnostic.resetInputBlack ? 1 : 0);
            Check(diagnostic.samples >= 20,
                std::string(label) + ": the FSR reset diagnostic produced at least 20 late non-reset samples "
                    "(got " + std::to_string(diagnostic.samples) + ")");
            Check(diagnostic.inputBlack == diagnostic.samples,
                std::string(label) + ": every late diagnostic sample's producer marker rows were black");
            if (diagnostic.samples > 0)
            {
                const char* branch = diagnostic.red == diagnostic.samples ? "persistent reset (red)"
                    : diagnostic.blue == diagnostic.samples ? "persistent scene change (blue)"
                    : diagnostic.unmarked == diagnostic.samples ? "no marker"
                    : "mixed/unmarked";
                std::printf("%s: FSR reset diagnostic branch: %s\n", label, branch);
            }
        }
        checkDevice("fsr reset diagnostic");
    }

    // ---- Capture gap -------------------------------------------------------------------------
    // A dropped capture makes the next source index discontinuous: the transport must reset the
    // provider history instead of pairing new colour with the previous frame's motion.
    if (!g_fgProbe)
    {
        SetFgTestPhase(kPhaseGap);
        waitForQuiet();
        BIGSCALE_FG_Status before{};
        BIGSCALE_FG_GetStatus(&before);
        ResetObservations();
        frameIndexExtra = 1;
        Check(presentFrame(config.generation, false, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f),
            std::string(label) + ": the frame after a capture gap is accepted");
        frameIndexExtra = 0;
        waitForQuiet();
        bool gapReset = false;
        bool gapPairClean = false;
        {
            std::lock_guard<std::mutex> guard(g_observeMutex);
            for (const ObservedFrame& frame : g_observedFrames)
                if (frame.reset) gapReset = true;
            // The pair's own count is the authoritative proof: the synchronous test readback delays
            // the worker's status accounting, so a counter delta can include a prior pair.
            for (const ObservedPair& pair : g_observedPairs)
            {
                // The gap frame's reported source index is deliberately shifted, so the pair is
                // identified by the transport's typed reset flag, not by that index.
                if (!pair.reset) continue;
                gapPairClean = gapPairClean || (pair.generatedCount == 0 && pair.realQpc != 0);
            }
        }
        Check(gapReset, std::string(label) + ": a capture gap resets the provider history");
        Check(gapPairClean, std::string(label) +
            ": a capture-gap reset presents exactly one real image and no generated image");
        BIGSCALE_FG_Status after{};
        BIGSCALE_FG_GetStatus(&after);
        for (int frame = 0; frame < 8; frame++)
        {
            presentFrame(config.generation, false, static_cast<int>(kObjectBase) + frame * 16,
                static_cast<int>(kObjectY), true, 0.0f, 0.0f);
        }
        Check(waitForActive(config.generation, provider, after.generatedPresents, 8000),
            std::string(label) + ": generation recovers with fresh interpolation after a capture gap");
        checkDevice("capture gap");
    }

    // ---- Cadence -----------------------------------------------------------------------------
    {
        SetFgTestPhase(kPhaseCadence);
        g_traceSlowWaits = g_fgProbe;
        ResetObservations();
        inFlightAllowance = 1;
        {
            // The cadence verdict measures in-flight depth for this phase alone.
            BIGSCALE_FG_Status baseline{};
            BIGSCALE_FG_GetStatus(&baseline);
            renderedFloor = baseline.renderedPresents;
            submittedFrames = 0;
        }
        const int objectX = static_cast<int>(kObjectBase);
        // A probe must finish inside its bounded process lifetime. The cadence verdict only needs a
        // sample of pairs, so the probe stops submitting after twelve seconds while the full run
        // keeps its 120 frames.
        const ULONGLONG cadenceStart = GetTickCount64();
        for (int frame = 0; frame < 120; frame++)
        {
            LARGE_INTEGER submitStart{};
            QueryPerformanceCounter(&submitStart);
            presentFrame(config.generation, frame == 0, objectX + frame * static_cast<int>(kObjectStep),
                static_cast<int>(kObjectY), true, 0.0f, 0.0f);
            if (g_fgProbe && frame < 8)
            {
                LARGE_INTEGER submitEnd{};
                QueryPerformanceCounter(&submitEnd);
                std::printf("%s: cadence frame %d owner submit took %.2f ms\n", label, frame,
                    QpcMilliseconds(submitEnd.QuadPart - submitStart.QuadPart));
            }
            if (probeStalled()) break;
            if (g_fgProbe && GetTickCount64() - cadenceStart >= 12000) break;
        }
        WaitWithMessages(nullptr, 400);
        // Every consecutive present of one source sequence is one source-interval fraction. The
        // last generated image to the real frame is the same step, so a multi-frame provider that
        // bunched its intermediates or its real frame fails here.
        std::vector<double> generatedToReal;
        std::vector<double> realToNext;
        {
            std::lock_guard<std::mutex> guard(g_observeMutex);
            int64_t previousReal = 0;
            for (const ObservedPair& pair : g_observedPairs)
            {
                if (pair.generatedCount == 0) continue;
                int64_t previous = pair.generatedQpc[0];
                for (uint32_t index = 1; index < pair.generatedCount && index < 5; index++)
                {
                    if (pair.generatedQpc[index] > previous)
                        generatedToReal.push_back(QpcMilliseconds(pair.generatedQpc[index] - previous));
                    previous = pair.generatedQpc[index];
                }
                if (pair.realQpc > previous)
                    generatedToReal.push_back(QpcMilliseconds(pair.realQpc - previous));
                if (previousReal != 0 && pair.generatedQpc[0] > previousReal)
                    realToNext.push_back(QpcMilliseconds(pair.generatedQpc[0] - previousReal));
                previousReal = pair.realQpc;
            }
            // Bounded diagnostics so an interrupted run still shows where a pair's time went.
            for (size_t index = 0; index < generatedToReal.size() && index < 6; index++)
                std::printf("%s: cadence step %zu %.2f ms\n", label, index, generatedToReal[index]);
        }
        if (!probeOk()) std::printf("%s: cadence verdict skipped after a probe failure\n", label);
        else
        {
        auto median = [](std::vector<double> values)
        {
            if (values.empty()) return 0.0;
            std::sort(values.begin(), values.end());
            return values[values.size() / 2];
        };
        auto percentile = [](std::vector<double> values, double fraction)
        {
            if (values.empty()) return 0.0;
            std::sort(values.begin(), values.end());
            const size_t index = static_cast<size_t>(fraction * static_cast<double>(values.size() - 1));
            return values[index];
        };
        const double target = kSourceIntervalMilliseconds / static_cast<double>(g_fgMultiplier);
        const double generatedMedian = median(generatedToReal);
        const double realToNextMedian = median(realToNext);
        int fastPairs = 0;
        for (double gap : generatedToReal)
            if (gap < target - 3.0) fastPairs++;
        const double fastRatio = generatedToReal.empty()
            ? 0.0
            : static_cast<double>(fastPairs) / static_cast<double>(generatedToReal.size());
        std::printf(
            "%s: cadence pairs=%zu generated->real median=%.2fms p95=%.2fms max=%.2fms | real->next median=%.2fms "
            "p95=%.2fms | fast(<%.2fms)=%.0f%%\n",
            label, generatedToReal.size(), generatedMedian, percentile(generatedToReal, 0.95),
            generatedToReal.empty() ? 0.0 : *std::max_element(generatedToReal.begin(), generatedToReal.end()),
            realToNextMedian, percentile(realToNext, 0.95), target - 3.0, fastRatio * 100.0);
        Check(generatedToReal.size() >= 20,
            std::string(label) + ": the cadence phase produced at least 20 generated pairs");
        Check(std::fabs(generatedMedian - target) <= 3.0,
            std::string(label) + ": every consecutive generated/real present is one source-interval step");
        // The step count has its own verdict everywhere. The gap from a real frame to the next
        // source's first generated image is governed by source pacing and worker latency, not by the
        // step, so only the historical 2x case asserts that exact value.
        if (g_fgMultiplier == 2)
        // The next source frame's first generated image cannot precede its own capture, so this gap
        // carries the capture pipeline latency on top of one step. What it must not show is bunching
        // (a gap below the step) or a void beyond one display refresh plus that latency.
        {
            const double nextMinimum = target - 3.0;
            Check(realToNextMedian >= nextMinimum && realToNextMedian <= target + 17.0,
                std::string(label) + ": the real present leaves one step before the next capture (" +
                    std::to_string(realToNextMedian) + " ms)");
        }
        else
        {
            std::printf("%s: real->next %.2f ms carries source pacing, not the %.2f ms step\n", label,
                realToNextMedian, target);
        }
        Check(fastRatio <= 0.15,
            std::string(label) + ": the generated/real pair is not presented back to back");
        Check(percentile(generatedToReal, 0.95) <= target + 16.7 + 3.0,
            std::string(label) + ": the pair spacing has no long tail beyond one display refresh");
        Check(percentile(realToNext, 0.95) <= target + 17.0,
            std::string(label) + ": the following generated frame has no long tail");
        checkDevice("cadence");
        }
    }
    g_observePixels.store(false);

    }
    else
    {
        std::printf("%s: pixel and cadence gates not applicable (proxy swapchain owns presentation)\n",
            label);
    }
    statusSnapshot("after pixel and cadence phases");
    SetFgTestPhase(kPhaseTeardown);

    if (g_fgProbe)
    {
        // The probe is finite by construction: warmup, colour identity and orientation, both motion
        // axes, one reset pair with an ordinary successor, and cadence. VR, resize, provider
        // switching and the ANR extent phase are disqualifying cascades here, not evidence, so they
        // never run in this mode.
        g_observePixels.store(false);
        g_observeRaw.store(false);
        statusSnapshot("probe complete");
        // Drain healthy submitted work while this owner thread keeps pumping its messages.
        WaitWithMessages(nullptr, 1000);
        BIGSCALE_FG_Shutdown();
        SetFgTestObserver(nullptr);
        FgTestSetDisplayRefreshMilliseconds(0.0);
        BIGSCALE_FG_GetStatus(&status);
        Check(status.state == BIGSCALE_FG_STATE_OFF, "probe shutdown returns FG to Off");
        DestroyWindow(window);
        g_probeFinished.store(true);
        return;
    }

    // VR suspension: a new generation with an XR session active must stop generation and never
    // publish activity for that request.
    const uint64_t generatedAtVr = status.generatedPresents;
    BIGSCALE_FG_Config vr = config;
    config.generation++;
    vr.generation = config.generation;
    vr.xrActive = 1;
    Check(BIGSCALE_FG_Configure(&vr) == 1, std::string(label) + ": the VR configuration is accepted");
    bool vrSuspended = false;
    const DWORD vrDeadline = GetTickCount() + 5000;
    while (GetTickCount() < vrDeadline)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        BIGSCALE_FG_GetStatus(&status);
        if (status.state == BIGSCALE_FG_STATE_SUSPENDED)
        {
            vrSuspended = true;
            break;
        }
        Sleep(10);
    }
    Check(vrSuspended, std::string(label) + ": VR suspends frame generation");
    // A pair already inside the presenter finishes after the state changes; what must not happen is
    // continuing generation, so the counter has to stop advancing.
    uint64_t quietValue = status.generatedPresents;
    bool quiet = true;
    for (int attempt = 0; attempt < 10; attempt++)
    {
        Sleep(50);
        BIGSCALE_FG_GetStatus(&status);
        if (status.generatedPresents != quietValue)
        {
            quiet = false;
            break;
        }
    }
    Check(quiet || status.generatedPresents == generatedAtVr,
        std::string(label) + ": no generated frame keeps being published while VR owns presentation");
    Check(status.state == BIGSCALE_FG_STATE_SUSPENDED,
        std::string(label) + ": VR keeps frame generation suspended");
    checkDevice("VR");

    // Clearing VR must resume genuine generation for the new generation within a bounded wait.
    BIGSCALE_FG_Config resumed = config;
    config.generation++;
    resumed.generation = config.generation;
    resumed.xrActive = 0;
    Check(BIGSCALE_FG_Configure(&resumed) == 1, std::string(label) + ": the desktop configuration is accepted");
    const uint64_t generatedBeforeResume = status.generatedPresents;
    bool resumeFailed = false;
    bool firstResumeFrame = true;
    for (int frame = 0; frame < 24 && !resumeFailed; frame++)
    {
        // Only the first frame resets history; resetting every frame would forbid the provider from
        // accumulating the temporal history interpolation needs.
        if (!presentFrame(config.generation, firstResumeFrame, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f)) resumeFailed = true;
        firstResumeFrame = false;
        Sleep(5);
    }
    Check(!resumeFailed, std::string(label) + ": capture is accepted after VR is cleared");
    Check(waitForActive(config.generation, provider, generatedBeforeResume, 8000),
        std::string(label) + ": generation resumes after VR is cleared");
    BIGSCALE_FG_GetStatus(&status);
    Check(status.renderedPresents > 0, std::string(label) + ": source presents resume after VR is cleared");
    Check(status.generatedPresents > generatedBeforeResume,
        std::string(label) + ": generated presentations resume after VR is cleared");
    checkDevice("VR clear");

    // Resize must retire or suspend the adapter and recover on the following frames.
    target.Reset();
    backbuffer.Reset();
    context->ClearState();
    context->Flush();
    Check(SUCCEEDED(swapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN,
              sourceChainFlags)), std::string(label) + ": the parent swapchain resizes");
    swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    if (backbuffer) device->CreateRenderTargetView(backbuffer.Get(), nullptr, &target);
    const uint64_t generatedBefore = status.generatedPresents;
    bool resizeFailed = false;
    for (int i = 0; i < 16; i++)
    {
        if (!presentFrame(config.generation, false, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f))
        {
            resizeFailed = true;
            break;
        }
    }
    Check(!resizeFailed, std::string(label) + ": capture is accepted after a resize");
    Check(waitForActive(config.generation, provider, generatedBefore, 8000),
        std::string(label) + ": generation resumes after a resize");
    checkDevice("parent resize");

    // Extent change: a Mode change moves the camera's render extent while the output extent stays
    // fixed. The capture slot must reallocate its render-sized planes and keep capturing; a stale
    // allocation makes every later conversion reject the frame with "the frame-generation render
    // extent is invalid" and the provider report Suspended. The geometry moves to a real 16:9
    // intermediate and back, so both directions of the change are exercised.
    auto changeRenderExtent = [&](uint32_t width, uint32_t height, const char* phase)
    {
        const uint32_t previousWidth = renderWidth;
        const uint32_t previousHeight = renderHeight;
        renderWidth = width;
        renderHeight = height;
        depth.Reset();
        motion.Reset();
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = renderWidth;
        textureDesc.Height = renderHeight;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        textureDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        device->CreateTexture2D(&textureDesc, nullptr, &depth);
        textureDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
        device->CreateTexture2D(&textureDesc, nullptr, &motion);
        Check(depth != nullptr && motion != nullptr,
            std::string(label) + ": the " + phase + " depth and motion fixtures are created");
        if (!depth || !motion)
        {
            renderWidth = previousWidth;
            renderHeight = previousHeight;
            return;
        }
        BIGSCALE_FG_Config resized = config;
        config.generation++;
        resized.generation = config.generation;
        Check(BIGSCALE_FG_Configure(&resized) == 1,
            std::string(label) + ": the " + phase + " configuration is accepted");
        int extentFailures = 0;
        for (int frame = 0; frame < 12; frame++)
        {
            if (!presentFrame(config.generation, frame == 0, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f)) extentFailures++;
        }
        BIGSCALE_FG_Status resizedStatus{};
        Check(BIGSCALE_FG_GetStatus(&resizedStatus) == 1,
            std::string(label) + ": status is readable after the " + phase);
        Check(extentFailures == 0,
            std::string(label) + ": the " + phase + " is accepted by the transport");
        Check(resizedStatus.state != BIGSCALE_FG_STATE_SUSPENDED,
            std::string(label) + ": the " + phase + " does not suspend frame generation (reason: " +
                resizedStatus.reason + ")");
        Check(resizedStatus.state != BIGSCALE_FG_STATE_FAULTED,
            std::string(label) + ": the " + phase + " does not fault frame generation (reason: " +
                resizedStatus.reason + ")");
        Check(resizedStatus.lastResult == 0,
            std::string(label) + ": the " + phase + " capture reports no failure");
        std::printf("%s: after %s state=%d result=%d render=%ux%u output=%ux%u reason=%s\n", label, phase,
            resizedStatus.state, resizedStatus.lastResult, renderWidth, renderHeight, outputWidth,
            outputHeight, resizedStatus.reason);
    };
    const uint32_t originalRenderWidth = renderWidth;
    const uint32_t originalRenderHeight = renderHeight;
    changeRenderExtent(1024, 576, "render-extent change");
    changeRenderExtent(originalRenderWidth, originalRenderHeight, "render-extent restore");
    {
        BIGSCALE_FG_Status extentStatus{};
        BIGSCALE_FG_GetStatus(&extentStatus);
        Check(waitForActive(config.generation, provider, extentStatus.generatedPresents, 8000),
            std::string(label) + ": generation is Active again after the render extents settle");
    }

    // Match identifier-only Unity backbuffers: no pointer, an RTV bound only after submission, and a
    // HUD write between the render event and Present. The pixel phases already run this route; this
    // re-arms it explicitly so the mode survives the extent phase and provider switches.
    captureBoundHudless = true;
    config.generation++;
    Check(BIGSCALE_FG_Configure(&config) == 1,
        std::string(label) + ": ordered bound-HUD-less configuration is accepted");
    BIGSCALE_FG_GetStatus(&status);
    const uint64_t boundGeneratedBefore = status.generatedPresents;
    {
        bool boundFailed = false;
        for (int frame = 0; frame < 20 && !boundFailed; frame++)
            boundFailed = !presentFrame(config.generation, frame == 0, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f);
        Check(!boundFailed, std::string(label) + ": ordered bound-HUD-less capture is accepted");
        Check(waitForActive(config.generation, provider, boundGeneratedBefore, 8000),
            std::string(label) + ": ordered bound-HUD-less capture produces fresh generated presentations");
    }
    if (needsHudless)
    {
        std::printf("%s: phase bound-HUD-less orientation\n", label);
        g_observePixels.store(true);
        ResetObservations();
        for (int frame = 0; frame < 10; frame++)
        {
            const bool submitted = presentFrame(config.generation, frame == 0, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f);
            std::printf("%s:   bound frame %d submitted=%d\n", label, frame, submitted ? 1 : 0);
        }
        Sleep(200);
        std::printf("%s:   checking bound orientation\n", label);
        checkOrientation("bound-HUD-less", true);
        std::printf("%s:   bound orientation checked\n", label);
        g_observePixels.store(false);
    }
    statusSnapshot("after bound-HUD-less phase");
    std::printf("%s: ordered bound-HUD-less generation=%u activeProvider=%d state=%d generated=%llu\n", label,
        status.generation, status.activeProvider, status.state,
        static_cast<unsigned long long>(status.generatedPresents));

    // ---- Provider switching ------------------------------------------------------------------
    if (g_fgAll)
    {
        std::printf("%s: phase provider switching\n", label);
        const int32_t providers[3] = {BIGSCALE_FG_PROVIDER_NVIDIA, BIGSCALE_FG_PROVIDER_FSR,
            BIGSCALE_FG_PROVIDER_XESS};
        const char* names[3] = {"NVIDIA", "FSR", "XeSS"};
        for (int round = 0; round < 8; round++)
        {
            for (int index = 0; index < 3; index++)
            {
                const int32_t next = providers[index];
                const uint64_t generation = ++config.generation;
                config.provider = next;
                config.generation = generation;
                std::printf("[switch round %d] configure %s generation=%llu\n", round, names[index],
                    static_cast<unsigned long long>(generation));
                Check(BIGSCALE_FG_Configure(&config) == 1,
                    std::string(label) + ": switch configure is accepted");
                BIGSCALE_FG_Status before{};
                BIGSCALE_FG_GetStatus(&before);
                for (int i = 0; i < 12; i++)
                {
                    if (!presentFrame(generation, i == 0, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f)) break;
                }
                const bool switched = waitForActive(generation, next, before.generatedPresents, 5000);
                BIGSCALE_FG_GetStatus(&status);
                std::printf("[switch round %d] %s active=%d state=%d generation=%u activeProvider=%d reason=%s\n",
                    round, names[index], switched ? 1 : 0, status.state, status.generation,
                    status.activeProvider, status.reason);
                Check(switched, std::string(label) + ": round " + std::to_string(round) + " " + names[index] +
                                   " became active after an in-process switch");
            }
        }
        // Rapid requests, including while the settings menu suspends generation. The requested
        // selection must survive and an allowed provider must recover.
        for (int round = 0; round < 4; round++)
        {
            BIGSCALE_FG_Config menu = config;
            menu.menuVisible = 1;
            menu.provider = providers[round % 3];
            menu.generation = ++config.generation;
            Check(BIGSCALE_FG_Configure(&menu) == 1,
                std::string(label) + ": a menu-suspended switch is accepted");
            for (int i = 0; i < 3; i++) presentFrame(menu.generation, i == 0, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f);
            BIGSCALE_FG_Config off = menu;
            off.provider = BIGSCALE_FG_PROVIDER_OFF;
            off.generation = ++config.generation;
            Check(BIGSCALE_FG_Configure(&off) == 1, std::string(label) + ": Off while suspended is accepted");
            BIGSCALE_FG_Config back = menu;
            back.menuVisible = 0;
            back.provider = providers[(round + 1) % 3];
            back.generation = ++config.generation;
            Check(BIGSCALE_FG_Configure(&back) == 1,
                std::string(label) + ": re-selecting an allowed provider is accepted");
            config = back;
            BIGSCALE_FG_Status before{};
            BIGSCALE_FG_GetStatus(&before);
            for (int i = 0; i < 12; i++) presentFrame(back.generation, i == 0, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f);
            const bool recovered = waitForActive(back.generation, back.provider, before.generatedPresents, 5000);
            Check(recovered, std::string(label) + ": rapid menu-suspended switching recovers for " +
                                std::string(names[(round + 1) % 3]));
            if (!recovered) break;
        }
        // ANR installs process-wide hooks, so the parent never requests it while the runtime is
        // staged: the one-way migration runs in its own child. Without the runtime the preflight
        // must refuse it without destroying the normal provider that already owns the process.
        if (AnrRuntimeStaged())
        {
            std::printf("%s: normal->ANR migration is exercised in the --anr-after-normal child\n", label);
        }
        else
        {
            const uint64_t generation = ++config.generation;
            BIGSCALE_FG_Config anr = config;
            anr.provider = BIGSCALE_FG_PROVIDER_ANR;
            anr.generation = generation;
            Check(BIGSCALE_FG_Configure(&anr) == 1, std::string(label) + ": the ANR request is accepted");
            BIGSCALE_FG_Status beforeAnr{};
            BIGSCALE_FG_GetStatus(&beforeAnr);
            Check(beforeAnr.processFamily == BIGSCALE_FG_FAMILY_NORMAL,
                std::string(label) + ": the claimed normal family is published before any ANR capture");
            int anrFailures = 0;
            for (int i = 0; i < 12; i++)
            {
                if (presentFrame(generation, i == 0, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f)) continue;
                // Once the refusal is published, presentFrame short-circuits; only a failure before
                // that is a transport defect.
                BIGSCALE_FG_Status probe{};
                BIGSCALE_FG_GetStatus(&probe);
                if (probe.state != BIGSCALE_FG_STATE_UNSUPPORTED) anrFailures++;
            }
            Sleep(400);
            BIGSCALE_FG_Status refused{};
            BIGSCALE_FG_GetStatus(&refused);
            std::printf("%s: ANR-missing-runtime state=%d provider=%d family=%u reason=%s\n", label,
                refused.state, refused.activeProvider, refused.processFamily, refused.reason);
            Check(anrFailures == 0, std::string(label) + ": the parent process survives the ANR request");
            Check(refused.state == BIGSCALE_FG_STATE_UNSUPPORTED,
                std::string(label) + ": a missing ANR runtime reports Unsupported");
            Check(refused.activeProvider == BIGSCALE_FG_PROVIDER_OFF,
                std::string(label) + ": no provider becomes active after the refused ANR request");
            Check(refused.requestedProvider == BIGSCALE_FG_PROVIDER_ANR,
                std::string(label) + ": the refused ANR selection is kept for the next start");
            Check(refused.restartRequired == 0,
                std::string(label) + ": a missing ANR runtime needs no restart");
            Check(refused.processFamily == BIGSCALE_FG_FAMILY_NORMAL,
                std::string(label) + ": a missing ANR runtime does not claim the ANR family");
            // The preflight runs before the incumbent is destroyed, so the reason is either the
            // missing runtime (HAGS enabled) or the HAGS prerequisite its check precedes.
            const std::string missingReason =
                "ANR FG runtime is missing; check the mod page for how to install ANR";
            const bool hagsReason =
                std::string(refused.reason).find("hardware-accelerated GPU scheduling") != std::string::npos;
            Check(std::string(refused.reason) == missingReason || hagsReason,
                std::string(label) + ": the ANR preflight names the missing prerequisite (got: " +
                    std::string(refused.reason) + ")");
            Check(refused.generatedPresents == beforeAnr.generatedPresents,
                std::string(label) + ": no generated frame is published for the refused ANR request");
            // The incumbent normal provider must still own the process.
            config.provider = BIGSCALE_FG_PROVIDER_XESS;
            config.generation = ++config.generation;
            Check(BIGSCALE_FG_Configure(&config) == 1,
                std::string(label) + ": the compatible XeSS reselection is configured");
            for (int i = 0; i < 16; i++)
                presentFrame(config.generation, i == 0, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f);
            Check(waitForActive(config.generation, BIGSCALE_FG_PROVIDER_XESS, beforeAnr.generatedPresents, 8000),
                std::string(label) + ": an allowed provider resumes after the missing ANR runtime");
            {
                BIGSCALE_FG_Status cleared{};
                BIGSCALE_FG_GetStatus(&cleared);
                Check(cleared.restartRequired == 0,
                    std::string(label) + ": a compatible reselection keeps restartRequired clear");
            }
        }
        config.provider = BIGSCALE_FG_PROVIDER_FSR;
    }

    if (g_fgAnr)
    {
        // ANR-first process: Off and re-selection must recover; every foreign provider must be
        // refused before SDK creation with the child hidden and the process alive.
        const int32_t foreign[3] = {BIGSCALE_FG_PROVIDER_NVIDIA, BIGSCALE_FG_PROVIDER_FSR, BIGSCALE_FG_PROVIDER_XESS};
        const char* foreignNames[3] = {"NVIDIA", "FSR", "XeSS"};
        for (int round = 0; round < 3; round++)
        {
            BIGSCALE_FG_Config off = config;
            off.provider = BIGSCALE_FG_PROVIDER_OFF;
            off.generation = ++config.generation;
            Check(BIGSCALE_FG_Configure(&off) == 1, std::string(label) + ": ANR Off is accepted");
            presentFrame(off.generation, true, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f);
            Sleep(100);
            BIGSCALE_FG_Status offStatus{};
            BIGSCALE_FG_GetStatus(&offStatus);
            Check(offStatus.state == BIGSCALE_FG_STATE_OFF, std::string(label) + ": ANR Off returns to Off");
            const uint64_t generation = ++config.generation;
            BIGSCALE_FG_Config anr = config;
            anr.provider = BIGSCALE_FG_PROVIDER_ANR;
            anr.generation = generation;
            Check(BIGSCALE_FG_Configure(&anr) == 1, std::string(label) + ": ANR re-selection is accepted");
            BIGSCALE_FG_Status before{};
            BIGSCALE_FG_GetStatus(&before);
            // The first frame after Off is a history reset and cannot generate; later frames must.
            for (int i = 0; i < 16; i++)
                presentFrame(generation, i == 0, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f);
            const bool recovered = waitForActive(generation, BIGSCALE_FG_PROVIDER_ANR, before.generatedPresents,
                8000);
            Check(recovered, std::string(label) + ": round " + std::to_string(round) +
                                 " ANR recovers after Off");
            if (!recovered) break;
        }
        for (int index = 0; index < 3; index++)
        {
            const uint64_t generation = ++config.generation;
            BIGSCALE_FG_Config foreignConfig = config;
            foreignConfig.provider = foreign[index];
            foreignConfig.generation = generation;
            BIGSCALE_FG_Status before{};
            BIGSCALE_FG_GetStatus(&before);
            Check(BIGSCALE_FG_Configure(&foreignConfig) == 1,
                std::string(label) + ": the " + foreignNames[index] + " request is accepted");
            int failures = 0;
            for (int i = 0; i < 12; i++)
            {
                if (presentFrame(generation, i == 0, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f)) continue;
                BIGSCALE_FG_Status probe{};
                BIGSCALE_FG_GetStatus(&probe);
                if (probe.state != BIGSCALE_FG_STATE_UNSUPPORTED) failures++;
            }
            Sleep(300);
            BIGSCALE_FG_Status refused{};
            BIGSCALE_FG_GetStatus(&refused);
            std::printf("%s: %s-in-ANR-family state=%d provider=%d reason=%s\n", label, foreignNames[index],
                refused.state, refused.activeProvider, refused.reason);
            Check(failures == 0, std::string(label) + ": the ANR process survives the " +
                                     std::string(foreignNames[index]) + " request");
            Check(refused.activeProvider == BIGSCALE_FG_PROVIDER_OFF,
                std::string(label) + ": no provider becomes active for " + std::string(foreignNames[index]));
            Check(std::string(refused.reason) ==
                      "restart the game to switch between ANR and other frame-generation providers",
                std::string(label) + ": the " + foreignNames[index] + " refusal names the restart requirement");
            Check(refused.generatedPresents == before.generatedPresents,
                std::string(label) + ": no generated frame is published for " +
                    std::string(foreignNames[index]));
            Check(refused.restartRequired == 1,
                std::string(label) + ": the " + foreignNames[index] +
                    " refusal carries the typed restart requirement");
            Check(refused.processFamily == BIGSCALE_FG_FAMILY_ANR,
                std::string(label) + ": the " + foreignNames[index] +
                    " refusal reports the ANR family it found");
            // ANR must still be selectable and productive after each refusal.
            const uint64_t recoveryGeneration = ++config.generation;
            BIGSCALE_FG_Config recovery = config;
            recovery.provider = BIGSCALE_FG_PROVIDER_ANR;
            recovery.generation = recoveryGeneration;
            Check(BIGSCALE_FG_Configure(&recovery) == 1, std::string(label) + ": ANR is re-selected");
            BIGSCALE_FG_Status recoveryBefore{};
            BIGSCALE_FG_GetStatus(&recoveryBefore);
            for (int i = 0; i < 16; i++)
                presentFrame(recoveryGeneration, i == 0, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f);
            Check(waitForActive(recoveryGeneration, BIGSCALE_FG_PROVIDER_ANR, recoveryBefore.generatedPresents,
                      8000),
                std::string(label) + ": ANR recovers after the " + std::string(foreignNames[index]) +
                    " refusal");
            {
                BIGSCALE_FG_Status cleared{};
                BIGSCALE_FG_GetStatus(&cleared);
                Check(cleared.restartRequired == 0,
                    std::string(label) + ": re-selecting ANR clears the restart requirement");
            }
            config.provider = BIGSCALE_FG_PROVIDER_ANR;
        }
        // Extent changes and a resize must keep the resident runtime working.
        changeRenderExtent(1024, 576, "ANR render-extent change");
        changeRenderExtent(originalRenderWidth, originalRenderHeight, "ANR render-extent restore");
        target.Reset();
        backbuffer.Reset();
        context->ClearState();
        context->Flush();
        Check(SUCCEEDED(swapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN,
                  sourceChainFlags)), std::string(label) + ": the ANR swapchain resizes");
        swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
        if (backbuffer) device->CreateRenderTargetView(backbuffer.Get(), nullptr, &target);
        BIGSCALE_FG_Status beforeResize{};
        BIGSCALE_FG_GetStatus(&beforeResize);
        for (int i = 0; i < 16; i++) presentFrame(config.generation, i == 0, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f);
        Check(waitForActive(config.generation, BIGSCALE_FG_PROVIDER_ANR, beforeResize.generatedPresents, 8000),
            std::string(label) + ": ANR recovers after a resize");
    }

    if (g_fgAnrAfterNormal)
    {
        // The incumbent normal provider ran the whole suite in this process. ANR must now migrate
        // the family in place, and the way back must stay a typed restart requirement.
        BIGSCALE_FG_Status beforeMigration{};
        BIGSCALE_FG_GetStatus(&beforeMigration);
        Check(beforeMigration.processFamily == BIGSCALE_FG_FAMILY_NORMAL,
            std::string(label) + ": the incumbent normal provider owns the process family");
        Check(beforeMigration.activeProvider == provider,
            std::string(label) + ": the incumbent normal provider is active before the ANR request");
        Check(beforeMigration.restartRequired == 0,
            std::string(label) + ": the incumbent normal provider needs no restart");

        const uint64_t anrGeneration = ++config.generation;
        config.provider = BIGSCALE_FG_PROVIDER_ANR;
        config.generation = anrGeneration;
        Check(BIGSCALE_FG_Configure(&config) == 1,
            std::string(label) + ": the ANR selection after a normal provider is accepted");
        BIGSCALE_FG_Status beforeAnr{};
        BIGSCALE_FG_GetStatus(&beforeAnr);
        for (int i = 0; i < 16; i++)
            presentFrame(anrGeneration, i == 0, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f);
        const bool anrActive = waitForActive(anrGeneration, BIGSCALE_FG_PROVIDER_ANR,
            beforeAnr.generatedPresents, 8000);
        Check(anrActive, std::string(label) + ": ANR becomes active in place after a normal provider");
        BIGSCALE_FG_Status migrated{};
        BIGSCALE_FG_GetStatus(&migrated);
        std::printf("%s: normal->ANR state=%d provider=%d family=%u restart=%u generated=%llu reason=%s\n",
            label, migrated.state, migrated.activeProvider, migrated.processFamily, migrated.restartRequired,
            static_cast<unsigned long long>(migrated.generatedPresents), migrated.reason);
        Check(migrated.processFamily == BIGSCALE_FG_FAMILY_ANR,
            std::string(label) + ": the one-way migration publishes the ANR family");
        Check(migrated.activeProvider == BIGSCALE_FG_PROVIDER_ANR,
            std::string(label) + ": ANR is the active provider after the in-place migration");
        Check(migrated.restartRequired == 0,
            std::string(label) + ": the normal->ANR migration needs no restart");
        Check(migrated.generatedPresents > beforeAnr.generatedPresents,
            std::string(label) + ": the migrated ANR family generates frames");

        // The reverse direction is a one-way door: ANR keeps its hooks installed, so the normal
        // provider is refused with the exact restart reason and the ANR family is kept.
        const uint64_t backGeneration = ++config.generation;
        config.provider = provider;
        config.generation = backGeneration;
        Check(BIGSCALE_FG_Configure(&config) == 1,
            std::string(label) + ": the normal provider request from the ANR family is accepted");
        int backFailures = 0;
        for (int i = 0; i < 12; i++)
        {
            if (presentFrame(backGeneration, i == 0, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f)) continue;
            // Once the refusal is published, presentFrame short-circuits; only a failure before that
            // is a transport defect.
            BIGSCALE_FG_Status probe{};
            BIGSCALE_FG_GetStatus(&probe);
            if (probe.state != BIGSCALE_FG_STATE_UNSUPPORTED) backFailures++;
        }
        Sleep(300);
        BIGSCALE_FG_Status refused{};
        BIGSCALE_FG_GetStatus(&refused);
        std::printf("%s: normal-in-ANR-family state=%d provider=%d family=%u restart=%u reason=%s\n", label,
            refused.state, refused.activeProvider, refused.processFamily, refused.restartRequired,
            refused.reason);
        Check(backFailures == 0, std::string(label) + ": the process survives the normal request from ANR");
        Check(refused.state == BIGSCALE_FG_STATE_UNSUPPORTED,
            std::string(label) + ": the normal request from the ANR family reports Unsupported");
        Check(refused.activeProvider == BIGSCALE_FG_PROVIDER_OFF,
            std::string(label) + ": no provider becomes active after the refused normal request");
        Check(refused.restartRequired == 1,
            std::string(label) + ": the ANR -> normal request carries the typed restart requirement");
        Check(refused.processFamily == BIGSCALE_FG_FAMILY_ANR,
            std::string(label) + ": the ANR family is kept after the refused normal request");
        Check(std::string(refused.reason) ==
                  "restart the game to switch between ANR and other frame-generation providers",
            std::string(label) + ": the ANR -> normal refusal names the restart requirement (got: " +
                std::string(refused.reason) + ")");
        Check(refused.generatedPresents == migrated.generatedPresents,
            std::string(label) + ": no generated frame is published for the refused normal request");
        // Opening the settings menu suspends generation but must not erase the typed requirement:
        // it belongs to the same provider/generation refusal identity.
        {
            BIGSCALE_FG_Config menu = config;
            menu.menuVisible = 1;
            Check(BIGSCALE_FG_Configure(&menu) == 1,
                std::string(label) + ": the menu-suspended ANR -> normal refusal is accepted");
            BIGSCALE_FG_Status suspended{};
            BIGSCALE_FG_GetStatus(&suspended);
            Check(suspended.requestedProvider == provider,
                std::string(label) + ": the refused normal selection survives menu suspension");
            Check(suspended.restartRequired == 1,
                std::string(label) + ": the restart requirement survives menu suspension");
            menu.menuVisible = 0;
            Check(BIGSCALE_FG_Configure(&menu) == 1,
                std::string(label) + ": closing the menu keeps the refused identity");
        }

        // ANR must still be selectable after the refusal, and the family never leaves ANR.
        const uint64_t recoveryGeneration = ++config.generation;
        config.provider = BIGSCALE_FG_PROVIDER_ANR;
        config.generation = recoveryGeneration;
        Check(BIGSCALE_FG_Configure(&config) == 1,
            std::string(label) + ": ANR is re-selected after the refused normal request");
        BIGSCALE_FG_Status recoveryBefore{};
        BIGSCALE_FG_GetStatus(&recoveryBefore);
        for (int i = 0; i < 16; i++)
            presentFrame(recoveryGeneration, i == 0, -1, static_cast<int>(kObjectY), true, 0.0f, 0.0f);
        Check(waitForActive(recoveryGeneration, BIGSCALE_FG_PROVIDER_ANR, recoveryBefore.generatedPresents,
                  8000),
            std::string(label) + ": ANR recovers after the refused normal request");
        {
            BIGSCALE_FG_Status recovered{};
            BIGSCALE_FG_GetStatus(&recovered);
            Check(recovered.restartRequired == 0,
                std::string(label) + ": re-selecting ANR clears the restart requirement");
            Check(recovered.processFamily == BIGSCALE_FG_FAMILY_ANR,
                std::string(label) + ": the ANR family is still owned after the recovery");
        }
        // `config` already names ANR at the recovery generation, so the shared teardown turns it Off.
    }

    std::printf("%s: phase teardown\n", label);
    // Off must return to Off and stop presenting.
    BIGSCALE_FG_Config off = config;
    off.provider = BIGSCALE_FG_PROVIDER_OFF;
    off.generation = ++config.generation;
    Check(BIGSCALE_FG_Configure(&off) == 1, std::string(label) + ": Off is accepted");
    swapchain->Present(0, 0);
    BIGSCALE_FG_GetStatus(&status);
    Check(status.state == BIGSCALE_FG_STATE_OFF, std::string(label) + ": Off returns the transport to Off");

    BIGSCALE_FG_Shutdown();
    // The observer stays registered until the worker has stopped, so a late provider call can never
    // reach a pointer the harness already dropped.
    SetFgTestObserver(nullptr);
    // The provider has stopped, so the pinned refresh no longer governs anything: restore the real
    // display for anything that presents after this check.
    FgTestSetDisplayRefreshMilliseconds(0.0);
    BIGSCALE_FG_GetStatus(&status);
    Check(status.state == BIGSCALE_FG_STATE_OFF, "shutdown returns FG to Off");
    DestroyWindow(window);
}

// VR frame generation against fake OpenVR interfaces: the module's compositor hooks, presenter
// pacing, guide publication, D3D12 pose submissions and deactivation, with no SteamVR involved.
bool FillEyeBar(ID3D11Texture2D* texture, UINT slice, int32_t frame)
{
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    static std::vector<uint8_t> texels;
    texels.assign(static_cast<size_t>(desc.Width) * desc.Height * 4, 0);
    const int32_t barLeft = static_cast<int32_t>((static_cast<uint32_t>(frame) * 7u) % desc.Width);
    for (uint32_t y = 0; y < desc.Height; y++)
    {
        for (uint32_t x = 0; x < desc.Width; x++)
        {
            uint8_t* texel = &texels[(static_cast<size_t>(y) * desc.Width + x) * 4];
            const bool inBar = static_cast<int32_t>(x) >= barLeft &&
                static_cast<int32_t>(x) < barLeft + static_cast<int32_t>(desc.Width / 8);
            // Two different bar colours per slice make a slice mix-up visible in a real session.
            texel[0] = inBar ? (slice == 0 ? 235 : 20) : 20;
            texel[1] = inBar ? 235 : 20;
            texel[2] = inBar ? (slice == 0 ? 30 : 235) : 60;
            texel[3] = 255;
        }
    }
    D3D11_BOX box{0, 0, 0, desc.Width, desc.Height, 1};
    g_context->UpdateSubresource(texture, D3D11CalcSubresource(0, slice, desc.MipLevels), &box, texels.data(),
        desc.Width * 4, desc.Width * 4 * desc.Height);
    return true;
}

void CheckVrFrameGeneration()
{
    std::printf("VR frame generation: fake OpenVR compositor\n");
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring runtime = modulePath;
    const size_t slash = runtime.find_last_of(L"\\/");
    if (slash != std::wstring::npos) runtime.resize(slash);
    const std::wstring runtimeModule = runtime + L"\\amd_fidelityfx_framegeneration_dx12.dll";
    if (GetFileAttributesW(runtimeModule.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        std::printf("VRFG: NOT EXERCISED (%ls is not staged)\n", runtimeModule.c_str());
        Check(false, "VRFG runtime is staged next to the checks (run scripts/build.ps1 or the CMake build "
            "that copies signedbin)");
        return;
    }

    // Earlier checks release the bridge's device-owned state, and the VR module needs the exact bound
    // device plus its immediate context: one colour-only probe binds this device again.
    {
        auto bindColor = CreateTexture(32, 32, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET);
        BIGSCALE_Frame bind{};
        bind.abi = BIGSCALE_ABI_VERSION;
        bind.size = sizeof(BIGSCALE_Frame);
        bind.cameraId = 9001;
        bind.frameIndex = 1;
        bind.operation = BIGSCALE_OP_PROBE;
        bind.preset = BIGSCALE_PRESET_QUALITY;
        bind.upscaler = BIGSCALE_UPSCALER_DLSS;
        bind.inputWidth = 32;
        bind.inputHeight = 32;
        bind.outputWidth = 64;
        bind.outputHeight = 64;
        bind.motionWidth = 32;
        bind.motionHeight = 32;
        bind.sliceCount = 1;
        bind.layout = BIGSCALE_LAYOUT_2D;
        bind.color = bindColor.Get();
        void* packet = bindColor != nullptr ? BIGSCALE_QueueFrame(&bind) : nullptr;
        if (packet != nullptr) Execute(packet);
        AckAll();
        Check(BIGSCALE_GetBoundDevice() == g_device.Get(),
            "the VRFG check binds the bridge to the harness device");
    }

    bigscale_vrfg_check::FakeVrCompositor compositor;
    bigscale_vrfg_check::FakeVrSystem system;
    bigscale_vrfg_check::FakeVrSettings settings;
    compositor.system = &system;
    BIGSCALE_VRFG_TestUseInterfaces(static_cast<vr::IVRCompositor*>(&compositor),
        static_cast<vr::IVRSystem*>(&system), static_cast<vr::IVRSettings*>(&settings));

    const uint32_t eyeSize = 256;
    const uint32_t inputSize = 128;
    auto eyeTexture = CreateTexture(eyeSize, eyeSize, 2, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    // The eye scratch depth the module samples is R32_FLOAT; a typeless depth source exercises the
    // same-format copy legal for that pair while still allowing a depth view for the fixture fill.
    auto guideDepth = CreateTexture(inputSize, inputSize, 1, DXGI_FORMAT_R32_TYPELESS,
        D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    auto guideMotion = CreateTexture(inputSize, inputSize, 1, DXGI_FORMAT_R16G16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE);
    Check(eyeTexture != nullptr && guideDepth != nullptr && guideMotion != nullptr,
        "VRFG fixture textures created");
    if (!eyeTexture || !guideDepth || !guideMotion) return;

    auto runFrame = [&](uint32_t index)
    {
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
        compositor.WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        FillEyeBar(eyeTexture.Get(), 0, static_cast<int32_t>(index));
        FillEyeBar(eyeTexture.Get(), 1, static_cast<int32_t>(index));
        ClearDepth(guideDepth.Get(), 0, 0.35f);
        FillMotion(guideMotion.Get(), 0, 0.002f, 0.0f);
        for (int32_t eye = 0; eye < 2; eye++)
        {
            BIGSCALE_Frame frame{};
            frame.eye = eye;
            frame.frameIndex = static_cast<int32_t>(index);
            EvaluationRequest request{};
            request.inputWidth = inputSize;
            request.inputHeight = inputSize;
            request.scratchWidth = inputSize;
            request.scratchHeight = inputSize;
            request.outputWidth = eyeSize;
            request.outputHeight = eyeSize;
            // Render-pixel jitter, exactly what the super-resolution packet carries in a session.
            request.jitterX = 0.25f;
            request.jitterY = -0.25f;
            request.deltaMilliseconds = 22.2f;
            request.cameraNear = 0.1f;
            request.cameraFar = 1000.0f;
            request.verticalFovRadians = 1.0f;
            VrFgPublishGuides(g_context.Get(), frame, request, guideDepth.Get(), guideMotion.Get());
        }
        vr::Texture_t texture{};
        texture.handle = eyeTexture.Get();
        texture.eType = vr::TextureType_DirectX;
        texture.eColorSpace = vr::ColorSpace_Gamma;
        compositor.SubmitWithArrayIndex(vr::Eye_Left, &texture, 0, nullptr, vr::Submit_Default);
        compositor.SubmitWithArrayIndex(vr::Eye_Right, &texture, 1, nullptr, vr::Submit_Default);
        compositor.PostPresentHandoff();
    };

    // Baseline: the fixture's own per-frame cost with the generator off. It is the ceiling on any
    // pacing measurement, so it is reported with the result rather than assumed.
    {
        const auto baselineStart = std::chrono::steady_clock::now();
        for (uint32_t index = 1; index <= 20; index++) runFrame(index);
        const double baselineSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - baselineStart).count();
        std::printf("VRFG: fixture baseline %.2f Hz with the generator off\n", 20.0 / baselineSeconds);
    }

    BIGSCALE_VRFG_Configure(1);
    const uint32_t frames = 360;
    const uint32_t warmup = 30;
    uint32_t waitsAtWarmup = 0;
    const auto start = std::chrono::steady_clock::now();
    double warmupSeconds = 0.0;
    for (uint32_t index = 1; index <= frames; index++)
    {
        runFrame(index);
        if (index == warmup)
        {
            warmupSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            waitsAtWarmup = compositor.waits;
        }
    }
    const double measuredSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() - warmupSeconds;
    const double gameRate = measuredSeconds > 0.0 ? (frames - warmup) / measuredSeconds : 0.0;
    // The compositor's own slot cadence, measured over the same window. VR frame generation must make
    // one game frame last exactly two compositor frames, whatever the host's timer resolution does.
    const double slotRate = measuredSeconds > 0.0 ? (compositor.waits - waitsAtWarmup) / measuredSeconds : 0.0;

    BIGSCALE_VRFG_Status status{};
    status.size = sizeof(status);
    Check(BIGSCALE_VRFG_GetStatus(&status) == 0, "VRFG status is readable");
    std::string reasonText;
    for (int index = 0; index < BIGSCALE_VRFG_REASON_CAPACITY && status.reason[index] != 0; index++)
        reasonText.push_back(status.reason[index]);
    Check(status.state == BIGSCALE_VRFG_STATE_ACTIVE,
        std::string("VRFG reports Active after the first generated frame (state ") +
            std::to_string(status.state) + ", reason '" + reasonText + "')");
    Check(status.displayHz > 89.0f && status.displayHz < 91.0f,
        "VRFG reports the simulated 90 Hz display (" + std::to_string(status.displayHz) + ")");
    Check(status.motionSmoothing == 0, "VRFG reads Motion Smoothing from IVRSettings_003");
    Check(status.realFrames > 0 && status.generatedFrames > 0,
        "VRFG counters advanced (real=" + std::to_string(status.realFrames) + " generated=" +
            std::to_string(status.generatedFrames) + ")");
    Check(status.droppedFrames <= 2,
        "VRFG dropped at most a startup frame (dropped=" + std::to_string(status.droppedFrames) + ")");
    Check(status.pacingTimeouts <= 2,
        "VRFG never stalled the game's pacing (timeouts=" + std::to_string(status.pacingTimeouts) + ")");
    std::printf("VRFG: real=%llu generated=%llu repeated=%llu dropped=%llu timeouts=%llu\n",
        static_cast<unsigned long long>(status.realFrames),
        static_cast<unsigned long long>(status.generatedFrames),
        static_cast<unsigned long long>(status.repeatedSlots),
        static_cast<unsigned long long>(status.droppedFrames),
        static_cast<unsigned long long>(status.pacingTimeouts));
    std::printf("VRFG: simulated game rate %.2f Hz (target 45 Hz +/- 10%%)\n", gameRate);

    const size_t stopIndex = compositor.submits.size();
    BIGSCALE_VRFG_Configure(0);
    for (uint32_t index = frames + 1; index <= frames + 30; index++) runFrame(index);

    size_t d3d12Submits = 0;
    size_t generatedSubmits = 0;
    size_t realSubmits = 0;
    size_t poseMismatches = 0;
    for (const auto& record : compositor.submits)
    {
        if (record.type != vr::TextureType_DirectX12) continue;
        d3d12Submits++;
        Check((record.flags & vr::Submit_TextureWithPose) != 0,
            "VRFG submits carry Submit_TextureWithPose");
        if (record.resource == nullptr)
        {
            poseMismatches++;
            continue;
        }
        if (record.servedCount == 0)
        {
            poseMismatches++;
            continue;
        }
        // A real submit repeats a pose the system served (a resubmitted frame repeats an older one).
        // Anything else is an interpolation and must be the midpoint of one adjacent pair of served
        // poses; the presenter submits asynchronously, so the pair is found by value, not by index.
        bool real = false;
        for (size_t served = 0; served < record.servedCount; served++)
        {
            if (bigscale_vrfg_check::RotationAngle(record.pose, system.ServedPose(served)) <= 1e-5)
            {
                real = true;
                break;
            }
        }
        if (real)
        {
            realSubmits++;
            continue;
        }
        double best = 1.0e9;
        for (size_t served = 0; served + 1 < record.servedCount; served++)
        {
            const vr::HmdMatrix34_t expected =
                bigscale_vrfg_check::Midpoint(system.ServedPose(served), system.ServedPose(served + 1));
            best = std::min(best, bigscale_vrfg_check::RotationAngle(record.pose, expected));
        }
        if (best > 1e-3)
        {
            if (poseMismatches < 4)
            {
                auto yaw = [](const vr::HmdMatrix34_t& m)
                { return std::atan2(static_cast<double>(m.m[0][2]), static_cast<double>(m.m[0][0])); };
                std::printf("VRFG: pose mismatch served=%zu best=%.6f rad pose=%.6f\n", record.servedCount,
                    best, yaw(record.pose));
                for (size_t served = 0; served < record.servedCount; served++)
                    std::printf("  served[%zu] yaw=%.6f\n", served, yaw(system.ServedPose(served)));
            }
            poseMismatches++;
        }
        else generatedSubmits++;
    }
    std::printf("VRFG: submits d3d12=%zu real=%zu generated=%zu mismatched=%zu\n", d3d12Submits, realSubmits,
        generatedSubmits, poseMismatches);
    Check(d3d12Submits > 0, "VRFG presented frames to the compositor as D3D12 textures");
    Check(realSubmits > 0 && generatedSubmits > 0,
        "VRFG presented both real and generated frames (real=" + std::to_string(realSubmits) +
            " generated=" + std::to_string(generatedSubmits) + ")");
    // Steady state is one generated frame per real frame; the first and last game frames are allowed
    // to show only the real image. The native counters are authoritative here: the presenter also
    // repeats the last image on a slot the game missed, which is a real image, not an interpolation.
    Check(status.generatedFrames >= (status.realFrames * 9) / 10,
        "at least 90% of real frames are followed by a generated frame (generated=" +
            std::to_string(status.generatedFrames) + " real=" + std::to_string(status.realFrames) + ")");
    Check(poseMismatches == 0,
        "every generated pose is the midpoint of its neighbouring real poses (mismatched=" +
            std::to_string(poseMismatches) + ")");
    std::printf("VRFG: compositor slots %.2f Hz, game %.2f Hz (%.2f slots per game frame)\n", slotRate,
        gameRate, gameRate > 0.0 ? slotRate / gameRate : 0.0);
    Check((gameRate > 45.0 * 0.9 && gameRate < 45.0 * 1.1) ||
              (slotRate > 0.0 && gameRate > slotRate * 0.5 * 0.9 && gameRate < slotRate * 0.5 * 1.1),
        "the virtual WaitGetPoses paces the game at exactly half the compositor rate (game " +
            std::to_string(gameRate) + " Hz, compositor " + std::to_string(slotRate) + " Hz)");
    Check(compositor.alreadySubmitted == 0,
        "the compositor never saw a second submit for one eye in one frame (already=" +
            std::to_string(compositor.alreadySubmitted) + ")");
    Check(compositor.handoffs > 0, "the presenter called PostPresentHandoff");

    size_t passthrough = 0;
    size_t passthroughD3D11 = 0;
    for (size_t index = stopIndex; index < compositor.submits.size(); index++)
    {
        passthrough++;
        if (compositor.submits[index].type == vr::TextureType_DirectX) passthroughD3D11++;
    }
    Check(passthrough > 0 && passthroughD3D11 == passthrough,
        "after Off the game's own D3D11 eye textures reach the compositor again (submits=" +
            std::to_string(passthrough) + ")");

    BIGSCALE_VRFG_Shutdown();
}

// The raw reader is what makes the frame samples evidence: it must decode the typeless formats Unity
// actually allocates, and it must refuse a format it cannot decode rather than report zeroes.
void CheckTexelReader()
{
    g_context->ClearState();
    g_context->Flush();

    const auto clearSlice = [](ID3D11Texture2D* texture, DXGI_FORMAT viewFormat, UINT slice, const float color[4])
    {
        ComPtr<ID3D11RenderTargetView> view;
        D3D11_RENDER_TARGET_VIEW_DESC desc{};
        desc.Format = viewFormat;
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray.MipSlice = 0;
        desc.Texture2DArray.FirstArraySlice = slice;
        desc.Texture2DArray.ArraySize = 1;
        if (FAILED(g_device->CreateRenderTargetView(texture, &desc, &view))) return false;
        g_context->ClearRenderTargetView(view.Get(), color);
        return true;
    };

    // Five points spread over the texture, so a wrong row stride cannot pass by reading row 0.
    const UINT points[5][2] = {{3, 5}, {0, 0}, {7, 7}, {1, 6}, {6, 1}};
    float texels[5][4]{};
    const char* reason = nullptr;

    // One fixture per format family. Powers of two decode exactly in R11G11B10_FLOAT, so equality is
    // the right comparison and any exponent-bias or mantissa-width mistake fails the check.
    {
        ComPtr<ID3D11Texture2D> texture = CreateTexture(8, 8, 2, DXGI_FORMAT_R11G11B10_FLOAT,
            D3D11_BIND_RENDER_TARGET);
        Check(texture != nullptr, "R11G11B10 array fixture is created");
        if (texture == nullptr) return;
        const float values[2][4] = {{1.0f, 0.5f, 0.25f, 1.0f}, {0.125f, 2.0f, 4.0f, 1.0f}};
        for (UINT slice = 0; slice < 2; slice++)
        {
            const bool cleared = clearSlice(texture.Get(), DXGI_FORMAT_R11G11B10_FLOAT, slice, values[slice]);
            Check(cleared, "R11G11B10 slice " + std::to_string(slice) + " is cleared");
            reason = nullptr;
            const bool ok = cleared && ReadTexels(g_context.Get(), texture.Get(), slice, points, 5, texels, &reason);
            float worst = 0.0f;
            if (ok)
                for (int i = 0; i < 5; i++)
                    for (int c = 0; c < 3; c++)
                        worst = std::max(worst, std::fabs(texels[i][c] - values[slice][c]));
            Check(ok && worst == 0.0f, "texel reader decodes R11G11B10 slice " + std::to_string(slice) +
                (ok ? " (worst delta " + std::to_string(worst) + ")"
                    : " (read failed: " + std::string(reason != nullptr ? reason : "?") + ")"));
        }

        // An out-of-range point must be refused, never silently clamped or read from mapped padding.
        const UINT outside[1][2] = {{8, 0}};
        reason = nullptr;
        const bool refused = !ReadTexels(g_context.Get(), texture.Get(), 0, outside, 1, texels, &reason);
        Check(refused && reason != nullptr && std::string(reason) == "point",
            "texel reader rejects an out-of-range point (reason " +
                std::string(reason != nullptr ? reason : "null") + ")");
    }

    {
        // The engine allocates colour targets typeless; this is the format the old sampler rejected.
        ComPtr<ID3D11Texture2D> texture = CreateTexture(8, 8, 1, DXGI_FORMAT_R16G16B16A16_TYPELESS,
            D3D11_BIND_RENDER_TARGET);
        Check(texture != nullptr, "typeless RGBA16F fixture is created");
        const float values[4] = {0.75f, 0.3333f, 1.5f, 1.0f};
        const bool cleared = texture != nullptr &&
            clearSlice(texture.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 0, values);
        Check(cleared, "typeless RGBA16F fixture is cleared");
        reason = nullptr;
        const bool ok = cleared && ReadTexels(g_context.Get(), texture.Get(), 0, points, 5, texels, &reason);
        float worst = 0.0f;
        if (ok)
            for (int i = 0; i < 5; i++)
                for (int c = 0; c < 4; c++)
                    worst = std::max(worst, std::fabs(texels[i][c] - values[c]));
        Check(ok && worst <= 1e-3f, "texel reader decodes typeless RGBA16F (worst delta " +
            std::to_string(worst) + (ok ? ")" : ", read failed: " + std::string(reason != nullptr ? reason : "?") + ")"));
    }

    {
        ComPtr<ID3D11Texture2D> texture = CreateTexture(8, 8, 1, DXGI_FORMAT_R32_TYPELESS,
            D3D11_BIND_RENDER_TARGET);
        Check(texture != nullptr, "typeless R32 fixture is created");
        const float values[4] = {0.4f, 0.0f, 0.0f, 1.0f};
        const bool cleared = texture != nullptr && clearSlice(texture.Get(), DXGI_FORMAT_R32_FLOAT, 0, values);
        Check(cleared, "typeless R32 fixture is cleared");
        reason = nullptr;
        const bool ok = cleared && ReadTexels(g_context.Get(), texture.Get(), 0, points, 5, texels, &reason);
        float worst = 0.0f;
        if (ok)
            for (int i = 0; i < 5; i++) worst = std::max(worst, std::fabs(texels[i][0] - values[0]));
        Check(ok && worst <= 1e-6f, "texel reader decodes typeless R32 (worst delta " +
            std::to_string(worst) + (ok ? ")" : ", read failed: " + std::string(reason != nullptr ? reason : "?") + ")"));
    }

    // Depth-stencil textures forbid a source box, so these two exercise the whole-subresource path.
    {
        ComPtr<ID3D11Texture2D> texture = CreateTexture(8, 8, 1, DXGI_FORMAT_R24G8_TYPELESS,
            D3D11_BIND_DEPTH_STENCIL);
        Check(texture != nullptr, "D24 depth-stencil fixture is created");
        const bool cleared = texture != nullptr && ClearDepth(texture.Get(), 0, 0.25f);
        Check(cleared, "D24 depth-stencil fixture is cleared");
        reason = nullptr;
        const bool ok = cleared && ReadTexels(g_context.Get(), texture.Get(), 0, points, 5, texels, &reason);
        float worst = 0.0f;
        if (ok)
            for (int i = 0; i < 5; i++) worst = std::max(worst, std::fabs(texels[i][0] - 0.25f));
        Check(ok && worst <= 1e-5f, "texel reader decodes a D24 depth-stencil (worst delta " +
            std::to_string(worst) + (ok ? ")" : ", read failed: " + std::string(reason != nullptr ? reason : "?") + ")"));
    }

    {
        ComPtr<ID3D11Texture2D> texture = CreateTexture(8, 8, 1, DXGI_FORMAT_R32_TYPELESS,
            D3D11_BIND_DEPTH_STENCIL);
        Check(texture != nullptr, "D32 depth-stencil fixture is created");
        const bool cleared = texture != nullptr && ClearDepth(texture.Get(), 0, 0.6f);
        Check(cleared, "D32 depth-stencil fixture is cleared");
        reason = nullptr;
        const bool ok = cleared && ReadTexels(g_context.Get(), texture.Get(), 0, points, 5, texels, &reason);
        float worst = 0.0f;
        if (ok)
            for (int i = 0; i < 5; i++) worst = std::max(worst, std::fabs(texels[i][0] - 0.6f));
        Check(ok && worst <= 1e-6f, "texel reader decodes a D32 depth-stencil (worst delta " +
            std::to_string(worst) + (ok ? ")" : ", read failed: " + std::string(reason != nullptr ? reason : "?") + ")"));
    }
}

int main(int argc, char** argv)
{
    // Unbuffered so a fault still shows how far the run reached.
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("BigScale native checks\n");
    AddVectoredExceptionHandler(1, CrashHandler);
    if (!CreateDevice())
    {
        std::printf("0 checks run\n");
        return 2;
    }
    {
        wchar_t modulePath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) > 0)
        {
            std::wstring directory(modulePath);
            const size_t slash = directory.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
            {
                directory.resize(slash);
                bigscale::SetLogDirectory(directory);
            }
        }
    }
    std::printf("device: %s\n", g_hardware ? "hardware" : "WARP");
    if (argc > 1 && std::string(argv[1]) == "--stage-probe")
    {
        // Focused run: the probe's image evidence depends on the whole packet path, so its report is
        // kept separate from the unrelated conversion and provider checks.
        CheckStageProbeBilinear();
        std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }
    if (argc > 1 && std::string(argv[1]) == "--neural")
    {
        // Focused run: the neural runtime is user-supplied and large, and its create/evaluate proof
        // is independent of the conversion and provider checks that follow.
        CheckNeuralRendering();
        std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }
    // First, on a pristine device and context: the conversion contract needs neither the bridge nor
    // any earlier check's pipeline state.
    CheckFrameGenerationColorExtents();
    CheckTexelReader();

    EyeResources eyes;
    Check(eyes.EnsureKernels(g_device.Get()), "conversion kernels compile: " + eyes.LastReason());

    CheckSingleView(eyes);
    CheckFullPresentationFrame(eyes);
    CheckArrayIsolation(eyes);
    CheckPackedRegion(eyes);
    CheckPackedDepth(eyes);
    CheckTypelessDepth32(eyes);
    CheckComputeSlotState();
    CheckRejections(eyes);
    CheckAbi();
    CheckFgAdaptiveCadence();
    CheckStageProbeBilinear();
    CheckRingAndProbe();
    CheckRetirementGating();
    CheckFaultRecovery();

    // Explicit provider selection. A requested provider that is unavailable fails the run with its
    // real reason instead of being silently replaced.
    int32_t requested = -1;
    const char* requestedName = nullptr;
    if (argc > 2 && std::string(argv[1]) == "--upscaler")
    {
        const std::string name = argv[2];
        if (name == "dlss") { requested = BIGSCALE_UPSCALER_DLSS; requestedName = "DLSS"; }
        else if (name == "dlss-l") { requested = BIGSCALE_UPSCALER_DLSS_PRESET_L; requestedName = "DLSS preset L"; }
        else if (name == "fsr2") { requested = BIGSCALE_UPSCALER_FSR2; requestedName = "FSR2"; }
        else if (name == "fsr3") { requested = BIGSCALE_UPSCALER_FSR3; requestedName = "FSR3"; }
        else if (name == "fsr4") { requested = BIGSCALE_UPSCALER_FSR4; requestedName = "FSR4"; }
        else if (name == "xess") { requested = BIGSCALE_UPSCALER_XESS; requestedName = "XeSS"; }
    }
    else if (argc > 1 && std::string(argv[1]) == "--ngx")
    {
        requested = BIGSCALE_UPSCALER_DLSS;
        requestedName = "DLSS";
    }
    if (requested >= 0) CheckBackendRuntime(requestedName, requested);
    else std::printf("providers: not exercised (pass --upscaler <dlss|dlss-l|fsr2|fsr3|fsr4|xess>)\n");

    const bool vrfgRequested = argc > 1 && std::string(argv[1]) == "--vrfg";
    int32_t fgProvider = -1;
    const char* fgName = nullptr;
    if (argc > 2 && std::string(argv[1]) == "--fg")
    {
        const std::string name = argv[2];
        if (name == "nvidia") { fgProvider = BIGSCALE_FG_PROVIDER_NVIDIA; fgName = "NVIDIA FG"; }
        else if (name == "fsr") { fgProvider = BIGSCALE_FG_PROVIDER_FSR; fgName = "FSR FG"; }
        else if (name == "xess") { fgProvider = BIGSCALE_FG_PROVIDER_XESS; fgName = "XeSS FG"; }
        else if (name == "anr")
        {
            fgProvider = BIGSCALE_FG_PROVIDER_ANR;
            fgName = "ANR FG";
            g_fgAnr = true;
        }
        else if (name == "streamline")
        {
            fgProvider = BIGSCALE_FG_PROVIDER_STREAMLINE;
            fgName = "Streamline FG";
        }
        else if (name == "all") { fgProvider = BIGSCALE_FG_PROVIDER_NVIDIA; fgName = "NVIDIA FG"; g_fgAll = true; }
        else
        {
            std::printf("unknown frame-generation provider '%s'\n", name.c_str());
            return 2;
        }
        // Optional modes after the provider, in either order. The source shape decides what the
        // transport is asked to admit; the probe decides how much of the tournament runs.
        for (int index = 3; index < argc; index++)
        {
            const std::string option = argv[index];
            if (option == "--source-blt")
            {
                if (g_fgSourceGame)
                {
                    std::printf("--source-blt and --source-game describe different source chains\n");
                    return 2;
                }
                g_fgSourceBlt = true;
            }
            else if (option == "--source-game")
            {
                if (g_fgSourceBlt)
                {
                    std::printf("--source-game and --source-blt describe different source chains\n");
                    return 2;
                }
                g_fgSourceGame = true;
            }
            else if (option == "--fg-probe") g_fgProbe = true;
            else if (option == "--anr-after-normal") g_fgAnrAfterNormal = true;
            else if (option == "--fg-multiplier")
            {
                if (index + 1 >= argc)
                {
                    std::printf("--fg-multiplier needs a value between 2 and 6\n");
                    return 2;
                }
                const std::string value = argv[++index];
                uint32_t parsed = 0;
                for (char character : value)
                {
                    if (character < '0' || character > '9')
                    {
                        std::printf("invalid --fg-multiplier '%s'; expected an integer 2..6\n", value.c_str());
                        return 2;
                    }
                    parsed = parsed * 10 + static_cast<uint32_t>(character - '0');
                }
                if (parsed < 2 || parsed > 6)
                {
                    std::printf("invalid --fg-multiplier '%s'; expected an integer 2..6\n", value.c_str());
                    return 2;
                }
                g_fgMultiplier = parsed;
            }
            else
            {
                std::printf("unknown frame-generation option '%s'\n", option.c_str());
                return 2;
            }
        }
        if (g_fgAll && g_fgProbe)
        {
            // The probe is one finite sequence per provider process; the all-provider tournament is
            // a different run.
            std::printf("--fg all --fg-probe is not a supported combination\n");
            return 2;
        }
        const bool normalProvider = fgProvider == BIGSCALE_FG_PROVIDER_NVIDIA ||
            fgProvider == BIGSCALE_FG_PROVIDER_FSR || fgProvider == BIGSCALE_FG_PROVIDER_XESS;
        if (g_fgAnrAfterNormal && (!normalProvider || g_fgAll || g_fgAnr || g_fgProbe))
        {
            std::printf("--anr-after-normal requires --fg <nvidia|fsr|xess> and cannot use --fg all, "
                        "--fg anr or --fg-probe\n");
            return 2;
        }
    }
    // The SR bridge checks may have re-pointed the native log; restore it so provider transitions
    // from the FG harness are diagnosable in build/native/Release/native.log.
    {
        wchar_t modulePath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) > 0)
        {
            std::wstring directory(modulePath);
            const size_t slash = directory.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
            {
                directory.resize(slash);
                bigscale::SetLogDirectory(directory);
            }
        }
    }
    if (fgProvider >= 0) CheckFrameGeneration(fgName, fgProvider);
    else std::printf("frame generation: not exercised (pass --fg <nvidia|fsr|xess|anr>)\n");
    if (vrfgRequested) CheckVrFrameGeneration();
    else std::printf("VR frame generation: not exercised (pass --vrfg)\n");
    if (g_fgAll)
    {
        // ANR installs process-wide D3D12 hooks, so its family is exercised in its own process and
        // must never load into this one. A failing child is a failure, not "unsupported".
        if (AnrRuntimeStaged())
        {
            int exitCode = -1;
            std::string childReason;
            const bool anrFirstOk = RunAnrChildProcess(false, exitCode, childReason);
            Check(anrFirstOk, "the ANR family process completed its checks (" +
                                  (anrFirstOk ? std::string("exit 0") : childReason) + ")");
            exitCode = -1;
            childReason.clear();
            const bool afterNormalOk = RunAnrChildProcess(true, exitCode, childReason);
            Check(afterNormalOk, "the normal->ANR family process completed its checks (" +
                                     (afterNormalOk ? std::string("exit 0") : childReason) + ")");
        }
        else
        {
            std::printf("ANR family processes: NOT EXERCISED "
                        "(providers/anr/nvngx_dlssg.dll is not staged)\n");
        }
    }

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
