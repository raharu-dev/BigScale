#include "FgCadence.h"

#include <cmath>

namespace bigscale
{
namespace
{
// The frame-latency waitable is signalled by the compositor. A bounded wait keeps a lost signal a
// reported provider failure instead of a hung worker thread.
constexpr DWORD kAvailabilityTimeout = 1000;
// Timers fire late by design; present as soon as less than this remains instead of sleeping again.
constexpr double kDeadlineSlackSeconds = 0.0005;
// Bounded recomputation: each iteration sleeps for the remaining time measured from QPC.
constexpr int kMaxTimerIterations = 4;

double QpcFrequency()
{
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart > 0 ? static_cast<double>(frequency.QuadPart) : 0.0;
}
}

int64_t FgQpcNow()
{
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

FgCadence::~FgCadence() { Detach(); }

bool FgCadence::Attach(IDXGISwapChain1* swapchain, std::string& reason)
{
    Detach();
    if (swapchain == nullptr)
    {
        reason = "the frame-generation presenter swapchain is unavailable for pacing";
        return false;
    }
    IDXGISwapChain2* swapchain2 = nullptr;
    if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain2))) || swapchain2 == nullptr)
    {
        reason = "the presenter swapchain does not expose frame-latency pacing (IDXGISwapChain2)";
        return false;
    }
    // Latency 1 means the waitable below signals once per compositor release, which is exactly one
    // present's worth of progress. A larger queue would hide the spacing defect instead of pacing it.
    if (FAILED(swapchain2->SetMaximumFrameLatency(1)))
    {
        swapchain2->Release();
        reason = "the presenter swapchain rejected a maximum frame latency of one";
        return false;
    }
    const HANDLE latency = swapchain2->GetFrameLatencyWaitableObject();
    if (latency == nullptr || latency == INVALID_HANDLE_VALUE)
    {
        swapchain2->Release();
        reason = "the presenter swapchain has no frame-latency waitable object";
        return false;
    }
    // High-resolution timers keep the spacing independent of the system timer tick. Older systems
    // reject the flag; a plain waitable timer still beats a spin loop.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (timer == nullptr) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (timer == nullptr)
    {
        swapchain2->Release();
        reason = "the frame-generation pacing timer could not be created";
        return false;
    }
    swapchain_ = swapchain2;
    latency_ = latency;
    timer_ = timer;
    lastPresentQpc_ = 0;
    lastPresentValid_ = false;
    return true;
}

void FgCadence::Reset() { lastPresentValid_ = false; }

void FgCadence::Detach()
{
    // The waitable object and the IDXGISwapChain2 reference are owned by the swapchain; only this
    // helper's reference and its own timer are released here.
    if (swapchain_ != nullptr)
    {
        swapchain_->Release();
        swapchain_ = nullptr;
    }
    latency_ = nullptr;
    if (timer_ != nullptr)
    {
        CloseHandle(timer_);
        timer_ = nullptr;
    }
    lastPresentQpc_ = 0;
    lastPresentValid_ = false;
}

void FgCadence::AfterPresent()
{
    lastPresentQpc_ = FgQpcNow();
    lastPresentValid_ = true;
}

bool FgCadence::BeforeNext(double stepMilliseconds, std::string& reason)
{
    if (swapchain_ == nullptr || latency_ == nullptr || timer_ == nullptr)
    {
        reason = "the presenter pacing state is not attached";
        return false;
    }
    if (!lastPresentValid_)
    {
        // Nothing of this sequence was scanout-submitted, so there is no step to enforce. A history
        // reset deliberately produces exactly this state, which is why the step is only validated
        // when spacing actually has to be derived from it.
        return true;
    }
    if (!std::isfinite(stepMilliseconds) || stepMilliseconds <= 0.0)
    {
        reason = "the frame-generation present step is not a positive finite duration";
        return false;
    }
    // 1. The child swapchain must be able to accept the next present. This is the same
    //    back-pressure a real frame gets, so an occluded or stalled compositor shows up as a wait
    //    rather than a queue of unpresented frames.
    const DWORD available = WaitForSingleObject(latency_, kAvailabilityTimeout);
    if (available != WAIT_OBJECT_0)
    {
        lastPresentValid_ = false;
        reason = available == WAIT_TIMEOUT
            ? "the presenter swapchain never released a frame-latency slot"
            : "waiting for presenter frame latency failed";
        return false;
    }
    // 2. The absolute deadline, always recomputed from QPC so scheduler oversleep never shifts the
    //    following pair. An expired deadline falls straight through to the present.
    const double frequency = QpcFrequency();
    if (frequency <= 0.0)
    {
        lastPresentValid_ = false;
        reason = "the performance counter frequency is unavailable for pacing";
        return false;
    }
    const double deadline = static_cast<double>(lastPresentQpc_) / frequency + stepMilliseconds / 1000.0;
    for (int iteration = 0; iteration < kMaxTimerIterations; iteration++)
    {
        const double remaining = deadline - static_cast<double>(FgQpcNow()) / frequency;
        if (remaining <= kDeadlineSlackSeconds) break;
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(remaining * 10000000.0);
        if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
        {
            lastPresentValid_ = false;
            reason = "the frame-generation pacing timer could not be armed";
            return false;
        }
        const DWORD waitMilliseconds = static_cast<DWORD>(remaining * 1000.0) + 50;
        if (WaitForSingleObject(timer_, waitMilliseconds) != WAIT_OBJECT_0)
        {
            lastPresentValid_ = false;
            reason = "the frame-generation pacing timer did not fire";
            return false;
        }
    }
    lastPresentValid_ = false;
    return true;
}
}
