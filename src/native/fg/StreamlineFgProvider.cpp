#include "StreamlineFgProvider.h"

#include "../Log.h"
#include "FgMath.h"
#include "StreamlinePatch.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace bigscale
{
using Microsoft::WRL::ComPtr;

namespace
{
constexpr uint32_t kSwapchainBuffers = 3;
constexpr uint32_t kViewportId = 0;

std::wstring Join(const std::wstring& directory, const wchar_t* leaf)
{
    if (directory.empty()) return leaf;
    const wchar_t last = directory.back();
    return last == L'\\' || last == L'/' ? directory + leaf : directory + L"\\" + leaf;
}

void OnStreamlineLog(sl::LogType type, const char* message)
{
    if (message == nullptr) return;
    if (type == sl::LogType::eError || type == sl::LogType::eWarn)
        Log("Streamline: %s", message);
}

void CopyMatrix(const FgMatrix& source, sl::float4x4& destination)
{
    for (int row = 0; row < 4; ++row)
    {
        destination.row[row].x = source.m[row][0];
        destination.row[row].y = source.m[row][1];
        destination.row[row].z = source.m[row][2];
        destination.row[row].w = source.m[row][3];
    }
}
}  // namespace

StreamlineFgProvider::Api& StreamlineFgProvider::SharedApi()
{
    static Api api;
    return api;
}

HMODULE& StreamlineFgProvider::SharedModule()
{
    static HMODULE module = nullptr;
    return module;
}

bool& StreamlineFgProvider::SharedInitialized()
{
    static bool initialized = false;
    return initialized;
}

std::mutex& StreamlineFgProvider::SharedMutex()
{
    static std::mutex mutex;
    return mutex;
}

ID3D12Device*& StreamlineFgProvider::SharedDevice()
{
    static ID3D12Device* device = nullptr;
    return device;
}

uint32_t& StreamlineFgProvider::SharedFrameIndex()
{
    static uint32_t index = 0;
    return index;
}

StreamlineFgProvider::StreamlineFgProvider() = default;
StreamlineFgProvider::~StreamlineFgProvider() { ReleaseSwapchain(); }

bool StreamlineFgProvider::LoadStreamline(std::string& reason)
{
    if (interposer_ != nullptr) return true;
    // The detours must exist before the interposer maps its plugins, and before the first provider
    // module is loaded by NGX.
    if (!streamlinepatch::Install(reason)) return false;
    const std::wstring path = Join(runtimeDirectory_, L"sl.interposer.dll");
    interposer_ = LoadLibraryW(path.c_str());
    if (interposer_ == nullptr)
    {
        reason = "sl.interposer.dll is missing from the plugin directory";
        return false;
    }
    return ResolveCoreApi(reason);
}

bool StreamlineFgProvider::ResolveCoreApi(std::string& reason)
{
    auto resolve = [this](const char* name) -> FARPROC {
        return GetProcAddress(interposer_, name);
    };
    api_.init = reinterpret_cast<PFun_slInit*>(resolve("slInit"));
    api_.shutdown = reinterpret_cast<PFun_slShutdown*>(resolve("slShutdown"));
    api_.setDevice = reinterpret_cast<PFun_slSetD3DDevice*>(resolve("slSetD3DDevice"));
    api_.isFeatureSupported =
        reinterpret_cast<PFun_slIsFeatureSupported*>(resolve("slIsFeatureSupported"));
    api_.setFeatureLoaded = reinterpret_cast<PFun_slSetFeatureLoaded*>(resolve("slSetFeatureLoaded"));
    api_.getFeatureFunction =
        reinterpret_cast<PFun_slGetFeatureFunction*>(resolve("slGetFeatureFunction"));
    api_.getFeatureRequirements =
        reinterpret_cast<PFun_slGetFeatureRequirements*>(resolve("slGetFeatureRequirements"));
    api_.upgradeInterface = reinterpret_cast<PFun_slUpgradeInterface*>(resolve("slUpgradeInterface"));
    api_.getNativeInterface =
        reinterpret_cast<PFun_slGetNativeInterface*>(resolve("slGetNativeInterface"));
    api_.getNewFrameToken =
        reinterpret_cast<PFun_slGetNewFrameToken*>(resolve("slGetNewFrameToken"));
    api_.setConstants = reinterpret_cast<PFun_slSetConstants*>(resolve("slSetConstants"));
    api_.setTagForFrame = reinterpret_cast<PFun_slSetTagForFrame*>(resolve("slSetTagForFrame"));
    if (api_.init == nullptr || api_.shutdown == nullptr || api_.setDevice == nullptr ||
        api_.isFeatureSupported == nullptr || api_.getFeatureFunction == nullptr ||
        api_.getFeatureRequirements == nullptr || api_.upgradeInterface == nullptr ||
        api_.getNativeInterface == nullptr || api_.getNewFrameToken == nullptr ||
        api_.setConstants == nullptr || api_.setTagForFrame == nullptr)
    {
        reason = "sl.interposer.dll does not export the core Streamline API";
        return false;
    }
    return true;
}

bool StreamlineFgProvider::ResolveFeatureApi(std::string& reason)
{
    auto feature = [this](sl::Feature id, const char* name, void** target) {
        return api_.getFeatureFunction(id, name, *target) == sl::Result::eOk;
    };
    if (!feature(sl::kFeatureDLSS_G, "slDLSSGSetOptions", reinterpret_cast<void**>(&api_.dlssgSetOptions)) ||
        !feature(sl::kFeatureDLSS_G, "slDLSSGGetState", reinterpret_cast<void**>(&api_.dlssgGetState)))
    {
        reason = "the Streamline DLSS-G plugin is not available";
        return false;
    }
    if (!feature(sl::kFeatureReflex, "slReflexSetOptions",
            reinterpret_cast<void**>(&api_.reflexSetOptions)) ||
        !feature(sl::kFeatureReflex, "slReflexSleep", reinterpret_cast<void**>(&api_.reflexSleep)))
    {
        reason = "the Streamline Reflex plugin is not available";
        return false;
    }
    return true;
}

bool StreamlineFgProvider::Initialize(const FgInit& init, std::string& reason)
{
    if (init.device == nullptr || init.queue == nullptr || init.child == nullptr ||
        init.width == 0 || init.height == 0)
    {
        reason = "frame generation needs a device, a queue and a child window";
        return false;
    }
    device_ = init.device;
    queue_ = init.queue;
    hwnd_ = init.child;
    width_ = init.width;
    height_ = init.height;
    format_ = init.format;
    runtimeDirectory_ = init.runtimeDirectory;

    if (!LoadStreamline(reason)) return false;
    {
        std::lock_guard<std::mutex> guard(SharedMutex());
        if (SharedInitialized())
        {
            api_ = SharedApi();
            interposer_ = SharedModule();
            initialized_ = true;
        }
    }
    if (!initialized_)
    {
        sl::Preferences preferences{};
        preferences.renderAPI = sl::RenderAPI::eD3D12;
        preferences.engine = sl::EngineType::eUnity;
        preferences.engineVersion = init.engineVersion.c_str();
        preferences.projectId = "dce2ec10-c62e-4ec2-a24e-7368ff734a44";
        preferences.logLevel = sl::LogLevel::eVerbose;
        preferences.logMessageCallback = &OnStreamlineLog;
        const wchar_t* pluginPaths[] = {runtimeDirectory_.c_str()};
        preferences.pathsToPlugins = pluginPaths;
        preferences.numPathsToPlugins = 1;
        if (!init.dataDirectory.empty()) preferences.pathToLogsAndData = init.dataDirectory.c_str();
        preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
            sl::PreferenceFlags::eUseManualHooking |
            sl::PreferenceFlags::eUseFrameBasedResourceTagging;
        const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex};
        preferences.featuresToLoad = features;
        preferences.numFeaturesToLoad = 2;
        const sl::Result result = api_.init(preferences, sl::kSDKVersion);
        if (result != sl::Result::eOk)
        {
            reason = "Streamline refused to initialize (result " + std::to_string(
                static_cast<int>(result)) + ")";
            return false;
        }
        initialized_ = true;
    }
    {
        std::lock_guard<std::mutex> guard(SharedMutex());
        if (SharedDevice() == nullptr)
        {
            const sl::Result deviceResult = api_.setDevice(device_);
            if (deviceResult != sl::Result::eOk)
            {
                reason = "Streamline refused the frame-generation device (result " +
                    std::to_string(static_cast<int>(deviceResult)) + ")";
                return false;
            }
            SharedDevice() = device_;
        }
        else if (SharedDevice() != device_)
        {
            reason = "Streamline is already bound to a different device in this process";
            return false;
        }
    }

    const LUID adapterLuid = device_->GetAdapterLuid();
    sl::AdapterInfo adapterInfo{};
    adapterInfo.deviceLUID = reinterpret_cast<uint8_t*>(const_cast<LUID*>(&adapterLuid));
    adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);
    const sl::Result supported = api_.isFeatureSupported(sl::kFeatureDLSS_G, adapterInfo);
    if (supported != sl::Result::eOk)
    {
        reason = supported == sl::Result::eErrorOSDisabledHWS
            ? "frame generation needs hardware-accelerated GPU scheduling"
            : "this adapter does not support Streamline frame generation (result " +
                std::to_string(static_cast<int>(supported)) + ")";
        return false;
    }
    if (!ResolveFeatureApi(reason)) return false;
    {
        std::lock_guard<std::mutex> guard(SharedMutex());
        SharedApi() = api_;
        SharedModule() = interposer_;
        SharedInitialized() = true;
    }

    if (!streamlinepatch::ProviderGated())
    {
        const std::string failure = streamlinepatch::LastFailure();
        reason = failure.empty() ? "the MFG unlock gates did not apply to any DLSS-G provider copy"
                                 : failure;
        return false;
    }

    if (!EnsureSwapchain(init, reason)) return false;
    sl::ReflexOptions reflex{};
    reflex.mode = sl::ReflexMode::eLowLatency;
    const sl::Result reflexResult = api_.reflexSetOptions(reflex);
    if (reflexResult != sl::Result::eOk)
    {
        reason = "Reflex could not be enabled for frame generation (result " +
            std::to_string(static_cast<int>(reflexResult)) + ")";
        return false;
    }
    if (!ApplyOptions(init.multiplier, init.width, init.height, DXGI_FORMAT_R16G16_FLOAT,
            DXGI_FORMAT_R32_TYPELESS, reason))
        return false;
    ProbeCapability(reason);
    suspended_ = false;
    Log("Streamline FG: initialized %ux%u, capability %ux", width_, height_,
        maximumMultiplier_ == 0 ? 2u : maximumMultiplier_);
    return true;
}

