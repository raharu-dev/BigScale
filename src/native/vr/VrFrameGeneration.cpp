#include "VrFrameGeneration.h"

#include "../Bridge.h"
#include "../Dx12Interop.h"
#include "../Log.h"
#include "VrFsrInterpolator.h"

#include <openvr.h>
#include <MinHook.h>

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace bigscale
{
namespace
{
constexpr uint32_t kSubmitEntries = 3;
constexpr uint32_t kGuideEntries = 2;
constexpr uint32_t kPoseHistory = 4;
constexpr uint32_t kQueueWaitMs = 50;
constexpr uint32_t kInteropTimeoutMs = 2000;
constexpr uint64_t kSummaryInterval = 600;

// One ring entry of guide data for one eye. Depth and motion are shared copies of the eye scratch the
// super-resolution dispatch just produced; sampling those directly would need them created as shared
// textures, which the desktop path must not pay for.
struct GuideEntry
{
    Dx12Interop::Shared depth;
    Dx12Interop::Shared motion;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t serial = 0;
    bool ready = false;
    VrFgFrame frame{};
    bool reset = false;
    bool reversedDepth = false;
};

// One shared copy of the image the game submitted for one eye, for one game frame.
struct EyeSource
{
    Dx12Interop::Shared shared;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t serial = 0;
    bool valid = false;
};

struct EyeState
{
    GuideEntry guides[kGuideEntries];
    EyeSource sources[kSubmitEntries];
    // Images handed to the compositor. `real` holds the game's frame, `generated` the interpolation.
    ComPtr<ID3D12Resource> submitReal[kSubmitEntries];
    ComPtr<ID3D12Resource> submitGenerated[kSubmitEntries];
    D3D12_RESOURCE_STATES submitRealState[kSubmitEntries] = {D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES submitGeneratedState[kSubmitEntries] = {D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON};
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    bool sourcesReady = false;
    vr::VRTextureBounds_t bounds{0.0f, 0.0f, 1.0f, 1.0f};
    uint64_t submittedSerial = 0;
    bool submitted = false;
};

// One real game frame waiting for its two compositor slots.
struct PendingFrame
{
    uint64_t serial = 0;
    vr::HmdMatrix34_t realPose{};
    vr::HmdMatrix34_t generatedPose{};
    bool hasGenerated = false;
    bool generatedShown = false;
    uint32_t realSlot = 0;
    uint32_t generatedSlot = 0;
    vr::VRTextureBounds_t bounds[2]{{0.0f, 0.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}};
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue = 0;
};

// What the presenter submits in one compositor slot.
struct SlotSubmission
{
    bool valid = false;
    bool generated = false;
    uint64_t serial = 0;
    vr::HmdMatrix34_t pose{};
    ComPtr<ID3D12Resource> resource[2];
    vr::VRTextureBounds_t bounds[2]{};
};

using WaitGetPosesFn = vr::EVRCompositorError (*)(void*, vr::TrackedDevicePose_t*, uint32_t,
                                                  vr::TrackedDevicePose_t*, uint32_t);
using SubmitFn = vr::EVRCompositorError (*)(void*, vr::EVREye, const vr::Texture_t*,
                                            const vr::VRTextureBounds_t*, vr::EVRSubmitFlags);
using SubmitArrayFn = vr::EVRCompositorError (*)(void*, vr::EVREye, const vr::Texture_t*, uint32_t,
                                                 const vr::VRTextureBounds_t*, vr::EVRSubmitFlags);
using PostPresentHandoffFn = void (*)(void*);

std::mutex g_mutex;
std::condition_variable g_releaseCv;

std::atomic<bool> g_requested{false};
std::atomic<bool> g_active{false};
std::atomic<bool> g_stop{false};
std::atomic<int32_t> g_state{BIGSCALE_VRFG_STATE_OFF};
std::atomic<int32_t> g_lastResult{0};
std::atomic<float> g_displayHz{0.0f};
std::atomic<uint32_t> g_motionSmoothing{0};
std::atomic<uint64_t> g_realFrames{0};
std::atomic<uint64_t> g_generatedFrames{0};
std::atomic<uint64_t> g_repeatedSlots{0};
std::atomic<uint64_t> g_droppedFrames{0};
std::atomic<uint64_t> g_pacingTimeouts{0};
std::atomic<uint64_t> g_fenceBlocked{0};
std::atomic<uint64_t> g_emptySlots{0};
std::atomic<uint64_t> g_gameSerial{0};
std::atomic<uint32_t> g_renderThreadId{0};
std::atomic<bool> g_activeAnnounced{false};

std::mutex g_reasonMutex;
char g_reason[BIGSCALE_VRFG_REASON_CAPACITY]{};

// Guarded by g_mutex.
uint64_t g_submittedRealSerial = 0;
std::deque<PendingFrame> g_queue;

// Install-time state. Every member below is touched only on the render thread once the module is
// active, and only on the main thread while it is being configured.
vr::IVRCompositor* g_compositor = nullptr;
vr::IVRSystem* g_system = nullptr;
vr::IVRSettings* g_settings = nullptr;
bool g_hooked = false;
bool g_refused = false;
float g_vsyncToPhotons = 0.0f;
float g_displayPeriod = 1.0f / 90.0f;
vr::ETrackingUniverseOrigin g_trackingSpace = vr::TrackingUniverseSeated;
vr::HmdMatrix34_t g_poses[kPoseHistory]{};
int64_t g_clockOrigin = 0;
double g_nextGameStart = 0.0;
bool g_poseValid[kPoseHistory]{};
uint64_t g_lastWriteWaitSerial = 0;
bool g_writeWaitDone = false;
uint64_t g_packetSerial = 0;
// The guide log describes a once-per-game-frame shape, so the first three packets and then every
// 600th are enough: logging every source frame cost a formatted write per frame.
uint32_t g_guideLogCount = 0;

Dx12Interop g_interop;
VrFsrInterpolator g_interpolator;
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<ID3D11DeviceContext4> g_context4;
EyeState g_eyes[2];
std::thread g_presenter;
std::vector<vr::TrackedDevicePose_t> g_presenterPoses;

WaitGetPosesFn g_origWaitGetPoses = nullptr;
SubmitFn g_origSubmit = nullptr;
SubmitArrayFn g_origSubmitArray = nullptr;
PostPresentHandoffFn g_origPostPresentHandoff = nullptr;

void SetState(int32_t state, const char* reason)
{
    g_state.store(state);
    {
        std::lock_guard<std::mutex> guard(g_reasonMutex);
        const size_t length = reason == nullptr ? 0 : std::strlen(reason);
        const size_t copy = length < (BIGSCALE_VRFG_REASON_CAPACITY - 1) ? length : (BIGSCALE_VRFG_REASON_CAPACITY - 1);
        if (copy > 0) std::memcpy(g_reason, reason, copy);
        g_reason[copy] = 0;
    }
}

// ---------------------------------------------------------------------------------------------
// Pose math. HmdMatrix34_t is a row-major 3x4 with a column-vector rotation, so the linear block is a
// standard rotation matrix.
// ---------------------------------------------------------------------------------------------
struct Quaternion
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

Quaternion RotationOf(const vr::HmdMatrix34_t& pose)
{
    const float m00 = pose.m[0][0], m01 = pose.m[0][1], m02 = pose.m[0][2];
    const float m10 = pose.m[1][0], m11 = pose.m[1][1], m12 = pose.m[1][2];
    const float m20 = pose.m[2][0], m21 = pose.m[2][1], m22 = pose.m[2][2];
    const float trace = m00 + m11 + m22;
    Quaternion q{};
    if (trace > 0.0f)
    {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    }
    else if (m00 > m11 && m00 > m22)
    {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    }
    else if (m11 > m22)
    {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    }
    else
    {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(length > 1e-9f) || !std::isfinite(length)) return Quaternion{};
    q.x /= length;
    q.y /= length;
    q.z /= length;
    q.w /= length;
    return q;
}

Quaternion Slerp(const Quaternion& a, const Quaternion& b, float t)
{
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    float sign = 1.0f;
    if (dot < 0.0f)
    {
        dot = -dot;
        sign = -1.0f;
    }
    if (dot > 0.9995f)
    {
        Quaternion out;
        out.x = a.x + (b.x * sign - a.x) * t;
        out.y = a.y + (b.y * sign - a.y) * t;
        out.z = a.z + (b.z * sign - a.z) * t;
        out.w = a.w + (b.w * sign - a.w) * t;
        const float length = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z + out.w * out.w);
        if (length > 1e-9f)
        {
            out.x /= length;
            out.y /= length;
            out.z /= length;
            out.w /= length;
        }
        return out;
    }
    const float theta = std::acos(std::max(-1.0f, std::min(1.0f, dot)));
    const float sinTheta = std::sin(theta);
    const float scaleA = std::sin((1.0f - t) * theta) / sinTheta;
    const float scaleB = std::sin(t * theta) / sinTheta * sign;
    Quaternion out;
    out.x = a.x * scaleA + b.x * scaleB;
    out.y = a.y * scaleA + b.y * scaleB;
    out.z = a.z * scaleA + b.z * scaleB;
    out.w = a.w * scaleA + b.w * scaleB;
    return out;
}

vr::HmdMatrix34_t Compose(const Quaternion& q, const float translation[3])
{
    vr::HmdMatrix34_t pose{};
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    pose.m[0][0] = 1.0f - 2.0f * (yy + zz);
    pose.m[0][1] = 2.0f * (xy - wz);
    pose.m[0][2] = 2.0f * (xz + wy);
    pose.m[1][0] = 2.0f * (xy + wz);
    pose.m[1][1] = 1.0f - 2.0f * (xx + zz);
    pose.m[1][2] = 2.0f * (yz - wx);
    pose.m[2][0] = 2.0f * (xz - wy);
    pose.m[2][1] = 2.0f * (yz + wx);
    pose.m[2][2] = 1.0f - 2.0f * (xx + yy);
    for (int row = 0; row < 3; row++) pose.m[row][3] = translation[row];
    return pose;
}

// Midpoint between two HMD poses: slerped rotation, linear translation.
vr::HmdMatrix34_t Interpolate(const vr::HmdMatrix34_t& from, const vr::HmdMatrix34_t& to)
{
    const Quaternion rotation = Slerp(RotationOf(from), RotationOf(to), 0.5f);
    float translation[3]{};
    for (int row = 0; row < 3; row++) translation[row] = (from.m[row][3] + to.m[row][3]) * 0.5f;
    return Compose(rotation, translation);
}

// Eye pose in tracking space: head pose composed with the eye-to-head transform.
vr::HmdMatrix34_t EyePose(const vr::HmdMatrix34_t& head, const vr::HmdMatrix34_t& eyeToHead)
{
    vr::HmdMatrix34_t out{};
    for (int row = 0; row < 3; row++)
    {
        for (int column = 0; column < 4; column++)
        {
            float sum = 0.0f;
            for (int k = 0; k < 3; k++) sum += head.m[row][k] * eyeToHead.m[k][column];
            if (column == 3) sum += head.m[row][3];
            out.m[row][column] = sum;
        }
    }
    return out;
}

void CameraBasis(const vr::HmdMatrix34_t& eyePose, VrFgFrame& frame)
{
    frame.cameraPosition[0] = eyePose.m[0][3];
    frame.cameraPosition[1] = eyePose.m[1][3];
    frame.cameraPosition[2] = eyePose.m[2][3];
    frame.cameraRight[0] = eyePose.m[0][0];
    frame.cameraRight[1] = eyePose.m[1][0];
    frame.cameraRight[2] = eyePose.m[2][0];
    frame.cameraUp[0] = eyePose.m[0][1];
    frame.cameraUp[1] = eyePose.m[1][1];
    frame.cameraUp[2] = eyePose.m[2][1];
    frame.cameraForward[0] = -eyePose.m[0][2];
    frame.cameraForward[1] = -eyePose.m[1][2];
    frame.cameraForward[2] = -eyePose.m[2][2];
}

// ---------------------------------------------------------------------------------------------
// Failure handling. Every refusal is published once, with the exact reason the HUD shows.
// ---------------------------------------------------------------------------------------------
void Fail(const char* reason)
{
    if (g_refused) return;
    g_refused = true;
    g_requested.store(false);
    Log("VRFG: %s", reason);
    SetState(BIGSCALE_VRFG_STATE_UNSUPPORTED, reason);
}

void Fault(const char* reason, int32_t result)
{
    if (g_refused) return;
    g_refused = true;
    g_requested.store(false);
    g_lastResult.store(result);
    Log("VRFG: fault: %s", reason);
    SetState(BIGSCALE_VRFG_STATE_FAULTED, reason);
}

// ---------------------------------------------------------------------------------------------
// Resource creation
// ---------------------------------------------------------------------------------------------
bool CreateSharedTexture(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format,
                         ID3D11Texture2D** texture)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = 0;
    description.CPUAccessFlags = 0;
    description.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    return SUCCEEDED(device->CreateTexture2D(&description, nullptr, texture)) && *texture != nullptr;
}

bool EnsureSubmitTarget(ID3D12Device* device, ComPtr<ID3D12Resource>& resource, D3D12_RESOURCE_STATES& state,
                        uint32_t width, uint32_t height, bool unorderedAccess)
{
    if (resource.Get() != nullptr)
    {
        const D3D12_RESOURCE_DESC existing = resource->GetDesc();
        if (existing.Width == width && existing.Height == height &&
            ((existing.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) == unorderedAccess)
            return true;
        resource.Reset();
        state = D3D12_RESOURCE_STATE_COMMON;
    }
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    // The generated image is a dispatch output, so only that target may be an unordered-access
    // resource; the real copy stays a plain texture. Both use a shared allocation so the frame stays
    // inspectable exactly like every other submitted eye image.
    description.Flags = unorderedAccess ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &description,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))) ||
        resource.Get() == nullptr)
        return false;
    state = D3D12_RESOURCE_STATE_COMMON;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Guide publication (render thread, inside the super-resolution dispatch)
