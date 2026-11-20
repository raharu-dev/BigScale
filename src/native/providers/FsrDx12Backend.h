// AMD FidelityFX Super Resolution 3.1 / 4 through the FidelityFX DX12 upscaler effect.
//
// Both providers run on the shared background-DX12 bridge: the game stays on D3D11, the SDK runs on
// a D3D12 device for the same adapter, and the two APIs are ordered with shared fences. The effect
// module is loaded directly because the pinned 2.3.0 loader only scans the executable directory; the
// requested major version is selected from the offered version ids and verified after creation, so a
// missing provider is Unsupported rather than a silent fallback.
#pragma once

#include "../Dx12Interop.h"
#include "../Upscaler.h"

#include <d3d11_4.h>
#include <d3d12.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bigscale
{
// Selects the provider version id for a requested major version. Ids encode
// (major << 22) | (minor << 12) | patch in the low word with bit 31 reserved for driver-provided
// implementations; names are diagnostic only and may be null.
bool SelectFsrProviderVersion(const uint64_t* ids, const char* const* names, size_t count,
                              int32_t majorVersion, uint64_t& selectedId, std::string& selectedName);

class FsrDx12Backend final : public IUpscalerBackend
{
public:
    FsrDx12Backend(UpscalerKind kind, int32_t majorVersion, bool amdOnly);

    ~FsrDx12Backend() override;

    UpscalerKind Kind() const override { return kind_; }

    void SetRuntimeDirectory(const std::wstring& runtimeDirectory) override { runtimeDirectory_ = runtimeDirectory; }

    bool Attach(ID3D11Device* device, ID3D11DeviceContext* context) override;
    bool Available() override;
    uint32_t RuntimeVersion() const override { return runtimeVersion_; }
    uint64_t ProviderVersionId() const override { return providerVersionId_; }

    bool QueryModes(uint32_t outputWidth, uint32_t outputHeight, ModeRange (&ranges)[kModeCount],
                    uint32_t& runtimeVersion) override;
    int32_t Evaluate(const ViewKey& key, const EvaluationRequest& request, bool& featureCreated) override;
    void Forget(const ViewKey& key) override;
    void Release() override;

private:
    struct View;
    struct Api;
    struct SharedTexture
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d11;
        Dx12Interop::Shared shared;
        uint32_t width = 0;
        uint32_t height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };

    bool LoadApi();
    bool AmdAdapter() const;
    bool EnumerateVersion();
    View* EnsureView(const ViewKey& key);
    bool EnsureShared(SharedTexture& texture, uint32_t width, uint32_t height, DXGI_FORMAT format,
                      bool unorderedAccess, std::string& reason);
    bool EnsureContext(View& view, const EvaluationRequest& request);
    void Destroy(View& view);
    const char* Label() const { return kind_ == UpscalerKind::Fsr4 ? "FSR 4" : "FSR 3.1"; }

    UpscalerKind kind_;
    int32_t majorVersion_;
    bool amdOnly_;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4_;
    std::wstring runtimeDirectory_;
    HMODULE module_ = nullptr;
    Api* api_ = nullptr;
    Dx12Interop interop_;
    std::vector<View*> views_;
    // Views a delayed retire kept alive because the D3D12 bridge had not gone idle. Released only by
    // this backend's own Release, which runs after the transport proved the GPU idle.
    std::vector<View*> retired_;
    uint64_t providerVersionId_ = 0;
    uint32_t runtimeVersion_ = 0;
    bool attached_ = false;
    bool amd_ = false;
};
}
