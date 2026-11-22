#include "Bridge.h"
#include "ContextState.h"
#include "EyeResources.h"
#include "Log.h"
#include "NeuralRendering.h"
#include "NgxBackend.h"
#include "StageProbe.h"
#include "vr/XrPresentationProof.h"
#include "TexelReader.h"
#include "FrameGeneration.h"
#include "Upscaler.h"
#include "vr/VrFrameGeneration.h"
#if defined(BIGSCALE_WITH_FSR)
#include "providers/Fsr2Backend.h"
#endif
#include "providers/FsrDx12Backend.h"
#include "providers/XessBackend.h"

#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <optional>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;

static_assert(sizeof(BIGSCALE_Frame) == 192, "BIGSCALE_Frame must stay 192 bytes for the managed mirror");
static_assert(offsetof(BIGSCALE_Frame, color) == 80, "BIGSCALE_Frame::color offset");
static_assert(offsetof(BIGSCALE_Frame, output) == 104, "BIGSCALE_Frame::output offset");
static_assert(offsetof(BIGSCALE_Frame, jitterX) == 112, "BIGSCALE_Frame::jitterX offset");
static_assert(offsetof(BIGSCALE_Frame, deltaMilliseconds) == 128, "BIGSCALE_Frame::deltaMilliseconds offset");
static_assert(offsetof(BIGSCALE_Frame, resourceTag) == 144, "BIGSCALE_Frame::resourceTag offset");
static_assert(offsetof(BIGSCALE_Frame, upscaler) == 148, "BIGSCALE_Frame::upscaler offset");
static_assert(offsetof(BIGSCALE_Frame, cameraNear) == 152, "BIGSCALE_Frame::cameraNear offset");
static_assert(offsetof(BIGSCALE_Frame, preExposure) == 164, "BIGSCALE_Frame::preExposure offset");
static_assert(offsetof(BIGSCALE_Frame, fgCaptureTag) == 168, "BIGSCALE_Frame::fgCaptureTag offset");
static_assert(offsetof(BIGSCALE_Frame, neuralMask) == 176, "BIGSCALE_Frame::neuralMask offset");
static_assert(offsetof(BIGSCALE_Frame, neuralMaskWidth) == 184, "BIGSCALE_Frame::neuralMaskWidth offset");
static_assert(offsetof(BIGSCALE_Frame, neuralMaskHeight) == 188, "BIGSCALE_Frame::neuralMaskHeight offset");
static_assert(sizeof(BIGSCALE_ModeInfo) == 32, "BIGSCALE_ModeInfo must stay 32 bytes");
static_assert(sizeof(BIGSCALE_ViewInfo) == 24, "BIGSCALE_ViewInfo must stay 24 bytes");
static_assert(sizeof(BIGSCALE_Completion) == 76, "BIGSCALE_Completion must stay 76 bytes");
static_assert(offsetof(BIGSCALE_Completion, kind) == 4, "BIGSCALE_Completion::kind offset");
static_assert(offsetof(BIGSCALE_Completion, resourceTag) == 8, "BIGSCALE_Completion::resourceTag offset");
static_assert(offsetof(BIGSCALE_Completion, result) == 32, "BIGSCALE_Completion::result offset");
static_assert(offsetof(BIGSCALE_Completion, upscaler) == 52, "BIGSCALE_Completion::upscaler offset");
static_assert(offsetof(BIGSCALE_Completion, sliceCount) == 68, "BIGSCALE_Completion::sliceCount offset");
static_assert(sizeof(BIGSCALE_Status) == 1176, "BIGSCALE_Status must stay 1176 bytes");
static_assert(offsetof(BIGSCALE_Status, views) == 224, "BIGSCALE_Status::views offset");
static_assert(offsetof(BIGSCALE_Status, faultCount) == 280, "BIGSCALE_Status::faultCount offset");
static_assert(offsetof(BIGSCALE_Status, neuralState) == 284, "BIGSCALE_Status::neuralState offset");
static_assert(offsetof(BIGSCALE_Status, completionPublished) == 288, "BIGSCALE_Status::completionPublished offset");
static_assert(offsetof(BIGSCALE_Status, completions) == 296, "BIGSCALE_Status::completions offset");
static_assert(offsetof(BIGSCALE_Status, queryUpscaler) == 904, "BIGSCALE_Status::queryUpscaler offset");
static_assert(offsetof(BIGSCALE_Status, activeUpscaler) == 908, "BIGSCALE_Status::activeUpscaler offset");
static_assert(offsetof(BIGSCALE_Status, providerVersionId) == 912, "BIGSCALE_Status::providerVersionId offset");
static_assert(offsetof(BIGSCALE_Status, backendReason) == 920, "BIGSCALE_Status::backendReason offset");
static_assert(sizeof(BIGSCALE_NeuralConfig) == 44, "BIGSCALE_NeuralConfig must stay 44 bytes");
static_assert(offsetof(BIGSCALE_NeuralConfig, abi) == 0, "BIGSCALE_NeuralConfig::abi offset");
static_assert(offsetof(BIGSCALE_NeuralConfig, enabled) == 8, "BIGSCALE_NeuralConfig::enabled offset");
static_assert(offsetof(BIGSCALE_NeuralConfig, passes) == 20, "BIGSCALE_NeuralConfig::passes offset");
static_assert(offsetof(BIGSCALE_NeuralConfig, strength) == 24, "BIGSCALE_NeuralConfig::strength offset");
static_assert(offsetof(BIGSCALE_NeuralConfig, characterMask) == 36, "BIGSCALE_NeuralConfig::characterMask offset");
namespace
{
using namespace bigscale;

struct RingSlot
{
    BIGSCALE_Frame frame{};
    uint32_t resourceTag = 0;
    int32_t cameraId = 0;
    int32_t eye = 0;
    int32_t frameIndex = 0;
    int32_t historyEpoch = 0;
    int32_t operation = 0;
    std::atomic<bool> busy{false};
};

RingSlot g_ring[BIGSCALE_RING_CAPACITY];

std::mutex g_statusMutex;

// Per-output-size mode capability cache. Asymmetric eyes can report different sizes, so the
// managed side asks for the block that matches the size it is sizing for.
constexpr int32_t kModeCacheCapacity = 4;
struct ModeCacheEntry
{
    int32_t upscaler = BIGSCALE_UPSCALER_OFF;
    uint32_t width = 0;
    uint32_t height = 0;
    BIGSCALE_ModeInfo modes[BIGSCALE_MODE_COUNT]{};
    uint64_t providerVersionId = 0;
};
ModeCacheEntry g_modeCache[kModeCacheCapacity];
int32_t g_modeCacheCount = 0;
uint32_t g_modeOutputWidth = 0;
uint32_t g_modeOutputHeight = 0;
BIGSCALE_ViewInfo g_views[BIGSCALE_MAX_VIEWS];
ViewKey g_viewKeys[BIGSCALE_MAX_VIEWS];
uint32_t g_viewUsed = 0;
uint32_t g_probeCount = 0;
uint32_t g_faultCount = 0;
uint32_t g_bilinearCount = 0;

// Retained completions, oldest first. Managed acknowledges a sequence after consuming a snapshot, so
// a record is never overwritten while unread. Saturation is a fault, not a silent drop.
BIGSCALE_Completion g_completions[BIGSCALE_COMPLETION_QUEUE];
uint32_t g_completionCount = 0;
uint32_t g_completionNextSequence = 1;

// In-flight retirement queries. Each entry owns the immutable record it will publish once the GPU
// proves that its commands finished.
struct PendingRetirement
{
    ComPtr<ID3D11Query> query;
    BIGSCALE_Completion record{};
    bool used = false;
};
PendingRetirement g_pending[BIGSCALE_PENDING_COUNT];

std::atomic<int32_t> g_state{BIGSCALE_STATE_WAITING};
std::atomic<int32_t> g_lastResult{0};
std::atomic<int32_t> g_lastError{BIGSCALE_ERROR_NONE};
std::atomic<int32_t> g_layout{BIGSCALE_LAYOUT_2D};
std::atomic<int32_t> g_preset{static_cast<int32_t>(Preset::Quality)};
std::atomic<int32_t> g_activeUpscaler{BIGSCALE_UPSCALER_OFF};
std::atomic<uint64_t> g_providerVersionId{0};
char g_reason[BIGSCALE_REASON_CAPACITY]{};
std::mutex g_reasonMutex;
std::atomic<uint32_t> g_outputWidth{0};
std::atomic<uint32_t> g_outputHeight{0};
std::atomic<uint32_t> g_inputWidth{0};
std::atomic<uint32_t> g_inputHeight{0};
std::atomic<uint32_t> g_runtimeVersion{0};
std::atomic<uint64_t> g_evaluations{0};
std::atomic<bool> g_shutdownRequested{false};
std::atomic<bool> g_faultLatched{false};

// Neural Rendering configuration, copied verbatim from managed under g_neuralConfigMutex. The
// generation changes only when a field changes, so a later stage can rebuild its feature set without
// diffing values itself. Nothing here touches the GPU or the NGX SDK: configure must never block the
// managed main thread behind render work.
BIGSCALE_NeuralConfig g_neuralConfig{};
uint64_t g_neuralGeneration = 0;
std::mutex g_neuralConfigMutex;
std::atomic<int32_t> g_neuralState{BIGSCALE_NEURAL_OFF};

// Field-by-field rather than memcmp so uninitialized padding can never fabricate a change.
bool SameNeuralConfig(const BIGSCALE_NeuralConfig& left, const BIGSCALE_NeuralConfig& right)
{
    return left.abi == right.abi && left.size == right.size && left.enabled == right.enabled &&
        left.style == right.style && left.timing == right.timing && left.passes == right.passes &&
        left.strength == right.strength && left.detail == right.detail && left.tone == right.tone &&
        left.characterMask == right.characterMask && left.reserved == right.reserved;
}

#if defined(BIGSCALE_TESTING)
// Native-check-only seam: a queued evaluation forces this result while the real bridge baseline,
// fault, completion and retirement logic still runs. Absent from the packaged bridge entirely.
std::optional<int32_t> g_forcedEvaluationResult;
#endif

std::wstring g_runtimeDirectory;
std::wstring g_dataDirectory;
std::string g_engineVersion = "6000.3.17f1";
bool g_initialized = false;

std::unique_ptr<IUpscalerBackend> g_backend;
int32_t g_backendKind = -1;
EyeResources g_eyes;
// DLSS5 Neural Rendering. Render-thread owned: SetEnvironment from initialise, Configure from the
// managed main thread (config copy only), Before/AfterUpscale from HandleProbeOrEvaluate, Release
// from ReleaseDeviceState after a proven GPU idle. Nothing here may fault the selected upscaler.
bigscale::NeuralRenderingStage g_neural;

// Device bookkeeping shared with the render event. Only touched on the render thread.
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
bool g_deviceBound = false;

// Serialises render-thread entry: Unity may call the event from either the render thread or a
// worker depending on the platform. NGX and the immediate context must never overlap.
std::mutex g_renderMutex;

void SetError(BIGSCALE_Error error, int32_t result = 0)
{
    g_lastError.store(static_cast<int32_t>(error));
    // A success clears the previous result so a stale NGX code is never misread as this frame's.
    if (error == BIGSCALE_ERROR_NONE) g_lastResult.store(0);
    else if (result != 0) g_lastResult.store(result);
}

void Publish(int32_t state)
{
    g_state.store(state);
}

// Latches a fault. A latched fault is never cleared by a later probe; only an explicit recovery or a
// full device teardown clears it.
void LatchFault(BIGSCALE_Error error, int32_t result = 0)
{
    SetError(error, result);
    g_faultLatched.store(true);
    Publish(BIGSCALE_STATE_FAULTED);
}

// Appends one immutable record. Returns false when the retained queue is full.
bool PublishRecord(BIGSCALE_Completion record)
{
    std::lock_guard<std::mutex> guard(g_statusMutex);
    if (g_completionCount >= static_cast<uint32_t>(BIGSCALE_COMPLETION_QUEUE)) return false;
    record.sequence = g_completionNextSequence++;
    g_completions[g_completionCount++] = record;
    return true;
}

void FillCompletion(BIGSCALE_Completion& record, const RingSlot& slot, int32_t result)
{
    record.resourceTag = slot.resourceTag;
    record.cameraId = slot.cameraId;
    record.eye = slot.eye;
    record.frameIndex = slot.frameIndex;
    record.historyEpoch = slot.historyEpoch;
    record.operation = slot.operation;
    record.result = result;
    record.inputWidth = slot.frame.inputWidth;
    record.inputHeight = slot.frame.inputHeight;
    record.outputWidth = slot.frame.outputWidth;
    record.outputHeight = slot.frame.outputHeight;
    record.upscaler = slot.frame.upscaler;
    record.preset = slot.frame.preset;
    record.layout = slot.frame.layout;
    record.slice = slot.frame.slice;
    record.sliceCount = slot.frame.sliceCount;
}

void PublishCompletion(const RingSlot& slot, BIGSCALE_CompletionKind kind, int32_t result)
{
    BIGSCALE_Completion record{};
    record.kind = static_cast<uint32_t>(kind);
    FillCompletion(record, slot, result);
    if (PublishRecord(record)) return;
    Log("completion queue is full (%d retained); treating this as a renderer fault.",
        BIGSCALE_COMPLETION_QUEUE);
    LatchFault(BIGSCALE_ERROR_COMPLETION_OVERFLOW);
}

void ResetCompletions()
{
    std::lock_guard<std::mutex> guard(g_statusMutex);
    g_completionCount = 0;
    std::memset(g_completions, 0, sizeof(g_completions));
}

// Creates the retirement query for one packet. Returns false when completion cannot be proven, in
// which case the caller must fault instead of allowing resources to be freed.
bool BeginRetirement(const RingSlot& slot, int32_t result)
{
    if (!g_device || !g_context) return false;
    int freeIndex = -1;
    for (int i = 0; i < BIGSCALE_PENDING_COUNT; i++)
    {
        if (!g_pending[i].used)
        {
            freeIndex = i;
            break;
        }
    }
    if (freeIndex < 0)
    {
        Log("no retirement query slot is free (%d in flight).", BIGSCALE_PENDING_COUNT);
        return false;
    }
    D3D11_QUERY_DESC description{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> query;
    if (FAILED(g_device->CreateQuery(&description, &query)) || !query)
    {
        Log("retirement query creation failed.");
        return false;
    }
    g_context->End(query.Get());
    PendingRetirement& entry = g_pending[freeIndex];
    entry.query = query;
    entry.record = BIGSCALE_Completion{};
    entry.record.kind = BIGSCALE_COMPLETION_RETIRED;
    FillCompletion(entry.record, slot, result);
    entry.used = true;
    return true;
}

// Publishes every retirement whose query resolved. Render thread only.
void PollRetirements()
{
    if (!g_context) return;
    for (int i = 0; i < BIGSCALE_PENDING_COUNT; i++)
    {
        PendingRetirement& entry = g_pending[i];
        if (!entry.used) continue;
        BOOL complete = FALSE;
        const HRESULT result = g_context->GetData(entry.query.Get(), &complete, sizeof(complete), 0);
        if (FAILED(result))
        {
            // A broken query can never prove completion; keep the record and fault.
            Log("retirement query failed result=0x%08x; retaining resources.", static_cast<unsigned>(result));
            LatchFault(BIGSCALE_ERROR_RETIREMENT_FAILED);
            continue;
        }
        if (result != S_OK || !complete) continue;
        if (!PublishRecord(entry.record))
        {
            Log("completion queue overflow while retiring; treating this as a renderer fault.");
            LatchFault(BIGSCALE_ERROR_COMPLETION_OVERFLOW);
        }
        entry.used = false;
        entry.query.Reset();
    }
}

// Publishes every outstanding retirement after a proven GPU drain. Render thread only.
void FlushRetirements()
{
    for (int i = 0; i < BIGSCALE_PENDING_COUNT; i++)
    {
        PendingRetirement& entry = g_pending[i];
        if (!entry.used) continue;
        if (!PublishRecord(entry.record))
        {
            Log("completion queue overflow while draining; treating this as a renderer fault.");
            LatchFault(BIGSCALE_ERROR_COMPLETION_OVERFLOW);
            return;
        }
        entry.used = false;
        entry.query.Reset();
    }
}

bool DeviceLost()
{
    if (!g_device) return true;
    return g_device->GetDeviceRemovedReason() != S_OK;
}

bool BindDevice(ID3D11Resource* resource)
{
    ComPtr<ID3D11Device> device;
    resource->GetDevice(&device);
    if (!device) return false;
    if (g_deviceBound) return device.Get() == g_device.Get();
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context) return false;
    g_device = device;
    g_context = context;
    g_deviceBound = true;
    g_faultLatched.store(false);
    Log("bound device featureLevel=0x%x", static_cast<unsigned>(device->GetFeatureLevel()));
    return true;
}

bool WaitGpuIdle()
{
    if (!g_device || !g_context) return false;
    D3D11_QUERY_DESC description{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> query;
    if (FAILED(g_device->CreateQuery(&description, &query))) return false;
    g_context->End(query.Get());
    g_context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;)
    {
        BOOL complete = FALSE;
        const HRESULT result = g_context->GetData(query.Get(), &complete, sizeof(complete), 0);
        if (FAILED(result)) return false;
        if (result == S_OK && complete) return true;
        if (std::chrono::steady_clock::now() >= deadline)
        {
            Log("GPU idle wait timed out; retaining device-owned resources.");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Diagnostics only (BIGSCALE_FLAG_SAMPLE_INPUT): reads one texel of a frame texture back to the CPU so
// the native log can show whether the bridge was fed the world or a constant, and whether the engine
// output it wrote carries the same content. Delegates to the format-aware raw reader, because the
// engine allocates its colour targets typeless; a float-only table rejected those and reported the
// failed reads as zeroes.
static bool SampleFramePixel(ID3D11Texture2D* texture, UINT slice, UINT x, UINT y, float out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (texture == nullptr || !g_device || !g_context) return false;
    const UINT point[1][2] = {{x, y}};
    float texel[1][4]{};
    const char* reason = nullptr;
    if (!ReadTexels(g_context.Get(), texture, slice, point, 1, texel, &reason)) return false;
    for (int i = 0; i < 4; i++) out[i] = texel[0][i];
    return true;
}

uint32_t ViewIndex(const ViewKey& key, bool create)
{
    for (uint32_t i = 0; i < g_viewUsed; i++)
        if (g_viewKeys[i] == key) return i;
    if (!create) return BIGSCALE_MAX_VIEWS;
    if (g_viewUsed >= BIGSCALE_MAX_VIEWS)
    {
        // Recycle the oldest view instead of refusing every later camera. Its NGX feature and
        // scratch are retired behind a completion query, so nothing in flight is released.
        const ViewKey victim = g_viewKeys[0];
        if (g_backend) g_backend->Forget(victim);
        g_eyes.Forget(victim);
        for (uint32_t i = 1; i < g_viewUsed; i++)
        {
            g_viewKeys[i - 1] = g_viewKeys[i];
            g_views[i - 1] = g_views[i];
        }
        g_viewUsed--;
        Log("recycled view slot camera=%d eye=%d", victim.cameraId, victim.eye);
    }
    const uint32_t index = g_viewUsed++;
    g_viewKeys[index] = key;
    g_views[index] = BIGSCALE_ViewInfo{};
    g_views[index].eye = static_cast<uint32_t>(key.eye);
    g_views[index].lastFrame = -1;
    g_views[index].lastEpoch = -1;
    return index;
}

void ResetViewIdentity()
{
    g_viewUsed = 0;
    for (uint32_t i = 0; i < BIGSCALE_MAX_VIEWS; i++) g_views[i] = BIGSCALE_ViewInfo{};
}

// Publishes a short, safely terminated UTF-8 reason for the visible card. The full SDK text is
// already in the log; this copy is bounded by the fixed status buffer.
void SetReason(const char* reason)
{
    std::lock_guard<std::mutex> guard(g_reasonMutex);
    const size_t length = reason == nullptr ? 0 : std::strlen(reason);
    const size_t copy = length < (BIGSCALE_REASON_CAPACITY - 1) ? length : (BIGSCALE_REASON_CAPACITY - 1);
    if (copy > 0) std::memcpy(g_reason, reason, copy);
    g_reason[copy] = 0;
}

void ClearReason() { SetReason(""); }

std::unique_ptr<IUpscalerBackend> CreateBackend(int32_t kind)
{
    switch (kind)
    {
        case BIGSCALE_UPSCALER_DLSS: return std::make_unique<NgxBackend>();
        case BIGSCALE_UPSCALER_DLSS_PRESET_L: return std::make_unique<NgxBackend>(true);
#if defined(BIGSCALE_WITH_FSR)
        case BIGSCALE_UPSCALER_FSR2: return std::make_unique<Fsr2Backend>();
#else
        case BIGSCALE_UPSCALER_FSR2:
            SetReason("this build was configured without the AMD FSR 2 backend");
            return nullptr;
#endif
        case BIGSCALE_UPSCALER_FSR3:
            return std::make_unique<FsrDx12Backend>(UpscalerKind::Fsr3, 3, false);
        case BIGSCALE_UPSCALER_FSR4:
            return std::make_unique<FsrDx12Backend>(UpscalerKind::Fsr4, 4, true);
        case BIGSCALE_UPSCALER_XESS: return std::make_unique<XessBackend>();
        default: return nullptr;
    }
}

// Creates or replaces the selected backend on the render thread. Off releases it and owns nothing.
bool EnsureBackend(int32_t kind)
{
    if (kind == BIGSCALE_UPSCALER_OFF)
    {
        if (g_backend) g_backend->Release();
        g_backend.reset();
        g_backendKind = -1;
        g_activeUpscaler.store(BIGSCALE_UPSCALER_OFF);
        g_providerVersionId.store(0);
        return true;
    }
    if (g_backend && g_backendKind == kind) return true;
    auto created = CreateBackend(kind);
    if (!created)
    {
        SetError(BIGSCALE_ERROR_BACKEND_UNSUPPORTED);
        SetReason("this build has no implementation for the selected upscaler");
        Publish(BIGSCALE_STATE_UNSUPPORTED);
        return false;
    }
    created->SetRuntimeDirectory(g_runtimeDirectory);
    created->SetDataDirectory(g_dataDirectory);
    created->SetEngineVersion(g_engineVersion);
    if (g_backend) g_backend->Release();
    g_backend = std::move(created);
    g_backendKind = kind;
    return true;
}

void HandleSwitch(const BIGSCALE_Frame& frame)
{
    const int32_t kind = frame.upscaler;
    Log("upscaler switch requested: %d -> %d", g_backendKind, kind);
    const bool idle = !g_deviceBound || WaitGpuIdle();
    if (!idle)
    {
        LatchFault(BIGSCALE_ERROR_RETIREMENT_FAILED);
        SetReason("the upscaler switch could not prove the GPU idle");
        return;
    }
    PollRetirements();
    FlushRetirements();
    {
        std::lock_guard<std::mutex> guard(g_statusMutex);
        g_eyes.Release();
        if (g_backend) g_backend->Release();
        g_backend.reset();
        g_backendKind = -1;
        ResetViewIdentity();
        g_modeOutputWidth = 0;
        g_modeOutputHeight = 0;
        g_modeCacheCount = 0;
        std::memset(g_modeCache, 0, sizeof(g_modeCache));
    }
    g_activeUpscaler.store(kind);
    g_providerVersionId.store(0);
    g_faultLatched.store(false);
    SetError(BIGSCALE_ERROR_NONE);
    ClearReason();
    Publish(kind == BIGSCALE_UPSCALER_OFF ? BIGSCALE_STATE_DISABLED : BIGSCALE_STATE_WAITING);
    Log("upscaler switched to %d after a proven drain.", kind);
}

void HandleProbe(const BIGSCALE_Frame& frame)
{
    if (!BindDevice(static_cast<ID3D11Resource*>(frame.color)))
    {
        LatchFault(BIGSCALE_ERROR_DEVICE_UNAVAILABLE);
        return;
    }
    if (DeviceLost())
    {
        LatchFault(BIGSCALE_ERROR_DEVICE_LOST);
        return;
    }
    const int32_t kind = frame.upscaler;
    if (kind == BIGSCALE_UPSCALER_OFF)
    {
        // Off owns nothing at all; the managed side restores the stock renderer.
        EnsureBackend(kind);
        SetError(BIGSCALE_ERROR_NONE);
        ClearReason();
        Publish(BIGSCALE_STATE_DISABLED);
        return;
    }
    if (!EnsureBackend(kind))
    {
        Publish(BIGSCALE_STATE_UNSUPPORTED);
        return;
    }
    if (!g_backend->Attach(g_device.Get(), g_context.Get()))
    {
        SetError(BIGSCALE_ERROR_BACKEND_INIT_FAILED);
        SetReason(g_backend->LastReason().empty() ? "the selected backend could not attach" :
                                                   g_backend->LastReason().c_str());
        Publish(BIGSCALE_STATE_UNSUPPORTED);
        return;
    }
    if (!g_backend->Available())
    {
        SetError(BIGSCALE_ERROR_BACKEND_UNSUPPORTED);
        SetReason(g_backend->LastReason().empty() ? "the selected backend is unavailable on this device or runtime" :
                                                   g_backend->LastReason().c_str());
        Publish(BIGSCALE_STATE_UNSUPPORTED);
        return;
    }
    const uint32_t outputWidth = static_cast<uint32_t>(frame.outputWidth);
    const uint32_t outputHeight = static_cast<uint32_t>(frame.outputHeight);
    if (outputWidth == 0 || outputHeight == 0)
    {
        LatchFault(BIGSCALE_ERROR_RESOURCE_SIZE);
        return;
    }

    {
        std::lock_guard<std::mutex> guard(g_statusMutex);
        ModeCacheEntry* entry = nullptr;
        for (int32_t i = 0; i < g_modeCacheCount; i++)
            if (g_modeCache[i].upscaler == kind && g_modeCache[i].width == outputWidth &&
                g_modeCache[i].height == outputHeight)
                entry = &g_modeCache[i];
        if (entry == nullptr)
        {
            ModeRange ranges[kModeCount];
            uint32_t runtimeVersion = 0;
            if (!g_backend->QueryModes(outputWidth, outputHeight, ranges, runtimeVersion))
            {
                SetError(BIGSCALE_ERROR_OPTIMAL_SETTINGS_FAILED);
                SetReason(g_backend->LastReason().empty() ? "the SDK reported no supported preset at this size" :
                                                           g_backend->LastReason().c_str());
                Publish(BIGSCALE_STATE_UNSUPPORTED);
                g_runtimeVersion.store(runtimeVersion);
                return;
            }
            // Reuse the least recently sized slot when the cache is full; the provider is part of
            // the identity, so another backend's sizes are never presented as this one's.
            if (g_modeCacheCount < kModeCacheCapacity)
            {
                entry = &g_modeCache[g_modeCacheCount++];
            }
            else
            {
                entry = &g_modeCache[0];
                for (int32_t i = 1; i < kModeCacheCapacity; i++)
                    if (g_modeCache[i].width < entry->width) entry = &g_modeCache[i];
            }
            entry->upscaler = kind;
            entry->width = outputWidth;
            entry->height = outputHeight;
            entry->providerVersionId = g_backend->ProviderVersionId();
            for (int i = 0; i < kModeCount; i++)
            {
                BIGSCALE_ModeInfo& info = entry->modes[i];
                info = BIGSCALE_ModeInfo{};
                info.supported = ranges[i].supported ? 1u : 0u;
                info.optimalWidth = ranges[i].optimalWidth;
                info.optimalHeight = ranges[i].optimalHeight;
                info.minWidth = ranges[i].minWidth;
                info.minHeight = ranges[i].minHeight;
                info.maxWidth = ranges[i].maxWidth;
                info.maxHeight = ranges[i].maxHeight;
            }
            g_runtimeVersion.store(runtimeVersion);
            g_modeOutputWidth = outputWidth;
            g_modeOutputHeight = outputHeight;
            Log("mode cache published provider=%d output=%ux%u (entries=%d)", kind, outputWidth, outputHeight,
                g_modeCacheCount);
        }
        g_probeCount++;
    }

    g_outputWidth.store(outputWidth);
    g_outputHeight.store(outputHeight);
    g_activeUpscaler.store(kind);
    g_providerVersionId.store(g_backend->ProviderVersionId());
    g_runtimeVersion.store(g_backend->RuntimeVersion());
    SetError(BIGSCALE_ERROR_NONE);
    ClearReason();
    // A probe never clears a latched fault, and it must not downgrade a renderer that is already
    // producing evaluations: it only records that the capability query answered.
    if (!g_faultLatched.load() && g_state.load() != BIGSCALE_STATE_ACTIVE) Publish(BIGSCALE_STATE_WAITING);
}

// Drops every device-owned resource after a proven drain, so a later probe re-initialises NGX.
void ReleaseDeviceState(const char* reason)
{
    {
        std::lock_guard<std::mutex> guard(g_statusMutex);
        g_eyes.Release();
        if (g_backend) g_backend->Release();
        g_backend.reset();
        g_backendKind = -1;
        ResetViewIdentity();
        g_modeOutputWidth = 0;
        g_modeOutputHeight = 0;
        g_modeCacheCount = 0;
        std::memset(g_modeCache, 0, sizeof(g_modeCache));
    }
    g_providerVersionId.store(0);
    ClearReason();
    StageProbe::Reset();
    XrPresentationProof::Release();
    // Every caller of this function proved the GPU idle, so the stage may free its features,
    // textures, parameter maps and the NGX runtime it owns.
    g_neural.Release();
    g_device.Reset();
    g_context.Reset();
    g_deviceBound = false;
    g_lastError.store(BIGSCALE_ERROR_NONE);
    g_lastResult.store(0);
    Log("%s", reason);
}

void HandleRecovery()
{
    Log("recovery requested: draining before clearing the fault.");
    const bool idle = !g_deviceBound || WaitGpuIdle();
    if (!idle)
    {
        // The GPU did not become idle, so nothing may be freed and the latch stays.
        LatchFault(BIGSCALE_ERROR_RETIREMENT_FAILED);
        g_shutdownRequested.store(false);
        return;
    }
    PollRetirements();
    FlushRetirements();
    g_faultLatched.store(false);
    ReleaseDeviceState("recovery drained: device-owned state dropped for a fresh probe.");
    Publish(BIGSCALE_STATE_WAITING);
    g_shutdownRequested.store(false);
}

void HandleShutdown()
{
    Log("shutdown requested: draining native state.");
    const bool idle = !g_deviceBound || WaitGpuIdle();
    if (!idle)
    {
        // Never free device-owned resources while their work may still be queued, and never claim
        // that shutdown completed.
        LatchFault(BIGSCALE_ERROR_RETIREMENT_FAILED);
        Log("shutdown retained device-owned resources because the GPU did not become idle.");
        g_shutdownRequested.store(false);
        return;
    }
    PollRetirements();
    FlushRetirements();
    g_faultLatched.store(false);
    ReleaseDeviceState("native state released after a proven GPU drain.");
    Publish(BIGSCALE_STATE_SHUTDOWN);
    g_shutdownRequested.store(false);
}

bool ValidateEvaluate(const BIGSCALE_Frame& frame)
{
    if (frame.color == nullptr || frame.depth == nullptr || frame.motion == nullptr || frame.output == nullptr)
    {
        SetError(BIGSCALE_ERROR_RESOURCE_FORMAT);
        return false;
    }
    if (frame.inputWidth <= 0 || frame.inputHeight <= 0 || frame.outputWidth <= 0 || frame.outputHeight <= 0)
    {
        SetError(BIGSCALE_ERROR_RESOURCE_SIZE);
        return false;
    }
    if (frame.inputWidth > frame.outputWidth || frame.inputHeight > frame.outputHeight)
    {
        // DLSS Super Resolution never renders above the target; DLAA is the 1x boundary.
        SetError(BIGSCALE_ERROR_RESOURCE_SIZE);
        return false;
    }
    if (frame.inputX < 0 || frame.inputY < 0 || frame.outputX < 0 || frame.outputY < 0)
    {
        SetError(BIGSCALE_ERROR_RESOURCE_SIZE);
        return false;
    }
    if (frame.slice < 0 || frame.sliceCount < 1 || frame.slice >= frame.sliceCount)
    {
        SetError(BIGSCALE_ERROR_UNSUPPORTED_LAYOUT);
        return false;
    }
    if (frame.layout < BIGSCALE_LAYOUT_2D || frame.layout > BIGSCALE_LAYOUT_PACKED)
    {
        SetError(BIGSCALE_ERROR_UNSUPPORTED_LAYOUT);
        return false;
    }
    if (frame.motionWidth <= 0 || frame.motionHeight <= 0)
    {
        SetError(BIGSCALE_ERROR_RESOURCE_SIZE);
        return false;
    }
    return true;
}

void HandleProbeOrEvaluate(BIGSCALE_Frame& frame, RingSlot& slot)
{
    if (frame.operation == BIGSCALE_OP_PROBE || frame.operation == BIGSCALE_OP_RECOVERY)
    {
        if (frame.operation == BIGSCALE_OP_RECOVERY) HandleRecovery();
        else HandleProbe(frame);
        // Probe and recovery issue no GPU work, so their retirement is immediate.
        PublishCompletion(slot, BIGSCALE_COMPLETION_RETIRED, static_cast<int32_t>(g_lastError.load()));
        return;
    }
    if (!ValidateEvaluate(frame))
    {
        PublishCompletion(slot, BIGSCALE_COMPLETION_RETIRED, static_cast<int32_t>(g_lastError.load()));
        return;
    }
    if (g_faultLatched.load())
    {
        // A latched fault keeps the previous baseline; no new NGX work is attempted.
        Log("packet refused while faulted camera=%d eye=%d", frame.cameraId, frame.eye);
        PublishCompletion(slot, BIGSCALE_COMPLETION_RETIRED, static_cast<int32_t>(g_lastError.load()));
        return;
    }

    auto* colorResource = static_cast<ID3D11Resource*>(frame.color);
    auto* outputResource = static_cast<ID3D11Resource*>(frame.output);
    if (!BindDevice(colorResource))
    {
        LatchFault(BIGSCALE_ERROR_DEVICE_UNAVAILABLE);
        PublishCompletion(slot, BIGSCALE_COMPLETION_RETIRED, static_cast<int32_t>(g_lastError.load()));
        return;
    }
    if (DeviceLost())
    {
        LatchFault(BIGSCALE_ERROR_DEVICE_LOST);
        PublishCompletion(slot, BIGSCALE_COMPLETION_RETIRED, static_cast<int32_t>(g_lastError.load()));
        return;
    }
    ComPtr<ID3D11Device> outputDevice;
    outputResource->GetDevice(&outputDevice);
    if (!outputDevice || outputDevice.Get() != g_device.Get())
    {
        LatchFault(BIGSCALE_ERROR_DEVICE_UNAVAILABLE);
        PublishCompletion(slot, BIGSCALE_COMPLETION_RETIRED, static_cast<int32_t>(g_lastError.load()));
        return;
    }
    auto* colorTexture = static_cast<ID3D11Texture2D*>(colorResource);
    auto* depthTexture = static_cast<ID3D11Texture2D*>(static_cast<ID3D11Resource*>(frame.depth));
    auto* motionTexture = static_cast<ID3D11Texture2D*>(static_cast<ID3D11Resource*>(frame.motion));
    auto* outputTexture = static_cast<ID3D11Texture2D*>(outputResource);
    D3D11_TEXTURE2D_DESC colorDescription{};
    colorTexture->GetDesc(&colorDescription);

    EvaluationRequest request{};
    request.color = colorResource;
    request.depth = depthTexture;
    request.motion = motionTexture;
    request.output = outputResource;
    request.inputWidth = static_cast<uint32_t>(frame.inputWidth);
    request.inputHeight = static_cast<uint32_t>(frame.inputHeight);
    request.outputWidth = static_cast<uint32_t>(frame.outputWidth);
    request.outputHeight = static_cast<uint32_t>(frame.outputHeight);
    request.inputX = frame.inputX;
    request.inputY = frame.inputY;
    request.outputX = frame.outputX;
    request.outputY = frame.outputY;
    request.slice = frame.slice;
    request.sliceCount = frame.sliceCount;
    request.layout = frame.layout;
    request.preset = static_cast<Preset>(frame.preset);
    request.frameIndex = frame.frameIndex;
    request.historyEpoch = frame.historyEpoch;
    request.motionWidth = frame.motionWidth;
    request.motionHeight = frame.motionHeight;
    request.resourceTag = frame.resourceTag;
    request.jitterX = frame.jitterX;
    request.jitterY = frame.jitterY;
    request.motionScaleX = frame.motionScaleX;
    request.motionScaleY = frame.motionScaleY;
    request.deltaMilliseconds = frame.deltaMilliseconds;
    request.cameraNear = frame.cameraNear;
    request.cameraFar = frame.cameraFar;
    request.verticalFovRadians = frame.verticalFovRadians;
    request.reversedDepth = (frame.flags & BIGSCALE_FLAG_REVERSED_DEPTH) != 0;
    request.hdrColor = (frame.flags & BIGSCALE_FLAG_HDR_COLOR) != 0 || IsHdrColorFormat(colorDescription.Format);
    request.reset = (frame.flags & BIGSCALE_FLAG_RESET) != 0;
    request.bilinearComparison = frame.operation == BIGSCALE_OP_BILINEAR_COMPARISON;

    // The scratch/feature extent is exactly the evaluated render extent. Creating at the SDK's
    // maximum input while evaluating a smaller subrect mixes two extents: NGX then reconstructs a
    // region proportional to the creation size, which no longer covers the requested output.
    request.scratchWidth = request.inputWidth;
    request.scratchHeight = request.inputHeight;

    const ViewKey key{frame.cameraId, frame.eye};
    const uint32_t viewIndex = ViewIndex(key, true);
    if (viewIndex >= BIGSCALE_MAX_VIEWS)
    {
        SetError(BIGSCALE_ERROR_UNSUPPORTED_LAYOUT);
        PublishCompletion(slot, BIGSCALE_COMPLETION_RETIRED, static_cast<int32_t>(g_lastError.load()));
        return;
    }

    // Exact extent contract: the active viewport must exist inside the colour allocation and the
    // output viewport inside the output allocation. A missing region is a refused packet, not a
    // silent crop that later presents as a smaller image.
    D3D11_TEXTURE2D_DESC outputDescription{};
    outputTexture->GetDesc(&outputDescription);
    if (request.inputWidth == 0 || request.inputHeight == 0 ||
        request.outputWidth == 0 || request.outputHeight == 0 ||
        request.inputX + request.inputWidth > colorDescription.Width ||
        request.inputY + request.inputHeight > colorDescription.Height ||
        request.outputX + request.outputWidth > outputDescription.Width ||
        request.outputY + request.outputHeight > outputDescription.Height ||
        request.scratchWidth != request.inputWidth || request.scratchHeight != request.inputHeight)
    {
        SetError(BIGSCALE_ERROR_RESOURCE_FORMAT);
        Log("packet extent rejected camera=%d eye=%d input=%ux%u at (%d,%d) in colour %ux%u, "
            "output=%ux%u at (%d,%d) in %ux%u, scratch=%ux%u",
            key.cameraId, key.eye, request.inputWidth, request.inputHeight, request.inputX, request.inputY,
            colorDescription.Width, colorDescription.Height, request.outputWidth, request.outputHeight,
            request.outputX, request.outputY, outputDescription.Width, outputDescription.Height,
            request.scratchWidth, request.scratchHeight);
        {
            std::lock_guard<std::mutex> guard(g_statusMutex);
            g_views[viewIndex].failures++;
        }
        PublishCompletion(slot, BIGSCALE_COMPLETION_RETIRED, static_cast<int32_t>(g_lastError.load()));
        return;
    }

    // A packet recorded for a superseded provider is never executed on the new one. Its own graph
    // still expects an image, so the same-frame spatial baseline is written and the work retires
    // without faulting the new backend.
    // The explicit bilinear comparison is provider neutral: it never consults or attaches a backend,
    // so it cannot be stale against a provider that was never selected.
    const bool staleProvider = !request.bilinearComparison && frame.upscaler != g_backendKind;
    if (!staleProvider && !request.bilinearComparison)
    {
        if (!g_backend && !EnsureBackend(frame.upscaler))
        {
            PublishCompletion(slot, BIGSCALE_COMPLETION_RETIRED, static_cast<int32_t>(g_lastError.load()));
            return;
        }
        if (!g_backend->Attach(g_device.Get(), g_context.Get()))
        {
            LatchFault(BIGSCALE_ERROR_BACKEND_INIT_FAILED);
            SetReason(g_backend->LastReason().empty() ? "the selected backend could not attach" :
                                                       g_backend->LastReason().c_str());
            PublishCompletion(slot, BIGSCALE_COMPLETION_RETIRED, static_cast<int32_t>(g_lastError.load()));
            return;
        }
    }

    // Unity's immediate context is left exactly as it was. Only the compute slots this packet can
    // touch are saved and restored: swapping a whole device-context state on this context makes the
    // next frame's depth copy read nothing through the VR mod's D3D11 hooks.
    ComputeSlotState slots(g_context.Get());

    if (!g_eyes.Prepare(g_device.Get(), key, request, colorTexture, depthTexture, motionTexture))
    {
        Log("scratch preparation failed camera=%d eye=%d: %s", key.cameraId, key.eye, g_eyes.LastReason().c_str());
        LatchFault(BIGSCALE_ERROR_RESOURCE_FORMAT);
        {
            std::lock_guard<std::mutex> guard(g_statusMutex);
            g_views[viewIndex].failures++;
        }
        PublishCompletion(slot, BIGSCALE_COMPLETION_RETIRED, static_cast<int32_t>(g_lastError.load()));
        return;
    }

    // Every later read goes through private copies, so no view is created on an engine texture and
    // no pipeline slot ever holds one.
    if (!g_eyes.MirrorSources(g_context.Get(), key, request))
    {
        Log("source mirror failed camera=%d eye=%d: %s", key.cameraId, key.eye, g_eyes.LastReason().c_str());
        LatchFault(BIGSCALE_ERROR_RESOURCE_FORMAT);
        {
            std::lock_guard<std::mutex> guard(g_statusMutex);
            g_views[viewIndex].failures++;
        }
        PublishCompletion(slot, BIGSCALE_COMPLETION_RETIRED, static_cast<int32_t>(g_lastError.load()));
        return;
    }

    // Same-frame spatial baseline from the view's own colour region, before any conversion. Every
    // later failure therefore leaves a valid current-frame image in the engine output.
    if (!g_eyes.WriteSourceBaseline(g_context.Get(), key, request, outputTexture))
    {
        Log("spatial baseline failed camera=%d eye=%d: %s", key.cameraId, key.eye, g_eyes.LastReason().c_str());
        LatchFault(BIGSCALE_ERROR_BASELINE_FAILED);
        {
            std::lock_guard<std::mutex> guard(g_statusMutex);
            g_views[viewIndex].failures++;
        }
        PublishCompletion(slot, BIGSCALE_COMPLETION_RETIRED, static_cast<int32_t>(g_lastError.load()));
        return;
    }

    if (staleProvider)
    {
        Log("stale packet from upscaler %d retired without execution camera=%d eye=%d frame=%d",
            frame.upscaler, frame.cameraId, frame.eye, frame.frameIndex);
        PublishCompletion(slot, BIGSCALE_COMPLETION_EXECUTED, BIGSCALE_ERROR_STALE_PACKET);
        if (!BeginRetirement(slot, BIGSCALE_ERROR_STALE_PACKET)) LatchFault(BIGSCALE_ERROR_RETIREMENT_FAILED);
        return;
    }

    if (!g_eyes.Extract(g_context.Get(), key, request))
    {
        Log("eye extraction failed camera=%d eye=%d: %s", key.cameraId, key.eye, g_eyes.LastReason().c_str());
        LatchFault(BIGSCALE_ERROR_RESOURCE_FORMAT);
        {
            std::lock_guard<std::mutex> guard(g_statusMutex);
            g_views[viewIndex].failures++;
        }
        if (!BeginRetirement(slot, static_cast<int32_t>(g_lastError.load())))
            LatchFault(BIGSCALE_ERROR_RETIREMENT_FAILED);
        return;
    }

    // Neural Rendering inputs. The mask is a Unity-owned render-graph texture that the graph keeps
    // alive for the whole pass: this pointer is copied without AddRef and must never be released here
    // or by the packet release loop. A partially described mask counts as no mask.
    void* neuralMask = frame.neuralMask;
    uint32_t neuralMaskWidth = frame.neuralMaskWidth > 0 ? static_cast<uint32_t>(frame.neuralMaskWidth) : 0;
    uint32_t neuralMaskHeight = frame.neuralMaskHeight > 0 ? static_cast<uint32_t>(frame.neuralMaskHeight) : 0;
    if (neuralMask == nullptr || neuralMaskWidth == 0 || neuralMaskHeight == 0)
    {
        neuralMask = nullptr;
        neuralMaskWidth = 0;
        neuralMaskHeight = 0;
    }
    // Neural rendering is only defined for one desktop view per texture. A VR, single-pass stereo or
    // packed packet reports Unsupported on the stage (a state note only, no GPU or SDK work) and never
    // reaches Before/AfterUpscale below.
    const bool neuralLayout = frame.layout == BIGSCALE_LAYOUT_2D && frame.sliceCount == 1;
    if (!neuralLayout) g_neural.MarkUnsupported();

    int32_t packetResult = 0;
    if (request.bilinearComparison)
    {
        if (!g_eyes.SpatialUpsample(g_context.Get(), key, request) ||
            !g_eyes.CopyBack(g_context.Get(), key, request, outputTexture))
        {
            Log("bilinear comparison failed camera=%d eye=%d: %s", key.cameraId, key.eye, g_eyes.LastReason().c_str());
            LatchFault(BIGSCALE_ERROR_RENDER_EVENT_FAILED);
            packetResult = static_cast<int32_t>(g_lastError.load());
        }
    }
    else
    {
        // The converted scratch is the NGX input; the engine texture is only written by CopyBack.
        request.color = g_eyes.ColorInput(key);
        request.depth = g_eyes.DepthInput(key);
        request.motion = g_eyes.MotionInput(key);
        request.output = g_eyes.OutputTarget(key);
        if (request.color == nullptr || request.depth == nullptr || request.motion == nullptr ||
            request.output == nullptr)
        {
            Log("scratch binding incomplete camera=%d eye=%d", key.cameraId, key.eye);
            LatchFault(BIGSCALE_ERROR_RESOURCE_FORMAT);
            packetResult = static_cast<int32_t>(g_lastError.load());
        }
        else
        {
            bool created = false;
            // Pre-upscaling neural rendering replaces the backend's colour input with the neural
            // image. A null return means the stage did not run this frame (AfterUpscaling timing,
            // disabled, not ready yet or a failure it already reported), so the converted scratch
            // stays the backend input and super resolution is unaffected.
            if (neuralLayout)
            {
                if (ID3D11Resource* neuralColor = g_neural.BeforeUpscale(g_device.Get(), g_context.Get(), key,
                        request, neuralMask, neuralMaskWidth, neuralMaskHeight))
                    request.color = neuralColor;
            }
#if defined(BIGSCALE_TESTING)
            const bool forced = g_forcedEvaluationResult.has_value();
            const int32_t result = forced ? *g_forcedEvaluationResult : g_backend->Evaluate(key, request, created);
            if (forced)
            {
                g_forcedEvaluationResult.reset();
                Log("test seam: injected evaluation result 0x%08x", static_cast<unsigned>(result));
            }
#else
            const int32_t result = g_backend->Evaluate(key, request, created);
#endif
            // Post-upscaling neural rendering composites into request.output, which the backend just
            // produced, before CopyBack moves it into the engine texture. A false return means the
            // stage left request.output untouched, so the super-resolution image is written. The
            // result is deliberately ignored for control flow: a neural failure must never latch the
            // bridge fault, set g_lastError or change packetResult, and the stage owns its own state
            // and logging.
            if (result == 0 && neuralLayout)
                g_neural.AfterUpscale(g_device.Get(), g_context.Get(), key, request, neuralMask, neuralMaskWidth,
                    neuralMaskHeight);
            // The backend contract is zero success and any nonzero value a failure; a positive
            // FidelityFX error is just as authoritative as a negative NGX code.
            if (result != 0)
            {
                Log("evaluation failed camera=%d eye=%d result=0x%08x baseline=%s", key.cameraId, key.eye,
                    static_cast<unsigned>(result), "kept");
                LatchFault(BIGSCALE_ERROR_BACKEND_EVALUATE_FAILED, result);
                g_faultCount++;
                {
                    std::lock_guard<std::mutex> guard(g_statusMutex);
                    g_views[viewIndex].failures++;
                }
                packetResult = result;
            }
            else if (!g_eyes.CopyBack(g_context.Get(), key, request, outputTexture))
            {
                Log("write back failed camera=%d eye=%d: %s", key.cameraId, key.eye, g_eyes.LastReason().c_str());
                LatchFault(BIGSCALE_ERROR_RENDER_EVENT_FAILED);
                packetResult = static_cast<int32_t>(g_lastError.load());
            }
            else
            {
                g_evaluations.fetch_add(1);
                g_inputWidth.store(request.inputWidth);
                g_inputHeight.store(request.inputHeight);
                g_outputWidth.store(request.outputWidth);
                g_outputHeight.store(request.outputHeight);
                g_preset.store(frame.preset);
                g_layout.store(frame.layout);
                {
                    std::lock_guard<std::mutex> guard(g_statusMutex);
                    g_views[viewIndex].evaluations++;
                    g_views[viewIndex].lastFrame = frame.frameIndex;
                    g_views[viewIndex].lastEpoch = frame.historyEpoch;
                    if (created) g_views[viewIndex].resets++;
                }
                SetError(BIGSCALE_ERROR_NONE);
                if (!g_faultLatched.load()) Publish(BIGSCALE_STATE_ACTIVE);
                // VR frame generation interpolates from exactly the guide images this packet just
                // produced, so they are published before this frame's graphics work is handed off.
                if (VrFgWantsGuides())
                    VrFgPublishGuides(g_context.Get(), frame, request, g_eyes.DepthInput(key),
                                      g_eyes.MotionInput(key));
            }
        }
    }

    // Diagnostics only: a successful packet has just filled the engine output for this eye while the
    // converted input scratch still holds the same frame, which is the one point where the two
    // stages can be compared. Provider evaluation and the bilinear control both reach this call, and
    // a failed packet never stamps a successful-output marker.
    const bool stageMarker = (frame.flags & BIGSCALE_FLAG_STAGE_MARKER) != 0;
    if (packetResult == 0)
    {
        // Verification-only: reads the compositor's mirror image while the identity bars are stamped,
        // and again after they stop. Nothing runs unless this launch set BIGSCALE_XR_PROOF=1.
        XrPresentationProof::ObserveFrame(g_device.Get(), g_context.Get(), key.cameraId, key.eye,
            frame.frameIndex, stageMarker);
    }
    if (packetResult == 0 && (frame.flags & BIGSCALE_FLAG_STAGE_PROBE) != 0)
    {
        StageProbe::AfterWriteBack(g_device.Get(), g_context.Get(), key.cameraId, key.eye,
            static_cast<ID3D11Texture2D*>(g_eyes.ColorInput(key)), outputTexture,
            static_cast<uint32_t>(request.slice), static_cast<uint32_t>(request.outputX),
            static_cast<uint32_t>(request.outputY), request.outputWidth, request.outputHeight, stageMarker);
    }

    // Diagnostics only: the same two textures, sampled once at the centre. This distinguishes a
    // constant camera image from a correct one without writing any file or marker.
    if (packetResult == 0 && (frame.flags & BIGSCALE_FLAG_SAMPLE_INPUT) != 0)
    {
        uint32_t evaluations = 0;
        {
            std::lock_guard<std::mutex> guard(g_statusMutex);
            if (viewIndex < BIGSCALE_MAX_VIEWS) evaluations = g_views[viewIndex].evaluations;
        }
        if (evaluations <= 3 || evaluations % 120 == 0)
        {
            // Centre plus two inset corners: a constant image gives identical values, while real
            // geometry or UI produces different ones even when the centre itself is a flat surface.
            const float fx[3] = {0.5f, 0.25f, 0.75f};
            const float fy[3] = {0.5f, 0.25f, 0.75f};
            float input[3][4]{};
            float output[3][4]{};
            int inputValid = 0;
            int outputValid = 0;
            auto* inputTexture = static_cast<ID3D11Texture2D*>(g_eyes.ColorInput(key));
            for (int i = 0; i < 3; i++)
            {
                if (SampleFramePixel(inputTexture, 0, static_cast<UINT>(request.inputWidth * fx[i]),
                        static_cast<UINT>(request.inputHeight * fy[i]), input[i]))
                    inputValid++;
                if (SampleFramePixel(outputTexture, static_cast<UINT>(request.slice),
                        static_cast<UINT>(request.outputX) + static_cast<UINT>(request.outputWidth * fx[i]),
                        static_cast<UINT>(request.outputY) + static_cast<UINT>(request.outputHeight * fy[i]),
                        output[i]))
                    outputValid++;
            }
            float depth[4]{};
            const bool depthValid = SampleFramePixel(static_cast<ID3D11Texture2D*>(g_eyes.DepthInput(key)), 0,
                request.inputWidth / 2, request.inputHeight / 2, depth);
            // The guide the provider is actually given, at the same point: a zero or inverted field here
            // is the difference between real temporal accumulation and a single-frame resize.
            float motion[4]{};
            const bool motionValid = SampleFramePixel(static_cast<ID3D11Texture2D*>(g_eyes.MotionInput(key)), 0,
                request.inputWidth / 2, request.inputHeight / 2, motion);

            // Proof runs measure the whole guide, not one texel: a field that is garbage away from the
            // centre makes the provider reject its own history while every centre sample looks fine.
            float motionMin = 0.0f;
            float motionMax = 0.0f;
            float motionMean = 0.0f;
            float motionMeanX = 0.0f;
            float motionMeanY = 0.0f;
            float depthMin = 0.0f;
            float depthMax = 0.0f;
            bool guideValid = false;
            if ((frame.flags & BIGSCALE_FLAG_STAGE_PROBE) != 0)
            {
                constexpr int grid = 8;
                UINT points[grid * grid][2]{};
                for (int gy = 0; gy < grid; gy++)
                    for (int gx = 0; gx < grid; gx++)
                    {
                        points[gy * grid + gx][0] = static_cast<UINT>((gx + 0.5) * request.inputWidth / grid);
                        points[gy * grid + gx][1] = static_cast<UINT>((gy + 0.5) * request.inputHeight / grid);
                    }
                const char* reason = nullptr;
                float texels[grid * grid][4]{};
                auto* motionTexture = static_cast<ID3D11Texture2D*>(g_eyes.MotionInput(key));
                if (motionTexture != nullptr &&
                    bigscale::ReadTexels(g_context.Get(), motionTexture, 0, points, grid * grid, texels, &reason))
                {
                    float minimum = FLT_MAX;
                    float maximum = -FLT_MAX;
                    double sum = 0.0;
                    double sumX = 0.0;
                    double sumY = 0.0;
                    for (int i = 0; i < grid * grid; i++)
                    {
                        const float x = texels[i][0] * request.motionScaleX;
                        const float y = texels[i][1] * request.motionScaleY;
                        const float magnitude = std::sqrt(x * x + y * y);
                        minimum = std::min(minimum, magnitude);
                        maximum = std::max(maximum, magnitude);
                        sum += magnitude;
                        sumX += x;
                        sumY += y;
                    }
                    motionMin = minimum;
                    motionMax = maximum;
                    motionMean = static_cast<float>(sum / (grid * grid));
                    motionMeanX = static_cast<float>(sumX / (grid * grid));
                    motionMeanY = static_cast<float>(sumY / (grid * grid));
                    float depthTexels[grid * grid][4]{};
                    auto* depthTexture = static_cast<ID3D11Texture2D*>(g_eyes.DepthInput(key));
                    if (depthTexture != nullptr &&
                        bigscale::ReadTexels(g_context.Get(), depthTexture, 0, points, grid * grid, depthTexels,
                            &reason))
                    {
                        float dMin = FLT_MAX;
                        float dMax = -FLT_MAX;
                        for (int i = 0; i < grid * grid; i++)
                        {
                            dMin = std::min(dMin, depthTexels[i][0]);
                            dMax = std::max(dMax, depthTexels[i][0]);
                        }
                        depthMin = dMin;
                        depthMax = dMax;
                        guideValid = true;
                    }
                }
            }
            if (guideValid)
                Log("guide camera=%d eye=%d eval=%u mvpx min=%.2f max=%.2f mean=%.2f meanxy=(%.2f,%.2f) "
                    "depth min=%.6f max=%.6f",
                    key.cameraId, key.eye, evaluations, motionMin, motionMax, motionMean, motionMeanX, motionMeanY,
                    depthMin, depthMax);
            Log("sample camera=%d eye=%d eval=%u valid=%d/%d/%d depth=%.6f mv=(%.6f,%.6f) "
                "inC=(%.3f,%.3f,%.3f) inTL=(%.3f,%.3f,%.3f) inBR=(%.3f,%.3f,%.3f) "
                "outC=(%.3f,%.3f,%.3f) outTL=(%.3f,%.3f,%.3f) outBR=(%.3f,%.3f,%.3f)",
                key.cameraId, key.eye, evaluations, inputValid, outputValid, depthValid ? 1 : 0, depth[0],
                motionValid ? motion[0] : 0.0f, motionValid ? motion[1] : 0.0f,
                input[0][0], input[0][1], input[0][2], input[1][0], input[1][1], input[1][2], input[2][0],
                input[2][1], input[2][2], output[0][0], output[0][1], output[0][2], output[1][0], output[1][1],
                output[1][2], output[2][0], output[2][1], output[2][2]);
        }
    }

    // The packet ran; its GPU commands are now queued. Retirement is what may verify activity or
    // release a resource, so a missing query is a fault rather than an assumption.
    PublishCompletion(slot, BIGSCALE_COMPLETION_EXECUTED, packetResult);
    if (!BeginRetirement(slot, packetResult))
    {
        LatchFault(BIGSCALE_ERROR_RETIREMENT_FAILED);
    }
}

void __stdcall RenderEvent(int32_t eventId, void* data)
{
    if (eventId == 2)
    {
        FrameGeneration::RenderEvent(eventId, data);
        return;
    }
    if (eventId != 1 || data == nullptr) return;
    auto* slot = static_cast<RingSlot*>(data);
    std::lock_guard<std::mutex> render(g_renderMutex);
    try
    {
        PollRetirements();
        switch (slot->frame.operation)
        {
            case BIGSCALE_OP_PROBE:
            case BIGSCALE_OP_RECOVERY:
            case BIGSCALE_OP_BILINEAR_COMPARISON:
            case BIGSCALE_OP_EVALUATE:
                HandleProbeOrEvaluate(slot->frame, *slot);
                break;
            case BIGSCALE_OP_SHUTDOWN:
                HandleShutdown();
                break;
            case BIGSCALE_OP_SWITCH:
                HandleSwitch(slot->frame);
                PublishCompletion(*slot, BIGSCALE_COMPLETION_RETIRED, static_cast<int32_t>(g_lastError.load()));
                break;
            default:
                Log("unknown operation %d", slot->frame.operation);
                break;
        }
        // A shutdown request made from the managed main thread drains here, on the render thread,
        // after the packet that was already queued.
        if (g_shutdownRequested.load() && slot->frame.operation != BIGSCALE_OP_SHUTDOWN) HandleShutdown();
    }
    catch (...)
    {
        Log("unhandled native exception; integration faulted.");
        LatchFault(BIGSCALE_ERROR_RENDER_EVENT_FAILED);
    }
    // Only the four engine textures were AddRef'd by QueueFrame, so only those are released here.
    // The neural mask is a managed render-graph texture for this pass: never AddRef'd, never released.
    for (void** resource : {reinterpret_cast<void**>(&slot->frame.color), reinterpret_cast<void**>(&slot->frame.depth),
             reinterpret_cast<void**>(&slot->frame.motion), reinterpret_cast<void**>(&slot->frame.output)})
    {
        if (*resource != nullptr) static_cast<ID3D11Resource*>(*resource)->Release();
        *resource = nullptr;
    }
    slot->busy = false;
}
}

BIGSCALE_EXPORT uint32_t BIGSCALE_GetAbiVersion() { return BIGSCALE_ABI_VERSION; }

BIGSCALE_EXPORT uint32_t BIGSCALE_GetFrameSize() { return sizeof(BIGSCALE_Frame); }

BIGSCALE_EXPORT uint32_t BIGSCALE_GetStatusSize() { return sizeof(BIGSCALE_Status); }

BIGSCALE_EXPORT uint32_t BIGSCALE_GetModeInfoSize() { return sizeof(BIGSCALE_ModeInfo); }

BIGSCALE_EXPORT uint32_t BIGSCALE_GetViewInfoSize() { return sizeof(BIGSCALE_ViewInfo); }

BIGSCALE_EXPORT uint32_t BIGSCALE_GetCompletionSize() { return sizeof(BIGSCALE_Completion); }

namespace
{
template <size_t N>
uint32_t CopyLayout(const uint32_t (&values)[N], uint32_t* offsets, uint32_t count)
{
    if (offsets != nullptr)
        for (uint32_t i = 0; i < count && i < N; i++) offsets[i] = values[i];
    return static_cast<uint32_t>(N);
}
}

BIGSCALE_EXPORT uint32_t BIGSCALE_GetFrameLayout(uint32_t* offsets, uint32_t count)
{
    const uint32_t values[] = {
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, abi)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, size)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, cameraId)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, eye)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, frameIndex)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, historyEpoch)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, operation)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, preset)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, inputWidth)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, inputHeight)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, outputWidth)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, outputHeight)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, inputX)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, inputY)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, outputX)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, outputY)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, slice)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, sliceCount)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, flags)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, layout)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, color)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, depth)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, motion)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, output)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, jitterX)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, jitterY)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, motionScaleX)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, motionScaleY)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, deltaMilliseconds)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, motionWidth)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, motionHeight)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, reserved)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, resourceTag)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, upscaler)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, cameraNear)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, cameraFar)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, verticalFovRadians)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, preExposure)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, fgCaptureTag)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, neuralMask)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, neuralMaskWidth)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Frame, neuralMaskHeight)),
    };
    return CopyLayout(values, offsets, count);
}

