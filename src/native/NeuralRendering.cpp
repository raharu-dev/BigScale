#include "NeuralRendering.h"

#include "Log.h"
#include "Shaders.h"
#include "shaders_generated.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace bigscale
{
namespace
{
// A rebuild happens only when the GPU can no longer reference the old feature set. The timeout is
// generous because it runs once per resolution/style/pass change, never per frame.
constexpr uint32_t kIdleTimeoutMilliseconds = 2000;
// Two consecutive frames of a view are contiguous; a larger jump means its history is gone.
constexpr int32_t kMaxFrameGap = 2;
// A configuration that failed to create retries after this many frames of its view instead of
// running a rebuild on every frame.
constexpr int32_t kRetryFrames = 240;
constexpr uint32_t kNoGeneration = 0xffffffffu;

int32_t ClampInt(int32_t value, int32_t low, int32_t high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

float SanitizeUnit(float value)
{
    if (!std::isfinite(value)) return 1.0f;
    if (value < 0.0f) return 0.0f;
    if (value > 2.0f) return 2.0f;
    return value;
}

const char* StateName(int32_t state)
{
    switch (state)
    {
        case BIGSCALE_NEURAL_OFF: return "Off";
        case BIGSCALE_NEURAL_WAITING: return "Waiting";
        case BIGSCALE_NEURAL_ACTIVE: return "Active";
        case BIGSCALE_NEURAL_RUNTIME_MISSING: return "Runtime missing";
        case BIGSCALE_NEURAL_FAILED: return "Failed";
        case BIGSCALE_NEURAL_UNSUPPORTED: return "Unsupported";
    }
    return "Unknown";
}

bool SameConfig(const BIGSCALE_NeuralConfig& left, const BIGSCALE_NeuralConfig& right)
{
    return left.abi == right.abi && left.size == right.size && left.enabled == right.enabled &&
           left.style == right.style && left.timing == right.timing && left.passes == right.passes &&
           left.strength == right.strength && left.detail == right.detail && left.tone == right.tone &&
           left.characterMask == right.characterMask && left.reserved == right.reserved;
}

// A shared texture the D3D12 side can open, sized exactly for one role. The D3D11 allocation is
// owned by the interop object so the D3D12 resource is released with it.
bool CreateSharedTexture(ID3D11Device* device, Dx12Interop& interop, Dx12Interop::Shared& shared,
    uint32_t width, uint32_t height, DXGI_FORMAT format, bool unorderedAccess, const char* what,
    std::string& reason)
{
    interop.Release(shared);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // The neural outputs are written as UAVs on the D3D12 side; a legacy shared handle opened on
    // D3D12 only supports that when the D3D11 allocation was created with the UAV bind flag.
    if (unorderedAccess) description.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    description.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &texture)) || !texture)
    {
        reason = std::string("the neural ") + what + " texture could not be created";
        return false;
    }
    std::string shareReason;
    if (!interop.Share(texture.Get(), shared, shareReason))
    {
        reason = std::string("the neural ") + what + " texture could not be shared: " + shareReason;
        return false;
    }
    return true;
}
}

struct NeuralRenderingStage::Geometry
{
    uint32_t colourWidth = 0;
    uint32_t colourHeight = 0;
    uint32_t guideWidth = 0;
    uint32_t guideHeight = 0;
};

struct NeuralRenderingStage::Kernel
{
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
};

struct NeuralRenderingStage::Pass
{
    NeuralFeature feature;
    int32_t lastFrame = -1;
    int32_t lastEpoch = 0;
    uint32_t evaluations = 0;
};

struct NeuralRenderingStage::View
{
    ViewKey key{};
    Geometry geometry{};
    int32_t style = -1;
    uint32_t passCount = 0;
    uint32_t generation = kNoGeneration;
    uint32_t failedGeneration = kNoGeneration;
    int32_t retryFrame = 0;
    uint32_t lastUse = 0;
    bool created = false;
    uint32_t maskWidth = 0;
    uint32_t maskHeight = 0;
    Pass passes[NeuralRenderingStage::kMaxPasses];

    Dx12Interop::Shared colour;
    Dx12Interop::Shared depth;
    Dx12Interop::Shared motion;
    Dx12Interop::Shared mask;
    Dx12Interop::Shared output[2];

    ComPtr<ID3D11ShaderResourceView> colourSrv;
    ComPtr<ID3D11ShaderResourceView> maskSrv;
    ComPtr<ID3D11ShaderResourceView> outputSrv[2];
    ComPtr<ID3D11Texture2D> result;
    ComPtr<ID3D11UnorderedAccessView> resultUav;
};

struct NeuralRenderingStage::Snapshot
{
    bool enabled = false;
    int32_t style = 0;
    int32_t timing = 0;
    int32_t passes = 1;
    float strength = 1.0f;
    float detail = 1.0f;
    float tone = 1.0f;
    bool characterMask = true;
    uint32_t generation = 0;
};

NeuralRenderingStage::NeuralRenderingStage() = default;

