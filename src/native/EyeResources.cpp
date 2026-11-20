#include "EyeResources.h"
#include "Log.h"
#include "Shaders.h"
#include "shaders_generated.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace bigscale
{
bool IsHdrColorFormat(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            return true;
        default:
            return false;
    }
}

namespace
{
constexpr DXGI_FORMAT kColorScratchFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kMotionScratchFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr DXGI_FORMAT kDepthScratchFormat = DXGI_FORMAT_R32_FLOAT;
constexpr size_t kMaxSets = 4;
constexpr UINT kRequiredColorSupport = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
    D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;

// A formatting family is the set of DXGI formats that share a bit layout. Copies are only legal
// inside one family; anything else goes through a typed read and the conversion kernels.
enum class Family
{
    Unknown,
    R32,
    R24G8,
    R32G8X24,
    Rgba16,
    Rg16,
    Rgba8,
    Bgra8,
    Rgba32,
    R11G11B10,
    R10G10B10A2,
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
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
            return Family::R24G8;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
            return Family::R32G8X24;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SINT:
            return Family::Rgba16;
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SINT:
            return Family::Rg16;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SINT:
            return Family::Rgba8;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return Family::Bgra8;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
            return Family::Rgba32;
        case DXGI_FORMAT_R11G11B10_FLOAT:
            return Family::R11G11B10;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT:
            return Family::R10G10B10A2;
        default:
            return Family::Unknown;
    }
}