BIGSCALE_EXPORT uint32_t BIGSCALE_GetCompletionLayout(uint32_t* offsets, uint32_t count)
{
    const uint32_t values[] = {
        static_cast<uint32_t>(offsetof(BIGSCALE_Completion, sequence)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Completion, kind)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Completion, resourceTag)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Completion, cameraId)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Completion, eye)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Completion, frameIndex)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Completion, historyEpoch)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Completion, operation)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Completion, result)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Completion, inputWidth)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Completion, inputHeight)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Completion, outputWidth)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Completion, outputHeight)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Completion, upscaler)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Completion, preset)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Completion, layout)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Completion, slice)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Completion, sliceCount)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Completion, reserved)),
    };
    return CopyLayout(values, offsets, count);
}

BIGSCALE_EXPORT uint32_t BIGSCALE_GetStatusLayout(uint32_t* offsets, uint32_t count)
{
    const uint32_t values[] = {
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, abi)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, size)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, state)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, lastResult)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, lastError)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, runtimeVersion)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, layout)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, preset)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, outputWidth)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, outputHeight)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, inputWidth)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, inputHeight)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, totalEvaluations)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, probeCount)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, modeCount)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, modes[0])),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, modes[1])),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, modes[2])),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, modes[3])),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, modes[4])),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, views[0])),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, views[1])),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, viewCount)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, bilinearComparisons)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, faultCount)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, neuralState)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, completionPublished)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, completionBase)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, completions[0])),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, completions[1])),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, completions[2])),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, completions[3])),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, completions[4])),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, completions[5])),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, completions[6])),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, completions[7])),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, queryUpscaler)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, activeUpscaler)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, providerVersionId)),
        static_cast<uint32_t>(offsetof(BIGSCALE_Status, backendReason)),
    };
    return CopyLayout(values, offsets, count);
}