NeuralRenderingStage::~NeuralRenderingStage() { Release(); }

void NeuralRenderingStage::SetEnvironment(const std::wstring& runtimeDirectory, const std::wstring& dataDirectory,
    const std::string& engineVersion)
{
    std::lock_guard<std::mutex> guard(configMutex_);
    runtimeDirectory_ = runtimeDirectory;
    dataDirectory_ = dataDirectory;
    if (!engineVersion.empty()) engineVersion_ = engineVersion;
    runtime_.SetDirectories(runtimeDirectory_, dataDirectory_);
}

void NeuralRenderingStage::Configure(const BIGSCALE_NeuralConfig& config)
{
    BIGSCALE_NeuralConfig next = config;
    next.enabled = config.enabled != 0 ? 1 : 0;
    next.characterMask = config.characterMask != 0 ? 1 : 0;
    next.passes = ClampInt(config.passes, 1, kMaxPasses);
    next.style = ClampInt(config.style, 0, 2);
    next.timing = ClampInt(config.timing, 0, 1);
    next.strength = SanitizeUnit(config.strength);
    next.detail = SanitizeUnit(config.detail);
    next.tone = SanitizeUnit(config.tone);
    next.reserved = 0;

    bool changed = false;
    {
        std::lock_guard<std::mutex> guard(configMutex_);
        if (!SameConfig(config_, next))
        {
            config_ = next;
            generation_++;
            changed = true;
        }
    }

    if (!next.enabled)
    {
        SetState(BIGSCALE_NEURAL_OFF);
        return;
    }
    if (!runtime_.RuntimePresent())
    {
        SetState(BIGSCALE_NEURAL_RUNTIME_MISSING);
        return;
    }
    // A redundant push never demotes a stage that is already producing frames; a real change
    // invalidates the feature set, so the next packet reports Waiting until it evaluates.
    const int32_t current = State();
    if (changed || current == BIGSCALE_NEURAL_OFF || current == BIGSCALE_NEURAL_RUNTIME_MISSING)
        SetState(BIGSCALE_NEURAL_WAITING);
}

NeuralRenderingStage::Snapshot NeuralRenderingStage::Capture() const
{
    std::lock_guard<std::mutex> guard(configMutex_);
    Snapshot snapshot;
    snapshot.enabled = config_.enabled != 0;
    snapshot.style = ClampInt(config_.style, 0, 2);
    snapshot.timing = config_.timing != 0 ? 1 : 0;
    snapshot.passes = ClampInt(config_.passes, 1, kMaxPasses);
    snapshot.strength = SanitizeUnit(config_.strength);
    snapshot.detail = SanitizeUnit(config_.detail);
    snapshot.tone = SanitizeUnit(config_.tone);
    snapshot.characterMask = config_.characterMask != 0;
    snapshot.generation = generation_;
    return snapshot;
}

void NeuralRenderingStage::MarkUnsupported()
{
    bool enabled = false;
    {
        std::lock_guard<std::mutex> guard(configMutex_);
        enabled = config_.enabled != 0;
    }
    if (enabled) SetState(BIGSCALE_NEURAL_UNSUPPORTED);
}

ID3D11Resource* NeuralRenderingStage::BeforeUpscale(ID3D11Device* device, ID3D11DeviceContext* context,
    const ViewKey& key, const EvaluationRequest& request, void* mask, uint32_t maskWidth, uint32_t maskHeight)
{
    const Snapshot snapshot = Capture();
    if (!snapshot.enabled || snapshot.timing != 0) return nullptr;
    std::lock_guard<std::mutex> guard(frameMutex_);
    return RunFrame(device, context, key, request, mask, maskWidth, maskHeight, snapshot, true);
}

bool NeuralRenderingStage::AfterUpscale(ID3D11Device* device, ID3D11DeviceContext* context, const ViewKey& key,
    const EvaluationRequest& request, void* mask, uint32_t maskWidth, uint32_t maskHeight)
{
    const Snapshot snapshot = Capture();
    if (!snapshot.enabled || snapshot.timing != 1) return false;
    std::lock_guard<std::mutex> guard(frameMutex_);
    return RunFrame(device, context, key, request, mask, maskWidth, maskHeight, snapshot, false) != nullptr;
}

