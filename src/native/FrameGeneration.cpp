#include "FrameGeneration.h"

#include "ContextState.h"
#include "Dx12Interop.h"
#include "Log.h"
#include "fg/FrameGenerationProvider.h"
#include "fg/FrameGenerationResources.h"
#include "fg/FgCapture.h"
#include "fg/FsrFgProvider.h"
#include "fg/FgTestObserver.h"
#include "fg/NgxFgProvider.h"
#include "fg/StreamlineFgProvider.h"
#include "fg/XellLatency.h"
#include "fg/XessFgProvider.h"

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <MinHook.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

// XeLL marker values, duplicated so the transport does not include the SDK headers.
namespace
{
constexpr int32_t kMarkerSimulationEnd = 1;
constexpr int32_t kMarkerRenderSubmitStart = 2;
constexpr int32_t kMarkerRenderSubmitEnd = 3;
constexpr int32_t kMarkerPresentStart = 4;
constexpr int32_t kMarkerPresentEnd = 5;
}

using Microsoft::WRL::ComPtr;

namespace bigscale
{
namespace
{
constexpr wchar_t kChildClass[] = L"BigScale.Presenter";
constexpr int32_t kSlotCount = 8;
constexpr uint64_t kMaxCaptureSerial = 1ull << 60;

enum class SlotPhase : int32_t
{
    Free = 0,
    // Token allocated; its graph has not reached the plugin event yet. No GPU work exists.
    Submitted,
    // The render event owns the slot and is copying the pre-overlay colour and guides.
    Capturing,
    // Guides and HUD-less colour are copied; the matching Present supplies the real colour.
    AwaitingPresent,
    // Present owns the slot and is copying the final backbuffer.
    PresentCapturing,
    // Immutable capture queued to the provider worker.
    Ready,
    // The worker owns the slot and is inside provider work.
    Generating,
    // Provider work was submitted; a transport completion fence proves when it is reusable.
    GpuPending,
    // Fence/submission state cannot be proven. The slot is retained, never reused.
    Faulted,
};

struct CaptureSlot
{
    BIGSCALE_FG_Frame frame{};
    uint64_t serial = 0;
    SlotPhase phase = SlotPhase::Free;
    FgSlotTextures textures;
    Dx12Interop::Shared color;
    Dx12Interop::Shared hudless;
    Dx12Interop::Shared depth;
    Dx12Interop::Shared motion;
    uint64_t sourceFenceValue = 0;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    bool hudlessRequired = false;
    // Multiplier snapshot of the configuration generation this capture belongs to.
    uint32_t multiplier = 2;
    // A retirement has been decided for this slot; it is either already published or owned by the
    // GPU-pending pump. Prevents a second publication for the same capture token.
    bool terminal = false;
    ID3D12Fence* completionFence = nullptr;
    uint64_t completionValue = 0;
    int32_t pendingResult = 0;
};

struct RetirementRecord
{
    uint32_t sequence = 0;
    uint32_t slotTag = 0;
    uint32_t generation = 0;
    int32_t result = 0;
};

struct DeferredRetirement
{
    bool publish = false;
    uint32_t slotTag = 0;
    uint32_t generation = 0;
    int32_t result = 0;
};

struct SharedStatus
{
    std::mutex mutex;
    int32_t state = BIGSCALE_FG_STATE_OFF;
    int32_t requested = BIGSCALE_FG_PROVIDER_OFF;
    int32_t active = BIGSCALE_FG_PROVIDER_OFF;
    uint32_t generation = 0;
    int32_t lastResult = 0;
    int32_t lastSourceFrame = -1;
    uint64_t renderedPresents = 0;
    uint64_t generatedPresents = 0;
    // Total multiplier in force for this generation, and the maximum total the current provider
    // proved. Zero respectively means "inactive" and "unknown".
    uint32_t activeMultiplier = 0;
    uint32_t maxMultiplier = 0;
    char reason[BIGSCALE_FG_REASON_CAPACITY]{};
    RetirementRecord retirements[BIGSCALE_FG_RETIREMENT_QUEUE]{};
    uint32_t retirementCount = 0;
    uint32_t nextSequence = 1;
};

std::atomic<bool> g_installed{false};
std::atomic<bool> g_shuttingDown{false};
std::atomic<int32_t> g_provider{BIGSCALE_FG_PROVIDER_OFF};
std::atomic<bool> g_xrActive{false};
std::atomic<bool> g_menuVisible{false};
std::atomic<uint32_t> g_multiplier{2};
std::atomic<HWND> g_parent{nullptr};
std::atomic<HWND> g_child{nullptr};
std::atomic<IDXGISwapChain*> g_childSwapchain{nullptr};
std::atomic<ID3D11Device*> g_captureDevice{nullptr};
std::atomic<uint64_t> g_presentId{0};
std::atomic<uint64_t> g_markerFrame{0};
std::atomic<bool> g_adapterResetRequested{false};
std::atomic<void*> g_pendingToken{nullptr};

Dx12Interop g_interop;
std::mutex g_interopMutex;
FrameGenerationResources g_resources;
std::mutex g_resourcesMutex;
ContextState g_captureState;
XellLatency g_xell;
std::mutex g_xellMutex;
// The queue is shared by every provider, so one transport fence proves that all provider work for
// a capture, including the final real-frame copy, has completed.
ComPtr<ID3D12Fence> g_completionFence;
uint64_t g_completionValue = 0;

CaptureSlot g_slots[kSlotCount];
std::mutex g_slotMutex;
uint64_t g_nextSerial = 1;

std::mutex g_workerMutex;
HANDLE g_workerEvent = nullptr;
HANDLE g_workerThread = nullptr;
std::atomic<bool> g_workerStop{false};
int32_t g_workerQueue[kSlotCount]{};
uint32_t g_workerQueueCount = 0;

std::unique_ptr<IFrameGenerationProvider> g_adapter;
std::atomic<bool> g_adapterExists{false};
bool g_adapterTeardownFailed = false;
std::string g_adapterTeardownReason;
// The worker writes refusal identity while the configuration/status threads read it. One small
// mutex with short, non-nested scopes avoids the previous unsynchronised std::string access.
std::mutex g_refusalMutex;
int32_t g_adapterRefusedProvider = BIGSCALE_FG_PROVIDER_OFF;
uint32_t g_adapterRefusedGeneration = 0;
std::string g_adapterRefusalReason;
// Typed restart requirement of the latched refusal. Never parsed back out of the reason text.
bool g_adapterRefusalRestart = false;
int32_t g_adapterProvider = BIGSCALE_FG_PROVIDER_OFF;
// The provider that owns the suspended, process-resident adapter. ClearAdapterIdentity resets the
// mutually exclusive capability fields, but a resume must still find the incumbent.
int32_t g_suspendedProvider = BIGSCALE_FG_PROVIDER_OFF;
uint32_t g_adapterWidth = 0;
uint32_t g_adapterHeight = 0;
bool g_adapterReversedDepth = false;
// Worker-only source-frame continuity. A capture can be dropped before the worker sees it, so a
// skipped index must reset interpolation history instead of pairing new colour with old motion.
bool g_frameHistory = false;
int32_t g_lastFrameIndex = -1;
int32_t g_lastFrameCamera = 0;
uint32_t g_lastFrameGeneration = 0;

std::wstring g_runtimeDirectory;
std::wstring g_dataDirectory;
std::string g_engineVersion = "6000.3.17f1";

SharedStatus g_status;

// Capture and owned-present entries are counted so shutdown can stop new work and wait for the
// current copy to leave before joining the worker or releasing any device state.
std::mutex g_entryMutex;
std::condition_variable g_entryCv;
int g_activeEntries = 0;

struct EntryScope
{
    bool entered = false;
    EntryScope()
    {
        std::lock_guard<std::mutex> guard(g_entryMutex);
        if (!g_shuttingDown.load())
        {
            g_activeEntries++;
            entered = true;
        }
    }
    ~EntryScope()
    {
        if (!entered) return;
        std::lock_guard<std::mutex> guard(g_entryMutex);
        g_activeEntries--;
        if (g_activeEntries == 0) g_entryCv.notify_all();
    }
    EntryScope(const EntryScope&) = delete;
    EntryScope& operator=(const EntryScope&) = delete;
};

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
PresentFn g_presentOriginal = nullptr;
Present1Fn g_present1Original = nullptr;
ResizeBuffersFn g_resizeOriginal = nullptr;

bool EnsureMarkers(std::string& reason)
{
    if (g_provider.load() != BIGSCALE_FG_PROVIDER_XESS) return true;
    std::lock_guard<std::mutex> guard(g_xellMutex);
    if (g_xell.Ready()) return true;
    if (!g_interop.Ready())
    {
        reason = "the frame-generation device is not ready for XeLL";
        return false;
    }
    std::string xellReason;
    if (!g_xell.Ensure(g_runtimeDirectory, g_interop.Device(), xellReason))
    {
        reason = xellReason;
        return false;
    }
    return true;
}

void Marker(int32_t phase, uint64_t frameId)
{
    if (g_shuttingDown.load()) return;
    if (g_provider.load() != BIGSCALE_FG_PROVIDER_XESS) return;
    if (frameId == 0 || frameId != g_markerFrame.load()) return;
    std::lock_guard<std::mutex> guard(g_xellMutex);
    g_xell.Marker(static_cast<uint32_t>(frameId & 0xffffffffull), phase);
}

void MarkerBoth(int32_t first, int32_t second, uint64_t frameId)
{
    if (g_shuttingDown.load()) return;
    if (g_provider.load() != BIGSCALE_FG_PROVIDER_XESS) return;
    if (frameId == 0 || frameId != g_markerFrame.load()) return;
    std::lock_guard<std::mutex> guard(g_xellMutex);
    const uint32_t low = static_cast<uint32_t>(frameId & 0xffffffffull);
    g_xell.Marker(low, first);
    g_xell.Marker(low, second);
}

std::string HexStatus(long status)
{
    char text[16]{};
    std::snprintf(text, sizeof(text), "%08lx", static_cast<unsigned long>(status));
    return text;
}

void WriteReason(const char* reason)
{
    std::lock_guard<std::mutex> guard(g_status.mutex);
    std::strncpy(g_status.reason, reason != nullptr ? reason : "", BIGSCALE_FG_REASON_CAPACITY - 1);
    g_status.reason[BIGSCALE_FG_REASON_CAPACITY - 1] = 0;
}

void SetState(int32_t state, const char* reason, int32_t result = 0)
{
    std::lock_guard<std::mutex> guard(g_status.mutex);
    g_status.state = state;
    g_status.lastResult = result;
    if (reason != nullptr)
    {
        std::strncpy(g_status.reason, reason, BIGSCALE_FG_REASON_CAPACITY - 1);
        g_status.reason[BIGSCALE_FG_REASON_CAPACITY - 1] = 0;
    }
}

// The public retirement sequence is assigned here, in publication order. A cancellation and a GPU
// completion may arrive out of capture order, which is exactly what the ordered Ack contract allows.
void PublishRetirement(uint32_t slotTag, uint32_t generation, int32_t result)
{
    std::lock_guard<std::mutex> guard(g_status.mutex);
    if (g_status.retirementCount >= BIGSCALE_FG_RETIREMENT_QUEUE)
    {
        g_status.state = BIGSCALE_FG_STATE_FAULTED;
        g_status.lastResult = 19;
        std::strncpy(g_status.reason, "capture retirement queue overflow", BIGSCALE_FG_REASON_CAPACITY - 1);
        g_status.reason[BIGSCALE_FG_REASON_CAPACITY - 1] = 0;
        return;
    }
    RetirementRecord& record = g_status.retirements[g_status.retirementCount++];
    record.sequence = g_status.nextSequence++;
    record.slotTag = slotTag;
    record.generation = generation;
    record.result = result;
}

// ShowWindow sends messages to the window's owner; calling it from another thread blocks until the
// owner pumps messages. Every visibility change therefore has to come from the owning thread.
void ShowChildOnOwner(bool visible)
{
    HWND child = g_child.load();
    if (child == nullptr) return;
    if (GetWindowThreadProcessId(child, nullptr) != GetCurrentThreadId()) return;
    ShowWindow(child, visible ? SW_SHOWNA : SW_HIDE);
}

// The provider worker can decide that the presenter must disappear (refusal, suspension, fault) but
// it is not the window's owner thread. The request is applied by the next present hook, which is.
std::atomic<bool> g_childHideRequested{false};

void RequestChildHidden() { g_childHideRequested.store(true); }

void ApplyChildHideRequest()
{
    if (!g_childHideRequested.exchange(false)) return;
    ShowChildOnOwner(false);
}

LRESULT CALLBACK ChildProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        default: return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

bool RegisterChildClass()
{
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.lpfnWndProc = ChildProc;
    description.hInstance = GetModuleHandleW(nullptr);
    description.lpszClassName = kChildClass;
    if (RegisterClassExW(&description) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        Log("FG: presenter window class registration failed (%lu).", GetLastError());
        return false;
    }
    registered = true;
    return true;
}

bool CreateChild(HWND parent, std::string& reason)
{
    if (parent == nullptr || !IsWindow(parent))
    {
        reason = "the game window is not available";
        return false;
    }
    if (GetWindowThreadProcessId(parent, nullptr) != GetCurrentThreadId())
    {
        reason = "the game window is owned by another thread";
        return false;
    }
    if (!RegisterChildClass())
    {
        reason = "the presenter window class could not be registered";
        return false;
    }
    RECT client{};
    GetClientRect(parent, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    HWND child = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_NOPARENTNOTIFY, kChildClass, L"",
        WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, width, height, parent, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (child == nullptr)
    {
        reason = "the presenter child window could not be created";
        return false;
    }
    SetWindowPos(child, HWND_TOP, 0, 0, width, height, SWP_NOACTIVATE | SWP_HIDEWINDOW);
    g_child.store(child);
    g_parent.store(parent);
    Log("FG: presenter child window created (%dx%d).", width, height);
    return true;
}

void DestroyChild()
{
    HWND child = g_child.exchange(nullptr);
    if (child != nullptr) DestroyWindow(child);
}

void UpdateChildGeometry()
{
    HWND parent = g_parent.load();
    HWND child = g_child.load();
    if (parent == nullptr || child == nullptr) return;
    RECT client{};
    GetClientRect(parent, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;
    RECT current{};
    GetWindowRect(child, &current);
    if ((current.right - current.left) != width || (current.bottom - current.top) != height)
        SetWindowPos(child, HWND_TOP, 0, 0, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
}

bool EncodeToken(uint64_t serial, int32_t index, void** token)
{
    if (serial == 0 || serial >= kMaxCaptureSerial || index < 0 || index >= kSlotCount) return false;
    const uint64_t value = (serial << 4) | static_cast<uint64_t>(index + 1);
    *token = reinterpret_cast<void*>(static_cast<uintptr_t>(value));
    return true;
}

bool DecodeToken(void* data, int32_t& index, uint64_t& serial)
{
    const uint64_t value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data));
    const uint64_t tag = value & 0xfull;
    if (tag < 1 || tag > static_cast<uint64_t>(kSlotCount)) return false;
    index = static_cast<int32_t>(tag - 1);
    serial = value >> 4;
    return serial != 0 && serial < kMaxCaptureSerial;
}

void ReleaseSlotLocked(CaptureSlot& slot)
{
    slot.phase = SlotPhase::Free;
    slot.serial = 0;
    slot.sourceFenceValue = 0;
    slot.completionFence = nullptr;
    slot.completionValue = 0;
    slot.pendingResult = 0;
    slot.terminal = false;
    slot.hudlessRequired = false;
    std::memset(&slot.frame, 0, sizeof(slot.frame));
}

// Decides the retirement for a slot that must stop without reaching the provider. A capture whose
// D3D11 work was already queued stays GPU-pending on the producer fence; one with no GPU work is
// released immediately. The caller publishes `out.publish` after leaving g_slotMutex.
DeferredRetirement CancelSlotLocked(CaptureSlot& slot, int32_t result)
{
    DeferredRetirement out;
    if (slot.phase == SlotPhase::Free || slot.terminal) return out;
    slot.terminal = true;
    ID3D12Fence* fence = nullptr;
    uint64_t value = 0;
    if (slot.sourceFenceValue != 0 && g_interop.Ready())
    {
        fence = g_interop.SourceFence();
        value = slot.sourceFenceValue;
    }
    if (fence != nullptr && value != 0)
    {
        slot.completionFence = fence;
        slot.completionValue = value;
        slot.pendingResult = result;
        slot.phase = SlotPhase::GpuPending;
        return out;
    }
    out.publish = true;
    out.slotTag = slot.frame.slotTag;
    out.generation = slot.frame.generation;
    out.result = result;
    ReleaseSlotLocked(slot);
    return out;
}

void Publish(DeferredRetirement retirement)
{
    if (retirement.publish) PublishRetirement(retirement.slotTag, retirement.generation, retirement.result);
}

// Cancels capture tokens that have no GPU work yet and any capture waiting for a Present that can
// no longer be paired (provider off, menu, XR or a swapchain resize).
void CancelOutstanding()
{
    DeferredRetirement published[kSlotCount + 1]{};
    int count = 0;
    {
        std::lock_guard<std::mutex> guard(g_slotMutex);
        void* token = g_pendingToken.exchange(nullptr);
        int32_t index = -1;
        uint64_t serial = 0;
        if (token != nullptr && DecodeToken(token, index, serial))
        {
            CaptureSlot& slot = g_slots[index];
            if (slot.serial == serial && slot.phase == SlotPhase::AwaitingPresent)
            {
                const DeferredRetirement retirement = CancelSlotLocked(slot, -2);
                if (retirement.publish && count < kSlotCount + 1) published[count++] = retirement;
            }
        }
        for (CaptureSlot& slot : g_slots)
        {
            // Submitted has no GPU work; Ready is producer-complete but no provider has claimed it,
            // so both can be retired through their producer fence on an Off/menu/XR/resize.
            if (slot.phase != SlotPhase::Submitted && slot.phase != SlotPhase::Ready) continue;
            const DeferredRetirement retirement = CancelSlotLocked(slot, -2);
            if (retirement.publish && count < kSlotCount + 1) published[count++] = retirement;
        }
    }
    for (int i = 0; i < count; i++) Publish(published[i]);
}

bool EnsureInterop(ID3D11Device* device, std::string& reason)
{
    std::lock_guard<std::mutex> guard(g_interopMutex);
    if (g_interop.Ready()) return true;
    if (!g_interop.Ensure(device, reason)) return false;
    Log("FG: D3D12 interop ready.");
    return true;
}

struct InteropLock
{
    std::unique_lock<std::mutex> lock;
    explicit InteropLock() : lock(g_interopMutex) {}
};

bool OpenOnDx12Locked(ID3D11Texture2D* texture, ComPtr<ID3D12Resource>& out, std::string& reason)
{
    if (texture == nullptr || !g_interop.Ready())
    {
        reason = "the frame-generation capture device is not ready";
        return false;
    }
    ComPtr<IDXGIResource> resource;
    HANDLE handle = nullptr;
    if (FAILED(texture->QueryInterface(IID_PPV_ARGS(&resource))) || !resource ||
        FAILED(resource->GetSharedHandle(&handle)) || handle == nullptr)
    {
        reason = "a shared capture texture has no shared handle";
        return false;
    }
    if (FAILED(g_interop.Device()->OpenSharedHandle(handle, IID_PPV_ARGS(&out))) || !out)
    {
        reason = "a shared capture texture could not be opened on D3D12";
        return false;
    }
    return true;
}

bool SharedMatches(const Dx12Interop::Shared& shared, ID3D11Texture2D* texture)
{
    return texture != nullptr && shared.d3d11.Get() == texture && shared.d3d12 != nullptr;
}

// Creates or resizes the slot's shared set and opens every required plane on the FG device.
bool EnsureSlotResources(CaptureSlot& slot, std::string& reason)
{
    std::lock_guard<std::mutex> guard(g_resourcesMutex);
    FgSlotTextures& textures = slot.textures;
    const bool matching = SharedMatches(slot.color, textures.color.Get()) &&
        SharedMatches(slot.depth, textures.depth.Get()) &&
        SharedMatches(slot.motion, textures.motion.Get()) &&
        (!slot.hudlessRequired ||
            (textures.hudless && SharedMatches(slot.hudless, textures.hudless.Get()))) &&
        // The D3D11 objects survive an extent change (a pooled or resized target keeps its identity), so
        // object identity alone must never declare a slot ready: a stale allocation makes every later
        // conversion reject the frame with "the frame-generation render extent is invalid".
        slot.renderWidth == textures.renderWidth && slot.renderHeight == textures.renderHeight &&
        slot.outputWidth == textures.outputWidth && slot.outputHeight == textures.outputHeight;
    if (matching) return true;
    slot.color.Reset();
    slot.hudless.Reset();
    slot.depth.Reset();
    slot.motion.Reset();
    ID3D11Device* device = g_captureDevice.load();
    if (device == nullptr)
    {
        reason = "the capture device is not bound";
        return false;
    }
    if (!g_resources.EnsureKernels(device, reason)) return false;
    if (!g_resources.EnsureSlot(device, textures, slot.renderWidth, slot.renderHeight, slot.outputWidth,
            slot.outputHeight, slot.hudlessRequired, reason))
        return false;
    {
        InteropLock interopLock;
        if (!OpenOnDx12Locked(textures.color.Get(), slot.color.d3d12, reason) ||
            !OpenOnDx12Locked(textures.depth.Get(), slot.depth.d3d12, reason) ||
            !OpenOnDx12Locked(textures.motion.Get(), slot.motion.d3d12, reason))
            return false;
        if (slot.hudlessRequired &&
            !OpenOnDx12Locked(textures.hudless.Get(), slot.hudless.d3d12, reason))
            return false;
    }
    slot.color.d3d11 = textures.color;
    slot.depth.d3d11 = textures.depth;
    slot.motion.d3d11 = textures.motion;
    slot.color.width = slot.outputWidth;
    slot.color.height = slot.outputHeight;
    slot.color.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    slot.depth.width = slot.renderWidth;
    slot.depth.height = slot.renderHeight;
    slot.depth.format = DXGI_FORMAT_R32_FLOAT;
    slot.motion.width = slot.renderWidth;
    slot.motion.height = slot.renderHeight;
    slot.motion.format = DXGI_FORMAT_R16G16_FLOAT;
    slot.color.state = D3D12_RESOURCE_STATE_COMMON;
    slot.depth.state = D3D12_RESOURCE_STATE_COMMON;
    slot.motion.state = D3D12_RESOURCE_STATE_COMMON;
    if (slot.hudlessRequired)
    {
        slot.hudless.d3d11 = textures.hudless;
        slot.hudless.width = slot.outputWidth;
        slot.hudless.height = slot.outputHeight;
        slot.hudless.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        slot.hudless.state = D3D12_RESOURCE_STATE_COMMON;
    }
    return true;
}

// Process-lifetime provider family. ANR installs D3D12 hooks that stay installed after every
// feature release, so it can never share a process with another frame-generation SDK, and the other
// SDKs can never be unloaded underneath it. The family is claimed by the first initialization
// attempt and is never cleared: switching across it requires a process restart. The values are the
// ABI's BIGSCALE_FG_FAMILY_* codes, and the worker publishes the claim through GetStatus so the UI can
// report a cross-family selection before any capture happens.
enum class RuntimeFamily : uint32_t
{
    Unused = BIGSCALE_FG_FAMILY_UNUSED,
    Normal = BIGSCALE_FG_FAMILY_NORMAL,
    Anr = BIGSCALE_FG_FAMILY_ANR,
    Streamline = BIGSCALE_FG_FAMILY_STREAMLINE,
};

std::atomic<RuntimeFamily> g_family{RuntimeFamily::Unused};
// True between a resident provider's Suspend and the next Resume.
bool g_adapterSuspended = false;

const char* const kRestartToSwitch =
    "restart the game to switch between ANR and other frame-generation providers";

RuntimeFamily FamilyOf(int32_t provider)
{
    if (provider == BIGSCALE_FG_PROVIDER_ANR) return RuntimeFamily::Anr;
    if (provider == BIGSCALE_FG_PROVIDER_STREAMLINE) return RuntimeFamily::Streamline;
    return RuntimeFamily::Normal;
}

// Deliberately leaked, process-lifetime storage. A resident SDK's hooks may run until the process
// exits, so the objects it can reach may not be destroyed by C++ static teardown.
struct RetainedGraph
{
    IFrameGenerationProvider* adapter = nullptr;
    std::unique_ptr<Dx12Interop> interop;
    ComPtr<ID3D12Fence> completionFence;
    FrameGenerationResources resources;
    CaptureSlot slots[kSlotCount];
};

RetainedGraph** RetainedGraphSlot()
{
    static RetainedGraph** slot = new RetainedGraph*(nullptr);
    return slot;
}

void LatchRefusal(int32_t provider, uint32_t generation, const std::string& text,
    bool restartRequired = false)
{
    std::lock_guard<std::mutex> guard(g_refusalMutex);
    g_adapterRefusedProvider = provider;
    g_adapterRefusedGeneration = generation;
    g_adapterRefusalReason = text;
    g_adapterRefusalRestart = restartRequired;
}

// Publishes the selected provider's proven maximum. Called by the worker after a capability query,
// including when the initialization that follows it fails, so the UI never keeps guessing.
void PublishCapability(uint32_t maximum)
{
    std::lock_guard<std::mutex> guard(g_status.mutex);
    g_status.maxMultiplier = maximum;
}

void ClearAdapterIdentity()
{
    g_adapterProvider = BIGSCALE_FG_PROVIDER_OFF;
    g_adapterWidth = 0;
    g_adapterHeight = 0;
    g_adapterReversedDepth = false;
    // The UI must name the requested provider, never a retired or suspended adapter.
    std::lock_guard<std::mutex> guard(g_status.mutex);
    g_status.active = BIGSCALE_FG_PROVIDER_OFF;
}

// Drains and releases the provider adapter. A failed drain keeps both the adapter and the XeLL
// context alive: a live provider may still reference that context, so releasing it early would
// destroy state a worker call is using. A process-resident provider is suspended instead, because
// its SDK cannot be unloaded inside this process.
bool DestroyAdapter(std::string& reason)
{
    const auto started = std::chrono::steady_clock::now();
    if (g_adapter != nullptr)
    {
        Log("FG: provider drain begin: adapter=%d generation=%u", g_adapterProvider, g_status.generation);
        if (g_adapterTeardownFailed)
        {
            reason = g_adapterTeardownReason.empty() ? "the frame-generation provider failed to tear down"
                                                     : g_adapterTeardownReason;
            return false;
        }
        if (g_adapter->ProcessResident())
        {
            std::string suspendReason;
            if (!g_adapter->Suspend(suspendReason))
            {
                reason = suspendReason.empty() ? "the process-resident frame-generation provider could not suspend"
                                               : suspendReason;
                return false;
            }
            g_adapterSuspended = true;
            g_adapterExists.store(false);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            Log("FG: provider suspended in %lld ms; its runtime stays resident for this process",
                static_cast<long long>(elapsed));
            // A second suspend for the same resident adapter must not erase the identity: the
            // identity field was cleared by the first one.
            if (g_adapterProvider != BIGSCALE_FG_PROVIDER_OFF)
                g_suspendedProvider = g_adapterProvider;
            ClearAdapterIdentity();
            return true;
        }
        std::string drainReason;
        g_adapter->Drain(drainReason);
        if (!drainReason.empty())
        {
            reason = drainReason;
            return false;
        }
        std::string shutdownReason;
        if (!g_adapter->Shutdown(shutdownReason))
        {
            g_adapterTeardownFailed = true;
            g_adapterTeardownReason = shutdownReason.empty()
                ? "the frame-generation provider failed to release its SDK state"
                : shutdownReason;
            reason = g_adapterTeardownReason;
            return false;
        }
        g_adapter.reset();
        g_adapterExists.store(false);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        Log("FG: provider drain complete in %lld ms", static_cast<long long>(elapsed));
    }
    {
        std::lock_guard<std::mutex> guard(g_xellMutex);
        g_xell.Release();
    }
    ClearAdapterIdentity();
    return true;
}

std::unique_ptr<IFrameGenerationProvider> MakeAdapter(int32_t provider)
{
    switch (provider)
    {
        case BIGSCALE_FG_PROVIDER_NVIDIA: return std::make_unique<NgxFgProvider>(NgxFgProvider::Flavor::Nvidia);
        case BIGSCALE_FG_PROVIDER_ANR: return std::make_unique<NgxFgProvider>(NgxFgProvider::Flavor::Anr);
        case BIGSCALE_FG_PROVIDER_FSR: return std::make_unique<FsrFgProvider>();
        case BIGSCALE_FG_PROVIDER_XESS: return std::make_unique<XessFgProvider>();
        case BIGSCALE_FG_PROVIDER_STREAMLINE:
            // WIP: DLSS-G through Streamline (multi-frame generation on Ada). Reachable only from the
            // offline harness (`BigScaleChecks --fg streamline`); the managed panel holds this
            // selection at Off until the in-game lifecycle is finished.
            return std::make_unique<StreamlineFgProvider>();
        default: return nullptr;
    }
}

bool EnsureAdapter(const CaptureSlot& slot, std::string& reason)
{
    const int32_t requested = g_provider.load();
    const uint32_t generation = slot.frame.generation;
    const bool reversedDepth = (slot.frame.flags & BIGSCALE_FG_FLAG_REVERSED_DEPTH) != 0;
    // SR history and camera changes reset the provider's history without recreating the SDK
    // context. Only device, provider, output extent and depth convention define the adapter.
    const bool matches = g_adapter != nullptr && g_adapterProvider == requested &&
        g_adapterWidth == static_cast<uint32_t>(slot.frame.outputWidth) &&
        g_adapterHeight == static_cast<uint32_t>(slot.frame.outputHeight) &&
        g_adapterReversedDepth == reversedDepth;
    if (matches) return true;
    if (g_adapterTeardownFailed)
    {
        reason = g_adapterTeardownReason.empty() ? "the frame-generation provider failed to tear down"
                                                 : g_adapterTeardownReason;
        SetState(BIGSCALE_FG_STATE_FAULTED, reason.c_str(), -1);
        return false;
    }
    auto buildInit = [&]() -> FgInit
    {
        FgInit init{};
        init.device = g_interop.Device();
        init.queue = g_interop.Queue();
        init.child = g_child.load();
        init.width = static_cast<uint32_t>(slot.frame.outputWidth);
        init.height = static_cast<uint32_t>(slot.frame.outputHeight);
        init.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        init.runtimeDirectory = g_runtimeDirectory;
        init.dataDirectory = g_dataDirectory;
        init.engineVersion = g_engineVersion;
        init.reversedDepth = reversedDepth;
        return init;
    };
    auto publishActive = [&](const FgInit& init)
    {
        g_adapterExists.store(true);
        g_adapterProvider = requested;
        g_adapterWidth = init.width;
        g_adapterHeight = init.height;
        g_adapterReversedDepth = reversedDepth;
        {
            std::lock_guard<std::mutex> guard(g_status.mutex);
            g_status.active = requested;
        }
        SetState(BIGSCALE_FG_STATE_WAITING_FIRST_GENERATED, "waiting for the first generated frame");
        Log("FG: %s active generation=%u %ux%u", g_adapter->Name(), generation, init.width, init.height);
    };

    const RuntimeFamily requestedFamily = FamilyOf(requested);
    const bool residentIncumbent = g_adapter != nullptr && g_adapter->ProcessResident();
    // The approved process boundary: a process that has loaded either side never loads the other.
    // The incumbent is suspended (resident) or released (normal), the child disappears and the
    // request is refused for this generation; the user's selection is kept so a restart activates it.
    if (g_family.load() != RuntimeFamily::Unused && g_family.load() != requestedFamily)
    {
        std::string retireReason;
        if (!DestroyAdapter(retireReason))
        {
            reason = retireReason;
            SetState(BIGSCALE_FG_STATE_FAULTED, reason.c_str(), -1);
            Log("FG: the previous provider could not be suspended: %s", reason.c_str());
            return false;
        }
        RequestChildHidden();
        LatchRefusal(requested, generation, kRestartToSwitch, true);
        reason = kRestartToSwitch;
        SetState(BIGSCALE_FG_STATE_UNSUPPORTED, kRestartToSwitch, -1);
        Log("FG: provider %d refused in the %s process family: %s", requested,
            g_family.load() == RuntimeFamily::Anr ? "ANR"
            : g_family.load() == RuntimeFamily::Streamline ? "Streamline"
                                                           : "normal",
            kRestartToSwitch);
        return false;
    }
    // A resident provider survives Off, suspension, resize and extent changes: it is suspended and
    // resumed in place instead of being destroyed and recreated.
    if (residentIncumbent && (requested == g_adapterProvider || requested == g_suspendedProvider))
    {
        FgInit init = buildInit();
        std::string suspendReason;
        if (!g_adapter->Suspend(suspendReason))
        {
            reason = suspendReason.empty() ? "the resident frame-generation provider could not suspend"
                                           : suspendReason;
            SetState(BIGSCALE_FG_STATE_FAULTED, reason.c_str(), -1);
            return false;
        }
        if (!g_adapter->Resume(init, reason))
        {
            // A resident SDK without usable state can never be recreated; the refusal is authoritative.
            g_adapterSuspended = true;
            g_adapterExists.store(false);
            ClearAdapterIdentity();
            LatchRefusal(requested, generation, reason);
            SetState(BIGSCALE_FG_STATE_UNSUPPORTED, reason.c_str(), -1);
            Log("FG: %s could not resume: %s", g_adapter->Name(), reason.c_str());
            return false;
        }
        g_adapterSuspended = false;
        g_suspendedProvider = BIGSCALE_FG_PROVIDER_OFF;
        PublishCapability(g_adapter->MaximumMultiplier());
        publishActive(init);
        return true;
    }
    {
        bool refused = false;
        {
            std::lock_guard<std::mutex> guard(g_refusalMutex);
            refused = g_adapterRefusedProvider == requested && g_adapterRefusedGeneration == generation;
            if (refused)
                reason = g_adapterRefusalReason.empty()
                    ? "the selected frame-generation provider is unavailable"
                    : g_adapterRefusalReason;
        }
        if (refused)
        {
            // Keep the refusal visible to the UI; the first failure is the authoritative reason.
            SetState(BIGSCALE_FG_STATE_UNSUPPORTED, reason.c_str(), -1);
            return false;
        }
    }
    std::string drainReason;
    if (!DestroyAdapter(drainReason))
    {
        reason = drainReason;
        SetState(BIGSCALE_FG_STATE_FAULTED, reason.c_str(), -1);
        Log("FG: the previous provider could not be replaced: %s", reason.c_str());
        return false;
    }
    std::unique_ptr<IFrameGenerationProvider> adapter = MakeAdapter(requested);
    if (adapter == nullptr)
    {
        reason = "the requested frame-generation provider is unknown";
        return false;
    }
    FgInit init = buildInit();
    if (init.child == nullptr)
    {
        reason = "the presenter window is not ready";
        return false;
    }
    if (!g_interop.Ready())
    {
        reason = "the frame-generation device is not ready";
        return false;
    }
    if (requested == BIGSCALE_FG_PROVIDER_XESS)
    {
        // A previous teardown released XeLL together with its adapter; recreate it before wiring the
        // next XeSS-FG context. Latency reduction stays an enhancement, not a prerequisite.
        std::string markerReason;
        if (!EnsureMarkers(markerReason))
            Log("FG: XeLL could not be recreated before the XeSS adapter: %s", markerReason.c_str());
        std::lock_guard<std::mutex> guard(g_xellMutex);
        init.xellContext = g_xell.Handle();
    }
    // Claim the process-lifetime family immediately before the first SDK initialization attempt, so
    // even a partial initialization can never escape it.
    if (g_family.load() == RuntimeFamily::Unused) g_family.store(requestedFamily);
    SetState(BIGSCALE_FG_STATE_INITIALIZING, "initializing the frame-generation provider");
    // The capability query runs inside Initialize before anything it can fail on, so a truthful
    // maximum is published even when the initialization that follows is refused.
    const bool initialized = adapter->Initialize(init, reason);
    PublishCapability(adapter->MaximumMultiplier());
    if (!initialized)
    {
        Log("FG: %s initialization failed: %s", adapter->Name(), reason.c_str());
        g_interop.LogValidationErrors("provider init");
        if (adapter->ProcessResident())
        {
            // Keep the resident SDK object: its module and hooks stay installed for the whole
            // process, so neither cleanup nor a local unique_ptr may destroy it.
            g_adapter = std::move(adapter);
            g_adapterSuspended = true;
            g_adapterExists.store(false);
            ClearAdapterIdentity();
            LatchRefusal(requested, generation, reason);
            SetState(BIGSCALE_FG_STATE_UNSUPPORTED, reason.c_str(), -1);
            return false;
        }
        // A failed initialization may leave live SDK state. It may only be discarded through a
        // checked teardown; if that teardown fails, the partial adapter is retained and latched.
        std::string cleanupReason;
        if (!adapter->Shutdown(cleanupReason))
        {
            reason += " (cleanup failed: " + cleanupReason + ")";
            g_adapter = std::move(adapter);
            g_adapterExists.store(true);
            g_adapterProvider = requested;
            g_adapterWidth = init.width;
            g_adapterHeight = init.height;
            g_adapterReversedDepth = reversedDepth;
            g_adapterTeardownFailed = true;
            g_adapterTeardownReason = cleanupReason;
            SetState(BIGSCALE_FG_STATE_FAULTED, reason.c_str(), -1);
            return false;
        }
        // Publish the refusal and latch it: retrying a refused SDK for every frame only spams the
        // log and blocks the worker. A provider/generation change clears the latch.
        LatchRefusal(requested, generation, reason);
        SetState(BIGSCALE_FG_STATE_UNSUPPORTED, reason.c_str(), -1);
        return false;
    }
    g_adapter = std::move(adapter);
    publishActive(init);
    return true;
}

bool RecordFrame(CaptureSlot& slot, std::string& reason, uint32_t& generatedCount)
{
    // Every path publishes the count of generated images this provider actually presented; an early
    // failure reports zero, and a failure after some intermediates succeeded keeps their count.
    generatedCount = 0;
    const HRESULT deviceRemoved = g_interop.Device() != nullptr ? g_interop.Device()->GetDeviceRemovedReason()
        : S_OK;
    if (FAILED(deviceRemoved))
    {
        reason = "the frame-generation device was removed (0x" +
            HexStatus(deviceRemoved) + "); frame generation is disabled for this session";
        SetState(BIGSCALE_FG_STATE_UNSUPPORTED, reason.c_str());
        ShowChildOnOwner(false);
        return false;
    }
    if (!EnsureAdapter(slot, reason)) return false;
    if (slot.color.d3d12 == nullptr || slot.depth.d3d12 == nullptr || slot.motion.d3d12 == nullptr ||
        (slot.hudlessRequired && slot.hudless.d3d12 == nullptr))
    {
        reason = "the frame-generation capture resources are incomplete";
        return false;
    }
    const bool selfExecutes = g_adapter->SelfExecutes();
    ID3D12GraphicsCommandList* list = nullptr;
    if (!selfExecutes)
    {
        std::string beginReason;
        if (!g_interop.Begin(beginReason)) { reason = beginReason; return false; }
        list = g_interop.List();
        if (list == nullptr)
        {
            reason = "the frame-generation command list is unavailable";
            return false;
        }
    }
    // Snapshot tracked states: transitions are recorded before submission, so a list that is never
    // submitted must not leave the slot claiming a state the GPU never entered.
    const D3D12_RESOURCE_STATES colorBefore = slot.color.state;
    const D3D12_RESOURCE_STATES depthBefore = slot.depth.state;
    const D3D12_RESOURCE_STATES motionBefore = slot.motion.state;
    const D3D12_RESOURCE_STATES hudlessBefore = slot.hudless.state;

    FgPresent present{};
    present.color = slot.color.d3d12.Get();
    present.hudless = slot.hudlessRequired ? slot.hudless.d3d12.Get() : nullptr;
    present.depth = slot.depth.d3d12.Get();
    present.motion = slot.motion.d3d12.Get();
    present.list = list;
    present.queue = g_interop.Queue();
    present.sourceFence = g_interop.SourceFence();
    present.sourceFenceValue = slot.sourceFenceValue;
    present.colorState = &slot.color.state;
    present.hudlessState = &slot.hudless.state;
    present.depthState = &slot.depth.state;
    present.motionState = &slot.motion.state;
    present.renderWidth = slot.renderWidth;
    present.renderHeight = slot.renderHeight;
    // The converted guide is render sized; the original URP allocation is never an SDK basis.
    present.motionWidth = slot.renderWidth;
    present.motionHeight = slot.renderHeight;
    present.outputWidth = slot.outputWidth;
    present.outputHeight = slot.outputHeight;
    present.sourceId = static_cast<uint32_t>(slot.frame.presentId & 0xffffffffull);
    present.frame = slot.frame;
    present.hudlessRequired = slot.hudlessRequired;
    // The multiplier snapshot belongs to this capture's configuration generation, never to the
    // configuration that may have replaced it while the slot was queued.
    present.multiplier = slot.multiplier;

    if (!selfExecutes)
    {
        Dx12Interop::Transition(list, slot.color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (slot.hudlessRequired)
            Dx12Interop::Transition(list, slot.hudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Dx12Interop::Transition(list, slot.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Dx12Interop::Transition(list, slot.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    TraceFgBoundary("record-begin", g_provider.load(), slot.frame.generation, slot.frame.frameIndex,
        selfExecutes ? "self-executes" : "transport-list");
    const bool recorded = g_adapter->Record(present, reason);
    TraceFgBoundary("record-end", g_provider.load(), slot.frame.generation, slot.frame.frameIndex,
        recorded ? "recorded" : "record-failed");
    if (!recorded) g_interop.LogValidationErrors("frame generation");
    if (!recorded && !selfExecutes)
    {
        // A transport-list provider that refused this packet must not have it submitted or
        // presented: Present would otherwise report success and the frame would be counted as a
        // generated/rendered pair the provider explicitly rejected. The list is never executed, so
        // the tracked states are restored from the snapshot taken before recording.
        slot.color.state = colorBefore;
        slot.depth.state = depthBefore;
        slot.motion.state = motionBefore;
        slot.hudless.state = hudlessBefore;
        if (reason.empty()) reason = "the frame-generation provider rejected this frame";
        return false;
    }
    if (!selfExecutes)
    {
        Dx12Interop::Transition(list, slot.motion, D3D12_RESOURCE_STATE_COMMON);
        Dx12Interop::Transition(list, slot.depth, D3D12_RESOURCE_STATE_COMMON);
        if (slot.hudlessRequired) Dx12Interop::Transition(list, slot.hudless, D3D12_RESOURCE_STATE_COMMON);
        Dx12Interop::Transition(list, slot.color, D3D12_RESOURCE_STATE_COMMON);
        std::string submitReason;
        const bool submitted = g_interop.Submit(submitReason, slot.sourceFenceValue);
        if (!submitted)
        {
            // The recorded barriers never executed, and the list cannot be discarded. Restore the
            // tracked states so the next capture does not issue an invalid transition.
            slot.color.state = colorBefore;
            slot.depth.state = depthBefore;
            slot.motion.state = motionBefore;
            slot.hudless.state = hudlessBefore;
            if (!recorded)
                reason = reason.empty() ? submitReason : reason;
            else
                reason = submitReason;
            return false;
        }
        present.submissionFence = g_interop.SubmissionFence();
        present.submissionValue = g_interop.SubmissionValue();
    }
    else if (!recorded)
    {
        // A self-executing provider that never submitted its list did not change any GPU state;
        // restore the tracked snapshot so the next capture transitions from the real state.
        slot.color.state = colorBefore;
        slot.depth.state = depthBefore;
        slot.motion.state = motionBefore;
        slot.hudless.state = hudlessBefore;
        return false;
    }
    std::string presentReason;
    const bool presented = g_adapter->Present(present, presentReason);
    // Read the count before returning: images already scanned out stay counted even when the final
    // real present failed.
    generatedCount = present.generatedCount;
    if (!presented)
    {
        reason = presentReason;
        return false;
    }
    return true;
}

bool SignalTransportCompletion(ID3D12Fence** fence, uint64_t* value, std::string& reason)
{
    if (!g_interop.Ready() || g_interop.Queue() == nullptr || g_interop.Device() == nullptr)
    {
        reason = "the frame-generation queue is unavailable for completion tracking";
        return false;
    }
    if (g_completionFence == nullptr)
    {
        if (FAILED(g_interop.Device()->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(&g_completionFence))))
        {
            reason = "the frame-generation completion fence could not be created";
            return false;
        }
    }
    const uint64_t signaled = ++g_completionValue;
    if (FAILED(g_interop.Queue()->Signal(g_completionFence.Get(), signaled)))
    {
        reason = "the frame-generation completion fence could not be signalled";
        return false;
    }
    *fence = g_completionFence.Get();
    *value = signaled;
    return true;
}

// Releases GPU-pending slots whose transport (or producer) fence completed. The retirement is
// published outside the slot lock.
void PumpCompletions()
{
    for (int32_t i = 0; i < kSlotCount; i++)
    {
        DeferredRetirement retirement{};
        bool faulted = false;
        std::string faultReason;
        {
            std::lock_guard<std::mutex> guard(g_slotMutex);
            CaptureSlot& slot = g_slots[i];
            if (slot.phase != SlotPhase::GpuPending) continue;
            const HRESULT removed = g_interop.Device() != nullptr
                ? g_interop.Device()->GetDeviceRemovedReason()
                : E_FAIL;
            if (FAILED(removed))
            {
                slot.phase = SlotPhase::Faulted;
                faulted = true;
                faultReason = "the frame-generation device was removed (0x" + HexStatus(removed) +
                    "); uncertain capture resources are retained";
                continue;
            }
            if (slot.completionFence == nullptr ||
                slot.completionFence->GetCompletedValue() < slot.completionValue)
                continue;
            retirement.publish = true;
            retirement.slotTag = slot.frame.slotTag;
            retirement.generation = slot.frame.generation;
            retirement.result = slot.pendingResult;
            ReleaseSlotLocked(slot);
        }
        if (faulted)
        {
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                SetState(BIGSCALE_FG_STATE_FAULTED, faultReason.c_str(), -1);
                Log("FG: %s", faultReason.c_str());
            }
            continue;
        }
        Publish(retirement);
    }
}

void PushWorker(CaptureSlot* slot)
{
    std::lock_guard<std::mutex> guard(g_workerMutex);
    const int32_t index = static_cast<int32_t>(slot - g_slots);
    for (uint32_t i = 0; i < g_workerQueueCount; i++)
        if (g_workerQueue[i] == index) return;
    if (g_workerQueueCount >= kSlotCount) return;
    g_workerQueue[g_workerQueueCount++] = index;
    if (g_workerEvent != nullptr) SetEvent(g_workerEvent);
}

DWORD WINAPI WorkerProc(void*)
{
    for (;;)
    {
        WaitForSingleObject(g_workerEvent, 100);
        if (g_workerStop.load()) break;
        for (;;)
        {
            if (g_adapterResetRequested.exchange(false))
            {
                std::string reason;
                if (!DestroyAdapter(reason))
                {
                    Log("FG: provider reset could not drain: %s", reason.c_str());
                    SetState(BIGSCALE_FG_STATE_FAULTED, reason.c_str(), -1);
                }
            }
            PumpCompletions();
            int32_t index = -1;
            {
                std::lock_guard<std::mutex> guard(g_workerMutex);
                if (g_workerQueueCount == 0) break;
                index = g_workerQueue[0];
                for (uint32_t i = 1; i < g_workerQueueCount; i++) g_workerQueue[i - 1] = g_workerQueue[i];
                g_workerQueueCount--;
            }
            if (index < 0 || index >= kSlotCount) continue;
            CaptureSlot& slot = g_slots[index];
            DeferredRetirement retirement{};
            bool claimed = false;
            {
                std::lock_guard<std::mutex> guard(g_slotMutex);
                if (slot.phase != SlotPhase::Ready) continue;
                bool stale = false;
                {
                    std::lock_guard<std::mutex> statusGuard(g_status.mutex);
                    // A queued slot for a superseded request (or one captured before Off/VR/menu) is
                    // retired here without touching a provider or a released resource.
                    stale = slot.frame.generation != g_status.generation ||
                        g_provider.load() == BIGSCALE_FG_PROVIDER_OFF || g_xrActive.load() ||
                        g_menuVisible.load();
                }
                if (stale)
                {
                    TraceFgBoundary("worker-stale", g_provider.load(), slot.frame.generation,
                        slot.frame.frameIndex, "queued-slot-superseded");
                    retirement = CancelSlotLocked(slot, -2);
                }
                else
                {
                    slot.phase = SlotPhase::Generating;
                    claimed = true;
                }
            }
            Publish(retirement);
            if (!claimed) continue;

            // Configure can change the request between the queue pop and provider work. Re-check it
            // before touching any provider or resource.
            {
                DeferredRetirement staleRetirement{};
                bool staleNow = false;
                {
                    std::lock_guard<std::mutex> guard(g_slotMutex);
                    bool current = false;
                    {
                        std::lock_guard<std::mutex> statusGuard(g_status.mutex);
                        current = slot.frame.generation == g_status.generation &&
                            g_provider.load() != BIGSCALE_FG_PROVIDER_OFF && !g_xrActive.load() &&
                            !g_menuVisible.load();
                    }
                    if (!current)
                    {
                        TraceFgBoundary("worker-stale", g_provider.load(), slot.frame.generation,
                            slot.frame.frameIndex, "recheck-superseded");
                        staleRetirement = CancelSlotLocked(slot, -2);
                        staleNow = true;
                    }
                }
                if (staleNow)
                {
                    Publish(staleRetirement);
                    continue;
                }
            }

            {
                std::lock_guard<std::mutex> guard(g_slotMutex);
                const BIGSCALE_FG_Frame& frame = slot.frame;
                if (g_frameHistory &&
                    (frame.frameIndex != g_lastFrameIndex + 1 || frame.cameraId != g_lastFrameCamera ||
                        frame.generation != g_lastFrameGeneration))
                {
                    slot.frame.flags |= BIGSCALE_FG_FLAG_RESET;
                }
            }
            std::string reason;
            uint32_t generatedCount = 0;
            const bool ok = RecordFrame(slot, reason, generatedCount);
            BIGSCALE_FG_Frame frame{};
            {
                std::lock_guard<std::mutex> guard(g_slotMutex);
                if (slot.phase != SlotPhase::Generating) continue;
                frame = slot.frame;
            }
            {
                std::lock_guard<std::mutex> guard(g_slotMutex);
                if (ok)
                {
                    g_frameHistory = true;
                    g_lastFrameIndex = slot.frame.frameIndex;
                    g_lastFrameCamera = slot.frame.cameraId;
                    g_lastFrameGeneration = slot.frame.generation;
                }
                else
                {
                    // The next accepted source frame has no proven predecessor.
                    g_frameHistory = false;
                }
            }
            const int32_t requested = g_provider.load();
            bool current = false;
            {
                std::lock_guard<std::mutex> statusGuard(g_status.mutex);
                // Only work of the locked requested generation/provider may count as this provider's
                // activity; an old provider's completion never does.
                current = frame.generation == g_status.generation && requested == g_status.requested &&
                    g_adapterProvider == requested && requested != BIGSCALE_FG_PROVIDER_OFF;
                if (current)
                {
                    // A real present is counted only when it actually succeeded; generated images
                    // counted by the provider were scanned out even when the later real present
                    // failed, so neither is fabricated nor erased.
                    if (ok)
                    {
                        g_status.lastSourceFrame = frame.frameIndex;
                        g_status.renderedPresents++;
                    }
                    g_status.generatedPresents += generatedCount;
                    if (ok && generatedCount > 0)
                    {
                        // A confirmed generated frame also clears a transient suspension; a menu or
                        // an XR session keeps its own explicit suspended state.
                        if (!g_xrActive.load() && !g_menuVisible.load() &&
                            (g_status.state == BIGSCALE_FG_STATE_WAITING_FIRST_GENERATED ||
                                g_status.state == BIGSCALE_FG_STATE_SUSPENDED))
                        {
                            g_status.state = BIGSCALE_FG_STATE_ACTIVE;
                            g_status.reason[0] = 0;
                        }
                    }
                }
            }
            if (ok)
            {
                if (current) ShowChildOnOwner(true);
            }
            else
            {
                bool refused = false;
                std::string refusalReason;
                {
                    std::lock_guard<std::mutex> guard(g_refusalMutex);
                    refused = g_adapterRefusedProvider == requested &&
                        g_adapterRefusedGeneration == frame.generation && !g_adapterRefusalReason.empty();
                    if (refused) refusalReason = g_adapterRefusalReason;
                }
                if (reason == "the presenter window is not ready")
                {
                    // The window is published by the game's own Present; the first capture can arrive
                    // one frame earlier. Defer instead of faulting.
                    SetState(BIGSCALE_FG_STATE_WAITING_WINDOW, reason.c_str());
                }
                else if (refused || g_adapterTeardownFailed)
                {
                    SetState(refused ? BIGSCALE_FG_STATE_UNSUPPORTED : BIGSCALE_FG_STATE_FAULTED,
                        refused ? refusalReason.c_str() : g_adapterTeardownReason.c_str(), -1);
                    ShowChildOnOwner(false);
                }
                else if (current)
                {
                    Log("FG: frame %d could not be generated: %s", frame.frameIndex, reason.c_str());
                    g_interop.LogValidationErrors("frame generation");
                    // A refused provider is not retried for every captured frame; a provider or
                    // generation change is what re-arms it.
                    {
                        // Keep the exact provider reason, not just the identity: the next
                        // EnsureAdapter must report the real refusal instead of a generic one.
                        std::lock_guard<std::mutex> guard(g_refusalMutex);
                        g_adapterRefusedProvider = requested;
                        g_adapterRefusedGeneration = frame.generation;
                        g_adapterRefusalReason = reason;
                        g_adapterRefusalRestart = false;
                    }
                    g_adapterResetRequested.store(true);
                    SetState(BIGSCALE_FG_STATE_FAULTED, reason.c_str(), -1);
                    ShowChildOnOwner(false);
                }
            }

            // Everything this provider submitted, including a partially submitted failure, is
            // ordered before a transport signal on the same queue. The slot only becomes reusable
            // when that signal completes; SDK Present returning is not GPU completion.
            std::string completionReason;
            ID3D12Fence* completionFence = nullptr;
            uint64_t completionValue = 0;
            const bool signaled = SignalTransportCompletion(&completionFence, &completionValue,
                completionReason);
            bool faulted = false;
            {
                std::lock_guard<std::mutex> guard(g_slotMutex);
                if (slot.phase != SlotPhase::Generating) continue;
                slot.terminal = true;
                if (signaled)
                {
                    slot.phase = SlotPhase::GpuPending;
                    slot.completionFence = completionFence;
                    slot.completionValue = completionValue;
                    slot.pendingResult = ok ? 0 : -1;
                }
                else
                {
                    // Fence signal failure means completion cannot be proven. Retain the slot rather
                    // than freeing resources the GPU may still be reading.
                    slot.phase = SlotPhase::Faulted;
                    faulted = true;
                }
            }
            if (faulted)
            {
                Log("FG: %s; the capture slot is retained.", completionReason.c_str());
                SetState(BIGSCALE_FG_STATE_FAULTED, completionReason.c_str(), -1);
            }
        }
    }
    return 0;
}

bool StartWorker()
{
    if (g_workerThread != nullptr) return true;
    g_workerEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_workerEvent == nullptr) return false;
    g_workerStop.store(false);
    g_workerThread = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
    if (g_workerThread == nullptr)
    {
        CloseHandle(g_workerEvent);
        g_workerEvent = nullptr;
        return false;
    }
    return true;
}

bool StopWorker()
{
    if (g_workerThread == nullptr) return true;
    g_workerStop.store(true);
    if (g_workerEvent != nullptr) SetEvent(g_workerEvent);
    const DWORD waited = WaitForSingleObject(g_workerThread, 10000);
    if (waited != WAIT_OBJECT_0) return false;
    CloseHandle(g_workerThread);
    g_workerThread = nullptr;
    if (g_workerEvent != nullptr)
    {
        CloseHandle(g_workerEvent);
        g_workerEvent = nullptr;
    }
    return true;
}

bool CaptureMatch(IDXGISwapChain* swapchain, UINT flags)
{
    if (swapchain == nullptr) return false;
    if (g_provider.load() == BIGSCALE_FG_PROVIDER_OFF) return false;
    if (g_xrActive.load() || g_menuVisible.load()) return false;
    if ((flags & DXGI_PRESENT_TEST) != 0) return false;
    if (swapchain == g_childSwapchain.load()) return false;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapchain->GetDesc(&desc)) || desc.OutputWindow == nullptr)
    {
        static bool reported = false;
        if (!reported) { reported = true; Log("FG: candidate swapchain has no describable output window."); }
        TraceFgBoundary("capture-match-rejected", g_provider.load(), 0u, -1, "no-output-window");
        return false;
    }
    // Only the window that was published as the game's parent may feed frame generation. Provider
    // proxies present on the child window and must pass straight through.
    if (desc.OutputWindow != g_parent.load())
    {
        // A rejection before a token is decoded has no generation or source frame to name.
        TraceFgBoundary("capture-match-rejected", g_provider.load(), 0u, -1,
            desc.OutputWindow == g_child.load() ? "provider-child-window" : "not-parent-window");
        return false;
    }
    ComPtr<ID3D11Device> device;
    if (FAILED(swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(device.GetAddressOf()))) ||
        !device)
        return false;
    ID3D11Device* captureDevice = g_captureDevice.load();
    if (captureDevice == nullptr || device.Get() != captureDevice)
    {
        static bool reported = false;
        if (!reported)
        {
            reported = true;
            Log("FG: candidate swapchain device does not match the capture device.");
        }
        return false;
    }
    // The game's untouched source chain is admitted whatever its buffer count and swap effect: only
    // buffer zero is read, and the provider's child swapchain has its own required shape. Record the
    // observed shape once per process so the production source is evidence, not an assumption.
    {
        static std::atomic<bool> sourceShapeReported{false};
        if (!sourceShapeReported.exchange(true))
            Log("FG: source swapchain buffers=%u swapEffect=%u flags=0x%x", desc.BufferCount,
                static_cast<unsigned>(desc.SwapEffect), desc.Flags);
    }
    return true;
}

bool PublishParent(IDXGISwapChain* swapchain)
{
    if (swapchain == nullptr) return false;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapchain->GetDesc(&desc)) || desc.OutputWindow == nullptr) return false;
    DWORD process = 0;
    GetWindowThreadProcessId(desc.OutputWindow, &process);
    if (process != GetCurrentProcessId()) return false;
    if (GetAncestor(desc.OutputWindow, GA_ROOT) != desc.OutputWindow) return false;
    HWND expected = desc.OutputWindow;
    HWND current = g_parent.load();
    if (current == expected) return true;
    if (current != nullptr && g_child.load() != nullptr)
    {
        // The game moved to another window; rebuild the presenter for the new one.
        DestroyChild();
        g_parent.store(nullptr);
    }
    g_parent.store(expected);
    return true;
}