BIGSCALE_EXPORT int32_t BIGSCALE_Initialize(const wchar_t* runtimeDirectory, const wchar_t* dataDirectory)
{
    std::lock_guard<std::mutex> guard(g_renderMutex);
    g_runtimeDirectory = runtimeDirectory != nullptr ? runtimeDirectory : L"";
    g_dataDirectory = dataDirectory != nullptr ? dataDirectory : L"";
    bigscale::SetLogDirectory(g_dataDirectory);
    if (g_runtimeDirectory.empty() || g_dataDirectory.empty())
    {
        Log("initialise rejected: runtime and data directories are both required.");
        return 0;
    }
    if (g_backend) g_backend->SetRuntimeDirectory(g_runtimeDirectory);
    if (g_backend) g_backend->SetDataDirectory(g_dataDirectory);
    bigscale::FrameGeneration::SetDirectories(g_runtimeDirectory, g_dataDirectory);
    bigscale::FrameGeneration::SetEngineVersion(g_engineVersion);
    g_initialized = true;
    Log("BigScale native bridge initialised abi=%u frameSize=%zu statusSize=%zu ring=%d retainedCompletions=%d",
        BIGSCALE_ABI_VERSION, sizeof(BIGSCALE_Frame), sizeof(BIGSCALE_Status), BIGSCALE_RING_CAPACITY,
        BIGSCALE_COMPLETION_QUEUE);
    g_neural.SetEnvironment(g_runtimeDirectory, g_dataDirectory, g_engineVersion);
    return 1;
}