bool StreamlineFgProvider::EnsureSwapchain(const FgInit& init, std::string& reason)
{
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory)
    {
        reason = "the Streamline presenter factory could not be created";
        return false;
    }
    void* factoryProxy = factory.Get();
    if (api_.upgradeInterface(&factoryProxy) != sl::Result::eOk || factoryProxy == nullptr)
    {
        reason = "Streamline refused to proxy the presenter factory";
        return false;
    }
    proxyFactory_.Attach(static_cast<IDXGIFactory2*>(factoryProxy));
    void* deviceProxy = device_;
    if (api_.upgradeInterface(&deviceProxy) != sl::Result::eOk || deviceProxy == nullptr)
    {
        reason = "Streamline refused to proxy the frame-generation device";
        return false;
    }
    proxyDevice_.Attach(static_cast<ID3D12Device*>(deviceProxy));

    if (proxySwapchain_ != nullptr)
    {
        if (width_ == init.width && height_ == init.height) return true;
        // DLSS-G does not survive ResizeBuffers on a swap chain it has attached to; the documented
        // recovery is a new swap chain, which also restarts Streamline's frame counting.
        ReleaseSwapchain();
    }

    ComPtr<IDXGIFactory5> factory5;
    BOOL tearing = FALSE;
    if (SUCCEEDED(factory.As(&factory5)))
        factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
    tearingSupported_ = tearing != FALSE;
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = init.width;
    description.Height = init.height;
    description.Format = format_;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = kSwapchainBuffers;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (tearingSupported_) description.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain1> swapchain;
    // Streamline intercepts ID3D12Device::CreateCommandQueue; the presenting queue must be created
    // through the proxy device or DLSS-G cannot associate it with the swap chain and refuses to
    // generate more than one intermediate frame.
    if (childQueue_ == nullptr)
    {
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (proxyDevice_ != nullptr &&
            FAILED(proxyDevice_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(childQueue_.GetAddressOf()))))
        {
            reason = "the Streamline presenting queue could not be created";
            return false;
        }
    }
    ID3D12CommandQueue* presentingQueue = childQueue_ != nullptr ? childQueue_.Get() : queue_;
    if (FAILED(proxyFactory_->CreateSwapChainForHwnd(presentingQueue, init.child, &description, nullptr,
            nullptr, &swapchain)) ||
        !swapchain || FAILED(swapchain.As(&proxySwapchain_)) || !proxySwapchain_)
    {
        reason = "the Streamline presenter swapchain could not be created";
        return false;
    }
    proxyFactory_->MakeWindowAssociation(init.child, DXGI_MWA_NO_ALT_ENTER);
    width_ = init.width;
    height_ = init.height;
    // A new proxy swap chain starts a new DLSS-G session with its own frame counting.
    SharedFrameIndex() = 0;
    Log("Streamline FG: presenter swapchain %ux%u, tearing %s", width_, height_,
        tearingSupported_ ? "on" : "unavailable");
    return true;
}