// ---------------------------------------------------------------------------------------------
bool EnsureGuideEntry(GuideEntry& entry, uint32_t width, uint32_t height, std::string& reason)
{
    if (entry.width == width && entry.height == height && entry.depth.d3d11.Get() != nullptr &&
        entry.motion.d3d11.Get() != nullptr)
    {
        if (!g_interop.Share(entry.depth.d3d11.Get(), entry.depth, reason)) return false;
        return g_interop.Share(entry.motion.d3d11.Get(), entry.motion, reason);
    }
    g_interop.Release(entry.depth);
    g_interop.Release(entry.motion);
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11Texture2D> motion;
    if (!CreateSharedTexture(g_device.Get(), width, height, DXGI_FORMAT_R32_FLOAT, &depth))
    {
        reason = "the VR frame-generation depth guide could not be created";
        return false;
    }
    if (!CreateSharedTexture(g_device.Get(), width, height, DXGI_FORMAT_R16G16_FLOAT, &motion))
    {
        reason = "the VR frame-generation motion guide could not be created";
        return false;
    }
    if (!g_interop.Share(depth.Get(), entry.depth, reason) ||
        !g_interop.Share(motion.Get(), entry.motion, reason))
        return false;
    entry.width = width;
    entry.height = height;
    return true;
}

