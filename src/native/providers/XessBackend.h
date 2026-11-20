// Intel XeSS Super Resolution through the shared background-DX12 bridge.
#pragma once

#include "../Dx12Interop.h"
#include "../Upscaler.h"

#include <d3d11_4.h>
#include <d3d12.h>

#include <cstdint>
#include <string>
#include <vector>

struct _xess_context_handle_t;
typedef struct _xess_context_handle_t* xess_context_handle_t;

namespace bigscale
{
class XessBackend final : public IUpscalerBackend
{
public:
    ~XessBackend() override;

    UpscalerKind Kind() const override { return UpscalerKind::XeSs; }

    void SetRuntimeDirectory(const std::wstring& runtimeDirectory) override { runtimeDirectory_ = runtimeDirectory; }

    bool Attach(ID3D11Device* device, ID3D11DeviceContext* context) override;
    bool Available() override;
    uint32_t RuntimeVersion() const override { return runtimeVersion_; }

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
    bool EnsureInterop();
    bool EnsureShared(SharedTexture& texture, uint32_t width, uint32_t height, DXGI_FORMAT format,
                      bool unorderedAccess, std::string& reason);
    View* EnsureView(const ViewKey& key);
    bool EnsureContext(View& view, const EvaluationRequest& request);
    void Destroy(View& view);
    int32_t QualitySetting(Preset preset) const;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4_;
    std::wstring runtimeDirectory_;
    HMODULE module_ = nullptr;
    Api* api_ = nullptr;
    Dx12Interop interop_;
    std::vector<View*> views_;
    // Views a delayed retire kept alive because the D3D12 bridge had not gone idle.
    std::vector<View*> retired_;
    struct QueryEntry
    {
        uint32_t width = 0;
        uint32_t height = 0;
        bool ok = false;
        ModeRange ranges[kModeCount]{};
    };
    std::vector<QueryEntry> queries_;
    uint32_t runtimeVersion_ = 0;
    bool attached_ = false;
};
}
