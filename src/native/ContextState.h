// Isolated D3D11 pipeline state shared by the super-resolution bridge and frame generation.
//
// Unity's D3D11 command lists are recorded from a process-wide immediate context. Plugin work that
// binds its own shaders, views and buffers must not observe or disturb Unity's bound state, so it
// swaps in one clean ID3DDeviceContextState and restores Unity's on every exit path.
#pragma once

#include <d3d11_4.h>
#include <wrl/client.h>

namespace bigscale
{
class ContextState
{
public:
    // Creates the clean state for this device/context pair. Repeated calls with the same device are
    // a no-op; a different device replaces the cached state.
    bool Ensure(ID3D11Device* device, ID3D11DeviceContext* context);
    void Release();
    bool Ready() const { return context1_ != nullptr && state_ != nullptr; }
    ID3D11DeviceContext1* Context1() const { return context1_.Get(); }

    // Scoped swap. Valid() is false when no clean state exists; callers must treat that as a failure
    // instead of running plugin work against Unity's state.
    class Guard
    {
    public:
        explicit Guard(ContextState& owner);
        ~Guard();
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        bool Valid() const { return valid_; }

    private:
        ContextState& owner_;
        Microsoft::WRL::ComPtr<ID3DDeviceContextState> previous_;
        bool valid_ = false;
    };

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
};

// Saves the compute-stage slots a super-resolution packet can bind and restores them on scope exit.
// Graphics state, render targets and the context-state object are deliberately never touched: a
// SwapDeviceContextState on Unity's immediate context empties the next frame's depth copy through
// the VR mod's name-keyed resource repair, which turns the world into flat fog.
class ComputeSlotState
{
public:
    explicit ComputeSlotState(ID3D11DeviceContext* context);
    ~ComputeSlotState();
    ComputeSlotState(const ComputeSlotState&) = delete;
    ComputeSlotState& operator=(const ComputeSlotState&) = delete;

private:
    static constexpr UINT kBuffers = 2;
    static constexpr UINT kResources = 8;
    static constexpr UINT kTargets = 8;
    static constexpr UINT kSamplers = 2;

    void Restore();

    ID3D11DeviceContext* context_ = nullptr;
    ID3D11ComputeShader* shader_ = nullptr;
    ID3D11Buffer* constantBuffers_[kBuffers] = {};
    ID3D11ShaderResourceView* resources_[kResources] = {};
    ID3D11UnorderedAccessView* targets_[kTargets] = {};
    ID3D11SamplerState* samplers_[kSamplers] = {};
};
}