// Copies the game backbuffer colour into the slot and signals the interop fence. The returned fence
// value is the exact final producer value for this capture.
bool CaptureColor(CaptureSlot& slot, ID3D11Device* device, ID3D11DeviceContext* context,
    IDXGISwapChain* swapchain, std::string& reason)
{
    ComPtr<ID3D11DeviceContext4> context4;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context4))) || !context4)
    {
        reason = "the game context is not an ID3D11DeviceContext4";
        return false;
    }
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || !backBuffer)
    {
        reason = "the game backbuffer could not be read";
        return false;
    }
    D3D11_TEXTURE2D_DESC backBufferDesc{};
    backBuffer->GetDesc(&backBufferDesc);
    if (backBufferDesc.Width != slot.outputWidth || backBufferDesc.Height != slot.outputHeight)
    {
        reason = "the frame-generation output extent changed";
        return false;
    }
    if (!g_captureState.Ensure(device, context))
    {
        reason = "the capture context state could not be created";
        return false;
    }
    std::string convertReason;
    {
        ContextState::Guard isolated(g_captureState);
        if (!isolated.Valid())
        {
            reason = "the capture context state could not be swapped in";
            return false;
        }
        std::lock_guard<std::mutex> guard(g_resourcesMutex);
        if (!g_resources.ConvertColor(context, slot.textures, backBuffer.Get(), slot.outputWidth,
                slot.outputHeight, convertReason))
        {
            reason = convertReason;
            return false;
        }
    }
    InteropLock interopLock;
    if (!g_interop.SignalFromD3D11(context4.Get(), reason, slot.sourceFenceValue)) return false;
    return true;
}

