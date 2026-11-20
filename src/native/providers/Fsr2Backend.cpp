#include "Fsr2Backend.h"

#include "../Log.h"

#include <ffx_fsr2.h>
#include <dx11/ffx_fsr2_dx11.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace bigscale
{
namespace
{
// Per-axis ratios reported by the FSR2 SDK documentation and helpers.
float RatioForPreset(Preset preset)
{
    switch (preset)
    {
        case Preset::Quality: return 1.5f;
        case Preset::Balanced: return 1.7f;
        case Preset::Performance: return 2.0f;
        case Preset::UltraPerformance: return 3.0f;
        case Preset::NativeAA: return 1.0f;
    }
    return 1.5f;
}

uint32_t RoundDownRatio(uint32_t size, float ratio)
{
    const float value = static_cast<float>(size) / ratio;
    return static_cast<uint32_t>(value + 0.5f);
}

uint32_t PackVersion()
{
    return (static_cast<uint32_t>(FFX_FSR2_VERSION_MAJOR) << 24) |
           (static_cast<uint32_t>(FFX_FSR2_VERSION_MINOR) << 16) |
           static_cast<uint32_t>(FFX_FSR2_VERSION_PATCH);
}

FfxResource Resource(const FfxFsr2Context* context, ID3D11Resource* resource, const wchar_t* name,
                     FfxResourceStates state = FFX_RESOURCE_STATE_COMPUTE_READ)
{
    return ffxGetResourceDX11(const_cast<FfxFsr2Context*>(context), resource, name, state);
}
}

struct Fsr2Backend::View
{
    ViewKey key{};
    FfxFsr2Context context{};
    void* scratch = nullptr;
    size_t scratchSize = 0;
    uint32_t maxRenderWidth = 0;
    uint32_t maxRenderHeight = 0;
    uint32_t displayWidth = 0;
    uint32_t displayHeight = 0;
    bool created = false;
    uint32_t evaluations = 0;
};

Fsr2Backend::~Fsr2Backend() { Release(); }

bool Fsr2Backend::Attach(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (device == nullptr || context == nullptr) return false;
    if (device_ != device)
    {
        Release();
        device_ = device;
    }
    context_ = context;
    runtimeVersion_ = PackVersion();
    ready_ = true;
    return true;
}

bool Fsr2Backend::Available()
{
    if (!ready_ || device_ == nullptr || context_ == nullptr)
    {
        lastReason_ = "the FSR 2 backend is not attached to the D3D11 device";
        return false;
    }
    const D3D_FEATURE_LEVEL level = device_->GetFeatureLevel();
    if (level < D3D_FEATURE_LEVEL_11_0)
    {
        lastReason_ = "FSR 2 requires Direct3D feature level 11.0";
        return false;
    }
    lastReason_.clear();
    return true;
}

bool Fsr2Backend::QueryModes(uint32_t outputWidth, uint32_t outputHeight, ModeRange (&ranges)[kModeCount],
                             uint32_t& runtimeVersion)
{
    runtimeVersion = runtimeVersion_;
    for (int i = 0; i < kModeCount; i++) ranges[i] = ModeRange{};
    if (!Available() || outputWidth == 0 || outputHeight == 0) return false;

    bool anySupported = false;
    for (int i = 0; i < kModeCount; i++)
    {
        const auto preset = static_cast<Preset>(i + 1);
        const float ratio = RatioForPreset(preset);
        const uint32_t width = RoundDownRatio(outputWidth, ratio);
        const uint32_t height = RoundDownRatio(outputHeight, ratio);
        // FSR2 has no vendor size query; the ratio is the documented recommendation and the SDK
        // accepts any render size that fits the created context. A degenerate size is unsupported.
        if (width < 32 || height < 32 || width > outputWidth || height > outputHeight) continue;
        ModeRange& range = ranges[i];
        range.supported = true;
        range.optimalWidth = width;
        range.optimalHeight = height;
        range.minWidth = 1;
        range.minHeight = 1;
        range.maxWidth = outputWidth;
        range.maxHeight = outputHeight;
        anySupported = true;
    }
    if (!anySupported) lastReason_ = "FSR 2 cannot size any preset at this output resolution";
    return anySupported;
}

Fsr2Backend::View* Fsr2Backend::Find(const ViewKey& key)
{
    for (View* view : views_)
        if (view->key == key) return view;
    return nullptr;
}

Fsr2Backend::View* Fsr2Backend::EnsureView(const ViewKey& key)
{
    if (View* existing = Find(key)) return existing;
    if (views_.size() >= static_cast<size_t>(kMaxViews))
    {
        // The bridge recycles views before this point; refuse rather than silently share history.
        lastReason_ = "more FSR 2 views than the bridge supports";
        return nullptr;
    }
    auto* view = new View();
    view->key = key;
    views_.push_back(view);
    return view;
}

bool Fsr2Backend::EnsureContext(View& view, const EvaluationRequest& request)
{
    if (view.created && view.maxRenderWidth == request.scratchWidth &&
        view.maxRenderHeight == request.scratchHeight && view.displayWidth == request.outputWidth &&
        view.displayHeight == request.outputHeight)
        return true;

    Destroy(view);
    view.scratchSize = ffxFsr2GetScratchMemorySizeDX11();
    view.scratch = std::malloc(view.scratchSize);
    if (view.scratch != nullptr) std::memset(view.scratch, 0, view.scratchSize);
    if (view.scratch == nullptr)
    {
        lastReason_ = "FSR 2 scratch allocation failed";
        return false;
    }

    FfxFsr2ContextDescription description{};
    description.flags = 0;
    if (request.hdrColor) description.flags |= FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE;
    if (request.reversedDepth) description.flags |= FFX_FSR2_ENABLE_DEPTH_INVERTED;
    description.maxRenderSize = {request.scratchWidth, request.scratchHeight};
    description.displaySize = {request.outputWidth, request.outputHeight};
    description.device = ffxGetDeviceDX11(device_);
    const FfxErrorCode interfaceResult = ffxFsr2GetInterfaceDX11(&description.callbacks, device_,
        view.scratch, view.scratchSize);
    if (interfaceResult != FFX_OK)
    {
        lastReason_ = "ffxFsr2GetInterfaceDX11 failed";
        Destroy(view);
        return false;
    }

    const FfxErrorCode createResult = ffxFsr2ContextCreate(&view.context, &description);
    if (createResult != FFX_OK)
    {
        lastReason_ = "ffxFsr2ContextCreate failed";
        Destroy(view);
        return false;
    }
    view.created = true;
    view.maxRenderWidth = request.scratchWidth;
    view.maxRenderHeight = request.scratchHeight;
    view.displayWidth = request.outputWidth;
    view.displayHeight = request.outputHeight;
    Log("FSR2 context created camera=%d eye=%d render=%ux%u display=%ux%u flags=0x%x", view.key.cameraId,
        view.key.eye, view.maxRenderWidth, view.maxRenderHeight, view.displayWidth, view.displayHeight,
        description.flags);
    return true;
}

int32_t Fsr2Backend::Evaluate(const ViewKey& key, const EvaluationRequest& request, bool& featureCreated)
{
    featureCreated = false;
    if (!Available()) return -1;
    View* view = EnsureView(key);
    if (view == nullptr) return -1;
    if (!EnsureContext(*view, request)) return -1;
    featureCreated = view->evaluations == 0;

    FfxFsr2DispatchDescription dispatch{};
    dispatch.commandList = reinterpret_cast<FfxCommandList>(context_);
    dispatch.color = Resource(&view->context, request.color, L"FSR2_Color");
    dispatch.depth = Resource(&view->context, request.depth, L"FSR2_Depth");
    dispatch.motionVectors = Resource(&view->context, request.motion, L"FSR2_Motion");
    dispatch.exposure = Resource(&view->context, nullptr, L"FSR2_Exposure");
    dispatch.reactive = Resource(&view->context, nullptr, L"FSR2_Reactive");
    dispatch.transparencyAndComposition = Resource(&view->context, nullptr, L"FSR2_Transparency");
    dispatch.output = Resource(&view->context, request.output, L"FSR2_Output",
        FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.jitterOffset = {request.jitterX, request.jitterY};
    // Raw current-to-previous UV displacement, top-down on D3D: FSR2 wants it negated and scaled
    // by the render size.
    dispatch.motionVectorScale = {static_cast<float>(request.inputWidth) * -1.0f,
        static_cast<float>(request.inputHeight) * -1.0f};
    dispatch.renderSize = {request.inputWidth, request.inputHeight};
    dispatch.enableSharpening = false;
    dispatch.sharpness = 0.0f;
    dispatch.frameTimeDelta = request.deltaMilliseconds;
    dispatch.preExposure = 1.0f;
    dispatch.reset = request.reset || featureCreated;
    dispatch.cameraNear = request.cameraNear;
    dispatch.cameraFar = request.cameraFar;
    dispatch.cameraFovAngleVertical = request.verticalFovRadians;
    dispatch.viewSpaceToMetersFactor = 1.0f;

    const FfxErrorCode result = ffxFsr2ContextDispatch(&view->context, &dispatch);
    if (result != FFX_OK)
    {
        lastReason_ = "ffxFsr2ContextDispatch failed";
        Log("FSR2 dispatch failed camera=%d eye=%d code=%d", key.cameraId, key.eye, static_cast<int>(result));
        return static_cast<int32_t>(result);
    }
    view->evaluations++;
    if (view->evaluations == 1 || view->evaluations % 600 == 0)
        Log("FSR2 dispatch ok camera=%d eye=%d render=%ux%u display=%ux%u reset=%d", key.cameraId, key.eye,
            request.inputWidth, request.inputHeight, request.outputWidth, request.outputHeight,
            request.reset ? 1 : 0);
    return 0;
}

void Fsr2Backend::Destroy(View& view)
{
    if (view.created)
    {
        ffxFsr2ContextDestroy(&view.context);
        view.created = false;
    }
    if (view.scratch != nullptr)
    {
        std::free(view.scratch);
        view.scratch = nullptr;
        view.scratchSize = 0;
    }
}

void Fsr2Backend::Forget(const ViewKey& key)
{
    for (size_t i = 0; i < views_.size(); i++)
    {
        if (!(views_[i]->key == key)) continue;
        View* view = views_[i];
        views_.erase(views_.begin() + static_cast<std::ptrdiff_t>(i));
        Destroy(*view);
        delete view;
        return;
    }
}

void Fsr2Backend::Release()
{
    for (View* view : views_)
    {
        Destroy(*view);
        delete view;
    }
    views_.clear();
    ready_ = false;
    context_ = nullptr;
    device_ = nullptr;
}
}