bool CopyGuide(ID3D11DeviceContext* context, ID3D11Texture2D* destination, ID3D11Resource* source,
               DXGI_FORMAT expected, uint32_t width, uint32_t height)
{
    ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&texture))) && texture)
    {
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (description.Width < width || description.Height < height) return false;
        // A copy needs identical bit layouts; an sRGB/unorm pair of the same storage is compatible,
        // but a different format is not and would silently corrupt the interpolation.
        const bool compatible = description.Format == expected ||
            (expected == DXGI_FORMAT_R32_FLOAT && description.Format == DXGI_FORMAT_R32_TYPELESS) ||
            (expected == DXGI_FORMAT_R16G16_FLOAT && description.Format == DXGI_FORMAT_R16G16_TYPELESS);
        if (!compatible) return false;
        context->CopySubresourceRegion(destination, 0, 0, 0, 0, texture.Get(), 0, nullptr);
        return true;
    }
    context->CopySubresourceRegion(destination, 0, 0, 0, 0, source, 0, nullptr);
    return true;
}
}

bool VrFgWantsGuides()
{
    return g_requested.load(std::memory_order_acquire) && g_active.load(std::memory_order_acquire);
}

void VrFgPublishGuides(ID3D11DeviceContext* context, const BIGSCALE_Frame& frame, const EvaluationRequest& request,
                       ID3D11Resource* depth, ID3D11Resource* motion)
{
    if (!VrFgWantsGuides() || context == nullptr || depth == nullptr || motion == nullptr) return;
    if (frame.eye < 0 || frame.eye > 1) return;
    if (request.inputWidth == 0 || request.inputHeight == 0) return;
    if (g_refused) return;
    const uint32_t width = request.scratchWidth != 0 ? request.scratchWidth : request.inputWidth;
    const uint32_t height = request.scratchHeight != 0 ? request.scratchHeight : request.inputHeight;
    const uint64_t serial = g_gameSerial.load(std::memory_order_acquire);
    if (serial == 0) return;

    EyeState& eye = g_eyes[frame.eye];
    GuideEntry& entry = eye.guides[serial % kGuideEntries];
    // One coarse D3D11 wait per game frame keeps the guide and eye copies behind the previous D3D12
    // generation, without stalling every copy on its own fence.
    std::string reason;
    if (!g_writeWaitDone || g_lastWriteWaitSerial != serial)
    {
        if (!g_interop.WaitOnD3D11(g_context4.Get(), reason))
        {
            Fault(reason.c_str(), 0);
            return;
        }
        g_lastWriteWaitSerial = serial;
        g_writeWaitDone = true;
    }
    if (!EnsureGuideEntry(entry, width, height, reason))
    {
        Fault(reason.c_str(), 0);
        return;
    }
    if (!CopyGuide(context, entry.depth.d3d11.Get(), depth, DXGI_FORMAT_R32_FLOAT, width, height) ||
        !CopyGuide(context, entry.motion.d3d11.Get(), motion, DXGI_FORMAT_R16G16_FLOAT, width, height))
    {
        Fault("the VR frame-generation guides have an unexpected format", 0);
        return;
    }
    entry.serial = serial;
    entry.ready = true;
    entry.frame.renderWidth = request.inputWidth;
    entry.frame.renderHeight = request.inputHeight;
    entry.frame.jitterX = request.jitterX;
    entry.frame.jitterY = request.jitterY;
    entry.frame.deltaMilliseconds = request.deltaMilliseconds;
    entry.frame.cameraNear = request.cameraNear;
    entry.frame.cameraFar = request.cameraFar;
    entry.frame.verticalFovRadians = request.verticalFovRadians;
    entry.reset = request.reset;
    entry.reversedDepth = request.reversedDepth;
    if (g_packetSerial != serial)
    {
        g_packetSerial = serial;
        ++g_guideLogCount;
        if (g_guideLogCount <= 3 || g_guideLogCount % 600 == 0)
        {
            Log("VRFG: frame %llu guides render=%ux%u jitter=(%.3f,%.3f) delta=%.2fms%s",
                static_cast<unsigned long long>(serial), request.inputWidth, request.inputHeight,
                request.jitterX, request.jitterY, request.deltaMilliseconds,
                request.reset ? " reset" : "");
        }
    }
}
}

