#include "DlssNrRuntime.h"

#include "Log.h"

#include <nvsdk_ngx.h>

#include <cstring>
#include <cwchar>
#include <exception>

namespace bigscale
{
namespace
{
// Project identity shared with the DLSS super-resolution backend and frame generation.
constexpr const char* kProjectId = "dce2ec10-c62e-4ec2-a24e-7368ff734a44";

// Creation-time parameter names. Every literal is set on the feature's own map before creation and
// again before every evaluation, because NGX may hand back one shared map for several features.
constexpr const char* kEnabled = "DLSSNR.Enabled";
constexpr const char* kWidth = "DLSSNR.Width";
constexpr const char* kHeight = "DLSSNR.Height";
constexpr const char* kCreationNodeMask = "CreationNodeMask";
constexpr const char* kVisibilityNodeMask = "VisibilityNodeMask";
constexpr const char* kHintRenderPreset = "DLSSNR.Hint.Render.Preset";
constexpr const char* kIntensity = "DLSSNR.Intensity";
constexpr const char* kStyle = "DLSSNR.Style";
constexpr const char* kLocalStructureStrength = "DLSSNR.LocalStructureStrength";
constexpr const char* kLocalToneStrength = "DLSSNR.LocalToneStrength";
constexpr const char* kSkinStructureStrength = "DLSSNR.SkinStructureStrength";
constexpr const char* kUseAutoMask = "DLSSNR.UseAutoMask";
constexpr const char* kUiCorrection = "DLSSNR.UICorrection";
constexpr const char* kControlMask = "DLSSNR.ControlMask";
constexpr const char* kUi = "DLSSNR.UI";
constexpr const char* kUiAlpha = "DLSSNR.UIAlpha";
constexpr const char* kBackbuffer = "DLSSNR.Backbuffer";

// Evaluation-time parameter names.
constexpr const char* kColor = "DLSSNR.Color";
constexpr const char* kDepth = "DLSSNR.Depth";
constexpr const char* kMotion = "DLSSNR.MVec";
constexpr const char* kOutput = "DLSSNR.Output";
constexpr const char* kDepthInverted = "DLSSNR.DepthInverted";
constexpr const char* kReset = "DLSSNR.Reset";
constexpr const char* kMotionScaleX = "DLSSNR.MVecScaleX";
constexpr const char* kMotionScaleY = "DLSSNR.MVecScaleY";

// Subrect parameter names. The neural feature names each region directly instead of reusing the
// DLSS subrect keys, so every base and extent is published explicitly.
constexpr const char* kColorSubrectBaseX = "DLSSNR.ColorSubrectBaseX";
constexpr const char* kColorSubrectBaseY = "DLSSNR.ColorSubrectBaseY";
constexpr const char* kColorSubrectWidth = "DLSSNR.ColorSubrectWidth";
constexpr const char* kColorSubrectHeight = "DLSSNR.ColorSubrectHeight";
constexpr const char* kOutputSubrectBaseX = "DLSSNR.OutputSubrectBaseX";
constexpr const char* kOutputSubrectBaseY = "DLSSNR.OutputSubrectBaseY";
constexpr const char* kOutputSubrectWidth = "DLSSNR.OutputSubrectWidth";
constexpr const char* kOutputSubrectHeight = "DLSSNR.OutputSubrectHeight";
constexpr const char* kDepthSubrectBaseX = "DLSSNR.DepthSubrectBaseX";
constexpr const char* kDepthSubrectBaseY = "DLSSNR.DepthSubrectBaseY";
constexpr const char* kDepthSubrectWidth = "DLSSNR.DepthSubrectWidth";
constexpr const char* kDepthSubrectHeight = "DLSSNR.DepthSubrectHeight";
constexpr const char* kMotionSubrectBaseX = "DLSSNR.MVecSubrectBaseX";
constexpr const char* kMotionSubrectBaseY = "DLSSNR.MVecSubrectBaseY";
constexpr const char* kMotionSubrectWidth = "DLSSNR.MVecSubrectWidth";
constexpr const char* kMotionSubrectHeight = "DLSSNR.MVecSubrectHeight";

// UI, UI-alpha and back-buffer regions are unused: neural rendering never receives any of those
// resources. Their regions are published as zero so the runtime cannot inherit a stale region.
constexpr const char* kUiSubrectBaseX = "DLSSNR.UISubrectBaseX";
constexpr const char* kUiSubrectBaseY = "DLSSNR.UISubrectBaseY";
constexpr const char* kUiSubrectWidth = "DLSSNR.UISubrectWidth";
constexpr const char* kUiSubrectHeight = "DLSSNR.UISubrectHeight";
constexpr const char* kUiAlphaSubrectBaseX = "DLSSNR.UIAlphaSubrectBaseX";
constexpr const char* kUiAlphaSubrectBaseY = "DLSSNR.UIAlphaSubrectBaseY";
constexpr const char* kUiAlphaSubrectWidth = "DLSSNR.UIAlphaSubrectWidth";
constexpr const char* kUiAlphaSubrectHeight = "DLSSNR.UIAlphaSubrectHeight";
constexpr const char* kBackbufferSubrectBaseX = "DLSSNR.BackbufferSubrectBaseX";
constexpr const char* kBackbufferSubrectBaseY = "DLSSNR.BackbufferSubrectBaseY";
constexpr const char* kBackbufferSubrectWidth = "DLSSNR.BackbufferSubrectWidth";
constexpr const char* kBackbufferSubrectHeight = "DLSSNR.BackbufferSubrectHeight";

// Address inside this bridge module. GetModuleHandleExW on it yields the real BigScale.Native.dll,
// which is hard-linked to nvngx.dll so the compatibility runtime's caller-path gate passes.
void ModuleAnchor() {}

std::wstring TrimSeparators(const std::wstring& directory)
{
    std::wstring trimmed = directory;
    while (!trimmed.empty() && (trimmed.back() == L'\\' || trimmed.back() == L'/')) trimmed.pop_back();
    return trimmed;
}

std::wstring Join(const std::wstring& directory, const wchar_t* leaf)
{
    if (directory.empty()) return leaf;
    std::wstring path = directory;
    if (path.back() != L'\\' && path.back() != L'/') path += L'\\';
    path += leaf;
    return path;
}

bool FileExists(const std::wstring& path)
{
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool EnsureDirectory(const std::wstring& path)
{
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return true;
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}
}

DlssNrRuntime::~DlssNrRuntime() { Release(); }

void DlssNrRuntime::SetDirectories(const std::wstring& runtimeDirectory, const std::wstring& dataDirectory)
{
    std::lock_guard<std::mutex> guard(mutex_);
    runtimeDirectory_ = TrimSeparators(runtimeDirectory);
    dataDirectory_ = TrimSeparators(dataDirectory);
    sourceDll_.clear();
    aliasPath_.clear();
    appDataDirectory_.clear();
}

bool DlssNrRuntime::ResolveSourceLocked()
{
    if (!sourceDll_.empty() && FileExists(sourceDll_)) return true;
    sourceDll_.clear();
    if (runtimeDirectory_.empty()) return false;
    // The provider tree is searched first so a staged update next to the plugin wins over a loose
    // copy beside the bridge.
    const wchar_t* candidates[2] = {L"providers\\dlss5\\nvngx_dlssnr.dll", L"nvngx_dlssnr.dll"};
    for (const wchar_t* candidate : candidates)
    {
        const std::wstring path = Join(runtimeDirectory_, candidate);
        if (FileExists(path))
        {
            sourceDll_ = path;
            return true;
        }
    }
    return false;
}

bool DlssNrRuntime::RuntimePresent()
{
    std::lock_guard<std::mutex> guard(mutex_);
    return ResolveSourceLocked();
}

bool DlssNrRuntime::Ensure(ID3D12Device* device, const std::wstring& runtimeDirectory,
    const std::wstring& dataDirectory, const std::string& engineVersion, std::string& reason)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (ready_.load()) return device_ == device;
    if (device == nullptr)
    {
        reason = "no D3D12 device is available for neural rendering";
        return false;
    }
    device_ = device;
    runtimeDirectory_ = TrimSeparators(runtimeDirectory);
    dataDirectory_ = TrimSeparators(dataDirectory);
    if (!engineVersion.empty()) engineVersion_ = engineVersion;
    if (!ResolveSourceLocked())
    {
        reason = "nvngx_dlssnr.dll is not staged in the plugin directory";
        return false;
    }

