#include "XessBackend.h"

#include "../Log.h"

#include <xess/xess.h>
#include <xess/xess_d3d12.h>

#include <algorithm>
#include <vector>

namespace bigscale
{
namespace
{
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
    return (HIWORD(info->dwFileVersionMS) << 24) | (LOWORD(info->dwFileVersionMS) << 16) |
           (HIWORD(info->dwFileVersionLS) & 0xFFFFu);
}

const char* ResultName(xess_result_t result)
{
    switch (result)
    {
        case XESS_RESULT_SUCCESS: return "success";
        case XESS_RESULT_WARNING_OLD_DRIVER: return "an older driver is installed";
        case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE: return "this GPU does not support XeSS";
        case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER: return "this driver does not support XeSS";
        case XESS_RESULT_ERROR_UNINITIALIZED: return "the XeSS context is not initialised";
        case XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY: return "XeSS ran out of GPU memory";
        case XESS_RESULT_ERROR_CANT_LOAD_LIBRARY: return "the XeSS library could not be loaded";
        default: return "the XeSS call failed";
    }
}
}

struct XessBackend::Api
{
    xess_result_t (*createContext)(ID3D12Device*, xess_context_handle_t*) = nullptr;
    xess_result_t (*buildPipelines)(xess_context_handle_t, ID3D12PipelineLibrary*, bool, uint32_t) = nullptr;
    xess_result_t (*init)(xess_context_handle_t, const xess_d3d12_init_params_t*) = nullptr;
    xess_result_t (*execute)(xess_context_handle_t, ID3D12GraphicsCommandList*,
                             const xess_d3d12_execute_params_t*) = nullptr;
    xess_result_t (*destroyContext)(xess_context_handle_t) = nullptr;
    xess_result_t (*setVelocityScale)(xess_context_handle_t, float, float) = nullptr;
    xess_result_t (*getOptimalInputResolution)(xess_context_handle_t, const xess_2d_t*, xess_quality_settings_t,
                                               xess_2d_t*, xess_2d_t*, xess_2d_t*) = nullptr;
};

struct XessBackend::View
{
    ViewKey key{};
    xess_context_handle_t context = nullptr;
    uint32_t inputWidth = 0;
    uint32_t inputHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    int32_t quality = -1;
    bool invertedDepth = false;
    uint32_t evaluations = 0;
    SharedTexture color;
    SharedTexture depth;
    SharedTexture motion;
    SharedTexture output;
};

XessBackend::~XessBackend() { Release(); }

bool XessBackend::LoadApi()
{
    if (module_ != nullptr) return true;
    if (api_ == nullptr) api_ = new Api();
    std::wstring path = runtimeDirectory_;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
    path += L"libxess.dll";
    module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module_ == nullptr)
    {
        Log("XeSS: libxess.dll could not be loaded from the runtime directory.");
        lastReason_ = "libxess.dll is not staged in the plugin directory";
        return false;
    }
    api_->createContext = reinterpret_cast<decltype(api_->createContext)>(GetProcAddress(module_, "xessD3D12CreateContext"));
    api_->buildPipelines = reinterpret_cast<decltype(api_->buildPipelines)>(
        GetProcAddress(module_, "xessD3D12BuildPipelines"));
    api_->init = reinterpret_cast<decltype(api_->init)>(GetProcAddress(module_, "xessD3D12Init"));
    api_->execute = reinterpret_cast<decltype(api_->execute)>(GetProcAddress(module_, "xessD3D12Execute"));
    api_->destroyContext =
        reinterpret_cast<decltype(api_->destroyContext)>(GetProcAddress(module_, "xessDestroyContext"));
    api_->setVelocityScale =
        reinterpret_cast<decltype(api_->setVelocityScale)>(GetProcAddress(module_, "xessSetVelocityScale"));
    api_->getOptimalInputResolution = reinterpret_cast<decltype(api_->getOptimalInputResolution)>(
        GetProcAddress(module_, "xessGetOptimalInputResolution"));
    if (api_->createContext == nullptr || api_->init == nullptr || api_->execute == nullptr ||
        api_->destroyContext == nullptr || api_->getOptimalInputResolution == nullptr)
    {
        lastReason_ = "libxess.dll is missing its D3D12 entry points";
        return false;
    }
    runtimeVersion_ = ReadModuleFileVersion(L"libxess.dll");
    Log("XeSS runtime loaded version=0x%08x", runtimeVersion_);
    return true;
}

bool XessBackend::EnsureInterop()
{
    std::string reason;
    if (!interop_.Ensure(device_, reason))
    {
        Log("XeSS: DX12 interop unavailable: %s", reason.c_str());
        lastReason_ = reason;
        return false;
    }
    return true;
}