namespace bigscale
{
vr::EVRCompositorError HookWaitGetPoses(void* self, vr::TrackedDevicePose_t* renderPoses, uint32_t renderCount,
                                       vr::TrackedDevicePose_t* gamePoses, uint32_t gameCount);

namespace
{
int64_t g_lastFinalizeQpc = 0;
vr::HmdMatrix34_t g_eyeToHead[2]{};
bool g_eyeToHeadValid = false;

int64_t NowQpc()
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

double QpcToSeconds(int64_t delta)
{
    static const double frequency = []
    {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart > 0 ? static_cast<double>(value.QuadPart) : 1.0;
    }();
    return static_cast<double>(delta) / frequency;
}

double NowSeconds()
{
    return QpcToSeconds(NowQpc() - g_clockOrigin);
}

bool FenceDone(const PendingFrame& frame)
{
    if (frame.fence == nullptr) return true;
    return frame.fence->GetCompletedValue() >= frame.fenceValue;
}

bool PoseContinuous(uint64_t serial)
{
    if (serial < 2) return false;
    const uint32_t current = static_cast<uint32_t>(serial % kPoseHistory);
    const uint32_t previous = static_cast<uint32_t>((serial - 1) % kPoseHistory);
    if (!g_poseValid[current] || !g_poseValid[previous]) return false;
    const Quaternion a = RotationOf(g_poses[previous]);
    const Quaternion b = RotationOf(g_poses[current]);
    const float dot = std::min(1.0f,
        std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w));
    const float angle = 2.0f * std::acos(dot);
    if (angle > 45.0f * 3.14159265f / 180.0f) return false;
    float distance = 0.0f;
    for (int row = 0; row < 3; row++)
    {
        const float delta = g_poses[current].m[row][3] - g_poses[previous].m[row][3];
        distance += delta * delta;
    }
    return std::sqrt(distance) <= 0.5f;
}

bool SubmitSlot(const SlotSubmission& submission)
{
    if (g_origSubmit == nullptr || g_compositor == nullptr) return false;
    for (int32_t eye = 0; eye < 2; eye++)
    {
        if (submission.resource[eye].Get() == nullptr) continue;
        vr::D3D12TextureData_t data{};
        data.m_pResource = submission.resource[eye].Get();
        data.m_pCommandQueue = g_interop.Queue();
        data.m_nNodeMask = 0;
        vr::VRTextureWithPose_t texture{};
        texture.handle = &data;
        texture.eType = vr::TextureType_DirectX12;
        texture.eColorSpace = vr::ColorSpace_Gamma;
        texture.mDeviceToAbsoluteTracking = submission.pose;
        const vr::EVRCompositorError result = g_origSubmit(g_compositor, static_cast<vr::EVREye>(eye), &texture,
            &submission.bounds[eye], vr::Submit_TextureWithPose);
        if (result != vr::VRCompositorError_None)
        {
            char reason[192];
            std::snprintf(reason, sizeof(reason),
                "SteamVR refused the D3D12 frame-generation submission (error %d)", static_cast<int>(result));
            Fail(reason);
            return false;
        }
    }
    return true;
}

void PresentLoop()
{
    g_presenterPoses.assign(vr::k_unMaxTrackedDeviceCount, vr::TrackedDevicePose_t{});
    SlotSubmission repeat;
    bool haveRepeat = false;
    while (!g_stop.load(std::memory_order_acquire))
    {
        if (g_origWaitGetPoses == nullptr || g_compositor == nullptr) break;
        g_origWaitGetPoses(g_compositor, g_presenterPoses.data(),
            static_cast<uint32_t>(g_presenterPoses.size()), nullptr, 0);
        if (g_stop.load(std::memory_order_acquire)) break;

        SlotSubmission submission;
        {
            std::lock_guard<std::mutex> guard(g_mutex);
            if (g_queue.empty()) g_emptySlots.fetch_add(1);
            if (!g_queue.empty())
            {
                PendingFrame& front = g_queue.front();
                if (!FenceDone(front)) g_fenceBlocked.fetch_add(1);
                if (FenceDone(front))
                {
                    const uint64_t serial = front.serial;
                    const bool generated = front.hasGenerated && !front.generatedShown;
                    submission.valid = true;
                    submission.generated = generated;
                    submission.serial = serial;
                    submission.pose = generated ? front.generatedPose : front.realPose;
                    const uint32_t slot = generated ? front.generatedSlot : front.realSlot;
                    for (int32_t eye = 0; eye < 2; eye++)
                    {
                        submission.resource[eye] = generated ? g_eyes[eye].submitGenerated[slot]
                                                             : g_eyes[eye].submitReal[slot];
                        submission.bounds[eye] = front.bounds[eye];
                    }
                    if (generated)
                    {
                        front.generatedShown = true;
                    }
                    else
                    {
                        g_queue.pop_front();
                        g_submittedRealSerial = serial;
                    }
                }
            }
        }
        if (submission.valid)
        {
            if (SubmitSlot(submission))
            {
                if (submission.generated)
                {
                    const uint64_t generated = g_generatedFrames.fetch_add(1) + 1;
                    if (generated == 1)
                    {
                        SetState(BIGSCALE_VRFG_STATE_ACTIVE,
                            g_motionSmoothing.load()
                                ? "SteamVR Motion Smoothing is on; it can halve the frame rate"
                                : "");
                        Log("VRFG: first generated frame presented.");
                    }
                }
                else
                {
                    const uint64_t real = g_realFrames.fetch_add(1) + 1;
                    repeat = submission;
                    haveRepeat = true;
                    if (real % kSummaryInterval == 0)
                        Log("VRFG: summary real=%llu generated=%llu repeated=%llu dropped=%llu timeouts=%llu",
                            static_cast<unsigned long long>(real),
                            static_cast<unsigned long long>(g_generatedFrames.load()),
                            static_cast<unsigned long long>(g_repeatedSlots.load()),
                            static_cast<unsigned long long>(g_droppedFrames.load()),
                            static_cast<unsigned long long>(g_pacingTimeouts.load()));
                }
            }
        }
        else if (haveRepeat)
        {
            // Never leave a compositor slot empty: the last real image is shown again instead.
            if (SubmitSlot(repeat)) g_repeatedSlots.fetch_add(1);
        }
        // A finished real submit frees ring depth for the game's next frame.
        g_releaseCv.notify_all();
        if (g_origPostPresentHandoff != nullptr) g_origPostPresentHandoff(g_compositor);
    }
}

// True when this real frame's guide pair is complete enough to interpolate.
bool CanGenerate(uint64_t serial, uint32_t& renderWidth, uint32_t& renderHeight)
{
    if (serial < 2) return false;
    const uint32_t currentIndex = static_cast<uint32_t>(serial % kGuideEntries);
    const uint32_t previousIndex = static_cast<uint32_t>((serial - 1) % kGuideEntries);
    const GuideEntry& left = g_eyes[0].guides[currentIndex];
    const GuideEntry& right = g_eyes[1].guides[currentIndex];
    if (!left.ready || !right.ready || left.serial != serial || right.serial != serial) return false;
    if (left.reset || right.reset) return false;
    if (left.frame.renderWidth != right.frame.renderWidth ||
        left.frame.renderHeight != right.frame.renderHeight)
        return false;
    const GuideEntry& previousLeft = g_eyes[0].guides[previousIndex];
    const GuideEntry& previousRight = g_eyes[1].guides[previousIndex];
    if (!previousLeft.ready || !previousRight.ready) return false;
    if (previousLeft.serial != serial - 1 || previousRight.serial != serial - 1) return false;
    if (previousLeft.frame.renderWidth != left.frame.renderWidth ||
        previousLeft.frame.renderHeight != left.frame.renderHeight ||
        previousRight.frame.renderWidth != left.frame.renderWidth)
        return false;
    if (!PoseContinuous(serial)) return false;
    if (g_lastFinalizeQpc == 0 || QpcToSeconds(NowQpc() - g_lastFinalizeQpc) >= 0.1) return false;
    renderWidth = left.frame.renderWidth;
    renderHeight = left.frame.renderHeight;
    return true;
}

bool DispatchEye(ID3D12GraphicsCommandList* list, int32_t eye, uint32_t slot, uint64_t serial, bool reset,
                 std::string& reason)
{
    EyeState& state = g_eyes[eye];
    const GuideEntry& guide = state.guides[serial % kGuideEntries];
    const vr::HmdMatrix34_t& realPose = g_poses[serial % kPoseHistory];
    VrFgGenerate generate;
    generate.eye = eye;
    generate.list = list;
    generate.real = state.submitReal[slot].Get();
    generate.generated = state.submitGenerated[slot].Get();
    generate.depth = guide.depth.d3d12.Get();
    generate.motion = guide.motion.d3d12.Get();
    generate.frame = guide.frame;
    if (g_eyeToHeadValid) CameraBasis(EyePose(realPose, g_eyeToHead[eye]), generate.frame);
    generate.reset = reset;

    GuideEntry& guideState = state.guides[serial % kGuideEntries];
    Dx12Interop::Transition(list, guideState.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Dx12Interop::Transition(list, guideState.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Dx12Interop::Transition(list, generate.generated, state.submitGeneratedState[slot],
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const bool recorded = g_interpolator.Record(generate, reason);
    Dx12Interop::Transition(list, generate.generated, state.submitGeneratedState[slot],
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Dx12Interop::Transition(list, guideState.depth, D3D12_RESOURCE_STATE_COMMON);
    Dx12Interop::Transition(list, guideState.motion, D3D12_RESOURCE_STATE_COMMON);
    return recorded;
}

void FinalizeFrame(uint64_t serial)
{
    if (serial == 0 || g_refused) return;
    EyeState& left = g_eyes[0];
    EyeState& right = g_eyes[1];
    const uint32_t slot = static_cast<uint32_t>(serial % kSubmitEntries);
    if (!left.submitted || left.submittedSerial != serial || !right.submitted || right.submittedSerial != serial)
    {
        g_droppedFrames.fetch_add(1);
        return;
    }
    if (!left.sourcesReady || left.width != right.width || left.height != right.height || left.width == 0)
    {
        g_droppedFrames.fetch_add(1);
        return;
    }
    ID3D12Device* device = g_interop.Device();
    if (device == nullptr) return;
    bool targetsReady = true;
    for (int32_t eye = 0; eye < 2 && targetsReady; eye++)
    {
        EyeState& state = g_eyes[eye];
        targetsReady = EnsureSubmitTarget(device, state.submitReal[slot], state.submitRealState[slot],
                          left.width, left.height, false) &&
                       EnsureSubmitTarget(device, state.submitGenerated[slot],
                          state.submitGeneratedState[slot], left.width, left.height, true);
    }
    if (!targetsReady)
    {
        Fault("the VR frame-generation submit images could not be created", 0);
        return;
    }
    // Contexts are created for the eye extent (they output full-size eyes) and follow the depth
    // convention the reconstruction used.
    std::string reason;
    const bool reversedDepth = left.guides[slot].ready ? left.guides[slot].reversedDepth : false;
    if (!g_interpolator.Ensure(device, left.width, left.height, reversedDepth, reason))
    {
        const std::string message = "AMD FidelityFX frame generation is unavailable: " + reason;
        Fail(message.c_str());
        return;
    }
    uint64_t fenceValue = 0;
    if (!g_interop.SignalFromD3D11(g_context4.Get(), reason, fenceValue))
    {
        Fault(reason.c_str(), 0);
        return;
    }
    // The compositor must see real frame n-2 before the game is allowed to reuse ring entry n, so the
    // game can never run more than two frames past what was actually presented.
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        if (!g_releaseCv.wait_for(lock, std::chrono::milliseconds(kQueueWaitMs),
                [&] { return g_submittedRealSerial + 2 >= serial || !g_active.load(); }))
        {
            g_droppedFrames.fetch_add(1);
            g_pacingTimeouts.fetch_add(1);
            return;
        }
    }
    if (!g_active.load()) return;
    if (!g_interop.Begin(reason))
    {
        Fault(reason.c_str(), 0);
        return;
    }
    ID3D12GraphicsCommandList* list = g_interop.List();
    for (int32_t eye = 0; eye < 2; eye++)
    {
        EyeState& state = g_eyes[eye];
        Dx12Interop::Transition(list, state.submitReal[slot].Get(), state.submitRealState[slot],
            D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(state.submitReal[slot].Get(), state.sources[slot].shared.d3d12.Get());
        Dx12Interop::Transition(list, state.submitReal[slot].Get(), state.submitRealState[slot],
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    bool generated = CanGenerate(serial, renderWidth, renderHeight);
    for (int32_t eye = 0; eye < 2; eye++)
    {
        // A skipped pair still resets the provider's history, so the next real pair never interpolates
        // against images from before the gap.
        const bool reset = !generated;
        if (reset && !g_interpolator.Ready(eye)) continue;
        std::string dispatchReason;
        if (!DispatchEye(list, eye, slot, serial, reset, dispatchReason))
        {
            if (generated) Log("VRFG: generation skipped for eye %d: %s", eye, dispatchReason.c_str());
            generated = false;
        }
    }
    if (!g_interop.Submit(reason, fenceValue))
    {
        Fault(reason.c_str(), 0);
        return;
    }
    PendingFrame pending;
    pending.serial = serial;
    pending.realPose = g_poses[serial % kPoseHistory];
    pending.generatedPose = serial >= 2 && g_poseValid[(serial - 1) % kPoseHistory]
        ? Interpolate(g_poses[(serial - 1) % kPoseHistory], pending.realPose)
        : pending.realPose;
    pending.hasGenerated = generated;
    pending.realSlot = slot;
    pending.generatedSlot = slot;
    for (int32_t eye = 0; eye < 2; eye++) pending.bounds[eye] = g_eyes[eye].bounds;
    pending.fence = g_interop.SubmissionFence();
    pending.fenceValue = g_interop.SubmissionValue();
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        // The release rule bounds the queue at two entries; anything deeper is a lost presenter.
        while (g_queue.size() >= 3)
        {
            g_queue.pop_front();
            g_droppedFrames.fetch_add(1);
        }
        g_queue.push_back(std::move(pending));
    }
    g_releaseCv.notify_all();
    if (!g_activeAnnounced.exchange(true))
        Log("VRFG: active display=%.1f Hz eye=%ux%u render=%ux%u", g_displayHz.load(), left.width, left.height,
            renderWidth != 0 ? renderWidth : left.width, renderHeight != 0 ? renderHeight : left.height);
    g_lastFinalizeQpc = NowQpc();
}

bool CaptureEye(vr::EVREye eyeIndex, const vr::Texture_t* texture, uint32_t arrayIndex,
                const vr::VRTextureBounds_t* bounds)
{
    if (eyeIndex != vr::Eye_Left && eyeIndex != vr::Eye_Right) return false;
    if (texture == nullptr || texture->handle == nullptr) return false;
    const int32_t eye = static_cast<int32_t>(eyeIndex);
    if (texture->eType != vr::TextureType_DirectX)
    {
        char reason[160];
        std::snprintf(reason, sizeof(reason),
            "VR frame generation supports 8-bit eye textures only (texture type %d)",
            static_cast<int>(texture->eType));
        Fail(reason);
        return false;
    }
    ComPtr<ID3D11Texture2D> source;
    auto* resource = static_cast<ID3D11Texture2D*>(texture->handle);
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&source))) || !source)
    {
        Fail("VR frame generation could not read the submitted eye texture");
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        description.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
        description.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS)
    {
        char reason[160];
        std::snprintf(reason, sizeof(reason),
            "VR frame generation supports 8-bit eye textures only (format %d)",
            static_cast<int>(description.Format));
        Fail(reason);
        return false;
    }
    uint32_t slice = arrayIndex;
    if (description.ArraySize >= 2 && arrayIndex == 0) slice = static_cast<uint32_t>(eye);
    if (slice >= description.ArraySize) slice = 0;
    const uint64_t serial = g_gameSerial.load(std::memory_order_acquire);
    if (serial == 0) return false;
    EyeState& state = g_eyes[eye];
    std::string reason;
    if (!state.sourcesReady || state.sourceWidth != description.Width ||
        state.sourceHeight != description.Height)
    {
        for (EyeSource& existing : state.sources) g_interop.Release(existing.shared);
        for (uint32_t i = 0; i < kSubmitEntries; i++)
        {
            ComPtr<ID3D11Texture2D> created;
            if (!CreateSharedTexture(g_device.Get(), description.Width, description.Height,
                    DXGI_FORMAT_R8G8B8A8_UNORM, &created) ||
                !g_interop.Share(created.Get(), state.sources[i].shared, reason))
            {
                Fault("the VR frame-generation eye copy could not be created", 0);
                return false;
            }
            state.sources[i].valid = false;
        }
        state.sourcesReady = true;
        state.sourceWidth = description.Width;
        state.sourceHeight = description.Height;
        state.width = description.Width;
        state.height = description.Height;
    }
    if (!g_writeWaitDone || g_lastWriteWaitSerial != serial)
    {
        if (!g_interop.WaitOnD3D11(g_context4.Get(), reason))
        {
            Fault(reason.c_str(), 0);
            return false;
        }
        g_lastWriteWaitSerial = serial;
        g_writeWaitDone = true;
    }
    EyeSource& target = state.sources[serial % kSubmitEntries];
    g_context->CopySubresourceRegion(target.shared.d3d11.Get(), 0, 0, 0, 0, source.Get(), slice, nullptr);
    target.width = description.Width;
    target.height = description.Height;
    target.serial = serial;
    target.valid = true;
    state.submittedSerial = serial;
    state.submitted = true;
    if (bounds != nullptr) state.bounds = *bounds;
    else state.bounds = vr::VRTextureBounds_t{0.0f, 0.0f, 1.0f, 1.0f};
    return true;
}

vr::EVRCompositorError HookSubmit(void* self, vr::EVREye eye, const vr::Texture_t* texture,
                                  const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags)
{
    if (self != g_compositor || !g_active.load(std::memory_order_acquire) ||
        GetCurrentThreadId() != g_renderThreadId.load())
        return g_origSubmit(self, eye, texture, bounds, flags);
    if (!CaptureEye(eye, texture, 0, bounds)) return vr::VRCompositorError_None;
    return vr::VRCompositorError_None;
}

vr::EVRCompositorError HookSubmitArray(void* self, vr::EVREye eye, const vr::Texture_t* texture, uint32_t arrayIndex,
                                       const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags)
{
    if (self != g_compositor || !g_active.load(std::memory_order_acquire) ||
        GetCurrentThreadId() != g_renderThreadId.load())
        return g_origSubmitArray(self, eye, texture, arrayIndex, bounds, flags);
    CaptureEye(eye, texture, arrayIndex, bounds);
    return vr::VRCompositorError_None;
}

void HookPostPresentHandoff(void* self)
{
    if (self != g_compositor || !g_active.load(std::memory_order_acquire) ||
        GetCurrentThreadId() != g_renderThreadId.load())
    {
        g_origPostPresentHandoff(self);
        return;
    }
    // The presenter owns the real handoff; this call only closes the game's frame.
    FinalizeFrame(g_gameSerial.load(std::memory_order_acquire));
}

bool InstallHooks(std::string& reason)
{
    void** vtable = *reinterpret_cast<void***>(g_compositor);
    if (vtable == nullptr)
    {
        reason = "the OpenVR compositor has no vtable";
        return false;
    }
    void* waitTarget = vtable[2];
    void* submitTarget = vtable[6];
    void* submitArrayTarget = vtable[7];
    void* handoffTarget = vtable[9];
    if (waitTarget == nullptr || submitTarget == nullptr || submitArrayTarget == nullptr ||
        handoffTarget == nullptr)
    {
        reason = "the OpenVR compositor does not expose the expected entry points";
        return false;
    }
    const MH_STATUS initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
    {
        reason = "MinHook could not be initialised for the OpenVR compositor";
        return false;
    }
    void* waitTrampoline = nullptr;
    void* submitTrampoline = nullptr;
    void* submitArrayTrampoline = nullptr;
    void* handoffTrampoline = nullptr;
    if (MH_CreateHook(waitTarget, reinterpret_cast<LPVOID>(&HookWaitGetPoses), &waitTrampoline) != MH_OK ||
        MH_CreateHook(submitTarget, reinterpret_cast<LPVOID>(&HookSubmit), &submitTrampoline) != MH_OK ||
        MH_CreateHook(submitArrayTarget, reinterpret_cast<LPVOID>(&HookSubmitArray), &submitArrayTrampoline) !=
            MH_OK ||
        MH_CreateHook(handoffTarget, reinterpret_cast<LPVOID>(&HookPostPresentHandoff), &handoffTrampoline) !=
            MH_OK)
    {
        reason = "the OpenVR compositor detours could not be created";
        return false;
    }
    // Calling the patched address again would re-enter this module, so every original call goes
    // through MinHook's trampoline.
    g_origWaitGetPoses = reinterpret_cast<WaitGetPosesFn>(waitTrampoline);
    g_origSubmit = reinterpret_cast<SubmitFn>(submitTrampoline);
    g_origSubmitArray = reinterpret_cast<SubmitArrayFn>(submitArrayTrampoline);
    g_origPostPresentHandoff = reinterpret_cast<PostPresentHandoffFn>(handoffTrampoline);
    void* targets[4] = {waitTarget, submitTarget, submitArrayTarget, handoffTarget};
    for (void* target : targets)
    {
        if (MH_EnableHook(target) != MH_OK)
        {
            reason = "the OpenVR compositor detours could not be enabled";
            return false;
        }
    }
    g_hooked = true;
    Log("VRFG: hooks installed on IVRCompositor_029 (WaitGetPoses, Submit, SubmitWithArrayIndex, "
        "PostPresentHandoff).");
    return true;
}

void ReleaseSession()
{
    std::string reason;
    g_interop.WaitIdle(kInteropTimeoutMs, reason);
    g_interpolator.Release();
    for (EyeState& state : g_eyes)
    {
        for (GuideEntry& guide : state.guides)
        {
            g_interop.Release(guide.depth);
            g_interop.Release(guide.motion);
            guide = GuideEntry{};
        }
        for (EyeSource& source : state.sources)
        {
            g_interop.Release(source.shared);
            source = EyeSource{};
        }
        for (uint32_t slot = 0; slot < kSubmitEntries; slot++)
        {
            state.submitReal[slot].Reset();
            state.submitGenerated[slot].Reset();
            state.submitRealState[slot] = D3D12_RESOURCE_STATE_COMMON;
            state.submitGeneratedState[slot] = D3D12_RESOURCE_STATE_COMMON;
        }
        state.width = 0;
        state.height = 0;
        state.sourcesReady = false;
        state.sourceWidth = 0;
        state.sourceHeight = 0;
        state.submitted = false;
        state.submittedSerial = 0;
    }
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_queue.clear();
        g_submittedRealSerial = 0;
    }
    g_interop.ReleaseAll();
    g_device.Reset();
    g_context.Reset();
    g_context4.Reset();
}

bool ResolveInterfaces()
{
    if (g_compositor != nullptr && g_system != nullptr && g_settings != nullptr) return true;
    HMODULE module = GetModuleHandleW(L"openvr_api.dll");
    if (module == nullptr) return false;
    auto getInterface = reinterpret_cast<void* (__cdecl*)(const char*, vr::EVRInitError*)>(
        GetProcAddress(module, "VR_GetGenericInterface"));
    if (getInterface == nullptr) return false;
    vr::EVRInitError error = vr::VRInitError_None;
    void* compositor = getInterface(vr::IVRCompositor_Version, &error);
    void* system = getInterface(vr::IVRSystem_Version, &error);
    void* settings = getInterface(vr::IVRSettings_Version, &error);
    if (compositor == nullptr || system == nullptr || settings == nullptr)
    {
        Log("VRFG: SteamVR did not hand out the compositor, system and settings interfaces (%d).",
            static_cast<int>(error));
        return false;
    }
    g_compositor = static_cast<vr::IVRCompositor*>(compositor);
    g_system = static_cast<vr::IVRSystem*>(system);
    g_settings = static_cast<vr::IVRSettings*>(settings);
    return true;
}

bool Activate()
{
    if (g_refused) return false;
    auto* device = static_cast<ID3D11Device*>(BIGSCALE_GetBoundDevice());
    if (device == nullptr || g_compositor == nullptr || g_system == nullptr)
    {
        SetState(BIGSCALE_VRFG_STATE_WAITING, "waiting for SteamVR");
        return false;
    }
    std::string reason;
    if (g_device.Get() != device)
    {
        g_device = device;
        g_context.Reset();
        g_context4.Reset();
        g_device->GetImmediateContext(&g_context);
        if (g_context != nullptr) g_context->QueryInterface(IID_PPV_ARGS(&g_context4));
        if (g_context4 == nullptr)
        {
            Fail("VR frame generation needs the game's Direct3D 11.4 immediate context");
            return false;
        }
    }
    if (!g_hooked && !InstallHooks(reason))
    {
        Fail(reason.c_str());
        return false;
    }
    if (!g_interop.Ensure(g_device.Get(), reason))
    {
        SetState(BIGSCALE_VRFG_STATE_WAITING, "waiting for SteamVR");
        Log("VRFG: the D3D12 interop device is not ready yet: %s", reason.c_str());
        return false;
    }
    const wchar_t* runtimeDirectory = BIGSCALE_GetRuntimeDirectory();
    if (runtimeDirectory == nullptr || !g_interpolator.Load(runtimeDirectory, reason))
    {
        const std::string message = "AMD FidelityFX frame generation is unavailable: " + reason;
        Fail(message.c_str());
        return false;
    }
    g_trackingSpace = g_compositor->GetTrackingSpace();
    vr::ETrackedPropertyError error = vr::TrackedProp_Success;
    float hz = g_system->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
        vr::Prop_DisplayFrequency_Float, &error);
    if (!(hz > 1.0f) || !std::isfinite(hz)) hz = 90.0f;
    g_displayHz.store(hz);
    g_displayPeriod = 1.0f / hz;
    float vsyncToPhotons = g_system->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
        vr::Prop_SecondsFromVsyncToPhotons_Float, &error);
    if (!(vsyncToPhotons >= 0.0f) || vsyncToPhotons > 0.1f) vsyncToPhotons = 0.011f;
    g_vsyncToPhotons = vsyncToPhotons;
    bool smoothing = false;
    if (g_settings != nullptr)
    {
        vr::EVRSettingsError settingsError = vr::VRSettingsError_None;
        smoothing = g_settings->GetBool("steamvr", "motionSmoothing", &settingsError);
    }
    g_motionSmoothing.store(smoothing ? 1u : 0u);
    if (smoothing)
        Log("VRFG: SteamVR Motion Smoothing is on; it can halve the frame rate with VR frame generation.");
    g_eyeToHeadValid = false;
    if (g_system != nullptr)
    {
        for (int32_t eye = 0; eye < 2; eye++)
        {
            g_eyeToHead[eye] = g_system->GetEyeToHeadTransform(static_cast<vr::EVREye>(eye));
            if (g_eyeToHead[eye].m[0][0] != 0.0f || g_eyeToHead[eye].m[1][1] != 0.0f ||
                g_eyeToHead[eye].m[2][2] != 0.0f)
                g_eyeToHeadValid = true;
        }
    }
    for (uint32_t i = 0; i < kPoseHistory; i++) g_poseValid[i] = false;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_queue.clear();
        g_submittedRealSerial = 0;
    }
    g_clockOrigin = NowQpc();
    g_nextGameStart = 2.0 * g_displayPeriod;
    g_gameSerial.store(0);
    g_lastFinalizeQpc = 0;
    g_lastWriteWaitSerial = 0;
    g_writeWaitDone = false;
    g_packetSerial = 0;
    g_activeAnnounced.store(false);
    g_stop.store(false);
    g_active.store(true);
    g_presenter = std::thread(PresentLoop);
    SetState(BIGSCALE_VRFG_STATE_WAITING, "waiting for the first generated frame");
    Log("VRFG: active display=%.1f Hz.", hz);
    return true;
}