bool PresentSourceFrame(IDXGISwapChain* swapchain, std::string& reason)
{
    void* token = g_pendingToken.exchange(nullptr);
    if (token == nullptr)
    {
        reason = "no capture matched this presentation";
        TraceFgBoundary("present-token-missing", g_provider.load(), 0u, -1, "no-pending-capture");
        return false;
    }
    int32_t index = -1;
    uint64_t serial = 0;
    if (!DecodeToken(token, index, serial))
    {
        reason = "the pending capture token is invalid";
        return false;
    }
    DeferredRetirement retirement{};
    bool claimed = false;
    {
        std::lock_guard<std::mutex> guard(g_slotMutex);
        CaptureSlot& slot = g_slots[index];
        if (slot.phase == SlotPhase::AwaitingPresent && slot.serial == serial)
        {
            slot.phase = SlotPhase::PresentCapturing;
            claimed = true;
        }
        else if (slot.phase == SlotPhase::AwaitingPresent)
        {
            // A newer capture replaced this token; the old one is retired without touching the
            // replacement's payload.
            retirement = CancelSlotLocked(slot, -2);
        }
    }
    Publish(retirement);
    if (!claimed)
    {
        TraceFgBoundary("present-token-unclaimed", g_provider.load(), 0u, -1, "slot-not-awaiting-present");
        return false;
    }

    CaptureSlot& slot = g_slots[index];
    bool superseded = false;
    {
        std::lock_guard<std::mutex> statusGuard(g_status.mutex);
        superseded = slot.frame.generation != g_status.generation ||
            g_provider.load() == BIGSCALE_FG_PROVIDER_OFF || g_xrActive.load() || g_menuVisible.load();
    }
    if (superseded)
    {
        {
            std::lock_guard<std::mutex> guard(g_slotMutex);
            retirement = CancelSlotLocked(slot, -2);
        }
        Publish(retirement);
        return false;
    }

    ComPtr<ID3D11Device> device;
    if (FAILED(swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(device.GetAddressOf()))) ||
        !device)
    {
        reason = "the game device is unavailable";
        {
            std::lock_guard<std::mutex> guard(g_slotMutex);
            retirement = CancelSlotLocked(slot, -1);
        }
        Publish(retirement);
        return false;
    }
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context)
    {
        reason = "the game context is unavailable";
        {
            std::lock_guard<std::mutex> guard(g_slotMutex);
            retirement = CancelSlotLocked(slot, -1);
        }
        Publish(retirement);
        return false;
    }
    if (!CaptureColor(slot, device.Get(), context.Get(), swapchain, reason))
    {
        {
            std::lock_guard<std::mutex> guard(g_slotMutex);
            retirement = CancelSlotLocked(slot, -1);
        }
        Publish(retirement);
        return false;
    }
    bool ready = false;
    {
        std::lock_guard<std::mutex> guard(g_slotMutex);
        if (slot.phase == SlotPhase::PresentCapturing && slot.serial == serial)
        {
            slot.phase = SlotPhase::Ready;
            ready = true;
        }
    }
    if (!ready) return false;
    PushWorker(&slot);
    return true;
}