    appDataDirectory_ = Join(dataDirectory_, L"dlss5");
    if (!EnsureDirectory(appDataDirectory_))
        Log("neural: the application data directory could not be created path=%ls error=%lu",
            appDataDirectory_.c_str(), static_cast<unsigned long>(GetLastError()));
    aliasPath_ = Join(appDataDirectory_, L"nvngx.dll");

    try
    {
        if (!PrepareDriverCoreLocked(device, engineVersion_, appDataDirectory_, reason)) return false;
        std::string directReason;
        if (!PrepareDirectLocked(device, directReason))
            Log("neural: using the driver NGX core: %s", directReason.c_str());
    }
    catch (const std::exception& error)
    {
        reason = std::string("neural runtime initialisation threw: ") + error.what();
        Log("neural: %s", reason.c_str());
        return false;
    }

    ready_ = true;
    return true;
}

bool DlssNrRuntime::PrepareDriverCoreLocked(ID3D12Device* device, const std::string& engineVersion,
    const std::wstring& appDataDirectory, std::string& reason)
{
    NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_Result probe = NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters);
    const bool preExisting = !NVSDK_NGX_FAILED(probe) && parameters != nullptr;
    if (!preExisting)
    {
        parameters = nullptr;
        // Descending search order: the DLSS5 provider tree, then the plugin root.
        std::vector<std::wstring> storage;
        if (!runtimeDirectory_.empty()) storage.push_back(Join(runtimeDirectory_, L"providers\\dlss5"));
        if (!runtimeDirectory_.empty()) storage.push_back(runtimeDirectory_);
        std::vector<const wchar_t*> pointers;
        pointers.reserve(storage.size());
        for (const std::wstring& path : storage) pointers.push_back(path.c_str());
        NVSDK_NGX_FeatureCommonInfo info{};
        if (!pointers.empty())
        {
            info.PathListInfo.Path = pointers.data();
            info.PathListInfo.Length = static_cast<unsigned int>(pointers.size());
        }

        const NVSDK_NGX_Result initialised = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId,
            NVSDK_NGX_ENGINE_TYPE_UNITY, engineVersion.c_str(), appDataDirectory.c_str(), device, &info,
            NVSDK_NGX_Version_API);
        if (NVSDK_NGX_FAILED(initialised))
        {
            reason = "the driver NGX D3D12 core could not be initialised";
            Log("neural: driver NGX initialisation failed result=0x%08x (capability probe 0x%08x)",
                static_cast<unsigned>(initialised), static_cast<unsigned>(probe));
            return false;
        }
        // Only a core this object started is shut down again; frame generation may own a
        // pre-existing core for the lifetime of the process.
        driverInitialized_ = true;
        Log("neural: driver NGX D3D12 core initialised project=%s engine=UNITY version=%s", kProjectId,
            engineVersion.c_str());
        probe = NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters);
        if (NVSDK_NGX_FAILED(probe) || parameters == nullptr)
        {
            reason = "NGX did not provide its D3D12 capability parameter map";
            Log("neural: capability parameters failed result=0x%08x", static_cast<unsigned>(probe));
            return false;
        }
    }
    else
    {
        Log("neural: the driver NGX D3D12 core is already initialised; sharing its capability parameters");
    }

    capabilities_ = parameters;
    RetainMapLocked(parameters);
    return true;
}