void Deactivate()
{
    if (g_active.exchange(false))
    {
        g_stop.store(true);
        g_releaseCv.notify_all();
        if (g_presenter.joinable()) g_presenter.join();
        Log("VRFG: summary real=%llu generated=%llu repeated=%llu dropped=%llu timeouts=%llu",
            static_cast<unsigned long long>(g_realFrames.load()),
            static_cast<unsigned long long>(g_generatedFrames.load()),
            static_cast<unsigned long long>(g_repeatedSlots.load()),
            static_cast<unsigned long long>(g_droppedFrames.load()),
            static_cast<unsigned long long>(g_pacingTimeouts.load()));
        ReleaseSession();
        Log("VRFG: deactivated.");
    }
    SetState(BIGSCALE_VRFG_STATE_OFF, "");
}
}

vr::EVRCompositorError HookWaitGetPoses(void* self, vr::TrackedDevicePose_t* renderPoses, uint32_t renderCount,
                                       vr::TrackedDevicePose_t* gamePoses, uint32_t gameCount)
{
    if (self != g_compositor) return g_origWaitGetPoses(self, renderPoses, renderCount, gamePoses, gameCount);
    const uint32_t current = GetCurrentThreadId();
    uint32_t known = g_renderThreadId.load();
    if (known == 0)
    {
        g_renderThreadId.store(current);
        known = current;
    }
    if (current != known) return g_origWaitGetPoses(self, renderPoses, renderCount, gamePoses, gameCount);
    if (!g_active.load(std::memory_order_acquire))
    {
        if (!g_requested.load() || !Activate() || !g_active.load())
            return g_origWaitGetPoses(self, renderPoses, renderCount, gamePoses, gameCount);
    }
    else if (!g_requested.load())
    {
        Deactivate();
        return g_origWaitGetPoses(self, renderPoses, renderCount, gamePoses, gameCount);
    }
    if (renderPoses == nullptr || renderCount == 0 || g_system == nullptr)
        return g_origWaitGetPoses(self, renderPoses, renderCount, gamePoses, gameCount);

    // The virtual half-rate boundary: one game frame per two compositor frames. Pacing is a deadline
    // rather than a handshake with the presenter, so the game always has both slots that follow its
    // previous submit to render the next frame; a stalled presenter only costs repeated slots.
    const double now = NowSeconds();
    const double step = 2.0 * g_displayPeriod;
    const double start = g_nextGameStart > now ? g_nextGameStart : now;
    g_nextGameStart = start + step;
    if (start > now)
    {
        std::this_thread::sleep_for(std::chrono::duration<double>(start - now));
        const double late = NowSeconds() - start;
        if (late > 4.0 * g_displayPeriod) g_pacingTimeouts.fetch_add(1);
    }
    if (!g_active.load(std::memory_order_acquire))
        return g_origWaitGetPoses(self, renderPoses, renderCount, gamePoses, gameCount);

    float sinceVsync = 0.0f;
    uint64_t frameCounter = 0;
    if (!g_system->GetTimeSinceLastVsync(&sinceVsync, &frameCounter)) sinceVsync = 0.0f;
    const float predicted = std::max(0.0f, g_displayPeriod - sinceVsync) + 3.0f * g_displayPeriod +
        g_vsyncToPhotons;
    g_system->GetDeviceToAbsoluteTrackingPose(g_trackingSpace, predicted, renderPoses, renderCount);
    if (gamePoses != nullptr && gameCount > 0)
        g_system->GetDeviceToAbsoluteTrackingPose(g_trackingSpace, predicted + 2.0f * g_displayPeriod, gamePoses,
            gameCount);
    const uint64_t serial = g_gameSerial.fetch_add(1) + 1;
    const uint32_t index = static_cast<uint32_t>(serial % kPoseHistory);
    g_poses[index] = renderPoses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
    g_poseValid[index] = renderPoses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid;
    return vr::VRCompositorError_None;
}