std::string g_lastSuspension;

void ReportSuspension(const std::string& reason)
{
    if (reason.empty() || reason == g_lastSuspension) return;
    g_lastSuspension = reason;
    Log("FG: source frame not generated: %s", reason.c_str());
}

bool PresentOwnedPath(IDXGISwapChain* swapchain, UINT flags, std::string& reason)
{
    if (!CaptureMatch(swapchain, flags)) return false;
    UpdateChildGeometry();
    if (!PresentSourceFrame(swapchain, reason))
    {
        // A present without a matching capture is the ordinary ordering gap right after capture is
        // re-armed (menu close, provider switch, resize). The next captured frame still generates, so
        // the state must not be downgraded to Suspended for it.
        if (reason == "no capture matched this presentation")
        {
            ReportSuspension(reason);
            return false;
        }
        WriteReason(reason.c_str());
        SetState(BIGSCALE_FG_STATE_SUSPENDED, reason.c_str());
        ReportSuspension(reason);
        return false;
    }
    g_lastSuspension.clear();
    return true;
}

HRESULT FinalPresent(IDXGISwapChain* swapchain, UINT flags)
{
    // The presenter child window shows the generated frames; the game swapchain is still presented
    // with sync 0 so Unity's cached frame-latency waitable keeps progressing.
    return g_presentOriginal != nullptr ? g_presentOriginal(swapchain, 0, flags) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* swapchain, UINT sync, UINT flags)
{
    if (g_presentOriginal == nullptr) return E_FAIL;
    EntryScope entry;
    if (!entry.entered) return g_presentOriginal(swapchain, sync, flags);
    ApplyChildHideRequest();
    PublishParent(swapchain);
    std::string reason;
    if (!PresentOwnedPath(swapchain, flags, reason)) return g_presentOriginal(swapchain, sync, flags);
    if (g_provider.load() != BIGSCALE_FG_PROVIDER_OFF) MarkerBoth(kMarkerRenderSubmitEnd, kMarkerPresentStart,
        g_markerFrame.load());
    const HRESULT result = FinalPresent(swapchain, flags);
    if (g_provider.load() != BIGSCALE_FG_PROVIDER_OFF) Marker(kMarkerPresentEnd, g_markerFrame.load());
    return result;
}

