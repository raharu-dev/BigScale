// BigScale — frame-generation ABI (separate from the SR bridge ABI).
//
// A FG packet carries camera/frame metadata and native pointers to the URP companion inputs.
// SubmitFrame copies CPU metadata only. The ordered render event resolves any requested bound RTV
// after preceding command-buffer binds, then copies HUD-less colour, depth and motion into owned
// shared textures; the matching game Present copies the final backbuffer into the slot colour. No
// pointer in this ABI outlives the render event that consumes it, and the native side never keeps a
// Unity resource alive beyond that event.
//
// Every struct is blittable Pack=8 with fixed-width integers, fixed float arrays and raw pointers.
#pragma once

#include <cstdint>

#if defined(_WIN32)
#define BIGSCALE_FG_EXPORT extern "C" __declspec(dllexport)
#else
#define BIGSCALE_FG_EXPORT extern "C"
#endif

#define BIGSCALE_FG_ABI_VERSION 7u
#define BIGSCALE_FG_REASON_CAPACITY 256
#define BIGSCALE_FG_RETIREMENT_COUNT 8
#define BIGSCALE_FG_RETIREMENT_QUEUE 64

// Process-lifetime provider family. The first SDK initialization attempt in a process claims one
// family, and the two families can never share a process. Never renumber.
#define BIGSCALE_FG_FAMILY_UNUSED 0u
#define BIGSCALE_FG_FAMILY_NORMAL 1u
#define BIGSCALE_FG_FAMILY_ANR 2u
// Streamline is a once-per-process SDK, so it is its own process family like ANR.
#define BIGSCALE_FG_FAMILY_STREAMLINE 3u

// Requested/active provider identity. Never renumber.
enum BIGSCALE_FG_Provider : int32_t
{
    BIGSCALE_FG_PROVIDER_OFF = 0,
    BIGSCALE_FG_PROVIDER_NVIDIA = 1,
    BIGSCALE_FG_PROVIDER_FSR = 2,
    BIGSCALE_FG_PROVIDER_XESS = 3,
    BIGSCALE_FG_PROVIDER_ANR = 4,
    BIGSCALE_FG_PROVIDER_STREAMLINE = 5,
};

enum BIGSCALE_FG_State : int32_t
{
    BIGSCALE_FG_STATE_OFF = 0,
    BIGSCALE_FG_STATE_WAITING_SWAPCHAIN = 1,
    BIGSCALE_FG_STATE_WAITING_WINDOW = 2,
    BIGSCALE_FG_STATE_INITIALIZING = 3,
    BIGSCALE_FG_STATE_WAITING_FIRST_GENERATED = 4,
    BIGSCALE_FG_STATE_ACTIVE = 5,
    BIGSCALE_FG_STATE_SUSPENDED = 6,
    BIGSCALE_FG_STATE_DRAINING = 7,
    BIGSCALE_FG_STATE_FAULTED = 8,
    BIGSCALE_FG_STATE_UNSUPPORTED = 9,
};

enum BIGSCALE_FG_Flags : uint32_t
{
    BIGSCALE_FG_FLAG_REVERSED_DEPTH = 1u << 0,
    BIGSCALE_FG_FLAG_RESET = 1u << 1,
    BIGSCALE_FG_FLAG_BOUND_RENDER_TARGET = 1u << 2,
};

// Configure copies CPU state only. Generation is bumped by the managed side on any provider,
// camera, extent or history discontinuity. captureReady reports that this frame supplied FG
// capture inputs; it is not super-resolution readiness. `multiplier` is the requested total
// presented frames per source frame (one real frame plus multiplier-1 generated frames).
struct BIGSCALE_FG_Config
{
    uint32_t abi;
    uint32_t size;
    int32_t provider;
    uint32_t generation;
    int32_t primaryCameraId;
    int32_t primaryHistoryEpoch;
    int32_t captureReady;
    int32_t xrActive;
    int32_t menuVisible;
    uint32_t multiplier;
};

// Depth and motion pointers are URP graph textures; the native transport converts their rows into
// presented-backbuffer rows and negates motion y before any provider sees them. Current/previous
// matrices and jitter therefore describe the presented backbuffer's rows (not flipped ones).
struct BIGSCALE_FG_Frame
{
    uint32_t abi;
    uint32_t size;
    uint32_t generation;
    uint32_t slotTag;
    int32_t cameraId;
    int32_t historyEpoch;
    int32_t frameIndex;
    int32_t renderWidth;
    int32_t renderHeight;
    int32_t outputWidth;
    int32_t outputHeight;
    uint32_t flags;
    float jitterX;
    float jitterY;
    float deltaMilliseconds;
    float cameraNear;
    float cameraFar;
    float verticalFovRadians;
    float aspect;
    float cameraPosition[3];
    float cameraUp[3];
    float cameraRight[3];
    float cameraForward[3];
    float currentView[16];
    float currentProjection[16];
    float previousView[16];
    float previousProjection[16];
    const void* depth;
    const void* motion;
    uint64_t presentId;
    int32_t motionWidth;
    int32_t motionHeight;
    // Pre-overlay scene colour. Used by providers that extract the HUD; null for XeSS, which keeps
    // its own AUTO UI handling. This explicit pointer always wins; the bound render target is only
    // consulted in the later render event when it is null and BIGSCALE_FG_FLAG_BOUND_RENDER_TARGET is
    // set, after preceding command-buffer SetRenderTarget calls have executed.
    const void* hudless;
};

