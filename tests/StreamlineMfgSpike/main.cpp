// Streamline multi-frame-generation feasibility spike. Test-only: measures whether an Ada GPU
// through Streamline emits frames 2..N when the MFGAdaUnlock profiled sites are applied.
//
// A vertical white bar moves 24 pixels per rendered frame across a black window. Streamline drives
// DLSS-G on the swapchain it hooked; the harness reads back the presented desktop composition and
// checks that each rendered-frame interval exposes the expected number of distinct bar positions at
// the expected spacing. That distinguishes "more presents" from "more real frames in time".

#include <windows.h>
#include <wrl/client.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "MinHook.h"

#include "sl.h"
#include "sl_dlss_g.h"
#include "sl_pcl.h"
#include "sl_reflex.h"

#include "binary_patch.hpp"
#include "midpoint.hpp"

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
constexpr uint32_t kBarWidth = 64;
constexpr int kBarStep = 24;
constexpr int kBarSpan = static_cast<int>(kWidth - kBarWidth);
constexpr uint32_t kBufferCount = 3;
constexpr int kSteadyStateFrame = 180;

struct Options {
  uint32_t multiplier = 4;
  uint32_t frames = 900;
  bool unlock = true;
  bool midpoint = true;
  bool capture = true;
};

Options g_options;

// Patches are owned for process lifetime; a spike has no restore path.
std::mutex g_patch_mutex;
std::set<HMODULE> g_patched;
std::atomic<bool> g_patch_failure{false};
std::vector<mfgunlock::patching::Write> g_gate_writes;
std::vector<mfgunlock::patching::Write> g_ceiling_writes;
std::vector<mfgunlock::patching::Write> g_pacing_writes;
std::vector<mfgunlock::midpoint::Patch> g_midpoint_patches;
void* g_midpoint_allocation = nullptr;

void Log(const char* format, ...) {
  va_list args;
  va_start(args, format);
  std::vfprintf(stdout, format, args);
  va_end(args);
  std::fflush(stdout);
}

void OnStreamlineLog(sl::LogType type, const char* message) {
  static const char* kNames[] = {"info", "warn", "error"};
  const auto index = static_cast<uint32_t>(type);
  Log("sl: [%s] %s\n", index < 3 ? kNames[index] : "log", message ? message : "");
}

std::wstring ModuleFileName(HMODULE module) {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return std::wstring();
  return std::wstring(buffer.data(), length);
}

std::wstring BaseName(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring Lower(std::wstring value) {
  for (auto& c : value) c = static_cast<wchar_t>(towlower(c));
  return value;
}

// ---------------------------------------------------------------------------------------------
// Profiled patch application, mirrored from MFGAdaUnlock's addon_streamline.inl.
// ---------------------------------------------------------------------------------------------
void PatchProvider(HMODULE module) {
  const auto* profile = mfgunlock::binarypatch::ProfileFor(module, 0);
  if (!profile) {
    // Streamline maps auxiliary provider copies (the driver-store build among them) that no profile
    // covers. They do not block the spike: the verdict requires that at least one provider copy
    // received the gates, which is what makes the capability report multi-frame.
    Log("provider: no exact profile (ignored)\n");
    return;
  }
  if (!mfgunlock::binarypatch::Plan(module, *profile, g_gate_writes) ||
      !mfgunlock::binarypatch::Apply(module, g_gate_writes)) {
    Log("provider: gate apply failed\n");
    g_patch_failure.store(true);
    return;
  }
  Log("provider: gates applied\n");
  if (!g_options.midpoint) return;
  std::string detail;
  if (!mfgunlock::midpoint::Apply(module, g_midpoint_patches, g_midpoint_allocation, detail)) {
    Log("provider: midpoint failed: %s\n",
        detail.empty() ? "no matching kernel profile" : detail.c_str());
    g_patch_failure.store(true);
    return;
  }
  Log("provider: midpoint redirected %zu slot(s)\n", g_midpoint_patches.size());
}

void PatchStreamline(HMODULE module) {
  const auto* ceiling = mfgunlock::binarypatch::ProfileFor(module, 2);
  if (!ceiling) {
    Log("streamline: ceiling missing\n");
    g_patch_failure.store(true);
  } else if (ceiling->count != 1 ||
             !mfgunlock::binarypatch::Plan(module, *ceiling, g_ceiling_writes,
                                           g_options.multiplier > 4 ? 5 : 0) ||
             !mfgunlock::binarypatch::Apply(module, g_ceiling_writes)) {
    Log("streamline: ceiling failed\n");
    g_patch_failure.store(true);
  } else {
    // The compiled immediate is the hard bound on generated frames for this plugin build.
    const auto* immediate = reinterpret_cast<const uint8_t*>(module) + ceiling->sites[0].rva + 1;
    Log("streamline: ceiling applied immediate=%u\n", static_cast<uint32_t>(*immediate));
  }

  const auto* pacing = mfgunlock::binarypatch::ProfileFor(module, 1);
  if (!pacing) {
    Log("streamline: pacing missing\n");
    g_patch_failure.store(true);
  } else if (!mfgunlock::binarypatch::Plan(module, *pacing, g_pacing_writes) ||
             !mfgunlock::binarypatch::Apply(module, g_pacing_writes)) {
    Log("streamline: pacing failed\n");
    g_patch_failure.store(true);
  } else {
    Log("streamline: pacing applied\n");
  }
}

void OnModuleLoaded(HMODULE module) {
  if (!module) return;
  const std::wstring path = ModuleFileName(module);
  if (path.empty()) return;
  const std::wstring name = Lower(BaseName(path));
  const bool provider = name == L"nvngx_dlssg.dll";
  const bool streamline = name == L"sl.dlss_g.dll";
  if (!provider && !streamline) return;

  std::lock_guard<std::mutex> guard(g_patch_mutex);
  if (!g_patched.insert(module).second) return;
  Log("patch: %ls\n", path.c_str());
  if (provider) {
    PatchProvider(module);
  } else {
    PatchStreamline(module);
  }
}

using PFN_LoadLibraryW = HMODULE(WINAPI*)(LPCWSTR);
using PFN_LoadLibraryExW = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
PFN_LoadLibraryW g_load_library_w = nullptr;
PFN_LoadLibraryExW g_load_library_ex_w = nullptr;

HMODULE WINAPI DetourLoadLibraryW(LPCWSTR name) {
  const HMODULE module = g_load_library_w(name);
  OnModuleLoaded(module);
  return module;
}

HMODULE WINAPI DetourLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
  const HMODULE module = g_load_library_ex_w(name, file, flags);
  OnModuleLoaded(module);
  return module;
}

