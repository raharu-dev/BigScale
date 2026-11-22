#include "NgxFgProvider.h"

#include "../Dx12Interop.h"
#include "../Log.h"
#include "../NgxSynchronization.h"
#include "../Shaders.h"
#include "FgMath.h"
#include "FgScheduling.h"
#include "FgTestObserver.h"
#include "shaders_generated.h"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <nvsdk_ngx_params.h>
#include <nvsdk_ngx_params_dlssg.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace bigscale
{
namespace
{
constexpr char kProjectId[] = "dce2ec10-c62e-4ec2-a24e-7368ff734a44";
constexpr uint32_t kSwapchainBuffers = 3;
constexpr D3D12_RESOURCE_STATES kReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
// ANR is never redistributed, so the missing-runtime message points at the mod page instead of a
// path the user may never have had.
constexpr char kAnrMissing[] =
    "ANR FG runtime is missing; check the mod page for how to install ANR";

void TransitionTo(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES& tracked,
                  D3D12_RESOURCE_STATES after)
{
    if (resource == nullptr || tracked == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = tracked;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
    tracked = after;
}

void TransitionSwapchain(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                         D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

bool WaitEvent(HANDLE event, uint32_t milliseconds)
{
    return event != nullptr && WaitForSingleObject(event, milliseconds) == WAIT_OBJECT_0;
}
}

NgxFgProvider::NgxFgProvider(Flavor flavor) : flavor_(flavor) {}

const char* NgxFgProvider::Name() const { return flavor_ == Flavor::Nvidia ? "NVIDIA DLSS-FG" : "ANR"; }

std::wstring AnrModulePath(const std::wstring& providerDirectory)
{
    if (providerDirectory.empty()) return {};
    std::wstring path = providerDirectory;
    if (path.back() != L'\\' && path.back() != L'/') path += L'\\';
    path += L"nvngx_dlssg.dll";
    return path;
}

bool NgxFgProvider::LoadAnr(std::string& reason)
{
    if (module_ != nullptr) return true;
    const std::wstring path = AnrModulePath(anrDirectory_);
    if (path.empty())
    {
        reason = kAnrMissing;
        return false;
    }
    module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module_ == nullptr)
    {
        reason = kAnrMissing;
        Log("ANR: provider module load failed (error=%lu).", static_cast<unsigned long>(GetLastError()));
        return false;
    }
    anr_.init = reinterpret_cast<decltype(anr_.init)>(GetProcAddress(module_, "NVSDK_NGX_D3D12_Init_Ext"));
    anr_.populate = reinterpret_cast<decltype(anr_.populate)>(
        GetProcAddress(module_, "NVSDK_NGX_D3D12_PopulateParameters_Impl"));
    anr_.create = reinterpret_cast<decltype(anr_.create)>(GetProcAddress(module_, "NVSDK_NGX_D3D12_CreateFeature"));
    anr_.evaluate = reinterpret_cast<decltype(anr_.evaluate)>(
        GetProcAddress(module_, "NVSDK_NGX_D3D12_EvaluateFeature"));
    anr_.release = reinterpret_cast<decltype(anr_.release)>(GetProcAddress(module_, "NVSDK_NGX_D3D12_ReleaseFeature"));
    anr_.shutdown1 = reinterpret_cast<decltype(anr_.shutdown1)>(
        GetProcAddress(module_, "NVSDK_NGX_D3D12_Shutdown1"));
    if (!anr_.init || !anr_.create || !anr_.evaluate || !anr_.release || !anr_.shutdown1)
    {
        reason = "the ANR runtime is missing its frame-generation entry points";
        return false;
    }
    return true;
}

bool NgxFgProvider::Preflight(const FgInit& init, std::string& reason)
{
    if (flavor_ != Flavor::Anr) return true;
    // Match InitializeAnr's first check: HAGS is a prerequisite for every NGX frame-generation
    // provider, and a HAGS-off device would be invalidated instead of returning a clean error.
    if (!HardwareSchedulingEnabled(init.device, reason)) return false;
    if (init.runtimeDirectory.empty())
    {
        reason = kAnrMissing;
        return false;
    }
    std::wstring directory = init.runtimeDirectory;
    if (directory.back() != L'\\' && directory.back() != L'/') directory += L'\\';
    directory += L"providers\\anr";
    const std::wstring module = AnrModulePath(directory);
    const DWORD attributes = GetFileAttributesW(module.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        reason = kAnrMissing;
        return false;
    }
    return true;
}

bool NgxFgProvider::Initialize(const FgInit& init, std::string& reason)
{
    if (teardownFailed_)
    {
        reason = teardownReason_.empty() ? "a previous frame-generation teardown failed" : teardownReason_;
        return false;
    }
    device_ = init.device;
    queue_ = init.queue;
    child_ = init.child;
    if (device_ == nullptr || queue_ == nullptr || child_ == nullptr)
    {
        reason = "frame generation needs a device, a queue and the presenter window";
        return false;
    }
    anrDirectory_ = init.runtimeDirectory;
    if (!anrDirectory_.empty() && anrDirectory_.back() != L'\\' && anrDirectory_.back() != L'/')
        anrDirectory_ += L'\\';
    anrDirectory_ += L"providers\\anr";
    if (!EnsureSwapchain(init, reason)) return false;
    if (!(flavor_ == Flavor::Nvidia ? InitializeNvidia(init, reason) : InitializeAnr(init, reason)))
    {
        DestroySwapchain();
        return false;
    }
    initialized_ = true;
    Log("%s: frame generation initialised %ux%u -> %ux%u.", Name(), init.width, init.height, init.width, init.height);
    return true;
}

bool NgxFgProvider::InitializeNvidia(const FgInit& init, std::string& reason)
{
    if (!HardwareSchedulingEnabled(device_, reason))
    {
        Log("%s: %s", Name(), reason.c_str());
        return false;
    }
    std::wstring runtime = init.runtimeDirectory;
    if (runtime.empty())
    {
        reason = "the NVIDIA FG runtime directory is empty";
        return false;
    }
    const wchar_t* paths[1] = {runtime.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    const std::string engine = init.engineVersion.empty() ? "6000.3.17f1" : init.engineVersion;
    const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_UNITY,
        engine.c_str(), init.dataDirectory.c_str(), device_, &info, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(result))
    {
        reason = "NVSDK_NGX_D3D12_Init_with_ProjectID failed: 0x" + std::to_string(static_cast<unsigned>(result));
        Log("%s: D3D12 NGX initialisation failed result=0x%08x", Name(), static_cast<unsigned>(result));
        return false;
    }
    sdkInitialized_ = true;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&params_)) || params_ == nullptr)
    {
        reason = "NGX did not provide its D3D12 parameter map";
        return false;
    }
    int available = 0;
    if (!NVSDK_NGX_FAILED(params_->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &available)) &&
        available == 0)
    {
        reason = "NVIDIA frame generation is not available on this GPU and driver";
        return false;
    }
    // The SDK reports how many generated frames one source may interpolate; a missing value means
    // exactly one, never an assumed multiplier. This implementation owns three output slots.
    int multiFrameCountMax = 0;
    uint32_t generated = 1;
    if (!NVSDK_NGX_FAILED(params_->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &multiFrameCountMax)) &&
        multiFrameCountMax > 0)
        generated = static_cast<uint32_t>(multiFrameCountMax);
    if (generated > 3) generated = 3;
    maximumMultiplier_ = generated + 1;
    Log("%s: capability MultiFrameCountMax=%d total=%ux", Name(), multiFrameCountMax, maximumMultiplier_);
    return true;
}

