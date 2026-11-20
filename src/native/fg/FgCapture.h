// Diagnostics-only capture of frame-generation pairs.
//
// Armed from the managed side (one hotkey press covers a fixed number of source frames). Every
// observation queues one readback of the pair's real colour, HUD-less colour, generated image and
// guide planes. Nothing here waits on the GPU from the provider thread: the provider submits one
// extra command list behind its own work and the writer thread maps each plane after its fence
// completes.
#pragma once

#include "FgTestObserver.h"

#include <cstdint>

namespace bigscale
{
// Returns false when a capture is already in progress or no diagnostics directory is available.
bool FgCaptureArm(uint32_t pairs);
// Provider worker thread, both provider flavours, every build.
void FgCaptureObserve(const FgTestGeneratedImage& image);
// Drains the writer and releases it. Called from FrameGeneration::Shutdown.
void FgCaptureShutdown();
}
