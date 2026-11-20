// D3D11 ↔ D3D12 bridge shared by the background-DX12 upscalers and the desktop presenter.
//
// The game keeps its D3D11 device and swapchain. This layer creates a D3D12 device on the exact
// same adapter, shares textures through legacy shared handles, and orders both APIs with shared
// fences, so an SDK can run on D3D12 while every resource it touches is still written by D3D11.
#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace bigscale
{
class Dx12Interop
{
public:
    // A shared texture opened on both devices. D3D11 owns the allocation.
    struct Shared
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d11;
        Microsoft::WRL::ComPtr<ID3D12Resource> d3d12;
        HANDLE handle = nullptr;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        uint32_t width = 0;
        uint32_t height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint32_t d3d11Subresource = 0;

        void Reset()
        {
            d3d12.Reset();
            d3d11.Reset();
            handle = nullptr;
            state = D3D12_RESOURCE_STATE_COMMON;
            width = 0;
            height = 0;
            format = DXGI_FORMAT_UNKNOWN;
        }
    };

    bool Ensure(ID3D11Device* device, std::string& reason);
    // Diagnostics only: prints the last validation errors when a debug layer is present.
    void LogValidationErrors(const char* context) const;
    bool Ready() const { return ready_; }
    uint32_t Generation() const { return generation_; }
    ID3D12Device* Device() const { return device_.Get(); }
    ID3D12CommandQueue* Queue() const { return queue_.Get(); }

    // Binds one D3D11 texture generation to a D3D12 resource. Recreates when size/format changes.
    bool Share(ID3D11Texture2D* texture, Shared& shared, std::string& reason);
    void Release(Shared& shared);

    // Orders the D3D11 writes before the next D3D12 submission and reports the exact fence value.
    // The matching Submit waits on that value, never on a concurrently changing global.
    bool SignalFromD3D11(ID3D11DeviceContext4* context, std::string& reason, uint64_t& signaledValue);

    // Prepares one submission ring slot. The caller records transitions, SDK work and the return
    // transition, then calls Submit.
    bool Begin(std::string& reason);
    ID3D12GraphicsCommandList* List() const { return current_ != nullptr ? current_->list.Get() : nullptr; }
    bool Submit(std::string& reason, uint64_t sourceFenceValue);
    // The fence and value that complete the submission just made, for bounded post-submit waits.
    ID3D12Fence* SubmissionFence() const { return current_ != nullptr ? current_->fence.Get() : nullptr; }
    uint64_t SubmissionValue() const { return current_ != nullptr ? current_->value : 0; }

    // The shared fence a self-executing provider must wait on before it reads D3D11-written data.
    ID3D12Fence* SourceFence() const { return d3d11FenceOnDx12_.Get(); }

    // Queues the D3D11 wait for the submission just made. Copy-back commands recorded after this
    // wait are therefore ordered behind the SDK's D3D12 work.
    bool WaitOnD3D11(ID3D11DeviceContext4* context, std::string& reason);

    // Waits for every submission this bridge made to complete. A caller that is about to destroy a
    // resource both APIs referenced must prove this first; the closed submission ring only proves
    // the reused slots, not work that was queued after the last recycle.
    bool WaitIdle(uint32_t timeoutMilliseconds, std::string& reason);

    // D3D12 state transitions around the shared resource.
    static void Transition(ID3D12GraphicsCommandList* list, Shared& shared, D3D12_RESOURCE_STATES state);
    // Same for a resource whose tracked state is owned by the caller.
    static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES& tracked, D3D12_RESOURCE_STATES state);

    void ReleaseAll();

private:
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        HANDLE event = nullptr;
        uint64_t value = 0;
        // Diagnostic invariant: true between a successful Reset and Close.
        bool open = false;
    };

    bool CreateDevice(ID3D11Device* device, std::string& reason);
    bool CreateSyncObjects(ID3D11Device* device, std::string& reason);

    Microsoft::WRL::ComPtr<IDXGIFactory4> factory_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Slot slots_[3];
    uint32_t nextSlot_ = 0;
    Slot* current_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Fence> d3d11Fence_;
    Microsoft::WRL::ComPtr<ID3D12Fence> d3d11FenceOnDx12_;
    uint64_t d3d11Value_ = 0;

    Microsoft::WRL::ComPtr<ID3D12Fence> d3d12Fence_;
    Microsoft::WRL::ComPtr<ID3D11Fence> d3d12FenceOnD3D11_;
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> validation_;
    int64_t validationSeen_ = 0;
    uint64_t d3d12Value_ = 0;

    uint32_t generation_ = 0;
    bool ready_ = false;
};
}