bool StreamlineFgProvider::ApplyOptions(uint32_t multiplier, uint32_t mvecWidth,
    uint32_t mvecHeight, DXGI_FORMAT mvecFormat, DXGI_FORMAT depthFormat, std::string& reason)
{
    if (multiplier < 2) multiplier = 2;
    if (multiplier > 6) multiplier = 6;
    // The transport reports zero guide extents for frames that carry no motion image; reusing the
    // last known extent keeps the options stable instead of re-issuing them every other frame.
    if (mvecWidth == 0) mvecWidth = appliedMvecWidth_ != 0 ? appliedMvecWidth_ : width_;
    if (mvecHeight == 0) mvecHeight = appliedMvecHeight_ != 0 ? appliedMvecHeight_ : height_;
    if (appliedMultiplier_ == multiplier && appliedWidth_ == width_ && appliedHeight_ == height_ &&
        appliedMvecWidth_ == mvecWidth && appliedMvecHeight_ == mvecHeight)
        return true;
    sl::DLSSGOptions options{};
    options.mode = sl::DLSSGMode::eOn;
    options.numFramesToGenerate = multiplier - 1;
    options.numBackBuffers = kSwapchainBuffers;
    // The guide buffers are render sized and can change while the output extent stays fixed; a
    // stale extent makes DLSS-G refuse to interpolate.
    options.mvecDepthWidth = mvecWidth;
    options.mvecDepthHeight = mvecHeight;
    options.colorWidth = width_;
    options.colorHeight = height_;
    options.colorBufferFormat = format_;
    options.mvecBufferFormat = mvecFormat;
    options.depthBufferFormat = depthFormat;
    const sl::Result result = api_.dlssgSetOptions(sl::ViewportHandle(kViewportId), options);
    if (result != sl::Result::eOk)
    {
        reason = "Streamline refused " + std::to_string(multiplier) +
            "x frame generation (result " + std::to_string(static_cast<int>(result)) + ")";
        return false;
    }
    appliedMultiplier_ = multiplier;
    appliedWidth_ = width_;
    appliedHeight_ = height_;
    appliedMvecWidth_ = mvecWidth;
    appliedMvecHeight_ = mvecHeight;
    featureEnabled_ = true;
    Log("Streamline FG: options total=%ux generated=%u color=%ux%u guide=%ux%u", multiplier,
        options.numFramesToGenerate, width_, height_, mvecWidth, mvecHeight);
    return true;
}

