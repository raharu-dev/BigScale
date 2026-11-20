// Source-period pacing for the manual presenter sequence.
//
// One source frame becomes M presented frames (one real plus M-1 generated), spaced one source
// period apart on the display. The manual child swaps (NVIDIA/ANR, FSR and XeSS's proxy) play every
// image themselves, so nothing else spaces them: back-to-back presents land on adjacent vertical
// blanks and the sequence is then followed by a full source period of nothing. At a 30 fps source on
// a 120 Hz display that reads as judder even though every image is correct.
//
// This helper owns the waitable swapchain state used for that spacing: DXGI's frame-latency waitable
// proves the child swapchain can accept the next present, and an absolute deadline derived from the
// previous present's QPC timestamp proves the spacing. The XeSS proxy swapchain, the game's own
// swapchain and its frame limiter are never touched.
#pragma once

#include <dxgi1_6.h>
#include <windows.h>

#include <cstdint>
#include <string>

namespace bigscale
{
// Monotonic performance-counter tick, shared by pacing and by the provider-visible present timing.
int64_t FgQpcNow();

class FgCadence
{
public:
    FgCadence() = default;
    ~FgCadence();
    FgCadence(const FgCadence&) = delete;
    FgCadence& operator=(const FgCadence&) = delete;

    // Binds the freshly created child swapchain. The swapchain must have been created with
    // DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT; a missing waitable, a failed latency
    // configuration or a timer that cannot be created fails provider initialization instead of
    // silently presenting the pair unpaced.
    bool Attach(IDXGISwapChain1* swapchain, std::string& reason);
    // Forgets the previous present timestamp without releasing anything. A history reset, a provider
    // switch, Off, a resize and swapchain destruction all invalidate the pending sequence, so the
    // next present must never wait on a deadline derived from a stale image.
    void Reset();
    // Unbinds the swapchain and releases the pacing timer. Idempotent.
    void Detach();
    bool Attached() const { return swapchain_ != nullptr; }
    // Records the QPC timestamp of a present that was actually submitted for scanout.
    void AfterPresent();
    // Waits for the child swapchain's frame-latency slot and for the absolute deadline
    // `lastPresent + stepMilliseconds`, recomputing the remaining time from QPC. An already expired
    // deadline presents immediately; nothing is queued to catch up.
    bool BeforeNext(double stepMilliseconds, std::string& reason);

private:
    IDXGISwapChain2* swapchain_ = nullptr;
    HANDLE latency_ = nullptr;
    HANDLE timer_ = nullptr;
    int64_t lastPresentQpc_ = 0;
    bool lastPresentValid_ = false;
};
}