HRESULT STDMETHODCALLTYPE Present1Hook(IDXGISwapChain1* swapchain, UINT sync, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters)
{
    if (g_present1Original == nullptr) return E_FAIL;
    EntryScope entry;
    if (!entry.entered) return g_present1Original(swapchain, sync, flags, parameters);
    ApplyChildHideRequest();
    PublishParent(reinterpret_cast<IDXGISwapChain*>(swapchain));
    std::string reason;
    if (!PresentOwnedPath(reinterpret_cast<IDXGISwapChain*>(swapchain), flags, reason))
        return g_present1Original(swapchain, sync, flags, parameters);
    if (g_provider.load() != BIGSCALE_FG_PROVIDER_OFF) MarkerBoth(kMarkerRenderSubmitEnd, kMarkerPresentStart,
        g_markerFrame.load());
    // Preserve the original Present1 semantics with exactly one present call: the waitable only needs
    // that call to complete, and the generated frame is already on screen in the child presenter.
    const HRESULT result = g_present1Original(swapchain, 0, flags, parameters);
    if (g_provider.load() != BIGSCALE_FG_PROVIDER_OFF) Marker(kMarkerPresentEnd, g_markerFrame.load());
    return result;
}

HRESULT STDMETHODCALLTYPE ResizeBuffersHook(IDXGISwapChain* swapchain, UINT bufferCount, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags)
{
    EntryScope entry;
    if (entry.entered && swapchain != nullptr && swapchain != g_childSwapchain.load())
    {
        CancelOutstanding();
        // Slot textures are their own allocations, not views of the backbuffer: only the provider is
        // retired. A free slot recreates its shared set on its next captured extent.
        g_adapterResetRequested.store(true);
        SetState(BIGSCALE_FG_STATE_WAITING_SWAPCHAIN, "the game swapchain is being resized");
    }
    return g_resizeOriginal != nullptr ? g_resizeOriginal(swapchain, bufferCount, width, height, format, flags)
                                       : E_FAIL;
}

