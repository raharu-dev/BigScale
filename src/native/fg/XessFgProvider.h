// Intel XeSS frame generation and XeLL latency reduction.
//
// XeSS-FG owns a proxy swapchain created from the window description; the application renders (or
// copies) the real frame into the proxy's current back buffer and tags depth/motion plus the frame
// constants for the current present id. XeLL is connected so the SDK can pace the generated frames.
#pragma once

#include "FrameGenerationProvider.h"

#include <xess_fg/xefg_swapchain.h>
#include <xess_fg/xefg_swapchain_d3d12.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace bigscale
{
class XessFgProvider final : public IFrameGenerationProvider
{
public:
    XessFgProvider();
    ~XessFgProvider() override = default;

    const char* Name() const override { return "Intel XeSS-FG"; }
    bool Initialize(const FgInit& init, std::string& reason) override;
    bool Record(FgPresent& present, std::string& reason) override;
    bool Present(FgPresent& present, std::string& reason) override;
    void Drain(std::string& reason) override;
    bool Suspend(std::string& reason) override;
    bool Resume(const FgInit& init, std::string& reason) override;
    bool Shutdown(std::string& reason) override;
    uint64_t GeneratedPresents() const override { return generatedPresents_; }
    int32_t LastResult() const override { return lastResult_; }
    uint32_t MaximumMultiplier() const override { return maximumMultiplier_; }

private:
    struct Api
    {
        xefg_swapchain_result_t (*createContext)(ID3D12Device*, xefg_swapchain_handle_t*) = nullptr;
        xefg_swapchain_result_t (*getProperties)(xefg_swapchain_handle_t, xefg_swapchain_d3d12_init_params_t*,
            uint32_t, uint32_t, uint32_t, xefg_swapchain_properties_t*) = nullptr;
        xefg_swapchain_result_t (*initFromDesc)(xefg_swapchain_handle_t, HWND, const DXGI_SWAP_CHAIN_DESC1*,
            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, ID3D12CommandQueue*, IDXGIFactory*,
            xefg_swapchain_d3d12_init_params_t*) = nullptr;
        xefg_swapchain_result_t (*getSwapchain)(xefg_swapchain_handle_t, const IID&, void**) = nullptr;
        xefg_swapchain_result_t (*setLatencyReduction)(xefg_swapchain_handle_t, void*) = nullptr;
        xefg_swapchain_result_t (*setEnabled)(xefg_swapchain_handle_t, uint32_t) = nullptr;
        xefg_swapchain_result_t (*setInterpolatedFrames)(xefg_swapchain_handle_t, uint32_t) = nullptr;
        xefg_swapchain_result_t (*tagFrameResource)(xefg_swapchain_handle_t, ID3D12GraphicsCommandList*,
            uint32_t, const xefg_swapchain_d3d12_resource_data_t*) = nullptr;
        xefg_swapchain_result_t (*tagFrameConstants)(xefg_swapchain_handle_t, uint32_t,
            const xefg_swapchain_frame_constant_data_t*) = nullptr;
        xefg_swapchain_result_t (*setPresentId)(xefg_swapchain_handle_t, uint32_t) = nullptr;
        xefg_swapchain_result_t (*getLastPresentStatus)(xefg_swapchain_handle_t,
            xefg_swapchain_present_status_t*) = nullptr;
        xefg_swapchain_result_t (*destroy)(xefg_swapchain_handle_t) = nullptr;
    };

    bool LoadApi(const std::wstring& runtimeDirectory, std::string& reason);
    bool CopyRealColor(ID3D12GraphicsCommandList* list, std::string& reason);

    HMODULE module_ = nullptr;
    Api api_{};
    xefg_swapchain_handle_t swapchain_ = nullptr;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> proxy_;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory_;
    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    ID3D12Resource* realColor_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t presentId_ = 0;
    // Maximum interpolations the SDK proved after context creation (total = this + 1), and the
    // interpolation count currently applied to the proxy.
    uint32_t maxInterpolated_ = 1;
    uint32_t interpolated_ = 1;
    uint32_t maximumMultiplier_ = 0;
    bool reversedDepth_ = false;
    uint64_t generatedPresents_ = 0;
    int32_t lastResult_ = 0;
    Microsoft::WRL::ComPtr<ID3D12Fence> drainFence_;
    HANDLE drainEvent_ = nullptr;
    uint64_t drainValue_ = 0;
    bool teardownFailed_ = false;
    std::string teardownReason_;
};
}