bool DlssNrRuntime::PrepareDirectLocked(ID3D12Device* device, std::string& reason)
{
    if (aliasPath_.empty() || sourceDll_.empty())
    {
        reason = "the compatibility runtime has no source or alias path";
        return false;
    }
    {
        // The compatibility runtime refuses a caller whose module path lacks nvngx.dll. This bridge
        // exports the five NGX entry points as forwarders, so the nvngx.dll installed here is a
        // distinct copy of this exact DLL, not the runtime itself; the real runtime sits beside it
        // as a sibling. It must be a copy and never a hard link: in game the bridge is already
        // loaded as BigScale.Native.dll, so a hard link to it resolves to that existing module,
        // whose path lacks nvngx.dll, and the runtime rejects the caller. A copy is a separate
        // file, so the loader creates a new module whose recorded path is this alias. No import
        // table is patched and no extra shipped binary is needed.
        // The forwarder is normally this bridge DLL itself. BIGSCALE_NVNGX_FORWARDER lets the
        // offline checks point at the built BigScale.Native.dll, because loading a copy of an EXE as
        // a DLL is not supported. The packaged game never sets it.
        std::wstring forwarderPath;
        {
            wchar_t overridePath[MAX_PATH * 4]{};
            const DWORD overrideLength =
                GetEnvironmentVariableW(L"BIGSCALE_NVNGX_FORWARDER", overridePath, MAX_PATH * 4);
            if (overrideLength > 0 && overrideLength < MAX_PATH * 4)
            {
                forwarderPath = overridePath;
            }
            else
            {
                HMODULE self = nullptr;
                if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCWSTR>(&ModuleAnchor), &self) ||
                    self == nullptr)
                {
                    reason = "the bridge module path could not be resolved for the nvngx.dll alias";
                    return false;
                }
                wchar_t selfPath[MAX_PATH * 4]{};
                const DWORD selfLength = GetModuleFileNameW(self, selfPath, MAX_PATH * 4);
                if (selfLength == 0 || selfLength >= MAX_PATH * 4)
                {
                    reason = "the bridge module path is not representable for the nvngx.dll alias";
                    return false;
                }
                forwarderPath = selfPath;
            }
        }

        DeleteFileW(aliasPath_.c_str());
        if (!CopyFileW(forwarderPath.c_str(), aliasPath_.c_str(), FALSE))
        {
            reason = "the nvngx.dll forwarder alias could not be copied";
            Log("neural: forwarder alias copy failed path=%ls error=%lu", aliasPath_.c_str(),
                static_cast<unsigned long>(GetLastError()));
            return false;
        }

        realPath_ = Join(appDataDirectory_, L"nvngx_dlssnr.dll");
        DeleteFileW(realPath_.c_str());
        if (!CreateHardLinkW(realPath_.c_str(), sourceDll_.c_str(), nullptr) &&
            !CopyFileW(sourceDll_.c_str(), realPath_.c_str(), FALSE))
        {
            reason = "the compatibility runtime could not be linked beside its nvngx.dll forwarder";
            Log("neural: runtime sibling creation failed path=%ls error=%lu", realPath_.c_str(),
                static_cast<unsigned long>(GetLastError()));
            return false;
        }
    }

    module_ = LoadLibraryExW(aliasPath_.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module_ == nullptr)
    {
        const DWORD error = GetLastError();
        reason = "the nvngx.dll alias could not be loaded";
        Log("neural: alias load failed path=%ls error=%lu", aliasPath_.c_str(),
            static_cast<unsigned long>(error));
        return false;
    }

    wchar_t modulePath[MAX_PATH * 4]{};
    const DWORD moduleLength = GetModuleFileNameW(module_, modulePath, MAX_PATH * 4);
    Log("neural: forwarder module path=%ls", modulePath);
    if (moduleLength == 0 || moduleLength >= MAX_PATH * 4 ||
        wcsstr(modulePath, L"nvngx.dll") == nullptr)
    {
        reason = "the nvngx.dll forwarder did not load under a path containing nvngx.dll";
        UnloadDirectLocked();
        return false;
    }

    initExt_ = reinterpret_cast<DlssNrInitExt>(GetProcAddress(module_, "NVSDK_NGX_D3D12_Init_Ext"));
    createFeature_ = reinterpret_cast<DlssNrCreateFeature>(GetProcAddress(module_, "NVSDK_NGX_D3D12_CreateFeature"));
    evaluateFeature_ =
        reinterpret_cast<DlssNrEvaluateFeature>(GetProcAddress(module_, "NVSDK_NGX_D3D12_EvaluateFeature"));
    releaseFeature_ =
        reinterpret_cast<DlssNrReleaseFeature>(GetProcAddress(module_, "NVSDK_NGX_D3D12_ReleaseFeature"));
    shutdown1_ = reinterpret_cast<DlssNrShutdown>(GetProcAddress(module_, "NVSDK_NGX_D3D12_Shutdown1"));
    if (initExt_ == nullptr || createFeature_ == nullptr || evaluateFeature_ == nullptr ||
        releaseFeature_ == nullptr || shutdown1_ == nullptr)
    {
        reason = "the compatibility runtime is missing its NGX D3D12 entry points";
        Log("neural: %s", reason.c_str());
        UnloadDirectLocked();
        return false;
    }

    const NVSDK_NGX_Result initialised = initExt_(kApplicationId, appDataDirectory_.c_str(), device,
        static_cast<unsigned int>(NVSDK_NGX_Version_API), capabilities_);
    if (NVSDK_NGX_FAILED(initialised))
    {
        reason = "the compatibility runtime rejected Init_Ext";
        Log("neural: compatibility Init_Ext failed result=0x%08x; falling back to the driver NGX core",
            static_cast<unsigned>(initialised));
        UnloadDirectLocked();
        return false;
    }
    directInitialized_ = true;
    Log("neural: compatibility runtime initialised app=0x%llx apiVersion=0x%08x path=%ls",
        static_cast<unsigned long long>(kApplicationId), static_cast<unsigned>(NVSDK_NGX_Version_API),
        aliasPath_.c_str());
    return true;
}