void VrFgConfigure(bool enabled)
{
    g_requested.store(enabled);
    if (!enabled)
    {
        if (!g_active.load()) SetState(BIGSCALE_VRFG_STATE_OFF, "");
        return;
    }
    if (g_refused || g_active.load()) return;
    // Installation is idempotent and retried by the managed side while SteamVR is not ready yet; the
    // detours stay for the process and are only *gated* by the active flag afterwards.
    if (!ResolveInterfaces())
    {
        SetState(BIGSCALE_VRFG_STATE_WAITING, "waiting for SteamVR");
        return;
    }
    if (!g_hooked)
    {
        std::string reason;
        if (!InstallHooks(reason))
        {
            Fail(reason.c_str());
            return;
        }
    }
    SetState(BIGSCALE_VRFG_STATE_WAITING, "waiting for the first generated frame");
}

void VrFgShutdown()
{
    g_requested.store(false);
    if (!g_active.load())
    {
        SetState(BIGSCALE_VRFG_STATE_OFF, "");
        return;
    }
    // Called from the managed main thread during plugin unload; the presenter is joined here too so
    // the detours are inert the moment this returns.
    if (GetCurrentThreadId() != g_renderThreadId.load()) Deactivate();
}

void VrFgFillStatus(BIGSCALE_VRFG_Status& status)
{
    status.abi = BIGSCALE_VRFG_ABI_VERSION;
    status.size = sizeof(BIGSCALE_VRFG_Status);
    status.state = g_state.load();
    status.lastResult = g_lastResult.load();
    status.displayHz = g_displayHz.load();
    status.motionSmoothing = g_motionSmoothing.load();
    status.realFrames = g_realFrames.load();
    status.generatedFrames = g_generatedFrames.load();
    status.repeatedSlots = g_repeatedSlots.load();
    status.droppedFrames = g_droppedFrames.load();
    status.pacingTimeouts = g_pacingTimeouts.load();
    std::lock_guard<std::mutex> guard(g_reasonMutex);
    std::memset(status.reason, 0, sizeof(status.reason));
    std::memcpy(status.reason, g_reason, std::strlen(g_reason));
}

#if defined(BIGSCALE_TESTING)
void VrFgTestUseInterfaces(void* compositor, void* system, void* settings)
{
    g_compositor = static_cast<vr::IVRCompositor*>(compositor);
    g_system = static_cast<vr::IVRSystem*>(system);
    g_settings = static_cast<vr::IVRSettings*>(settings);
}
#endif
}