// DXGI_FORMAT_UNKNOWN means the source cannot be read at all.
DXGI_FORMAT ColorReadFormat(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case DXGI_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT MotionReadFormat(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R16G16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R16G16_UNORM: return DXGI_FORMAT_R16G16_UNORM;
        case DXGI_FORMAT_R32G32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

// Typed SRV format for a depth resource. Device depth is preserved exactly because the read only
// reinterprets the stored value; nonlinear formats keep their nonlinear value.
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

// Storage format for a private mirror of one engine source. A typeless form keeps the copy inside
// the source's formatting family (depth-stencil sources cannot be copied into a colour-format
// resource) while the typed read format stays a view of that same family.
DXGI_FORMAT MirrorFormat(DXGI_FORMAT format)
{
    switch (Classify(format))
    {
        case Family::R32: return DXGI_FORMAT_R32_TYPELESS;
        case Family::R24G8: return DXGI_FORMAT_R24G8_TYPELESS;
        case Family::R32G8X24: return DXGI_FORMAT_R32G8X24_TYPELESS;
        case Family::Rgba16: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        case Family::Rg16: return DXGI_FORMAT_R16G16_TYPELESS;
        case Family::Rgba8: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case Family::Bgra8: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case Family::Rgba32: return DXGI_FORMAT_R32G32B32A32_TYPELESS;
        case Family::R11G11B10: return DXGI_FORMAT_R11G11B10_FLOAT;
        case Family::R10G10B10A2: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
        default: return format;
    }
}

bool SameFamily(DXGI_FORMAT a, DXGI_FORMAT b)
{
    const Family left = Classify(a);
    return left != Family::Unknown && left == Classify(b);
}

bool CoversWholeSubresource(const EvaluationRequest& request, const D3D11_TEXTURE2D_DESC& source)
{
    return request.inputX == 0 && request.inputY == 0 &&
        static_cast<UINT>(request.inputWidth) == source.Width &&
        static_cast<UINT>(request.inputHeight) == source.Height;
}

bool Describe(const char* what, const D3D11_TEXTURE2D_DESC& desc, std::string& reason)
{
    if (desc.Width == 0 || desc.Height == 0)
    {
        reason = std::string(what) + " has no extent";
        return false;
    }
    if (desc.SampleDesc.Count != 1)
    {
        reason = std::string(what) + " is multisampled";
        return false;
    }
    if (desc.MipLevels != 1)
    {
        reason = std::string(what) + " has mip levels";
        return false;
    }
    return true;
}

bool CheckSupport(ID3D11Device* device, DXGI_FORMAT format, UINT flags, const char* what, std::string& reason)
{
    UINT support = 0;
    if (FAILED(device->CheckFormatSupport(format, &support)) || (support & flags) != flags)
    {
        reason = std::string("device does not support ") + what + " format " +
            std::to_string(static_cast<unsigned>(format));
        return false;
    }
    return true;
}
}

EyeResources::Set* EyeResources::Find(const ViewKey& key)
{
    for (Set* set : sets_)
        if (set->key == key) return set;
    return nullptr;
}

bool EyeResources::Reset(Set& set)
{
    set.color.Reset();
    set.depth.Reset();
    set.motion.Reset();
    set.output.Reset();
    set.colorMirror = Mirror{};
    set.depthMirror = Mirror{};
    set.motionMirror = Mirror{};
    set.colorSrv.Reset();
    set.depthSrv.Reset();
    set.motionSrv.Reset();
    set.colorUav.Reset();
    set.depthUav.Reset();
    set.motionUav.Reset();
    set.outputUav.Reset();
    set.colorSource = DXGI_FORMAT_UNKNOWN;
    set.depthSource = DXGI_FORMAT_UNKNOWN;
    set.motionSource = DXGI_FORMAT_UNKNOWN;
    set.scratchWidth = 0;
    set.scratchHeight = 0;
    set.inputWidth = 0;
    set.inputHeight = 0;
    set.outputWidth = 0;
    set.outputHeight = 0;
    return true;
}

EyeResources::Set* EyeResources::Ensure(Set* set, const EvaluationRequest& request, DXGI_FORMAT colorSource,
                                        DXGI_FORMAT depthSource, DXGI_FORMAT motionSource)
{
    reason_.clear();
    const bool matches = set->output != nullptr &&
        set->scratchWidth == request.scratchWidth && set->scratchHeight == request.scratchHeight &&
        set->outputWidth == request.outputWidth && set->outputHeight == request.outputHeight &&
        set->colorSource == colorSource && set->depthSource == depthSource &&
        set->motionSource == motionSource;
    if (!matches) Reset(*set);
    return set;
}

bool EyeResources::CreateScratch(ID3D11Device* device, Set& set, const EvaluationRequest& request,
                                 DXGI_FORMAT colorSource, DXGI_FORMAT depthSource, DXGI_FORMAT motionSource)
{
    const DXGI_FORMAT colorRead = ColorReadFormat(colorSource);
    const DXGI_FORMAT motionRead = MotionReadFormat(motionSource);
    const DXGI_FORMAT depthRead = DepthReadFormat(depthSource);
    if (colorRead == DXGI_FORMAT_UNKNOWN)
    {
        reason_ = "unsupported colour format " + std::to_string(static_cast<unsigned>(colorSource));
        return false;
    }
    if (motionRead == DXGI_FORMAT_UNKNOWN)
    {
        reason_ = "unsupported motion format " + std::to_string(static_cast<unsigned>(motionSource));
        return false;
    }
    if (depthRead == DXGI_FORMAT_UNKNOWN)
    {
        reason_ = "unsupported depth format " + std::to_string(static_cast<unsigned>(depthSource));
        return false;
    }

    if (!CheckSupport(device, kColorScratchFormat, kRequiredColorSupport, "RGBA16F scratch", reason_)) return false;
    if (!CheckSupport(device, kMotionScratchFormat, kRequiredColorSupport, "RG16F scratch", reason_)) return false;
    if (!CheckSupport(device, kDepthScratchFormat, kRequiredColorSupport, "R32F scratch", reason_)) return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = request.scratchWidth;
    desc.Height = request.scratchHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    desc.Format = kColorScratchFormat;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &set.color)))
    {
        reason_ = "colour scratch allocation failed";
        return false;
    }
    desc.Format = kMotionScratchFormat;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &set.motion)))
    {
        reason_ = "motion scratch allocation failed";
        return false;
    }
    desc.Format = kDepthScratchFormat;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &set.depth)))
    {
        reason_ = "depth scratch allocation failed";
        return false;
    }

    D3D11_TEXTURE2D_DESC out = desc;
    out.Format = kColorScratchFormat;
    out.Width = request.outputWidth;
    out.Height = request.outputHeight;
    out.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(&out, nullptr, &set.output)))
    {
        reason_ = "output scratch allocation failed";
        return false;
    }

    if (FAILED(device->CreateShaderResourceView(set.color.Get(), nullptr, &set.colorSrv)))
    {
        reason_ = "colour scratch SRV failed";
        return false;
    }
    if (FAILED(device->CreateShaderResourceView(set.motion.Get(), nullptr, &set.motionSrv)))
    {
        reason_ = "motion scratch SRV failed";
        return false;
    }
    if (FAILED(device->CreateShaderResourceView(set.depth.Get(), nullptr, &set.depthSrv)))
    {
        reason_ = "depth scratch SRV failed";
        return false;
    }

    if (FAILED(device->CreateUnorderedAccessView(set.color.Get(), nullptr, &set.colorUav)))
    {
        reason_ = "colour scratch UAV failed";
        return false;
    }
    if (FAILED(device->CreateUnorderedAccessView(set.motion.Get(), nullptr, &set.motionUav)))
    {
        reason_ = "motion scratch UAV failed";
        return false;
    }
    if (FAILED(device->CreateUnorderedAccessView(set.depth.Get(), nullptr, &set.depthUav)))
    {
        reason_ = "depth scratch UAV failed";
        return false;
    }
    if (FAILED(device->CreateUnorderedAccessView(set.output.Get(), nullptr, &set.outputUav)))
    {
        reason_ = "output scratch UAV failed";
        return false;
    }

    set.colorSource = colorSource;
    set.depthSource = depthSource;
    set.motionSource = motionSource;
    set.scratchWidth = request.scratchWidth;
    set.scratchHeight = request.scratchHeight;
    set.inputWidth = request.inputWidth;
    set.inputHeight = request.inputHeight;
    set.outputWidth = request.outputWidth;
    set.outputHeight = request.outputHeight;
    Log("scratch created camera=%d eye=%d input=%ux%u scratch=%ux%u output=%ux%u "
        "sourceFormats colour=%u depth=%u motion=%u",
        set.key.cameraId, set.key.eye, set.inputWidth, set.inputHeight, set.scratchWidth, set.scratchHeight,
        set.outputWidth, set.outputHeight, static_cast<unsigned>(colorSource), static_cast<unsigned>(depthSource),
        static_cast<unsigned>(motionSource));
    return true;
}