bool DlssNrRuntime::AcquireParametersLocked(NVSDK_NGX_Parameter*& parameters, std::string& reason)
{
    parameters = nullptr;
    const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters);
    if (NVSDK_NGX_FAILED(result) || parameters == nullptr)
    {
        parameters = nullptr;
        reason = "the NGX capability parameter map is unavailable";
        Log("neural: capability parameters failed result=0x%08x", static_cast<unsigned>(result));
        return false;
    }
    // The header documents a fresh map per call, while some builds return a shared object. A
    // reference count covers both: the map is destroyed when its last feature releases it, and a
    // shared map survives until every feature and the capability reference are gone.
    RetainMapLocked(parameters);
    return true;
}

void DlssNrRuntime::RetainMapLocked(NVSDK_NGX_Parameter* parameters)
{
    if (parameters == nullptr) return;
    for (OwnedParameterMap& entry : parameterMaps_)
    {
        if (entry.map != parameters) continue;
        entry.references++;
        return;
    }
    parameterMaps_.push_back({parameters, 1});
}

void DlssNrRuntime::ReleaseMapLocked(NVSDK_NGX_Parameter* parameters)
{
    if (parameters == nullptr) return;
    for (size_t index = 0; index < parameterMaps_.size(); index++)
    {
        if (parameterMaps_[index].map != parameters) continue;
        if (parameterMaps_[index].references > 1)
        {
            parameterMaps_[index].references--;
            return;
        }
        const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_DestroyParameters(parameters);
        if (NVSDK_NGX_FAILED(result))
            Log("neural: parameter map destruction failed result=0x%08x", static_cast<unsigned>(result));
        parameterMaps_.erase(parameterMaps_.begin() + static_cast<std::ptrdiff_t>(index));
        return;
    }
}

