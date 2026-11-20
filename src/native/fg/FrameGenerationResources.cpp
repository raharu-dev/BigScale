#include "FrameGenerationResources.h"

#include "../Log.h"
#include "../Shaders.h"
#include "shaders_generated.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace bigscale
{
namespace
{
constexpr DXGI_FORMAT kFgColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kFgDepthFormat = DXGI_FORMAT_R32_FLOAT;
constexpr DXGI_FORMAT kFgMotionFormat = DXGI_FORMAT_R16G16_FLOAT;

// Formats that share a bit layout; a copy is only legal inside one family.
enum class Family
{
    Unknown,
    R32,
    R24G8,
    R32G8X24,
    Rgba8,
    Bgra8,
    Rg16,
};

Family Classify(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT:
            return Family::R32;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
            return Family::R24G8;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
            return Family::R32G8X24;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return Family::Rgba8;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return Family::Bgra8;
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:
            return Family::Rg16;
        default:
            return Family::Unknown;
    }
}

// Reads encoded SDR values without the sRGB transfer function, matching the FG colour contract.
// The shader stage of the colour conversion reads float4 texels, so every format Unity uses for a
// final post-processed attachment must be sampleable here. URP's HDR intermediate is
// R16G16B16A16_FLOAT: a set restricted to 8-bit formats rejected every real HUD-less colour source
// with "the pre-overlay colour format cannot be read for frame generation", which is exactly the
// plane NVIDIA, ANR and FSR need and XeSS never asks for.
DXGI_FORMAT ColorReadFormat(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT DepthReadFormat(DXGI_FORMAT format)
{
    switch (Classify(format))
    {
        case Family::R32: return DXGI_FORMAT_R32_FLOAT;
        case Family::R24G8: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case Family::R32G8X24: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

bool CreateShared(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format,
                  D3D11_BIND_FLAG extraBind, ComPtr<ID3D11Texture2D>& texture, std::string& reason)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | extraBind;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture)) || !texture)
    {
        reason = "a shared frame-generation capture texture could not be created";
        return false;
    }
    return true;
}
}

void FgSlotTextures::Reset()
{
    colorSrv.Reset();
    depthSrv.Reset();
    motionSrv.Reset();
    colorUav.Reset();
    hudlessUav.Reset();
    depthUav.Reset();
    motionUav.Reset();
    colorStagingSrv.Reset();
    colorStaging.Reset();
    colorStagingFormat = DXGI_FORMAT_UNKNOWN;
    depthStagingSrv.Reset();
    depthStaging.Reset();
    color.Reset();
    hudless.Reset();
    depth.Reset();
    motion.Reset();
    renderWidth = 0;
    renderHeight = 0;
    outputWidth = 0;
    outputHeight = 0;
}

bool FrameGenerationResources::EnsureKernels(ID3D11Device* device, std::string& reason)
{
    if (kernelsReady_ && Ready()) return true;
    if (device == nullptr)
    {
        reason = "the frame-generation capture device is not available";
        return false;
    }
    struct Entry
    {
        Kernel* kernel;
        const unsigned char* code;
        unsigned int size;
        const char* name;
    };
    const Entry entries[4] = {
        {&convertColor_, kConvertColorCso, kConvertColorCsoSize, "convert_color"},
        {&graphColor_, kGraphToScreenColorCso, kGraphToScreenColorCsoSize, "graph_to_screen_color"},
        {&graphDepth_, kGraphToScreenDepthCso, kGraphToScreenDepthCsoSize, "graph_to_screen_depth"},
        {&graphMotion_, kGraphToScreenMotionCso, kGraphToScreenMotionCsoSize, "graph_to_screen_motion"},
    };
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = 32;
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    for (const Entry& entry : entries)
    {
        if (entry.size == 0)
        {
            reason = std::string("missing kernel bytecode: ") + entry.name;
            Log("%s", reason.c_str());
            return false;
        }
        if (FAILED(device->CreateComputeShader(entry.code, entry.size, nullptr, &entry.kernel->shader)))
        {
            reason = std::string("CreateComputeShader failed: ") + entry.name;
            Log("%s", reason.c_str());
            return false;
        }
        if (FAILED(device->CreateBuffer(&buffer, nullptr, &entry.kernel->constants)))
        {
            reason = std::string("constant buffer failed: ") + entry.name;
            Log("%s", reason.c_str());
            return false;
        }
    }
    // The half-resolution kernel is only used when the motion field is exactly half resolution.
    if (!graphMotionHalfShader_ && FAILED(device->CreateComputeShader(kGraphToScreenMotionHalfCso,
            kGraphToScreenMotionHalfCsoSize, nullptr, &graphMotionHalfShader_)))
    {
        reason = "CreateComputeShader failed: graph_to_screen_motion_half";
        return false;
    }
    if (!motionHalfConstants_ && FAILED(device->CreateBuffer(&buffer, nullptr, &motionHalfConstants_)))
    {
        reason = "constant buffer failed: graph_to_screen_motion_half";
        return false;
    }
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (!linearSampler_ && FAILED(device->CreateSamplerState(&sampler, &linearSampler_)))
    {
        reason = "linear sampler creation failed";
        return false;
    }
    kernelsReady_ = true;
    return true;
}

