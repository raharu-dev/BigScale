#include "FgTestObserver.h"

#include "../Log.h"
#include "FgCapture.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace bigscale
{
#ifdef BIGSCALE_TESTING
namespace
{
FgTestObserver g_observer = nullptr;

// Fixed-size, allocation-free trace state. It is read and written from the owner thread and the
// frame-generation worker, so every access is under one short mutex; no lock is ever held across a
// provider call or a GPU wait.
std::mutex g_traceMutex;
const char* g_phase = "setup";
// FSR reset diagnostic switch. Read by the provider on every Record and written by the harness
// before the diagnostic phase; atomic so neither side needs the trace mutex around it.
std::atomic<bool> g_fsrResetIndicators{false};
// Four begin/end traces per stage and phase are enough to locate an ordering boundary.
constexpr int kMaxBeginEndFrames = 4;
constexpr int kTraceSlots = 48;

struct TraceSlot
{
    const char* stage = nullptr;
    const char* phase = nullptr;
    uint32_t generation = 0;
    int32_t frame = -1;
    int count = 0;
    bool used = false;
};

TraceSlot g_trace[kTraceSlots];

bool IsRejectionStage(const char* stage)
{
    return std::strstr(stage, "rejected") != nullptr || std::strstr(stage, "-missing") != nullptr ||
        std::strstr(stage, "-unclaimed") != nullptr || std::strcmp(stage, "worker-stale") == 0;
}

TraceSlot* FindSlotLocked(const char* stage, const char* phase, uint32_t generation)
{
    for (TraceSlot& slot : g_trace)
    {
        if (!slot.used) continue;
        if (slot.stage != stage) continue;
        if (IsRejectionStage(stage))
        {
            if (slot.generation == generation) return &slot;
        }
        else if (slot.phase == phase)
        {
            return &slot;
        }
    }
    for (TraceSlot& slot : g_trace)
    {
        if (slot.used) continue;
        slot.used = true;
        slot.stage = stage;
        slot.phase = phase;
        slot.generation = generation;
        return &slot;
    }
    // The table is bounded; a full table only means later stages are not reported.
    return nullptr;
}
}

void SetFgTestObserver(FgTestObserver observer) { g_observer = observer; }

void ObserveFgGeneratedImage(const FgTestGeneratedImage& image)
{
    FgCaptureObserve(image);
    const FgTestObserver observer = g_observer;
    if (observer != nullptr) observer(&image, nullptr);
}

void ObserveFgPairTiming(const FgTestPairTiming& timing)
{
    const FgTestObserver observer = g_observer;
    if (observer != nullptr) observer(nullptr, &timing);
}

void SetFgTestPhase(const char* phase)
{
    std::lock_guard<std::mutex> guard(g_traceMutex);
    g_phase = phase != nullptr ? phase : "setup";
}

void SetFgTestFsrResetIndicators(bool enabled) { g_fsrResetIndicators.store(enabled); }

bool FgTestFsrResetIndicators() { return g_fsrResetIndicators.load(); }

void TraceFgBoundary(const char* stage, int32_t provider, uint32_t generation, int32_t sourceFrame,
    const char* detail)
{
    if (stage == nullptr) return;
    const char* phase = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_traceMutex);
        TraceSlot* slot = FindSlotLocked(stage, g_phase, generation);
        if (slot == nullptr) return;
        const bool rejection = IsRejectionStage(stage);
        if (slot->count != 0 && rejection) return;
        if (slot->count >= kMaxBeginEndFrames) return;
        slot->count++;
        slot->frame = sourceFrame;
        phase = g_phase;
    }
    Log("FGTRACE stage=%s phase=%s provider=%d generation=%u frame=%d thread=%lu detail=%s", stage, phase,
        provider, generation, sourceFrame, static_cast<unsigned long>(GetCurrentThreadId()),
        detail != nullptr ? detail : "");
}
#else
void ObserveFgGeneratedImage(const FgTestGeneratedImage& image) { FgCaptureObserve(image); }
#endif
}