void DlssNrRuntime::ApplyCreationLocked(NVSDK_NGX_Parameter* parameters, const NeuralFeature& feature)
{
    parameters->Set(kEnabled, static_cast<unsigned int>(1));
    parameters->Set(kWidth, static_cast<unsigned int>(feature.width));
    parameters->Set(kHeight, static_cast<unsigned int>(feature.height));
    parameters->Set(kCreationNodeMask, static_cast<unsigned int>(1));
    parameters->Set(kVisibilityNodeMask, static_cast<unsigned int>(1));
    parameters->Set(kHintRenderPreset, static_cast<int>(0));
    parameters->Set(kIntensity, static_cast<float>(feature.tuning.strength));
    parameters->Set(kStyle, static_cast<int>(feature.tuning.style));
    parameters->Set(kLocalStructureStrength, static_cast<float>(feature.tuning.detail));
    parameters->Set(kLocalToneStrength, static_cast<float>(feature.tuning.tone));
    parameters->Set(kSkinStructureStrength, -1.0f);
    parameters->Set(kUseAutoMask, static_cast<int>(0));
    parameters->Set(kUiCorrection, static_cast<int>(1));
    // The stage blends the mod-owned character mask itself, so the runtime is given no mask and no
    // UI or back-buffer surface at all.
    parameters->Set(kControlMask, static_cast<ID3D12Resource*>(nullptr));
    parameters->Set(kUi, static_cast<ID3D12Resource*>(nullptr));
    parameters->Set(kUiAlpha, static_cast<ID3D12Resource*>(nullptr));
    parameters->Set(kBackbuffer, static_cast<ID3D12Resource*>(nullptr));
    parameters->Set(kUiSubrectBaseX, static_cast<unsigned int>(0));
    parameters->Set(kUiSubrectBaseY, static_cast<unsigned int>(0));
    parameters->Set(kUiSubrectWidth, static_cast<unsigned int>(0));
    parameters->Set(kUiSubrectHeight, static_cast<unsigned int>(0));
    parameters->Set(kUiAlphaSubrectBaseX, static_cast<unsigned int>(0));
    parameters->Set(kUiAlphaSubrectBaseY, static_cast<unsigned int>(0));
    parameters->Set(kUiAlphaSubrectWidth, static_cast<unsigned int>(0));
    parameters->Set(kUiAlphaSubrectHeight, static_cast<unsigned int>(0));
    parameters->Set(kBackbufferSubrectBaseX, static_cast<unsigned int>(0));
    parameters->Set(kBackbufferSubrectBaseY, static_cast<unsigned int>(0));
    parameters->Set(kBackbufferSubrectWidth, static_cast<unsigned int>(0));
    parameters->Set(kBackbufferSubrectHeight, static_cast<unsigned int>(0));
}