bool FrameGenerationResources::EnsureSlot(ID3D11Device* device, FgSlotTextures& slot, uint32_t renderWidth,
                                          uint32_t renderHeight, uint32_t outputWidth, uint32_t outputHeight,
                                          bool needsHudless, std::string& reason)
{
    if (device == nullptr || renderWidth == 0 || renderHeight == 0 || outputWidth == 0 || outputHeight == 0)
    {
        reason = "the frame-generation capture extent is invalid";
        return false;
    }
    const bool extentsMatch = slot.color && slot.depth && slot.motion &&
        slot.renderWidth == renderWidth && slot.renderHeight == renderHeight &&
        slot.outputWidth == outputWidth && slot.outputHeight == outputHeight;
    if (extentsMatch && (!needsHudless || slot.hudless))
    {
        // The provider changed from one that consumes HUD-less colour to one that does not. This
        // slot is safely retired before it can reach this path, so the unused plane can be freed.
        if (!needsHudless && slot.hudless)
        {
            slot.hudlessUav.Reset();
            slot.hudless.Reset();
        }
        return true;
    }
    slot.Reset();
    if (!CreateShared(device, outputWidth, outputHeight, kFgColorFormat, D3D11_BIND_UNORDERED_ACCESS, slot.color,
            reason))
        return false;
    if (needsHudless &&
        !CreateShared(device, outputWidth, outputHeight, kFgColorFormat, D3D11_BIND_UNORDERED_ACCESS,
            slot.hudless, reason))
        return false;
    if (!CreateShared(device, renderWidth, renderHeight, kFgDepthFormat, D3D11_BIND_UNORDERED_ACCESS, slot.depth,
            reason))
        return false;
    if (!CreateShared(device, renderWidth, renderHeight, kFgMotionFormat, D3D11_BIND_UNORDERED_ACCESS,
            slot.motion, reason))
        return false;
    slot.renderWidth = renderWidth;
    slot.renderHeight = renderHeight;
    slot.outputWidth = outputWidth;
    slot.outputHeight = outputHeight;
    return true;
}