bool StreamlineFgProvider::ProbeCapability(std::string& reason)
{
    sl::DLSSGOptions options{};
    options.mode = sl::DLSSGMode::eOn;
    options.numBackBuffers = kSwapchainBuffers;
    sl::DLSSGState state{};
    const sl::Result result = api_.dlssgGetState(sl::ViewportHandle(kViewportId), state, &options);
    if (result != sl::Result::eOk) return false;
    if (state.numFramesToGenerateMax > 0)
        maximumMultiplier_ = std::min<uint32_t>(state.numFramesToGenerateMax + 1, 6);
    (void)reason;
    return true;
}

bool StreamlineFgProvider::UploadRealFrame(FgPresent& present, std::string& reason)
{
    const uint32_t index = proxySwapchain_->GetCurrentBackBufferIndex();
    Microsoft::WRL::ComPtr<ID3D12Resource> target;
    if (FAILED(proxySwapchain_->GetBuffer(index, IID_PPV_ARGS(&target))) || !target)
    {
        reason = "the Streamline back buffer is unavailable";
        return false;
    }
    if (copyAllocator_ == nullptr)
    {
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&copyAllocator_))) ||
            FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                copyAllocator_.Get(), nullptr, IID_PPV_ARGS(&copyList_))))
        {
            reason = "the Streamline frame-copy list could not be created";
            return false;
        }
        copyList_->Close();
        if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyFence_))))
        {
            reason = "the Streamline frame-copy fence could not be created";
            return false;
        }
        copyEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (copyEvent_ == nullptr)
        {
            reason = "the Streamline frame-copy event could not be created";
            return false;
        }
    }
    copyAllocator_->Reset();
    copyList_->Reset(copyAllocator_.Get(), nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = target.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    copyList_->ResourceBarrier(1, &barrier);
    copyList_->CopyResource(target.Get(), present.color);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    copyList_->ResourceBarrier(1, &barrier);
    copyList_->Close();
    ID3D12CommandList* lists[1] = {copyList_.Get()};
    ID3D12CommandQueue* copyQueue = childQueue_ != nullptr ? childQueue_.Get() : queue_;
    copyQueue->ExecuteCommandLists(1, lists);
    const uint64_t value = ++copyValue_;
    if (FAILED(copyQueue->Signal(copyFence_.Get(), value)))
    {
        reason = "the Streamline frame-copy fence could not be signalled";
        return false;
    }
    if (copyFence_->GetCompletedValue() < value)
    {
        copyFence_->SetEventOnCompletion(value, copyEvent_);
        WaitForSingleObject(copyEvent_, INFINITE);
    }
    return true;
}

