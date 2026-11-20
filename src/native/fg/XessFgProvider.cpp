#include "XessFgProvider.h"

#include "../Log.h"

#include <windows.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace bigscale
{
namespace
{
constexpr uint32_t kSwapchainBuffers = 3;

const char* ResultName(int32_t result)
{
    switch (result)
    {
        case 0: return "success";
        case 2: return "warning: old driver";
        case 3: return "warning: too few frames";
        case 4: return "warning: frame id mismatch";
        case 5: return "warning: missing present status";
        case 6: return "warning: resource sizes mismatch";
        case -1: return "unsupported device";
        case -2: return "unsupported driver";
        case -3: return "uninitialized";
        case -4: return "invalid argument";
        case -5: return "out of memory";
        case -6: return "device error";
        case -7: return "not implemented";
        case -8: return "invalid context";
        case -10: return "unsupported";
        case -15: return "latency reduction unsupported";
        case -16: return "latency reduction function missing";
        default: return "unknown";
    }
}
}

XessFgProvider::XessFgProvider() = default;

bool XessFgProvider::LoadApi(const std::wstring& runtimeDirectory, std::string& reason)
{
    if (module_ != nullptr) return true;
    std::wstring path = runtimeDirectory;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
    path += L"libxess_fg.dll";
    module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module_ == nullptr)
    {
        reason = "libxess_fg.dll is not staged in the plugin directory";
        Log("XeSS-FG: module load failed (error=%lu).", static_cast<unsigned long>(GetLastError()));
        return false;
    }
    api_.createContext = reinterpret_cast<decltype(api_.createContext)>(
        GetProcAddress(module_, "xefgSwapChainD3D12CreateContext"));
    api_.getProperties = reinterpret_cast<decltype(api_.getProperties)>(
        GetProcAddress(module_, "xefgSwapChainD3D12GetProperties"));
    api_.initFromDesc = reinterpret_cast<decltype(api_.initFromDesc)>(
        GetProcAddress(module_, "xefgSwapChainD3D12InitFromSwapChainDesc"));
    api_.getSwapchain = reinterpret_cast<decltype(api_.getSwapchain)>(
        GetProcAddress(module_, "xefgSwapChainD3D12GetSwapChainPtr"));
    api_.setLatencyReduction = reinterpret_cast<decltype(api_.setLatencyReduction)>(
        GetProcAddress(module_, "xefgSwapChainSetLatencyReduction"));
    api_.setEnabled = reinterpret_cast<decltype(api_.setEnabled)>(
        GetProcAddress(module_, "xefgSwapChainSetEnabled"));
    api_.setInterpolatedFrames = reinterpret_cast<decltype(api_.setInterpolatedFrames)>(
        GetProcAddress(module_, "xefgSwapChainSetNumInterpolatedFrames"));
    api_.tagFrameResource = reinterpret_cast<decltype(api_.tagFrameResource)>(
        GetProcAddress(module_, "xefgSwapChainD3D12TagFrameResource"));
    api_.tagFrameConstants = reinterpret_cast<decltype(api_.tagFrameConstants)>(
        GetProcAddress(module_, "xefgSwapChainTagFrameConstants"));
    api_.setPresentId = reinterpret_cast<decltype(api_.setPresentId)>(
        GetProcAddress(module_, "xefgSwapChainSetPresentId"));
    api_.getLastPresentStatus = reinterpret_cast<decltype(api_.getLastPresentStatus)>(
        GetProcAddress(module_, "xefgSwapChainGetLastPresentStatus"));
    api_.getProperties = reinterpret_cast<decltype(api_.getProperties)>(
        GetProcAddress(module_, "xefgSwapChainD3D12GetProperties"));
    api_.destroy = reinterpret_cast<decltype(api_.destroy)>(GetProcAddress(module_, "xefgSwapChainDestroy"));
    if (!api_.createContext || !api_.getProperties || !api_.initFromDesc || !api_.getSwapchain || !api_.setEnabled ||
        !api_.setInterpolatedFrames || !api_.tagFrameResource || !api_.tagFrameConstants ||
        !api_.setPresentId || !api_.getLastPresentStatus || !api_.destroy)
    {
        reason = "libxess_fg.dll is missing its swapchain entry points";
        return false;
    }
    return true;
}