bool InstallLoadHooks() {
  if (MH_Initialize() != MH_OK) {
    Log("hooks: MH_Initialize failed\n");
    return false;
  }
  if (MH_CreateHookApi(L"kernel32.dll", "LoadLibraryW",
                       reinterpret_cast<LPVOID>(&DetourLoadLibraryW),
                       reinterpret_cast<LPVOID*>(&g_load_library_w)) != MH_OK ||
      MH_CreateHookApi(L"kernel32.dll", "LoadLibraryExW",
                       reinterpret_cast<LPVOID>(&DetourLoadLibraryExW),
                       reinterpret_cast<LPVOID*>(&g_load_library_ex_w)) != MH_OK) {
    Log("hooks: MH_CreateHookApi failed\n");
    return false;
  }
  if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
    Log("hooks: MH_EnableHook failed\n");
    return false;
  }
  // Nothing this harness patches can be loaded yet: InstallLoadHooks runs before slInit.
  return true;
}

// ---------------------------------------------------------------------------------------------
// Scene helpers.
// ---------------------------------------------------------------------------------------------
void MakeIdentity(sl::float4x4& matrix) {
  for (int r = 0; r < 4; ++r) {
    matrix.row[r].x = r == 0 ? 1.0f : 0.0f;
    matrix.row[r].y = r == 1 ? 1.0f : 0.0f;
    matrix.row[r].z = r == 2 ? 1.0f : 0.0f;
    matrix.row[r].w = r == 3 ? 1.0f : 0.0f;
  }
}

D3D12_RESOURCE_DESC Tex2DDesc(DXGI_FORMAT format, uint32_t width, uint32_t height,
                              D3D12_RESOURCE_FLAGS flags) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = flags;
  return desc;
}

D3D12_HEAP_PROPERTIES DefaultHeap() {
  return D3D12_HEAP_PROPERTIES{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                               D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
}

D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                        D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  return barrier;
}

// Reads the presented desktop along the window's middle row, so the measured positions are what the
// compositor actually showed rather than what the app enqueued.
struct DesktopCapture {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<IDXGIOutputDuplication> duplication;
  ComPtr<ID3D11Texture2D> staging;
  UINT left{};
  UINT row{};
  bool ready{};
  std::atomic<uint32_t> timeouts{0};
  std::atomic<uint32_t> errors{0};
  std::atomic<uint32_t> frames{0};
  std::atomic<uint32_t> blanks{0};

