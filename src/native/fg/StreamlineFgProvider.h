// Streamline frame generation (NVIDIA DLSS-G on Ada).
//
// WORK IN PROGRESS: the provider, its profiled patch layer and its ABI value are implemented and
// verified in the offline harness at 2x/4x, but the in-game lifecycle (a resident provider across
// generation changes in the transport) is not finished. The managed panel therefore never offers
// this provider and holds a stored selection at Off; `BigScaleChecks --fg streamline` exercises it.
//
// Streamline is loaded dynamically and driven in manual-hooking mode: the game never links the
// interposer, so this provider upgrades its own child swapchain and presents through the proxy.
// DLSS-G owns presentation from then on; one source present may emit up to five images.
//
// The MFGAdaUnlock profile transactions are applied the moment the NVIDIA modules map, before the
// runtime caches the multi-frame capability.
#pragma once

#include "FrameGenerationProvider.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>
#include <mutex>

// Streamline types only; the inline helpers in these headers bind to the interposer import library,
// which this DLL must not depend on.
#include "sl.h"
#include "sl_dlss_g.h"
#include "sl_pcl.h"
#include "sl_reflex.h"

namespace bigscale
{
class StreamlineFgProvider final : public IFrameGenerationProvider
{
public:
    StreamlineFgProvider();
    ~StreamlineFgProvider() override;

    const char* Name() const override { return "NVIDIA DLSS-G (Streamline)"; }
    bool SelfExecutes() const override { return true; }
    // slInit/slShutdown cannot cycle inside a running process, so the SDK stays resident and a
    // provider change only suspends the live feature.
    bool ProcessResident() const override { return true; }

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
        PFun_slInit* init = nullptr;
        PFun_slShutdown* shutdown = nullptr;
        PFun_slSetD3DDevice* setDevice = nullptr;
        PFun_slIsFeatureSupported* isFeatureSupported = nullptr;
        PFun_slSetFeatureLoaded* setFeatureLoaded = nullptr;
        PFun_slGetFeatureFunction* getFeatureFunction = nullptr;
        PFun_slGetFeatureRequirements* getFeatureRequirements = nullptr;
        PFun_slUpgradeInterface* upgradeInterface = nullptr;
        PFun_slGetNativeInterface* getNativeInterface = nullptr;
        PFun_slGetNewFrameToken* getNewFrameToken = nullptr;
        PFun_slSetConstants* setConstants = nullptr;
        PFun_slSetTagForFrame* setTagForFrame = nullptr;
        PFun_slDLSSGSetOptions* dlssgSetOptions = nullptr;
        PFun_slDLSSGGetState* dlssgGetState = nullptr;
        PFun_slReflexSetOptions* reflexSetOptions = nullptr;
        PFun_slReflexSleep* reflexSleep = nullptr;
    };

    bool LoadStreamline(std::string& reason);
    // Streamline is a process-wide singleton: slInit succeeds once, and a provider recreated for a
    // new configuration must reuse the module, the resolved API and the initialized SDK.
    static Api& SharedApi();
    static HMODULE& SharedModule();
    static bool& SharedInitialized();
    static ID3D12Device*& SharedDevice();
    static uint32_t& SharedFrameIndex();
    static std::mutex& SharedMutex();
    bool ResolveCoreApi(std::string& reason);
    bool ResolveFeatureApi(std::string& reason);
    bool EnsureSwapchain(const FgInit& init, std::string& reason);
    bool EnsureOutput(const FgInit& init, std::string& reason);
    bool UploadRealFrame(FgPresent& present, std::string& reason);
    void ApplyConstants(const FgPresent& present);
    void ApplyTags(const FgPresent& present);
    bool ApplyOptions(uint32_t multiplier, uint32_t mvecWidth, uint32_t mvecHeight,
        DXGI_FORMAT mvecFormat, DXGI_FORMAT depthFormat, std::string& reason);
    bool ProbeCapability(std::string& reason);
    void ReleaseSwapchain();

    Api api_{};
    HMODULE interposer_ = nullptr;
    bool initialized_ = false;
    bool suspended_ = false;

    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    HWND hwnd_ = nullptr;
    std::wstring runtimeDirectory_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_R8G8B8A8_UNORM;

    Microsoft::WRL::ComPtr<ID3D12Device> proxyDevice_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> childQueue_;
    Microsoft::WRL::ComPtr<IDXGIFactory2> proxyFactory_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> proxySwapchain_;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter_;

    // The real frame is copied into the swapchain backbuffer before the present DLSS-G intercepts.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> copyAllocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> copyList_;
    Microsoft::WRL::ComPtr<ID3D12Fence> copyFence_;
    HANDLE copyEvent_ = nullptr;
    uint64_t copyValue_ = 0;
    int32_t lastHistoryEpoch_ = 0;
    uint32_t lastRenderWidth_ = 0;
    uint32_t lastRenderHeight_ = 0;

    sl::FrameToken* token_ = nullptr;
    uint32_t frameIndex_ = 0;
    uint32_t maximumMultiplier_ = 0;
    uint32_t appliedMultiplier_ = 0;
    uint32_t appliedWidth_ = 0;
    uint32_t appliedHeight_ = 0;
    uint32_t appliedMvecWidth_ = 0;
    uint32_t appliedMvecHeight_ = 0;
    uint32_t generatedPresents_ = 0;
    uint32_t idlePresents_ = 0;
    int32_t lastResult_ = 0;
    bool tearingSupported_ = false;
    bool featureEnabled_ = false;
    std::string lastFailure_;
};
}
