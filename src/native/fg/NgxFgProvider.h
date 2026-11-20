// NVIDIA DLSS Frame Generation through NGX, and ANR through its provider DLL.
//
// Both flavors share one implementation because the parameter map contract is identical: the same
// dlssg parameters, the same camera matrices and the same multi-interpolation evaluation. What
// differs is only who owns the parameter map (NVIDIA's static loader, or a host map for ANR) and
// which module the entry points come from. One source frame records multiplier-1 evaluations, each
// with its own output texture and interpolation-disable output, and each image is presented in
// intermediate order before the real source frame.
#pragma once

#include "FgCadence.h"
#include "FgMath.h"
#include "FrameGenerationProvider.h"
#include "NgxParameterMap.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>

namespace bigscale
{
class NgxFgProvider final : public IFrameGenerationProvider
{
public:
    enum class Flavor
    {
        Nvidia,
        Anr,
    };

    explicit NgxFgProvider(Flavor flavor);
    ~NgxFgProvider() override = default;

    const char* Name() const override;
    bool SelfExecutes() const override { return true; }
    // ANR's hooks stay installed after every feature release, so its SDK is never unloaded inside
    // this process; the transport keeps the ownership graph until the process exits.
    bool ProcessResident() const override { return flavor_ == Flavor::Anr; }
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
    // Total multiplier equals one real frame plus at most this many generated intermediates. ANR
    // reports up to five; NVIDIA is bounded to three by this implementation's own fixed capacity.
    static constexpr uint32_t kMaxGeneratedFrames = 5;

    // One generated intermediate: the SDK's interpolated output, its interpolation-disable output
    // and the readback buffer that verdict is copied into. Only `requiredSlots_` of these exist.
    struct GeneratedSlot
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> image;
        D3D12_RESOURCE_STATES imageState = D3D12_RESOURCE_STATE_COMMON;
        Microsoft::WRL::ComPtr<ID3D12Resource> disable;
        D3D12_RESOURCE_STATES disableState = D3D12_RESOURCE_STATE_COMMON;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        bool disabled = false;
    };

    struct AnrApi
    {
        NVSDK_NGX_Result(NVSDK_CONV* init)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
            const NVSDK_NGX_FeatureCommonInfo*) = nullptr;
        NVSDK_NGX_Result(NVSDK_CONV* populate)(NVSDK_NGX_Parameter*) = nullptr;
        NVSDK_NGX_Result(NVSDK_CONV* create)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
            NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**) = nullptr;
        NVSDK_NGX_Result(NVSDK_CONV* evaluate)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
            NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback) = nullptr;
        NVSDK_NGX_Result(NVSDK_CONV* release)(NVSDK_NGX_Handle*) = nullptr;
        NVSDK_NGX_Result(NVSDK_CONV* shutdown1)(ID3D12Device*) = nullptr;
    };

    bool LoadAnr(std::string& reason);
    bool InitializeNvidia(const FgInit& init, std::string& reason);
    bool InitializeAnr(const FgInit& init, std::string& reason);
    bool SetStaticParameters(const FgInit& init, uint32_t renderWidth, uint32_t renderHeight,
        std::string& reason);
    bool EnsureFeature(FgPresent& present, std::string& reason);
    bool WaitForSubmissions(uint32_t timeoutMilliseconds, std::string& reason);
    bool CreateFeature(std::string& reason);
    bool EnsureSwapchain(const FgInit& init, std::string& reason);
    bool EnsureOutputTextures(uint32_t width, uint32_t height, uint32_t requiredSlots, std::string& reason);
    bool FillEvaluationParameters(FgPresent& present, uint32_t multiFrameCount, uint32_t multiFrameIndex,
        std::string& reason);
    bool ReadDisableInterpolation(uint32_t slots, std::string& reason);
    // Copies one generated intermediate into the child swapchain's current back buffer and submits
    // the copy. Presentation happens in the caller so every intermediate is an ordered present.
    bool CopyGenerated(FgPresent& present, uint32_t index, std::string& reason);
    // `suppressedPair` marks the reset/disabled branch: the callback then receives one real-only
    // observation of the child buffer the real copy wrote. The caller owns cadence spacing.
    bool CopyRealAndPresent(FgPresent& present, std::string& reason, bool suppressedPair = false);
    // One reusable allocator/list pair. A pair is only reset after its last submission completed, so
    // no object the GPU may still be executing is released or rewritten.
    struct ListRing
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        uint64_t value = 0;
    };
    bool BeginListRing(ListRing& ring, std::string& reason, ID3D12GraphicsCommandList** list);
    bool SubmitList(ID3D12GraphicsCommandList* list, ListRing& ring, uint64_t sourceFenceValue,
        std::string& reason);
    bool DestroyFeature(std::string& reason);
    bool ReleaseHandle(std::string& reason);
    void DestroySwapchain();

    Flavor flavor_;
    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    HWND child_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t renderWidth_ = 0;
    uint32_t renderHeight_ = 0;

    std::wstring anrDirectory_;
    HMODULE module_ = nullptr;
    AnrApi anr_{};
    // Matrix storage the parameter map points at; NGX reads it during EvaluateFeature.
    FgMatrix viewToClip_;
    FgMatrix clipToView_;
    FgMatrix clipToPrevClip_;
    FgMatrix prevClipToClip_;
    FgMatrix clipToLensClip_;
    NgxParameterMap map_;
    NVSDK_NGX_Parameter* params_ = nullptr;
    NVSDK_NGX_Parameter* caps_ = nullptr;
    NVSDK_NGX_Handle* handle_ = nullptr;
    bool initialized_ = false;
    // The SDK instance was created successfully; it may still need Shutdown1 even when the
    // provider never became ready.
    bool sdkInitialized_ = false;
    bool teardownFailed_ = false;
    std::string teardownReason_;

    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain_;
    GeneratedSlot slots_[kMaxGeneratedFrames];
    uint32_t requiredSlots_ = 0;
    D3D12_RESOURCE_STATES childStates_[3] = {D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_COMMON};
    uint32_t swapchainWidth_ = 0;
    uint32_t swapchainHeight_ = 0;
    // Total multiplier (one real plus generated) the SDK proved for this flavor. Zero means the
    // capability query has not answered yet; the managed side then bootstraps the safe 2x.
    uint32_t maximumMultiplier_ = 0;

    // Pacing of the generated/real pair on the manual child swapchain.
    FgCadence cadence_;
    int64_t lastRealPresentQpc_ = 0;
    long lastRealPresentResult_ = 0;

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> createAllocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> createList_;
    Microsoft::WRL::ComPtr<ID3D12Fence> createFence_;
    Microsoft::WRL::ComPtr<ID3D12Fence> submitFence_;
    ListRing sdkList_;
    ListRing copyList_;
    ListRing realList_;
    ID3D12Fence* sourceFence_ = nullptr;
    uint64_t submitCounter_ = 0;
    ID3D12Resource* pendingReal_ = nullptr;
    D3D12_RESOURCE_STATES* pendingColorState_ = nullptr;
    HANDLE createEvent_ = nullptr;
    uint64_t createValue_ = 0;

    uint64_t generatedPresents_ = 0;
    int32_t lastResult_ = 0;
    bool frameDisabled_ = false;
    // Multiplier and generated-image state of the pair currently between Record and Present.
    uint32_t pairMultiplier_ = 2;
    uint32_t pairSlots_ = 0;
    // The raw interpolation-disable byte of the last pair (-1 before the first readback). The test
    // observer reports it so an SDK that never writes the output is distinguishable from one that
    // deliberately suppressed a frame.
    int32_t disableByte_ = -1;
};
}