bool EyeResources::Prepare(ID3D11Device* device, const ViewKey& key, const EvaluationRequest& request,
                           ID3D11Texture2D* sourceColor, ID3D11Texture2D* sourceDepth, ID3D11Texture2D* sourceMotion)
{
    if (!kernelsReady_ && !EnsureKernels(device)) return false;
    if (request.scratchWidth == 0 || request.scratchHeight == 0 || request.inputWidth == 0 || request.inputHeight == 0)
    {
        reason_ = "invalid scratch request";
        return false;
    }
    if (request.inputWidth > request.scratchWidth || request.inputHeight > request.scratchHeight)
    {
        reason_ = "input larger than the scratch extent";
        return false;
    }

    Set* set = Find(key);
    if (set == nullptr)
    {
        // Bounded set table: evict the least recently used view rather than failing every new camera.
        if (sets_.size() >= kMaxSets)
        {
            Set* oldest = nullptr;
            for (Set* candidate : sets_)
                if (oldest == nullptr || candidate->lastFrame < oldest->lastFrame) oldest = candidate;
            if (oldest != nullptr) Forget(oldest->key);
        }
        set = new Set();
        set->key = key;
        sets_.push_back(set);
    }
    set->lastFrame = static_cast<uint32_t>(request.frameIndex);

    if (sourceColor == nullptr || sourceDepth == nullptr || sourceMotion == nullptr)
    {
        reason_ = "missing source textures";
        return false;
    }
    D3D11_TEXTURE2D_DESC color{}, depth{}, motion{};
    sourceColor->GetDesc(&color);
    sourceDepth->GetDesc(&depth);
    sourceMotion->GetDesc(&motion);
    if (!Describe("colour source", color, reason_)) return false;
    if (!Describe("depth source", depth, reason_)) return false;
    if (!Describe("motion source", motion, reason_)) return false;
    if (color.ArraySize < static_cast<UINT>(request.sliceCount) ||
        depth.ArraySize < static_cast<UINT>(request.sliceCount) ||
        motion.ArraySize < static_cast<UINT>(request.sliceCount))
    {
        reason_ = "array slice count mismatch";
        return false;
    }
    set = Ensure(set, request, color.Format, depth.Format, motion.Format);
    if (set->output == nullptr && !CreateScratch(device, *set, request, color.Format, depth.Format, motion.Format))
        return false;
    if (!EnsureMirror(device, *set, set->colorMirror, color, ColorReadFormat(color.Format), "colour")) return false;
    if (!EnsureMirror(device, *set, set->depthMirror, depth, DepthReadFormat(depth.Format), "depth")) return false;
    if (!EnsureMirror(device, *set, set->motionMirror, motion, MotionReadFormat(motion.Format), "motion")) return false;
    return true;
}

