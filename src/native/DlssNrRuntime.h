// DLSS5 Neural Rendering: the NGX feature-18 runtime.
//
// The user supplies the 40-series fp16 neural-rendering compatibility runtime as
// `nvngx_dlssnr.dll` under the plugin directory. It is never redistributed. Because that build
// resolves its own module by the name `nvngx.dll`, the stage installs a hard link (or, across
// volumes, a copy) with that name inside the application data directory and loads it with
// LOAD_WITH_ALTERED_SEARCH_PATH. No import table of the game process is patched.
//
// One object owns the alias module, one driver-core capability map, and every parameter map it ever
// handed out. Features are created by either the compat module (when it initialised) or the driver
// NGX core (which frame generation may already own); each feature records its owner so it is
// released through the same module that created it.
#pragma once

#include <windows.h>

#include <d3d12.h>

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace bigscale
{
// Export signatures of the compatibility runtime. They match the pinned SDK declarations, except
// that the SDK version is an unsigned int and the parameter map is passed non-const, which is the
// form the 310.9 compatibility build exposes.
using DlssNrInitExt = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*,
                                                    unsigned int, NVSDK_NGX_Parameter*);
using DlssNrCreateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                          NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using DlssNrEvaluateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                            NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using DlssNrReleaseFeature = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using DlssNrShutdown = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);

// Tuning fixed when the feature is created. Every sequential pass owns one of these.
struct NeuralTuning
{
    int32_t style = 0;
    float strength = 1.0f;
    float detail = 1.0f;
    float tone = 1.0f;
};

// Per-frame resources handed to one created feature.
struct NeuralEvaluation
{
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* output = nullptr;
    uint32_t width = 0;         // colour/output extent
    uint32_t height = 0;
    uint32_t guideWidth = 0;    // depth/motion extent, independent of the colour extent
    uint32_t guideHeight = 0;
    bool depthInverted = false;
    bool reset = false;
    float motionScaleX = 0.0f;
    float motionScaleY = 0.0f;
};

// One created NGX feature and the parameter map it was created from.
struct NeuralFeature
{
    enum Owner : int32_t
    {
        None = 0,
        Driver = 1,  // the driver NGX D3D12 core, also used by frame generation
        Direct = 2,  // the user-supplied compatibility runtime
    };

    NVSDK_NGX_Handle* handle = nullptr;
    NVSDK_NGX_Parameter* parameters = nullptr;
    int32_t owner = None;
    NeuralTuning tuning{};
    uint32_t width = 0;
    uint32_t height = 0;
};

class DlssNrRuntime
{
public:
    // NGX feature id of DLSS5 Neural Rendering and the application id the 310.9 runtime expects.
    static constexpr int32_t kFeatureId = 18;
    static constexpr unsigned long long kApplicationId = 0x24480451ull;

    DlssNrRuntime() = default;
    ~DlssNrRuntime();
    DlssNrRuntime(const DlssNrRuntime&) = delete;
    DlssNrRuntime& operator=(const DlssNrRuntime&) = delete;

    void SetDirectories(const std::wstring& runtimeDirectory, const std::wstring& dataDirectory);
    // Re-resolves the supplied DLL with a file attribute query. Never loads a module and never
    // touches the GPU, so the configuration export may call it from any thread.
    bool RuntimePresent();
    bool Ready() const { return ready_.load(); }

    // Loads and initialises the runtime on this device. Idempotent per device; a missing file,
    // a failed driver-core initialisation and an exhausted fallback are the only failures.
    bool Ensure(ID3D12Device* device, const std::wstring& runtimeDirectory, const std::wstring& dataDirectory,
                const std::string& engineVersion, std::string& reason);

    bool CreateFeature(ID3D12GraphicsCommandList* list, uint32_t width, uint32_t height,
                       const NeuralTuning& tuning, NeuralFeature& feature, std::string& reason);
    bool EvaluateFeature(ID3D12GraphicsCommandList* list, NeuralFeature& feature,
                         const NeuralEvaluation& evaluation, std::string& reason);
    void ReleaseFeature(NeuralFeature& feature);
    // Releases features first is the caller's duty; this drops the alias module, shuts down only the
    // cores this object started, and destroys every parameter map exactly once.
    void Release();

private:
    bool ResolveSourceLocked();
    bool PrepareDriverCoreLocked(ID3D12Device* device, const std::string& engineVersion,
                                 const std::wstring& appDataDirectory, std::string& reason);
    bool PrepareDirectLocked(ID3D12Device* device, std::string& reason);
    bool AcquireParametersLocked(NVSDK_NGX_Parameter*& parameters, std::string& reason);
    void RetainMapLocked(NVSDK_NGX_Parameter* parameters);
    void ReleaseMapLocked(NVSDK_NGX_Parameter* parameters);
    bool CreateWithDriverLocked(ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter* parameters,
                                NVSDK_NGX_Handle** handle, NVSDK_NGX_Result& result);
    void ApplyCreationLocked(NVSDK_NGX_Parameter* parameters, const NeuralFeature& feature);
    void UnloadDirectLocked();
    void NoteResultLocked(const char* stage, NVSDK_NGX_Result result, bool success);

    std::mutex mutex_;
    std::wstring runtimeDirectory_;
    std::wstring dataDirectory_;
    std::wstring sourceDll_;
    std::wstring aliasPath_;
    std::wstring realPath_;
    std::wstring appDataDirectory_;
    std::string engineVersion_;

    ID3D12Device* device_ = nullptr;
    NVSDK_NGX_Parameter* capabilities_ = nullptr;
    struct OwnedParameterMap
    {
        NVSDK_NGX_Parameter* map = nullptr;
        uint32_t references = 0;
    };
    std::vector<OwnedParameterMap> parameterMaps_;

    HMODULE module_ = nullptr;
    DlssNrInitExt initExt_ = nullptr;
    DlssNrCreateFeature createFeature_ = nullptr;
    DlssNrEvaluateFeature evaluateFeature_ = nullptr;
    DlssNrReleaseFeature releaseFeature_ = nullptr;
    DlssNrShutdown shutdown1_ = nullptr;

    std::atomic<bool> ready_{false};
    bool directInitialized_ = false;
    bool driverInitialized_ = false;
    uint32_t directFeatures_ = 0;
    // Last result code reported per stage, so a failing frame logs once per transition.
    NVSDK_NGX_Result lastCreateResult_ = NVSDK_NGX_Result_Success;
    NVSDK_NGX_Result lastEvaluateResult_ = NVSDK_NGX_Result_Success;
};
}