void StreamlineFgProvider::ApplyConstants(const FgPresent& present)
{
    const BIGSCALE_FG_Frame& frame = present.frame;
    const FgMatrix view = FgMatrix::FromArray(frame.currentView);
    const FgMatrix projection = FgMatrix::FromArray(frame.currentProjection);
    const FgMatrix previousView = FgMatrix::FromArray(frame.previousView);
    const FgMatrix previousProjection = FgMatrix::FromArray(frame.previousProjection);
    const FgMatrix currentVp = Mul(view, projection);
    const FgMatrix previousVp = Mul(previousView, previousProjection);
    FgMatrix inverseCurrent{};
    FgMatrix inversePrevious{};
    FgMatrix inverseProjection{};
    FgMatrix clipToPrev = FgMatrix::Identity();
    FgMatrix prevToClip = FgMatrix::Identity();
    const bool invertible = Inverse(currentVp, inverseCurrent) && Inverse(previousVp, inversePrevious) &&
        Inverse(projection, inverseProjection);
    if (invertible)
    {
        clipToPrev = Mul(inverseCurrent, previousVp);
        prevToClip = Mul(inversePrevious, currentVp);
    }
    else
    {
        // A degenerate first frame must still publish constants: Streamline disables DLSS-G for any
        // present that arrives without them.
        inverseProjection = FgMatrix::Identity();
        static bool warned = false;
        if (!warned)
        {
            Log("Streamline FG: camera matrices are singular; publishing identity reprojection");
            warned = true;
        }
    }

    sl::Constants constants{};
    CopyMatrix(projection, constants.cameraViewToClip);
    CopyMatrix(inverseProjection, constants.clipToCameraView);
    for (int row = 0; row < 4; ++row)
    {
        constants.clipToLensClip.row[row].x = row == 0 ? 1.0f : 0.0f;
        constants.clipToLensClip.row[row].y = row == 1 ? 1.0f : 0.0f;
        constants.clipToLensClip.row[row].z = row == 2 ? 1.0f : 0.0f;
        constants.clipToLensClip.row[row].w = row == 3 ? 1.0f : 0.0f;
    }
    CopyMatrix(clipToPrev, constants.clipToPrevClip);
    CopyMatrix(prevToClip, constants.prevClipToClip);
    const float renderWidth = present.renderWidth == 0 ? static_cast<float>(width_)
                                                       : static_cast<float>(present.renderWidth);
    const float renderHeight = present.renderHeight == 0 ? static_cast<float>(height_)
                                                         : static_cast<float>(present.renderHeight);
    constants.jitterOffset = sl::float2(2.0f * frame.jitterX / renderWidth,
        2.0f * frame.jitterY / renderHeight);
    // The transport hands previous-to-current vectors with y pointing down; Streamline wants the
    // current-to-previous pixel displacement, so both axes are negated by the scale.
    constants.mvecScale = sl::float2(-1.0f / renderWidth, -1.0f / renderHeight);
    constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    constants.cameraPos = sl::float3(frame.cameraPosition[0], frame.cameraPosition[1],
        frame.cameraPosition[2]);
    constants.cameraUp = sl::float3(frame.cameraUp[0], frame.cameraUp[1], frame.cameraUp[2]);
    constants.cameraRight = sl::float3(frame.cameraRight[0], frame.cameraRight[1],
        frame.cameraRight[2]);
    constants.cameraFwd = sl::float3(frame.cameraForward[0], frame.cameraForward[1],
        frame.cameraForward[2]);
    constants.cameraNear = frame.cameraNear;
    constants.cameraFar = frame.cameraFar;
    constants.cameraFOV = frame.verticalFovRadians;
    constants.cameraAspectRatio = frame.aspect;
    constants.depthInverted = sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    const bool extentChanged = present.renderWidth != lastRenderWidth_ ||
        present.renderHeight != lastRenderHeight_;
    lastRenderWidth_ = present.renderWidth;
    lastRenderHeight_ = present.renderHeight;
    constants.reset = (frame.historyEpoch != lastHistoryEpoch_ || frame.frameIndex <= 0 ||
        extentChanged)
        ? sl::Boolean::eTrue
        : sl::Boolean::eFalse;
    lastHistoryEpoch_ = frame.historyEpoch;
    constants.orthographicProjection = sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    const sl::Result applied = api_.setConstants(constants, *token_, sl::ViewportHandle(kViewportId));
    if (applied != sl::Result::eOk)
        Log("Streamline FG: constants rejected (result %d)", static_cast<int>(applied));
}

