// Backend-neutral upscaling contract. One selected implementation is created on the render thread
// and owns its own SDK context, per-view features and scratch. The bridge never assumes NGX.
#pragma once

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace bigscale
{
// Stable wire values shared with managed UpscalerKind. Never renumber.
enum class UpscalerKind : int32_t
{
    Off = 0,
    Dlss = 1,
    Fsr2 = 2,
    Fsr3 = 3,
    Fsr4 = 4,
    XeSs = 5,
    // DLSS Super Resolution with NGX render preset L pinned at feature creation.
    DlssPresetL = 6,
};

// Provider-neutral preset, matching the native five-slot mode block order:
// Quality, Balanced, Performance, UltraPerformance, NativeAA.
enum class Preset : int32_t
{
    Quality = 1,
    Balanced = 2,
    Performance = 3,
    UltraPerformance = 4,
    NativeAA = 5,
};

constexpr int32_t kModeCount = 5;
constexpr int32_t kMaxViews = 2;

inline int32_t PresetIndex(Preset preset)
{
    switch (preset)
    {
        case Preset::Quality: return 0;
        case Preset::Balanced: return 1;
        case Preset::Performance: return 2;
        case Preset::UltraPerformance: return 3;
        case Preset::NativeAA: return 4;
    }
    return 0;
}

// Supported input range one preset reports at one output size.
struct ModeRange
{
    bool supported = false;
    uint32_t optimalWidth = 0;
    uint32_t optimalHeight = 0;
    uint32_t minWidth = 0;
    uint32_t minHeight = 0;
    uint32_t maxWidth = 0;
    uint32_t maxHeight = 0;
};

// Identity of one independently tracked SDK context and its history.
struct ViewKey
{
    int32_t cameraId = 0;
    int32_t eye = 0;

    bool operator==(const ViewKey& other) const
    {
        return cameraId == other.cameraId && eye == other.eye;
    }
};

struct EvaluationRequest
{
    ID3D11Resource* color = nullptr;
    ID3D11Resource* depth = nullptr;
    ID3D11Resource* motion = nullptr;
    ID3D11Resource* output = nullptr;
    uint32_t inputWidth = 0;
    uint32_t inputHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    uint32_t scratchWidth = 0;
    uint32_t scratchHeight = 0;
    int32_t inputX = 0;
    int32_t inputY = 0;
    int32_t outputX = 0;
    int32_t outputY = 0;
    int32_t slice = 0;
    int32_t sliceCount = 1;
    int32_t layout = 0;
    Preset preset = Preset::Quality;
    int32_t frameIndex = 0;
    int32_t historyEpoch = 0;
    int32_t motionWidth = 0;
    int32_t motionHeight = 0;
    uint32_t resourceTag = 0;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float motionScaleX = 0.0f;
    float motionScaleY = 0.0f;
    float deltaMilliseconds = 0.0f;
    float cameraNear = 0.1f;
    float cameraFar = 1000.0f;
    float verticalFovRadians = 1.0f;
    bool reversedDepth = false;
    bool hdrColor = false;
    bool reset = false;
    bool bilinearComparison = false;
};

class IUpscalerBackend
{
public:
    virtual ~IUpscalerBackend() = default;

    virtual UpscalerKind Kind() const = 0;

    virtual void SetEngineVersion(const std::string& engineVersion) { (void)engineVersion; }
    virtual void SetRuntimeDirectory(const std::wstring& runtimeDirectory) { (void)runtimeDirectory; }
    virtual void SetDataDirectory(const std::wstring& dataDirectory) { (void)dataDirectory; }

    // Called on the render thread with a live device. Idempotent per device.
    virtual bool Attach(ID3D11Device* device, ID3D11DeviceContext* context) = 0;

    // True when the SDK and this GPU/driver can actually reconstruct. Must not create contexts.
    virtual bool Available() = 0;

    virtual uint32_t RuntimeVersion() const = 0;

    // 64-bit implementation identity for APIs that report one; zero otherwise.
    virtual uint64_t ProviderVersionId() const { return 0; }

    // Short human-readable reason for the current Unavailable/Faulted state.
    const std::string& LastReason() const { return lastReason_; }

    // Queries every preset's supported input range for one output size.
    virtual bool QueryModes(uint32_t outputWidth, uint32_t outputHeight, ModeRange (&ranges)[kModeCount],
                            uint32_t& runtimeVersion) = 0;

    // Evaluates one logical view. Zero means the evaluation succeeded; every nonzero return value
    // is a failure and is preserved verbatim, so a positive FidelityFX error code is never mistaken
    // for success.
    virtual int32_t Evaluate(const ViewKey& key, const EvaluationRequest& request, bool& featureCreated) = 0;

    // Drops one view's context. Only called when its GPU work has completed.
    virtual void Forget(const ViewKey& key) = 0;

    // Releases contexts retired by a mode/size change and no longer in flight.
    virtual void CollectRetired(ID3D11DeviceContext* context) { (void)context; }

    // Releases every feature and shuts the SDK down. The caller guarantees GPU idle.
    virtual void Release() = 0;

protected:
    std::string lastReason_;
};
}
