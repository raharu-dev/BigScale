#include "FsrFgProvider.h"

#include "../Dx12Interop.h"
#include "../Log.h"
#include "FgTestObserver.h"

#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace bigscale
{
namespace
{
constexpr uint32_t kSwapchainBuffers = 3;

std::string Hex(long value)
{
    char text[16]{};
    std::snprintf(text, sizeof(text), "%08lx", static_cast<unsigned long>(value));
    return text;
}
}

FsrFgProvider::FsrFgProvider() = default;

FsrFgProvider::~FsrFgProvider() = default;

bool FsrFgProvider::CreateSwapchain(const FgInit& init, std::string& reason)
{
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory)
    {
        reason = "the frame-generation DXGI factory could not be created";
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
        !swapchain || FAILED(swapchain.As(&child_)) || !child_)
    {
        reason = "the frame-generation presenter swapchain could not be created";
        return false;
    }
    UINT supported = 0;
    if (FAILED(child_->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &supported)) ||
        supported == 0 || FAILED(child_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)))
    {
        reason = "frame generation requires an SDR presenter";
        return false;
    }
    if (!cadence_.Attach(swapchain.Get(), reason)) return false;
    return true;
}

bool FsrFgProvider::CreateContexts(const FgInit& init, std::string& reason)
{
    return runtime_.CreateContext(device_.Get(), init.width, init.height, init.reversedDepth, &fgContext_, reason);
}

bool FsrFgProvider::WaitOutstanding(uint32_t timeoutMilliseconds, std::string& reason)
{
    auto wait = [&](ListSlot& slot) -> bool
    {
        if (slot.fence == nullptr || slot.value == 0) return true;
        if (slot.fence->GetCompletedValue() >= slot.value) return true;
        if (slot.event == nullptr)
        {
            slot.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (slot.event == nullptr)
            {
                reason = "the frame-generation submission event could not be created";
                return false;
            }
        }
        if (FAILED(slot.fence->SetEventOnCompletion(slot.value, slot.event)))
        {
            reason = "the frame-generation submission fence could not be waited on";
            return false;
        }
        if (WaitForSingleObject(slot.event, timeoutMilliseconds) != WAIT_OBJECT_0)
        {
            reason = "the frame-generation submission did not complete";
            return false;
        }
        return true;
    };
    for (ListSlot& slot : lists_)
        if (!wait(slot)) return false;
    return wait(realList_);
}

bool FsrFgProvider::EnsureOutputTexture(uint32_t width, uint32_t height, std::string& reason)
{
    if (generated_ != nullptr && width_ == width && height_ == height) return true;
    // The previous output may still be read by a submission; prove completion before replacing it.
    if (!WaitOutstanding(2000, reason)) return false;
    width_ = width;
    height_ = height;
    generated_.Reset();
    generatedState_ = D3D12_RESOURCE_STATE_COMMON;
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
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&generated_))) ||
        !generated_)
    {
        reason = "the FSR generated-frame output texture could not be created";
        return false;
    }
    return true;
}

bool FsrFgProvider::Initialize(const FgInit& init, std::string& reason)
{
    if (teardownFailed_)
    {
        reason = teardownReason_.empty() ? "a previous frame-generation teardown failed" : teardownReason_;
        return false;
    }
    device_ = init.device;
    queue_ = init.queue;
    childWindow_ = init.child;
    width_ = init.width;
    height_ = init.height;
    runtimeDirectory_ = init.runtimeDirectory;
    if (device_ == nullptr || queue_ == nullptr || init.child == nullptr)
    {
        reason = "frame generation needs a device, a queue and the presenter window";
        return false;
    }
    if (!runtime_.Load(runtimeDirectory_, reason)) return false;
    if (!runtime_.Select(device_.Get(), reason)) return false;
    if (!CreateSwapchain(init, reason)) return false;
    if (!CreateContexts(init, reason)) return false;
    if (!EnsureOutputTexture(width_, height_, reason)) return false;
    Log("FSR-FG: context and presenter ready %ux%u.", width_, height_);
    return true;
}

