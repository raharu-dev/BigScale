// XeLL latency tracker used by XeSS frame generation.
//
// The context is created on the render thread because that is where the six per-frame markers are
// emitted (begin frame, end simulation, begin render, present start/end). XeSS-FG receives the same
// context for its latency reduction. Only the XeSS provider uses this; every other provider leaves
// it destroyed.
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>

namespace bigscale
{
class XellLatency
{
public:
    bool Ensure(const std::wstring& runtimeDirectory, ID3D12Device* device, std::string& reason);
    // Destroys the context. Must run after the XeSS-FG context has been destroyed.
    void Release();
    bool Ready() const { return context_ != nullptr; }
    void* Handle() const { return context_; }

    void Sleep(uint32_t frameId);
    void Marker(uint32_t frameId, int32_t phase);

    const std::string& LastReason() const { return reason_; }

private:
    struct Api
    {
        int32_t (*create)(ID3D12Device* device, void** context) = nullptr;
        int32_t (*destroy)(void* context) = nullptr;
        int32_t (*setSleepMode)(void* context, const void* parameters) = nullptr;
        int32_t (*sleep)(void* context, uint32_t frameId) = nullptr;
        int32_t (*addMarker)(void* context, uint32_t frameId, int32_t marker) = nullptr;
    };

    void* context_ = nullptr;
    HMODULE module_ = nullptr;
    Api api_{};
    std::string reason_ = "not initialised";
};
}