BIGSCALE_EXPORT void BIGSCALE_SetEngineVersion(const char* engineVersion)
{
    if (engineVersion == nullptr) return;
    g_engineVersion = engineVersion;
    if (g_backend) g_backend->SetEngineVersion(g_engineVersion);
    bigscale::FrameGeneration::SetEngineVersion(g_engineVersion);
    g_neural.SetEnvironment(g_runtimeDirectory, g_dataDirectory, g_engineVersion);
}

// Calling this with 0 before BIGSCALE_Initialize prevents native.log from being created at all: the
// flag is remembered and SetLogDirectory only opens the file when logging is enabled.
BIGSCALE_EXPORT void BIGSCALE_SetNativeLog(int32_t enabled) { bigscale::SetLogEnabled(enabled != 0); }

BIGSCALE_EXPORT void* BIGSCALE_GetRenderEvent() { return reinterpret_cast<void*>(&RenderEvent); }

BIGSCALE_EXPORT void* BIGSCALE_GetBoundDevice() { return g_device.Get(); }

BIGSCALE_EXPORT const wchar_t* BIGSCALE_GetRuntimeDirectory() { return g_runtimeDirectory.c_str(); }

BIGSCALE_EXPORT void* BIGSCALE_QueueFrame(const BIGSCALE_Frame* frame)
{
    if (!g_initialized || frame == nullptr) return nullptr;
    if (frame->abi != BIGSCALE_ABI_VERSION || frame->size != sizeof(BIGSCALE_Frame))
    {
        SetError(BIGSCALE_ERROR_ABI_MISMATCH);
        return nullptr;
    }
    if (frame->preset < BIGSCALE_PRESET_QUALITY || frame->preset > BIGSCALE_PRESET_NATIVE_AA ||
        frame->operation < 0 || frame->operation > BIGSCALE_OP_SWITCH ||
        frame->upscaler < BIGSCALE_UPSCALER_OFF || frame->upscaler > BIGSCALE_UPSCALER_DLSS_PRESET_L)
    {
        SetError(BIGSCALE_ERROR_RESOURCE_FORMAT);
        return nullptr;
    }
    const bool needsResources = frame->operation == BIGSCALE_OP_EVALUATE ||
        frame->operation == BIGSCALE_OP_BILINEAR_COMPARISON || frame->operation == BIGSCALE_OP_PROBE;
    if (needsResources && frame->color == nullptr)
    {
        SetError(BIGSCALE_ERROR_RESOURCE_FORMAT);
        return nullptr;
    }
    for (int i = 0; i < BIGSCALE_RING_CAPACITY; i++)
    {
        RingSlot& slot = g_ring[i];
        bool expected = false;
        if (!slot.busy.compare_exchange_strong(expected, true)) continue;
        slot.frame = *frame;
        slot.resourceTag = frame->resourceTag;
        slot.cameraId = frame->cameraId;
        slot.eye = frame->eye;
        slot.frameIndex = frame->frameIndex;
        slot.historyEpoch = frame->historyEpoch;
        slot.operation = frame->operation;
        // slot.frame = *frame above also copies neuralMask, deliberately without an AddRef: the mask
        // is a Unity render-graph texture that the graph owns for the whole pass, not a resource whose
        // lifetime this ring extends. Never add it to this AddRef pair or to RenderEvent's release.
        for (void** resource : {reinterpret_cast<void**>(&slot.frame.color), reinterpret_cast<void**>(&slot.frame.depth),
                 reinterpret_cast<void**>(&slot.frame.motion), reinterpret_cast<void**>(&slot.frame.output)})
        {
            if (*resource != nullptr) static_cast<ID3D11Resource*>(*resource)->AddRef();
        }
        return &slot;
    }
    SetError(BIGSCALE_ERROR_RING_FULL);
    g_faultCount++;
    Publish(BIGSCALE_STATE_FAULTED);
    Log("render packet ring is full (%d slots busy); treating as a renderer fault.", BIGSCALE_RING_CAPACITY);
    return nullptr;
}