void StreamlineFgProvider::ApplyTags(const FgPresent& present)
{
    sl::Resource depth(sl::ResourceType::eTex2d, present.depth,
        present.depthState != nullptr ? static_cast<uint32_t>(*present.depthState) : 0);
    depth.width = present.renderWidth;
    depth.height = present.renderHeight;
    sl::Resource motion(sl::ResourceType::eTex2d, present.motion,
        present.motionState != nullptr ? static_cast<uint32_t>(*present.motionState) : 0);
    motion.width = present.motionWidth;
    motion.height = present.motionHeight;
    sl::ResourceTag tags[4];
    uint32_t count = 0;
    tags[count++] = sl::ResourceTag(&depth, sl::kBufferTypeDepth,
        sl::ResourceLifecycle::eValidUntilPresent);
    tags[count++] = sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors,
        sl::ResourceLifecycle::eValidUntilPresent);
    sl::Resource hudless(sl::ResourceType::eTex2d, present.hudless,
        present.hudlessState != nullptr ? static_cast<uint32_t>(*present.hudlessState) : 0);
    hudless.width = present.renderWidth;
    hudless.height = present.renderHeight;
    if (present.hudless != nullptr)
        tags[count++] = sl::ResourceTag(&hudless, sl::kBufferTypeHUDLessColor,
            sl::ResourceLifecycle::eValidUntilPresent);
    api_.setTagForFrame(*token_, sl::ViewportHandle(kViewportId), tags, count, nullptr);
}

