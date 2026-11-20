// Desktop frame-generation presenter, capture and provider worker.
//
// The game keeps its D3D11 swapchain. This module captures the URP depth/motion pair during the
// recorded render event, copies the final backbuffer at Present into a shared RGBA8 slot, and hands
// the immutable slot to one provider worker. The worker owns the D3D12 interop instance, the child
// swapchain and the SDK context; the render thread only copies, signs a fence and presents.
#pragma once

#include "FrameGenerationAbi.h"

#include <cstdint>
#include <string>

namespace bigscale
{
class FrameGeneration
{
public:
    // Installs the DXGI detours lazily. Safe to call repeatedly.
    static bool EnsureInstalled();
    static void Configure(const BIGSCALE_FG_Config& config);
    // Runtime/data directories and the engine version feed the provider initializers.
    static void SetDirectories(const std::wstring& runtimeDirectory, const std::wstring& dataDirectory);
    static void SetEngineVersion(const std::string& engineVersion);
    static void* GameWindow();
    static bool CreatePresenterWindow(void* parent);
    static void* Submit(const BIGSCALE_FG_Frame& frame);
    static void GetStatus(BIGSCALE_FG_Status* status);
    static void Ack(uint32_t throughSequence);
    static void RenderEvent(int32_t eventId, void* data);
    // Window work is only legal on the owning thread; returns false from any other thread.
    static bool SetPresenterVisible(bool visible);
    // XeLL phases. A frame whose marker set is not armed emits none of its phases.
    static void BeginFrame(uint64_t presentId);
    static void EndSimulation(uint64_t presentId);
    static void BeginRender(uint64_t presentId);
    static void Shutdown();
};
}
