// BigScale — VR frame-generation ABI (separate from the SR bridge and desktop FG ABIs).
//
// VR frame generation lives entirely inside the OpenVR compositor cycle: the managed side only says
// whether it is wanted and reads back a status snapshot. No pointer crosses this boundary, so every
// struct is blittable Pack=8 with fixed-width integers, floats and a bounded reason buffer.
#pragma once

#include <cstdint>

#if defined(_WIN32)
#define BIGSCALE_VRFG_EXPORT extern "C" __declspec(dllexport)
#else
#define BIGSCALE_VRFG_EXPORT extern "C"
#endif

#define BIGSCALE_VRFG_ABI_VERSION 1u
#define BIGSCALE_VRFG_REASON_CAPACITY 256

enum BIGSCALE_VRFG_State : int32_t
{
    BIGSCALE_VRFG_STATE_OFF = 0,
    BIGSCALE_VRFG_STATE_WAITING = 1,
    BIGSCALE_VRFG_STATE_ACTIVE = 2,
    BIGSCALE_VRFG_STATE_UNSUPPORTED = 3,
    BIGSCALE_VRFG_STATE_FAULTED = 4,
};

struct BIGSCALE_VRFG_Status
{
    uint32_t abi;
    uint32_t size;
    int32_t state;
    int32_t lastResult;
    float displayHz;
    uint32_t motionSmoothing;
    uint64_t realFrames;
    uint64_t generatedFrames;
    uint64_t repeatedSlots;
    uint64_t droppedFrames;
    uint64_t pacingTimeouts;
    char reason[BIGSCALE_VRFG_REASON_CAPACITY];
};

BIGSCALE_VRFG_EXPORT uint32_t BIGSCALE_VRFG_GetAbiVersion();
BIGSCALE_VRFG_EXPORT uint32_t BIGSCALE_VRFG_GetStatusSize();
// Writes the offset of each status field in the order the managed mirror declares them.
BIGSCALE_VRFG_EXPORT uint32_t BIGSCALE_VRFG_GetStatusLayout(uint32_t* offsets, uint32_t count);
BIGSCALE_VRFG_EXPORT int32_t BIGSCALE_VRFG_Configure(int32_t enabled);
BIGSCALE_VRFG_EXPORT int32_t BIGSCALE_VRFG_GetStatus(BIGSCALE_VRFG_Status* status);
BIGSCALE_VRFG_EXPORT void BIGSCALE_VRFG_Shutdown();

#if defined(BIGSCALE_TESTING)
// Native-check-only seam: the offline harness supplies its own interface objects, so the module
// never has to find a live SteamVR session.
BIGSCALE_VRFG_EXPORT void BIGSCALE_VRFG_TestUseInterfaces(void* compositor, void* system, void* settings);
#endif