bool XessFgProvider::Initialize(const FgInit& init, std::string& reason)
{
    if (teardownFailed_)
    {
        reason = teardownReason_.empty() ? "a previous frame-generation teardown failed" : teardownReason_;
        return false;
    }
    device_ = init.device;
    queue_ = init.queue;
    width_ = init.width;
    height_ = init.height;
    if (device_ == nullptr || queue_ == nullptr || init.child == nullptr)
    {
        reason = "frame generation needs a device, a queue and the presenter window";
        return false;
    }
    if (!LoadApi(init.runtimeDirectory, reason)) return false;
    const int32_t created = api_.createContext(device_, &swapchain_);
    if (created != 0 || swapchain_ == nullptr)
    {
        reason = "xefgSwapChainD3D12CreateContext failed: " + std::to_string(created);
        Log("XeSS-FG: context creation failed code=%d (%s)", created, ResultName(created));
        return false;
    }
    // The SDK owns the real interpolation capacity; the host must not guess it. Query it after
    // context creation and before swapchain initialization, and refuse to continue if the query
    // fails: an unverified capacity must never be reported as supported.
    xefg_swapchain_d3d12_init_params_t query{};
    query.maxInterpolatedFrames = XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES;
    xefg_swapchain_properties_t properties{};
    const int32_t queried = api_.getProperties(swapchain_, &query, width_, height_,
        DXGI_FORMAT_R8G8B8A8_UNORM, &properties);
    if (queried != 0)
    {
        reason = "xefgSwapChainD3D12GetProperties failed: " + std::to_string(queried) + " (" +
            ResultName(queried) + ")";
        Log("XeSS-FG: capability query failed code=%d (%s)", queried, ResultName(queried));
        return false;
    }
    maxInterpolated_ = properties.maxSupportedInterpolations;
    if (maxInterpolated_ == 0) maxInterpolated_ = 1;
    if (maxInterpolated_ > 5) maxInterpolated_ = 5;
    maximumMultiplier_ = maxInterpolated_ + 1;
    Log("XeSS-FG: capability maxSupportedInterpolations=%u total=%ux",
        properties.maxSupportedInterpolations, maximumMultiplier_);
    const uint32_t requested = init.multiplier < 2 ? 2u : init.multiplier;
    if (requested - 1 > maxInterpolated_)
    {
        reason = "Intel XeSS-FG supports at most " + std::to_string(maximumMultiplier_) +
            "x total (one real frame plus " + std::to_string(maxInterpolated_) +
            " generated frames); " + std::to_string(requested) + "x was requested";
        return false;
    }
    if (init.xellContext != nullptr && api_.setLatencyReduction != nullptr)
    {
        const int32_t connected = api_.setLatencyReduction(swapchain_, init.xellContext);
        if (connected != 0)
        {
            // Latency reduction is an enhancement; generation itself can still run.
            Log("XeSS-FG: latency reduction was refused (%d: %s).", connected, ResultName(connected));
        }
    }
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width_;
    description.Height = height_;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = kSwapchainBuffers;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory)
    {
        reason = "the XeSS-FG DXGI factory could not be created";
        return false;
    }
    factory_ = factory;
    xefg_swapchain_d3d12_init_params_t parameters{};
    // Initialize at the discovered capacity; the per-multiplier value is applied later with
    // SetNumInterpolatedFrames, which does not reinitialize the proxy.
    parameters.maxInterpolatedFrames = maxInterpolated_;
    parameters.uiMode = XEFG_SWAPCHAIN_UI_MODE_AUTO;
    parameters.initFlags = init.reversedDepth ? XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH
                                             : XEFG_SWAPCHAIN_INIT_FLAG_NONE;
    reversedDepth_ = init.reversedDepth;
    const int32_t initialised = api_.initFromDesc(swapchain_, init.child, &description, nullptr, queue_,
        factory_.Get(), &parameters);
    if (initialised != 0)
    {
        reason = "xefgSwapChainD3D12InitFromSwapChainDesc failed: " + std::to_string(initialised);
        Log("XeSS-FG: swapchain initialization failed code=%d (%s)", initialised, ResultName(initialised));
        return false;
    }
    if (FAILED(api_.getSwapchain(swapchain_, __uuidof(IDXGISwapChain4), reinterpret_cast<void**>(
            proxy_.ReleaseAndGetAddressOf()))) ||
        !proxy_)
    {
        reason = "the XeSS-FG proxy swapchain could not be retrieved";
        return false;
    }
    const int32_t frames = api_.setInterpolatedFrames(swapchain_, requested - 1);
    if (frames != 0)
    {
        // The requested count was validated against the discovered capacity; a refusal here is
        // authoritative and must not be reported as a working multiplier.
        reason = "xefgSwapChainSetNumInterpolatedFrames(" + std::to_string(requested - 1) +
            ") failed: " + std::to_string(frames) + " (" + ResultName(frames) + ")";
        Log("XeSS-FG: SetNumInterpolatedFrames failed code=%d (%s)", frames, ResultName(frames));
        return false;
    }
    interpolated_ = requested - 1;
    const int32_t enabled = api_.setEnabled(swapchain_, 1);
    if (enabled != 0)
    {
        reason = "xefgSwapChainSetEnabled failed: " + std::to_string(enabled);
        Log("XeSS-FG: enabling generation failed code=%d (%s)", enabled, ResultName(enabled));
        return false;
    }
    Log("XeSS-FG: proxy swapchain ready %ux%u.", width_, height_);
    return true;
}

