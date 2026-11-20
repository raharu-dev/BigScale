#include "FsrDx12Backend.h"

#include "../Log.h"

#include <ffx_api.h>
#include <ffx_api_dx12.h>
#include <ffx_upscale.h>

#include <cstdio>
#include <vector>

namespace bigscale
{
namespace
{
uint32_t PackVersion()
{
    return (static_cast<uint32_t>(FFX_UPSCALER_VERSION_MAJOR) << 24) |
           (static_cast<uint32_t>(FFX_UPSCALER_VERSION_MINOR) << 16) |
           static_cast<uint32_t>(FFX_UPSCALER_VERSION_PATCH);
}

float RatioForPreset(Preset preset)
{
    switch (preset)
    {
        case Preset::Quality: return 1.5f;
        case Preset::Balanced: return 1.7f;
        case Preset::Performance: return 2.0f;
        case Preset::UltraPerformance: return 3.0f;
        case Preset::NativeAA: return 1.0f;
    }
    return 1.5f;
}
}

struct FsrDx12Backend::Api
{
    ffxReturnCode_t (*createContext)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*) = nullptr;
    ffxReturnCode_t (*destroyContext)(ffxContext*, const ffxAllocationCallbacks*) = nullptr;
    ffxReturnCode_t (*configure)(ffxContext*, const ffxConfigureDescHeader*) = nullptr;
    ffxReturnCode_t (*query)(ffxContext*, ffxQueryDescHeader*) = nullptr;
    ffxReturnCode_t (*dispatch)(ffxContext*, const ffxDispatchDescHeader*) = nullptr;
};

struct FsrDx12Backend::View
{
    ViewKey key{};
    ffxContext context = nullptr;
    uint32_t inputWidth = 0;
    uint32_t inputHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    Preset preset = Preset::Quality;
    bool invertedDepth = false;
    uint32_t evaluations = 0;
    // The FSR API requires the create descriptors to stay live until ffxDestroyContext.
    ffxCreateContextDescUpscale upscale{};
    ffxCreateBackendDX12Desc backend{};
    ffxCreateContextDescUpscaleVersion apiVersion{};
    ffxOverrideVersion overrideVersion{};
    SharedTexture color;
    SharedTexture depth;
    SharedTexture motion;
    SharedTexture output;
};

FsrDx12Backend::FsrDx12Backend(UpscalerKind kind, int32_t majorVersion, bool amdOnly)
    : kind_(kind), majorVersion_(majorVersion), amdOnly_(amdOnly)
{
}

FsrDx12Backend::~FsrDx12Backend() { Release(); }

bool FsrDx12Backend::LoadApi()
{
    if (module_ != nullptr) return true;
    if (api_ == nullptr) api_ = new Api();
    std::wstring path = runtimeDirectory_;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
    path += L"amd_fidelityfx_upscaler_dx12.dll";
    module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module_ == nullptr)
    {
        const DWORD error = GetLastError();
        lastReason_ = "amd_fidelityfx_upscaler_dx12.dll is not staged in the plugin directory";
        Log("%s: upscaler module could not be loaded (error=%lu).", Label(), static_cast<unsigned long>(error));
        return false;
    }
    api_->createContext = reinterpret_cast<decltype(api_->createContext)>(GetProcAddress(module_, "ffxCreateContext"));
    api_->destroyContext = reinterpret_cast<decltype(api_->destroyContext)>(GetProcAddress(module_, "ffxDestroyContext"));
    api_->configure = reinterpret_cast<decltype(api_->configure)>(GetProcAddress(module_, "ffxConfigure"));
    api_->query = reinterpret_cast<decltype(api_->query)>(GetProcAddress(module_, "ffxQuery"));
    api_->dispatch = reinterpret_cast<decltype(api_->dispatch)>(GetProcAddress(module_, "ffxDispatch"));
    if (!api_->createContext || !api_->destroyContext || !api_->configure || !api_->query || !api_->dispatch)
    {
        lastReason_ = "the FidelityFX upscaler runtime is missing its API entry points";
        return false;
    }
    runtimeVersion_ = PackVersion();
    Log("%s: upscaler module loaded path=%ls apiHeaderVersion=0x%08x", Label(), path.c_str(), runtimeVersion_);
    return true;
}

bool FsrDx12Backend::AmdAdapter() const
{
    if (!device_) return false;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(const_cast<ID3D11Device*>(device_)->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || !dxgiDevice ||
        FAILED(dxgiDevice->GetAdapter(&adapter)) || !adapter)
        return false;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc))) return false;
    Log("%s: adapter check vendor=0x%04x device=0x%04x", Label(), desc.VendorId, desc.DeviceId);
    return desc.VendorId == 0x1002;
}

