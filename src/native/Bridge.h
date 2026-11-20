// BigScale — native bridge ABI.
//
// The exported surface is a flat C ABI with sequential Pack=8 structs. No C++ objects, no
// bool, no references cross the boundary; every field is a fixed-width integer, float or raw
// pointer so the managed mirror in Native.cs can be validated field-by-field at startup.
#pragma once

#include <cstdint>

#if defined(_WIN32)
#define BIGSCALE_EXPORT extern "C" __declspec(dllexport)
#else
#define BIGSCALE_EXPORT extern "C"
#endif

// Bumped whenever a shared struct changes. Both sides refuse a mismatch.
#define BIGSCALE_ABI_VERSION 4u

// Operation codes carried by BIGSCALE_Frame::operation.
enum BIGSCALE_Operation : int32_t
{
    BIGSCALE_OP_PROBE = 0,              // capability + supported-size query only, never evaluates
    BIGSCALE_OP_EVALUATE = 1,           // reconstruction
    BIGSCALE_OP_BILINEAR_COMPARISON = 2,// explicit non-temporal comparison at the same input size
    BIGSCALE_OP_SHUTDOWN = 3,           // drain, release the backend, shut the SDK down
    BIGSCALE_OP_RECOVERY = 4,           // drain, drop faulted state, allow a fresh probe
    BIGSCALE_OP_SWITCH = 5,             // drain the old provider, acknowledge the selected one
};

// Stable backend identity. Never renumber; managed UpscalerKind mirrors these values.
enum BIGSCALE_Upscaler : int32_t
{
    BIGSCALE_UPSCALER_OFF = 0,
    BIGSCALE_UPSCALER_DLSS = 1,
    BIGSCALE_UPSCALER_FSR2 = 2,
    BIGSCALE_UPSCALER_FSR3 = 3,
    BIGSCALE_UPSCALER_FSR4 = 4,
    BIGSCALE_UPSCALER_XESS = 5,
    BIGSCALE_UPSCALER_DLSS_PRESET_L = 6,
};

// Provider-neutral preset. Mode block order is Quality, Balanced, Performance, UltraPerformance,
// NativeAA. Vendor quality numbers never cross the ABI.
enum BIGSCALE_Preset : int32_t
{
    BIGSCALE_PRESET_QUALITY = 1,
    BIGSCALE_PRESET_BALANCED = 2,
    BIGSCALE_PRESET_PERFORMANCE = 3,
    BIGSCALE_PRESET_ULTRA_PERFORMANCE = 4,
    BIGSCALE_PRESET_NATIVE_AA = 5,
};

// Frame layout kinds.
enum BIGSCALE_Layout : int32_t
{
    BIGSCALE_LAYOUT_2D = 0,      // one logical view per texture, full texture is the eye
    BIGSCALE_LAYOUT_ARRAY = 1,   // single-pass stereo: one slice per logical view
    BIGSCALE_LAYOUT_PACKED = 2,  // several viewports packed into one texture
};

// BIGSCALE_Frame::flags bits.
enum BIGSCALE_Flags : int32_t
{
    BIGSCALE_FLAG_REVERSED_DEPTH = 1 << 0,
    BIGSCALE_FLAG_HDR_COLOR = 1 << 1,
    BIGSCALE_FLAG_RESET = 1 << 2,
    // Diagnostics only: sample the reconstruction input and the written engine output, and count how
    // often each stage changes, so a frozen image can be attributed to one stage. Carries no payload.
    BIGSCALE_FLAG_STAGE_PROBE = 1 << 3,
    // Diagnostics only: read one centre texel of the converted input and of the written engine output
    // back into the native log for the first evaluations and then every 120th. No marker, no file.
    BIGSCALE_FLAG_SAMPLE_INPUT = 1 << 4,
    // Diagnostics only: stamp this eye's identity bar (vertical for eye 0, horizontal for eye 1) into
    // the written engine output, so a compositor image can be matched to the eye that produced it.
    // Verified offline by StageProbe::ClassifyIdentityMarker; never part of a normal launch.
    BIGSCALE_FLAG_STAGE_MARKER = 1 << 5,
};

// Status state machine, mirrored by managed RendererState.
enum BIGSCALE_State : int32_t
{
    BIGSCALE_STATE_WAITING = 0,
    BIGSCALE_STATE_ACTIVE = 1,
    BIGSCALE_STATE_DISABLED = 2,
    BIGSCALE_STATE_UNSUPPORTED = 3,
    BIGSCALE_STATE_FAULTED = 4,
    BIGSCALE_STATE_SHUTDOWN = 5,
};

