// AMD FidelityFX frame generation through the pinned DX12 effect DLL.
//
// The context is created with the SDK's "do not modify the swapchain" flag: the application owns
// presentation. That keeps one presenter path for every provider (the shared child swapchain and
// display-paced 2x), avoids depending on the SDK's internal pacing queue, and still uses the real
// FidelityFX frame-generation context for the interpolated image.
#pragma once

#include "FgCadence.h"
#include "FgMath.h"
#include "FrameGenerationProvider.h"
#include "FsrFgRuntime.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace bigscale
{
class FsrFgProvider final : public IFrameGenerationProvider
{
public:
    FsrFgProvider();
    ~FsrFgProvider() override;

    const char* Name() const override { return "AMD FSR-FG"; }
    bool SelfExecutes() const override { return true; }
    bool Initialize(const FgInit& init, std::string& reason) override;
    bool Record(FgPresent& present, std::string& reason) override;
    bool Present(FgPresent& present, std::string& reason) override;
    void Drain(std::string& reason) override;
    bool Suspend(std::string& reason) override;
    bool Resume(const FgInit& init, std::string& reason) override;
    bool Shutdown(std::string& reason) override;
    uint64_t GeneratedPresents() const override { return generatedPresents_; }
    int32_t LastResult() const override { return lastResult_; }

private:
    bool CreateSwapchain(const FgInit& init, std::string& reason);
    bool CreateContexts(const FgInit& init, std::string& reason);
    bool WaitOutstanding(uint32_t timeoutMilliseconds, std::string& reason);
    bool EnsureOutputTexture(uint32_t width, uint32_t height, std::string& reason);
    // The FidelityFX dispatch executes the command list it records, so the provider owns the list
    // and submits it behind the D3D11 capture fence itself.
    bool BeginOwnList(ID3D12GraphicsCommandList** list, std::string& reason);
    // Closes and submits the list this provider recorded, then signals its fence. False means the
    // submission is not proven complete; `present.executed` is never set in that case.
    bool ExecuteOwnList(std::string& reason);
    // Reuses one persistent allocator/list pair, never resetting objects the GPU may still execute.
    bool BeginRealList(ID3D12GraphicsCommandList** list, std::string& reason);
    bool CopyRealAndPresent(FgPresent& present, std::string& reason);
    bool DestroyContexts(std::string& reason);
    void DestroySwapchain();

    FsrFgRuntime runtime_;
    void* fgContext_ = nullptr;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> child_;
    Microsoft::WRL::ComPtr<ID3D12Resource> generated_;
    D3D12_RESOURCE_STATES generatedState_ = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES childStates_[3] = {D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_COMMON};
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    ID3D12CommandQueue* queue_ = nullptr;
    HWND childWindow_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    struct ListSlot
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        HANDLE event = nullptr;
        uint64_t value = 0;
    };
    ListSlot lists_[3];
    ListSlot realList_;
    uint32_t nextList_ = 0;
    ListSlot* current_ = nullptr;
    ID3D12Fence* sourceFence_ = nullptr;
    uint64_t sourceFenceValue_ = 0;
    ID3D12Resource* pendingReal_ = nullptr;
    D3D12_RESOURCE_STATES* pendingColorState_ = nullptr;
    uint64_t frameId_ = 0;
    uint64_t generatedPresents_ = 0;
    int32_t lastResult_ = 0;
    // A reset dispatch outputs a copy of the current frame, not an interpolation: it presents the
    // real frame once and never counts as generated.
    bool resetCopy_ = false;
    FgCadence cadence_;
    int64_t lastRealPresentQpc_ = 0;
    long lastRealPresentResult_ = 0;

    std::wstring runtimeDirectory_;
    Microsoft::WRL::ComPtr<ID3D12Fence> drainFence_;
    HANDLE drainEvent_ = nullptr;
    uint64_t drainValue_ = 0;
    bool teardownFailed_ = false;
    std::string teardownReason_;
};
}
