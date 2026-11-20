// NVIDIA DLSS Super Resolution through the pinned NGX D3D11 API.
#pragma once

#include "Upscaler.h"

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace bigscale
{
class NgxBackend final : public IUpscalerBackend
{
public:
    explicit NgxBackend(bool presetL = false);

    UpscalerKind Kind() const override { return presetL_ ? UpscalerKind::DlssPresetL : UpscalerKind::Dlss; }

    void SetEngineVersion(const std::string& engineVersion) override;
    void SetRuntimeDirectory(const std::wstring& runtimeDirectory) override;
    void SetDataDirectory(const std::wstring& dataDirectory) override;

    bool Attach(ID3D11Device* device, ID3D11DeviceContext* context) override;
    bool Available() override;
    uint32_t RuntimeVersion() const override { return runtimeVersion_; }

    bool QueryModes(uint32_t outputWidth, uint32_t outputHeight, ModeRange (&ranges)[kModeCount],
                    uint32_t& runtimeVersion) override;
    int32_t Evaluate(const ViewKey& key, const EvaluationRequest& request, bool& featureCreated) override;
    void Forget(const ViewKey& key) override;
    void CollectRetired(ID3D11DeviceContext* context) override;
    void Release() override;

private:
    struct Feature;
    struct Retired;

    Feature* Find(const ViewKey& key);
    Feature* Create(const ViewKey& key);
    bool EnsureInitialized(ID3D11Device* device);
    void Retire(Feature& feature);
    void DestroyFeature(Feature& feature);

    // Preset L is a create-time hint; it never changes after the backend is constructed.
    bool presetL_ = false;
    std::string engineVersion_ = "6000.3.17f1";
    std::wstring runtimeDirectory_;
    std::wstring dataDirectory_;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    void* parameters_ = nullptr;   // NVSDK_NGX_Parameter*
    bool initialized_ = false;
    bool ready_ = false;
    uint32_t runtimeVersion_ = 0;
    Feature* features_ = nullptr;
    Retired* retired_ = nullptr;
};
}