ID3D11Resource* NeuralRenderingStage::RunFrame(ID3D11Device* device, ID3D11DeviceContext* context,
    const ViewKey& key, const EvaluationRequest& request, void* mask, uint32_t maskWidth, uint32_t maskHeight,
    const Snapshot& snapshot, bool beforeUpscaling)
{
    if (device == nullptr || context == nullptr)
    {
        Fail("neural rendering has no device or context");
        return nullptr;
    }
    if (request.layout != BIGSCALE_LAYOUT_2D || request.sliceCount != 1)
    {
        MarkUnsupported();
        return nullptr;
    }
    // Leaving a VR or packed session must resume the status row: a supported packet downgrades the
    // earlier Unsupported diagnostic back to Waiting until this packet evaluates.
    if (State() == BIGSCALE_NEURAL_UNSUPPORTED) SetState(BIGSCALE_NEURAL_WAITING);
    if (request.color == nullptr || request.depth == nullptr || request.motion == nullptr ||
        request.output == nullptr)
    {
        Fail("the neural packet is missing a resource");
        return nullptr;
    }

    // A failed runtime initialisation is not retried at frame rate. A configuration change bumps the
    // generation; an explicit reset packet also clears the latch.
    if (request.reset) runtimeFailedGeneration_ = 0xffffffffu;
    if (runtimeFailedGeneration_ == snapshot.generation && request.frameIndex < runtimeRetryFrame_)
        return nullptr;

    std::string reason;
    if (!EnsureRuntime(device, context, reason))
    {
        runtimeFailedGeneration_ = snapshot.generation;
        runtimeRetryFrame_ = request.frameIndex + kRetryFrames;
        // A missing file is a persistent diagnostic, not a fault of the upscaler; every other
        // initialisation failure is reported as Failed with the SDK reason in the log.
        if (!runtime_.RuntimePresent())
        {
            NoteOnce(reason);
            SetState(BIGSCALE_NEURAL_RUNTIME_MISSING);
            return nullptr;
        }
        Fail(reason);
        return nullptr;
    }

    Geometry geometry;
    geometry.colourWidth = beforeUpscaling ? request.inputWidth : request.outputWidth;
    geometry.colourHeight = beforeUpscaling ? request.inputHeight : request.outputHeight;
    geometry.guideWidth = request.inputWidth;
    geometry.guideHeight = request.inputHeight;
    if (geometry.colourWidth == 0 || geometry.colourHeight == 0 || geometry.guideWidth == 0 ||
        geometry.guideHeight == 0)
    {
        Fail("the neural packet reported an empty extent");
        return nullptr;
    }
    // The compatibility model's smallest tiles are larger than 64x64; evaluating a tiny extent made
    // the D3D12 device hang in the offline checks. Refuse anything below the smallest verified size
    // instead of risking the upscaler's device. Normal game resolutions are far above this.
    constexpr uint32_t kMinimumNeuralExtent = 256;
    if (geometry.colourWidth < kMinimumNeuralExtent || geometry.colourHeight < kMinimumNeuralExtent)
    {
        Fail("the neural extent is below the verified 256x256 minimum");
        return nullptr;
    }

    // Before upscaling the neural answer replaces the backend's colour input; after upscaling the
    // backend's own result is the neural source and the final image is written back into it.
    ID3D11Texture2D* colourSource =
        static_cast<ID3D11Texture2D*>(beforeUpscaling ? request.color : request.output);
    D3D11_TEXTURE2D_DESC colourDescription{};
    colourSource->GetDesc(&colourDescription);
    if (colourDescription.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        colourDescription.Width < geometry.colourWidth || colourDescription.Height < geometry.colourHeight)
    {
        Fail("the neural colour input is not RGBA16F at the stage extent");
        return nullptr;
    }

    ID3D11Texture2D* maskTexture = nullptr;
    bool useMask = false;
    if (snapshot.characterMask && mask != nullptr && maskWidth > 0 && maskHeight > 0)
    {
        ID3D11Texture2D* candidate = static_cast<ID3D11Texture2D*>(mask);
        D3D11_TEXTURE2D_DESC maskDescription{};
        candidate->GetDesc(&maskDescription);
        if ((maskDescription.Format == DXGI_FORMAT_R8_UNORM ||
             maskDescription.Format == DXGI_FORMAT_R8_TYPELESS) &&
            maskDescription.SampleDesc.Count == 1 &&
            maskDescription.Width == maskWidth && maskDescription.Height == maskHeight &&
            maskWidth == request.outputWidth && maskHeight == request.outputHeight)
        {
            maskTexture = candidate;
            useMask = true;
        }
        else
        {
            // An unusable mask never fails neural rendering: the model answer is used unmasked.
            char detail[192]{};
            std::snprintf(detail, sizeof(detail),
                "the character mask is format=%u %ux%u samples=%u, requested %ux%u; continuing without masking",
                static_cast<unsigned>(maskDescription.Format), maskDescription.Width, maskDescription.Height,
                static_cast<unsigned>(maskDescription.SampleDesc.Count), maskWidth, maskHeight);
            NoteOnce(detail);
        }
    }

    View* view = AcquireView(key, reason);
    if (view == nullptr)
    {
        Fail(reason);
        return nullptr;
    }
    view->lastUse = ++useCounter_;

    if (view->created && NeedsRebuild(*view, snapshot, geometry))
    {
        if (!interop_.WaitIdle(kIdleTimeoutMilliseconds, reason))
        {
            // The old feature set stays valid and the image is untouched; retry on the next packet.
            NoteOnce("the GPU did not become idle before the neural feature set was rebuilt: " + reason);
            return nullptr;
        }
        ReleaseFeatures(*view);
        ReleaseTextures(*view);
        view->created = false;
    }

    if (!view->created)
    {
        if (view->failedGeneration == snapshot.generation && request.frameIndex < view->retryFrame)
            return nullptr;
        // A previous creation attempt may have created some passes before a later pass or the
        // submission failed. Release those handles before EnsureTextures replaces the resources they
        // reference, and before the retry overwrites their slots.
        ReleaseFeatures(*view);
        if (!EnsureTextures(device, *view, geometry, reason) ||
            !EnsureMask(device, *view, useMask ? maskWidth : 0, useMask ? maskHeight : 0, reason) ||
            !CopyInputs(context, *view, request, colourSource, geometry, maskTexture, useMask, reason) ||
            !CreateFeatureFrame(context, *view, snapshot, geometry, reason))
        {
            Fail(reason);
            view->failedGeneration = snapshot.generation;
            view->retryFrame = request.frameIndex + kRetryFrames;
            return nullptr;
        }
        view->geometry = geometry;
        view->style = snapshot.style;
        view->passCount = static_cast<uint32_t>(snapshot.passes);
        view->generation = snapshot.generation;
        view->failedGeneration = kNoGeneration;
        view->created = true;
        SetState(BIGSCALE_NEURAL_WAITING);
        // The creation frame only proves the feature set exists; the next packet evaluates it.
        return nullptr;
    }

    if (!EnsureMask(device, *view, useMask ? maskWidth : 0, useMask ? maskHeight : 0, reason) ||
        !CopyInputs(context, *view, request, colourSource, geometry, maskTexture, useMask, reason) ||
        !EvaluateFrame(context, *view, request, snapshot, geometry, reason) ||
        !Composite(context, *view, geometry, (snapshot.passes - 1) % 2, useMask, reason))
    {
        Fail(reason);
        return nullptr;
    }

    lastFailure_.clear();
    SetState(BIGSCALE_NEURAL_ACTIVE);
    if (!beforeUpscaling) context->CopyResource(request.output, view->result.Get());
    return view->result.Get();
}