bool XessBackend::Attach(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (device == nullptr || context == nullptr) return false;
    if (device_ != device)
    {
        Release();
        device_ = device;
    }
    context_ = context;
    if (!context4_)
    {
        if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context4_))) || !context4_)
        {
            lastReason_ = "the game context is not an ID3D11DeviceContext4";
            return false;
        }
    }
    attached_ = true;
    return true;
}

bool XessBackend::Available()
{
    if (!attached_)
    {
        lastReason_ = "the XeSS backend is not attached";
        return false;
    }
    if (!LoadApi()) return false;
    if (!EnsureInterop()) return false;
    lastReason_.clear();
    return true;
}

int32_t XessBackend::QualitySetting(Preset preset) const
{
    switch (preset)
    {
        case Preset::Quality: return XESS_QUALITY_SETTING_QUALITY;
        case Preset::Balanced: return XESS_QUALITY_SETTING_BALANCED;
        case Preset::Performance: return XESS_QUALITY_SETTING_PERFORMANCE;
        case Preset::UltraPerformance: return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
        case Preset::NativeAA: return XESS_QUALITY_SETTING_AA;
    }
    return XESS_QUALITY_SETTING_QUALITY;
}

bool XessBackend::QueryModes(uint32_t outputWidth, uint32_t outputHeight, ModeRange (&ranges)[kModeCount],
                             uint32_t& runtimeVersion)
{
    runtimeVersion = runtimeVersion_;
    for (int i = 0; i < kModeCount; i++) ranges[i] = ModeRange{};
    if (!Available() || outputWidth == 0 || outputHeight == 0) return false;

    for (const QueryEntry& entry : queries_)
    {
        if (entry.width != outputWidth || entry.height != outputHeight) continue;
        for (int i = 0; i < kModeCount; i++) ranges[i] = entry.ranges[i];
        if (!entry.ok) lastReason_ = "XeSS reported no usable input resolution at this output size";
        return entry.ok;
    }

    // XeSS answers ratio questions through an initialised context. One tiny AA context is created
    // for this query and destroyed immediately; it is never used to present a frame.
    QueryEntry entry;
    entry.width = outputWidth;
    entry.height = outputHeight;
    xess_context_handle_t query = nullptr;
    if (api_->createContext(interop_.Device(), &query) != XESS_RESULT_SUCCESS || query == nullptr)
    {
        Log("XeSS: query context creation failed.");
        lastReason_ = "xessD3D12CreateContext failed on the bridged device";
        queries_.push_back(entry);
        return false;
    }
    xess_d3d12_init_params_t init{};
    init.outputResolution = {outputWidth, outputHeight};
    init.qualitySetting = XESS_QUALITY_SETTING_AA;
    init.initFlags = 0;
    if (api_->buildPipelines != nullptr) api_->buildPipelines(query, nullptr, true, 0);
    xess_result_t result = api_->init(query, &init);
    if (result != XESS_RESULT_SUCCESS)
    {
        Log("XeSS: query context init failed code=%d", static_cast<int>(result));
        lastReason_ = std::string("XeSS query initialisation failed: ") + ResultName(result);
        api_->destroyContext(query);
        queries_.push_back(entry);
        return false;
    }

    bool any = false;
    for (int i = 0; i < kModeCount; i++)
    {
        const auto preset = static_cast<Preset>(i + 1);
        xess_2d_t optimal{};
        xess_2d_t minimum{};
        xess_2d_t maximum{};
        result = api_->getOptimalInputResolution(query, &init.outputResolution,
            static_cast<xess_quality_settings_t>(QualitySetting(preset)), &optimal, &minimum, &maximum);
        if (result != XESS_RESULT_SUCCESS || optimal.x < 16 || optimal.y < 16) continue;
        ModeRange& range = ranges[i];
        range.supported = true;
        range.optimalWidth = optimal.x;
        range.optimalHeight = optimal.y;
        range.minWidth = minimum.x > 0 ? minimum.x : optimal.x;
        range.minHeight = minimum.y > 0 ? minimum.y : optimal.y;
        range.maxWidth = maximum.x > 0 ? maximum.x : outputWidth;
        range.maxHeight = maximum.y > 0 ? maximum.y : outputHeight;
        any = true;
    }
    api_->destroyContext(query);
    entry.ok = any;
    for (int i = 0; i < kModeCount; i++) entry.ranges[i] = ranges[i];
    queries_.push_back(entry);
    if (!any) lastReason_ = "XeSS reported no usable input resolution at this output size";
    else
        Log("XeSS mode ranges queried output=%ux%u quality=%ux%u", outputWidth, outputHeight,
            ranges[0].optimalWidth, ranges[0].optimalHeight);
    return any;
}