bool FsrFgProvider::BeginOwnList(ID3D12GraphicsCommandList** list, std::string& reason)
{
    ListSlot& slot = lists_[nextList_];
    nextList_ = (nextList_ + 1) % 3;
    if (slot.fence == nullptr)
    {
        const HRESULT fenceResult = device_ != nullptr
            ? device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.fence))
            : E_POINTER;
        if (FAILED(fenceResult))
        {
            const HRESULT removed = device_ != nullptr ? device_->GetDeviceRemovedReason() : E_POINTER;
            Log("FSR-FG: fence creation failed hr=0x%08x removed=0x%08x device=%p",
                static_cast<unsigned>(fenceResult), static_cast<unsigned>(removed), device_.Get());
            reason = "the frame-generation submission fence could not be created";
            return false;
        }
        slot.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    // Wait for the previous use of this ring entry, then release its list objects. The FidelityFX
    // dispatch is given a fresh list every frame because it may close the one it records into.
    if (slot.fence->GetCompletedValue() < slot.value)
    {
        if (slot.event == nullptr)
        {
            reason = "the frame-generation submission ring did not drain";
            return false;
        }
        slot.fence->SetEventOnCompletion(slot.value, slot.event);
        if (WaitForSingleObject(slot.event, 2000) != WAIT_OBJECT_0)
        {
            reason = "the frame-generation submission ring did not drain";
            return false;
        }
    }
    slot.list.Reset();
    slot.allocator.Reset();
    const HRESULT allocatorResult = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&slot.allocator));
    const HRESULT listResult = SUCCEEDED(allocatorResult)
        ? device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
            IID_PPV_ARGS(&slot.list))
        : allocatorResult;
    if (FAILED(allocatorResult) || FAILED(listResult))
    {
        Log("FSR-FG: list creation failed allocator=0x%08x list=0x%08x removed=0x%08x",
            static_cast<unsigned>(allocatorResult), static_cast<unsigned>(listResult),
            static_cast<unsigned>(device_->GetDeviceRemovedReason()));
        reason = "the frame-generation command list could not be created";
        return false;
    }
    current_ = &slot;
    *list = slot.list.Get();
    return true;
}

bool FsrFgProvider::ExecuteOwnList(std::string& reason)
{
    if (current_ == nullptr)
    {
        reason = "the frame-generation command list was not recorded";
        return false;
    }
    if (current_->list == nullptr || current_->fence == nullptr || queue_ == nullptr || device_ == nullptr)
    {
        reason = "the frame-generation submission ring is unavailable";
        current_ = nullptr;
        return false;
    }
    const HRESULT closed = current_->list->Close();
    if (FAILED(closed) && FAILED(device_->GetDeviceRemovedReason()))
    {
        // The SDK may already have closed the list; a removed device is the real failure.
        lastResult_ = static_cast<int32_t>(closed);
        reason = "the frame-generation command list could not be closed";
        current_ = nullptr;
        return false;
    }
    const uint64_t fenceValue = current_->value + 1;
    if (sourceFence_ != nullptr && sourceFenceValue_ > 0)
    {
        const HRESULT waited = queue_->Wait(sourceFence_, sourceFenceValue_);
        if (FAILED(waited))
        {
            lastResult_ = static_cast<int32_t>(waited);
            reason = "the frame-generation queue could not wait on the capture fence";
            current_ = nullptr;
            return false;
        }
    }
    ID3D12CommandList* lists[1] = {current_->list.Get()};
    queue_->ExecuteCommandLists(1, lists);
    const HRESULT signalled = queue_->Signal(current_->fence.Get(), fenceValue);
    if (FAILED(signalled))
    {
        lastResult_ = static_cast<int32_t>(signalled);
        reason = "the frame-generation submission fence could not be signalled";
        current_ = nullptr;
        return false;
    }
    current_->value = fenceValue;
    current_ = nullptr;
    return true;
}