bool NeuralRenderingStage::EnsureRuntime(ID3D11Device* device, ID3D11DeviceContext* context, std::string& reason)
{
    // A missing user-supplied runtime must not create a D3D12 device at all: the upscaler and frame
    // generation keep their existing device state, and the status row reports Runtime missing.
    if (!runtime_.Ready() && !runtime_.RuntimePresent())
    {
        reason = "nvngx_dlssnr.dll is not staged in the plugin directory";
        return false;
    }
    if (!interop_.Ready() && !interop_.Ensure(device, reason)) return false;
    if (!runtime_.Ready())
    {
        // SetEnvironment runs before the first packet; the copy only removes a cross-thread read of
        // the strings if the bridge forwards a late engine version.
        std::wstring runtimeDirectory;
        std::wstring dataDirectory;
        std::string engineVersion;
        {
            std::lock_guard<std::mutex> guard(configMutex_);
            runtimeDirectory = runtimeDirectory_;
            dataDirectory = dataDirectory_;
            engineVersion = engineVersion_;
        }
        if (!runtime_.Ensure(interop_.Device(), runtimeDirectory, dataDirectory, engineVersion, reason))
            return false;
    }
    if (!EnsureKernels(device, reason)) return false;
    if (!EnsureBlackMask(device, reason)) return false;
    return Context4(context, reason);
}

bool NeuralRenderingStage::EnsureKernels(ID3D11Device* device, std::string& reason)
{
    if (kernel_ && kernel_->shader && kernelDevice_ == device) return true;
    kernel_.reset(new Kernel());
    kernelDevice_ = nullptr;
    if (kNeuralMaskCsoSize == 0)
    {
        reason = "missing kernel bytecode: neural_mask";
        return false;
    }
    if (FAILED(device->CreateComputeShader(kNeuralMaskCso, kNeuralMaskCsoSize, nullptr, &kernel_->shader)) ||
        !kernel_->shader)
    {
        reason = "CreateComputeShader failed: neural_mask";
        return false;
    }
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = 16;
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &kernel_->constants)) || !kernel_->constants)
    {
        reason = "the neural composite constants could not be created";
        kernel_->shader.Reset();
        return false;
    }
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sampler, &kernel_->sampler)) || !kernel_->sampler)
    {
        reason = "the neural composite sampler could not be created";
        kernel_->shader.Reset();
        kernel_->constants.Reset();
        return false;
    }
    kernelDevice_ = device;
    return true;
}

bool NeuralRenderingStage::EnsureBlackMask(ID3D11Device* device, std::string& reason)
{
    if (blackMask_ && blackMaskSrv_ && blackMaskDevice_ == device) return true;
    blackMask_.Reset();
    blackMaskSrv_.Reset();
    blackMaskDevice_ = nullptr;
    const uint8_t clear = 0;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = &clear;
    data.SysMemPitch = 1;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = 1;
    description.Height = 1;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&description, &data, &blackMask_)) || !blackMask_)
    {
        reason = "the empty character mask could not be created";
        return false;
    }
    if (FAILED(device->CreateShaderResourceView(blackMask_.Get(), nullptr, &blackMaskSrv_)) || !blackMaskSrv_)
    {
        reason = "the empty character mask view could not be created";
        blackMask_.Reset();
        return false;
    }
    blackMaskDevice_ = device;
    return true;
}