// Stable last-error codes. Zero means no error recorded. 4-7 keep their original numeric values
// but are now backend-neutral.
enum BIGSCALE_Error : int32_t
{
    BIGSCALE_ERROR_NONE = 0,
    BIGSCALE_ERROR_ABI_MISMATCH = 1,
    BIGSCALE_ERROR_DEVICE_UNAVAILABLE = 2,
    BIGSCALE_ERROR_DEVICE_LOST = 3,
    BIGSCALE_ERROR_BACKEND_INIT_FAILED = 4,
    BIGSCALE_ERROR_BACKEND_UNSUPPORTED = 5,
    BIGSCALE_ERROR_BACKEND_CREATE_FAILED = 6,
    BIGSCALE_ERROR_BACKEND_EVALUATE_FAILED = 7,
    BIGSCALE_ERROR_OPTIMAL_SETTINGS_FAILED = 8,
    BIGSCALE_ERROR_RESOURCE_FORMAT = 9,
    BIGSCALE_ERROR_RESOURCE_SIZE = 10,
    BIGSCALE_ERROR_RING_FULL = 11,
    BIGSCALE_ERROR_RENDER_EVENT_FAILED = 12,
    BIGSCALE_ERROR_SHADER_COMPILE = 13,
    BIGSCALE_ERROR_UNSUPPORTED_LAYOUT = 14,
    BIGSCALE_ERROR_BASELINE_FAILED = 15,
    BIGSCALE_ERROR_STATE_SWAP_FAILED = 16,
    BIGSCALE_ERROR_COMPLETION_OVERFLOW = 17,
    BIGSCALE_ERROR_RETIREMENT_FAILED = 18,
    // An old-provider packet retired without executing. Not a fault of the new provider.
    BIGSCALE_ERROR_STALE_PACKET = 19,
};

constexpr int32_t BIGSCALE_RING_CAPACITY = 32;
constexpr int32_t BIGSCALE_MODE_COUNT = 5;
constexpr int32_t BIGSCALE_MAX_VIEWS = 2;
// A status snapshot carries at most this many oldest-unacknowledged completions.
constexpr int32_t BIGSCALE_COMPLETION_COUNT = 8;
// Unacknowledged completions retained natively. Managed acknowledges by sequence after consuming a
// snapshot, so a slow reader can never lose a record and a fast writer faults instead of overwriting.
constexpr int32_t BIGSCALE_COMPLETION_QUEUE = 64;
// In-flight retirement queries. A packet without a retirement slot is refused before issuing GPU work.
constexpr int32_t BIGSCALE_PENDING_COUNT = 16;
// Fixed UTF-8 reason capacity in the status snapshot.
constexpr int32_t BIGSCALE_REASON_CAPACITY = 256;

// One preset's reported capability at the current output size. Index order matches
// BIGSCALE_MODE_COUNT: Quality, Balanced, Performance, UltraPerformance, NativeAA.
struct BIGSCALE_ModeInfo
{
    uint32_t supported;      // 1 when the preset is usable at this output size
    uint32_t optimalWidth;   // SDK reported optimal input width
    uint32_t optimalHeight;
    uint32_t minWidth;       // SDK reported supported input range
    uint32_t minHeight;
    uint32_t maxWidth;
    uint32_t maxHeight;
    uint32_t reserved;
};

// Per logical view counters. Identity is the (camera, eye) pair, never the frame number.
struct BIGSCALE_ViewInfo
{
    uint32_t eye;
    uint32_t evaluations;
    int32_t lastFrame;
    int32_t lastEpoch;
    uint32_t failures;
    uint32_t resets;
};

// Completion kinds. EXECUTED records that the packet ran on the render thread; RETIRED records that
// its GPU work provably finished. Only a RETIRED record may release a resource or verify activity.
enum BIGSCALE_CompletionKind : uint32_t
{
    BIGSCALE_COMPLETION_EXECUTED = 0,
    BIGSCALE_COMPLETION_RETIRED = 1,
};

// One packet completion. Records are immutable once published and retained until acknowledged.
struct BIGSCALE_Completion
{
    uint32_t sequence;      // strictly increasing, starts at 1
    uint32_t kind;          // BIGSCALE_CompletionKind
    uint32_t resourceTag;   // echoed from the packet, identifies the managed output generation
    int32_t cameraId;
    int32_t eye;
    int32_t frameIndex;
    int32_t historyEpoch;
    int32_t operation;
    int32_t result;         // 0 on success, otherwise the error/SDK result code
    int32_t inputWidth;
    int32_t inputHeight;
    int32_t outputWidth;
    int32_t outputHeight;
    int32_t upscaler;       // provider that produced this packet
    int32_t preset;
    int32_t layout;
    int32_t slice;
    int32_t sliceCount;
    uint32_t reserved;
};

// Sequential Pack=8. Offsets are asserted in Bridge.cpp and compared against managed
// Marshal.OffsetOf at startup.
struct BIGSCALE_Frame
{
    uint32_t abi;             // 0
    uint32_t size;            // 4
    int32_t cameraId;         // 8
    int32_t eye;              // 12
    int32_t frameIndex;       // 16
    int32_t historyEpoch;     // 20
    int32_t operation;        // 24
    int32_t preset;           // 28
    int32_t inputWidth;       // 32
    int32_t inputHeight;      // 36
    int32_t outputWidth;      // 40
    int32_t outputHeight;     // 44
    int32_t inputX;           // 48
    int32_t inputY;           // 52
    int32_t outputX;          // 56
    int32_t outputY;          // 60
    int32_t slice;            // 64
    int32_t sliceCount;       // 68
    int32_t flags;            // 72
    int32_t layout;           // 76
    void* color;              // 80
    void* depth;              // 88
    void* motion;             // 96
    void* output;             // 104
    float jitterX;            // 112
    float jitterY;            // 116
    float motionScaleX;       // 120
    float motionScaleY;       // 124
    float deltaMilliseconds;  // 128
    int32_t motionWidth;      // 132
    int32_t motionHeight;     // 136
    int32_t reserved;         // 140
    uint32_t resourceTag;     // 144
    int32_t upscaler;         // 148
    float cameraNear;         // 152
    float cameraFar;          // 156
    float verticalFovRadians; // 160
    float preExposure;        // 164
    uint32_t fgCaptureTag;    // 168
};