struct BIGSCALE_FG_Retirement
{
    uint32_t sequence;
    uint32_t slotTag;
    uint32_t generation;
    int32_t result;
};

struct BIGSCALE_FG_Status
{
    uint32_t abi;
    uint32_t size;
    int32_t state;
    int32_t requestedProvider;
    int32_t activeProvider;
    uint32_t generation;
    int32_t lastResult;
    int32_t lastSourceFrame;
    void* parentHwnd;
    uint64_t renderedPresents;
    uint64_t generatedPresents;
    // Total multiplier actually in force for this generation (0 while inactive), and the maximum
    // total the selected provider proved it supports (0 while unknown).
    uint32_t activeMultiplier;
    uint32_t maxMultiplier;
    // Typed: the selected provider cannot run in this process family and needs a restart. Never
    // inferred from `reason`, which carries arbitrary SDK text.
    uint32_t restartRequired;
    // Process-lifetime provider family claimed by the first SDK initialization attempt
    // (BIGSCALE_FG_FAMILY_*). Lets the UI tell a cross-family selection immediately.
    uint32_t processFamily;
    char reason[BIGSCALE_FG_REASON_CAPACITY];
    uint32_t retirementCount;
    uint32_t reserved;
    BIGSCALE_FG_Retirement retirements[BIGSCALE_FG_RETIREMENT_COUNT];
};

BIGSCALE_FG_EXPORT uint32_t BIGSCALE_FG_GetAbiVersion();
BIGSCALE_FG_EXPORT uint32_t BIGSCALE_FG_GetFrameSize();
BIGSCALE_FG_EXPORT uint32_t BIGSCALE_FG_GetConfigSize();
BIGSCALE_FG_EXPORT uint32_t BIGSCALE_FG_GetStatusSize();
BIGSCALE_FG_EXPORT uint32_t BIGSCALE_FG_GetFrameLayout(uint32_t* offsets, uint32_t count);
BIGSCALE_FG_EXPORT uint32_t BIGSCALE_FG_GetConfigLayout(uint32_t* offsets, uint32_t count);
BIGSCALE_FG_EXPORT uint32_t BIGSCALE_FG_GetStatusLayout(uint32_t* offsets, uint32_t count);
BIGSCALE_FG_EXPORT int32_t BIGSCALE_FG_Configure(const BIGSCALE_FG_Config* config);
BIGSCALE_FG_EXPORT void* BIGSCALE_FG_GetGameWindow();
BIGSCALE_FG_EXPORT int32_t BIGSCALE_FG_CreatePresenterWindow(void* parent);
BIGSCALE_FG_EXPORT void* BIGSCALE_FG_SubmitFrame(const BIGSCALE_FG_Frame* frame);
BIGSCALE_FG_EXPORT int32_t BIGSCALE_FG_GetStatus(BIGSCALE_FG_Status* status);
BIGSCALE_FG_EXPORT void BIGSCALE_FG_AckCaptures(uint32_t throughSequence);
// Diagnostics only: writes the next `pairs` frame-generation pairs to disk. Returns 1 when armed,
// 0 when a capture is already in progress or the diagnostics directory is unavailable.
BIGSCALE_FG_EXPORT int32_t BIGSCALE_FG_RequestCapture(uint32_t pairs);
BIGSCALE_FG_EXPORT void BIGSCALE_FG_Shutdown();
// Latency markers: one call per phase per source id. A frame whose marker set is not armed emits
// none of its phases, so XeLL never sees a partial frame.
BIGSCALE_FG_EXPORT void BIGSCALE_FG_BeginFrame(uint64_t presentId);
BIGSCALE_FG_EXPORT void BIGSCALE_FG_EndSimulation(uint64_t presentId);
BIGSCALE_FG_EXPORT void BIGSCALE_FG_BeginRender(uint64_t presentId);
// Command-buffer plugin event target: event 2 consumes a submitted capture slot.
BIGSCALE_FG_EXPORT void* BIGSCALE_FG_GetRenderEvent();
// Window visibility must be changed on the window's owning thread; false means "not this thread".
BIGSCALE_FG_EXPORT int32_t BIGSCALE_FG_SetPresenterVisible(int32_t visible);