bool NeuralRenderingStage::Context4(ID3D11DeviceContext* context, std::string& reason)
{
    if (context4Source_ == context && context4_) return true;
    context4_.Reset();
    context4Source_ = nullptr;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context4_))) || !context4_)
    {
        reason = "the game context is not an ID3D11DeviceContext4";
        return false;
    }
    context4Source_ = context;
    return true;
}

NeuralRenderingStage::View* NeuralRenderingStage::AcquireView(const ViewKey& key, std::string& reason)
{
    for (View* view : views_)
    {
        if (view->key == key) return view;
    }
    if (views_.size() >= static_cast<size_t>(kMaxViews))
    {
        // A third view can only appear if the camera set changed; the least recently used one is
        // retired, which needs its work to have completed.
        size_t victim = 0;
        for (size_t index = 1; index < views_.size(); index++)
        {
            if (views_[index]->lastUse < views_[victim]->lastUse) victim = index;
        }
        if (!interop_.WaitIdle(kIdleTimeoutMilliseconds, reason))
        {
            reason = "the GPU did not become idle before a neural view was retired: " + reason;
            return nullptr;
        }
        View* retired = views_[victim];
        ReleaseFeatures(*retired);
        ReleaseTextures(*retired);
        delete retired;
        views_.erase(views_.begin() + static_cast<std::ptrdiff_t>(victim));
    }
    View* view = new View();
    view->key = key;
    view->lastUse = ++useCounter_;
    views_.push_back(view);
    return view;
}

bool NeuralRenderingStage::NeedsRebuild(const View& view, const Snapshot& snapshot, const Geometry& geometry) const
{
    return view.generation != snapshot.generation || view.style != snapshot.style ||
           view.passCount != static_cast<uint32_t>(snapshot.passes) ||
           view.geometry.colourWidth != geometry.colourWidth ||
           view.geometry.colourHeight != geometry.colourHeight ||
           view.geometry.guideWidth != geometry.guideWidth ||
           view.geometry.guideHeight != geometry.guideHeight;
}

bool NeuralRenderingStage::EnsureTextures(ID3D11Device* device, View& view, const Geometry& geometry,
    std::string& reason)
{
    if (!CreateSharedTexture(device, interop_, view.colour, geometry.colourWidth, geometry.colourHeight,
            DXGI_FORMAT_R16G16B16A16_FLOAT, false, "colour input", reason) ||
        !CreateSharedTexture(device, interop_, view.depth, geometry.guideWidth, geometry.guideHeight,
            DXGI_FORMAT_R32_FLOAT, false, "depth input", reason) ||
        !CreateSharedTexture(device, interop_, view.motion, geometry.guideWidth, geometry.guideHeight,
            DXGI_FORMAT_R16G16_FLOAT, false, "motion input", reason) ||
        !CreateSharedTexture(device, interop_, view.output[0], geometry.colourWidth, geometry.colourHeight,
            DXGI_FORMAT_R16G16B16A16_FLOAT, true, "model output 0", reason) ||
        !CreateSharedTexture(device, interop_, view.output[1], geometry.colourWidth, geometry.colourHeight,
            DXGI_FORMAT_R16G16B16A16_FLOAT, true, "model output 1", reason))
        return false;

    view.colourSrv.Reset();
    if (FAILED(device->CreateShaderResourceView(view.colour.d3d11.Get(), nullptr, &view.colourSrv)) ||
        !view.colourSrv)
    {
        reason = "the neural colour view could not be created";
        return false;
    }
    for (int index = 0; index < 2; index++)
    {
        view.outputSrv[index].Reset();
        if (FAILED(device->CreateShaderResourceView(view.output[index].d3d11.Get(), nullptr,
                &view.outputSrv[index])) ||
            !view.outputSrv[index])
        {
            reason = "the neural model output view could not be created";
            return false;
        }
    }

    // The composite writes straight into this texture, which is also the image returned to the
    // super-resolution backend before upscaling; it never leaves the D3D11 device.
    D3D11_TEXTURE2D_DESC description{};
    description.Width = geometry.colourWidth;
    description.Height = geometry.colourHeight;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    view.result.Reset();
    view.resultUav.Reset();
    if (FAILED(device->CreateTexture2D(&description, nullptr, &view.result)) || !view.result)
    {
        reason = "the neural result texture could not be created";
        return false;
    }
    if (FAILED(device->CreateUnorderedAccessView(view.result.Get(), nullptr, &view.resultUav)) ||
        !view.resultUav)
    {
        reason = "the neural result view could not be created";
        return false;
    }
    return true;
}

