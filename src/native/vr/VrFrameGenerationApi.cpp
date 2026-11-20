// Exported VR frame-generation ABI surface. Every descriptor is validated field-by-field by the
// managed mirror before it is trusted.
#include "../VrFrameGenerationAbi.h"

#include "VrFrameGeneration.h"

#include <cstddef>

using namespace bigscale;

static_assert(sizeof(BIGSCALE_VRFG_Status) == 320, "BIGSCALE_VRFG_Status size");

BIGSCALE_VRFG_EXPORT uint32_t BIGSCALE_VRFG_GetAbiVersion() { return BIGSCALE_VRFG_ABI_VERSION; }

BIGSCALE_VRFG_EXPORT uint32_t BIGSCALE_VRFG_GetStatusSize() { return sizeof(BIGSCALE_VRFG_Status); }

BIGSCALE_VRFG_EXPORT uint32_t BIGSCALE_VRFG_GetStatusLayout(uint32_t* offsets, uint32_t count)
{
    const uint32_t values[] = {
        static_cast<uint32_t>(offsetof(BIGSCALE_VRFG_Status, abi)),
        static_cast<uint32_t>(offsetof(BIGSCALE_VRFG_Status, size)),
        static_cast<uint32_t>(offsetof(BIGSCALE_VRFG_Status, state)),
        static_cast<uint32_t>(offsetof(BIGSCALE_VRFG_Status, lastResult)),
        static_cast<uint32_t>(offsetof(BIGSCALE_VRFG_Status, displayHz)),
        static_cast<uint32_t>(offsetof(BIGSCALE_VRFG_Status, motionSmoothing)),
        static_cast<uint32_t>(offsetof(BIGSCALE_VRFG_Status, realFrames)),
        static_cast<uint32_t>(offsetof(BIGSCALE_VRFG_Status, generatedFrames)),
        static_cast<uint32_t>(offsetof(BIGSCALE_VRFG_Status, repeatedSlots)),
        static_cast<uint32_t>(offsetof(BIGSCALE_VRFG_Status, droppedFrames)),
        static_cast<uint32_t>(offsetof(BIGSCALE_VRFG_Status, pacingTimeouts)),
        static_cast<uint32_t>(offsetof(BIGSCALE_VRFG_Status, reason)),
    };
    if (offsets != nullptr)
    {
        constexpr uint32_t total = sizeof(values) / sizeof(values[0]);
        for (uint32_t i = 0; i < count && i < total; i++) offsets[i] = values[i];
    }
    return static_cast<uint32_t>(sizeof(values) / sizeof(values[0]));
}

BIGSCALE_VRFG_EXPORT int32_t BIGSCALE_VRFG_Configure(int32_t enabled)
{
    VrFgConfigure(enabled != 0);
    return 0;
}

BIGSCALE_VRFG_EXPORT int32_t BIGSCALE_VRFG_GetStatus(BIGSCALE_VRFG_Status* status)
{
    if (status == nullptr) return -1;
    if (status->size != sizeof(BIGSCALE_VRFG_Status)) return -2;
    VrFgFillStatus(*status);
    return 0;
}

BIGSCALE_VRFG_EXPORT void BIGSCALE_VRFG_Shutdown() { VrFgShutdown(); }

#if defined(BIGSCALE_TESTING)
BIGSCALE_VRFG_EXPORT void BIGSCALE_VRFG_TestUseInterfaces(void* compositor, void* system, void* settings)
{
    VrFgTestUseInterfaces(compositor, system, settings);
}
#endif