BIGSCALE_EXPORT int32_t BIGSCALE_ConfigureNeuralRendering(const BIGSCALE_NeuralConfig* config)
{
    if (config == nullptr || config->abi != BIGSCALE_NEURAL_ABI_VERSION ||
        config->size != sizeof(BIGSCALE_NeuralConfig))
    {
        return BIGSCALE_ERROR_ABI_MISMATCH;
    }
    std::lock_guard<std::mutex> guard(g_neuralConfigMutex);
    if (!SameNeuralConfig(g_neuralConfig, *config)) g_neuralGeneration++;
    g_neuralConfig = *config;
    // The stage copies the block under its own mutex and reports the state it reached from that copy.
    // A disabled feature must not run at all; an accepted enabled configuration waits for the first
    // successfully evaluated frame. Values are stored unclamped and the stage reports RuntimeMissing,
    // Failed and Unsupported from its own observation of the runtime and the packet. No GPU work runs
    // here, so the managed main thread is never blocked behind render or SDK work.
    g_neural.Configure(g_neuralConfig);
    g_neuralState.store(g_neural.State());
    return 0;
}

BIGSCALE_EXPORT int32_t BIGSCALE_GetStatus(BIGSCALE_Status* status)
{
    if (status == nullptr) return 0;
    // In fields: the provider and output size whose mode block the caller wants. Read before the
    // snapshot is overwritten. Zero falls back to the most recently published size.
    const int32_t wantedUpscaler = status->queryUpscaler;
    const uint32_t wantedWidth = status->outputWidth;
    const uint32_t wantedHeight = status->outputHeight;
    std::lock_guard<std::mutex> guard(g_statusMutex);
    std::memset(status, 0, sizeof(BIGSCALE_Status));
    status->abi = BIGSCALE_ABI_VERSION;
    status->size = sizeof(BIGSCALE_Status);
    status->state = g_state.load();
    status->lastResult = g_lastResult.load();
    status->lastError = g_lastError.load();
    status->runtimeVersion = g_runtimeVersion.load();
    status->layout = g_layout.load();
    status->preset = g_preset.load();
    status->outputWidth = g_outputWidth.load();
    status->outputHeight = g_outputHeight.load();
    status->inputWidth = g_inputWidth.load();
    status->inputHeight = g_inputHeight.load();
    status->totalEvaluations = g_evaluations.load();
    status->probeCount = g_probeCount;
    status->activeUpscaler = g_activeUpscaler.load();
    status->providerVersionId = g_providerVersionId.load();
    {
        std::lock_guard<std::mutex> reasonGuard(g_reasonMutex);
        std::memcpy(status->backendReason, g_reason, sizeof(g_reason));
    }
    status->backendReason[BIGSCALE_REASON_CAPACITY - 1] = 0;
    status->queryUpscaler = wantedUpscaler;

    const ModeCacheEntry* block = nullptr;
    if (wantedWidth != 0 && wantedHeight != 0)
    {
        for (int32_t i = 0; i < g_modeCacheCount; i++)
            if (g_modeCache[i].width == wantedWidth && g_modeCache[i].height == wantedHeight &&
                (wantedUpscaler == BIGSCALE_UPSCALER_OFF || g_modeCache[i].upscaler == wantedUpscaler))
                block = &g_modeCache[i];
    }
    else if (g_modeOutputWidth != 0)
    {
        for (int32_t i = 0; i < g_modeCacheCount; i++)
            if (g_modeCache[i].width == g_modeOutputWidth && g_modeCache[i].height == g_modeOutputHeight &&
                g_modeCache[i].upscaler == g_activeUpscaler.load())
                block = &g_modeCache[i];
    }
    if (block != nullptr)
    {
        status->modeCount = BIGSCALE_MODE_COUNT;
        std::memcpy(status->modes, block->modes, sizeof(block->modes));
        if (block->providerVersionId != 0) status->providerVersionId = block->providerVersionId;
    }
    else if (wantedWidth != 0 || wantedHeight != 0)
    {
        // Preserve the requested size so the caller can tell an unanswered query from a match.
        status->outputWidth = wantedWidth;
        status->outputHeight = wantedHeight;
    }
    std::memcpy(status->views, g_views, sizeof(g_views));
    status->viewCount = g_viewUsed;
    status->bilinearComparisons = g_bilinearCount;
    status->faultCount = g_faultCount;
    // The stage owns the state machine: this snapshot must report what the stage actually reached,
    // including RuntimeMissing, Failed and Unsupported, not the state a configure call published.
    status->neuralState = g_neural.State();
    // Oldest retained records first; managed acknowledges the newest it consumed.
    status->completionPublished = g_completionCount;
    status->completionBase = g_completionCount > 0 ? g_completions[0].sequence : 0;
    const uint32_t count = g_completionCount < static_cast<uint32_t>(BIGSCALE_COMPLETION_COUNT)
        ? g_completionCount
        : static_cast<uint32_t>(BIGSCALE_COMPLETION_COUNT);
    for (uint32_t i = 0; i < count; i++) status->completions[i] = g_completions[i];
    return 1;
}

BIGSCALE_EXPORT void BIGSCALE_AckCompletions(uint32_t throughSequence)
{
    std::lock_guard<std::mutex> guard(g_statusMutex);
    uint32_t drop = 0;
    while (drop < g_completionCount && g_completions[drop].sequence <= throughSequence) drop++;
    if (drop == 0) return;
    const uint32_t remaining = g_completionCount - drop;
    if (remaining > 0)
        std::memmove(g_completions, g_completions + drop, remaining * sizeof(BIGSCALE_Completion));
    std::memset(g_completions + remaining, 0, (g_completionCount - remaining) * sizeof(BIGSCALE_Completion));
    g_completionCount = remaining;
}

BIGSCALE_EXPORT void BIGSCALE_RequestShutdown() { g_shutdownRequested.store(true); }

#if defined(BIGSCALE_TESTING)
BIGSCALE_EXPORT void BIGSCALE_TestForceNextEvaluationResult(int32_t result)
{
    g_forcedEvaluationResult = result;
}
#endif