bool StreamlineFgProvider::Record(FgPresent& present, std::string& reason)
{
    if (suspended_ || proxySwapchain_ == nullptr)
    {
        reason = "Streamline frame generation is suspended";
        return false;
    }
    // Streamline's frame index drives the constants/tags lookup for the present that follows.
    const uint32_t index = SharedFrameIndex()++;
    const sl::Result token = api_.getNewFrameToken(token_, &index);
    if (token != sl::Result::eOk || token_ == nullptr)
    {
        reason = "Streamline refused a frame token (result " +
            std::to_string(static_cast<int>(token)) + ")";
        return false;
    }
    if (!UploadRealFrame(present, reason)) return false;
    // Reflex pacing is mandatory for DLSS-G: it decides when the source frame may be submitted, and
    // without it the pacer drops interpolated frames.
    if (api_.reflexSleep != nullptr) api_.reflexSleep(*token_);
    // Options are applied before the frame's constants and tags: Streamline clears the frame's
    // recorded data when the options change, so anything tagged before them is lost.
    DXGI_FORMAT mvecFormat = DXGI_FORMAT_R16G16_FLOAT;
    DXGI_FORMAT depthFormat = DXGI_FORMAT_R32_TYPELESS;
    if (present.motion != nullptr) mvecFormat = present.motion->GetDesc().Format;
    if (present.depth != nullptr) depthFormat = present.depth->GetDesc().Format;
    if (!ApplyOptions(present.multiplier, present.motionWidth, present.motionHeight, mvecFormat,
            depthFormat, reason))
    {
        Log("Streamline FG: record refused: %s", reason.c_str());
        return false;
    }
    ApplyConstants(present);
    ApplyTags(present);
    present.executed = true;
    return true;
}

