// Native observer seam for the offline frame-generation checks and the in-game diagnostic capture.
//
// The manual providers own their child swapchain, so nothing outside them can see the generated
// image that actually reaches the display. These structures carry the real resources (and the fence
// that completes every command touching them) to a harness callback, so a readback proves pixels
// instead of counting presents. `ObserveFgGeneratedImage` exists in every build because the
// diagnostics capture consumes it; every other function here is compiled out of the shipping DLL,
// and only the BigScaleChecks target defines BIGSCALE_TESTING.
#pragma once

#include "../FrameGenerationAbi.h"

#include <d3d12.h>

#include <cstdint>

namespace bigscale
{
// One generated image of one source frame, captured on the provider worker thread before the
// provider writes those resources again.
struct FgTestGeneratedImage
{
    const char* provider = nullptr;
    uint32_t generation = 0;
    int32_t sourceFrame = 0;
    bool reset = false;
    // 1-based position of this intermediate inside its source frame, and how many exist. An
    // implementation that duplicated one image would report identical pixels for distinct indices.
    uint32_t intermediateIndex = 1;
    uint32_t intermediateCount = 1;
    // The SDK's own output of this pair, in presented-backbuffer rows.
    ID3D12Resource* generated = nullptr;
    // The resource the provider copies into the child back buffer for the generated image.
    ID3D12Resource* childCopySource = nullptr;
    uint32_t childBufferIndex = 0;
    // Tracked states of the two resources above, so a reader can transition them exactly and
    // restore the provider's own bookkeeping afterwards.
    D3D12_RESOURCE_STATES generatedState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES childCopyState = D3D12_RESOURCE_STATE_COMMON;
    // The real colour resource of the same pair.
    ID3D12Resource* realColor = nullptr;
    // The child swapchain buffer this pair writes and presents. Null when the provider (or a reset
    // pair) wrote no child buffer at all.
    ID3D12Resource* childBackbuffer = nullptr;
    // The exact resources the provider handed the SDK as final colour and pre-overlay colour.
    ID3D12Resource* evaluationColor = nullptr;
    ID3D12Resource* evaluationHudless = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    // When this value completes, every command touching the resources above has finished.
    uint64_t fenceValue = 0;
    // True when the provider presented only the real image for this pair. Such a sample carries no
    // interpolation: `generated` is null and the generated counts must not move.
    bool realOnly = false;
    // The SDK's own interpolation-disable verdict for this pair (byte 0 of its output), when the
    // provider reads one back. Providers without that output leave it false.
    bool interpolationDisabled = false;
    // Tracked states of every resource above, so a reader can transition them exactly and restore
    // the provider's own bookkeeping afterwards.
    D3D12_RESOURCE_STATES childBackbufferState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES realColorState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES evaluationColorState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES evaluationHudlessState = D3D12_RESOURCE_STATE_COMMON;
    // The guide planes of the same pair, in presented-backbuffer rows after the transport's
    // conversion, plus the render extent they describe.
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES motionState = D3D12_RESOURCE_STATE_COMMON;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    // The metadata packet this pair was recorded from. Valid only during the observation call.
    const BIGSCALE_FG_Frame* frame = nullptr;
};

// Present timing of one generated/real pair. QPC ticks, read immediately around each Present call.
struct FgTestPairTiming
{
    const char* provider = nullptr;
    uint32_t generation = 0;
    int32_t sourceFrame = 0;
    // Generated images of this source frame that were actually presented, with the QPC timestamp
    // and HRESULT of each present in increasing intermediate order.
    uint32_t generatedCount = 0;
    int64_t generatedQpc[5]{};
    long generatedResult[5]{};
    int64_t realQpc = 0;
    long realResult = 0;
    // Suppression identity of this pair: the transport's reset flag and, for a provider that reads
    // the SDK's interpolation-disable output, the raw byte it reported. -1 means the provider has
    // no such output, so no disable verdict exists for it.
    bool reset = false;
    int32_t disableByte = -1;
};

// Published by both provider flavours on their worker thread. In a shipping build this only feeds
// the diagnostics capture; in the checks build it also reaches the harness observer.
void ObserveFgGeneratedImage(const FgTestGeneratedImage& image);

#ifdef BIGSCALE_TESTING
using FgTestObserver = void (*)(const FgTestGeneratedImage* image, const FgTestPairTiming* timing);
void SetFgTestObserver(FgTestObserver observer);
// `image` and `timing` are null when only the other kind of observation is published.
void ObserveFgPairTiming(const FgTestPairTiming& timing);
// Test-only phase label the harness sets before each probe phase. It only labels traces.
void SetFgTestPhase(const char* phase);
// Test-only switch for the FSR reset diagnostic. While enabled, the FSR provider ORs the SDK's
// debug reset-indicator flag into its per-frame configure descriptor and publishes the reset
// dispatch's generated plane. Never enabled for quality phases or in a shipping build.
void SetFgTestFsrResetIndicators(bool enabled);
bool FgTestFsrResetIndicators();
// Test-only display refresh override, in milliseconds per refresh. The cadence governor must make the
// same decision on any monitor, so the harness pins the refresh instead of measuring the host's.
// Passing 0 restores the real display: the next eligible parent present re-queries it.
void FgTestSetDisplayRefreshMilliseconds(double milliseconds);
// Test-only boundary trace. `stage` is a fixed literal ("configure", "render-admitted",
// "capture-match-rejected", "present-token-missing", "worker-stale", "record-begin", ...). A
// rejection stage is reported once per generation; a begin/end stage reports the first four source
// frames of each phase. Nothing here changes admission, locks or timing.
void TraceFgBoundary(const char* stage, int32_t provider, uint32_t generation, int32_t sourceFrame,
    const char* detail);
#else
inline void SetFgTestObserver(void (*)(const FgTestGeneratedImage*, const FgTestPairTiming*)) {}
inline void ObserveFgPairTiming(const FgTestPairTiming&) {}
inline void SetFgTestPhase(const char*) {}
inline void SetFgTestFsrResetIndicators(bool) {}
inline bool FgTestFsrResetIndicators() { return false; }
inline void TraceFgBoundary(const char*, int32_t, uint32_t, int32_t, const char*) {}
#endif
}