bool FrameGenerationResources::Dispatch(Kernel& kernel, ID3D11DeviceContext* context,
                                        ID3D11ShaderResourceView* srv, ID3D11UnorderedAccessView* uav,
                                        const int32_t (&values)[4], uint32_t width, uint32_t height)
{
    if (!kernel.shader || !kernel.constants) return false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(kernel.constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    std::memcpy(mapped.pData, values, sizeof(values));
    context->Unmap(kernel.constants.Get(), 0);

    ID3D11Buffer* constantBuffer = kernel.constants.Get();
    ID3D11ShaderResourceView* resources[1] = {srv};
    ID3D11UnorderedAccessView* targets[1] = {uav};
    ID3D11SamplerState* samplers[1] = {linearSampler_.Get()};
    context->CSSetShader(kernel.shader.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &constantBuffer);
    context->CSSetShaderResources(0, 1, resources);
    context->CSSetUnorderedAccessViews(0, 1, targets, nullptr);
    if (samplers[0] != nullptr) context->CSSetSamplers(0, 1, samplers);
    context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    return true;
}

bool FrameGenerationResources::DispatchMotionHalf(ID3D11DeviceContext* context,
                                                  ID3D11ShaderResourceView* srv,
                                                  ID3D11UnorderedAccessView* uav, const float (&values)[8],
                                                  uint32_t width, uint32_t height)
{
    if (!graphMotionHalfShader_ || !motionHalfConstants_) return false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(motionHalfConstants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    std::memcpy(mapped.pData, values, sizeof(values));
    context->Unmap(motionHalfConstants_.Get(), 0);

    ID3D11Buffer* constantBuffer = motionHalfConstants_.Get();
    ID3D11ShaderResourceView* resources[1] = {srv};
    ID3D11UnorderedAccessView* targets[1] = {uav};
    ID3D11SamplerState* samplers[1] = {linearSampler_.Get()};
    context->CSSetShader(graphMotionHalfShader_.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &constantBuffer);
    context->CSSetShaderResources(0, 1, resources);
    context->CSSetUnorderedAccessViews(0, 1, targets, nullptr);
    if (samplers[0] != nullptr) context->CSSetSamplers(0, 1, samplers);
    context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    return true;
}

bool FrameGenerationResources::ConvertColorInto(ID3D11DeviceContext* context, FgSlotTextures& slot,
                                                ID3D11Texture2D* destination,
                                                Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>& targetUav,
                                                ID3D11Texture2D* source, uint32_t width, uint32_t height,
                                                bool graphRows, const char* label, std::string& reason)
{
    if (!Ready())
    {
        reason = "the frame-generation conversion kernels are not ready";
        return false;
    }
    if (source == nullptr || !destination)
    {
        reason = std::string("no ") + label + " source for frame generation";
        return false;
    }
    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    if (sourceDesc.SampleDesc.Count != 1)
    {
        reason = std::string("the ") + label + " source is multisampled";
        return false;
    }
    D3D11_TEXTURE2D_DESC destinationDesc{};
    destination->GetDesc(&destinationDesc);
    if (width == 0 || height == 0 || width != slot.outputWidth || height != slot.outputHeight ||
        destinationDesc.Width != width || destinationDesc.Height != height)
    {
        reason = std::string("the ") + label + " source does not match the output extent";
        return false;
    }
    if (sourceDesc.Width < width || sourceDesc.Height < height)
    {
        reason = std::string("the ") + label + " source is smaller than the frame-generation output extent";
        return false;
    }
    // A pooled allocation may be larger than the presented extent. Only the active output-sized
    // region is copied: a whole-resource copy between different sizes is invalid, and a smaller
    // source can never be stretched into the output extent.
    const bool exactExtent = sourceDesc.Width == width && sourceDesc.Height == height;
    const D3D11_BOX activeRegion{0, 0, 0, width, height, 1};
    // A same-format copy can neither reorder rows nor apply a transfer function, so a graph-space
    // source always goes through the row- and transfer-converting kernel.
    if (Classify(sourceDesc.Format) == Family::Rgba8 && !graphRows)
    {
        if (exactExtent) context->CopyResource(destination, source);
        else context->CopySubresourceRegion(destination, 0, 0, 0, 0, source, 0, &activeRegion);
        return true;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = ColorReadFormat(sourceDesc.Format);
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    if (srvDesc.Format == DXGI_FORMAT_UNKNOWN)
    {
        reason = std::string("the ") + label + " format cannot be read for frame generation";
        return false;
    }
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device)
    {
        reason = "the frame-generation capture device is unavailable";
        return false;
    }
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(source, &srvDesc, &srv)) || !srv)
    {
        // The source was created without a shader-resource bind flag. A same-format copy into a
        // slot-owned staging texture preserves the encoded SDR values and is always legal.
        if (!slot.colorStaging || slot.colorStagingFormat != sourceDesc.Format ||
            slot.outputWidth != width || slot.outputHeight != height)
        {
            slot.colorStagingSrv.Reset();
            slot.colorStaging.Reset();
            D3D11_TEXTURE2D_DESC stagingDesc{};
            stagingDesc.Width = width;
            stagingDesc.Height = height;
            stagingDesc.MipLevels = 1;
            stagingDesc.ArraySize = 1;
            stagingDesc.Format = sourceDesc.Format;
            stagingDesc.SampleDesc.Count = 1;
            stagingDesc.Usage = D3D11_USAGE_DEFAULT;
            stagingDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const HRESULT stagingResult = device->CreateTexture2D(&stagingDesc, nullptr, &slot.colorStaging);
            const HRESULT stagingSrvResult = SUCCEEDED(stagingResult) && slot.colorStaging
                ? device->CreateShaderResourceView(slot.colorStaging.Get(), &srvDesc, &slot.colorStagingSrv)
                : stagingResult;
            if (FAILED(stagingResult) || !slot.colorStaging ||
                FAILED(stagingSrvResult) || !slot.colorStagingSrv)
            {
                char detail[96]{};
                // The device removal reason is the difference between a bad descriptor and a lost
                // device, and it is the only way to tell them apart from a log line.
                std::snprintf(detail, sizeof(detail), " (stage=0x%08x srv=0x%08x removed=0x%08x)",
                    static_cast<unsigned>(stagingResult), static_cast<unsigned>(stagingSrvResult),
                    static_cast<unsigned>(device->GetDeviceRemovedReason()));
                reason = std::string("the ") + label + " could not be staged for frame generation" + detail;
                return false;
            }
            slot.colorStagingFormat = sourceDesc.Format;
        }
        if (exactExtent) context->CopyResource(slot.colorStaging.Get(), source);
        else context->CopySubresourceRegion(slot.colorStaging.Get(), 0, 0, 0, 0, source, 0, &activeRegion);
        srv = slot.colorStagingSrv;
    }
    if (!targetUav)
    {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = kFgColorFormat;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(device->CreateUnorderedAccessView(destination, &uavDesc, &targetUav)) || !targetUav)
        {
            reason = std::string("the frame-generation ") + label + " UAV could not be created";
            return false;
        }
    }
    const int32_t values[4] = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
    Kernel& kernel = graphRows ? graphColor_ : convertColor_;
    if (!Dispatch(kernel, context, srv.Get(), targetUav.Get(), values, width, height))
    {
        reason = std::string("the ") + label + " conversion failed";
        return false;
    }
    return true;
}

