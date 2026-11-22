// Frame-generation provider contract.
//
// One provider instance is owned by the frame-generation worker thread. Every method below is
// called from that thread only, so an implementation may keep non-atomic state. The worker owns the
// D3D12 interop instance: it begins one command list, hands it to the provider, and submits it.
#pragma once

#include "../FrameGenerationAbi.h"

#include <d3d12.h>
#include <windows.h>

#include <cstdint>
#include <string>

namespace bigscale
{
struct FgInit
{
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    HWND child = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    std::wstring runtimeDirectory;
    std::wstring dataDirectory;
    std::string engineVersion;
    // XeLL context created on the render thread, handed to XeSS-FG as its latency tracker.
    void* xellContext = nullptr;
    // Depth convention of the captured frames, fixed at creation time.
    bool reversedDepth = false;
    // Requested total presented frames per source frame: one real plus multiplier-1 generated.
    // Providers clamp nothing here: an unsupported value must be refused with an exact reason.
    uint32_t multiplier = 2;
};

struct FgPresent
{
    // Slot-owned shared textures, already transitioned to NON_PIXEL_SHADER_RESOURCE. `color` is the
    // final scene colour including HUD; `hudless` is the same frame immediately before overlay UI.
    ID3D12Resource* color = nullptr;
    ID3D12Resource* hudless = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    // Filled before Record so a self-executing provider can order its own queue work.
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* sourceFence = nullptr;
    uint64_t sourceFenceValue = 0;
    D3D12_RESOURCE_STATES* colorState = nullptr;
    D3D12_RESOURCE_STATES* hudlessState = nullptr;
    D3D12_RESOURCE_STATES* depthState = nullptr;
    D3D12_RESOURCE_STATES* motionState = nullptr;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t motionWidth = 0;
    uint32_t motionHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    uint32_t sourceId = 0;
    BIGSCALE_FG_Frame frame{};
    // Requested total presented frames per source frame for this source frame's generation.
    uint32_t multiplier = 2;
    // Filled by the worker after the recorded list is submitted, so a provider can retire small
    // readbacks (for example the interpolation-disable flag) before it presents.
    ID3D12Fence* submissionFence = nullptr;
    uint64_t submissionValue = 0;
    // Set by the provider to the number of generated images it actually presented for this source
    // frame. Zero means only the real frame reached scanout; it is never fabricated from the
    // requested multiplier.
    uint32_t generatedCount = 0;
    // Set when the provider recorded and executed its own command list.
    bool executed = false;
    // True when this provider requires a HUD-less colour input. The transport validates that the
    // slot actually carries one before recording.
    bool hudlessRequired = false;
};

class IFrameGenerationProvider
{
public:
    virtual ~IFrameGenerationProvider() = default;
    virtual const char* Name() const = 0;
    // True when Record submits its own work on queue and the transport must not use its list.
    virtual bool SelfExecutes() const { return false; }
    // Creates every SDK context and the child presentation path. False means the provider cannot
    // run here; the reason is published verbatim.
    virtual bool Initialize(const FgInit& init, std::string& reason) = 0;
    // Checks install prerequisites without loading the SDK, creating resources or touching the
    // device. The transport calls this before a process-family transition so a missing runtime
    // cannot claim the family. Default: no prerequisite.
    virtual bool Preflight(const FgInit& init, std::string& reason)
    {
        (void)init;
        (void)reason;
        return true;
    }
    // Records every GPU command for one source frame into present.list: SDK evaluation, proxy
    // copies and resource tagging. The worker closes and submits that list after it returns.
    virtual bool Record(FgPresent& present, std::string& reason) = 0;
    // Issues the presentation after the recorded list was submitted. False means this frame could
    // not be presented; the transport falls back to presenting the real frame only.
    virtual bool Present(FgPresent& present, std::string& reason) = 0;
    // Waits for every GPU/pacing operation this provider started, then frees what no longer moves.
    virtual void Drain(std::string& reason) = 0;
    // True when this provider's SDK cannot be unloaded inside the hosting process: its hooks stay
    // resident after every feature release, so the transport must retain the ownership graph until
    // the process exits and may only suspend the feature across provider changes.
    virtual bool ProcessResident() const { return false; }
    // Releases the live feature while keeping the SDK, module and device objects resident. The
    // transport calls this instead of Drain/Shutdown for a process-resident provider.
    virtual bool Suspend(std::string& reason) = 0;
    // Re-arms a suspended provider for the supplied child window and output extent. Only a
    // process-resident provider is resumed; normal providers are recreated from scratch.
    virtual bool Resume(const FgInit& init, std::string& reason) = 0;
    // Releases every SDK/resource this provider owns. A failure latches a terminal teardown failure
    // inside the provider; later calls return the saved result without repeating the failed call.
    // Successful teardown is idempotent.
    virtual bool Shutdown(std::string& reason) = 0;
    virtual uint64_t GeneratedPresents() const = 0;
    virtual int32_t LastResult() const = 0;
    // Total multiplier (one real plus generated) this provider proved it supports. Zero means the
    // capability is not known yet; the transport never reports a previous provider's value.
    virtual uint32_t MaximumMultiplier() const { return 2; }
};
}
