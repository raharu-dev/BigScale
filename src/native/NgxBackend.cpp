#include "NgxBackend.h"
#include "Log.h"
#include "NgxSynchronization.h"

#include <windows.h>
#include <d3d11_1.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_d3d.h>

#include <cstring>
#include <vector>

namespace bigscale
{
namespace
{
// Original project id for this integration. Not a game or vendor AppID.
constexpr const char* kProjectId = "dce2ec10-c62e-4ec2-a24e-7368ff734a44";

uint32_t ReadModuleFileVersion(const wchar_t* moduleName)
{
    const HMODULE module = GetModuleHandleW(moduleName);
    if (module == nullptr) return 0;
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(module, path, MAX_PATH) == 0) return 0;
    DWORD ignored = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(path, &ignored);
    if (bytes == 0) return 0;
    std::vector<uint8_t> storage(bytes);
    if (!GetFileVersionInfoW(path, 0, bytes, storage.data())) return 0;
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(storage.data(), L"\\", reinterpret_cast<void**>(&info), &length) || info == nullptr) return 0;
    const uint32_t major = HIWORD(info->dwFileVersionMS);
    const uint32_t minor = LOWORD(info->dwFileVersionMS);
    const uint32_t build = HIWORD(info->dwFileVersionLS);
    return (major << 24) | (minor << 16) | (build & 0xFFFFu);
}
}

struct NgxBackend::Feature
{
    ViewKey key{};
    NVSDK_NGX_Handle* handle = nullptr;
    uint32_t createWidth = 0;
    uint32_t createHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    int32_t quality = -1;
    uint32_t flags = 0;
    int32_t lastFrame = -1;
    int32_t lastEpoch = -1;
    uint32_t evaluations = 0;
    uint32_t failures = 0;
    uint32_t resets = 0;
    Feature* next = nullptr;
};

// A feature retired by a mode/size change. Its GPU work may still be in flight, so it is only
// released after a D3D11 event query proves the work has completed.
struct NgxBackend::Retired
{
    NVSDK_NGX_Handle* handle = nullptr;
    ID3D11Query* query = nullptr;
    uint32_t createWidth = 0;
    uint32_t createHeight = 0;
    Retired* next = nullptr;
};

NgxBackend::NgxBackend(bool presetL) : presetL_(presetL) {}

void NgxBackend::SetEngineVersion(const std::string& engineVersion)
{
    if (!engineVersion.empty()) engineVersion_ = engineVersion;
}

void NgxBackend::SetRuntimeDirectory(const std::wstring& runtimeDirectory) { runtimeDirectory_ = runtimeDirectory; }

void NgxBackend::SetDataDirectory(const std::wstring& dataDirectory) { dataDirectory_ = dataDirectory; }

bool NgxBackend::EnsureInitialized(ID3D11Device* device)
{
    std::lock_guard<std::recursive_mutex> guard(NgxMutex());
    if (initialized_) return device_ == device;
    device_ = device;

    const wchar_t* paths[1] = {runtimeDirectory_.empty() ? nullptr : runtimeDirectory_.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    if (paths[0] != nullptr)
    {
        info.PathListInfo.Path = paths;
        info.PathListInfo.Length = 1;
    }

    const NVSDK_NGX_Result result = NVSDK_NGX_D3D11_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_UNITY,
        engineVersion_.c_str(), dataDirectory_.c_str(), device, &info, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(result))
    {
        Log("NGX D3D11 initialisation failed result=0x%08x", static_cast<unsigned>(result));
        return false;
    }
    initialized_ = true;
    runtimeVersion_ = ReadModuleFileVersion(L"nvngx_dlss.dll");
    Log("NGX D3D11 initialised project=%s engine=UNITY version=%s runtimeVersion=0x%08x", kProjectId,
        engineVersion_.c_str(), runtimeVersion_);
    return true;
}

bool NgxBackend::Attach(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (device == nullptr || context == nullptr) return false;
    if (!EnsureInitialized(device))
    {
        ready_ = false;
        return false;
    }
    context_ = context;
    ready_ = true;
    return true;
}