// Thread-safe copied snapshot. BIGSCALE_GetStatus never calls an SDK or touches a D3D context.
struct BIGSCALE_Status
{
    uint32_t abi;                     // 0
    uint32_t size;                    // 4
    int32_t state;                    // 8
    int32_t lastResult;               // 12
    int32_t lastError;                // 16
    uint32_t runtimeVersion;          // 20
    int32_t layout;                   // 24
    int32_t preset;                   // 28
    uint32_t outputWidth;             // 32
    uint32_t outputHeight;            // 36
    uint32_t inputWidth;              // 40
    uint32_t inputHeight;             // 44
    uint64_t totalEvaluations;        // 48
    uint32_t probeCount;              // 56
    uint32_t modeCount;               // 60
    BIGSCALE_ModeInfo modes[BIGSCALE_MODE_COUNT];        // 64
    BIGSCALE_ViewInfo views[BIGSCALE_MAX_VIEWS];         // 224
    uint32_t viewCount;               // 272
    uint32_t bilinearComparisons;     // 276
    uint32_t faultCount;              // 280
    uint32_t reserved;                // 284
    uint32_t completionPublished;     // 288 oldest-unacknowledged records retained natively
    uint32_t completionBase;          // 292 sequence of the oldest retained record (0 when empty)
    BIGSCALE_Completion completions[BIGSCALE_COMPLETION_COUNT]; // 296, oldest first
    int32_t queryUpscaler;            // 904 in: provider whose mode block is requested
    int32_t activeUpscaler;           // 908 the backend that actually produced the last frame
    uint64_t providerVersionId;       // 912 64-bit implementation identity, zero when unavailable
    char backendReason[BIGSCALE_REASON_CAPACITY];      // 920 fixed UTF-8, NUL terminated
};

BIGSCALE_EXPORT uint32_t BIGSCALE_GetAbiVersion();
BIGSCALE_EXPORT uint32_t BIGSCALE_GetFrameSize();
BIGSCALE_EXPORT uint32_t BIGSCALE_GetStatusSize();
BIGSCALE_EXPORT uint32_t BIGSCALE_GetModeInfoSize();
BIGSCALE_EXPORT uint32_t BIGSCALE_GetViewInfoSize();
BIGSCALE_EXPORT uint32_t BIGSCALE_GetCompletionSize();
// Writes the native byte offset of each field in declaration order into offsets[0..count).
// Returns the number of fields available so the caller can size its table.
BIGSCALE_EXPORT uint32_t BIGSCALE_GetFrameLayout(uint32_t* offsets, uint32_t count);
BIGSCALE_EXPORT uint32_t BIGSCALE_GetStatusLayout(uint32_t* offsets, uint32_t count);
BIGSCALE_EXPORT uint32_t BIGSCALE_GetCompletionLayout(uint32_t* offsets, uint32_t count);
BIGSCALE_EXPORT int32_t BIGSCALE_Initialize(const wchar_t* runtimeDirectory, const wchar_t* dataDirectory);
BIGSCALE_EXPORT void BIGSCALE_SetEngineVersion(const char* engineVersion);
BIGSCALE_EXPORT void* BIGSCALE_GetRenderEvent();
// The exact D3D11 device the renderer is bound to, or null before the first packet.
BIGSCALE_EXPORT void* BIGSCALE_GetBoundDevice();
// The plugin directory NVAPI-adjacent runtimes are loaded from (FidelityFX, XeSS, Streamline).
BIGSCALE_EXPORT const wchar_t* BIGSCALE_GetRuntimeDirectory();
BIGSCALE_EXPORT void* BIGSCALE_QueueFrame(const BIGSCALE_Frame* frame);
BIGSCALE_EXPORT int32_t BIGSCALE_GetStatus(BIGSCALE_Status* status);
// Drops every retained completion whose sequence is <= throughSequence.
BIGSCALE_EXPORT void BIGSCALE_AckCompletions(uint32_t throughSequence);
BIGSCALE_EXPORT void BIGSCALE_RequestShutdown();

#if defined(BIGSCALE_TESTING)
// Native-check-only regression seam, compiled only into BigScaleChecks: the next evaluation
// skips the SDK call and reports this result instead, while the bridge's own baseline, fault,
// completion and retirement path runs unchanged.
BIGSCALE_EXPORT void BIGSCALE_TestForceNextEvaluationResult(int32_t result);
#endif