bool InstallHooks()
{
    if (g_installed.load()) return true;
    const MH_STATUS initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log("FG: MinHook initialisation failed (%d).", static_cast<int>(initialized));
        return false;
    }
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = GetDesktopWindow();
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<IDXGISwapChain> dummy;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &desc, &dummy, nullptr, nullptr, nullptr)) ||
        !dummy)
    {
        Log("FG: dummy swapchain creation failed; presentation hooks are unavailable.");
        return false;
    }
    void** vtable = *reinterpret_cast<void***>(dummy.Get());
    void* present = vtable[8];
    void* resize = vtable[13];
    void* present1 = vtable[22];
    dummy.Reset();

    const MH_STATUS presentStatus = MH_CreateHook(present, reinterpret_cast<void**>(&PresentHook),
        reinterpret_cast<void**>(&g_presentOriginal));
    const MH_STATUS resizeStatus = MH_CreateHook(resize, reinterpret_cast<void**>(&ResizeBuffersHook),
        reinterpret_cast<void**>(&g_resizeOriginal));
    const MH_STATUS present1Status = MH_CreateHook(present1, reinterpret_cast<void**>(&Present1Hook),
        reinterpret_cast<void**>(&g_present1Original));
    if (presentStatus != MH_OK || resizeStatus != MH_OK || present1Status != MH_OK)
    {
        Log("FG: Present/ResizeBuffers detours failed (present=%d resize=%d present1=%d).",
            static_cast<int>(presentStatus), static_cast<int>(resizeStatus), static_cast<int>(present1Status));
        return false;
    }
    const MH_STATUS enabled = MH_EnableHook(MH_ALL_HOOKS);
    if (enabled != MH_OK)
    {
        Log("FG: detours could not be enabled (%d).", static_cast<int>(enabled));
        return false;
    }
    g_installed.store(true);
    Log("FG: DXGI presentation detours installed.");
    return true;
}
}

bool FrameGeneration::EnsureInstalled() { return InstallHooks(); }

void FrameGeneration::SetDirectories(const std::wstring& runtimeDirectory, const std::wstring& dataDirectory)
{
    g_runtimeDirectory = runtimeDirectory;
    g_dataDirectory = dataDirectory;
}

void FrameGeneration::SetEngineVersion(const std::string& engineVersion)
{
    if (!engineVersion.empty()) g_engineVersion = engineVersion;
}

void FrameGeneration::Configure(const BIGSCALE_FG_Config& config)
{
    if (g_shuttingDown.load()) return;
    g_provider.store(config.provider);
    g_xrActive.store(config.xrActive != 0);
    g_menuVisible.store(config.menuVisible != 0);
    // The requested total multiplier is validated at the wire boundary so a native caller cannot
    // overrun the fixed intermediate arrays; provider capability is checked separately.
    const uint32_t multiplier = config.multiplier < 2 ? 2u : (config.multiplier > 6 ? 6u : config.multiplier);
    if (multiplier != g_multiplier.load())
        Log("FG: requested total multiplier %u (generation=%u, provider=%d)", multiplier, config.generation,
            config.provider);
    g_multiplier.store(multiplier);
    TraceFgBoundary("configure", config.provider, config.generation, -1,
        config.menuVisible != 0 ? "menu" : config.xrActive != 0 ? "xr" : config.provider == BIGSCALE_FG_PROVIDER_OFF ? "off" : "request");
    bool rearmed = true;
    {
        std::lock_guard<std::mutex> guard(g_status.mutex);
        // Opening/closing settings or an unrelated history tick must not overwrite a real provider
        // failure. Only an explicit provider/generation change re-arms the native state.
        rearmed = config.provider != g_status.requested || config.generation != g_status.generation;
        const bool failed = g_status.state == BIGSCALE_FG_STATE_FAULTED ||
            g_status.state == BIGSCALE_FG_STATE_UNSUPPORTED;
        // A provider change may never inherit the previous provider's discovered maximum.
        if (config.provider != g_status.requested) g_status.maxMultiplier = 0;
        g_status.requested = config.provider;
        g_status.generation = config.generation;
        const bool inactive = config.provider == BIGSCALE_FG_PROVIDER_OFF || config.xrActive != 0 ||
            config.menuVisible != 0;
        g_status.activeMultiplier = inactive ? 0 : multiplier;
        if (failed && !rearmed && config.provider != BIGSCALE_FG_PROVIDER_OFF)
            return;
    }
    {
        std::lock_guard<std::mutex> guard(g_refusalMutex);
        if (config.provider != g_adapterRefusedProvider || config.generation != g_adapterRefusedGeneration)
        {
            g_adapterRefusedProvider = BIGSCALE_FG_PROVIDER_OFF;
            g_adapterRefusedGeneration = 0;
            g_adapterRefusalReason.clear();
            g_adapterRefusalRestart = false;
        }
    }
    if (config.provider == BIGSCALE_FG_PROVIDER_OFF)
    {
        // The worker owns every Ready/Generating slot and the provider (including XeLL). It retires
        // queued work in order; releasing those textures here would free one a provider call may
        // still be reading. CPU-only captures have no such owner and are retired now.
        CancelOutstanding();
        g_adapterResetRequested.store(true);
        ShowChildOnOwner(false);
        SetState(BIGSCALE_FG_STATE_OFF, "");
        return;
    }
    if (config.xrActive != 0)
    {
        // XeLL and the adapter are destroyed by the worker after it drains, never from the
        // configuration thread while generation may be inside a provider call.
        CancelOutstanding();
        g_adapterResetRequested.store(true);
        ShowChildOnOwner(false);
        SetState(BIGSCALE_FG_STATE_SUSPENDED, "VR");
        return;
    }
    if (config.menuVisible != 0)
    {
        CancelOutstanding();
        ShowChildOnOwner(false);
        SetState(BIGSCALE_FG_STATE_SUSPENDED, "the settings menu is open");
        return;
    }
    if (!EnsureInstalled())
    {
        SetState(BIGSCALE_FG_STATE_UNSUPPORTED, "the DXGI presentation hooks could not be installed");
        return;
    }
    if (!StartWorker())
    {
        SetState(BIGSCALE_FG_STATE_UNSUPPORTED, "the frame-generation worker could not be started");
        return;
    }
    // Wait for the first capture to learn the device and window; nothing is taken over before it.
    if (!g_adapterExists.load())
    {
        SetState(BIGSCALE_FG_STATE_WAITING_SWAPCHAIN, "waiting for an eligible URP capture");
        return;
    }
    SetState(BIGSCALE_FG_STATE_WAITING_FIRST_GENERATED, "");
}