bool DlssNrRuntime::CreateWithDriverLocked(ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter* parameters,
    NVSDK_NGX_Handle** handle, NVSDK_NGX_Result& result)
{
    *handle = nullptr;
    result = NVSDK_NGX_D3D12_CreateFeature(list, static_cast<NVSDK_NGX_Feature>(kFeatureId), parameters, handle);
    return !NVSDK_NGX_FAILED(result) && *handle != nullptr;
}

void DlssNrRuntime::NoteResultLocked(const char* stage, NVSDK_NGX_Result result, bool success)
{
    NVSDK_NGX_Result& previous = std::strcmp(stage, "create") == 0 ? lastCreateResult_ : lastEvaluateResult_;
    // A successful result never logs per frame; only the first success and every transition do.
    if (success == !NVSDK_NGX_FAILED(previous) && result == previous) return;
    previous = result;
    Log("neural: %s result=0x%08x", stage, static_cast<unsigned>(result));
}

bool DlssNrRuntime::CreateFeature(ID3D12GraphicsCommandList* list, uint32_t width, uint32_t height,
    const NeuralTuning& tuning, NeuralFeature& feature, std::string& reason)
{
    std::lock_guard<std::mutex> guard(mutex_);
    feature = NeuralFeature{};
    if (!ready_.load() || list == nullptr)
    {
        reason = "the neural runtime is not ready to create a feature";
        return false;
    }

    NVSDK_NGX_Parameter* parameters = nullptr;
    if (!AcquireParametersLocked(parameters, reason)) return false;

    feature.parameters = parameters;
    feature.tuning = tuning;
    feature.width = width;
    feature.height = height;
    ApplyCreationLocked(parameters, feature);

    NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
    NVSDK_NGX_Handle* handle = nullptr;
    if (directInitialized_ && createFeature_ != nullptr)
    {
        result = createFeature_(list, static_cast<NVSDK_NGX_Feature>(kFeatureId), parameters, &handle);
        if (!NVSDK_NGX_FAILED(result) && handle != nullptr) feature.owner = NeuralFeature::Direct;
    }

    if (feature.owner == NeuralFeature::None)
    {
        if (directInitialized_ && directFeatures_ == 0)
        {
            // The first creation is the compat runtime's only chance: once one of its features
            // exists, its module must stay loaded, so a later failure is reported instead.
            Log("neural: compatibility feature creation failed result=0x%08x; falling back to the driver core",
                static_cast<unsigned>(result));
            UnloadDirectLocked();
            ApplyCreationLocked(parameters, feature);
        }
        if (!CreateWithDriverLocked(list, parameters, &handle, result))
        {
            NoteResultLocked("create", result, false);
            ReleaseMapLocked(feature.parameters);
            feature.parameters = nullptr;
            reason = "NGX could not create the neural feature";
            Log("neural: %s result=0x%08x", reason.c_str(), static_cast<unsigned>(result));
            return false;
        }
        feature.owner = NeuralFeature::Driver;
    }

    feature.handle = handle;
    if (feature.owner == NeuralFeature::Direct) directFeatures_++;
    NoteResultLocked("create", result, true);
    Log("neural: feature created owner=%s %ux%u style=%d strength=%.2f detail=%.2f tone=%.2f",
        feature.owner == NeuralFeature::Direct ? "compat" : "driver", width, height, tuning.style,
        tuning.strength, tuning.detail, tuning.tone);
    return true;
}