bool NeuralRenderingStage::EnsureMask(ID3D11Device* device, View& view, uint32_t width, uint32_t height,
    std::string& reason)
{
    if (width == 0 || height == 0)
    {
        view.maskWidth = 0;
        view.maskHeight = 0;
        return true;
    }
    if (view.mask.d3d11 && view.maskSrv && view.maskWidth == width && view.maskHeight == height) return true;
    if (!CreateSharedTexture(device, interop_, view.mask, width, height, DXGI_FORMAT_R8_UNORM, false,
            "character mask", reason))
        return false;
    view.maskSrv.Reset();
    if (FAILED(device->CreateShaderResourceView(view.mask.d3d11.Get(), nullptr, &view.maskSrv)) ||
        !view.maskSrv)
    {
        reason = "the neural character mask view could not be created";
        return false;
    }
    view.maskWidth = width;
    view.maskHeight = height;
    return true;
}

bool NeuralRenderingStage::CopyInputs(ID3D11DeviceContext* context, View& view, const EvaluationRequest& request,
    ID3D11Texture2D* colourSource, const Geometry& geometry, ID3D11Texture2D* maskTexture, bool useMask,
    std::string& reason)
{
    ID3D11Texture2D* depthSource = static_cast<ID3D11Texture2D*>(request.depth);
    ID3D11Texture2D* motionSource = static_cast<ID3D11Texture2D*>(request.motion);
    D3D11_TEXTURE2D_DESC description{};
    depthSource->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_R32_FLOAT || description.Width < geometry.guideWidth ||
        description.Height < geometry.guideHeight)
    {
        reason = "the neural depth input is not R32F at the guide extent";
        return false;
    }
    motionSource->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_R16G16_FLOAT || description.Width < geometry.guideWidth ||
        description.Height < geometry.guideHeight)
    {
        reason = "the neural motion input is not RG16F at the guide extent";
        return false;
    }

    D3D11_BOX box{};
    box.left = 0;
    box.top = 0;
    box.front = 0;
    box.right = geometry.colourWidth;
    box.bottom = geometry.colourHeight;
    box.back = 1;
    context->CopySubresourceRegion(view.colour.d3d11.Get(), 0, 0, 0, 0, colourSource, 0, &box);

    box.right = geometry.guideWidth;
    box.bottom = geometry.guideHeight;
    context->CopySubresourceRegion(view.depth.d3d11.Get(), 0, 0, 0, 0, depthSource, 0, &box);
    context->CopySubresourceRegion(view.motion.d3d11.Get(), 0, 0, 0, 0, motionSource, 0, &box);

    if (useMask && maskTexture != nullptr && view.mask.d3d11)
    {
        D3D11_BOX maskBox{};
        maskBox.left = 0;
        maskBox.top = 0;
        maskBox.front = 0;
        maskBox.right = view.maskWidth;
        maskBox.bottom = view.maskHeight;
        maskBox.back = 1;
        // The engine mask is copied into a one-mip, one-slice texture owned by this stage, so the
        // model answer and the mask can never disagree about the resource shape.
        context->CopySubresourceRegion(view.mask.d3d11.Get(), 0, 0, 0, 0, maskTexture, 0, &maskBox);
    }
    return true;
}