void* FrameGeneration::GameWindow() { return g_parent.load(); }

bool FrameGeneration::SetPresenterVisible(bool visible)
{
    if (g_shuttingDown.load()) return false;
    HWND child = g_child.load();
    if (child == nullptr) return false;
    if (GetWindowThreadProcessId(child, nullptr) != GetCurrentThreadId()) return false;
    ShowWindow(child, visible ? SW_SHOWNA : SW_HIDE);
    return true;
}

bool FrameGeneration::CreatePresenterWindow(void* parent)
{
    // A VR session owns its own compositor and stereo presentation. The transport may not create a
    // desktop presenter there even if a caller forgot to gate the request.
    if (g_shuttingDown.load() || g_xrActive.load() || parent == nullptr) return false;
    std::string reason;
    if (!CreateChild(static_cast<HWND>(parent), reason))
    {
        SetState(BIGSCALE_FG_STATE_UNSUPPORTED, reason.c_str());
        return false;
    }
    return true;
}

void* FrameGeneration::Submit(const BIGSCALE_FG_Frame& frame)
{
    EntryScope entry;
    if (!entry.entered) return nullptr;
    if (g_provider.load() == BIGSCALE_FG_PROVIDER_OFF || g_xrActive.load() || g_menuVisible.load())
        return nullptr;
    std::lock_guard<std::mutex> guard(g_slotMutex);
    CaptureSlot* slot = nullptr;
    for (CaptureSlot& candidate : g_slots)
    {
        if (candidate.phase == SlotPhase::Free)
        {
            slot = &candidate;
            break;
        }
    }
    // The ring stays at a fixed eight slots. Exhaustion drops this capture immediately; the game's
    // ordinary presentation is untouched and this is not a capture fault.
    if (slot == nullptr) return nullptr;
    if (g_nextSerial == 0 || g_nextSerial >= kMaxCaptureSerial)
    {
        static bool reported = false;
        if (!reported)
        {
            reported = true;
            Log("FG: capture token space exhausted; further captures are rejected.");
        }
        return nullptr;
    }
    // Submission copies CPU metadata only; texture capture and bound-RTV lookup run in RenderEvent.
    // The multiplier is part of that metadata so a queued capture keeps its own generation's value.
    slot->multiplier = g_multiplier.load();
    slot->frame = frame;
    slot->serial = g_nextSerial++;
    slot->phase = SlotPhase::Submitted;
    slot->sourceFenceValue = 0;
    slot->completionFence = nullptr;
    slot->completionValue = 0;
    slot->pendingResult = 0;
    slot->terminal = false;
    void* token = nullptr;
    if (!EncodeToken(slot->serial, static_cast<int32_t>(slot - g_slots), &token))
    {
        ReleaseSlotLocked(*slot);
        return nullptr;
    }
    return token;
}

void FrameGeneration::RenderEvent(int32_t eventId, void* data)
{
    if (eventId != 2 || data == nullptr) return;
    EntryScope entry;
    if (!entry.entered) return;
    int32_t index = -1;
    uint64_t serial = 0;
    if (!DecodeToken(data, index, serial)) return;
    BIGSCALE_FG_Frame frame{};
    {
        std::lock_guard<std::mutex> guard(g_slotMutex);
        CaptureSlot& slot = g_slots[index];
        // A replayed event for an invalidated serial never touches a replacement payload.
        if (slot.phase != SlotPhase::Submitted || slot.serial != serial) return;
        slot.phase = SlotPhase::Capturing;
        frame = slot.frame;
    }

    DeferredRetirement retirement{};
    // The requested provider is fixed for this event; the rejection lambda below names it without
    // re-reading the atomic.
    const int32_t provider = g_provider.load();
    auto cancel = [&](int32_t result)
    {
        std::lock_guard<std::mutex> guard(g_slotMutex);
        CaptureSlot& current = g_slots[index];
        if (current.serial != serial || current.phase != SlotPhase::Capturing) return;
        retirement = CancelSlotLocked(current, result);
    };
    auto cancelAndPublish = [&](int32_t result, const char* why)
    {
        // Test-only: names the first rejection of this generation. It changes no admission.
        TraceFgBoundary("render-rejected", provider, frame.generation, frame.frameIndex, why);
        cancel(result);
        Publish(retirement);
    };

    const bool hudlessRequired = provider == BIGSCALE_FG_PROVIDER_NVIDIA ||
        provider == BIGSCALE_FG_PROVIDER_ANR || provider == BIGSCALE_FG_PROVIDER_FSR;
    bool superseded = false;
    {
        std::lock_guard<std::mutex> statusGuard(g_status.mutex);
        superseded = frame.generation != g_status.generation || provider == BIGSCALE_FG_PROVIDER_OFF ||
            g_xrActive.load() || g_menuVisible.load();
    }
    if (superseded)
    {
        cancelAndPublish(-2, "generation-off-xr-or-menu");
        return;
    }
    TraceFgBoundary("render-admitted", provider, frame.generation, frame.frameIndex, "capture-sources");
    auto* depth = static_cast<ID3D11Texture2D*>(const_cast<void*>(frame.depth));
    auto* motion = static_cast<ID3D11Texture2D*>(const_cast<void*>(frame.motion));
    if (depth == nullptr || motion == nullptr || frame.renderWidth <= 0 || frame.renderHeight <= 0 ||
        frame.outputWidth <= 0 || frame.outputHeight <= 0)
    {
        SetState(BIGSCALE_FG_STATE_SUSPENDED, "frame generation needs URP depth and motion vectors");
        cancelAndPublish(-1, "depth-or-motion-missing");
        return;
    }
    ComPtr<ID3D11Device> device;
    depth->GetDevice(&device);
    if (!device)
    {
        cancelAndPublish(-1, "capture-device-missing");
        return;
    }
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context)
    {
        cancelAndPublish(-1, "capture-context-missing");
        return;
    }
    std::string reason;
    if (!EnsureInterop(device.Get(), reason))
    {
        SetState(BIGSCALE_FG_STATE_UNSUPPORTED, reason.c_str());
        cancelAndPublish(-1, "interop-unavailable");
        return;
    }
    g_captureDevice.store(device.Get());

    // An explicit graph texture always wins. Identifier-only built-in targets instead bind an RTV
    // before this render event on the same command buffer. Resolve it now, before swapping context
    // state; Submit only copied metadata and never inspected render-target bindings.
    ID3D11Texture2D* hudless = static_cast<ID3D11Texture2D*>(const_cast<void*>(frame.hudless));
    ComPtr<ID3D11Texture2D> boundHudless;
    if (hudless == nullptr && hudlessRequired && (frame.flags & BIGSCALE_FG_FLAG_BOUND_RENDER_TARGET) != 0)
    {
        ComPtr<ID3D11RenderTargetView> target;
        ComPtr<ID3D11Resource> resource;
        context->OMGetRenderTargets(1, &target, nullptr);
        if (target)
        {
            target->GetResource(&resource);
            if (resource) resource.As(&boundHudless);
        }
        if (boundHudless) hudless = boundHudless.Get();
    }
    if (hudlessRequired && hudless == nullptr)
    {
        SetState(BIGSCALE_FG_STATE_SUSPENDED,
            "the selected provider needs pre-overlay colour, but no HUD-less source was supplied");
        cancelAndPublish(-1, "hudless-source-missing");
        return;
    }

    {
        std::lock_guard<std::mutex> guard(g_slotMutex);
        CaptureSlot& slot = g_slots[index];
        if (slot.serial != serial || slot.phase != SlotPhase::Capturing) return;
        slot.renderWidth = static_cast<uint32_t>(frame.renderWidth);
        slot.renderHeight = static_cast<uint32_t>(frame.renderHeight);
        slot.outputWidth = static_cast<uint32_t>(frame.outputWidth);
        slot.outputHeight = static_cast<uint32_t>(frame.outputHeight);
        slot.hudlessRequired = hudlessRequired;
    }
    if (!EnsureSlotResources(g_slots[index], reason))
    {
        SetState(BIGSCALE_FG_STATE_SUSPENDED, reason.c_str());
        cancelAndPublish(-1, "slot-resources-unavailable");
        return;
    }
    if (!g_captureState.Ensure(device.Get(), context.Get()))
    {
        cancelAndPublish(-1, "capture-state-unavailable");
        return;
    }

    ComPtr<ID3D11DeviceContext4> context4;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context4))) || !context4)
    {
        cancelAndPublish(-1, "context4-unavailable");
        return;
    }
    bool converted = false;
    // An explicit pointer is a URP graph texture, whose rows are the complement of the presented
    // backbuffer's. The bound route resolves an RTV target, which already has screen rows.
    const bool hudlessGraphRows = frame.hudless != nullptr;
    if (hudlessRequired && hudless != nullptr)
    {
        static uint32_t loggedHudlessGeneration = 0xFFFFFFFFu;
        if (loggedHudlessGeneration != frame.generation)
        {
            loggedHudlessGeneration = frame.generation;
            D3D11_TEXTURE2D_DESC hudlessDesc{};
            hudless->GetDesc(&hudlessDesc);
            Log("FG: HUD-less route=%s format=%u source=%ux%u generation=%u",
                hudlessGraphRows ? "graph" : "bound-backbuffer", static_cast<unsigned>(hudlessDesc.Format),
                hudlessDesc.Width, hudlessDesc.Height, frame.generation);
        }
    }
    {
        ContextState::Guard isolated(g_captureState);
        if (!isolated.Valid())
        {
            SetState(BIGSCALE_FG_STATE_SUSPENDED, "the capture context state could not be swapped in");
            cancelAndPublish(-1, "context-state-swap-failed");
            return;
        }
        std::lock_guard<std::mutex> guard(g_resourcesMutex);
        CaptureSlot& slot = g_slots[index];
        converted = true;
        if (hudlessRequired)
            converted = g_resources.ConvertHudless(context.Get(), slot.textures, hudless, slot.outputWidth,
                slot.outputHeight, hudlessGraphRows, reason);
        if (converted)
            converted = g_resources.ConvertDepthMotion(context.Get(), slot.textures, depth, motion,
                slot.renderWidth, slot.renderHeight, reason);
    }
    if (!converted)
    {
        SetState(BIGSCALE_FG_STATE_SUSPENDED, reason.c_str());
        cancelAndPublish(-1, "capture-conversion-failed");
        return;
    }
    {
        InteropLock interopLock;
        uint64_t signaled = 0;
        if (!g_interop.SignalFromD3D11(context4.Get(), reason, signaled))
        {
            // The producer fence is the only proof that the queued copies finished. Without it the
            // slot must never be reused.
            {
                std::lock_guard<std::mutex> guard(g_slotMutex);
                CaptureSlot& slot = g_slots[index];
                if (slot.serial == serial) slot.phase = SlotPhase::Faulted;
            }
            SetState(BIGSCALE_FG_STATE_FAULTED, reason.c_str(), -1);
            return;
        }
        std::lock_guard<std::mutex> guard(g_slotMutex);
        CaptureSlot& slot = g_slots[index];
        if (slot.serial == serial) slot.sourceFenceValue = signaled;
    }
    if (provider == BIGSCALE_FG_PROVIDER_XESS)
        Marker(kMarkerRenderSubmitStart, g_markerFrame.load());

    void* token = nullptr;
    DeferredRetirement replaced{};
    bool published = false;
    {
        std::lock_guard<std::mutex> guard(g_slotMutex);
        CaptureSlot& slot = g_slots[index];
        if (slot.serial != serial || slot.phase != SlotPhase::Capturing) return;
        bool stillCurrent = false;
        {
            std::lock_guard<std::mutex> statusGuard(g_status.mutex);
            stillCurrent = slot.frame.generation == g_status.generation &&
                g_provider.load() == provider && !g_xrActive.load() && !g_menuVisible.load();
        }
        if (!stillCurrent)
        {
            retirement = CancelSlotLocked(slot, -2);
            published = true;
        }
        else
        {
            void* previous = g_pendingToken.exchange(nullptr);
            int32_t previousIndex = -1;
            uint64_t previousSerial = 0;
            if (previous != nullptr && DecodeToken(previous, previousIndex, previousSerial))
            {
                CaptureSlot& old = g_slots[previousIndex];
                if (old.phase == SlotPhase::AwaitingPresent && old.serial == previousSerial)
                    replaced = CancelSlotLocked(old, -2);
            }
            if (EncodeToken(serial, index, &token))
            {
                // The render event has copied every Unity-owned source. Only slot-owned shared
                // resources survive into worker work.
                slot.frame.depth = nullptr;
                slot.frame.motion = nullptr;
                slot.frame.hudless = nullptr;
                slot.phase = SlotPhase::AwaitingPresent;
                g_pendingToken.store(token);
                TraceFgBoundary("token-published", provider, frame.generation, frame.frameIndex,
                    "capture-fenced");
            }
            else
            {
                retirement = CancelSlotLocked(slot, -1);
                published = true;
            }
        }
    }
    (void)published;
    Publish(retirement);
    Publish(replaced);
}