bool StreamlineFgProvider::Present(FgPresent& present, std::string& reason)
{
    if (proxySwapchain_ == nullptr)
    {
        reason = "Streamline has no presenter swapchain";
        return false;
    }
    const HRESULT presented =
        proxySwapchain_->Present(0, tearingSupported_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (FAILED(presented))
    {
        reason = "the Streamline present failed (HRESULT 0x" + std::to_string(presented) + ")";
        return false;
    }
    sl::DLSSGOptions options{};
    options.mode = sl::DLSSGMode::eOn;
    options.numBackBuffers = kSwapchainBuffers;
    sl::DLSSGState state{};
    const sl::Result result = api_.dlssgGetState(sl::ViewportHandle(kViewportId), state, &options);
    if (result != sl::Result::eOk)
    {
        lastResult_ = static_cast<int32_t>(result);
        present.generatedCount = 0;
        return true;
    }
    lastResult_ = static_cast<int32_t>(state.status);
    if (state.numFramesToGenerateMax > 0)
        maximumMultiplier_ = std::min<uint32_t>(state.numFramesToGenerateMax + 1, 6);
    const uint32_t generated =
        state.numFramesActuallyPresented > 1 ? state.numFramesActuallyPresented - 1 : 0;
    present.generatedCount = generated;
    generatedPresents_ += generated;
    // DLSS-G disables interpolation itself when a frame arrives without complete constants or tags,
    // and stays off until the options are re-asserted. Re-arming every couple of frames would clear
    // the very inputs that make the next frame work, so it only happens after a long idle run.
    if (generated > 0)
    {
        idlePresents_ = 0;
    }
    else if (featureEnabled_ && ++idlePresents_ >= 30)
    {
        idlePresents_ = 0;
        appliedMultiplier_ = 0;
    }
    if (state.status != sl::DLSSGStatus::eOk)
    {
        reason = "Streamline frame generation reported status " +
            std::to_string(static_cast<uint32_t>(state.status));
    }
    return true;
}

void StreamlineFgProvider::Drain(std::string& reason)
{
    if (queue_ == nullptr || copyFence_ == nullptr) return;
    const uint64_t wait = copyValue_;
    if (wait != 0 && copyFence_->GetCompletedValue() < wait)
    {
        copyFence_->SetEventOnCompletion(wait, copyEvent_);
        if (WaitForSingleObject(copyEvent_, 5000) != WAIT_OBJECT_0)
            reason = "Streamline frame copies did not drain";
    }
}

bool StreamlineFgProvider::Suspend(std::string& reason)
{
    // While the provider is suspended the transport still presents the child swap chain directly,
    // and a present that DLSS-G sees without tags disables interpolation. Turning the feature off is
    // the only way to keep those frames clean; the transport suspends between frames, not mid-frame.
    if (initialized_ && !suspended_ && api_.dlssgSetOptions != nullptr && proxySwapchain_ != nullptr)
    {
        sl::DLSSGOptions options{};
        options.mode = sl::DLSSGMode::eOff;
        options.numBackBuffers = kSwapchainBuffers;
        const sl::Result result = api_.dlssgSetOptions(sl::ViewportHandle(kViewportId), options);
        if (result != sl::Result::eOk)
        {
            reason = "Streamline refused to suspend frame generation (result " +
                std::to_string(static_cast<int>(result)) + ")";
        }
    }
    suspended_ = true;
    featureEnabled_ = false;
    appliedMultiplier_ = 0;
    return true;
}

bool StreamlineFgProvider::Resume(const FgInit& init, std::string& reason)
{
    suspended_ = false;
    appliedMultiplier_ = 0;
    // The transport hides the child window while generation is suspended, which detaches DLSS-G
    // from the swap chain; a resumed session must attach to a fresh one, which also restarts the
    // session's frame counting.
    ReleaseSwapchain();
    return EnsureSwapchain(init, reason);
}

void StreamlineFgProvider::ReleaseSwapchain()
{
    if (copyEvent_ != nullptr)
    {
        CloseHandle(copyEvent_);
        copyEvent_ = nullptr;
    }
    copyFence_.Reset();
    copyList_.Reset();
    copyAllocator_.Reset();
    childQueue_.Reset();
    proxySwapchain_.Reset();
    proxyFactory_.Reset();
    proxyDevice_.Reset();
}

bool StreamlineFgProvider::Shutdown(std::string& reason)
{
    Suspend(reason);
    Drain(reason);
    ReleaseSwapchain();
    adapter_.Reset();
    // The interposer, its plugins and the patched provider images stay resident by contract.
    initialized_ = true;
    return true;
}
}