  bool Init(IDXGIFactory1* factory) {
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(0, &adapter))) return false;
    ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(0, &output))) return false;
    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output.As(&output1))) return false;

    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    if (!d3d11) return false;
    using PFN_D3D11CreateDevice = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                   const D3D_FEATURE_LEVEL*, UINT, UINT,
                                                   ID3D11Device**, D3D_FEATURE_LEVEL*,
                                                   ID3D11DeviceContext**);
    const auto create =
        reinterpret_cast<PFN_D3D11CreateDevice>(GetProcAddress(d3d11, "D3D11CreateDevice"));
    if (!create) return false;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(create(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 1,
                      D3D11_SDK_VERSION, device.GetAddressOf(), nullptr,
                      context.GetAddressOf()))) {
      return false;
    }
    if (FAILED(output1->DuplicateOutput(device.Get(), duplication.GetAddressOf()))) return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) return false;
    ready = true;
    return true;
  }

  // Blocks until a composition frame arrives. Returns the bar's left edge in window coordinates, -1
  // on timeout, or -2 when duplication is lost. The desktop compositor latches a window at its own
  // cadence, so sampling must run continuously rather than once per rendered frame.
  int Sample() {
    if (!ready) return -2;
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    const HRESULT acquired = duplication->AcquireNextFrame(50, &info, resource.GetAddressOf());
    if (acquired == DXGI_ERROR_WAIT_TIMEOUT) {
      timeouts.fetch_add(1);
      return -1;
    }
    if (FAILED(acquired)) {
      errors.fetch_add(1);
      return -2;
    }
    frames.fetch_add(1);
    int position = -1;
    ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(resource.As(&texture))) {
      const D3D11_BOX box{left, row, 0, left + kWidth, row + 1, 1};
      context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture.Get(), 0, &box);
      D3D11_MAPPED_SUBRESOURCE mapped{};
      if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        const auto* pixels = static_cast<const uint8_t*>(mapped.pData);
        for (uint32_t x = 0; x < kWidth; ++x) {
          const uint8_t* p = pixels + static_cast<size_t>(x) * 4;
          if (p[0] + p[1] + p[2] > 3 * 16) {
            position = static_cast<int>(x);
            break;
          }
        }
        context->Unmap(staging.Get(), 0);
      }
    }
    duplication->ReleaseFrame();
    if (position < 0) blanks.fetch_add(1);
    return position;
  }
};

struct IntervalStats {
  size_t intervals = 0;
  int distinct_median = 0;
  double step_median = 0.0;
  std::vector<int> distinct_histogram;
  std::vector<int> delta_histogram;
};

IntervalStats AnalysePositions(const std::vector<int>& positions) {
  std::vector<std::vector<int>> groups;
  std::vector<int> current;
  for (const int position : positions) {
    if (current.empty()) {
      current.push_back(position);
      continue;
    }
    // A wrap or the next 24-pixel step starts a new rendered-frame interval.
    if (position < current.back() - 600 || position >= current.front() + kBarStep) {
      groups.push_back(current);
      current.assign(1, position);
      continue;
    }
    current.push_back(position);
  }
  if (!current.empty()) groups.push_back(current);

  std::vector<int> distinct_counts;
  std::vector<double> steps;
  for (const auto& group : groups) {
    std::vector<int> distinct;
    for (const int position : group) {
      if (distinct.empty() || distinct.back() != position) distinct.push_back(position);
    }
    distinct_counts.push_back(static_cast<int>(distinct.size()));
    for (size_t i = 1; i < distinct.size(); ++i) {
      const int delta = distinct[i] - distinct[i - 1];
      if (delta > 0) steps.push_back(static_cast<double>(delta));
    }
  }

  IntervalStats stats;
  stats.intervals = groups.size();
  if (!distinct_counts.empty()) {
    std::sort(distinct_counts.begin(), distinct_counts.end());
    stats.distinct_median = distinct_counts[distinct_counts.size() / 2];
  }
  stats.distinct_histogram.assign(9, 0);
  for (const int count : distinct_counts) {
    if (count >= 0 && count <= 8) ++stats.distinct_histogram[count];
  }
  stats.delta_histogram.assign(25, 0);
  for (const double step : steps) {
    const int bucket = static_cast<int>(step + 0.5);
    if (bucket >= 0 && bucket <= 24) ++stats.delta_histogram[bucket];
  }
  if (!steps.empty()) {
    std::sort(steps.begin(), steps.end());
    stats.step_median = steps[steps.size() / 2];
  }
  return stats;
}