void FrameGeneration::BeginFrame(uint64_t presentId)
{
    if (g_shuttingDown.load()) return;
    g_presentId.store(presentId);
    const int32_t provider = g_provider.load();
    const bool armed = provider == BIGSCALE_FG_PROVIDER_XESS && !g_xrActive.load() && !g_menuVisible.load() &&
        g_captureDevice.load() != nullptr;
    g_markerFrame.store(armed ? presentId : 0);
    if (!armed) return;
    std::string reason;
    if (!EnsureMarkers(reason))
    {
        g_markerFrame.store(0);
        return;
    }
    std::lock_guard<std::mutex> guard(g_xellMutex);
    g_xell.Sleep(static_cast<uint32_t>(presentId & 0xffffffffull));
    g_xell.Marker(static_cast<uint32_t>(presentId & 0xffffffffull), 0);
}

void FrameGeneration::EndSimulation(uint64_t presentId)
{
    Marker(kMarkerSimulationEnd, presentId);
}

void FrameGeneration::BeginRender(uint64_t presentId)
{
    Marker(kMarkerRenderSubmitStart, presentId);
}

void FrameGeneration::GetStatus(BIGSCALE_FG_Status* status)
{
    if (status == nullptr) return;
    // Snapshot the refusal identity without nesting the refusal and status locks.
    int32_t refusedProvider = BIGSCALE_FG_PROVIDER_OFF;
    uint32_t refusedGeneration = 0;
    bool refusedRestart = false;
    char refusalReason[BIGSCALE_FG_REASON_CAPACITY]{};
    {
        std::lock_guard<std::mutex> guard(g_refusalMutex);
        refusedProvider = g_adapterRefusedProvider;
        refusedGeneration = g_adapterRefusedGeneration;
        refusedRestart = g_adapterRefusalRestart;
        std::strncpy(refusalReason, g_adapterRefusalReason.c_str(), BIGSCALE_FG_REASON_CAPACITY - 1);
    }
    std::lock_guard<std::mutex> guard(g_status.mutex);
    // Only the provider's own refused-at-initialise latch marks this request Unsupported. Before a
    // rendering adapter exists the ordinary waiting state is correct: the HAGS prerequisite is
    // queried from the live adapter when the provider is actually initialised.
    const bool refused = refusedProvider != BIGSCALE_FG_PROVIDER_OFF &&
        refusedProvider == g_status.requested && refusedGeneration == g_status.generation &&
        refusalReason[0] != 0;
    std::memset(status, 0, sizeof(BIGSCALE_FG_Status));
    status->abi = BIGSCALE_FG_ABI_VERSION;
    status->size = sizeof(BIGSCALE_FG_Status);
    status->state = refused ? BIGSCALE_FG_STATE_UNSUPPORTED : g_status.state;
    status->requestedProvider = g_status.requested;
    status->activeProvider = g_status.active;
    status->generation = g_status.generation;
    status->lastResult = g_status.lastResult;
    status->lastSourceFrame = g_status.lastSourceFrame;
    status->parentHwnd = g_parent.load();
    status->renderedPresents = g_status.renderedPresents;
    status->generatedPresents = g_status.generatedPresents;
    status->activeMultiplier = g_status.activeMultiplier;
    status->maxMultiplier = g_status.maxMultiplier;
    status->processFamily = static_cast<uint32_t>(g_family.load());
    const char* reason = refused ? refusalReason : g_status.reason;
    std::strncpy(status->reason, reason, BIGSCALE_FG_REASON_CAPACITY - 1);
    status->reason[BIGSCALE_FG_REASON_CAPACITY - 1] = 0;
    // The typed restart requirement travels with the same refusal identity, so the UI never has to
    // infer it from SDK text.
    status->restartRequired = refused && refusedRestart ? 1u : 0u;
    const uint32_t count = g_status.retirementCount < BIGSCALE_FG_RETIREMENT_COUNT
        ? g_status.retirementCount
        : BIGSCALE_FG_RETIREMENT_COUNT;
    status->retirementCount = g_status.retirementCount;
    for (uint32_t i = 0; i < count; i++)
    {
        status->retirements[i].sequence = g_status.retirements[i].sequence;
        status->retirements[i].slotTag = g_status.retirements[i].slotTag;
        status->retirements[i].generation = g_status.retirements[i].generation;
        status->retirements[i].result = g_status.retirements[i].result;
    }
}

void FrameGeneration::Ack(uint32_t throughSequence)
{
    std::lock_guard<std::mutex> guard(g_status.mutex);
    uint32_t drop = 0;
    while (drop < g_status.retirementCount && g_status.retirements[drop].sequence <= throughSequence) drop++;
    if (drop == 0) return;
    const uint32_t remaining = g_status.retirementCount - drop;
    if (remaining > 0)
        std::memmove(g_status.retirements, g_status.retirements + drop, remaining * sizeof(RetirementRecord));
    std::memset(g_status.retirements + remaining, 0,
        (g_status.retirementCount - remaining) * sizeof(RetirementRecord));
    g_status.retirementCount = remaining;
}

namespace
{
bool SettleCompletions(std::string& reason)
{
    if (g_completionFence == nullptr || g_completionValue == 0) return true;
    if (g_interop.Device() != nullptr)
    {
        const HRESULT removed = g_interop.Device()->GetDeviceRemovedReason();
        if (FAILED(removed))
        {
            reason = "the frame-generation device was removed before completion (" + HexStatus(removed) + ")";
            return false;
        }
    }
    if (g_completionFence->GetCompletedValue() >= g_completionValue) return true;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr)
    {
        reason = "the completion wait event could not be created";
        return false;
    }
    if (FAILED(g_completionFence->SetEventOnCompletion(g_completionValue, event)))
    {
        CloseHandle(event);
        reason = "the completion fence could not be waited on";
        return false;
    }
    const DWORD waited = WaitForSingleObject(event, 5000);
    CloseHandle(event);
    if (waited != WAIT_OBJECT_0)
    {
        reason = "the frame-generation completion fence did not signal";
        return false;
    }
    return true;
}
}

void FrameGeneration::Shutdown()
{
    const auto shutdownStarted = std::chrono::steady_clock::now();
    Log("FG: shutdown begin");
    // The capture writer reads only provider-published resources, but its thread must be gone
    // before any provider tears its device down.
    FgCaptureShutdown();
    {
        std::unique_lock<std::mutex> lock(g_entryMutex);
        if (g_shuttingDown.load()) return;
        g_shuttingDown.store(true);
        if (!g_entryCv.wait_for(lock, std::chrono::seconds(5), [] { return g_activeEntries == 0; }))
        {
            Log("FG: shutdown could not quiesce capture/present entries; retaining frame-generation state.");
            return;
        }
    }
    if (!StopWorker())
    {
        // A worker inside a provider call must not have its state freed underneath it. The process
        // is ending, so retaining the adapter, XeLL, slots, child window and shared resources is the
        // only safe outcome.
        Log("FG: shutdown could not drain the provider worker; retaining provider state.");
        return;
    }
    std::string reason;
    if (!SettleCompletions(reason))
    {
        Log("FG: shutdown could not prove GPU completion: %s; retaining frame-generation state.",
            reason.c_str());
        SetState(BIGSCALE_FG_STATE_FAULTED, reason.c_str(), -1);
        return;
    }
    // The worker is stopped and every capture token's GPU work is complete; publish the remaining
    // retirements and release their slots before the device goes away.
    PumpCompletions();
    if (g_family.load() == RuntimeFamily::Anr)
    {
        // ANR has become resident: its hooks run until the process exits, so nothing they can reach
        // may be destroyed. The whole ownership graph is moved into deliberately leaked storage
        // instead of being released; the bounded retention is the approved tradeoff.
        RetainedGraph* retained = *RetainedGraphSlot();
        if (retained == nullptr)
        {
            retained = new RetainedGraph();
            *RetainedGraphSlot() = retained;
        }
        retained->adapter = g_adapter.release();
        retained->interop = std::make_unique<Dx12Interop>(std::move(g_interop));
        retained->completionFence = std::move(g_completionFence);
        retained->resources = std::move(g_resources);
        {
            std::lock_guard<std::mutex> guard(g_slotMutex);
            for (int32_t index = 0; index < kSlotCount; index++)
            {
                retained->slots[index] = std::move(g_slots[index]);
                g_slots[index] = CaptureSlot{};
            }
        }
        g_pendingToken.store(nullptr);
        RequestChildHidden();
        ShowChildOnOwner(false);
        SetState(BIGSCALE_FG_STATE_OFF, "");
        Log("FG: shutdown retained the process-resident ANR ownership graph until process exit.");
        return;
    }
    if (!DestroyAdapter(reason))
    {
        Log("FG: shutdown could not release the frame-generation provider: %s", reason.c_str());
        SetState(BIGSCALE_FG_STATE_FAULTED, reason.c_str(), -1);
        return;
    }
    {
        std::lock_guard<std::mutex> guard(g_slotMutex);
        g_pendingToken.store(nullptr);
        for (CaptureSlot& slot : g_slots)
        {
            // A faulted slot has no proven completion. Its resources are retained rather than
            // released underneath possible GPU work.
            if (slot.phase == SlotPhase::Faulted) continue;
            ReleaseSlotLocked(slot);
            slot.color.Reset();
            slot.hudless.Reset();
            slot.depth.Reset();
            slot.motion.Reset();
            slot.textures.Reset();
        }
        g_nextSerial = 1;
    }
    {
        std::lock_guard<std::mutex> guard(g_resourcesMutex);
        g_resources = FrameGenerationResources();
    }
    g_completionFence.Reset();
    g_completionValue = 0;
    DestroyChild();
    g_parent.store(nullptr);
    g_childSwapchain.store(nullptr);
    g_captureDevice.store(nullptr);
    SetState(BIGSCALE_FG_STATE_OFF, "");
    const auto shutdownElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - shutdownStarted).count();
    Log("FG: shutdown complete in %lld ms", static_cast<long long>(shutdownElapsed));
}
}
