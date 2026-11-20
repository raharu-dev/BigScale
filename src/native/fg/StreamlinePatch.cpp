#include "StreamlinePatch.h"

#include "../Log.h"

#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "MinHook.h"
#include "binary_patch.hpp"
#include "midpoint.hpp"

namespace bigscale::streamlinepatch
{
namespace
{
std::mutex g_mutex;
std::set<HMODULE> g_seen;
std::vector<mfgunlock::patching::Write> g_gateWrites;
std::vector<mfgunlock::patching::Write> g_ceilingWrites;
std::vector<mfgunlock::patching::Write> g_pacingWrites;
std::vector<mfgunlock::midpoint::Patch> g_midpointPatches;
void* g_midpointAllocation = nullptr;
std::string g_failure;
bool g_hooksInstalled = false;
bool g_midpointWanted = true;

using PFN_LoadLibraryW = HMODULE(WINAPI*)(LPCWSTR);
using PFN_LoadLibraryExW = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
PFN_LoadLibraryW g_loadLibraryW = nullptr;
PFN_LoadLibraryExW g_loadLibraryExW = nullptr;

std::wstring ModulePath(HMODULE module)
{
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return std::wstring();
    return std::wstring(buffer.data(), length);
}

std::wstring LowerBaseName(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    for (wchar_t& c : name) c = static_cast<wchar_t>(towlower(c));
    return name;
}

void Fail(const std::string& message)
{
    if (g_failure.empty()) g_failure = message;
    Log("Streamline patch: %s", message.c_str());
}

void PatchProvider(HMODULE module)
{
    const auto* profile = mfgunlock::binarypatch::ProfileFor(module, 0);
    if (profile == nullptr)
    {
        // Streamline maps auxiliary provider copies (the driver-store build among them) that no
        // profile covers. The provider refuses to run only when *no* copy could be gated.
        Log("Streamline patch: provider copy without an exact profile ignored");
        return;
    }
    if (!mfgunlock::binarypatch::Plan(module, *profile, g_gateWrites) ||
        !mfgunlock::binarypatch::Apply(module, g_gateWrites))
    {
        Fail("the architecture gates of the pinned DLSS-G provider did not match their profile");
        return;
    }
    Log("Streamline patch: architecture gates applied to the DLSS-G provider");
    if (!g_midpointWanted) return;
    std::string detail;
    if (!mfgunlock::midpoint::Apply(module, g_midpointPatches, g_midpointAllocation, detail))
    {
        Fail("the interpolation kernel could not be redirected: " +
            (detail.empty() ? std::string("no matching kernel profile") : detail));
        return;
    }
    Log("Streamline patch: interpolation kernel redirected for %zu descriptor slot(s)",
        g_midpointPatches.size());
}

void PatchStreamline(HMODULE module)
{
    const auto* ceiling = mfgunlock::binarypatch::ProfileFor(module, 2);
    if (ceiling == nullptr)
    {
        Fail("the Streamline DLSS-G plugin has no exact frame-ceiling profile");
        return;
    }
    // Only a compiled immediate below the wanted maximum is raised; a plugin already compiled for
    // five generated frames keeps its own bound.
    const uint8_t raised = g_midpointWanted ? 5 : 0;
    if (ceiling->count != 1 ||
        !mfgunlock::binarypatch::Plan(module, *ceiling, g_ceilingWrites, raised) ||
        !mfgunlock::binarypatch::Apply(module, g_ceilingWrites))
    {
        Fail("the Streamline frame ceiling did not match its profile");
        return;
    }
    const auto* immediate =
        reinterpret_cast<const uint8_t*>(module) + ceiling->sites[0].rva + 1;
    Log("Streamline patch: frame ceiling applied (compiled immediate %u generated frames)",
        static_cast<uint32_t>(*immediate));

    const auto* pacing = mfgunlock::binarypatch::ProfileFor(module, 1);
    if (pacing == nullptr)
    {
        Fail("the Streamline DLSS-G plugin has no exact software-pacing profile");
        return;
    }
    if (!mfgunlock::binarypatch::Plan(module, *pacing, g_pacingWrites) ||
        !mfgunlock::binarypatch::Apply(module, g_pacingWrites))
    {
        Fail("the Streamline software-pacing site did not match its profile");
        return;
    }
    Log("Streamline patch: software flip pacing applied");
}

void OnModuleLoaded(HMODULE module)
{
    if (module == nullptr) return;
    const std::wstring path = ModulePath(module);
    if (path.empty()) return;
    const std::wstring name = LowerBaseName(path);
    const bool provider = name == L"nvngx_dlssg.dll";
    const bool streamline = name == L"sl.dlss_g.dll";
    if (!provider && !streamline) return;

    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_seen.insert(module).second) return;
    Log("Streamline patch: module mapped %ls", path.c_str());
    if (provider)
        PatchProvider(module);
    else
        PatchStreamline(module);
}

HMODULE WINAPI DetourLoadLibraryW(LPCWSTR name)
{
    const HMODULE module = g_loadLibraryW(name);
    OnModuleLoaded(module);
    return module;
}

HMODULE WINAPI DetourLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    const HMODULE module = g_loadLibraryExW(name, file, flags);
    OnModuleLoaded(module);
    return module;
}
}  // namespace

bool Install(std::string& reason)
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_hooksInstalled) return true;
    // MinHook is process wide: the desktop presenter initializes it first, and a second caller must
    // reuse the existing state instead of failing.
    const MH_STATUS initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
    {
        reason = "the module-load detours could not be initialized";
        return false;
    }
    if (MH_CreateHookApi(L"kernel32.dll", "LoadLibraryW", reinterpret_cast<LPVOID>(&DetourLoadLibraryW),
            reinterpret_cast<LPVOID*>(&g_loadLibraryW)) != MH_OK ||
        MH_CreateHookApi(L"kernel32.dll", "LoadLibraryExW",
            reinterpret_cast<LPVOID>(&DetourLoadLibraryExW),
            reinterpret_cast<LPVOID*>(&g_loadLibraryExW)) != MH_OK)
    {
        reason = "the module-load detours could not be installed";
        return false;
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
    {
        reason = "the module-load detours could not be enabled";
        return false;
    }
    g_hooksInstalled = true;
    return true;
}

bool ApplyToModule(HMODULE module, bool midpoint, std::string& reason)
{
    g_midpointWanted = midpoint;
    OnModuleLoaded(module);
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_failure.empty())
    {
        reason = g_failure;
        return false;
    }
    return true;
}

bool ProviderGated()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return !g_gateWrites.empty();
}

std::string LastFailure()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_failure;
}
}  // namespace bigscale::streamlinepatch