bool NgxBackend::Available()
{
    std::lock_guard<std::recursive_mutex> guard(NgxMutex());
    if (!initialized_) return false;
    if (parameters_ == nullptr)
    {
        NVSDK_NGX_Result result = NVSDK_NGX_D3D11_GetCapabilityParameters(
            reinterpret_cast<NVSDK_NGX_Parameter**>(&parameters_));
        if (NVSDK_NGX_FAILED(result) || parameters_ == nullptr)
        {
            parameters_ = nullptr;
            Log("NGX capability parameters failed result=0x%08x", static_cast<unsigned>(result));
            return false;
        }
    }
    int available = 0;
    reinterpret_cast<NVSDK_NGX_Parameter*>(parameters_)->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (!available) Log("NGX reports DLSS Super Resolution unavailable on this device or driver.");
    return available != 0;
}

// The capability parameter that carries one mode's render preset. Every mode has its own hint key,
// so the create-time selection is per quality mode.
const char* RenderPresetHint(Preset preset)
{
    switch (preset)
    {
        case Preset::Quality: return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
        case Preset::Balanced: return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced;
        case Preset::Performance: return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance;
        case Preset::UltraPerformance: return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance;
        case Preset::NativeAA: return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA;
    }
    return nullptr;
}

NVSDK_NGX_PerfQuality_Value NgxQualityValue(Preset preset)
{
    switch (preset)
    {
        case Preset::Quality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        case Preset::Balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
        case Preset::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case Preset::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        case Preset::NativeAA: return NVSDK_NGX_PerfQuality_Value_DLAA;
    }
    return NVSDK_NGX_PerfQuality_Value_MaxQuality;
}

bool NgxBackend::QueryModes(uint32_t outputWidth, uint32_t outputHeight, ModeRange (&ranges)[kModeCount],
    uint32_t& runtimeVersion)
{
    runtimeVersion = runtimeVersion_;
    for (int i = 0; i < kModeCount; i++)
    {
        ranges[i] = ModeRange{};
        ranges[i].optimalWidth = outputWidth;
        ranges[i].optimalHeight = outputHeight;
        ranges[i].minWidth = outputWidth;
        ranges[i].minHeight = outputHeight;
        ranges[i].maxWidth = outputWidth;
        ranges[i].maxHeight = outputHeight;
    }
    if (!Available() || outputWidth == 0 || outputHeight == 0) return false;

    auto* parameters = reinterpret_cast<NVSDK_NGX_Parameter*>(parameters_);
    // Neutral slot order: Quality, Balanced, Performance, UltraPerformance, NativeAA.
    static const NVSDK_NGX_PerfQuality_Value kOrder[kModeCount] = {
        NVSDK_NGX_PerfQuality_Value_MaxQuality,
        NVSDK_NGX_PerfQuality_Value_Balanced,
        NVSDK_NGX_PerfQuality_Value_MaxPerf,
        NVSDK_NGX_PerfQuality_Value_UltraPerformance,
        NVSDK_NGX_PerfQuality_Value_DLAA,
    };

    bool anySupported = false;
    for (int i = 0; i < kModeCount; i++)
    {
        unsigned optimalWidth = 0, optimalHeight = 0, maxWidth = 0, maxHeight = 0, minWidth = 0, minHeight = 0;
        float sharpness = 0.0f;
        const NVSDK_NGX_Result result = NGX_DLSS_GET_OPTIMAL_SETTINGS(parameters, outputWidth, outputHeight,
            kOrder[i], &optimalWidth, &optimalHeight, &maxWidth, &maxHeight, &minWidth, &minHeight, &sharpness);
        if (NVSDK_NGX_FAILED(result))
        {
            Log("NGX optimal settings mode=%d output=%ux%u failed result=0x%08x", i, outputWidth, outputHeight,
                static_cast<unsigned>(result));
            continue;
        }
        if (optimalWidth == 0 || optimalHeight == 0 || maxWidth == 0 || maxHeight == 0) continue;
        if (minWidth == 0) minWidth = optimalWidth;
        if (minHeight == 0) minHeight = optimalHeight;
        ranges[i].supported = true;
        ranges[i].optimalWidth = optimalWidth;
        ranges[i].optimalHeight = optimalHeight;
        ranges[i].minWidth = minWidth;
        ranges[i].minHeight = minHeight;
        ranges[i].maxWidth = maxWidth;
        ranges[i].maxHeight = maxHeight;
        anySupported = true;
    }
    Log("NGX mode ranges for output=%ux%u: %s", outputWidth, outputHeight, anySupported ? "reported" : "empty");
    return anySupported;
}