bool SelectFsrProviderVersion(const uint64_t* ids, const char* const* names, size_t count,
                              int32_t majorVersion, uint64_t& selectedId, std::string& selectedName)
{
    selectedId = 0;
    selectedName.clear();
    if (ids == nullptr || count == 0 || majorVersion < 0) return false;

    bool haveBest = false;
    uint32_t bestVersion = 0;
    bool bestIsDriver = false;
    size_t bestIndex = 0;
    for (size_t i = 0; i < count; i++)
    {
        const uint32_t version = static_cast<uint32_t>(ids[i]) & 0x7fffffffu;
        if (version == 0) continue;
        if (static_cast<int32_t>(version >> 22) != majorVersion) continue;
        const bool isDriver = (static_cast<uint32_t>(ids[i]) & 0x80000000u) != 0;
        if (haveBest && (version < bestVersion || (version == bestVersion && (isDriver || !bestIsDriver))))
            continue;
        haveBest = true;
        bestVersion = version;
        bestIsDriver = isDriver;
        bestIndex = i;
    }
    if (!haveBest) return false;

    selectedId = ids[bestIndex];
    if (names != nullptr && names[bestIndex] != nullptr) selectedName = names[bestIndex];
    return true;
}

bool FsrDx12Backend::EnumerateVersion()
{
    if (providerVersionId_ != 0) return true;
    ffxQueryDescGetVersions desc{};
    desc.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    desc.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.device = interop_.Device();
    uint64_t count = 0;
    desc.outputCount = &count;
    if (api_->query(nullptr, &desc.header) != FFX_API_RETURN_OK || count == 0)
    {
        lastReason_ = std::string("no FidelityFX upscaler implementation is installed for ") + Label();
        Log("%s: version enumeration returned no implementations.", Label());
        return false;
    }
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    uint64_t capacity = count;
    desc.outputCount = &capacity;
    desc.versionIds = ids.data();
    desc.versionNames = names.data();
    if (api_->query(nullptr, &desc.header) != FFX_API_RETURN_OK || capacity == 0)
    {
        lastReason_ = "the FidelityFX version list could not be read";
        return false;
    }
    ids.resize(capacity);
    names.resize(capacity);

    for (uint64_t i = 0; i < capacity; i++)
        Log("%s: offered provider id=0x%llx name=%s", Label(), static_cast<unsigned long long>(ids[i]),
            names[i] != nullptr ? names[i] : "?");

    uint64_t selected = 0;
    std::string selectedName;
    if (!SelectFsrProviderVersion(ids.data(), names.data(), static_cast<size_t>(capacity), majorVersion_,
            selected, selectedName))
    {
        lastReason_ = std::string("no ") + Label() + " implementation is installed (only other versions were found)";
        Log("%s: no matching implementation in the installed provider list.", Label());
        return false;
    }
    providerVersionId_ = selected;
    const uint32_t selectedVersion = static_cast<uint32_t>(selected) & 0x7fffffffu;
    Log("%s implementation selected: %s (id=0x%llx version=%u.%u.%u)", Label(),
        selectedName.empty() ? "?" : selectedName.c_str(), static_cast<unsigned long long>(selected),
        selectedVersion >> 22, (selectedVersion >> 12) & 0x3ffu, selectedVersion & 0xfffu);
    return true;
}

bool FsrDx12Backend::EnsureShared(SharedTexture& texture, uint32_t width, uint32_t height, DXGI_FORMAT format,
                                  bool unorderedAccess, std::string& reason)
{
    if (texture.d3d11 && texture.width == width && texture.height == height && texture.format == format &&
        texture.shared.d3d12)
        return true;
    texture.shared.Reset();
    texture.d3d11.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (unorderedAccess ? D3D11_BIND_UNORDERED_ACCESS : 0);
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &texture.d3d11)) || !texture.d3d11)
    {
        reason = "a shared interop texture could not be created";
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    HANDLE handle = nullptr;
    if (FAILED(texture.d3d11->QueryInterface(IID_PPV_ARGS(&resource))) || !resource ||
        FAILED(resource->GetSharedHandle(&handle)) || handle == nullptr ||
        FAILED(interop_.Device()->OpenSharedHandle(handle, IID_PPV_ARGS(&texture.shared.d3d12))) ||
        !texture.shared.d3d12)
    {
        reason = "a shared interop texture could not be opened on the bridge device";
        texture.d3d11.Reset();
        return false;
    }
    texture.shared.d3d11 = texture.d3d11;
    texture.shared.handle = handle;
    texture.shared.width = width;
    texture.shared.height = height;
    texture.shared.format = format;
    texture.shared.state = D3D12_RESOURCE_STATE_COMMON;
    texture.width = width;
    texture.height = height;
    texture.format = format;
    return true;
}