bool ParseOptions(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    auto next = [&](uint32_t& out) {
      if (i + 1 >= argc) return false;
      out = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
      return out != 0;
    };
    if (argument == "--multiplier") {
      if (!next(g_options.multiplier)) return false;
    } else if (argument == "--frames") {
      if (!next(g_options.frames)) return false;
    } else if (argument == "--no-unlock") {
      g_options.unlock = false;
    } else if (argument == "--no-midpoint") {
      g_options.midpoint = false;
    } else if (argument == "--no-capture") {
      g_options.capture = false;
    } else {
      return false;
    }
  }
  return g_options.multiplier >= 2 && g_options.multiplier <= 6 && g_options.frames >= 200;
}

struct Scene {
  ComPtr<ID3D12Resource> motion_vectors;
  ComPtr<ID3D12Resource> depth;
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  ComPtr<ID3D12DescriptorHeap> dsv_heap;
  std::vector<ComPtr<ID3D12Resource>> back_buffers;
  UINT rtv_size = 0;
};

}  // namespace

int main(int argc, char** argv) {
  if (!ParseOptions(argc, argv)) {
    Log("usage: StreamlineMfgSpike [--multiplier 2..6] [--frames N] [--no-unlock] "
        "[--no-midpoint] [--no-capture]\n");
    return 2;
  }
  const uint32_t multiplier = g_options.multiplier;
  Log("run multiplier=%u frames=%u unlock=%d midpoint=%d capture=%d\n", multiplier,
      g_options.frames, g_options.unlock ? 1 : 0, g_options.midpoint ? 1 : 0,
      g_options.capture ? 1 : 0);

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  const std::wstring self_path = ModuleFileName(nullptr);
  if (self_path.empty()) {
    Log("VERDICT BLOCKED: GetModuleFileNameW failed\n");
    return 3;
  }
  const std::wstring exe_dir = self_path.substr(0, self_path.find_last_of(L"\\/"));

  if (g_options.unlock && !InstallLoadHooks()) {
    Log("VERDICT BLOCKED: hooks failed\n");
    return 3;
  }

  sl::Preferences preferences{};
  preferences.renderAPI = sl::RenderAPI::eD3D12;
  preferences.engine = sl::EngineType::eUnity;
  preferences.engineVersion = "6000.3.17f1";
  preferences.projectId = "dce2ec10-c62e-4ec2-a24e-7368ff734a44";
  preferences.logLevel = sl::LogLevel::eVerbose;
  preferences.logMessageCallback = &OnStreamlineLog;
  const wchar_t* plugin_paths[] = {exe_dir.c_str()};
  preferences.pathsToPlugins = plugin_paths;
  preferences.numPathsToPlugins = 1;
  preferences.pathToLogsAndData = exe_dir.c_str();
  // Frame-based tagging is required by slSetTagForFrame. OTA stays off so the profiled plugin is the
  // one that runs.
  preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                      sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
  preferences.featuresToLoad = features;
  preferences.numFeaturesToLoad = 3;

  sl::Result result = slInit(preferences, sl::kSDKVersion);
  if (result != sl::Result::eOk) {
    Log("VERDICT BLOCKED: slInit %d\n", static_cast<int>(result));
    return 3;
  }
  Log("streamline: sdk=%d.%d.%d\n", SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);

  ComPtr<IDXGIFactory2> factory;
  HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf()));
  if (FAILED(hr)) {
    Log("VERDICT BLOCKED: CreateDXGIFactory2 0x%08lx\n", static_cast<unsigned long>(hr));
    return 3;
  }
  ComPtr<IDXGIAdapter1> adapter;
  factory->EnumAdapters1(0, adapter.GetAddressOf());
  DXGI_ADAPTER_DESC1 adapter_desc{};
  if (adapter) adapter->GetDesc1(&adapter_desc);
  Log("adapter: %ls\n", adapter_desc.Description);

  ComPtr<ID3D12Device> device;
  hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                         IID_PPV_ARGS(device.GetAddressOf()));
  if (FAILED(hr)) {
    Log("VERDICT BLOCKED: D3D12CreateDevice 0x%08lx\n", static_cast<unsigned long>(hr));
    return 3;
  }
  // Required even with the interposer: Streamline only tracks a device the host hands it explicitly.
  result = slSetD3DDevice(device.Get());
  if (result != sl::Result::eOk) {
    Log("VERDICT BLOCKED: slSetD3DDevice %d\n", static_cast<int>(result));
    return 3;
  }

  sl::AdapterInfo adapter_info{};
  adapter_info.deviceLUID = reinterpret_cast<uint8_t*>(&adapter_desc.AdapterLuid);
  adapter_info.deviceLUIDSizeInBytes = sizeof(LUID);
  result = slIsFeatureSupported(sl::kFeatureDLSS_G, adapter_info);
  if (result != sl::Result::eOk) {
    Log("VERDICT BLOCKED: DLSS-G unsupported %d\n", static_cast<int>(result));
    return 3;
  }
  sl::FeatureRequirements requirements{};
  if (slGetFeatureRequirements(sl::kFeatureDLSS_G, requirements) == sl::Result::eOk) {
    Log("requirements: hardwareSchedulingRequired=%d\n",
        (requirements.flags & sl::FeatureRequirementFlags::eHardwareSchedulingRequired) ? 1 : 0);
  }

  DEVMODEW display_mode{};
  display_mode.dmSize = sizeof(display_mode);
  const DWORD refresh = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &display_mode)
                            ? display_mode.dmDisplayFrequency
                            : 0;
  const uint32_t app_fps = std::max<uint32_t>(
      15, static_cast<uint32_t>(0.9 * static_cast<double>(refresh) / multiplier));
  Log("refresh: %lu Hz, app fps cap %u\n", static_cast<unsigned long>(refresh), app_fps);

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = DefWindowProcW;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = L"StreamlineMfgSpikeWindow";
  RegisterClassExW(&window_class);
  const HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                      window_class.lpszClassName, L"Streamline MFG spike",
                                      WS_POPUP | WS_VISIBLE, 100, 100, static_cast<int>(kWidth),
                                      static_cast<int>(kHeight), nullptr, nullptr,
                                      window_class.hInstance, nullptr);
  if (!window) {
    Log("VERDICT BLOCKED: CreateWindowExW failed\n");
    return 3;
  }
  RECT window_rect{};
  GetWindowRect(window, &window_rect);
  ShowWindow(window, SW_SHOWNOACTIVATE);

  DXGI_SWAP_CHAIN_DESC1 swap_chain_desc{};
  swap_chain_desc.Width = kWidth;
  swap_chain_desc.Height = kHeight;
  swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  swap_chain_desc.SampleDesc.Count = 1;
  swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_chain_desc.BufferCount = kBufferCount;
  swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  swap_chain_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  ComPtr<IDXGIFactory5> factory5;
  BOOL tearing_supported = FALSE;
  if (SUCCEEDED(factory.As(&factory5))) {
    factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing_supported,
                                  sizeof(tearing_supported));
  }
  if (tearing_supported) swap_chain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComPtr<ID3D12CommandQueue> queue;
  if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue.GetAddressOf())))) {
    Log("VERDICT BLOCKED: CreateCommandQueue failed\n");
    return 3;
  }

  ComPtr<IDXGISwapChain1> swap_chain1;
  hr = factory->CreateSwapChainForHwnd(queue.Get(), window, &swap_chain_desc, nullptr, nullptr,
                                       swap_chain1.GetAddressOf());
  if (FAILED(hr)) {
    Log("VERDICT BLOCKED: CreateSwapChainForHwnd 0x%08lx\n", static_cast<unsigned long>(hr));
    return 3;
  }
  factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
  ComPtr<IDXGISwapChain3> swap_chain;
  swap_chain1.As(&swap_chain);

  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> command_list;
  device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                 IID_PPV_ARGS(allocator.GetAddressOf()));
  device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                            IID_PPV_ARGS(command_list.GetAddressOf()));
  command_list->Close();

  ComPtr<ID3D12Fence> fence;
  device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
  const HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  Scene scene;
  scene.rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
  rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_heap_desc.NumDescriptors = kBufferCount + 1;
  device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(scene.rtv_heap.GetAddressOf()));
  D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{};
  dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  dsv_heap_desc.NumDescriptors = 1;
  device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(scene.dsv_heap.GetAddressOf()));

  const D3D12_CPU_DESCRIPTOR_HANDLE rtv_base =
      scene.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  scene.back_buffers.resize(kBufferCount);
  for (UINT i = 0; i < kBufferCount; ++i) {
    swap_chain->GetBuffer(i, IID_PPV_ARGS(scene.back_buffers[i].GetAddressOf()));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtv_base.ptr + static_cast<SIZE_T>(i) * scene.rtv_size};
    device->CreateRenderTargetView(scene.back_buffers[i].Get(), nullptr, rtv);
  }

  const D3D12_CPU_DESCRIPTOR_HANDLE motion_rtv{
      rtv_base.ptr + static_cast<SIZE_T>(kBufferCount) * scene.rtv_size};
  {
    const auto desc = Tex2DDesc(DXGI_FORMAT_R16G16_FLOAT, kWidth, kHeight,
                                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const D3D12_HEAP_PROPERTIES heap = DefaultHeap();
    const D3D12_CLEAR_VALUE clear{DXGI_FORMAT_R16G16_FLOAT, {0.0f, 0.0f, 0.0f, 0.0f}};
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
            IID_PPV_ARGS(scene.motion_vectors.GetAddressOf())))) {
      Log("VERDICT BLOCKED: motion vector texture failed\n");
      return 3;
    }
    device->CreateRenderTargetView(scene.motion_vectors.Get(), nullptr, motion_rtv);
  }
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle =
      scene.dsv_heap->GetCPUDescriptorHandleForHeapStart();
  {
    const auto desc = Tex2DDesc(DXGI_FORMAT_R32_TYPELESS, kWidth, kHeight,
                                D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    const D3D12_HEAP_PROPERTIES heap = DefaultHeap();
    const D3D12_CLEAR_VALUE clear{DXGI_FORMAT_D32_FLOAT, {1.0f, 0.0f, 0.0f, 0.0f}};
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
            IID_PPV_ARGS(scene.depth.GetAddressOf())))) {
      Log("VERDICT BLOCKED: depth texture failed\n");
      return 3;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
    dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
    dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(scene.depth.Get(), &dsv_desc, dsv_handle);
  }

  sl::ReflexOptions reflex{};
  reflex.mode = sl::ReflexMode::eLowLatency;
  reflex.frameLimitUs = 1000000u / app_fps;
  result = slReflexSetOptions(reflex);
  Log("reflex: %d frameLimitUs=%u\n", static_cast<int>(result), reflex.frameLimitUs);

  DesktopCapture capture;
  if (g_options.capture) {
    capture.left = static_cast<UINT>(window_rect.left);
    capture.row = static_cast<UINT>(window_rect.top + static_cast<int>(kHeight) / 2);
    ComPtr<IDXGIFactory1> factory1;
    if (FAILED(factory.As(&factory1)) || !capture.Init(factory1.Get())) {
      Log("capture: unavailable, verdict uses the present ratio only\n");
      g_options.capture = false;
    } else {
      Log("capture: window=(%ld,%ld) row=%u\n", static_cast<long>(window_rect.left),
          static_cast<long>(window_rect.top), capture.row);
    }
  }

  const sl::ViewportHandle viewport{0};
  sl::DLSSGOptions options{};
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = multiplier - 1;
  options.numBackBuffers = kBufferCount;
  options.mvecDepthWidth = kWidth;
  options.mvecDepthHeight = kHeight;
  options.colorWidth = kWidth;
  options.colorHeight = kHeight;
  options.colorBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
  options.mvecBufferFormat = DXGI_FORMAT_R16G16_FLOAT;
  options.depthBufferFormat = DXGI_FORMAT_R32_TYPELESS;

  sl::Result set_options_result = sl::Result::eOk;
  bool options_set = false;
  uint32_t max_generated = 0;
  uint32_t status_first = 0;
  uint64_t presented = 0;
  uint32_t counted = 0;
  std::vector<int> positions;
  uint64_t fence_value = 0;
  uint32_t frame_index = 0;
  sl::FrameToken* token = nullptr;
  bool quit = false;

  // The compositor latches this window at its own cadence, so the duplication reader runs on its own
  // thread for the whole steady-state window instead of once per rendered frame.
  std::atomic<bool> capture_active{false};
  std::atomic<bool> capture_stop{false};
  std::mutex capture_mutex;
  std::thread capture_thread;
  if (g_options.capture) {
    capture_thread = std::thread([&] {
      while (!capture_stop.load(std::memory_order_relaxed)) {
        if (!capture_active.load(std::memory_order_relaxed)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          continue;
        }
        const int position = capture.Sample();
        if (position == -2) break;
        if (position < 0) continue;
        std::lock_guard<std::mutex> guard(capture_mutex);
        positions.push_back(position);
      }
    });
  }

  for (uint32_t frame = 0; frame < g_options.frames && !quit; ++frame) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) quit = true;
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    if (quit) break;

    frame_index = frame;
    result = slGetNewFrameToken(token, &frame_index);
    if (result != sl::Result::eOk) {
      Log("VERDICT BLOCKED: slGetNewFrameToken %d\n", static_cast<int>(result));
      return 3;
    }
    slReflexSleep(*token);

    const int bar_x = static_cast<int>((static_cast<uint64_t>(frame) * kBarStep) % kBarSpan);
    const bool wrapped = frame != 0 && bar_x == 0;
    const UINT back_buffer = swap_chain->GetCurrentBackBufferIndex();
    slPCLSetMarker(sl::PCLMarker::eSimulationStart, *token);

    allocator->Reset();
    command_list->Reset(allocator.Get(), nullptr);
    auto to_render_target = TransitionBarrier(scene.back_buffers[back_buffer].Get(),
                                              D3D12_RESOURCE_STATE_PRESENT,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET);
    command_list->ResourceBarrier(1, &to_render_target);

    const D3D12_CPU_DESCRIPTOR_HANDLE back_rtv{
        rtv_base.ptr + static_cast<SIZE_T>(back_buffer) * scene.rtv_size};
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    command_list->OMSetRenderTargets(1, &back_rtv, FALSE, nullptr);
    command_list->ClearRenderTargetView(back_rtv, black, 0, nullptr);
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    command_list->OMSetRenderTargets(1, &motion_rtv, FALSE, nullptr);
    command_list->ClearRenderTargetView(motion_rtv, zero, 0, nullptr);
    command_list->OMSetRenderTargets(0, nullptr, FALSE, &dsv_handle);
    command_list->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    const D3D12_RECT bar{bar_x, 0, bar_x + static_cast<int>(kBarWidth),
                         static_cast<int>(kHeight)};
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    command_list->OMSetRenderTargets(1, &back_rtv, FALSE, nullptr);
    command_list->ClearRenderTargetView(back_rtv, white, 1, &bar);
    // Pixel-space displacement; Streamline scales it by Constants::mvecScale. Writing an
    // already-normalized value here would understate the motion by the buffer width.
    const float motion[4] = {-static_cast<float>(kBarStep), 0.0f, 0.0f, 0.0f};
    command_list->OMSetRenderTargets(1, &motion_rtv, FALSE, nullptr);
    command_list->ClearRenderTargetView(motion_rtv, motion, 1, &bar);
    command_list->OMSetRenderTargets(0, nullptr, FALSE, &dsv_handle);
    command_list->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 1, &bar);

    auto to_present = TransitionBarrier(scene.back_buffers[back_buffer].Get(),
                                        D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        D3D12_RESOURCE_STATE_PRESENT);
    command_list->ResourceBarrier(1, &to_present);
    command_list->Close();
    ID3D12CommandList* lists[] = {command_list.Get()};
    queue->ExecuteCommandLists(1, lists);
    const uint64_t signal = ++fence_value;
    queue->Signal(fence.Get(), signal);
    slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *token);
    slPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, *token);

    sl::Constants constants{};
    MakeIdentity(constants.cameraViewToClip);
    MakeIdentity(constants.clipToCameraView);
    MakeIdentity(constants.clipToLensClip);
    MakeIdentity(constants.clipToPrevClip);
    MakeIdentity(constants.prevClipToClip);
    constants.jitterOffset = sl::float2(0.0f, 0.0f);
    constants.mvecScale = sl::float2(1.0f / kWidth, 1.0f / kHeight);
    constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    constants.cameraPos = sl::float3(0.0f, 0.0f, 0.0f);
    constants.cameraUp = sl::float3(0.0f, 1.0f, 0.0f);
    constants.cameraRight = sl::float3(1.0f, 0.0f, 0.0f);
    constants.cameraFwd = sl::float3(0.0f, 0.0f, 1.0f);
    constants.cameraNear = 0.1f;
    constants.cameraFar = 1000.0f;
    constants.cameraFOV = 1.0472f;
    constants.cameraAspectRatio = static_cast<float>(kWidth) / static_cast<float>(kHeight);
    constants.depthInverted = sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = (frame == 0 || wrapped) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    result = slSetConstants(constants, *token, viewport);
    if (result != sl::Result::eOk) {
      Log("VERDICT BLOCKED: slSetConstants %d at frame %u\n", static_cast<int>(result), frame);
      return 1;
    }

    sl::Resource depth_resource(sl::ResourceType::eTex2d, scene.depth.Get(),
                                D3D12_RESOURCE_STATE_DEPTH_WRITE);
    depth_resource.width = kWidth;
    depth_resource.height = kHeight;
    depth_resource.nativeFormat = DXGI_FORMAT_R32_TYPELESS;
    sl::Resource motion_resource(sl::ResourceType::eTex2d, scene.motion_vectors.Get(),
                                 D3D12_RESOURCE_STATE_RENDER_TARGET);
    motion_resource.width = kWidth;
    motion_resource.height = kHeight;
    motion_resource.nativeFormat = DXGI_FORMAT_R16G16_FLOAT;
    sl::ResourceTag tags[2] = {
        {&depth_resource, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent},
        {&motion_resource, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent},
    };
    result = slSetTagForFrame(*token, viewport, tags, 2, nullptr);
    if (result != sl::Result::eOk) {
      Log("VERDICT BLOCKED: slSetTagForFrame %d at frame %u\n", static_cast<int>(result), frame);
      return 1;
    }

    if (!options_set) {
      set_options_result = slDLSSGSetOptions(viewport, options);
      options_set = true;
      Log("setoptions: %d\n", static_cast<int>(set_options_result));
    }

    slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *token);
    slPCLSetMarker(sl::PCLMarker::ePresentStart, *token);
    const HRESULT present =
        swap_chain->Present(0, tearing_supported ? DXGI_PRESENT_ALLOW_TEARING : 0);
    slPCLSetMarker(sl::PCLMarker::ePresentEnd, *token);
    if (FAILED(present)) {
      Log("VERDICT BLOCKED: Present 0x%08lx at frame %u\n", static_cast<unsigned long>(present),
          frame);
      return 3;
    }

    sl::DLSSGState state{};
    const sl::Result state_result = slDLSSGGetState(viewport, state, &options);
    if (state_result == sl::Result::eOk) {
      if (frame >= kSteadyStateFrame) {
        presented += state.numFramesActuallyPresented;
        ++counted;
        max_generated = std::max(max_generated, state.numFramesToGenerateMax);
        const uint32_t status = static_cast<uint32_t>(state.status);
        if (status != 0 && status_first == 0) status_first = status;
      }
    } else if (frame == kSteadyStateFrame) {
      Log("state: %d\n", static_cast<int>(state_result));
    }

    if (fence->GetCompletedValue() < signal) {
      fence->SetEventOnCompletion(signal, fence_event);
      WaitForSingleObject(fence_event, INFINITE);
    }

    if (frame == kSteadyStateFrame) capture_active.store(true, std::memory_order_relaxed);
  }

  capture_stop.store(true, std::memory_order_relaxed);
  if (capture_thread.joinable()) capture_thread.join();

  const IntervalStats stats = AnalysePositions(positions);
  Log("max generated=%u\n", max_generated);
  Log("status=0x%x\n", status_first);
  const double ratio = counted != 0 ? static_cast<double>(presented) / counted : 0.0;
  Log("present ratio=%.2f over %u frames\n", ratio, counted);
  if (g_options.capture) {
    Log("capture: intervals=%zu distinctMedian=%d stepMedian=%.1fpx\n", stats.intervals,
        stats.distinct_median, stats.step_median);
    Log("capture debug: frames=%u timeouts=%u errors=%u blanks=%u\n", capture.frames.load(),
        capture.timeouts.load(), capture.errors.load(), capture.blanks.load());
    std::string distinct_histogram;
    for (size_t i = 0; i < stats.distinct_histogram.size(); ++i) {
      if (stats.distinct_histogram[i] == 0) continue;
      distinct_histogram += std::to_string(i) + ":" + std::to_string(stats.distinct_histogram[i]) + " ";
    }
    std::string delta_histogram;
    for (size_t i = 0; i < stats.delta_histogram.size(); ++i) {
      if (stats.delta_histogram[i] == 0) continue;
      delta_histogram += std::to_string(i) + "px:" + std::to_string(stats.delta_histogram[i]) + " ";
    }
    Log("capture debug: distinctHist=%s\n", distinct_histogram.c_str());
    Log("capture debug: deltaHist=%s\n", delta_histogram.c_str());
  } else {
    Log("capture: skipped\n");
  }

  const char* failure = nullptr;
  if (g_patch_failure.load()) {
    failure = "patches";
  } else if (g_options.unlock && g_gate_writes.empty()) {
    failure = "provider gates";
  } else if (set_options_result != sl::Result::eOk) {
    failure = "setoptions";
  } else if (status_first != 0) {
    failure = "status";
  } else if (max_generated < multiplier - 1) {
    failure = "max generated";
  } else if (ratio < static_cast<double>(multiplier) - 0.3) {
    failure = "present ratio";
  } else if (g_options.capture && g_options.midpoint) {
    if (stats.distinct_median != static_cast<int>(multiplier)) {
      failure = "capture intervals";
    } else if (std::fabs(stats.step_median - static_cast<double>(kBarStep) / multiplier) > 2.0) {
      failure = "capture step";
    }
  }

  DestroyWindow(window);
  if (fence_event) CloseHandle(fence_event);
  slShutdown();
  if (failure) {
    Log("VERDICT FAIL: %s\n", failure);
    return 1;
  }
  Log("VERDICT PASS\n");
  return 0;
}
