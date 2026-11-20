// AMD FidelityFX Super Resolution 2 on the game's D3D11 device.
#pragma once

#include "../Upscaler.h"

#include <d3d11.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bigscale
{
class Fsr2Backend final : public IUpscalerBackend
{
public:
    ~Fsr2Backend() override;

    UpscalerKind Kind() const override { return UpscalerKind::Fsr2; }

    void SetEngineVersion(const std::string& engineVersion) override { (void)engineVersion; }
    void SetRuntimeDirectory(const std::wstring& runtimeDirectory) override { (void)runtimeDirectory; }
    void SetDataDirectory(const std::wstring& dataDirectory) override { (void)dataDirectory; }

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

    View* Find(const ViewKey& key);
    View* EnsureView(const ViewKey& key);
    bool EnsureContext(View& view, const EvaluationRequest& request);
    void Destroy(View& view);

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    std::vector<View*> views_;
    uint32_t runtimeVersion_ = 0;
    bool ready_ = false;
};
}