bool NgxFgProvider::InitializeAnr(const FgInit& init, std::string& reason)
{
    if (!HardwareSchedulingEnabled(device_, reason))
    {
        Log("%s: %s", Name(), reason.c_str());
        return false;
    }
    if (!LoadAnr(reason)) return false;
    const std::wstring providerDirectory = anrDirectory_;
    const wchar_t* paths[1] = {providerDirectory.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    // Application id 1 keeps the provider independent of NVIDIA's project registry and driver gate.
    const NVSDK_NGX_Result result = anr_.init(1ull, init.dataDirectory.c_str(), device_, NVSDK_NGX_Version_API, &info);
    if (NVSDK_NGX_FAILED(result))
    {
        reason = "ANR NVSDK_NGX_D3D12_Init_Ext failed: 0x" + std::to_string(static_cast<unsigned>(result));
        Log("%s: provider initialisation failed result=0x%08x", Name(), static_cast<unsigned>(result));
        return false;
    }
    sdkInitialized_ = true;
    map_.Reset();
    params_ = &map_;
    if (anr_.populate != nullptr) anr_.populate(params_);
    // ANR publishes the same capability key through its populated map, but its current release
    // supports one generated frame per source: 2x total. A missing value keeps that same safe
    // one-generated-frame contract instead of guessing.
    int multiFrameCountMax = 0;
    uint32_t generated = 1;
    if (!NVSDK_NGX_FAILED(params_->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &multiFrameCountMax)) &&
        multiFrameCountMax > 0)
        generated = static_cast<uint32_t>(multiFrameCountMax);
    if (generated > 1) generated = 1;
    maximumMultiplier_ = generated + 1;
    Log("%s: capability MultiFrameCountMax=%d total=%ux", Name(), multiFrameCountMax, maximumMultiplier_);
    (void)init;
    return true;
}

bool NgxFgProvider::SetStaticParameters(const FgInit& init, uint32_t renderWidth, uint32_t renderHeight,
    std::string& reason)
{
    width_ = init.width;
    height_ = init.height;
    renderWidth_ = renderWidth;
    renderHeight_ = renderHeight;
    params_->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    params_->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    params_->Set(NVSDK_NGX_Parameter_Width, width_);
    params_->Set(NVSDK_NGX_Parameter_Height, height_);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_Width, width_);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_Height, height_);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFormat,
        static_cast<unsigned>(DXGI_FORMAT_R8G8B8A8_UNORM));
    params_->Set(NVSDK_NGX_DLSSG_Parameter_InternalWidth, renderWidth_);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_InternalHeight, renderHeight_);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0u);
    return true;
}

bool NgxFgProvider::WaitForSubmissions(uint32_t timeoutMilliseconds, std::string& reason)
{
    if (submitFence_ == nullptr || submitCounter_ == 0) return true;
    if (createEvent_ == nullptr)
    {
        createEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (createEvent_ == nullptr)
        {
            reason = "the frame-generation submission event could not be created";
            return false;
        }
    }
    if (submitFence_->GetCompletedValue() >= submitCounter_) return true;
    if (FAILED(submitFence_->SetEventOnCompletion(submitCounter_, createEvent_)))
    {
        reason = "the frame-generation submission fence could not be waited on";
        return false;
    }
    if (!WaitEvent(createEvent_, timeoutMilliseconds))
    {
        reason = "the frame-generation submission did not complete";
        return false;
    }
    return true;
}

bool NgxFgProvider::EnsureFeature(FgPresent& present, std::string& reason)
{
    const uint32_t renderWidth = present.renderWidth != 0 ? present.renderWidth : present.outputWidth;
    const uint32_t renderHeight = present.renderHeight != 0 ? present.renderHeight : present.outputHeight;
    if (handle_ != nullptr && renderWidth_ == renderWidth && renderHeight_ == renderHeight &&
        width_ == present.outputWidth && height_ == present.outputHeight)
        return true;
    // Releasing or resizing a live feature must not race work the GPU may still execute.
    if (!WaitForSubmissions(2000, reason)) return false;
    // Only the feature handle is released here: the D3D12 NGX instance stays initialized.
    if (!ReleaseHandle(reason)) return false;
    FgInit init{};
    init.width = present.outputWidth;
    init.height = present.outputHeight;
    width_ = present.outputWidth;
    height_ = present.outputHeight;
    if (!SetStaticParameters(init, renderWidth, renderHeight, reason)) return false;
    return CreateFeature(reason);
}

bool NgxFgProvider::CreateFeature(std::string& reason)
{
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&createAllocator_))) ||
        FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, createAllocator_.Get(), nullptr,
            IID_PPV_ARGS(&createList_))) ||
        FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&createFence_))))
    {
        reason = "the NGX feature-creation command list could not be created";
        return false;
    }
    if (createEvent_ != nullptr) CloseHandle(createEvent_);
    createEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (createEvent_ == nullptr)
    {
        reason = "the NGX feature-creation event could not be created";
        return false;
    }
    const auto createStarted = std::chrono::steady_clock::now();
    Log("%s: feature create begin output=%ux%u render=%ux%u", Name(), width_, height_, renderWidth_,
        renderHeight_);
    NVSDK_NGX_Result created;
    {
        std::lock_guard<std::recursive_mutex> guard(NgxMutex());
        created = flavor_ == Flavor::Nvidia
            ? NVSDK_NGX_D3D12_CreateFeature(createList_.Get(), NVSDK_NGX_Feature_FrameGeneration, params_, &handle_)
            : anr_.create(createList_.Get(), NVSDK_NGX_Feature_FrameGeneration, params_, &handle_);
    }
    // Executing and waiting for the creation work happens outside the shared NGX lock.
    createList_->Close();
    ID3D12CommandList* lists[1] = {createList_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    const uint64_t creationValue = ++createValue_;
    if (FAILED(queue_->Signal(createFence_.Get(), creationValue)))
    {
        reason = "the frame-generation creation fence could not be signalled";
        return false;
    }
    if (FAILED(createFence_->SetEventOnCompletion(creationValue, createEvent_)))
    {
        reason = "the frame-generation creation fence could not be waited on";
        return false;
    }
    if (!WaitEvent(createEvent_, 5000))
    {
        reason = "frame-generation feature creation did not complete";
        return false;
    }
    const auto createElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - createStarted).count();
    if (NVSDK_NGX_FAILED(created) || handle_ == nullptr)
    {
        reason = std::string("frame-generation feature creation failed: 0x") +
            std::to_string(static_cast<unsigned>(created));
        Log("%s: CreateFeature failed result=0x%08x after %lld ms", Name(), static_cast<unsigned>(created),
            static_cast<long long>(createElapsed));
        handle_ = nullptr;
        return false;
    }
    Log("%s: feature ready in %lld ms", Name(), static_cast<long long>(createElapsed));
    return true;
}