bool FrameGenerationResources::ConvertColor(ID3D11DeviceContext* context, FgSlotTextures& slot,
                                            ID3D11Texture2D* source, uint32_t width, uint32_t height,
                                            std::string& reason)
{
    return ConvertColorInto(context, slot, slot.color.Get(), slot.colorUav, source, width, height, false,
        "game backbuffer", reason);
}

bool FrameGenerationResources::ConvertHudless(ID3D11DeviceContext* context, FgSlotTextures& slot,
                                              ID3D11Texture2D* source, uint32_t width, uint32_t height,
                                              bool graphRows, std::string& reason)
{
    if (!slot.hudless)
    {
        reason = "the selected frame-generation provider has no HUD-less colour plane";
        return false;
    }
    return ConvertColorInto(context, slot, slot.hudless.Get(), slot.hudlessUav, source, width, height,
        graphRows, "HUD-less colour", reason);
}

namespace
{
bool SameAspect(uint32_t width, uint32_t height, uint32_t referenceWidth, uint32_t referenceHeight)
{
    if (width == 0 || height == 0 || referenceWidth == 0 || referenceHeight == 0) return false;
    const double left = static_cast<double>(width) / static_cast<double>(referenceWidth);
    const double right = static_cast<double>(height) / static_cast<double>(referenceHeight);
    const double scale = left > right ? left : right;
    return std::fabs(left - right) <= 0.005 * scale;
}
}