bool XessFgProvider::CopyRealColor(ID3D12GraphicsCommandList* list, std::string& reason)
{
    const uint32_t index = proxy_->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> target;
    if (FAILED(proxy_->GetBuffer(index, IID_PPV_ARGS(&target))) || !target)
    {
        reason = "the XeSS-FG proxy backbuffer could not be acquired";
        return false;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = target.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, &barrier);
    list->CopyResource(target.Get(), realColor_);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &barrier);
    return true;
}

bool XessFgProvider::Record(FgPresent& present, std::string& reason)
{
    if (swapchain_ == nullptr || proxy_ == nullptr || present.list == nullptr)
    {
        reason = "Intel frame generation is not ready";
        return false;
    }
    const uint32_t multiplier = present.multiplier < 2 ? 2u : present.multiplier;
    if (multiplier - 1 > maxInterpolated_)
    {
        reason = "Intel XeSS-FG supports at most " + std::to_string(maximumMultiplier_) +
            "x total (one real frame plus " + std::to_string(maxInterpolated_) +
            " generated frames); " + std::to_string(multiplier) + "x was requested";
        return false;
    }
    if (multiplier - 1 != interpolated_)
    {
        // A multiplier change is a proxy parameter change on this worker, not a context rebuild.
        const int32_t applied = api_.setInterpolatedFrames(swapchain_, multiplier - 1);
        if (applied != 0)
        {
            reason = "xefgSwapChainSetNumInterpolatedFrames(" + std::to_string(multiplier - 1) +
                ") failed: " + std::to_string(applied) + " (" + ResultName(applied) + ")";
            Log("XeSS-FG: SetNumInterpolatedFrames failed code=%d (%s)", applied, ResultName(applied));
            return false;
        }
        interpolated_ = multiplier - 1;
    }
    realColor_ = present.color;
    presentId_ = present.sourceId;
    if (!CopyRealColor(present.list, reason)) return false;

    xefg_swapchain_d3d12_resource_data_t motion{};
    motion.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
    motion.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
    motion.resourceBase = {0, 0};
    motion.resourceSize = {present.renderWidth, present.renderHeight};
    motion.pResource = present.motion;
    motion.incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (api_.tagFrameResource(swapchain_, present.list, presentId_, &motion) != 0)
    {
        reason = "the XeSS-FG motion vectors could not be tagged";
        return false;
    }
    xefg_swapchain_d3d12_resource_data_t depth = motion;
    depth.type = XEFG_SWAPCHAIN_RES_DEPTH;
    depth.pResource = present.depth;
    if (api_.tagFrameResource(swapchain_, present.list, presentId_, &depth) != 0)
    {
        reason = "the XeSS-FG depth buffer could not be tagged";
        return false;
    }
    xefg_swapchain_frame_constant_data_t constants{};
    std::memcpy(constants.viewMatrix, present.frame.currentView, sizeof(constants.viewMatrix));
    std::memcpy(constants.projectionMatrix, present.frame.currentProjection, sizeof(constants.projectionMatrix));
    constants.jitterOffsetX = present.frame.jitterX;
    constants.jitterOffsetY = present.frame.jitterY;
    // The transport hands previous-to-current vectors in presented-backbuffer rows with y down, so
    // both axes are negated to obtain the SDK's pixel displacement.
    constants.motionVectorScaleX = -static_cast<float>(present.renderWidth);
    constants.motionVectorScaleY = -static_cast<float>(present.renderHeight);
    constants.resetHistory = (present.frame.flags & BIGSCALE_FG_FLAG_RESET) != 0 ? 1u : 0u;
    constants.frameRenderTime = present.frame.deltaMilliseconds;
    if (api_.tagFrameConstants(swapchain_, presentId_, &constants) != 0)
    {
        reason = "the XeSS-FG frame constants could not be tagged";
        return false;
    }
    if (api_.setPresentId(swapchain_, presentId_) != 0)
    {
        reason = "the XeSS-FG present id could not be set";
        return false;
    }
    return true;
}