bool FsrDx12Backend::Attach(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (device == nullptr || context == nullptr) return false;
    if (device_ != device)
    {
        Release();
        device_ = device;
    }
    context_ = context;
    if (!context4_ && (FAILED(context->QueryInterface(IID_PPV_ARGS(&context4_))) || !context4_))
    {
        lastReason_ = "the game context is not an ID3D11DeviceContext4";
        return false;
    }
    amd_ = AmdAdapter();
    attached_ = true;
    return true;
}

bool FsrDx12Backend::Available()
{
    if (!attached_)
    {
        lastReason_ = "the backend is not attached";
        return false;
    }
    if (amdOnly_ && !amd_)
    {
        lastReason_ = "official AMD FSR 4 requires a supported AMD GPU";
        return false;
    }
    if (!LoadApi()) return false;
    std::string reason;
    if (!interop_.Ensure(device_, reason))
    {
        lastReason_ = reason;
        return false;
    }
    if (!EnumerateVersion()) return false;
    lastReason_.clear();
    return true;
}

bool FsrDx12Backend::QueryModes(uint32_t outputWidth, uint32_t outputHeight, ModeRange (&ranges)[kModeCount],
                                uint32_t& runtimeVersion)
{
    runtimeVersion = runtimeVersion_;
    for (int i = 0; i < kModeCount; i++) ranges[i] = ModeRange{};
    if (!Available() || outputWidth == 0 || outputHeight == 0) return false;

    bool any = false;
    for (int i = 0; i < kModeCount; i++)
    {
        const auto preset = static_cast<Preset>(i + 1);
        const float ratio = RatioForPreset(preset);
        const uint32_t width = static_cast<uint32_t>(static_cast<float>(outputWidth) / ratio + 0.5f);
        const uint32_t height = static_cast<uint32_t>(static_cast<float>(outputHeight) / ratio + 0.5f);
        if (width < 32 || height < 32 || width > outputWidth || height > outputHeight) continue;
        ModeRange& range = ranges[i];
        range.supported = true;
        range.optimalWidth = width;
        range.optimalHeight = height;
        range.minWidth = 1;
        range.minHeight = 1;
        range.maxWidth = outputWidth;
        range.maxHeight = outputHeight;
        any = true;
    }
    if (!any) lastReason_ = std::string(Label()) + " cannot size any preset at this output resolution";
    return any;
}

FsrDx12Backend::View* FsrDx12Backend::EnsureView(const ViewKey& key)
{
    for (View* view : views_)
        if (view->key == key) return view;
    if (views_.size() >= static_cast<size_t>(kMaxViews))
    {
        lastReason_ = "more FidelityFX views than the bridge supports";
        return nullptr;
    }
    auto* view = new View();
    view->key = key;
    views_.push_back(view);
    return view;
}