bool NeuralRenderingStage::CreateFeatureFrame(ID3D11DeviceContext* context, View& view,
    const Snapshot& snapshot, const Geometry& geometry, std::string& reason)
{
    (void)context;
    // One model per submission. The compatibility runtime keeps large CUDA state per feature; two
    // creations batched into a single command list on this driver left the D3D12 device unusable
    // after the list retired. Submitting and draining each creation keeps every feature's setup
    // inside its own completed command buffer and costs nothing per frame (creation happens once
    // per view and configuration).
    for (int32_t pass = 0; pass < snapshot.passes; pass++)
    {
        uint64_t sourceFenceValue = 0;
        std::string status;
        if (!interop_.SignalFromD3D11(context4_.Get(), status, sourceFenceValue) || !interop_.Begin(status))
        {
            reason = "the neural submission could not start: " + status;
            return false;
        }

        ID3D12GraphicsCommandList* list = interop_.List();
        Dx12Interop::Transition(list, view.colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Dx12Interop::Transition(list, view.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Dx12Interop::Transition(list, view.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (view.mask.d3d12)
            Dx12Interop::Transition(list, view.mask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Dx12Interop::Transition(list, view.output[0], D3D12_RESOURCE_STATE_COMMON);
        Dx12Interop::Transition(list, view.output[1], D3D12_RESOURCE_STATE_COMMON);

        NeuralTuning tuning;
        tuning.style = snapshot.style;
        tuning.strength = snapshot.strength;
        tuning.detail = snapshot.detail;
        // The reference contract applies the tone strength to the first layer only.
        tuning.tone = pass == 0 ? snapshot.tone : 0.0f;
        const bool created = runtime_.CreateFeature(list, geometry.colourWidth, geometry.colourHeight, tuning,
            view.passes[pass].feature, reason);

        Dx12Interop::Transition(list, view.colour, D3D12_RESOURCE_STATE_COMMON);
        Dx12Interop::Transition(list, view.depth, D3D12_RESOURCE_STATE_COMMON);
        Dx12Interop::Transition(list, view.motion, D3D12_RESOURCE_STATE_COMMON);
        if (view.mask.d3d12)
            Dx12Interop::Transition(list, view.mask, D3D12_RESOURCE_STATE_COMMON);
        Dx12Interop::Transition(list, view.output[0], D3D12_RESOURCE_STATE_COMMON);
        Dx12Interop::Transition(list, view.output[1], D3D12_RESOURCE_STATE_COMMON);

        // A failed creation still closes and submits this list so the next packet starts from a clean
        // ring slot; every feature already created stays owned by the view and is released with it.
        const bool submitted = interop_.Submit(status, sourceFenceValue);
        if (!created) return false;
        if (!submitted || !interop_.WaitOnD3D11(context4_.Get(), status))
        {
            reason = "the neural feature creation submission did not complete: " + status;
            return false;
        }
    }
    return true;
}

bool NeuralRenderingStage::EvaluateFrame(ID3D11DeviceContext* context, View& view, const EvaluationRequest& request,
    const Snapshot& snapshot, const Geometry& geometry, std::string& reason)
{
    (void)context;
    uint64_t sourceFenceValue = 0;
    std::string status;
    if (!interop_.SignalFromD3D11(context4_.Get(), status, sourceFenceValue) || !interop_.Begin(status))
    {
        reason = "the neural submission could not start: " + status;
        return false;
    }

    ID3D12GraphicsCommandList* list = interop_.List();
    Dx12Interop::Transition(list, view.colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Dx12Interop::Transition(list, view.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Dx12Interop::Transition(list, view.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    bool evaluated = true;
    for (int32_t pass = 0; pass < snapshot.passes && evaluated; pass++)
    {
        Dx12Interop::Shared& source = pass == 0 ? view.colour : view.output[(pass - 1) % 2];
        Dx12Interop::Shared& target = view.output[pass % 2];
        Dx12Interop::Transition(list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        NeuralEvaluation evaluation;
        evaluation.color = source.d3d12.Get();
        evaluation.depth = view.depth.d3d12.Get();
        evaluation.motion = view.motion.d3d12.Get();
        evaluation.output = target.d3d12.Get();
        evaluation.width = geometry.colourWidth;
        evaluation.height = geometry.colourHeight;
        evaluation.guideWidth = geometry.guideWidth;
        evaluation.guideHeight = geometry.guideHeight;
        evaluation.depthInverted = request.reversedDepth;
        evaluation.motionScaleX = request.motionScaleX;
        evaluation.motionScaleY = request.motionScaleY;

        Pass& state = view.passes[pass];
        const bool firstUse = state.lastFrame < 0;
        const bool epochChange = state.lastEpoch != request.historyEpoch;
        const bool frameGap = state.lastFrame >= 0 &&
            (request.frameIndex < state.lastFrame || request.frameIndex - state.lastFrame > kMaxFrameGap);
        evaluation.reset = request.reset || firstUse || epochChange || frameGap;

        evaluated = runtime_.EvaluateFeature(list, state.feature, evaluation, reason);
        // The runtime writes the model output as a UAV. D3D12 needs an explicit UAV barrier before
        // the next pass samples the same resource; a state transition alone is not a substitute for
        // a UAV-to-UAV hazard barrier.
        D3D12_RESOURCE_BARRIER uavBarrier{};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = target.d3d12.Get();
        list->ResourceBarrier(1, &uavBarrier);
        Dx12Interop::Transition(list, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (evaluated)
        {
            state.lastFrame = request.frameIndex;
            state.lastEpoch = request.historyEpoch;
            state.evaluations++;
        }
    }

    // Both ping-pong slots return to COMMON: the next frame copies into the colour input and
    // transitions whichever slot it writes.
    Dx12Interop::Transition(list, view.output[0], D3D12_RESOURCE_STATE_COMMON);
    Dx12Interop::Transition(list, view.output[1], D3D12_RESOURCE_STATE_COMMON);
    Dx12Interop::Transition(list, view.colour, D3D12_RESOURCE_STATE_COMMON);
    Dx12Interop::Transition(list, view.depth, D3D12_RESOURCE_STATE_COMMON);
    Dx12Interop::Transition(list, view.motion, D3D12_RESOURCE_STATE_COMMON);

    const bool submitted = interop_.Submit(status, sourceFenceValue);
    if (!submitted || !interop_.WaitOnD3D11(context4_.Get(), status))
    {
        reason = "the neural evaluation submission did not complete: " + status;
        return false;
    }
    return evaluated;
}

bool NeuralRenderingStage::Composite(ID3D11DeviceContext* context, View& view, const Geometry& geometry,
    int32_t finalOutput, bool hasMask, std::string& reason)
{
    if (!kernel_ || !kernel_->shader || !kernel_->constants || !kernel_->sampler)
    {
        reason = "the neural composite kernel is not ready";
        return false;
    }
    if (finalOutput < 0 || finalOutput > 1 || !view.outputSrv[finalOutput] || !view.colourSrv || !view.resultUav)
    {
        reason = "the neural composite has no model answer";
        return false;
    }

    struct Constants
    {
        uint32_t width;
        uint32_t height;
        float hasMask;
        float pad;
    } constants{geometry.colourWidth, geometry.colourHeight, hasMask ? 1.0f : 0.0f, 0.0f};

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(kernel_->constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        reason = "the neural composite constants could not be mapped";
        return false;
    }
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(kernel_->constants.Get(), 0);

    ID3D11ShaderResourceView* resources[3] = {
        view.outputSrv[finalOutput].Get(),
        view.colourSrv.Get(),
        hasMask ? view.maskSrv.Get() : blackMaskSrv_.Get()};
    ID3D11UnorderedAccessView* targets[1] = {view.resultUav.Get()};
    if (resources[0] == nullptr || resources[1] == nullptr || resources[2] == nullptr || targets[0] == nullptr)
    {
        reason = "the neural composite is missing a view";
        return false;
    }

    ID3D11Buffer* constantBuffer = kernel_->constants.Get();
    ID3D11SamplerState* samplers[1] = {kernel_->sampler.Get()};
    context->CSSetShader(kernel_->shader.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &constantBuffer);
    context->CSSetShaderResources(0, 3, resources);
    context->CSSetUnorderedAccessViews(0, 1, targets, nullptr);
    context->CSSetSamplers(0, 1, samplers);
    context->Dispatch((geometry.colourWidth + 7) / 8, (geometry.colourHeight + 7) / 8, 1);

    // Nothing this dispatch bound stays on Unity's context. The bridge saves and restores the rest.
    ID3D11ShaderResourceView* noResources[3] = {nullptr, nullptr, nullptr};
    ID3D11UnorderedAccessView* noTargets[1] = {nullptr};
    ID3D11Buffer* noBuffer[1] = {nullptr};
    ID3D11SamplerState* noSampler[1] = {nullptr};
    context->CSSetShaderResources(0, 3, noResources);
    context->CSSetUnorderedAccessViews(0, 1, noTargets, nullptr);
    context->CSSetConstantBuffers(0, 1, noBuffer);
    context->CSSetSamplers(0, 1, noSampler);
    context->CSSetShader(nullptr, nullptr, 0);
    return true;
}

void NeuralRenderingStage::ReleaseFeatures(View& view)
{
    for (Pass& pass : view.passes)
    {
        runtime_.ReleaseFeature(pass.feature);
        pass.lastFrame = -1;
        pass.lastEpoch = 0;
        pass.evaluations = 0;
    }
}

void NeuralRenderingStage::ReleaseTextures(View& view)
{
    interop_.Release(view.colour);
    interop_.Release(view.depth);
    interop_.Release(view.motion);
    interop_.Release(view.mask);
    interop_.Release(view.output[0]);
    interop_.Release(view.output[1]);
    view.colourSrv.Reset();
    view.maskSrv.Reset();
    view.outputSrv[0].Reset();
    view.outputSrv[1].Reset();
    view.result.Reset();
    view.resultUav.Reset();
    view.maskWidth = 0;
    view.maskHeight = 0;
}

void NeuralRenderingStage::Fail(const std::string& reason)
{
    lastReason_ = reason;
    if (reason != lastFailure_)
    {
        lastFailure_ = reason;
        Log("neural: %s", reason.c_str());
    }
    SetState(BIGSCALE_NEURAL_FAILED);
}

void NeuralRenderingStage::NoteOnce(const std::string& message)
{
    if (message == lastFailure_) return;
    lastFailure_ = message;
    Log("neural: %s", message.c_str());
}

void NeuralRenderingStage::SetState(int32_t state)
{
    const int32_t previous = state_.exchange(state);
    if (previous != state) Log("neural: state %s -> %s", StateName(previous), StateName(state));
}

void NeuralRenderingStage::Release()
{
    std::lock_guard<std::mutex> guard(frameMutex_);
    for (View* view : views_)
    {
        ReleaseFeatures(*view);
        ReleaseTextures(*view);
        delete view;
    }
    views_.clear();
    runtime_.Release();
    interop_.ReleaseAll();
    kernel_.reset();
    kernelDevice_ = nullptr;
    blackMask_.Reset();
    blackMaskSrv_.Reset();
    blackMaskDevice_ = nullptr;
    context4_.Reset();
    context4Source_ = nullptr;
    runtimeFailedGeneration_ = 0xffffffffu;
    runtimeRetryFrame_ = 0;
    bool enabled = false;
    {
        std::lock_guard<std::mutex> configGuard(configMutex_);
        enabled = config_.enabled != 0;
    }
    SetState(enabled ? BIGSCALE_NEURAL_WAITING : BIGSCALE_NEURAL_OFF);
}
}