bool FrameGenerationResources::ConvertDepthMotion(ID3D11DeviceContext* context, FgSlotTextures& slot,
                                                  ID3D11Texture2D* depth, ID3D11Texture2D* motion,
                                                  uint32_t width, uint32_t height, std::string& reason)
{
    if (!Ready())
    {
        reason = "the frame-generation conversion kernels are not ready";
        return false;
    }
    if (width == 0 || height == 0 || width != slot.renderWidth || height != slot.renderHeight)
    {
        reason = "the frame-generation render extent is invalid";
        return false;
    }
    if (depth == nullptr || motion == nullptr || !slot.depth || !slot.motion)
    {
        reason = "frame generation needs URP depth and motion vectors";
        return false;
    }
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device)
    {
        reason = "the frame-generation capture device is unavailable";
        return false;
    }
    D3D11_TEXTURE2D_DESC depthDesc{};
    depth->GetDesc(&depthDesc);
    D3D11_TEXTURE2D_DESC motionDesc{};
    motion->GetDesc(&motionDesc);
    if (depthDesc.Width < width || depthDesc.Height < height || depthDesc.SampleDesc.Count != 1)
    {
        reason = "the URP depth texture does not contain the render extent";
        return false;
    }

    // Depth: a typed read preserves nonlinear device depth; inside the D32 family a region copy into
    // a staging texture is bit-exact and works when the resource cannot be viewed as an SRV. The
    // staging texture exists because the transport must flip rows, and a copy cannot reorder them.
    {
        ComPtr<ID3D11ShaderResourceView> srv;
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DepthReadFormat(depthDesc.Format);
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MostDetailedMip = 0;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.ArraySize = 1;
        const bool viewable = srvDesc.Format != DXGI_FORMAT_UNKNOWN &&
            SUCCEEDED(device->CreateShaderResourceView(depth, &srvDesc, &srv)) && srv;
        if (!viewable)
        {
            if (Classify(depthDesc.Format) != Family::R32)
            {
                reason = "the URP depth texture cannot be read for frame generation";
                return false;
            }
            if (!slot.depthStaging || !slot.depthStagingSrv || slot.renderWidth != width ||
                slot.renderHeight != height)
            {
                slot.depthStagingSrv.Reset();
                slot.depthStaging.Reset();
                D3D11_TEXTURE2D_DESC stagingDesc{};
                stagingDesc.Width = width;
                stagingDesc.Height = height;
                stagingDesc.MipLevels = 1;
                stagingDesc.ArraySize = 1;
                stagingDesc.Format = kFgDepthFormat;
                stagingDesc.SampleDesc.Count = 1;
                stagingDesc.Usage = D3D11_USAGE_DEFAULT;
                stagingDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                const HRESULT stagingResult = device->CreateTexture2D(&stagingDesc, nullptr,
                    &slot.depthStaging);
                const HRESULT stagingSrvResult = SUCCEEDED(stagingResult) && slot.depthStaging
                    ? device->CreateShaderResourceView(slot.depthStaging.Get(), &srvDesc,
                          &slot.depthStagingSrv)
                    : stagingResult;
                if (FAILED(stagingResult) || !slot.depthStaging || FAILED(stagingSrvResult) ||
                    !slot.depthStagingSrv)
                {
                    char detail[96]{};
                    std::snprintf(detail, sizeof(detail), " (stage=0x%08x srv=0x%08x removed=0x%08x)",
                        static_cast<unsigned>(stagingResult), static_cast<unsigned>(stagingSrvResult),
                        static_cast<unsigned>(device->GetDeviceRemovedReason()));
                    reason = std::string("the URP depth texture could not be staged for frame generation") +
                        detail;
                    return false;
                }
            }
            // Only the active render region is copied; a pooled allocation may be larger.
            const D3D11_BOX sourceBox{0, 0, 0, width, height, 1};
            context->CopySubresourceRegion(slot.depthStaging.Get(), 0, 0, 0, 0, depth, 0, &sourceBox);
            srv = slot.depthStagingSrv;
        }
        if (!slot.depthUav)
        {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = kFgDepthFormat;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            if (FAILED(device->CreateUnorderedAccessView(slot.depth.Get(), &uavDesc, &slot.depthUav)))
            {
                reason = "the frame-generation depth UAV could not be created";
                return false;
            }
        }
        const int32_t values[4] = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
        if (!Dispatch(graphDepth_, context, srv.Get(), slot.depthUav.Get(), values, width, height))
        {
            reason = "the URP depth conversion failed";
            return false;
        }
    }

    // Motion: the active source region is the render extent whenever the allocation contains it.
    // Only an exactly half-resolution allocation is resampled; anything else is a wrong scale.
    const bool coversRender = motionDesc.Width >= width && motionDesc.Height >= height &&
        SameAspect(motionDesc.Width, motionDesc.Height, width, height);
    const bool halfResolution = motionDesc.Width * 2 == width && motionDesc.Height * 2 == height;
    if (motionDesc.SampleDesc.Count != 1 || (!coversRender && !halfResolution))
    {
        reason = "the URP motion-vector extent is neither the render extent nor its exact half";
        return false;
    }
    ComPtr<ID3D11ShaderResourceView> srv;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    if (Classify(motionDesc.Format) != Family::Rg16 ||
        FAILED(device->CreateShaderResourceView(motion, &srvDesc, &srv)) || !srv)
    {
        reason = "the URP motion-vector texture cannot be read for frame generation";
        return false;
    }
    if (!slot.motionUav)
    {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = kFgMotionFormat;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(device->CreateUnorderedAccessView(slot.motion.Get(), &uavDesc, &slot.motionUav)))
        {
            reason = "the frame-generation motion UAV could not be created";
            return false;
        }
    }
    if (coversRender)
    {
        const int32_t values[4] = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
        if (!Dispatch(graphMotion_, context, srv.Get(), slot.motionUav.Get(), values, width, height))
        {
            reason = "the URP motion-vector conversion failed";
            return false;
        }
        return true;
    }
    // Half-resolution motion fields are resampled bilinearly; the kernel negates y with the rows.
    const float values[8] = {
        0.5f / static_cast<float>(motionDesc.Width),
        0.5f / static_cast<float>(motionDesc.Height),
        1.0f / static_cast<float>(width),
        1.0f / static_cast<float>(height),
        static_cast<float>(width),
        static_cast<float>(height),
        0.0f,
        0.0f,
    };
    if (!DispatchMotionHalf(context, srv.Get(), slot.motionUav.Get(), values, width, height))
    {
        reason = "the URP motion-vector resample failed";
        return false;
    }
    return true;
}
}