bool FsrDx12Backend::EnsureContext(View& view, const EvaluationRequest& request)
{
    if (view.context != nullptr && view.inputWidth == request.inputWidth &&
        view.inputHeight == request.inputHeight && view.outputWidth == request.outputWidth &&
        view.outputHeight == request.outputHeight && view.preset == request.preset &&
        view.invertedDepth == request.reversedDepth)
        return true;

    Destroy(view);
    std::string reason;
    if (!EnsureShared(view.color, request.inputWidth, request.inputHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, false,
            reason) ||
        !EnsureShared(view.depth, request.inputWidth, request.inputHeight, DXGI_FORMAT_R32_FLOAT, false, reason) ||
        !EnsureShared(view.motion, request.inputWidth, request.inputHeight, DXGI_FORMAT_R16G16_FLOAT, false, reason) ||
        !EnsureShared(view.output, request.outputWidth, request.outputHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, true,
            reason))
    {
        lastReason_ = reason;
        return false;
    }

    view.upscale = ffxCreateContextDescUpscale{};
    view.upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    view.upscale.flags = 0;
    if (request.reversedDepth) view.upscale.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (request.hdrColor) view.upscale.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    view.upscale.maxRenderSize = {request.inputWidth, request.inputHeight};
    view.upscale.maxUpscaleSize = {request.outputWidth, request.outputHeight};

    view.backend = ffxCreateBackendDX12Desc{};
    view.backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    view.backend.device = interop_.Device();

    view.apiVersion = ffxCreateContextDescUpscaleVersion{};
    view.apiVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    view.apiVersion.version = FFX_UPSCALER_VERSION;

    view.overrideVersion = ffxOverrideVersion{};
    view.overrideVersion.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    view.overrideVersion.versionId = providerVersionId_;

    view.upscale.header.pNext = &view.backend.header;
    view.backend.header.pNext = &view.apiVersion.header;
    view.apiVersion.header.pNext = &view.overrideVersion.header;
    view.overrideVersion.header.pNext = nullptr;

    const ffxReturnCode_t result = api_->createContext(&view.context, &view.upscale.header, nullptr);
    if (result != FFX_API_RETURN_OK || view.context == nullptr)
    {
        lastReason_ = std::string("ffxCreateContext failed for ") + Label();
        Log("%s: context creation failed code=%u", Label(), result);
        view.context = nullptr;
        return false;
    }
    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const ffxReturnCode_t queryResult = api_->query(&view.context, &provider.header);
    if (queryResult != FFX_API_RETURN_OK || provider.versionName == nullptr || provider.versionId == 0)
    {
        lastReason_ = std::string("the created ") + Label() + " context could not report its provider";
        Log("%s: provider version query failed code=%u", Label(), queryResult);
        api_->destroyContext(&view.context, nullptr);
        view.context = nullptr;
        return false;
    }
    if (provider.versionId != providerVersionId_)
    {
        lastReason_ = std::string("the created ") + Label() + " context was not the selected provider";
        Log("%s: provider mismatch selected=0x%llx actual=0x%llx name=%s", Label(),
            static_cast<unsigned long long>(providerVersionId_),
            static_cast<unsigned long long>(provider.versionId), provider.versionName);
        api_->destroyContext(&view.context, nullptr);
        view.context = nullptr;
        return false;
    }
    Log("%s: provider verified: %s (id=0x%llx)", Label(), provider.versionName,
        static_cast<unsigned long long>(provider.versionId));
    view.inputWidth = request.inputWidth;
    view.inputHeight = request.inputHeight;
    view.outputWidth = request.outputWidth;
    view.outputHeight = request.outputHeight;
    view.preset = request.preset;
    view.invertedDepth = request.reversedDepth;
    Log("%s: context created camera=%d eye=%d render=%ux%u output=%ux%u", Label(), view.key.cameraId,
        view.key.eye, view.inputWidth, view.inputHeight, view.outputWidth, view.outputHeight);
    return true;
}