bool NgxFgProvider::BeginListRing(ListRing& ring, std::string& reason, ID3D12GraphicsCommandList** list)
{
    if (device_ == nullptr || queue_ == nullptr || submitFence_ == nullptr)
    {
        reason = std::string(Name()) + " has no submission ring";
        return false;
    }
    if (createEvent_ == nullptr)
    {
        createEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (createEvent_ == nullptr)
        {
            reason = "the frame-generation submission event could not be created";
            return false;
        }
    }
    bool created = false;
    if (ring.allocator == nullptr)
    {
        const HRESULT allocatorResult = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&ring.allocator));
        const HRESULT listResult = SUCCEEDED(allocatorResult)
            ? device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ring.allocator.Get(), nullptr,
                IID_PPV_ARGS(&ring.list))
            : allocatorResult;
        if (FAILED(allocatorResult) || FAILED(listResult))
        {
            reason = "the frame-generation command list could not be created";
            return false;
        }
        created = true;
    }
    if (!created)
    {
        // Wait for this pair's previous submission before touching it, then close any list the SDK
        // left open and reset the allocator behind it. A fence that cannot be armed or a removed
        // device must fail here: reusing the allocator or the plane descriptors while the GPU may
        // still read them is not a recoverable submission.
        const uint64_t completed = submitFence_->GetCompletedValue();
        if (completed == UINT64_MAX)
        {
            reason = "the frame-generation submission fence reported device removal";
            return false;
        }
        if (ring.value != 0 && completed < ring.value)
        {
            const HRESULT armed = submitFence_->SetEventOnCompletion(ring.value, createEvent_);
            if (FAILED(armed))
            {
                reason = "the frame-generation submission fence could not be awaited";
                return false;
            }
            if (!WaitEvent(createEvent_, 2000))
            {
                reason = submitFence_->GetCompletedValue() == UINT64_MAX
                    ? "the frame-generation device was removed while waiting for a submission"
                    : "the frame-generation submission did not complete";
                return false;
            }
        }
        ring.list->Close();
        const HRESULT allocatorReset = ring.allocator->Reset();
        const HRESULT listReset = SUCCEEDED(allocatorReset) ? ring.list->Reset(ring.allocator.Get(), nullptr)
                                                           : allocatorReset;
        if (FAILED(allocatorReset) || FAILED(listReset))
        {
            reason = "the frame-generation command list could not be reset";
            return false;
        }
    }
    *list = ring.list.Get();
    return true;
}

bool NgxFgProvider::SubmitList(ID3D12GraphicsCommandList* list, ListRing& ring, uint64_t sourceFenceValue,
    std::string& reason)
{
    if (list == nullptr)
    {
        reason = "the frame-generation command list is unavailable";
        return false;
    }
    if (FAILED(list->Close()))
    {
        // The SDK may have closed the list after recording; executing a closed list is legal.
        Log("%s: the SDK had already closed the command list it recorded into.", Name());
    }
    if (sourceFenceValue != 0 && sourceFence_ != nullptr &&
        FAILED(queue_->Wait(sourceFence_, sourceFenceValue)))
    {
        reason = "the frame-generation queue could not wait on the capture fence";
        return false;
    }
    ID3D12CommandList* lists[1] = {list};
    queue_->ExecuteCommandLists(1, lists);
    const uint64_t value = ++submitCounter_;
    if (FAILED(queue_->Signal(submitFence_.Get(), value)))
    {
        reason = "the frame-generation submission fence could not be signalled";
        return false;
    }
    ring.value = value;
    return true;
}

bool NgxFgProvider::EnsureSwapchain(const FgInit& init, std::string& reason)
{
    if (swapchain_ != nullptr)
    {
        if (swapchainWidth_ == init.width && swapchainHeight_ == init.height) return true;
        // The child swapchain may only change size once its own queued presents completed.
        if (!WaitForSubmissions(5000, reason)) return false;
        if (FAILED(swapchain_->ResizeBuffers(kSwapchainBuffers, init.width, init.height,
                DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)))
        {
            reason = "the frame-generation presenter swapchain could not be resized";
            return false;
        }
        for (D3D12_RESOURCE_STATES& state : childStates_) state = D3D12_RESOURCE_STATE_COMMON;
        swapchainWidth_ = init.width;
        swapchainHeight_ = init.height;
        cadence_.Reset();
        return true;
    }
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory)
    {
        reason = "the presenter DXGI factory could not be created";
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = init.width;
    description.Height = init.height;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = kSwapchainBuffers;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    // The waitable is what spaces the generated and real presents; see FgCadence.
    description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> swapchain;
    if (FAILED(factory->CreateSwapChainForHwnd(queue_, init.child, &description, nullptr, nullptr, &swapchain)) ||
        !swapchain || FAILED(swapchain.As(&swapchain_)) || !swapchain_)
    {
        reason = "the frame-generation presenter swapchain could not be created";
        return false;
    }
    UINT supported = 0;
    if (FAILED(swapchain_->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &supported)) ||
        supported == 0 || FAILED(swapchain_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)))
    {
        reason = "frame generation requires an SDR presenter";
        return false;
    }
    if (!cadence_.Attach(swapchain.Get(), reason)) return false;
    swapchainWidth_ = init.width;
    swapchainHeight_ = init.height;
    return true;
}

