// NGX caller-path forwarder.
//
// The user-supplied DLSS neural-rendering runtime refuses a caller whose module path does not
// contain `nvngx.dll`. OptiScaler solves that by patching the runtime's import table; this project
// does not. Instead the bridge exports five NGX entry points and is hard-linked (or copied) to
// `<data>\dlss5\nvngx.dll`; the runtime therefore sees a caller named `nvngx.dll`. Those exports load
// the user's real `nvngx_dlssnr.dll` from the same directory (or BIGSCALE_NVNGX_REAL) and forward the
// call unchanged. No shipped binary is added and no process import table is modified.
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace
{
HMODULE g_real = nullptr;
SRWLOCK g_lock = SRWLOCK_INIT;

HMODULE RealModule()
{
    AcquireSRWLockExclusive(&g_lock);
    if (g_real == nullptr)
    {
        wchar_t path[MAX_PATH * 4]{};
        DWORD length = GetEnvironmentVariableW(L"BIGSCALE_NVNGX_REAL", path, MAX_PATH * 4);
        if (length > 0 && length < MAX_PATH * 4)
        {
            g_real = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }
        if (g_real == nullptr)
        {
            HMODULE self = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&RealModule), &self) &&
                self != nullptr)
            {
                length = GetModuleFileNameW(self, path, MAX_PATH * 4);
                wchar_t* slash = length > 0 && length < MAX_PATH * 4 ? wcsrchr(path, L'\\') : nullptr;
                if (slash != nullptr)
                {
                    wcscpy_s(slash + 1, MAX_PATH * 4 - static_cast<size_t>(slash + 1 - path),
                        L"nvngx_dlssnr.dll");
                    g_real = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                }
            }
        }
        if (g_real == nullptr) g_real = LoadLibraryW(L"nvngx_dlssnr.dll");
    }
    ReleaseSRWLockExclusive(&g_lock);
    return g_real;
}

FARPROC Resolve(const char* name)
{
    HMODULE module = RealModule();
    return module != nullptr ? GetProcAddress(module, name) : nullptr;
}
}

extern "C" int __cdecl BigScaleNvngxInitExt(unsigned long long applicationId, const wchar_t* path,
    void* device, unsigned int version, void* parameters)
{
    using Fn = int(__cdecl*)(unsigned long long, const wchar_t*, void*, unsigned int, void*);
    auto fn = reinterpret_cast<Fn>(Resolve("NVSDK_NGX_D3D12_Init_Ext"));
    return fn != nullptr ? fn(applicationId, path, device, version, parameters) : 0xBAD00000;
}

extern "C" int __cdecl BigScaleNvngxCreateFeature(void* commandList, int feature, void* parameters,
    void** handle)
{
    using Fn = int(__cdecl*)(void*, int, void*, void**);
    auto fn = reinterpret_cast<Fn>(Resolve("NVSDK_NGX_D3D12_CreateFeature"));
    return fn != nullptr ? fn(commandList, feature, parameters, handle) : 0xBAD00000;
}

extern "C" int __cdecl BigScaleNvngxEvaluateFeature(void* commandList, const void* handle, void* parameters,
    void* callback)
{
    using Fn = int(__cdecl*)(void*, const void*, void*, void*);
    auto fn = reinterpret_cast<Fn>(Resolve("NVSDK_NGX_D3D12_EvaluateFeature"));
    return fn != nullptr ? fn(commandList, handle, parameters, callback) : 0xBAD00000;
}

extern "C" int __cdecl BigScaleNvngxReleaseFeature(void* handle)
{
    using Fn = int(__cdecl*)(void*);
    auto fn = reinterpret_cast<Fn>(Resolve("NVSDK_NGX_D3D12_ReleaseFeature"));
    return fn != nullptr ? fn(handle) : 0xBAD00000;
}

extern "C" int __cdecl BigScaleNvngxShutdown1(void* device)
{
    using Fn = int(__cdecl*)(void*);
    auto fn = reinterpret_cast<Fn>(Resolve("NVSDK_NGX_D3D12_Shutdown1"));
    return fn != nullptr ? fn(device) : 0xBAD00000;
}

// Called by DlssNrRuntime before the alias module is unloaded. Freeing the real runtime from inside
// DllMain would run under the loader lock, so the owner does it explicitly instead.
extern "C" void __cdecl BigScaleNvngxReleaseReal()
{
    AcquireSRWLockExclusive(&g_lock);
    if (g_real != nullptr)
    {
        FreeLibrary(g_real);
        g_real = nullptr;
    }
    ReleaseSRWLockExclusive(&g_lock);
}
