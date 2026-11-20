// FidelityFX frame-generation runtime: module load, provider version selection and context
// creation/destruction, shared by the desktop presenter and the VR compositor path.
//
// Both callers must select, create and configure a frame-generation context the same way (the
// "do not modify the swapchain" flag, the pinned major version, the version cross-check after
// creation), so that policy lives here once. Everything a caller owns afterwards — its presenter
// swapchain, its command lists and its fences — stays in the provider.
#pragma once

#include <ffx_api.h>
#include <ffx_api_dx12.h>
#include <ffx_framegeneration.h>
#include <ffx_api_framegeneration_dx12.h>

#include <d3d12.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace bigscale
{
class FsrFgRuntime
{
public:
    struct Api
    {
        ffxReturnCode_t (*createContext)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*) = nullptr;
        ffxReturnCode_t (*destroyContext)(ffxContext*, const ffxAllocationCallbacks*) = nullptr;
        ffxReturnCode_t (*configure)(ffxContext*, const ffxConfigureDescHeader*) = nullptr;
        ffxReturnCode_t (*query)(ffxContext*, ffxQueryDescHeader*) = nullptr;
        ffxReturnCode_t (*dispatch)(ffxContext*, const ffxDispatchDescHeader*) = nullptr;
    };

    FsrFgRuntime() = default;
    ~FsrFgRuntime();

    FsrFgRuntime(const FsrFgRuntime&) = delete;
    FsrFgRuntime& operator=(const FsrFgRuntime&) = delete;

    // Loads amd_fidelityfx_framegeneration_dx12.dll from `runtimeDirectory` and every entry point.
    // Idempotent; the module stays loaded until Unload.
    bool Load(const std::wstring& runtimeDirectory, std::string& reason);
    bool Loaded() const { return module_ != nullptr; }

    // Enumerates the installed providers and pins the newest one of the requested major version.
    bool Select(ID3D12Device* device, std::string& reason);

    // Creates one context at display size with the no-swapchain contract. Descriptor storage is kept
    // per context until DestroyContext, so several contexts (one per eye) never share storage.
    bool CreateContext(ID3D12Device* device, uint32_t width, uint32_t height, bool reversedDepth,
                       void** context, std::string& reason);
    bool DestroyContext(void** context);

    void Unload();

    const Api& api() const { return api_; }
    // Packed provider version (major << 22) | (minor << 12) | patch, for diagnostics only.
    uint32_t Version() const { return version_; }

private:
    struct DescriptorStorage;

    HMODULE module_ = nullptr;
    Api api_{};
    uint64_t versionId_ = 0;
    uint32_t version_ = 0;
    std::unordered_map<void*, DescriptorStorage*> storage_;
};
}