bool NgxFgProvider::EnsureOutputTextures(uint32_t width, uint32_t height, uint32_t requiredSlots,
    std::string& reason)
{
    if (requiredSlots == 0) requiredSlots = 1;
    if (requiredSlots > kMaxGeneratedFrames)
    {
        reason = "the requested frame-generation multiplier exceeds this provider's fixed output capacity";
        return false;
    }
    bool matches = requiredSlots_ >= requiredSlots;
    if (matches)
    {
        for (uint32_t i = 0; i < requiredSlots; i++)
        {
            if (slots_[i].image == nullptr || slots_[i].disable == nullptr || slots_[i].readback == nullptr)
            {
                matches = false;
                break;
            }
        }
    }
    if (matches && width_ == width && height_ == height) return true;
    // Resetting these resources while a previous submission still reads them would be a use-after-
    // free on the GPU. Prove completion first; a failed wait keeps the old set intact.
    if (!WaitForSubmissions(2000, reason)) return false;
    width_ = width;
    height_ = height;
    for (GeneratedSlot& slot : slots_)
    {
        slot.image.Reset();
        slot.disable.Reset();
        slot.readback.Reset();
        slot.imageState = D3D12_RESOURCE_STATE_COMMON;
        slot.disableState = D3D12_RESOURCE_STATE_COMMON;
        slot.disabled = false;
    }
    requiredSlots_ = 0;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    for (uint32_t i = 0; i < requiredSlots; i++)
    {
        GeneratedSlot& slot = slots_[i];
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slot.image))) ||
            !slot.image)
        {
            reason = "the generated-frame output texture could not be created";
            return false;
        }
        description.Width = 4;
        description.Height = 1;
        description.Format = DXGI_FORMAT_UNKNOWN;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slot.disable))) ||
            !slot.disable)
        {
            reason = "the interpolation-disable output buffer could not be created";
            return false;
        }
        D3D12_HEAP_PROPERTIES upload{};
        upload.Type = D3D12_HEAP_TYPE_READBACK;
        // A READBACK heap resource cannot carry the unordered-access flag, and it must stay a plain
        // buffer: the flag was only valid for the disable output above.
        description.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(device_->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.readback))) ||
            !slot.readback)
        {
            reason = "the interpolation-disable readback buffer could not be created";
            return false;
        }
        // Restore the texture description for the next slot's image.
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = width;
        description.Height = height;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }
    requiredSlots_ = requiredSlots;
    return true;
}

bool NgxFgProvider::FillEvaluationParameters(FgPresent& present, uint32_t multiFrameCount,
    uint32_t multiFrameIndex, std::string& reason)
{
    const BIGSCALE_FG_Frame& frame = present.frame;
    const uint32_t renderWidth = present.renderWidth;
    const uint32_t renderHeight = present.renderHeight;
    if (renderWidth == 0 || renderHeight == 0 || present.outputWidth == 0 || present.outputHeight == 0)
    {
        reason = "frame generation received an empty render or output extent";
        return false;
    }
    params_->Set(NVSDK_NGX_Parameter_Width, present.outputWidth);
    params_->Set(NVSDK_NGX_Parameter_Height, present.outputHeight);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_Width, present.outputWidth);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_Height, present.outputHeight);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, static_cast<unsigned>(DXGI_FORMAT_R8G8B8A8_UNORM));
    params_->Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, present.color);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_Depth, present.depth);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_MVecs, present.motion);
    // Real HUD-less scene colour immediately before the overlay UI. NGX extracts the HUD itself;
    // UI/UI-alpha remain null because no genuine alpha surface exists.
    params_->Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, present.hudless);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_UI, static_cast<ID3D12Resource*>(nullptr));
    params_->Set(NVSDK_NGX_DLSSG_Parameter_UIAlpha, static_cast<ID3D12Resource*>(nullptr));
    params_->Set(NVSDK_NGX_DLSSG_Parameter_BidirectionalDistortionField, static_cast<ID3D12Resource*>(nullptr));
    // Each evaluation owns its own interpolated output and interpolation-disable output, so no
    // intermediate overwrites an image an earlier present may still be scanning out.
    GeneratedSlot& output = slots_[multiFrameIndex - 1];
    params_->Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, output.image.Get());
    params_->Set(NVSDK_NGX_DLSSG_Parameter_OutputReal, static_cast<ID3D12Resource*>(nullptr));
    params_->Set(NVSDK_NGX_DLSSG_Parameter_OutputDisableInterpolation, output.disable.Get());

    const FgMatrix view = FgMatrix::FromArray(frame.currentView);
    const FgMatrix projection = FgMatrix::FromArray(frame.currentProjection);
    const FgMatrix previousView = FgMatrix::FromArray(frame.previousView);
    const FgMatrix previousProjection = FgMatrix::FromArray(frame.previousProjection);
    if (!view.Finite() || !projection.Finite() || !previousView.Finite() || !previousProjection.Finite())
    {
        reason = "the camera matrices are not finite; frame generation rejected this frame";
        return false;
    }
    const FgMatrix currentVp = Mul(view, projection);
    const FgMatrix previousVp = Mul(previousView, previousProjection);
    FgMatrix inverseCurrent{};
    FgMatrix inversePrevious{};
    FgMatrix clipToPrev{};
    FgMatrix prevToClip{};
    FgMatrix inverseProjection{};
    if (!Inverse(currentVp, inverseCurrent) || !Inverse(previousVp, inversePrevious) ||
        !Inverse(projection, inverseProjection))
    {
        reason = "the camera matrices are singular; frame generation rejected this frame";
        return false;
    }
    clipToPrev = Mul(inverseCurrent, previousVp);
    prevToClip = Mul(inversePrevious, currentVp);
    if (!clipToPrev.Finite() || !prevToClip.Finite())
    {
        reason = "the reprojection matrices are not finite; frame generation rejected this frame";
        return false;
    }

    // The parameter map stores the pointer, so the matrices live in this provider until after the
    // evaluation that reads them.
    viewToClip_ = projection;
    clipToView_ = inverseProjection;
    clipToPrevClip_ = clipToPrev;
    prevClipToClip_ = prevToClip;
    clipToLensClip_ = FgMatrix::Identity();
    params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraViewToClip, &viewToClip_.m[0][0]);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_ClipToCameraView, &clipToView_.m[0][0]);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_ClipToLensClip, &clipToLensClip_.m[0][0]);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip, &clipToPrevClip_.m[0][0]);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_PrevClipToClip, &prevClipToClip_.m[0][0]);

    // Clip-space jitter (NDC units) from the render-pixel jitter the upscale path also uses. The
    // jitter is expressed in render pixels, never output pixels.
    const float jitterX = 2.0f * frame.jitterX / static_cast<float>(renderWidth);
    const float jitterY = 2.0f * frame.jitterY / static_cast<float>(renderHeight);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_JitterOffsetX, jitterX);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_JitterOffsetY, jitterY);
    // Direct DLSS-FG MvecScale parameters are in pixels. The transport hands previous-to-current
    // vectors in presented-backbuffer rows with y pointing down, and both axes are negated to
    // obtain the SDK's current-to-previous pixel displacement. The offline vertical-motion check
    // asserts the resulting interpolation direction, so this sign is proven, not assumed.
    params_->Set(NVSDK_NGX_DLSSG_Parameter_MvecScaleX, -static_cast<float>(renderWidth));
    params_->Set(NVSDK_NGX_DLSSG_Parameter_MvecScaleY, -static_cast<float>(renderHeight));

    params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetX, 0.0f);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetY, 0.0f);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraPosX, frame.cameraPosition[0]);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraPosY, frame.cameraPosition[1]);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraPosZ, frame.cameraPosition[2]);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraUpX, frame.cameraUp[0]);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraUpY, frame.cameraUp[1]);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraUpZ, frame.cameraUp[2]);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraRightX, frame.cameraRight[0]);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraRightY, frame.cameraRight[1]);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraRightZ, frame.cameraRight[2]);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraFwdX, frame.cameraForward[0]);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraFwdY, frame.cameraForward[1]);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraFwdZ, frame.cameraForward[2]);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraNear, frame.cameraNear);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraFar, frame.cameraFar);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraFOV, frame.verticalFovRadians);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio, frame.aspect);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_ColorBuffersHDR, 0u);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_DepthInverted,
        (frame.flags & BIGSCALE_FG_FLAG_REVERSED_DEPTH) != 0 ? 1u : 0u);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraMotionIncluded, 1u);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_MvecDilated, 0u);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_Reset, (frame.flags & BIGSCALE_FG_FLAG_RESET) != 0 ? 1u : 0u);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, multiFrameCount);
    params_->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, multiFrameIndex);
    return true;
}