int32_t FsrDx12Backend::Evaluate(const ViewKey& key, const EvaluationRequest& request, bool& featureCreated)
{
    featureCreated = false;
    if (!Available()) return -1;
    View* view = EnsureView(key);
    if (view == nullptr) return -1;
    if (!EnsureContext(*view, request)) return -1;
    featureCreated = view->evaluations == 0;

    context_->CopyResource(view->color.d3d11.Get(), request.color);
    context_->CopyResource(view->depth.d3d11.Get(), request.depth);
    context_->CopyResource(view->motion.d3d11.Get(), request.motion);
    std::string reason;
    uint64_t sourceFenceValue = 0;
    if (!interop_.SignalFromD3D11(context4_.Get(), reason, sourceFenceValue) || !interop_.Begin(reason))
    {
        Log("%s: interop submission start failed: %s", Label(), reason.c_str());
        lastReason_ = reason;
        return -1;
    }

    ID3D12GraphicsCommandList* list = interop_.List();
    Dx12Interop::Transition(list, view->color.shared, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Dx12Interop::Transition(list, view->depth.shared, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Dx12Interop::Transition(list, view->motion.shared, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Dx12Interop::Transition(list, view->output.shared, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ffxDispatchDescUpscale dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = list;
    dispatch.color = ffxApiGetResourceDX12(view->color.shared.d3d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.depth = ffxApiGetResourceDX12(view->depth.shared.d3d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.motionVectors = ffxApiGetResourceDX12(view->motion.shared.d3d12.Get(),
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.exposure = ffxApiGetResourceDX12(nullptr);
    dispatch.reactive = ffxApiGetResourceDX12(nullptr);
    dispatch.transparencyAndComposition = ffxApiGetResourceDX12(nullptr);
    dispatch.output = ffxApiGetResourceDX12(view->output.shared.d3d12.Get(),
        FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    // Unity gives previous-to-current UV displacement, already Y-flipped on D3D; FSR wants
    // current-to-previous pixel-space motion.
    dispatch.jitterOffset = {request.jitterX, request.jitterY};
    dispatch.motionVectorScale = {-static_cast<float>(request.inputWidth),
        -static_cast<float>(request.inputHeight)};
    dispatch.renderSize = {request.inputWidth, request.inputHeight};
    dispatch.upscaleSize = {request.outputWidth, request.outputHeight};
    dispatch.enableSharpening = false;
    dispatch.sharpness = 0.0f;
    dispatch.frameTimeDelta = request.deltaMilliseconds;
    dispatch.preExposure = 1.0f;
    dispatch.reset = request.reset || featureCreated;
    dispatch.cameraNear = request.cameraNear;
    dispatch.cameraFar = request.cameraFar;
    dispatch.cameraFovAngleVertical = request.verticalFovRadians;
    dispatch.viewSpaceToMetersFactor = 1.0f;
    dispatch.flags = 0;

    const ffxReturnCode_t result = api_->dispatch(&view->context, &dispatch.header);
    Dx12Interop::Transition(list, view->output.shared, D3D12_RESOURCE_STATE_COMMON);
    Dx12Interop::Transition(list, view->motion.shared, D3D12_RESOURCE_STATE_COMMON);
    Dx12Interop::Transition(list, view->depth.shared, D3D12_RESOURCE_STATE_COMMON);
    Dx12Interop::Transition(list, view->color.shared, D3D12_RESOURCE_STATE_COMMON);

    if (result != FFX_API_RETURN_OK)
    {
        lastReason_ = std::string("ffxDispatch failed for ") + Label();
        Log("%s: dispatch failed camera=%d eye=%d code=%u", Label(), key.cameraId, key.eye, result);
        return static_cast<int32_t>(result);
    }
    if (!interop_.Submit(reason, sourceFenceValue) || !interop_.WaitOnD3D11(context4_.Get(), reason))
    {
        Log("%s: interop submission completion failed: %s", Label(), reason.c_str());
        lastReason_ = reason;
        return -1;
    }
    context_->CopyResource(request.output, view->output.d3d11.Get());

    view->evaluations++;
    if (view->evaluations == 1 || view->evaluations % 600 == 0)
        Log("%s: dispatch ok camera=%d eye=%d render=%ux%u output=%ux%u reset=%d", Label(), key.cameraId,
            key.eye, request.inputWidth, request.inputHeight, request.outputWidth, request.outputHeight,
            dispatch.reset ? 1 : 0);
    return 0;
}

void FsrDx12Backend::Destroy(View& view)
{
    if (view.context != nullptr)
    {
        if (api_ != nullptr && api_->destroyContext != nullptr) api_->destroyContext(&view.context, nullptr);
        view.context = nullptr;
    }
    view.color.shared.Reset();
    view.depth.shared.Reset();
    view.motion.shared.Reset();
    view.output.shared.Reset();
    view.color.d3d11.Reset();
    view.depth.d3d11.Reset();
    view.motion.d3d11.Reset();
    view.output.d3d11.Reset();
}

void FsrDx12Backend::Forget(const ViewKey& key)
{
    for (size_t i = 0; i < views_.size(); i++)
    {
        if (!(views_[i]->key == key)) continue;
        View* view = views_[i];
        views_.erase(views_.begin() + static_cast<std::ptrdiff_t>(i));
        // A stereo session fills both view slots, so a rig rebuild recycles a view whose shared
        // textures may still be read by a submission the closed ring has already handed out again.
        // Destroying under it is a device removal; prove the bridge idle instead.
        std::string reason;
        if (!interop_.WaitIdle(2000, reason))
        {
            Log("%s: view retire wait failed (%s); keeping camera=%d eye=%d until release.", Label(),
                reason.c_str(), key.cameraId, key.eye);
            retired_.push_back(view);
            return;
        }
        Destroy(*view);
        delete view;
        return;
    }
}

void FsrDx12Backend::Release()
{
    for (View* view : views_)
    {
        Destroy(*view);
        delete view;
    }
    views_.clear();
    // Release only runs after a proven GPU drain, so views kept by a failed retire wait are safe now.
    for (View* view : retired_)
    {
        Destroy(*view);
        delete view;
    }
    retired_.clear();
    interop_.ReleaseAll();
    if (module_ != nullptr)
    {
        FreeLibrary(module_);
        module_ = nullptr;
    }
    delete api_;
    api_ = nullptr;
    context4_.Reset();
    context_ = nullptr;
    device_ = nullptr;
    attached_ = false;
    providerVersionId_ = 0;
}
}