bool FsrFgProvider::Record(FgPresent& present, std::string& reason)
{
    present.executed = false;
    resetCopy_ = false;
    present.generatedCount = 0;
    // The pinned FidelityFX integration consumes outputs[0] only. A higher total is refused with
    // its exact reason instead of duplicating one interpolation into several presents.
    if (present.multiplier > 2)
    {
        reason = "AMD FSR-FG supports at most 2x total (one real frame plus one generated frame); " +
            std::to_string(present.multiplier) + "x was requested";
        return false;
    }
    // A packet only owns pending work once its dispatch and submission actually succeeded.
    pendingReal_ = nullptr;
    pendingColorState_ = nullptr;
    if (fgContext_ == nullptr)
    {
        reason = "AMD frame generation is not ready";
        return false;
    }
    if (!EnsureOutputTexture(present.outputWidth, present.outputHeight, reason)) return false;
    sourceFence_ = present.sourceFence;
    sourceFenceValue_ = present.sourceFenceValue;
    ID3D12GraphicsCommandList* list = nullptr;
    if (!BeginOwnList(&list, reason)) return false;
    present.list = list;
    // ExecuteOwnList clears the current slot; the observation below needs the submitted fence.
    ListSlot* submitted = current_;

    Dx12Interop::Transition(list, present.color, *present.colorState,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Dx12Interop::Transition(list, present.depth, *present.depthState,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Dx12Interop::Transition(list, present.motion, *present.motionState,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (present.hudless != nullptr)
        Dx12Interop::Transition(list, present.hudless, *present.hudlessState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    frameId_++;
    ffxConfigureDescFrameGeneration configure{};
    configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    configure.swapChain = nullptr;
    configure.frameGenerationEnabled = true;
    configure.allowAsyncWorkloads = false;
    configure.HUDLessColor = present.hudless != nullptr
        ? ffxApiGetResourceDX12(present.hudless, FFX_API_RESOURCE_STATE_COMPUTE_READ)
        : ffxApiGetResourceDX12(nullptr);
    configure.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
#ifdef BIGSCALE_TESTING
    // Diagnostic-only: the SDK paints its reset/scene-change markers into the generated image when
    // this configure flag is set. It is never part of the create descriptor, never set for a
    // quality phase, and absent from shipping builds.
    const bool diagnosticIndicators = FgTestFsrResetIndicators();
    if (diagnosticIndicators)
        configure.flags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS;
#else
    constexpr bool diagnosticIndicators = false;
#endif
    configure.generationRect = {0, 0, static_cast<int32_t>(present.outputWidth),
        static_cast<int32_t>(present.outputHeight)};
    configure.frameID = frameId_;
    if (runtime_.api().configure(&fgContext_, &configure.header) != FFX_API_RETURN_OK)
    {
        reason = "ffxConfigure failed for this frame";
        return false;
    }
    ffxDispatchDescFrameGenerationPrepareV2 prepare{};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prepare.frameID = frameId_;
    prepare.commandList = list;
    prepare.renderSize = {present.renderWidth, present.renderHeight};
    prepare.jitterOffset = {present.frame.jitterX, present.frame.jitterY};
    // The transport hands previous-to-current vectors in presented-backbuffer rows with y down, so
    // both axes are negated to obtain FSR's current-to-previous pixel displacement.
    prepare.motionVectorScale = {-static_cast<float>(present.renderWidth),
        -static_cast<float>(present.renderHeight)};
    prepare.frameTimeDelta = present.frame.deltaMilliseconds;
    prepare.reset = (present.frame.flags & BIGSCALE_FG_FLAG_RESET) != 0;
    resetCopy_ = prepare.reset;
    // A history reset starts a new pair from scratch: the previous generated image must never delay
    // this frame's real present.
    if (resetCopy_) cadence_.Reset();
    prepare.cameraNear = present.frame.cameraNear;
    prepare.cameraFar = present.frame.cameraFar;
    prepare.cameraFovAngleVertical = present.frame.verticalFovRadians;
    prepare.viewSpaceToMetersFactor = 1.0f;
    prepare.depth = ffxApiGetResourceDX12(present.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    prepare.motionVectors = ffxApiGetResourceDX12(present.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    std::memcpy(prepare.cameraPosition, present.frame.cameraPosition, sizeof(prepare.cameraPosition));
    std::memcpy(prepare.cameraUp, present.frame.cameraUp, sizeof(prepare.cameraUp));
    std::memcpy(prepare.cameraRight, present.frame.cameraRight, sizeof(prepare.cameraRight));
    std::memcpy(prepare.cameraForward, present.frame.cameraForward, sizeof(prepare.cameraForward));
    const ffxReturnCode_t prepared = runtime_.api().dispatch(&fgContext_, &prepare.header);
    if (prepared != FFX_API_RETURN_OK)
    {
        lastResult_ = static_cast<int32_t>(prepared);
        reason = "ffxDispatch failed for the frame-generation prepare";
        Log("FSR-FG: prepare dispatch failed code=%u", static_cast<unsigned>(prepared));
        return false;
    }

    Dx12Interop::Transition(list, generated_.Get(), generatedState_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ffxDispatchDescFrameGeneration generate{};
    generate.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    generate.commandList = list;
    generate.presentColor = ffxApiGetResourceDX12(present.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    generate.outputs[0] = ffxApiGetResourceDX12(generated_.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    generate.numGeneratedFrames = 1;
    generate.reset = prepare.reset;
    generate.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    generate.minMaxLuminance[0] = 0.0f;
    generate.minMaxLuminance[1] = 0.0f;
    generate.generationRect = configure.generationRect;
    generate.frameID = frameId_;
    const ffxReturnCode_t generated = runtime_.api().dispatch(&fgContext_, &generate.header);
    if (generated != FFX_API_RETURN_OK)
    {
        lastResult_ = static_cast<int32_t>(generated);
        reason = "ffxDispatch failed for the frame-generation dispatch";
        Log("FSR-FG: generation dispatch failed code=%u", static_cast<unsigned>(generated));
        return false;
    }
#ifdef BIGSCALE_TESTING
    if (diagnosticIndicators)
    {
        // Only the reset diagnostic emits this. The frame id is the host identifier shared by the
        // configure, prepare and generate descriptors, and the rectangle is the one both dispatches
        // received.
        Log("FSR-FG: diagnostic frame id=%llu reset=%d rect=%d,%d %dx%d version=%u.%u.%u",
            static_cast<unsigned long long>(frameId_), resetCopy_ ? 1 : 0, configure.generationRect.left,
            configure.generationRect.top, configure.generationRect.width, configure.generationRect.height,
            runtime_.Version() >> 22, (runtime_.Version() >> 12) & 0x3ffu, runtime_.Version() & 0xfffu);
    }
#endif
    // Flip-model rule: one command list may only write the swapchain's current back buffer, and a
    // buffer may only be written once between presents. A reset dispatch produces a copy of the real
    // frame and is presented as the real image only, so its generated copy is skipped here and the
    // single write happens in Present.
    uint32_t current = 0;
    ComPtr<ID3D12Resource> generatedTarget;
    if (!resetCopy_)
    {
        Dx12Interop::Transition(list, generated_.Get(), generatedState_,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        current = child_->GetCurrentBackBufferIndex();
        if (FAILED(child_->GetBuffer(current, IID_PPV_ARGS(&generatedTarget))) || !generatedTarget)
        {
            reason = "the presenter backbuffer could not be acquired";
            return false;
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.pResource = generatedTarget.Get();
        barrier.Transition.StateBefore = childStates_[current];
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &barrier);
        list->CopyResource(generatedTarget.Get(), generated_.Get());
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &barrier);
        childStates_[current] = D3D12_RESOURCE_STATE_PRESENT;
    }

    // HUD-less colour, depth and motion are finished with; the real colour stays readable for the
    // real-frame copy.
    if (present.hudless != nullptr)
        Dx12Interop::Transition(list, present.hudless, *present.hudlessState, D3D12_RESOURCE_STATE_COMMON);
    Dx12Interop::Transition(list, present.motion, *present.motionState, D3D12_RESOURCE_STATE_COMMON);
    Dx12Interop::Transition(list, present.depth, *present.depthState, D3D12_RESOURCE_STATE_COMMON);

    if (!ExecuteOwnList(reason)) return false;
    // Only a submitted, fenced recording becomes pending present work.
    pendingReal_ = present.color;
    pendingColorState_ = present.colorState;
    present.executed = true;
    FgTestGeneratedImage image;
    image.provider = Name();
    image.generation = present.frame.generation;
    image.sourceFrame = present.frame.frameIndex;
    image.reset = resetCopy_;
    // A reset dispatch writes no child buffer (its generated copy is skipped), so publishing one
    // would let a reader sample the previous pair's contents. The reset diagnostic needs the SDK's
    // own reset output to read its marker, so only while its test switch is on is the generated
    // plane published for a reset image; the child and copy targets stay null.
    const bool diagnosticResetOutput = resetCopy_ && diagnosticIndicators;
    image.generated = (resetCopy_ && !diagnosticResetOutput) ? nullptr : generated_.Get();
    image.childCopySource = resetCopy_ ? nullptr : generated_.Get();
    image.generatedState = generatedState_;
    image.childCopyState = generatedState_;
    image.childBufferIndex = current;
    image.childBackbuffer = resetCopy_ ? nullptr : generatedTarget.Get();
    image.childBackbufferState = resetCopy_ ? D3D12_RESOURCE_STATE_COMMON : childStates_[current];
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
    image.fence = submitted != nullptr ? submitted->fence.Get() : nullptr;
    image.fenceValue = submitted != nullptr ? submitted->value : 0;
    ObserveFgGeneratedImage(image);
    return true;
}

bool FsrFgProvider::Present(FgPresent& present, std::string& reason)
{
    if (child_ == nullptr)
    {
        reason = "AMD frame generation has no presenter";
        return false;
    }
    if (!present.executed || pendingReal_ == nullptr || pendingColorState_ == nullptr)
    {
        reason = "the generated frame was not submitted for this packet";
        return false;
    }
#ifdef BIGSCALE_TESTING
    FgTestPairTiming timing;
    timing.provider = Name();
    timing.generation = present.frame.generation;
    timing.sourceFrame = present.frame.frameIndex;
    timing.reset = resetCopy_;
#endif
    if (resetCopy_)
    {
        // The SDK's reset output is a copy of the current real frame, not an interpolation. Present
        // the real frame once so the display keeps advancing without a duplicate image.
        cadence_.Reset();
        const bool presentedReal = CopyRealAndPresent(present, reason);
#ifdef BIGSCALE_TESTING
        timing.realQpc = lastRealPresentQpc_;
        timing.realResult = lastRealPresentResult_;
        ObserveFgPairTiming(timing);
#endif
        return presentedReal;
    }
    const HRESULT generatedPresent = child_->Present(1, 0);
#ifdef BIGSCALE_TESTING
    // The boundary trace exists for the offline pair-timing harness. A shipping build must not pay
    // for the formatting or call the no-op.
    {
        char detail[64]{};
        std::snprintf(detail, sizeof(detail), "resetCopy=%d sdk=%d", resetCopy_ ? 1 : 0, lastResult_);
        TraceFgBoundary("generated-present-begin", BIGSCALE_FG_PROVIDER_FSR, present.frame.generation,
            present.frame.frameIndex, detail);
        std::snprintf(detail, sizeof(detail), "hr=0x%08x sdk=%d", static_cast<unsigned>(generatedPresent),
            lastResult_);
        TraceFgBoundary("generated-present-end", BIGSCALE_FG_PROVIDER_FSR, present.frame.generation,
            present.frame.frameIndex, detail);
    }
#endif
    const int64_t generatedQpc = FgQpcNow();
    if (generatedPresent == DXGI_STATUS_OCCLUDED)
    {
        // Never scanned out: not a generated frame, and no deadline for the following pair.
        cadence_.Reset();
        const bool presentedReal = CopyRealAndPresent(present, reason);
#ifdef BIGSCALE_TESTING
        timing.generatedCount = 0;
        timing.realQpc = lastRealPresentQpc_;
        timing.realResult = lastRealPresentResult_;
        ObserveFgPairTiming(timing);
#endif
        return presentedReal;
    }
    if (FAILED(generatedPresent))
    {
        reason = "the generated frame could not be presented (0x" + Hex(generatedPresent) + ")";
        lastResult_ = static_cast<int32_t>(generatedPresent);
        return false;
    }
    generatedPresents_++;
    // FSR exposes exactly one interpolated output: a successful generated present is one image.
    // Higher totals are never simulated by duplicating this one.
    present.generatedCount = 1;
    cadence_.AfterPresent();
    if (!cadence_.BeforeNext(present.frame.deltaMilliseconds * 0.5, reason)) return false;
    if (generatedPresents_ == 1 || generatedPresents_ % 600 == 0)
        Log("FSR-FG: generated frame %llu presented.", static_cast<unsigned long long>(generatedPresents_));
    const bool presentedReal = CopyRealAndPresent(present, reason);
#ifdef BIGSCALE_TESTING
    timing.generatedCount = 1;
    timing.generatedQpc[0] = generatedQpc;
    timing.generatedResult[0] = generatedPresent;
    timing.realQpc = lastRealPresentQpc_;
    timing.realResult = lastRealPresentResult_;
    ObserveFgPairTiming(timing);
#endif
    return presentedReal;
}

bool FsrFgProvider::BeginRealList(ID3D12GraphicsCommandList** list, std::string& reason)
{
    ListSlot& slot = realList_;
    bool created = false;
    if (slot.allocator == nullptr)
    {
        const HRESULT allocatorResult = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&slot.allocator));
        const HRESULT listResult = SUCCEEDED(allocatorResult)
            ? device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                IID_PPV_ARGS(&slot.list))
            : allocatorResult;
        if (FAILED(allocatorResult) || FAILED(listResult))
        {
            reason = "the real-frame copy list could not be created";
            return false;
        }
        created = true;
    }
    if (slot.fence == nullptr)
    {
        if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.fence)))) 
        {
            reason = "the real-frame copy fence could not be created";
            return false;
        }
        slot.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    // Never reset objects the GPU may still be executing.
    if (slot.fence->GetCompletedValue() < slot.value)
    {
        if (slot.event == nullptr)
        {
            reason = "the real-frame copy ring did not drain";
            return false;
        }
        slot.fence->SetEventOnCompletion(slot.value, slot.event);
        if (WaitForSingleObject(slot.event, 2000) != WAIT_OBJECT_0)
        {
            reason = "the real-frame copy ring did not drain";
            return false;
        }
    }
    if (!created)
    {
        // An allocator cannot be reset while any list recorded on it is still open; closing an
        // already closed list is harmless and also recovers a list left open by a failed frame.
        slot.list->Close();
        const HRESULT allocatorReset = slot.allocator->Reset();
        const HRESULT listReset = SUCCEEDED(allocatorReset) ? slot.list->Reset(slot.allocator.Get(), nullptr)
                                                           : allocatorReset;
        if (FAILED(allocatorReset) || FAILED(listReset))
        {
            Log("FSR-FG: real-frame list reset failed allocator=0x%08x list=0x%08x value=%llu completed=%llu",
                static_cast<unsigned>(allocatorReset), static_cast<unsigned>(listReset),
                static_cast<unsigned long long>(slot.value),
                static_cast<unsigned long long>(slot.fence->GetCompletedValue()));
            reason = "the real-frame copy list could not be reset";
            return false;
        }
    }
    *list = slot.list.Get();
    return true;
}

bool FsrFgProvider::CopyRealAndPresent(FgPresent& present, std::string& reason)
{
    if (pendingReal_ == nullptr || pendingColorState_ == nullptr || child_ == nullptr)
    {
        reason = "the real frame is not pending for presentation";
        return false;
    }
    ID3D12GraphicsCommandList* list = nullptr;
    if (!BeginRealList(&list, reason)) return false;
    const uint32_t current = child_->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> target;
    if (FAILED(child_->GetBuffer(current, IID_PPV_ARGS(&target))) || !target)
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
    if (FAILED(list->Close()))
    {
        reason = "the real-frame copy list could not be closed";
        return false;
    }
    ID3D12CommandList* lists[1] = {list};
    queue_->ExecuteCommandLists(1, lists);
    const uint64_t fenceValue = realList_.value + 1;
    if (FAILED(queue_->Signal(realList_.fence.Get(), fenceValue)))
    {
        reason = "the real-frame copy fence could not be signalled";
        return false;
    }
    realList_.value = fenceValue;
    pendingReal_ = nullptr;
    pendingColorState_ = nullptr;
    // The caller already waited the half source-frame step after the generated present; the real
    // copy is queued behind it.
    const HRESULT presented = child_->Present(1, 0);
#ifdef BIGSCALE_TESTING
    {
        char detail[64]{};
        std::snprintf(detail, sizeof(detail), "resetCopy=%d sdk=%d", resetCopy_ ? 1 : 0, lastResult_);
        TraceFgBoundary("real-present-begin", BIGSCALE_FG_PROVIDER_FSR, present.frame.generation,
            present.frame.frameIndex, detail);
        std::snprintf(detail, sizeof(detail), "hr=0x%08x sdk=%d", static_cast<unsigned>(presented),
            lastResult_);
        TraceFgBoundary("real-present-end", BIGSCALE_FG_PROVIDER_FSR, present.frame.generation,
            present.frame.frameIndex, detail);
    }
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

void FsrFgProvider::Drain(std::string& reason)
{
    if (queue_ == nullptr || device_ == nullptr) return;
    if (drainFence_ == nullptr && FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&drainFence_))))
    {
        reason = "the frame-generation drain fence could not be created";
        return;
    }
    if (drainEvent_ == nullptr) drainEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const uint64_t value = ++drainValue_;
    if (FAILED(queue_->Signal(drainFence_.Get(), value)))
    {
        reason = "the frame-generation drain fence could not be signalled";
        return;
    }
    if (drainEvent_ == nullptr)
    {
        reason = "the frame-generation drain event is unavailable";
        return;
    }
    drainFence_->SetEventOnCompletion(value, drainEvent_);
    if (WaitForSingleObject(drainEvent_, 5000) != WAIT_OBJECT_0)
        reason = "the frame-generation queue did not drain";
}

bool FsrFgProvider::DestroyContexts(std::string& reason)
{
    if (fgContext_ == nullptr) return true;
    if (!runtime_.DestroyContext(&fgContext_))
    {
        reason = "ffxDestroyContext failed for the frame-generation context";
        return false;
    }
    return true;
}

void FsrFgProvider::DestroySwapchain()
{
    cadence_.Detach();
    generated_.Reset();
    generatedState_ = D3D12_RESOURCE_STATE_COMMON;
    child_.Reset();
}

bool FsrFgProvider::Suspend(std::string& reason)
{
    cadence_.Reset();
    // FSR is not process-resident: the transport destroys it through the checked drain/shutdown
    // path. This entry point exists only so every caller can use one contract.
    Drain(reason);
    return reason.empty();
}

bool FsrFgProvider::Resume(const FgInit& init, std::string& reason)
{
    (void)init;
    reason = "AMD FSR-FG is not a process-resident provider";
    return false;
}

bool FsrFgProvider::Shutdown(std::string& reason)
{
    if (teardownFailed_)
    {
        reason = teardownReason_.empty() ? "the frame-generation provider failed to tear down"
                                         : teardownReason_;
        return false;
    }
    if (fgContext_ == nullptr && !runtime_.Loaded() && child_ == nullptr) return true;
    // The transport drained already; this bounded wait proves no submission still reads the context.
    if (!WaitOutstanding(5000, reason))
    {
        teardownFailed_ = true;
        teardownReason_ = reason;
        return false;
    }
    if (!DestroyContexts(reason))
    {
        teardownFailed_ = true;
        teardownReason_ = reason;
        return false;
    }
    DestroySwapchain();
    for (ListSlot& slot : lists_)
    {
        if (slot.event != nullptr)
        {
            CloseHandle(slot.event);
            slot.event = nullptr;
        }
        slot.fence.Reset();
        slot.list.Reset();
        slot.allocator.Reset();
        slot.value = 0;
    }
    if (drainEvent_ != nullptr)
    {
        CloseHandle(drainEvent_);
        drainEvent_ = nullptr;
    }
    drainFence_.Reset();
    drainValue_ = 0;
    current_ = nullptr;
    sourceFence_ = nullptr;
    sourceFenceValue_ = 0;
    pendingReal_ = nullptr;
    pendingColorState_ = nullptr;
    runtime_.Unload();
    device_.Reset();
    queue_ = nullptr;
    childWindow_ = nullptr;
    frameId_ = 0;
    drainFence_.Reset();
    drainValue_ = 0;
    return true;
}
}