bool EyeResources::EnsureMirror(ID3D11Device* device, Set& set, Mirror& mirror,
                                const D3D11_TEXTURE2D_DESC& source, DXGI_FORMAT readFormat, const char* what)
{
    if (mirror.texture != nullptr && mirror.width == source.Width && mirror.height == source.Height &&
        mirror.source == source.Format)
        return true;
    if (readFormat == DXGI_FORMAT_UNKNOWN)
    {
        reason_ = std::string(what) + " mirror read format unsupported";
        return false;
    }

    mirror = Mirror{};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = source.Width;
    desc.Height = source.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = MirrorFormat(source.Format);
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &mirror.texture)))
    {
        reason_ = std::string(what) + " mirror allocation failed";
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = readFormat;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(mirror.texture.Get(), &srvDesc, &mirror.srv)))
    {
        mirror.texture.Reset();
        reason_ = std::string(what) + " mirror SRV failed";
        return false;
    }
    mirror.width = source.Width;
    mirror.height = source.Height;
    mirror.source = source.Format;
    Log("mirror created camera=%d eye=%d %s %ux%u format=%u", set.key.cameraId, set.key.eye, what, mirror.width,
        mirror.height, static_cast<unsigned>(source.Format));
    return true;
}

bool EyeResources::MirrorSources(ID3D11DeviceContext* context, const ViewKey& key,
                                 const EvaluationRequest& request)
{
    Set* set = Find(key);
    if (set == nullptr || set->output == nullptr)
    {
        reason_ = "mirror without scratch";
        return false;
    }
    ID3D11Texture2D* sources[3] = {static_cast<ID3D11Texture2D*>(request.color),
        static_cast<ID3D11Texture2D*>(request.depth), static_cast<ID3D11Texture2D*>(request.motion)};
    Mirror* mirrors[3] = {&set->colorMirror, &set->depthMirror, &set->motionMirror};
    static const char* const names[3] = {"colour", "depth", "motion"};
    for (int i = 0; i < 3; i++)
    {
        if (sources[i] == nullptr)
        {
            reason_ = "mirror without sources";
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        sources[i]->GetDesc(&desc);
        Mirror& mirror = *mirrors[i];
        if (mirror.texture == nullptr || mirror.width != desc.Width || mirror.height != desc.Height ||
            mirror.source != desc.Format)
        {
            reason_ = std::string(names[i]) + " mirror is stale";
            return false;
        }
        const UINT sourceSubresource = D3D11CalcSubresource(0, static_cast<UINT>(request.slice), desc.MipLevels);
        context->CopySubresourceRegion(mirror.texture.Get(), 0, 0, 0, 0, sources[i], sourceSubresource, nullptr);
    }
    return true;
}

bool EyeResources::Dispatch(ComPtr<ID3D11ComputeShader>& shader, ComPtr<ID3D11Buffer>& constants,
                            ID3D11DeviceContext* context, ID3D11ShaderResourceView* srv,
                            ID3D11UnorderedAccessView* uav, const int32_t (&values)[4], uint32_t width,
                            uint32_t height)
{
    if (!shader || !constants) return false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    std::memcpy(mapped.pData, values, sizeof(values));
    context->Unmap(constants.Get(), 0);

    ID3D11Buffer* constantBuffer = constants.Get();
    ID3D11ShaderResourceView* resources[1] = {srv};
    ID3D11UnorderedAccessView* targets[1] = {uav};
    ID3D11SamplerState* samplers[1] = {linearSampler_.Get()};
    context->CSSetShader(shader.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &constantBuffer);
    context->CSSetShaderResources(0, 1, resources);
    context->CSSetUnorderedAccessViews(0, 1, targets, nullptr);
    if (samplers[0] != nullptr) context->CSSetSamplers(0, 1, samplers);
    context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    return true;
}

bool EyeResources::DispatchResample(ID3D11DeviceContext* context, ID3D11ShaderResourceView* srv,
                                    ID3D11UnorderedAccessView* uav, const float (&values)[8], uint32_t width,
                                    uint32_t height)
{
    if (!resampleMotion_.shader || !resampleMotion_.constants) return false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(resampleMotion_.constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    std::memcpy(mapped.pData, values, sizeof(values));
    context->Unmap(resampleMotion_.constants.Get(), 0);

    ID3D11Buffer* constantBuffer = resampleMotion_.constants.Get();
    ID3D11ShaderResourceView* resources[1] = {srv};
    ID3D11UnorderedAccessView* targets[1] = {uav};
    ID3D11SamplerState* samplers[1] = {linearSampler_.Get()};
    context->CSSetShader(resampleMotion_.shader.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &constantBuffer);
    context->CSSetShaderResources(0, 1, resources);
    context->CSSetUnorderedAccessViews(0, 1, targets, nullptr);
    if (samplers[0] != nullptr) context->CSSetSamplers(0, 1, samplers);
    context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    return true;
}

bool EyeResources::DispatchScale(ID3D11DeviceContext* context, ID3D11ShaderResourceView* srv,
                                 ID3D11UnorderedAccessView* uav, const float (&values)[8], uint32_t width,
                                 uint32_t height)
{
    if (!upsample_.shader || !upsample_.constants) return false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(upsample_.constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    std::memcpy(mapped.pData, values, sizeof(values));
    context->Unmap(upsample_.constants.Get(), 0);

    ID3D11Buffer* constantBuffer = upsample_.constants.Get();
    ID3D11ShaderResourceView* resources[1] = {srv};
    ID3D11UnorderedAccessView* targets[1] = {uav};
    ID3D11SamplerState* samplers[1] = {linearSampler_.Get()};
    context->CSSetShader(upsample_.shader.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &constantBuffer);
    context->CSSetShaderResources(0, 1, resources);
    context->CSSetUnorderedAccessViews(0, 1, targets, nullptr);
    if (samplers[0] != nullptr) context->CSSetSamplers(0, 1, samplers);
    context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    return true;
}

bool EyeResources::Extract(ID3D11DeviceContext* context, const ViewKey& key, const EvaluationRequest& request)
{
    Set* set = Find(key);
    if (set == nullptr || set->output == nullptr)
    {
        reason_ = "scratch set missing";
        return false;
    }
    auto* sourceColor = static_cast<ID3D11Texture2D*>(request.color);
    auto* sourceDepth = static_cast<ID3D11Texture2D*>(request.depth);
    auto* sourceMotion = static_cast<ID3D11Texture2D*>(request.motion);
    if (sourceColor == nullptr || sourceDepth == nullptr || sourceMotion == nullptr)
    {
        reason_ = "extract without sources";
        return false;
    }
    D3D11_TEXTURE2D_DESC color{}, depth{}, motion{};
    sourceColor->GetDesc(&color);
    sourceDepth->GetDesc(&depth);
    sourceMotion->GetDesc(&motion);
    if (!Describe("colour source", color, reason_)) return false;
    if (!Describe("depth source", depth, reason_)) return false;
    if (!Describe("motion source", motion, reason_)) return false;

    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (set->colorMirror.srv == nullptr || set->depthMirror.srv == nullptr || set->motionMirror.srv == nullptr)
    {
        reason_ = "mirrors missing";
        return false;
    }
    for (auto* resource : {static_cast<ID3D11Resource*>(sourceColor), static_cast<ID3D11Resource*>(sourceDepth),
             static_cast<ID3D11Resource*>(sourceMotion)})
    {
        ComPtr<ID3D11Device> owner;
        resource->GetDevice(&owner);
        if (!owner || owner.Get() != device.Get())
        {
            reason_ = "input resources belong to different devices";
            return false;
        }
    }

    const UINT inputWidth = static_cast<UINT>(request.inputWidth);
    const UINT inputHeight = static_cast<UINT>(request.inputHeight);
    const UINT inputX = static_cast<UINT>(request.inputX);
    const UINT inputY = static_cast<UINT>(request.inputY);
    if (inputX + inputWidth > color.Width || inputY + inputHeight > color.Height ||
        inputX + inputWidth > depth.Width || inputY + inputHeight > depth.Height ||
        inputX + inputWidth > set->scratchWidth || inputY + inputHeight > set->scratchHeight)
    {
        reason_ = "input region outside the source or scratch";
        return false;
    }
    const bool full = CoversWholeSubresource(request, color);
    // Motion has its own region rules; never reuse the colour-derived decision.
    const bool motionFull = request.inputX == 0 && request.inputY == 0 &&
        inputWidth == motion.Width && inputHeight == motion.Height;

    // Colour: a native copy is only legal inside one formatting family.
    if (SameFamily(color.Format, kColorScratchFormat) && full &&
        inputWidth <= set->scratchWidth && inputHeight <= set->scratchHeight)
    {
        const D3D11_BOX destination{0, 0, 0, inputWidth, inputHeight, 1};
        context->CopySubresourceRegion(set->color.Get(), 0, 0, 0, 0, set->colorMirror.texture.Get(), 0, &destination);
    }
    else
    {
        const int32_t values[4] = {static_cast<int32_t>(inputX), static_cast<int32_t>(inputY),
            static_cast<int32_t>(inputWidth), static_cast<int32_t>(inputHeight)};
        if (!Dispatch(convertColor_.shader, convertColor_.constants, context, set->colorMirror.srv.Get(),
                set->colorUav.Get(), values, inputWidth, inputHeight))
        {
            reason_ = "colour conversion dispatch failed";
            return false;
        }
    }

    // Depth: the scratch is a shader-readable R32_FLOAT texture. Reading the mirror through a typed
    // view preserves nonlinear device depth exactly, and never asks for a depth-stencil copy into a
    // UAV-capable colour-format resource.
    {
        const int32_t values[4] = {static_cast<int32_t>(inputX), static_cast<int32_t>(inputY),
            static_cast<int32_t>(inputWidth), static_cast<int32_t>(inputHeight)};
        if (!Dispatch(convertDepth_.shader, convertDepth_.constants, context, set->depthMirror.srv.Get(),
                set->depthUav.Get(), values, inputWidth, inputHeight))
        {
            reason_ = "depth conversion dispatch failed";
            return false;
        }
    }

    // Motion: copied only when the source is already the RG16F family and the region is its whole
    // subresource, so the copy cannot silently resample or clip the vectors.
    if (SameFamily(motion.Format, kMotionScratchFormat) && motionFull &&
        inputWidth <= set->scratchWidth && inputHeight <= set->scratchHeight)
    {
        const D3D11_BOX destination{0, 0, 0, inputWidth, inputHeight, 1};
        context->CopySubresourceRegion(set->motion.Get(), 0, 0, 0, 0, set->motionMirror.texture.Get(), 0, &destination);
    }
    else
    {
        // The motion allocation may be the full active extent or an exact half of it. Convert the
        // active region into the input-sized scratch without changing the raw UV values: a half-size
        // field is resampled, an equal-size region is loaded exactly.
        const double scaleX = color.Width > 0 ? static_cast<double>(motion.Width) / color.Width : 1.0;
        const double scaleY = color.Height > 0 ? static_cast<double>(motion.Height) / color.Height : 1.0;
        const uint32_t motionX = static_cast<uint32_t>(std::llround(inputX * scaleX));
        const uint32_t motionY = static_cast<uint32_t>(std::llround(inputY * scaleY));
        const uint32_t motionW = std::max<uint32_t>(1, static_cast<uint32_t>(std::llround(inputWidth * scaleX)));
        const uint32_t motionH = std::max<uint32_t>(1, static_cast<uint32_t>(std::llround(inputHeight * scaleY)));
        if (motionX + motionW > motion.Width || motionY + motionH > motion.Height)
        {
            reason_ = "motion region outside the source";
            return false;
        }

        if (motionW == inputWidth && motionH == inputHeight)
        {
            const int32_t convert[4] = {static_cast<int32_t>(motionX), static_cast<int32_t>(motionY),
                static_cast<int32_t>(inputWidth), static_cast<int32_t>(inputHeight)};
            if (!Dispatch(convertMotion_.shader, convertMotion_.constants, context, set->motionMirror.srv.Get(),
                    set->motionUav.Get(), convert, inputWidth, inputHeight))
            {
                reason_ = "motion conversion dispatch failed";
                return false;
            }
        }
        else
        {
            const float values[8] = {
                (static_cast<float>(motionX) + 0.5f) / static_cast<float>(motion.Width),
                (static_cast<float>(motionY) + 0.5f) / static_cast<float>(motion.Height),
                static_cast<float>(motionW) / static_cast<float>(motion.Width) / static_cast<float>(inputWidth),
                static_cast<float>(motionH) / static_cast<float>(motion.Height) / static_cast<float>(inputHeight),
                static_cast<float>(inputWidth),
                static_cast<float>(inputHeight),
                0.0f,
                0.0f,
            };
            if (!DispatchResample(context, set->motionMirror.srv.Get(), set->motionUav.Get(), values, inputWidth,
                    inputHeight))
            {
                reason_ = "motion resample dispatch failed";
                return false;
            }
        }
    }
    return true;
}

bool EyeResources::SpatialUpsample(ID3D11DeviceContext* context, const ViewKey& key,
                                   const EvaluationRequest& request)
{
    Set* set = Find(key);
    if (set == nullptr || set->output == nullptr)
    {
        reason_ = "upsample without scratch";
        return false;
    }
    if (request.outputWidth == 0 || request.outputHeight == 0)
    {
        reason_ = "upsample without an output size";
        return false;
    }
    return UpsampleRegion(context, *set, set->colorSrv.Get(), 0, 0, set->scratchWidth, set->scratchHeight,
        request.inputWidth, request.inputHeight);
}

bool EyeResources::UpsampleRegion(ID3D11DeviceContext* context, Set& set, ID3D11ShaderResourceView* srv,
                                  uint32_t sourceX, uint32_t sourceY, uint32_t sourceWidth, uint32_t sourceHeight,
                                  uint32_t inputWidth, uint32_t inputHeight)
{
    if (srv == nullptr)
    {
        reason_ = "upsample without a source view";
        return false;
    }

    const float values[8] = {
        static_cast<float>(inputWidth) / static_cast<float>(set.outputWidth),
        static_cast<float>(inputHeight) / static_cast<float>(set.outputHeight),
        static_cast<float>(sourceWidth),
        static_cast<float>(sourceHeight),
        static_cast<float>(set.outputWidth),
        static_cast<float>(set.outputHeight),
        static_cast<float>(sourceX),
        static_cast<float>(sourceY),
    };
    if (!DispatchScale(context, srv, set.outputUav.Get(), values, set.outputWidth, set.outputHeight))
    {
        reason_ = "spatial upsample dispatch failed";
        return false;
    }
    return true;
}

bool EyeResources::CopyBack(ID3D11DeviceContext* context, const ViewKey& key, const EvaluationRequest& request,
                            ID3D11Texture2D* outputTexture)
{
    Set* set = Find(key);
    if (set == nullptr || set->output == nullptr || outputTexture == nullptr)
    {
        reason_ = "copy back without scratch";
        return false;
    }
    D3D11_TEXTURE2D_DESC destination{};
    outputTexture->GetDesc(&destination);
    if (!Describe("output texture", destination, reason_)) return false;
    if (Classify(destination.Format) != Family::Rgba16)
    {
        reason_ = "output format is not RGBA16F";
        return false;
    }
    if (destination.ArraySize < static_cast<UINT>(request.sliceCount))
    {
        reason_ = "output array is smaller than the slice count";
        return false;
    }
    const UINT width = static_cast<UINT>(request.outputWidth);
    const UINT height = static_cast<UINT>(request.outputHeight);
    const UINT left = static_cast<UINT>(request.outputX);
    const UINT top = static_cast<UINT>(request.outputY);
    if (width == 0 || height == 0 || left + width > destination.Width || top + height > destination.Height)
    {
        reason_ = "output region outside the destination";
        return false;
    }
    const D3D11_BOX box{0, 0, 0, width, height, 1};
    const UINT subresource = D3D11CalcSubresource(0, static_cast<UINT>(request.slice), destination.MipLevels);
    context->CopySubresourceRegion(outputTexture, subresource, left, top, 0, set->output.Get(), 0, &box);
    return true;
}

bool EyeResources::WriteSourceBaseline(ID3D11DeviceContext* context, const ViewKey& key,
                                       const EvaluationRequest& request, ID3D11Texture2D* outputTexture)
{
    Set* set = Find(key);
    if (set == nullptr || set->output == nullptr)
    {
        reason_ = "baseline without an allocated output";
        return false;
    }
    if (set->colorMirror.srv == nullptr || set->colorMirror.texture == nullptr)
    {
        reason_ = "baseline without a colour mirror";
        return false;
    }
    if (!UpsampleRegion(context, *set, set->colorMirror.srv.Get(), static_cast<uint32_t>(request.inputX),
            static_cast<uint32_t>(request.inputY), set->colorMirror.width, set->colorMirror.height,
            static_cast<uint32_t>(request.inputWidth), static_cast<uint32_t>(request.inputHeight)))
        return false;
    if (!CopyBack(context, key, request, outputTexture)) return false;
    return true;
}

ID3D11Resource* EyeResources::ColorInput(const ViewKey& key)
{
    Set* set = Find(key);
    return set == nullptr ? nullptr : set->color.Get();
}

ID3D11Resource* EyeResources::DepthInput(const ViewKey& key)
{
    Set* set = Find(key);
    return set == nullptr ? nullptr : set->depth.Get();
}

ID3D11Resource* EyeResources::MotionInput(const ViewKey& key)
{
    Set* set = Find(key);
    return set == nullptr ? nullptr : set->motion.Get();
}

ID3D11Resource* EyeResources::OutputTarget(const ViewKey& key)
{
    Set* set = Find(key);
    return set == nullptr ? nullptr : set->output.Get();
}

void EyeResources::Forget(const ViewKey& key)
{
    for (size_t i = 0; i < sets_.size(); i++)
    {
        if (!(sets_[i]->key == key)) continue;
        delete sets_[i];
        sets_.erase(sets_.begin() + static_cast<ptrdiff_t>(i));
        return;
    }
}

void EyeResources::Release()
{
    for (Set* set : sets_) delete set;
    sets_.clear();
    convertColor_ = Kernel{};
    convertDepth_ = Kernel{};
    convertMotion_ = Kernel{};
    upsample_ = Kernel{};
    resampleMotion_ = Kernel{};
    linearSampler_.Reset();
    kernelsReady_ = false;
}

bool EyeResources::EnsureKernels(ID3D11Device* device)
{
    if (kernelsReady_) return true;
    struct Entry
    {
        Kernel* kernel;
        const unsigned char* code;
        unsigned int size;
        const char* name;
    };
    const Entry entries[5] = {
        {&convertColor_, kConvertColorCso, kConvertColorCsoSize, "convert_color"},
        {&convertDepth_, kConvertDepthCso, kConvertDepthCsoSize, "convert_depth"},
        {&convertMotion_, kConvertMotionCso, kConvertMotionCsoSize, "convert_motion"},
        {&upsample_, kUpsampleCso, kUpsampleCsoSize, "upsample"},
        {&resampleMotion_, kResampleMotionCso, kResampleMotionCsoSize, "resample_motion"},
    };
    D3D11_BUFFER_DESC buffer{};
    // 32 bytes for every kernel so the upsample kernel's eight-field block fits; the conversion
    // kernels only read the first 16 bytes.
    buffer.ByteWidth = 32;
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    for (const Entry& entry : entries)
    {
        if (entry.size == 0)
        {
            reason_ = std::string("missing kernel bytecode: ") + entry.name;
            Log("%s", reason_.c_str());
            return false;
        }
        if (FAILED(device->CreateComputeShader(entry.code, entry.size, nullptr, &entry.kernel->shader)))
        {
            reason_ = std::string("CreateComputeShader failed: ") + entry.name;
            Log("%s", reason_.c_str());
            return false;
        }
        if (FAILED(device->CreateBuffer(&buffer, nullptr, &entry.kernel->constants)))
        {
            reason_ = std::string("constant buffer failed: ") + entry.name;
            Log("%s", reason_.c_str());
            return false;
        }
    }
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sampler, &linearSampler_)))
    {
        reason_ = "linear sampler creation failed";
        Log("%s", reason_.c_str());
        return false;
    }
    kernelsReady_ = true;
    Log("conversion kernels compiled and loaded (5 kernels).");
    return true;
}
}