XessBackend::View* XessBackend::EnsureView(const ViewKey& key)
{
    for (View* view : views_)
        if (view->key == key) return view;
    if (views_.size() >= static_cast<size_t>(kMaxViews))
    {
        lastReason_ = "more XeSS views than the bridge supports";
        return nullptr;
    }
    auto* view = new View();
    view->key = key;
    views_.push_back(view);
    return view;
}

bool XessBackend::EnsureShared(SharedTexture& texture, uint32_t width, uint32_t height, DXGI_FORMAT format,
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
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &texture.d3d11)) || !texture.d3d11)
    {
        reason = "a shared DX12 interop texture could not be created";
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    HANDLE handle = nullptr;
    if (FAILED(texture.d3d11->QueryInterface(IID_PPV_ARGS(&resource))) || !resource ||
        FAILED(resource->GetSharedHandle(&handle)) || handle == nullptr ||
        FAILED(interop_.Device()->OpenSharedHandle(handle, IID_PPV_ARGS(&texture.shared.d3d12))) ||
        !texture.shared.d3d12)
    {
        reason = "a shared DX12 interop texture could not be opened on the bridge device";
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

bool XessBackend::EnsureContext(View& view, const EvaluationRequest& request)
{
    const int32_t quality = QualitySetting(request.preset);
    if (view.context != nullptr && view.inputWidth == request.inputWidth &&
        view.inputHeight == request.inputHeight && view.outputWidth == request.outputWidth &&
        view.outputHeight == request.outputHeight && view.quality == quality &&
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

    if (api_->createContext(interop_.Device(), &view.context) != XESS_RESULT_SUCCESS || view.context == nullptr)
    {
        Log("XeSS: view context creation failed camera=%d eye=%d", view.key.cameraId, view.key.eye);
        lastReason_ = "xessD3D12CreateContext failed on the bridged device";
        return false;
    }
    xess_d3d12_init_params_t init{};
    init.outputResolution = {request.outputWidth, request.outputHeight};
    init.qualitySetting = static_cast<xess_quality_settings_t>(quality);
    init.initFlags = request.reversedDepth ? XESS_INIT_FLAG_INVERTED_DEPTH : XESS_INIT_FLAG_NONE;
    if (api_->buildPipelines != nullptr) api_->buildPipelines(view.context, nullptr, true, init.initFlags);
    const xess_result_t result = api_->init(view.context, &init);
    if (result != XESS_RESULT_SUCCESS)
    {
        Log("XeSS: view context init failed camera=%d eye=%d code=%d", view.key.cameraId, view.key.eye,
            static_cast<int>(result));
        lastReason_ = std::string("xessD3D12Init failed: ") + ResultName(result);
        api_->destroyContext(view.context);
        view.context = nullptr;
        return false;
    }
    // Same raw-UV extraction and pixel-space conversion as the working FidelityFX DX12 paths.
    if (api_->setVelocityScale == nullptr)
    {
        lastReason_ = "the XeSS runtime is missing xessSetVelocityScale";
        Log("XeSS: xessSetVelocityScale export is unavailable; velocity conversion cannot be configured.");
        api_->destroyContext(view.context);
        view.context = nullptr;
        return false;
    }
    const xess_result_t velocity = api_->setVelocityScale(view.context, -static_cast<float>(request.inputWidth),
        -static_cast<float>(request.inputHeight));
    if (velocity != XESS_RESULT_SUCCESS)
    {
        lastReason_ = std::string("xessSetVelocityScale failed: ") + ResultName(velocity);
        Log("XeSS: xessSetVelocityScale failed camera=%d eye=%d code=%d", view.key.cameraId, view.key.eye,
            static_cast<int>(velocity));
        api_->destroyContext(view.context);
        view.context = nullptr;
        return false;
    }
    view.inputWidth = request.inputWidth;
    view.inputHeight = request.inputHeight;
    view.outputWidth = request.outputWidth;
    view.outputHeight = request.outputHeight;
    view.quality = quality;
    view.invertedDepth = request.reversedDepth;
    Log("XeSS context created camera=%d eye=%d render=%ux%u output=%ux%u quality=%d", view.key.cameraId,
        view.key.eye, view.inputWidth, view.inputHeight, view.outputWidth, view.outputHeight, quality);
    return true;
}

int32_t XessBackend::Evaluate(const ViewKey& key, const EvaluationRequest& request, bool& featureCreated)
{
    featureCreated = false;
    if (!Available()) return -1;
    View* view = EnsureView(key);
    if (view == nullptr) return -1;
    if (!EnsureContext(*view, request)) return -1;
    featureCreated = view->evaluations == 0;

    // D3D11 copies the scratch inputs into the shared textures, then hands the GPU to D3D12.
    context_->CopyResource(view->color.d3d11.Get(), request.color);
    context_->CopyResource(view->depth.d3d11.Get(), request.depth);
    context_->CopyResource(view->motion.d3d11.Get(), request.motion);
    std::string reason;
    uint64_t sourceFenceValue = 0;
    if (!interop_.SignalFromD3D11(context4_.Get(), reason, sourceFenceValue) || !interop_.Begin(reason))
    {
        Log("XeSS: interop submission start failed: %s", reason.c_str());
        lastReason_ = reason;
        return -1;
    }

    ID3D12GraphicsCommandList* list = interop_.List();
    Dx12Interop::Transition(list, view->color.shared, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Dx12Interop::Transition(list, view->depth.shared, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Dx12Interop::Transition(list, view->motion.shared, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Dx12Interop::Transition(list, view->output.shared, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    xess_d3d12_execute_params_t execute{};
    execute.pColorTexture = view->color.shared.d3d12.Get();
    execute.pVelocityTexture = view->motion.shared.d3d12.Get();
    execute.pDepthTexture = view->depth.shared.d3d12.Get();
    execute.pOutputTexture = view->output.shared.d3d12.Get();
    // Unity's jitter is already in render pixels. NGX and both FidelityFX backends receive the same
    // two values unchanged and reconstruct correctly, so XeSS takes the identical vector.
    execute.jitterOffsetX = request.jitterX;
    execute.jitterOffsetY = request.jitterY;
    execute.exposureScale = 1.0f;
    execute.resetHistory = request.reset || featureCreated ? 1u : 0u;
    execute.inputWidth = request.inputWidth;
    execute.inputHeight = request.inputHeight;
    execute.inputColorBase = {0, 0};
    execute.inputMotionVectorBase = {0, 0};
    execute.inputDepthBase = {0, 0};
    execute.outputColorBase = {0, 0};

    const xess_result_t result = api_->execute(view->context, list, &execute);
    Dx12Interop::Transition(list, view->output.shared, D3D12_RESOURCE_STATE_COMMON);
    Dx12Interop::Transition(list, view->motion.shared, D3D12_RESOURCE_STATE_COMMON);
    Dx12Interop::Transition(list, view->depth.shared, D3D12_RESOURCE_STATE_COMMON);
    Dx12Interop::Transition(list, view->color.shared, D3D12_RESOURCE_STATE_COMMON);

    if (result != XESS_RESULT_SUCCESS)
    {
        lastReason_ = std::string("xessD3D12Execute failed: ") + ResultName(result);
        Log("XeSS execute failed camera=%d eye=%d code=%d", key.cameraId, key.eye, static_cast<int>(result));
        return static_cast<int32_t>(result);
    }
    if (!interop_.Submit(reason, sourceFenceValue) || !interop_.WaitOnD3D11(context4_.Get(), reason))
    {
        Log("XeSS: interop submission completion failed: %s", reason.c_str());
        lastReason_ = reason;
        return -1;
    }
    // Ordered behind the shared-fence wait, so D3D11 sees the finished XeSS output.
    context_->CopyResource(request.output, view->output.d3d11.Get());

    view->evaluations++;
    if (view->evaluations == 1 || view->evaluations % 600 == 0)
        Log("XeSS execute ok camera=%d eye=%d render=%ux%u output=%ux%u reset=%d jitter=(%.4f,%.4f) "
            "velocityScale=(%d,%d)",
            key.cameraId, key.eye, request.inputWidth, request.inputHeight, request.outputWidth,
            request.outputHeight, execute.resetHistory, execute.jitterOffsetX, execute.jitterOffsetY,
            -static_cast<int>(request.inputWidth), -static_cast<int>(request.inputHeight));
    return 0;
}

void XessBackend::Destroy(View& view)
{
    if (view.context != nullptr)
    {
        if (api_ != nullptr && api_->destroyContext != nullptr) api_->destroyContext(view.context);
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

void XessBackend::Forget(const ViewKey& key)
{
    for (size_t i = 0; i < views_.size(); i++)
    {
        if (!(views_[i]->key == key)) continue;
        View* view = views_[i];
        views_.erase(views_.begin() + static_cast<std::ptrdiff_t>(i));
        // Same as FSR: a recycled stereo view may still be referenced by an in-flight submission.
        std::string reason;
        if (!interop_.WaitIdle(2000, reason))
        {
            Log("XeSS: view retire wait failed (%s); keeping camera=%d eye=%d until release.",
                reason.c_str(), key.cameraId, key.eye);
            retired_.push_back(view);
            return;
        }
        Destroy(*view);
        delete view;
        return;
    }
}

void XessBackend::Release()
{
    for (View* view : views_)
    {
        Destroy(*view);
        delete view;
    }
    views_.clear();
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
}
}
