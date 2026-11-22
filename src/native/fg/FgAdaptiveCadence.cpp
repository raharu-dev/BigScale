#include "FgAdaptiveCadence.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bigscale
{
namespace
{
uint32_t ClampTwoToSix(uint32_t value)
{
    if (value < 2) return 2;
    if (value > 6) return 6;
    return value;
}
}

uint32_t FgChoosePresentedCount(const FgCadenceInput& input)
{
    const uint32_t configured = ClampTwoToSix(input.configuredMultiplier);
    const uint32_t provider = ClampTwoToSix(input.providerMaximum);
    const uint32_t ceiling = std::min(configured, provider);
    // An unknowable display rate keeps the request untouched: the governor only pauses against a
    // refresh interval it actually measured.
    if (!std::isfinite(input.refreshIntervalMilliseconds) || input.refreshIntervalMilliseconds <= 0.0)
        return ceiling;
    if (!std::isfinite(input.sourceIntervalMilliseconds) || input.sourceIntervalMilliseconds <= 0.0)
        return ceiling;
    // The 0.75 ms tolerance absorbs harness and timer slack around an exact M * refresh source (the
    // native source timer may fire 0.5 ms early). It is far below the next step, so it can never
    // multiply an infeasible rate.
    const double fit = std::floor((input.sourceIntervalMilliseconds + 0.75) /
        input.refreshIntervalMilliseconds);
    if (fit < 1.0) return 1;
    if (fit >= static_cast<double>(ceiling)) return ceiling;
    return static_cast<uint32_t>(fit);
}

void FgMultiplierGovernor::Reset()
{
    average_ = 0.0;
    samples_ = 0;
    current_ = 1;
    raiseFrames_ = 0;
    dropFrames_ = 0;
    configured_ = 0;
}

uint32_t FgMultiplierGovernor::Observe(const FgCadenceInput& input)
{
    const uint32_t configured = ClampTwoToSix(input.configuredMultiplier);
    if (configured != configured_)
    {
        Reset();
        configured_ = configured;
    }
    // No measured refresh means no evidence that any count fails to fit, so the request is kept and
    // the transport behaves exactly as it did before this governor existed.
    if (input.refreshIntervalMilliseconds <= 0.0)
    {
        current_ = configured_;
        raiseFrames_ = 0;
        return current_;
    }
    const double source = input.sourceIntervalMilliseconds;
    if (!std::isfinite(source) || source <= 0.1)
    {
        // An interval this small is not a frame rate: it is a clock that did not advance or a
        // non-positive delta. It is no evidence either way, so the current decision stands.
        average_ = 0.0;
        samples_ = 0;
        raiseFrames_ = 0;
        dropFrames_ = 0;
        return current_;
    }
    if (source > 500.0)
    {
        // A gap far longer than a refresh (a hitch, a loading screen, the window being dragged) can
        // only ever make *more* room for presents. Adopting the ceiling here is what stops a stall
        // from becoming a self-sustaining pause: a paused transport presents nothing, which reads as
        // another long gap, and holding the previous decision would never leave that state.
        FgCadenceInput ceiling = input;
        ceiling.sourceIntervalMilliseconds = std::numeric_limits<double>::infinity();
        average_ = 0.0;
        samples_ = 0;
        raiseFrames_ = 0;
        dropFrames_ = 0;
        current_ = FgChoosePresentedCount(ceiling);
        return current_;
    }
    // EWMA: a raise must be justified by the smoothed interval, so one outlier cannot ratchet the
    // multiplier up into a state the display cannot sustain. The first samples use a fast filter: the
    // interval right after a configuration is measured across window and swapchain setup, so it can
    // be far off the steady rate, and a slow filter would take tens of frames to converge and keep
    // generation paused for that whole time.
    const double alpha = samples_ < 8 ? 0.5 : 0.1;
    if (samples_ == 0)
        average_ = source;
    else
        average_ += alpha * (source - average_);
    samples_++;

    FgCadenceInput sample = input;
    sample.sourceIntervalMilliseconds = source;
    sample.configuredMultiplier = configured_;
    // Both the raw sample and the smoothed interval decide a drop. A source whose present intervals
    // straddle the M * refresh boundary by a couple of milliseconds (ordinary scheduling jitter: the
    // present lands a little before or after the slot) produces isolated samples that do not fit
    // while the rate itself does; acting on the raw sample alone made generation flap between M and
    // 1 every few frames, which cancels and restarts captures and hides the presenter repeatedly.
    const uint32_t fitNow = FgChoosePresentedCount(sample);
    FgCadenceInput smoothed = input;
    smoothed.sourceIntervalMilliseconds = average_;
    smoothed.configuredMultiplier = configured_;
    const uint32_t fitAverage = FgChoosePresentedCount(smoothed);
    if (samples_ == 1)
    {
        // One interval is not a measurement. Start at the ceiling the request allows and let the next
        // samples decide: a rate that cannot fit drops within two frames, while a rate that does fit
        // is never wrongly paused because of a single setup-time outlier.
        FgCadenceInput ceiling = input;
        ceiling.sourceIntervalMilliseconds = std::numeric_limits<double>::infinity();
        current_ = FgChoosePresentedCount(ceiling);
        raiseFrames_ = 0;
        dropFrames_ = 0;
    }
    else if (fitNow < current_ && fitAverage < current_)
    {
        // A rate that is genuinely too fast still drops quickly, but only once the filter agrees: a
        // single short interval (a double-present, a scheduler outlier) can never drop the multiplier
        // on its own.
        dropFrames_++;
        raiseFrames_ = 0;
        if (dropFrames_ >= 2)
        {
            current_ = fitNow;
            dropFrames_ = 0;
            // The filter restarts from the sample that justified the drop. Carrying the pre-drop
            // average would describe a rate that no longer exists and delay the recovery by tens of
            // frames, which showed up as a generation dropout of about a second after every hitch.
            average_ = source;
        }
    }
    else
    {
        // `fitNow == current_` is not a reason to abandon a raise. Raises are gated by the smoothed
        // interval, and a source sitting exactly on an M * refresh boundary alternates between M and
        // M - 1 on raw samples while its average still fits M. Clearing the counter on every equal
        // sample made the raise unconvergeable: a 2x request at 2 x 60 Hz source and 60 Hz display
        // settled at 1x and never generated.
        dropFrames_ = 0;
        if (fitAverage <= current_)
        {
            raiseFrames_ = 0;
        }
        else
        {
            // Raises stay one step at a time and must be justified by the smoothed interval for a run
            // of frames. Ten frames is enough evidence for the filter (0.17 s at 60 fps): thirty made
            // every hitch cost about a second of missing generation before the multiplier could climb
            // back, which is far more visible than a single step at the wrong time.
            raiseFrames_++;
            if (raiseFrames_ >= 10)
            {
                current_ += 1;
                raiseFrames_ = 0;
            }
        }
    }
    return current_;
}
}