bool NgxFgProvider::Record(FgPresent& present, std::string& reason)
{
    if (!initialized_ || present.queue == nullptr)
    {
        reason = std::string(Name()) + " is not ready to generate";
        return false;
    }
    const uint32_t multiplier = present.multiplier < 2 ? 2u : present.multiplier;
    if (multiplier > maximumMultiplier_)
    {
        reason = std::string(Name()) + " supports at most " + std::to_string(maximumMultiplier_) +
            "x total (one real frame plus " + std::to_string(maximumMultiplier_ - 1) +
            " generated frames); " + std::to_string(multiplier) + "x was requested";
        return false;
    }
    const uint32_t slots = multiplier - 1;
    pairMultiplier_ = multiplier;
    pairSlots_ = 0;
    frameDisabled_ = false;
    disableByte_ = -1;
    if (!EnsureOutputTextures(present.outputWidth, present.outputHeight, slots, reason)) return false;
    if (!EnsureFeature(present, reason)) return false;
    if (submitFence_ == nullptr &&
        FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&submitFence_))))
    {
        reason = "the frame-generation submission fence could not be created";
        return false;
    }
    sourceFence_ = present.sourceFence;
    // A history reset starts a new pair from scratch: the previous sequence must never delay this
    // frame's real present.
    if ((present.frame.flags & BIGSCALE_FG_FLAG_RESET) != 0) cadence_.Reset();

    // The SDK records into its own list; the application then owns the generated-frame copies.
    ID3D12GraphicsCommandList* list = nullptr;
    if (!BeginListRing(sdkList_, reason, &list)) return false;
    Dx12Interop::Transition(list, present.color, *present.colorState,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Dx12Interop::Transition(list, present.depth, *present.depthState,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Dx12Interop::Transition(list, present.motion, *present.motionState,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (present.hudless != nullptr)
        Dx12Interop::Transition(list, present.hudless, *present.hudlessState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (uint32_t i = 0; i < slots; i++)
    {
        Dx12Interop::Transition(list, slots_[i].image.Get(), slots_[i].imageState,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Dx12Interop::Transition(list, slots_[i].disable.Get(), slots_[i].disableState,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // One source frame needs multiplier-1 interpolations. Every evaluation shares the source
    // identity, camera matrices, jitter and simulation interval; only the intermediate index and
    // its dedicated output change.
    for (uint32_t index = 1; index <= slots; index++)
    {
        NVSDK_NGX_Result evaluated;
        {
            // Stateful NGX calls are serialised against the D3D11 super-resolution backend. The lock
            // is scoped to parameter preparation and the SDK call; no GPU wait or submission is
            // inside it.
            std::lock_guard<std::recursive_mutex> guard(NgxMutex());
            if (!FillEvaluationParameters(present, slots, index, reason))
                return false;
            evaluated = flavor_ == Flavor::Nvidia
                ? NVSDK_NGX_D3D12_EvaluateFeature(list, handle_, params_, nullptr)
                : anr_.evaluate(list, handle_, params_, nullptr);
        }
        if (NVSDK_NGX_FAILED(evaluated))
        {
            lastResult_ = static_cast<int32_t>(evaluated);
            reason = std::string("frame-generation evaluation failed: 0x") +
                std::to_string(static_cast<unsigned>(evaluated));
            Log("%s: EvaluateFeature failed result=0x%08x index=%u", Name(),
                static_cast<unsigned>(evaluated), index);
            return false;
        }
        // The interpolation-disable verdict is copied here, on the list the SDK just recorded into,
        // so the readback is queued behind that evaluation. Recording it in a second list would cost
        // a whole queue round-trip before the first generated image could be presented.
        GeneratedSlot& written = slots_[index - 1];
        Dx12Interop::Transition(list, written.disable.Get(), written.disableState,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(written.readback.Get(), written.disable.Get());
        Dx12Interop::Transition(list, written.disable.Get(), written.disableState,
            D3D12_RESOURCE_STATE_COMMON);
    }
    // The application closes and executes the list the SDK recorded into. If the SDK already closed
    // it, executing a closed list is legal.
    if (present.hudless != nullptr)
        Dx12Interop::Transition(list, present.hudless, *present.hudlessState, D3D12_RESOURCE_STATE_COMMON);
    Dx12Interop::Transition(list, present.motion, *present.motionState, D3D12_RESOURCE_STATE_COMMON);
    Dx12Interop::Transition(list, present.depth, *present.depthState, D3D12_RESOURCE_STATE_COMMON);
    Dx12Interop::Transition(list, present.color, *present.colorState, D3D12_RESOURCE_STATE_COMMON);
    pendingReal_ = present.color;
    pendingColorState_ = present.colorState;
    if (!SubmitList(list, sdkList_, present.sourceFenceValue, reason)) return false;
    present.executed = true;
    present.submissionFence = submitFence_.Get();
    present.submissionValue = sdkList_.value;
    pairSlots_ = slots;
    return true;
}

bool NgxFgProvider::ReadDisableInterpolation(uint32_t slots, std::string& reason)
{
    if (slots > requiredSlots_)
    {
        reason = "the interpolation-disable outputs were not allocated for this frame";
        return false;
    }
    if (createEvent_ == nullptr)
    {
        reason = "no event is available for the interpolation-disable readback";
        return false;
    }
    frameDisabled_ = false;
    disableByte_ = -1;
    for (uint32_t i = 0; i < slots; i++)
    {
        GeneratedSlot& slot = slots_[i];
        if (slot.readback == nullptr) continue;
        // The SDK defines this output as a Boolean in byte 0; reading a uint32 would include three
        // bytes of unrelated readback data.
        const D3D12_RANGE readRange{0, 1};
        void* mapped = nullptr;
        if (FAILED(slot.readback->Map(0, &readRange, &mapped)) || mapped == nullptr)
        {
            reason = "the interpolation-disable readback buffer could not be mapped";
            return false;
        }
        const uint8_t disabled = *static_cast<const uint8_t*>(mapped);
        const D3D12_RANGE writeRange{0, 0};
        slot.readback->Unmap(0, &writeRange);
        slot.disabled = disabled != 0;
        if (slot.disabled) frameDisabled_ = true;
        // The harness reports one byte; the first non-zero verdict names the suppression.
        if (disableByte_ < 0 || disabled != 0) disableByte_ = static_cast<int32_t>(disabled);
    }
    if (disableByte_ < 0) disableByte_ = 0;
    return true;
}

bool NgxFgProvider::CopyGenerated(FgPresent& present, uint32_t index, std::string& reason)
{
    if (index >= pairSlots_ || swapchain_ == nullptr)
    {
        reason = "the generated intermediate is not pending for presentation";
        return false;
    }
    ID3D12GraphicsCommandList* list = nullptr;
    if (!BeginListRing(copyList_, reason, &list)) return false;
    ID3D12Resource* source = slots_[index].image.Get();
    D3D12_RESOURCE_STATES* sourceState = &slots_[index].imageState;
    const uint32_t current = swapchain_->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> target;
    if (FAILED(swapchain_->GetBuffer(current, IID_PPV_ARGS(&target))) || !target)
    {
        reason = "the generated-frame backbuffer could not be acquired";
        return false;
    }
    Dx12Interop::Transition(list, source, *sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.pResource = target.Get();
    barrier.Transition.StateBefore = childStates_[current];
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, &barrier);
    list->CopyResource(target.Get(), source);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &barrier);
    childStates_[current] = D3D12_RESOURCE_STATE_PRESENT;
    Dx12Interop::Transition(list, source, *sourceState, D3D12_RESOURCE_STATE_COMMON);
    if (!SubmitList(list, copyList_, 0, reason)) return false;
    // Published after the child copy was submitted: the reader sees exactly the image this present
    // will scan out, together with the provider's own fence that proves it is complete.
    FgTestGeneratedImage image;
    image.provider = Name();
    image.generation = present.frame.generation;
    image.sourceFrame = present.frame.frameIndex;
    image.reset = (present.frame.flags & BIGSCALE_FG_FLAG_RESET) != 0;
    image.intermediateIndex = index + 1;
    image.intermediateCount = pairSlots_;
    image.generated = slots_[index].image.Get();
    image.generatedState = slots_[index].imageState;
    image.childCopySource = source;
    image.childCopyState = *sourceState;
    image.childBufferIndex = current;
    image.childBackbuffer = target.Get();
    image.childBackbufferState = childStates_[current];
    image.realColor = present.color;
    image.realColorState = *present.colorState;
    image.evaluationColor = present.color;
    image.evaluationColorState = *present.colorState;
    image.evaluationHudless = present.hudless;
    image.evaluationHudlessState = present.hudless != nullptr ? *present.hudlessState
                                                             : D3D12_RESOURCE_STATE_COMMON;
    image.depth = present.depth;
    image.depthState = *present.depthState;
    image.motion = present.motion;
    image.motionState = *present.motionState;
    image.renderWidth = present.renderWidth;
    image.renderHeight = present.renderHeight;
    image.frame = &present.frame;
    image.width = present.outputWidth;
    image.height = present.outputHeight;
    image.queue = queue_;
    image.fence = submitFence_.Get();
    image.fenceValue = copyList_.value;
    ObserveFgGeneratedImage(image);
    return true;
}

bool NgxFgProvider::Present(FgPresent& present, std::string& reason)
{
    if (present.submissionFence != nullptr && present.submissionValue != 0)
    {
        if (present.submissionFence->GetCompletedValue() < present.submissionValue)
            present.submissionFence->SetEventOnCompletion(present.submissionValue, createEvent_);
        if (present.submissionFence->GetCompletedValue() < present.submissionValue &&
            !WaitEvent(createEvent_, 2000))
        {
            reason = "frame-generation submission did not complete";
            return false;
        }
    }
    const bool resetPair = (present.frame.flags & BIGSCALE_FG_FLAG_RESET) != 0;
    const uint32_t slots = pairSlots_;
    if (slots == 0 || !ReadDisableInterpolation(slots, reason))
    {
        lastResult_ = -1;
        return false;
    }
#ifdef BIGSCALE_TESTING
    FgTestPairTiming timing;
    timing.provider = Name();
    timing.generation = present.frame.generation;
    timing.sourceFrame = present.frame.frameIndex;
    timing.reset = resetPair;
    timing.disableByte = disableByte_;
#endif
    // Interpolation is suppressed for this pair: either an SDK output disabled it, or the transport
    // reset the history. Exactly one real Present keeps the display advancing without claiming a
    // generated frame, and no pacing deadline may delay an image that has no generated predecessor.
    //
    // The real image is always copied into the current child buffer first. That copy is the only
    // guarantee the buffer holds this frame's colour: Record skips the generated copies for a reset
    // pair, and the pinned disable-output contract only says interpolated frames must not be shown -
    // it does not promise that the SDK's disabled output already is the real frame.
    if (frameDisabled_ || resetPair)
    {
        cadence_.Reset();
        present.generatedCount = 0;
        const bool presentedReal = CopyRealAndPresent(present, reason, true);
#ifdef BIGSCALE_TESTING
        timing.realQpc = lastRealPresentQpc_;
        timing.realResult = lastRealPresentResult_;
        ObserveFgPairTiming(timing);
#endif
        return presentedReal;
    }

    // One source interval is split into `pairMultiplier_` equal steps. Every intermediate is copied
    // into the *current* child back buffer and presented on its own, in increasing index order,
    // before the real source frame. Preloading all intermediates into one buffer would present the
    // same image repeatedly.
    const double step = present.frame.deltaMilliseconds / static_cast<double>(pairMultiplier_);
#ifdef BIGSCALE_TESTING
    const int32_t traceProvider = flavor_ == Flavor::Nvidia ? BIGSCALE_FG_PROVIDER_NVIDIA
                                                           : BIGSCALE_FG_PROVIDER_ANR;
#endif
    uint32_t generatedCount = 0;
    for (uint32_t index = 0; index < slots; index++)
    {
        // A failure between intermediates keeps the images already scanned out counted and never
        // fabricates the ones that were not.
        if (index > 0 && !cadence_.BeforeNext(step, reason))
        {
            present.generatedCount = generatedCount;
            return false;
        }
        if (!CopyGenerated(present, index, reason))
        {
            present.generatedCount = generatedCount;
            return false;
        }
#if defined(BIGSCALE_TESTING)
        // Harness-only timing evidence: the shipping build formats nothing and calls no trace.
        char detail[64]{};
        std::snprintf(detail, sizeof(detail), "index=%u count=%u", index + 1, slots);
        TraceFgBoundary("generated-present-begin", traceProvider, present.frame.generation,
            present.frame.frameIndex, detail);
#endif
        const HRESULT generatedPresent = swapchain_->Present(1, 0);
#if defined(BIGSCALE_TESTING)
        std::snprintf(detail, sizeof(detail), "hr=0x%08x index=%u", static_cast<unsigned>(generatedPresent),
            index + 1);
        TraceFgBoundary("generated-present-end", traceProvider, present.frame.generation,
            present.frame.frameIndex, detail);
#endif
        const int64_t generatedQpc = FgQpcNow();
        if (generatedPresent == DXGI_STATUS_OCCLUDED)
        {
            // The compositor discarded this image: it was never scanned out, so neither it nor any
            // later intermediate is a generated frame. Present the real frame once and let a later
            // pair resume pacing.
            cadence_.Reset();
            present.generatedCount = generatedCount;
            const bool presentedReal = CopyRealAndPresent(present, reason);
#ifdef BIGSCALE_TESTING
            timing.generatedCount = generatedCount;
            timing.realQpc = lastRealPresentQpc_;
            timing.realResult = lastRealPresentResult_;
            ObserveFgPairTiming(timing);
#endif
            return presentedReal;
        }
        if (FAILED(generatedPresent))
        {
            Log("%s: generated Present failed hr=0x%08x index=%u", Name(),
                static_cast<unsigned>(generatedPresent), index + 1);
            // Images already presented stay counted; nothing is fabricated for this one.
            present.generatedCount = generatedCount;
            reason = "the generated frame could not be presented";
            return false;
        }
        generatedPresents_++;
        generatedCount++;
#ifdef BIGSCALE_TESTING
        if (index < 5)
        {
            timing.generatedQpc[index] = generatedQpc;
            timing.generatedResult[index] = generatedPresent;
        }
#endif
        cadence_.AfterPresent();
    }
    present.generatedCount = generatedCount;
    if (generatedCount > 0 && !cadence_.BeforeNext(step, reason)) return false;
    const bool presentedReal = CopyRealAndPresent(present, reason);
#ifdef BIGSCALE_TESTING
    timing.generatedCount = generatedCount;
    timing.realQpc = lastRealPresentQpc_;
    timing.realResult = lastRealPresentResult_;
    ObserveFgPairTiming(timing);
#endif
    return presentedReal;
}

bool NgxFgProvider::CopyRealAndPresent(FgPresent& present, std::string& reason, bool suppressedPair)
{
    if (pendingReal_ == nullptr || pendingColorState_ == nullptr || swapchain_ == nullptr)
    {
        reason = "the real frame is not pending for presentation";
        return false;
    }
    ID3D12Resource* const realSource = pendingReal_;
    ID3D12GraphicsCommandList* list = nullptr;
    if (!BeginListRing(realList_, reason, &list)) return false;
    const uint32_t current = swapchain_->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> target;
    if (FAILED(swapchain_->GetBuffer(current, IID_PPV_ARGS(&target))) || !target)
    {
        reason = "the real-frame backbuffer could not be acquired";
        return false;
    }
    Dx12Interop::Transition(list, pendingReal_, *pendingColorState_, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.pResource = target.Get();
    barrier.Transition.StateBefore = childStates_[current];
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, &barrier);
    list->CopyResource(target.Get(), pendingReal_);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &barrier);
    childStates_[current] = D3D12_RESOURCE_STATE_PRESENT;
    Dx12Interop::Transition(list, pendingReal_, *pendingColorState_, D3D12_RESOURCE_STATE_COMMON);
    if (!SubmitList(list, realList_, 0, reason)) return false;
    pendingReal_ = nullptr;
    pendingColorState_ = nullptr;
#ifdef BIGSCALE_TESTING
    if (suppressedPair)
    {
        // A suppressed pair presents the real image only: publish exactly what reached the child
        // buffer, so the child pixels can be compared with the retained real colour instead of being
        // assumed equal. The fence belongs to this real copy, and the callback consumes every
        // pointer synchronously before this method returns.
        FgTestGeneratedImage image;
        image.provider = Name();
        image.generation = present.frame.generation;
        image.sourceFrame = present.frame.frameIndex;
        image.reset = (present.frame.flags & BIGSCALE_FG_FLAG_RESET) != 0;
        image.realOnly = true;
        image.interpolationDisabled = frameDisabled_;
        image.generated = nullptr;
        image.childCopySource = realSource;
        image.childBackbuffer = target.Get();
        image.childBackbufferState = D3D12_RESOURCE_STATE_PRESENT;
        image.childBufferIndex = current;
        image.realColor = realSource;
        image.realColorState = D3D12_RESOURCE_STATE_COMMON;
        image.evaluationColor = present.color;
        image.evaluationColorState = *present.colorState;
        image.evaluationHudless = present.hudless;
        image.evaluationHudlessState = present.hudless != nullptr ? *present.hudlessState
                                                                 : D3D12_RESOURCE_STATE_COMMON;
        image.depth = present.depth;
        image.depthState = *present.depthState;
        image.motion = present.motion;
        image.motionState = *present.motionState;
        image.renderWidth = present.renderWidth;
        image.renderHeight = present.renderHeight;
        image.frame = &present.frame;
        image.width = present.outputWidth;
        image.height = present.outputHeight;
        image.queue = queue_;
        image.fence = submitFence_.Get();
        image.fenceValue = realList_.value;
        ObserveFgGeneratedImage(image);
    }
#endif
    // The caller owns cadence spacing: it waits one source-interval step before every present after
    // the first, so this path never adds a second wait.
#if defined(BIGSCALE_TESTING)
    char realDetail[64]{};
    std::snprintf(realDetail, sizeof(realDetail), "suppressed=%d disableByte=%d", suppressedPair ? 1 : 0,
        disableByte_);
    TraceFgBoundary("real-present-begin",
        flavor_ == Flavor::Nvidia ? BIGSCALE_FG_PROVIDER_NVIDIA : BIGSCALE_FG_PROVIDER_ANR,
        present.frame.generation, present.frame.frameIndex, realDetail);
#endif
    const HRESULT presented = swapchain_->Present(1, 0);
#if defined(BIGSCALE_TESTING)
    std::snprintf(realDetail, sizeof(realDetail), "hr=0x%08x", static_cast<unsigned>(presented));
    TraceFgBoundary("real-present-end",
        flavor_ == Flavor::Nvidia ? BIGSCALE_FG_PROVIDER_NVIDIA : BIGSCALE_FG_PROVIDER_ANR,
        present.frame.generation, present.frame.frameIndex, realDetail);
#endif
    lastRealPresentQpc_ = FgQpcNow();
    lastRealPresentResult_ = presented;
    if (FAILED(presented))
    {
        reason = "the real frame could not be presented";
        return false;
    }
    return true;
}

bool NgxFgProvider::ReleaseHandle(std::string& reason)
{
    if (handle_ == nullptr) return true;
    NVSDK_NGX_Result result;
    {
        std::lock_guard<std::recursive_mutex> guard(NgxMutex());
        result = flavor_ == Flavor::Nvidia
            ? NVSDK_NGX_D3D12_ReleaseFeature(handle_)
            : anr_.release(handle_);
    }
    if (NVSDK_NGX_FAILED(result))
    {
        reason = std::string("releasing the frame-generation feature failed: 0x") +
            std::to_string(static_cast<unsigned>(result));
        return false;
    }
    handle_ = nullptr;
    return true;
}

bool NgxFgProvider::DestroyFeature(std::string& reason)
{
    if (!ReleaseHandle(reason)) return false;
    if (createList_ != nullptr) createList_.Reset();
    if (createAllocator_ != nullptr) createAllocator_.Reset();
    if (createFence_ != nullptr) createFence_.Reset();
    if (createEvent_ != nullptr)
    {
        CloseHandle(createEvent_);
        createEvent_ = nullptr;
    }
    if (sdkInitialized_)
    {
        NVSDK_NGX_Result result;
        {
            std::lock_guard<std::recursive_mutex> guard(NgxMutex());
            result = flavor_ == Flavor::Nvidia
                ? NVSDK_NGX_D3D12_Shutdown1(device_)
                : anr_.shutdown1(device_);
        }
        if (NVSDK_NGX_FAILED(result))
        {
            // The instance may still be live inside the module. Keep the initialization flag set so
            // no later path can treat the teardown as successful.
            reason = std::string("shutting down the NGX D3D12 instance failed: 0x") +
                std::to_string(static_cast<unsigned>(result));
            return false;
        }
        sdkInitialized_ = false;
    }
    initialized_ = false;
    return true;
}

void NgxFgProvider::DestroySwapchain()
{
    cadence_.Detach();
    for (GeneratedSlot& slot : slots_)
    {
        slot.image.Reset();
        slot.disable.Reset();
        slot.readback.Reset();
        slot.imageState = D3D12_RESOURCE_STATE_COMMON;
        slot.disableState = D3D12_RESOURCE_STATE_COMMON;
        slot.disabled = false;
    }
    requiredSlots_ = 0;
    swapchain_.Reset();
    swapchainWidth_ = 0;
    swapchainHeight_ = 0;
    for (D3D12_RESOURCE_STATES& state : childStates_) state = D3D12_RESOURCE_STATE_COMMON;
}

void NgxFgProvider::Drain(std::string& reason)
{
    if (queue_ == nullptr || createFence_ == nullptr || createEvent_ == nullptr) return;
    const uint64_t value = ++createValue_;
    if (FAILED(queue_->Signal(createFence_.Get(), value)))
    {
        reason = "the frame-generation drain fence could not be signalled";
        return;
    }
    if (FAILED(createFence_->SetEventOnCompletion(value, createEvent_)))
    {
        reason = "the frame-generation drain fence could not be waited on";
        return;
    }
    if (!WaitEvent(createEvent_, 5000)) reason = "the frame-generation submission did not drain";
}

bool NgxFgProvider::Suspend(std::string& reason)
{
    cadence_.Reset();
    if (flavor_ != Flavor::Anr)
    {
        // A normal provider is never kept resident: the transport destroys it through the checked
        // drain/shutdown path. This entry point exists only so every caller can use one contract.
        Drain(reason);
        return reason.empty();
    }
    // Release only the feature handle. The module, SDK instance, parameter map, command lists,
    // device/queue and child swapchain stay alive: the SDK's hooks outlive any single feature.
    if (!WaitForSubmissions(5000, reason)) return false;
    if (!ReleaseHandle(reason)) return false;
    initialized_ = false;
    pendingReal_ = nullptr;
    pendingColorState_ = nullptr;
    Log("%s: feature suspended; the runtime stays resident for this process.", Name());
    return true;
}

bool NgxFgProvider::Resume(const FgInit& init, std::string& reason)
{
    if (flavor_ != Flavor::Anr)
    {
        reason = std::string(Name()) + " is not a process-resident provider";
        return false;
    }
    if (device_ == nullptr || queue_ == nullptr || module_ == nullptr || params_ == nullptr)
    {
        // A resident provider whose initialization never reached a usable SDK state can only report
        // that truthfully; manufacturing a success here would run a half-initialised SDK.
        reason = "the ANR runtime has no resident SDK state to resume";
        return false;
    }
    if (init.child == nullptr)
    {
        reason = "the presenter window is unavailable for the resumed provider";
        return false;
    }
    if (!WaitForSubmissions(5000, reason)) return false;
    child_ = init.child;
    if (!EnsureSwapchain(init, reason)) return false;
    if (!EnsureOutputTextures(init.width, init.height, 1, reason)) return false;
    // The feature handle is recreated by EnsureFeature from the next source frame's real extents;
    // the SDK instance itself is never initialised twice in one process.
    renderWidth_ = 0;
    renderHeight_ = 0;
    initialized_ = true;
    cadence_.Reset();
    Log("%s: resumed after suspension at %ux%u.", Name(), init.width, init.height);
    return true;
}

bool NgxFgProvider::Shutdown(std::string& reason)
{
    if (ProcessResident())
    {
        // Approved process-lifetime retention: ANR's hooks stay installed after a feature release,
        // so neither its module nor its D3D12 instance may be torn down inside this process.
        return true;
    }
    if (teardownFailed_)
    {
        reason = teardownReason_.empty() ? "the frame-generation provider failed to tear down"
                                         : teardownReason_;
        return false;
    }
    if (device_ == nullptr && module_ == nullptr && swapchain_ == nullptr && handle_ == nullptr &&
        !sdkInitialized_)
        return true;
    std::string waitReason;
    if (!WaitForSubmissions(5000, waitReason))
    {
        teardownFailed_ = true;
        teardownReason_ = waitReason;
        reason = waitReason;
        return false;
    }
    if (!DestroyFeature(reason))
    {
        teardownFailed_ = true;
        teardownReason_ = reason;
        return false;
    }
    DestroySwapchain();
    if (module_ != nullptr)
    {
        FreeLibrary(module_);
        module_ = nullptr;
    }
    anr_ = AnrApi{};
    params_ = nullptr;
    caps_ = nullptr;
    device_ = nullptr;
    queue_ = nullptr;
    child_ = nullptr;
    submitFence_.Reset();
    sdkList_ = ListRing{};
    copyList_ = ListRing{};
    realList_ = ListRing{};
    submitCounter_ = 0;
    pendingReal_ = nullptr;
    pendingColorState_ = nullptr;
    return true;
}
}
