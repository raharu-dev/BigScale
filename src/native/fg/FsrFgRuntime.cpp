#include "FsrFgRuntime.h"

#include "../Log.h"
#include "../providers/FsrDx12Backend.h"

#include <cstdio>
#include <vector>

namespace bigscale
{
struct FsrFgRuntime::DescriptorStorage
{
    ffxCreateContextDescFrameGeneration create{};
    ffxCreateContextDescFrameGenerationVersion version{};
    ffxCreateBackendDX12Desc backend{};
};

FsrFgRuntime::~FsrFgRuntime()
{
    for (auto& entry : storage_) delete entry.second;
    storage_.clear();
    Unload();
}

bool FsrFgRuntime::Load(const std::wstring& runtimeDirectory, std::string& reason)
{
    if (module_ != nullptr) return true;
    std::wstring path = runtimeDirectory;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
    path += L"amd_fidelityfx_framegeneration_dx12.dll";
    module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module_ == nullptr)
    {
        reason = "amd_fidelityfx_framegeneration_dx12.dll is not staged in the plugin directory";
        Log("FSR-FG: module load failed (error=%lu).", static_cast<unsigned long>(GetLastError()));
        return false;
    }
    api_.createContext = reinterpret_cast<decltype(api_.createContext)>(GetProcAddress(module_, "ffxCreateContext"));
    api_.destroyContext = reinterpret_cast<decltype(api_.destroyContext)>(GetProcAddress(module_, "ffxDestroyContext"));
    api_.configure = reinterpret_cast<decltype(api_.configure)>(GetProcAddress(module_, "ffxConfigure"));
    api_.query = reinterpret_cast<decltype(api_.query)>(GetProcAddress(module_, "ffxQuery"));
    api_.dispatch = reinterpret_cast<decltype(api_.dispatch)>(GetProcAddress(module_, "ffxDispatch"));
    if (!api_.createContext || !api_.destroyContext || !api_.configure || !api_.query || !api_.dispatch)
    {
        reason = "the FidelityFX frame-generation runtime is missing its API entry points";
        Unload();
        return false;
    }
    return true;
}

bool FsrFgRuntime::Select(ID3D12Device* device, std::string& reason)
{
    if (!Loaded())
    {
        reason = "the FidelityFX frame-generation runtime is not loaded";
        return false;
    }
    ffxQueryDescGetVersions query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    query.device = device;
    uint64_t count = 0;
    query.outputCount = &count;
    if (api_.query(nullptr, &query.header) != FFX_API_RETURN_OK || count == 0)
    {
        reason = "no FidelityFX frame-generation provider is installed";
        return false;
    }
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    uint64_t capacity = count;
    query.outputCount = &capacity;
    query.versionIds = ids.data();
    query.versionNames = names.data();
    if (api_.query(nullptr, &query.header) != FFX_API_RETURN_OK || capacity == 0)
    {
        reason = "the FidelityFX frame-generation version list could not be read";
        return false;
    }
    for (uint64_t i = 0; i < capacity; i++)
        Log("FSR-FG: offered provider id=0x%llx name=%s", static_cast<unsigned long long>(ids[i]),
            names[i] != nullptr ? names[i] : "?");
    uint64_t selected = 0;
    std::string selectedName;
    if (!SelectFsrProviderVersion(ids.data(), names.data(), static_cast<size_t>(capacity), 3, selected,
            selectedName))
    {
        reason = "no AMD frame-generation provider for major version 3 was found";
        return false;
    }
    versionId_ = selected;
    version_ = static_cast<uint32_t>(selected) & 0x7fffffffu;
    Log("FSR-FG: selected provider %s id=0x%llx version=%u.%u.%u", selectedName.c_str(),
        static_cast<unsigned long long>(selected), version_ >> 22, (version_ >> 12) & 0x3ffu,
        version_ & 0xfffu);
    return true;
}

