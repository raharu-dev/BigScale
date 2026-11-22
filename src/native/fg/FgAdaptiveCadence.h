// Display-aware frame-generation cadence.
//
// A requested multiplier of M is only physically displayable when one source frame interval can
// hold M presents at the display refresh: sourceInterval >= M * refresh. When the source runs
// faster than that, the child swapchain can either queue (input latency grows with the backlog) or
// miss vertical blanks (a periodic judder beat). This helper answers two questions with no Windows
// or DXGI dependency: how many total presents per source frame fit right now, and how that count
// should move over time without oscillating.
#pragma once

#include <cstdint>

namespace bigscale
{
struct FgCadenceInput
{
    double sourceIntervalMilliseconds = 0.0;
    double refreshIntervalMilliseconds = 0.0;
    uint32_t configuredMultiplier = 2;
    uint32_t providerMaximum = 6;
};

// Largest total presented frames per source frame that fits the display: 1..ceiling.
uint32_t FgChoosePresentedCount(const FgCadenceInput& input);

class FgMultiplierGovernor
{
public:
    void Reset();
    uint32_t Observe(const FgCadenceInput& input);
    uint32_t Current() const { return current_; }
private:
    double average_ = 0.0;
    uint64_t samples_ = 0;
    uint32_t current_ = 1;
    uint32_t raiseFrames_ = 0;
    uint32_t dropFrames_ = 0;
    uint32_t configured_ = 0;
};
}