bool DlssNrRuntime::EvaluateFeature(ID3D12GraphicsCommandList* list, NeuralFeature& feature,
    const NeuralEvaluation& evaluation, std::string& reason)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (list == nullptr || feature.handle == nullptr || feature.parameters == nullptr)
    {
        reason = "the neural feature is not ready to evaluate";
        return false;
    }
    if (evaluation.color == nullptr || evaluation.depth == nullptr || evaluation.motion == nullptr ||
        evaluation.output == nullptr)
    {
        reason = "the neural evaluation is missing a resource";
        return false;
    }

    NVSDK_NGX_Parameter* parameters = feature.parameters;
    // Creation-time tuning is immutable for a created feature. Re-applying it here made the
    // compatibility runtime reconfigure its model on every frame, which ended in a device hang;
    // only the evaluation inputs and the per-frame reset flag change now.
    parameters->Set(kColor, evaluation.color);
    parameters->Set(kDepth, evaluation.depth);
    parameters->Set(kMotion, evaluation.motion);
    parameters->Set(kOutput, evaluation.output);
    parameters->Set(kWidth, static_cast<unsigned int>(evaluation.width));
    parameters->Set(kHeight, static_cast<unsigned int>(evaluation.height));
    parameters->Set(kDepthInverted, static_cast<int>(evaluation.depthInverted ? 1 : 0));
    parameters->Set(kReset, static_cast<int>(evaluation.reset ? 1 : 0));
    parameters->Set(kMotionScaleX, static_cast<float>(evaluation.motionScaleX));
    parameters->Set(kMotionScaleY, static_cast<float>(evaluation.motionScaleY));
    // Colour and output cover the stage extent; the guides keep their own size, so every region
    // publishes its own origin and extent.
    parameters->Set(kColorSubrectBaseX, static_cast<unsigned int>(0));
    parameters->Set(kColorSubrectBaseY, static_cast<unsigned int>(0));
    parameters->Set(kColorSubrectWidth, static_cast<unsigned int>(evaluation.width));
    parameters->Set(kColorSubrectHeight, static_cast<unsigned int>(evaluation.height));
    parameters->Set(kOutputSubrectBaseX, static_cast<unsigned int>(0));
    parameters->Set(kOutputSubrectBaseY, static_cast<unsigned int>(0));
    parameters->Set(kOutputSubrectWidth, static_cast<unsigned int>(evaluation.width));
    parameters->Set(kOutputSubrectHeight, static_cast<unsigned int>(evaluation.height));
    parameters->Set(kDepthSubrectBaseX, static_cast<unsigned int>(0));
    parameters->Set(kDepthSubrectBaseY, static_cast<unsigned int>(0));
    parameters->Set(kDepthSubrectWidth, static_cast<unsigned int>(evaluation.guideWidth));
    parameters->Set(kDepthSubrectHeight, static_cast<unsigned int>(evaluation.guideHeight));
    parameters->Set(kMotionSubrectBaseX, static_cast<unsigned int>(0));
    parameters->Set(kMotionSubrectBaseY, static_cast<unsigned int>(0));
    parameters->Set(kMotionSubrectWidth, static_cast<unsigned int>(evaluation.guideWidth));
    parameters->Set(kMotionSubrectHeight, static_cast<unsigned int>(evaluation.guideHeight));

    NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
    if (feature.owner == NeuralFeature::Direct && evaluateFeature_ != nullptr)
    {
        result = evaluateFeature_(list, feature.handle, parameters, nullptr);
    }
    else
    {
        result = NVSDK_NGX_D3D12_EvaluateFeature(list, feature.handle, parameters, nullptr);
    }
    if (NVSDK_NGX_FAILED(result))
    {
        NoteResultLocked("evaluate", result, false);
        reason = "NGX could not evaluate the neural feature";
        return false;
    }
    NoteResultLocked("evaluate", result, true);
    return true;
}