bool FsrFgRuntime::CreateContext(ID3D12Device* device, uint32_t width, uint32_t height, bool reversedDepth,
                                 void** context, std::string& reason)
{
    if (!Loaded() || !api_.createContext || !api_.configure || !api_.query)
    {
        reason = "the FidelityFX frame-generation runtime is not loaded";
        return false;
    }
    if (context == nullptr)
    {
        reason = "no destination for the frame-generation context was provided";
        return false;
    }
    *context = nullptr;
    auto* storage = new DescriptorStorage();

    storage->create = ffxCreateContextDescFrameGeneration{};
    storage->create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    // Create-time flags come only from FfxApiCreateContextFramegenerationFlags. The no-swapchain
    // bit is a *configure/dispatch* flag; its bit 3 collides with depth-inverted and must never be
    // written here.
    storage->create.flags = reversedDepth ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0;
    storage->create.displaySize = {width, height};
    storage->create.maxRenderSize = {width, height};
    storage->create.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;

    storage->backend = ffxCreateBackendDX12Desc{};
    storage->backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    storage->backend.device = device;

    storage->version = ffxCreateContextDescFrameGenerationVersion{};
    storage->version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    storage->version.version = version_;

    storage->create.header.pNext = &storage->backend.header;
    storage->backend.header.pNext = &storage->version.header;
    storage->version.header.pNext = nullptr;

    const ffxReturnCode_t created = api_.createContext(context, &storage->create.header, nullptr);
    if (created != FFX_API_RETURN_OK || *context == nullptr)
    {
        reason = "ffxCreateContext failed for the frame-generation context";
        Log("FSR-FG: context creation failed code=%u", static_cast<unsigned>(created));
        delete storage;
        *context = nullptr;
        return false;
    }
    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (api_.query(context, &provider.header) != FFX_API_RETURN_OK || provider.versionId != versionId_)
    {
        reason = "the created frame-generation context is not the selected provider";
        Log("FSR-FG: provider mismatch selected=0x%llx actual=0x%llx", static_cast<unsigned long long>(versionId_),
            static_cast<unsigned long long>(provider.versionId));
        if (api_.destroyContext != nullptr) api_.destroyContext(context, nullptr);
        delete storage;
        *context = nullptr;
        return false;
    }
    ffxConfigureDescFrameGeneration configure{};
    configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    configure.swapChain = nullptr;
    configure.frameGenerationEnabled = true;
    configure.allowAsyncWorkloads = false;
    configure.HUDLessColor = ffxApiGetResourceDX12(nullptr);
    configure.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    configure.generationRect = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
    configure.frameID = 0;
    const ffxReturnCode_t configured = api_.configure(context, &configure.header);
    if (configured != FFX_API_RETURN_OK)
    {
        reason = "ffxConfigure failed for the frame-generation context";
        Log("FSR-FG: configure failed code=%u", static_cast<unsigned>(configured));
        if (api_.destroyContext != nullptr) api_.destroyContext(context, nullptr);
        delete storage;
        *context = nullptr;
        return false;
    }
    // The chained create descriptors must stay alive until this context is destroyed; the map keeps
    // one block per context so a second eye's creation cannot invalidate the first context's chain.
    storage_[*context] = storage;
    return true;
}

bool FsrFgRuntime::DestroyContext(void** context)
{
    if (context == nullptr || *context == nullptr) return true;
    if (api_.destroyContext == nullptr)
    {
        Log("FSR-FG: context destroy failed: the FidelityFX context-destroy entry point is unavailable");
        return false;
    }
    const ffxReturnCode_t destroyed = api_.destroyContext(context, nullptr);
    if (destroyed != FFX_API_RETURN_OK)
    {
        Log("FSR-FG: context destroy failed code=%u", static_cast<unsigned>(destroyed));
        return false;
    }
    const auto entry = storage_.find(*context);
    if (entry != storage_.end())
    {
        delete entry->second;
        storage_.erase(entry);
    }
    *context = nullptr;
    return true;
}

void FsrFgRuntime::Unload()
{
    for (auto& entry : storage_) delete entry.second;
    storage_.clear();
    if (module_ != nullptr)
    {
        FreeLibrary(module_);
        module_ = nullptr;
    }
    api_ = Api{};
    versionId_ = 0;
    version_ = 0;
}
}