NgxBackend::Feature* NgxBackend::Find(const ViewKey& key)
{
    for (Feature* feature = features_; feature != nullptr; feature = feature->next)
        if (feature->key == key) return feature;
    return nullptr;
}

NgxBackend::Feature* NgxBackend::Create(const ViewKey& key)
{
    std::lock_guard<std::recursive_mutex> guard(NgxMutex());
    auto* feature = new Feature();
    feature->key = key;
    feature->next = features_;
    features_ = feature;
    return feature;
}

void NgxBackend::DestroyFeature(Feature& feature)
{
    std::lock_guard<std::recursive_mutex> guard(NgxMutex());
    if (feature.handle != nullptr)
    {
        NVSDK_NGX_D3D11_ReleaseFeature(feature.handle);
        feature.handle = nullptr;
    }
}

void NgxBackend::Retire(Feature& feature)
{
    std::lock_guard<std::recursive_mutex> guard(NgxMutex());
    if (feature.handle == nullptr) return;
    auto* retired = new Retired();
    retired->handle = feature.handle;
    retired->createWidth = feature.createWidth;
    retired->createHeight = feature.createHeight;
    feature.handle = nullptr;

    if (device_ != nullptr && context_ != nullptr)
    {
        D3D11_QUERY_DESC description{D3D11_QUERY_EVENT, 0};
        if (SUCCEEDED(device_->CreateQuery(&description, &retired->query)) && retired->query != nullptr)
        {
            context_->End(retired->query);
        }
    }
    if (retired->query == nullptr)
    {
        // No query available: keep the handle rather than risk releasing GPU state that is still in
        // flight. It is released by the full teardown, which guarantees GPU idle first.
        Log("NGX retired feature without a completion query; keeping it until teardown.");
    }
    retired->next = retired_;
    retired_ = retired;
}

void NgxBackend::CollectRetired(ID3D11DeviceContext* context)
{
    std::lock_guard<std::recursive_mutex> guard(NgxMutex());
    if (context == nullptr)
    {
        return;
    }
    Retired** link = &retired_;
    while (*link != nullptr)
    {
        Retired* entry = *link;
        // A retired feature without a query can never be proven complete, so it is retained until
        // the full teardown, which requires a proven GPU drain.
        if (entry->query == nullptr)
        {
            link = &entry->next;
            continue;
        }
        BOOL complete = FALSE;
        const HRESULT result = context->GetData(entry->query, &complete, sizeof(complete), 0);
        if (FAILED(result))
        {
            // A broken query never proves completion; keep the handle until full teardown.
            link = &entry->next;
            continue;
        }
        if (result != S_OK || !complete)
        {
            link = &entry->next;
            continue;
        }
        *link = entry->next;
        if (entry->handle != nullptr) NVSDK_NGX_D3D11_ReleaseFeature(entry->handle);
        if (entry->query != nullptr) entry->query->Release();
        Log("NGX released a retired feature create=%ux%u", entry->createWidth, entry->createHeight);
        delete entry;
    }
}

void NgxBackend::Forget(const ViewKey& key)
{
    std::lock_guard<std::recursive_mutex> guard(NgxMutex());
    Feature** link = &features_;
    while (*link != nullptr)
    {
        Feature* feature = *link;
        if (!(feature->key == key))
        {
            link = &feature->next;
            continue;
        }
        *link = feature->next;
        Retire(*feature);
        delete feature;
        return;
    }
}