void DlssNrRuntime::ReleaseFeature(NeuralFeature& feature)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (feature.handle != nullptr)
    {
        NVSDK_NGX_Result result = NVSDK_NGX_Result_Success;
        if (feature.owner == NeuralFeature::Direct && releaseFeature_ != nullptr)
            result = releaseFeature_(feature.handle);
        else
            result = NVSDK_NGX_D3D12_ReleaseFeature(feature.handle);
        if (NVSDK_NGX_FAILED(result))
            Log("neural: feature release failed result=0x%08x", static_cast<unsigned>(result));
        if (feature.owner == NeuralFeature::Direct && directFeatures_ > 0) directFeatures_--;
    }
    feature.handle = nullptr;
    ReleaseMapLocked(feature.parameters);
    feature.parameters = nullptr;
    feature.owner = NeuralFeature::None;
}

void DlssNrRuntime::UnloadDirectLocked()
{
    initExt_ = nullptr;
    createFeature_ = nullptr;
    evaluateFeature_ = nullptr;
    releaseFeature_ = nullptr;
    shutdown1_ = nullptr;
    if (module_ != nullptr)
    {
        // The alias owns the only reference to the real runtime. Release it explicitly before the
        // forwarder module is unloaded, then drop the alias itself.
        using ReleaseReal = void(__cdecl*)();
        auto releaseReal = reinterpret_cast<ReleaseReal>(GetProcAddress(module_, "BigScaleNvngxReleaseReal"));
        if (releaseReal != nullptr) releaseReal();
        FreeLibrary(module_);
        module_ = nullptr;
    }
    // Both names are per-device links/copies. Removing them after the module is unloaded keeps a
    // cross-volume copy of the 264 MB runtime from surviving an upgrade or a failed initialisation.
    if (!aliasPath_.empty()) DeleteFileW(aliasPath_.c_str());
    if (!realPath_.empty()) DeleteFileW(realPath_.c_str());
    directInitialized_ = false;
}

void DlssNrRuntime::Release()
{
    std::lock_guard<std::mutex> guard(mutex_);
    // Maps belong to the driver core and must be destroyed while it is still initialised; after
    // Shutdown1 the SDK returns NotInitialized for every one of them.
    for (OwnedParameterMap& entry : parameterMaps_)
    {
        if (entry.map == nullptr) continue;
        const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_DestroyParameters(entry.map);
        if (NVSDK_NGX_FAILED(result))
            Log("neural: parameter map destruction failed result=0x%08x", static_cast<unsigned>(result));
    }
    parameterMaps_.clear();
    capabilities_ = nullptr;
    if (directInitialized_ && shutdown1_ != nullptr && device_ != nullptr)
    {
        const NVSDK_NGX_Result result = shutdown1_(device_);
        if (NVSDK_NGX_FAILED(result))
            Log("neural: compatibility shutdown failed result=0x%08x", static_cast<unsigned>(result));
    }
    UnloadDirectLocked();
    if (driverInitialized_ && device_ != nullptr)
    {
        const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Shutdown1(device_);
        if (NVSDK_NGX_FAILED(result))
            Log("neural: driver NGX shutdown failed result=0x%08x", static_cast<unsigned>(result));
        driverInitialized_ = false;
    }
    ready_ = false;
    device_ = nullptr;
    lastCreateResult_ = NVSDK_NGX_Result_Success;
    lastEvaluateResult_ = NVSDK_NGX_Result_Success;
}
}
