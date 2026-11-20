#include "XellLatency.h"

#include "../Log.h"

#include <windows.h>

#include <xell/xell.h>
#include <xell/xell_d3d12.h>

namespace bigscale
{
namespace
{
constexpr int32_t kXellSimulationStart = 0;
constexpr int32_t kXellSimulationEnd = 1;
constexpr int32_t kXellRenderSubmitStart = 2;
constexpr int32_t kXellRenderSubmitEnd = 3;
constexpr int32_t kXellPresentStart = 4;
constexpr int32_t kXellPresentEnd = 5;
constexpr int32_t kXellMarkerCount = 7;
static_assert(XELL_SIMULATION_START == kXellSimulationStart, "XeLL simulation start marker");
static_assert(XELL_SIMULATION_END == kXellSimulationEnd, "XeLL simulation end marker");
static_assert(XELL_RENDERSUBMIT_START == kXellRenderSubmitStart, "XeLL render submit start marker");
static_assert(XELL_RENDERSUBMIT_END == kXellRenderSubmitEnd, "XeLL render submit end marker");
static_assert(XELL_PRESENT_START == kXellPresentStart, "XeLL present start marker");
static_assert(XELL_PRESENT_END == kXellPresentEnd, "XeLL present end marker");
static_assert(XELL_MARKER_COUNT == kXellMarkerCount, "XeLL marker count");
}

bool XellLatency::Ensure(const std::wstring& runtimeDirectory, ID3D12Device* device, std::string& reason)
{
    if (context_ != nullptr) return true;
    if (device == nullptr)
    {
        reason_ = "the frame-generation device is not available for XeLL";
        reason = reason_;
        return false;
    }
    if (module_ == nullptr)
    {
        std::wstring path = runtimeDirectory;
        if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
        path += L"libxell.dll";
        module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (module_ == nullptr)
        {
            reason_ = "libxell.dll is not staged in the plugin directory";
            Log("XeLL: module load failed (error=%lu).", static_cast<unsigned long>(GetLastError()));
            reason = reason_;
            return false;
        }
    }
    api_.create = reinterpret_cast<decltype(api_.create)>(GetProcAddress(module_, "xellD3D12CreateContext"));
    api_.destroy = reinterpret_cast<decltype(api_.destroy)>(GetProcAddress(module_, "xellDestroyContext"));
    api_.setSleepMode = reinterpret_cast<decltype(api_.setSleepMode)>(GetProcAddress(module_, "xellSetSleepMode"));
    api_.sleep = reinterpret_cast<decltype(api_.sleep)>(GetProcAddress(module_, "xellSleep"));
    api_.addMarker = reinterpret_cast<decltype(api_.addMarker)>(GetProcAddress(module_, "xellAddMarkerData"));
    if (!api_.create || !api_.destroy || !api_.setSleepMode || !api_.sleep || !api_.addMarker)
    {
        reason_ = "libxell.dll is missing its latency-tracking entry points";
        reason = reason_;
        return false;
    }
    void* context = nullptr;
    const int32_t created = api_.create(device, &context);
    if (created != 0 || context == nullptr)
    {
        reason_ = "xellD3D12CreateContext failed: " + std::to_string(created);
        Log("XeLL: context creation failed code=%d", created);
        reason = reason_;
        return false;
    }
    context_ = context;
    xell_sleep_params_t parameters{};
    parameters.bLowLatencyMode = 1;
    parameters.minimumIntervalUs = 0;
    const int32_t sleepMode = api_.setSleepMode(context_, &parameters);
    if (sleepMode != 0)
    {
        // Latency reduction is an enhancement, not a precondition for generating frames.
        Log("XeLL: low-latency sleep mode was refused (%d); continuing with markers only.", sleepMode);
    }
    reason_.clear();
    Log("XeLL: latency context created.");
    return true;
}

void XellLatency::Release()
{
    if (context_ != nullptr && api_.destroy != nullptr) api_.destroy(context_);
    context_ = nullptr;
    if (module_ != nullptr)
    {
        FreeLibrary(module_);
        module_ = nullptr;
    }
    api_ = Api{};
}

void XellLatency::Sleep(uint32_t frameId)
{
    if (context_ == nullptr || api_.sleep == nullptr) return;
    api_.sleep(context_, frameId);
}

void XellLatency::Marker(uint32_t frameId, int32_t phase)
{
    if (context_ == nullptr || api_.addMarker == nullptr) return;
    if (phase < 0 || phase >= kXellMarkerCount) return;
    api_.addMarker(context_, frameId, phase);
}
}
