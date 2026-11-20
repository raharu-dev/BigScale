// Exported FG ABI surface. Descriptors are validated field-by-field by the managed mirror.
#include "FrameGenerationAbi.h"

#include "FrameGeneration.h"
#include "fg/FgCapture.h"

#include <cstddef>

using namespace bigscale;

namespace
{
template <size_t N>
uint32_t Copy(const uint32_t (&values)[N], uint32_t* offsets, uint32_t count)
{
    if (offsets != nullptr)
        for (uint32_t i = 0; i < count && i < N; i++) offsets[i] = values[i];
    return static_cast<uint32_t>(N);
}
}

static_assert(sizeof(BIGSCALE_FG_Config) == 40, "BIGSCALE_FG_Config size");
static_assert(sizeof(BIGSCALE_FG_Frame) == 424, "BIGSCALE_FG_Frame size");
static_assert(sizeof(BIGSCALE_FG_Retirement) == 16, "BIGSCALE_FG_Retirement size");
static_assert(sizeof(BIGSCALE_FG_Status) == 464, "BIGSCALE_FG_Status size");

BIGSCALE_FG_EXPORT uint32_t BIGSCALE_FG_GetAbiVersion() { return BIGSCALE_FG_ABI_VERSION; }
BIGSCALE_FG_EXPORT uint32_t BIGSCALE_FG_GetFrameSize() { return sizeof(BIGSCALE_FG_Frame); }
BIGSCALE_FG_EXPORT uint32_t BIGSCALE_FG_GetConfigSize() { return sizeof(BIGSCALE_FG_Config); }
BIGSCALE_FG_EXPORT uint32_t BIGSCALE_FG_GetStatusSize() { return sizeof(BIGSCALE_FG_Status); }

BIGSCALE_FG_EXPORT uint32_t BIGSCALE_FG_GetConfigLayout(uint32_t* offsets, uint32_t count)
{
    const uint32_t values[] = {
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Config, abi)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Config, size)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Config, provider)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Config, generation)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Config, primaryCameraId)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Config, primaryHistoryEpoch)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Config, captureReady)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Config, xrActive)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Config, menuVisible)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Config, multiplier)),
    };
    return Copy(values, offsets, count);
}

BIGSCALE_FG_EXPORT uint32_t BIGSCALE_FG_GetFrameLayout(uint32_t* offsets, uint32_t count)
{
    const uint32_t values[] = {
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, abi)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, size)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, generation)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, slotTag)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, cameraId)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, historyEpoch)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, frameIndex)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, renderWidth)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, renderHeight)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, outputWidth)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, outputHeight)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, flags)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, jitterX)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, jitterY)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, deltaMilliseconds)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, cameraNear)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, cameraFar)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, verticalFovRadians)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, aspect)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, cameraPosition)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, cameraUp)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, cameraRight)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, cameraForward)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, currentView)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, currentProjection)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, previousView)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, previousProjection)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, depth)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, motion)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, presentId)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, motionWidth)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, motionHeight)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Frame, hudless)),
    };
    return Copy(values, offsets, count);
}

BIGSCALE_FG_EXPORT uint32_t BIGSCALE_FG_GetStatusLayout(uint32_t* offsets, uint32_t count)
{
    const uint32_t values[] = {
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Status, abi)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Status, size)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Status, state)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Status, requestedProvider)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Status, activeProvider)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Status, generation)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Status, lastResult)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Status, lastSourceFrame)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Status, parentHwnd)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Status, renderedPresents)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Status, generatedPresents)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Status, activeMultiplier)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Status, maxMultiplier)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Status, restartRequired)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Status, processFamily)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Status, reason)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Status, retirementCount)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Status, reserved)),
        static_cast<uint32_t>(offsetof(BIGSCALE_FG_Status, retirements)),
    };
    return Copy(values, offsets, count);
}

BIGSCALE_FG_EXPORT int32_t BIGSCALE_FG_Configure(const BIGSCALE_FG_Config* config)
{
    if (config == nullptr || config->abi != BIGSCALE_FG_ABI_VERSION || config->size != sizeof(BIGSCALE_FG_Config))
        return 0;
    FrameGeneration::Configure(*config);
    return 1;
}

BIGSCALE_FG_EXPORT void* BIGSCALE_FG_GetGameWindow() { return FrameGeneration::GameWindow(); }

BIGSCALE_FG_EXPORT int32_t BIGSCALE_FG_CreatePresenterWindow(void* parent)
{
    return FrameGeneration::CreatePresenterWindow(parent) ? 1 : 0;
}

BIGSCALE_FG_EXPORT void* BIGSCALE_FG_SubmitFrame(const BIGSCALE_FG_Frame* frame)
{
    if (frame == nullptr || frame->abi != BIGSCALE_FG_ABI_VERSION || frame->size != sizeof(BIGSCALE_FG_Frame))
        return nullptr;
    return FrameGeneration::Submit(*frame);
}

BIGSCALE_FG_EXPORT int32_t BIGSCALE_FG_GetStatus(BIGSCALE_FG_Status* status)
{
    if (status == nullptr) return 0;
    FrameGeneration::GetStatus(status);
    return 1;
}

BIGSCALE_FG_EXPORT void BIGSCALE_FG_AckCaptures(uint32_t throughSequence)
{
    FrameGeneration::Ack(throughSequence);
}

BIGSCALE_FG_EXPORT void BIGSCALE_FG_BeginFrame(uint64_t presentId)
{
    FrameGeneration::BeginFrame(presentId);
}

BIGSCALE_FG_EXPORT void BIGSCALE_FG_EndSimulation(uint64_t presentId)
{
    FrameGeneration::EndSimulation(presentId);
}

BIGSCALE_FG_EXPORT void BIGSCALE_FG_BeginRender(uint64_t presentId)
{
    FrameGeneration::BeginRender(presentId);
}

namespace
{
void FrameGenerationRenderEvent(int32_t eventId, void* data) { FrameGeneration::RenderEvent(eventId, data); }
}

BIGSCALE_FG_EXPORT void* BIGSCALE_FG_GetRenderEvent() { return reinterpret_cast<void*>(&FrameGenerationRenderEvent); }

BIGSCALE_FG_EXPORT int32_t BIGSCALE_FG_SetPresenterVisible(int32_t visible)
{
    return FrameGeneration::SetPresenterVisible(visible != 0) ? 1 : 0;
}

BIGSCALE_FG_EXPORT void BIGSCALE_FG_Shutdown() { FrameGeneration::Shutdown(); }

BIGSCALE_FG_EXPORT int32_t BIGSCALE_FG_RequestCapture(uint32_t pairs)
{
    return FgCaptureArm(pairs) ? 1 : 0;
}