bool XessFgProvider::Present(FgPresent& present, std::string& reason)
{
    if (proxy_ == nullptr || api_.getLastPresentStatus == nullptr)
    {
        reason = "Intel frame generation has no proxy swapchain";
        return false;
    }
    const HRESULT result = proxy_->Present(1, 0);
    if (FAILED(result))
    {
        lastResult_ = static_cast<int32_t>(result);
        reason = "the XeSS-FG proxy Present failed";
        return false;
    }
    xefg_swapchain_present_status_t status{};
    const int32_t queried = api_.getLastPresentStatus(swapchain_, &status);
    if (queried != 0)
    {
        lastResult_ = queried;
        Log("XeSS-FG: present status unavailable (%d: %s).", queried, ResultName(queried));
        return true;
    }
    lastResult_ = static_cast<int32_t>(status.frameGenResult);
    if (status.frameGenResult == 0 && status.isFrameGenEnabled != 0 && status.framesPresented > 1)
    {
        // The proxy reports how many images the SDK actually presented, including the real one. It
        // is bounded by the configured intermediate count so a proxy misreport cannot inflate the
        // generated tally.
        uint32_t generated = status.framesPresented - 1;
        if (generated > maxInterpolated_) generated = maxInterpolated_;
        generatedPresents_ += generated;
        present.generatedCount = generated;
    }
    else if (status.frameGenResult != 0)
    {
        Log("XeSS-FG: interpolation result %d (%s).", status.frameGenResult, ResultName(status.frameGenResult));
    }
    return true;
}

void XessFgProvider::Drain(std::string& reason)
{
    if (swapchain_ == nullptr || api_.setEnabled == nullptr) return;
    const int32_t disabled = api_.setEnabled(swapchain_, 0);
    if (disabled != 0)
    {
        reason = "disabling XeSS frame generation failed: " + std::to_string(disabled);
        return;
    }
    if (queue_ == nullptr || device_ == nullptr) return;
    if (drainFence_ == nullptr &&
        FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drainFence_))))
    {
        reason = "the XeSS frame-generation drain fence could not be created";
        return;
    }
    if (drainEvent_ == nullptr)
    {
        drainEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (drainEvent_ == nullptr)
        {
            reason = "the XeSS frame-generation drain event could not be created";
            return;
        }
    }
    const uint64_t value = ++drainValue_;
    if (FAILED(queue_->Signal(drainFence_.Get(), value)))
    {
        reason = "the XeSS frame-generation drain fence could not be signalled";
        return;
    }
    if (FAILED(drainFence_->SetEventOnCompletion(value, drainEvent_)))
    {
        reason = "the XeSS frame-generation drain fence could not be waited on";
        return;
    }
    if (WaitForSingleObject(drainEvent_, 5000) != WAIT_OBJECT_0)
        reason = "the XeSS frame-generation queue did not drain";
}

bool XessFgProvider::Suspend(std::string& reason)
{
    // XeSS is not process-resident: the transport destroys it through the checked drain/shutdown
    // path. This entry point exists only so every caller can use one contract.
    Drain(reason);
    return reason.empty();
}

bool XessFgProvider::Resume(const FgInit& init, std::string& reason)
{
    (void)init;
    reason = "Intel XeSS-FG is not a process-resident provider";
    return false;
}

bool XessFgProvider::Shutdown(std::string& reason)
{
    if (teardownFailed_)
    {
        reason = teardownReason_.empty() ? "the frame-generation provider failed to tear down"
                                         : teardownReason_;
        return false;
    }
    if (swapchain_ == nullptr && module_ == nullptr) return true;
    // The host's proxy references must be gone before the SDK context is destroyed.
    proxy_.Reset();
    if (swapchain_ != nullptr)
    {
        if (api_.destroy == nullptr)
        {
            reason = "the XeSS swapchain destroy entry point is unavailable";
            teardownFailed_ = true;
            teardownReason_ = reason;
            return false;
        }
        const int32_t destroyed = api_.destroy(swapchain_);
        if (destroyed != 0)
        {
            // POINTER_STILL_IN_USE and device errors mean the SDK still owns live state.
            reason = "xefgSwapChainDestroy failed: " + std::to_string(destroyed) + " (" +
                ResultName(destroyed) + ")";
            teardownFailed_ = true;
            teardownReason_ = reason;
            return false;
        }
    }
    swapchain_ = nullptr;
    maxInterpolated_ = 1;
    interpolated_ = 1;
    maximumMultiplier_ = 0;
    factory_.Reset();
    if (module_ != nullptr)
    {
        FreeLibrary(module_);
        module_ = nullptr;
    }
    api_ = Api{};
    device_ = nullptr;
    queue_ = nullptr;
    realColor_ = nullptr;
    presentId_ = 0;
    if (drainEvent_ != nullptr)
    {
        CloseHandle(drainEvent_);
        drainEvent_ = nullptr;
    }
    drainFence_.Reset();
    drainValue_ = 0;
    return true;
}
}