void NgxBackend::Release()
{
    std::lock_guard<std::recursive_mutex> guard(NgxMutex());
    CollectRetired(context_);
    for (Feature* feature = features_; feature != nullptr;)
    {
        Feature* next = feature->next;
        DestroyFeature(*feature);
        delete feature;
        feature = next;
    }
    features_ = nullptr;

    for (Retired* entry = retired_; entry != nullptr;)
    {
        Retired* next = entry->next;
        if (entry->handle != nullptr) NVSDK_NGX_D3D11_ReleaseFeature(entry->handle);
        if (entry->query != nullptr) entry->query->Release();
        delete entry;
        entry = next;
    }
    retired_ = nullptr;

    // Order matters: features are gone, then the capability parameter object, then the device.
    if (parameters_ != nullptr)
    {
        NVSDK_NGX_D3D11_DestroyParameters(reinterpret_cast<NVSDK_NGX_Parameter*>(parameters_));
        parameters_ = nullptr;
    }
    if (initialized_ && device_ != nullptr)
    {
        NVSDK_NGX_D3D11_Shutdown1(device_);
        initialized_ = false;
    }
    ready_ = false;
    context_ = nullptr;
    device_ = nullptr;
    Log("NGX released for device.");
}

int32_t NgxBackend::Evaluate(const ViewKey& key, const EvaluationRequest& request, bool& featureCreated)
{
    std::lock_guard<std::recursive_mutex> guard(NgxMutex());
    featureCreated = false;
    if (!ready_ || context_ == nullptr) return static_cast<int32_t>(NVSDK_NGX_Result_FAIL_NotInitialized);
    if (!Available()) return static_cast<int32_t>(NVSDK_NGX_Result_FAIL_FeatureNotSupported);

    CollectRetired(context_);

    auto* parameters = reinterpret_cast<NVSDK_NGX_Parameter*>(parameters_);
    const int32_t quality = static_cast<int32_t>(NgxQualityValue(request.preset));
    uint32_t flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    if (request.reversedDepth) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (request.hdrColor) flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;

    // Create the feature at exactly the extent that will be evaluated. NGX maps the render subrect
    // against the creation extent; creating at a larger capability maximum while evaluating a
    // smaller active viewport leaves the rest of the output unwritten.
    const uint32_t createWidth = request.inputWidth;
    const uint32_t createHeight = request.inputHeight;

    Feature* feature = Find(key);
    if (feature == nullptr) feature = Create(key);

    const bool descriptorsMatch = feature->handle != nullptr &&
        feature->createWidth == createWidth && feature->createHeight == createHeight &&
        feature->outputWidth == request.outputWidth && feature->outputHeight == request.outputHeight &&
        feature->quality == quality && feature->flags == flags;

    if (!descriptorsMatch)
    {
        Retire(*feature);
        // Preset L is a create-time hint on the capability parameter set. It is written on every create so
        // a parameter object can never carry a previous selection into a new feature.
        if (parameters != nullptr)
        {
            if (const char* hint = RenderPresetHint(request.preset))
                NVSDK_NGX_Parameter_SetI(reinterpret_cast<NVSDK_NGX_Parameter*>(parameters), hint,
                    presetL_ ? NVSDK_NGX_DLSS_Hint_Render_Preset_L : NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
        }
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth = createWidth;
        create.Feature.InHeight = createHeight;
        create.Feature.InTargetWidth = request.outputWidth;
        create.Feature.InTargetHeight = request.outputHeight;
        create.Feature.InPerfQualityValue = static_cast<NVSDK_NGX_PerfQuality_Value>(quality);
        create.InFeatureCreateFlags = static_cast<int>(flags);
        create.InEnableOutputSubrects = false;
        const NVSDK_NGX_Result result = NGX_D3D11_CREATE_DLSS_EXT(context_, &feature->handle, parameters, &create);
        Log("NGX create camera=%d eye=%d quality=%d render=%ux%u create=%ux%u output=%ux%u flags=0x%x presetL=%d result=0x%08x",
            key.cameraId, key.eye, quality, request.inputWidth, request.inputHeight, createWidth,
            createHeight, request.outputWidth, request.outputHeight, flags, presetL_ ? 1 : 0,
            static_cast<unsigned>(result));
        if (NVSDK_NGX_FAILED(result) || feature->handle == nullptr)
        {
            feature->handle = nullptr;
            feature->failures++;
            return static_cast<int32_t>(result);
        }
        feature->createWidth = createWidth;
        feature->createHeight = createHeight;
        feature->outputWidth = request.outputWidth;
        feature->outputHeight = request.outputHeight;
        feature->quality = quality;
        feature->flags = flags;
        feature->lastFrame = -1;
        feature->lastEpoch = -1;
        featureCreated = true;
    }

    // History invalidation: first use, an explicit managed request, an epoch change, or a gap in
    // the render sequence for this view. Ordinary head motion never resets.
    const bool reset = request.reset || feature->lastFrame < 0 || feature->lastEpoch != request.historyEpoch ||
        request.frameIndex != feature->lastFrame + 1;

    NVSDK_NGX_D3D11_DLSS_Eval_Params evaluate{};
    evaluate.Feature.pInColor = request.color;
    evaluate.Feature.pInOutput = request.output;
    evaluate.pInDepth = request.depth;
    evaluate.pInMotionVectors = request.motion;
    // NGX requires the jitter in render pixels in the same coordinate and direction system as the
    // motion vectors. TemporalInputPolicy.Jitter already produced that value for every provider, so
    // this backend applies no conversion of its own.
    evaluate.InJitterOffsetX = request.jitterX;
    evaluate.InJitterOffsetY = request.jitterY;
    evaluate.InMVScaleX = request.motionScaleX;
    evaluate.InMVScaleY = request.motionScaleY;
    evaluate.InRenderSubrectDimensions = {request.inputWidth, request.inputHeight};
    evaluate.InColorSubrectBase = {static_cast<unsigned>(request.inputX), static_cast<unsigned>(request.inputY)};
    evaluate.InDepthSubrectBase = {static_cast<unsigned>(request.inputX), static_cast<unsigned>(request.inputY)};
    evaluate.InMVSubrectBase = {static_cast<unsigned>(request.inputX), static_cast<unsigned>(request.inputY)};
    evaluate.InOutputSubrectBase = {static_cast<unsigned>(request.outputX), static_cast<unsigned>(request.outputY)};
    evaluate.InReset = reset ? 1 : 0;
    evaluate.InPreExposure = 1.0f;
    evaluate.InExposureScale = 1.0f;
    evaluate.InFrameTimeDeltaInMsec = request.deltaMilliseconds;

    const NVSDK_NGX_Result result = NGX_D3D11_EVALUATE_DLSS_EXT(context_, feature->handle, parameters, &evaluate);
    if (NVSDK_NGX_FAILED(result))
    {
        feature->failures++;
        Log("NGX evaluate camera=%d eye=%d result=0x%08x reset=%d render=%ux%u subrect=(%d,%d) output=(%d,%d)",
            key.cameraId, key.eye, static_cast<unsigned>(result), evaluate.InReset, request.inputWidth,
            request.inputHeight, request.inputX, request.inputY, request.outputX, request.outputY);
        return static_cast<int32_t>(result);
    }
    feature->evaluations++;
    feature->lastFrame = request.frameIndex;
    feature->lastEpoch = request.historyEpoch;
    if (reset) feature->resets++;
    if (feature->evaluations == 1 || feature->evaluations % 600 == 0)
        Log("NGX evaluate camera=%d eye=%d ok evaluations=%u reset=%d render=%ux%u create=%ux%u "
            "jitter=(%.5f,%.5f) mvScale=(%.2f,%.2f)",
            key.cameraId, key.eye, feature->evaluations, evaluate.InReset, request.inputWidth, request.inputHeight,
            feature->createWidth, feature->createHeight, request.jitterX, request.jitterY, request.motionScaleX,
            request.motionScaleY);
    // The backend contract is zero success. NGX reports success as 0x1, which the bridge must never
    // confuse with a failure code.
    return 0;
}
}
