#include "Dx12Interop.h"

#include "Log.h"

#include <d3d11_1.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace bigscale
{
namespace
{

// Shared textures are created with the legacy MISC_SHARED flag: D3D12 can open that handle, while
// an NT handle would require a keyed mutex on the producer. Fences are NT-shared because they have
// explicit shared-fence APIs on both APIs.
constexpr D3D12_HEAP_PROPERTIES kDefaultHeap = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
    D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
}

bool Dx12Interop::Ensure(ID3D11Device* device, std::string& reason)
{
    if (ready_ && device != nullptr && device_.Get() != nullptr) return true;
    if (device == nullptr)
    {
        reason = "no D3D11 device to bridge from";
        return false;
    }
    if (!CreateDevice(device, reason)) return false;
    if (!CreateSyncObjects(device, reason)) return false;
    if (!queue_)
    {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        desc.NodeMask = 0;
        if (FAILED(device_->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue_))) || !queue_)
        {
            reason = "D3D12 command queue creation failed";
            return false;
        }
    }
    for (Slot& slot : slots_)
    {
        if (!slot.allocator && FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&slot.allocator))))
        {
            reason = "D3D12 command allocator creation failed";
            return false;
        }
        if (!slot.list && FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                slot.allocator.Get(), nullptr, IID_PPV_ARGS(&slot.list))))
        {
            reason = "D3D12 command list creation failed";
            return false;
        }
        if (slot.list) slot.list->Close();
        if (!slot.fence && FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.fence))))
        {
            reason = "D3D12 submission fence creation failed";
            return false;
        }
        if (slot.event == nullptr) slot.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    generation_++;
    ready_ = true;
    Log("DX12 interop device ready (generation=%u, adapter LUID preserved).", generation_);
    return true;
}

bool Dx12Interop::CreateDevice(ID3D11Device* device, std::string& reason)
{
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || !dxgiDevice)
    {
        reason = "the D3D11 device does not expose an IDXGIDevice";
        return false;
    }
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter)) || !adapter)
    {
        reason = "the D3D11 adapter could not be queried";
        return false;
    }
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))) || !factory)
    {
        reason = "the DXGI factory could not be queried";
        return false;
    }
    // Optional validation: enabled only for local diagnosis, never in play.
    const bool validationRequested = std::getenv("BIGSCALE_DX12_VALIDATION") != nullptr;
    if (validationRequested)
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) && debug) debug->EnableDebugLayer();
    }
    // Frame generation needs the full D3D12 feature set (DLSS-G requires shader model 6.6 on a
    // 12_1-capable device). Ask for the highest level the adapter supports and fall back gradually;
    // the super-resolution providers only need 11_0, so nothing regresses on older hardware.
    HRESULT result = E_FAIL;
    for (const D3D_FEATURE_LEVEL level : {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
             D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0})
    {
        result = D3D12CreateDevice(adapter.Get(), level, IID_PPV_ARGS(&device_));
        if (SUCCEEDED(result) && device_) break;
        device_.Reset();
    }
    if (FAILED(result) || !device_)
    {
        // Retry once with the debug layer, but only for the development flag: without it a debug-layer
        // failure must not be masked by a second creation attempt that reports a different reason.
        if (validationRequested)
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) && debug)
            {
                debug->EnableDebugLayer();
                for (const D3D_FEATURE_LEVEL level : {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
                         D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0})
                {
                    result = D3D12CreateDevice(adapter.Get(), level, IID_PPV_ARGS(&device_));
                    if (SUCCEEDED(result) && device_) break;
                    device_.Reset();
                }
            }
        }
    }
    if (FAILED(result) || !device_)
    {
        reason = "a D3D12 device could not be created on the game's adapter";
        return false;
    }
    if (SUCCEEDED(device_.As(&validation_)) && validation_)
    {
        validation_->SetMuteDebugOutput(FALSE);
    }
    Log("DX12 validation layer active=%d", validation_ != nullptr ? 1 : 0);
    factory_ = factory;
    Log("DX12 device created on the game's exact adapter.");
    return true;
}

bool Dx12Interop::CreateSyncObjects(ID3D11Device* device, std::string& reason)
{
    ComPtr<ID3D11Device5> device5;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5))) || !device5)
    {
        reason = "the game device is not an ID3D11Device5; shared fence synchronisation is unavailable";
        return false;
    }
    if (!d3d11Fence_)
    {
        if (FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&d3d11Fence_))) || !d3d11Fence_)
        {
            reason = "the D3D11 shared fence could not be created";
            return false;
        }
        HANDLE handle = nullptr;
        if (FAILED(d3d11Fence_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle)) || handle == nullptr)
        {
            reason = "the D3D11 fence handle could not be shared";
            return false;
        }
        const HRESULT result = device_->OpenSharedHandle(handle, IID_PPV_ARGS(&d3d11FenceOnDx12_));
        CloseHandle(handle);
        if (FAILED(result) || !d3d11FenceOnDx12_)
        {
            reason = "D3D12 could not open the D3D11 shared fence";
            return false;
        }
    }
    if (!d3d12Fence_)
    {
        if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&d3d12Fence_))) || !d3d12Fence_)
        {
            reason = "the D3D12 shared fence could not be created";
            return false;
        }
        HANDLE handle = nullptr;
        if (FAILED(device_->CreateSharedHandle(d3d12Fence_.Get(), nullptr, GENERIC_ALL, nullptr, &handle)) ||
            handle == nullptr)
        {
            reason = "the D3D12 fence handle could not be shared";
            return false;
        }
        const HRESULT result = device5->OpenSharedFence(handle, IID_PPV_ARGS(&d3d12FenceOnD3D11_));
        CloseHandle(handle);
        if (FAILED(result) || !d3d12FenceOnD3D11_)
        {
            reason = "D3D11 could not open the D3D12 shared fence";
            return false;
        }
    }
    Log("DX12 interop shared fences ready.");
    return true;
}

bool Dx12Interop::SignalFromD3D11(ID3D11DeviceContext4* context, std::string& reason, uint64_t& signaledValue)
{
    signaledValue = 0;
    if (context == nullptr || !d3d11Fence_)
    {
        reason = "no shared D3D11 fence is available";
        return false;
    }
    const uint64_t value = ++d3d11Value_;
    if (FAILED(context->Signal(d3d11Fence_.Get(), value)))
    {
        reason = "the D3D11 fence could not be signalled";
        return false;
    }
    context->Flush();
    signaledValue = value;
    return true;
}

bool Dx12Interop::WaitOnD3D11(ID3D11DeviceContext4* context, std::string& reason)
{
    if (context == nullptr || !d3d12FenceOnD3D11_)
    {
        reason = "no shared D3D12 fence is available";
        return false;
    }
    if (FAILED(context->Wait(d3d12FenceOnD3D11_.Get(), d3d12Value_)))
    {
        reason = "the D3D11 wait on the D3D12 fence failed";
        return false;
    }
    return true;
}

bool Dx12Interop::WaitIdle(uint32_t timeoutMilliseconds, std::string& reason)
{
    for (Slot& slot : slots_)
    {
        if (!slot.fence || slot.value == 0) continue;
        if (slot.fence->GetCompletedValue() >= slot.value) continue;
        if (slot.event == nullptr)
        {
            slot.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (slot.event == nullptr)
            {
                reason = "the D3D12 idle event could not be created";
                return false;
            }
        }
        if (FAILED(slot.fence->SetEventOnCompletion(slot.value, slot.event)))
        {
            reason = "the D3D12 idle fence could not be waited on";
            return false;
        }
        if (WaitForSingleObject(slot.event, timeoutMilliseconds) != WAIT_OBJECT_0)
        {
            reason = "the D3D12 submission ring did not drain";
            return false;
        }
    }
    return true;
}

bool Dx12Interop::Share(ID3D11Texture2D* texture, Shared& shared, std::string& reason)
{
    if (texture == nullptr)
    {
        reason = "no D3D11 texture to share";
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (shared.d3d11.Get() == texture && shared.d3d12 && shared.width == desc.Width &&
        shared.height == desc.Height && shared.format == desc.Format)
        return true;

    Release(shared);
    ComPtr<IDXGIResource> dxgiResource;
    if (FAILED(texture->QueryInterface(IID_PPV_ARGS(&dxgiResource))) || !dxgiResource)
    {
        reason = "the texture does not expose an IDXGIResource";
        return false;
    }
    HANDLE handle = nullptr;
    if (FAILED(dxgiResource->GetSharedHandle(&handle)) || handle == nullptr)
    {
        reason = "the texture has no shared handle (it was not created with MISC_SHARED)";
        return false;
    }
    ComPtr<ID3D12Resource> resource;
    if (FAILED(device_->OpenSharedHandle(handle, IID_PPV_ARGS(&resource))) || !resource)
    {
        reason = "D3D12 could not open the shared texture handle";
        return false;
    }
    shared.d3d11 = texture;
    shared.d3d12 = resource;
    shared.handle = handle;
    shared.state = D3D12_RESOURCE_STATE_COMMON;
    shared.width = desc.Width;
    shared.height = desc.Height;
    shared.format = desc.Format;
    shared.d3d11Subresource = 0;
    return true;
}

void Dx12Interop::Release(Shared& shared)
{
    shared.d3d12.Reset();
    shared.d3d11.Reset();
    shared.handle = nullptr;
    shared.state = D3D12_RESOURCE_STATE_COMMON;
    shared.width = 0;
    shared.height = 0;
    shared.format = DXGI_FORMAT_UNKNOWN;
}

void Dx12Interop::LogValidationErrors(const char* context) const
{
    if (validation_ == nullptr || context == nullptr) return;
    const UINT64 count = validation_->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; i++)
    {
        SIZE_T size = 0;
        if (FAILED(validation_->GetMessage(i, nullptr, &size)) || size == 0) continue;
        std::vector<char> buffer(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        if (FAILED(validation_->GetMessage(i, message, &size))) continue;
        if (message->Severity > D3D12_MESSAGE_SEVERITY_WARNING) continue;
        Log("DX12 validation [%s]: %s", context, message->pDescription);
    }
    validation_->ClearStoredMessages();
}

bool Dx12Interop::Begin(std::string& reason)
{
    if (!ready_)
    {
        reason = "the DX12 interop device is not ready";
        return false;
    }
    current_ = &slots_[nextSlot_];
    nextSlot_ = (nextSlot_ + 1) % 3;
    // Wait for this ring entry's previous submission, then hand out a fresh list. A provider may
    // close the list it recorded into, so a recycled list cannot be assumed to be in a known state.
    if (current_->fence && current_->value != 0 && current_->fence->GetCompletedValue() < current_->value)
    {
        if (current_->event == nullptr)
        {
            reason = "the D3D12 submission ring did not drain";
            return false;
        }
        current_->fence->SetEventOnCompletion(current_->value, current_->event);
        if (WaitForSingleObject(current_->event, 2000) != WAIT_OBJECT_0)
        {
            reason = "the D3D12 submission ring did not drain";
            return false;
        }
    }
    current_->list.Reset();
    current_->allocator.Reset();
    const HRESULT allocatorResult =
        device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&current_->allocator));
    const HRESULT listResult = SUCCEEDED(allocatorResult)
        ? device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, current_->allocator.Get(), nullptr,
              IID_PPV_ARGS(&current_->list))
        : allocatorResult;
    if (FAILED(allocatorResult) || FAILED(listResult))
    {
        const HRESULT removed = device_->GetDeviceRemovedReason();
        char detail[160]{};
        std::snprintf(detail, sizeof(detail),
            "the D3D12 command list could not be created (allocator 0x%08x, list 0x%08x, removed 0x%08x)",
            static_cast<unsigned>(allocatorResult), static_cast<unsigned>(listResult),
            static_cast<unsigned>(removed));
        reason = detail;
        return false;
    }
    current_->open = true;
    return true;
}

bool Dx12Interop::Submit(std::string& reason, uint64_t sourceFenceValue)
{
    if (current_ == nullptr) return false;
    current_->open = false;
    const HRESULT closed = current_->list->Close();
    if (FAILED(closed))
    {
        // Never execute a list whose close failed: an open or invalid list is an invalid call that
        // can remove the device. The slot is recreated by the next Begin.
        const HRESULT removed = device_ != nullptr ? device_->GetDeviceRemovedReason() : E_FAIL;
        Log("DX12 interop list close failed hr=0x%08x removed=0x%08x", static_cast<unsigned>(closed),
            static_cast<unsigned>(removed));
        LogValidationErrors("command list close");
        reason = "the D3D12 command list could not be closed";
        return false;
    }
    // Order this submission behind the exact D3D11 copies signalled for this packet.
    if (d3d11FenceOnDx12_ && sourceFenceValue > 0 &&
        FAILED(queue_->Wait(d3d11FenceOnDx12_.Get(), sourceFenceValue)))
    {
        reason = "the D3D12 queue could not wait on the D3D11 fence";
        return false;
    }
    ID3D12CommandList* lists[1] = {current_->list.Get()};
    queue_->ExecuteCommandLists(1, lists);
    current_->value++;
    if (FAILED(queue_->Signal(current_->fence.Get(), current_->value)))
    {
        reason = "the D3D12 submission fence could not be signalled";
        return false;
    }
    if (d3d12Fence_)
    {
        d3d12Value_++;
        if (FAILED(queue_->Signal(d3d12Fence_.Get(), d3d12Value_)))
        {
            reason = "the shared D3D12 fence could not be signalled";
            return false;
        }
    }
    return true;
}

void Dx12Interop::Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES& tracked, D3D12_RESOURCE_STATES state)
{
    if (list == nullptr || resource == nullptr || tracked == state) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = tracked;
    barrier.Transition.StateAfter = state;
    list->ResourceBarrier(1, &barrier);
    tracked = state;
}

void Dx12Interop::Transition(ID3D12GraphicsCommandList* list, Shared& shared, D3D12_RESOURCE_STATES state)
{
    if (list == nullptr || !shared.d3d12 || shared.state == state) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = shared.d3d12.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = shared.state;
    barrier.Transition.StateAfter = state;
    list->ResourceBarrier(1, &barrier);
    shared.state = state;
}

void Dx12Interop::ReleaseAll()
{
    for (Slot& slot : slots_)
    {
        if (slot.event != nullptr)
        {
            CloseHandle(slot.event);
            slot.event = nullptr;
        }
        slot.value = 0;
        slot.fence.Reset();
        slot.list.Reset();
        slot.allocator.Reset();
    }
    d3d11FenceOnDx12_.Reset();
    d3d12FenceOnD3D11_.Reset();
    d3d11Fence_.Reset();
    d3d12Fence_.Reset();
    queue_.Reset();
    device_.Reset();
    factory_.Reset();
    ready_ = false;
    current_ = nullptr;
}
}
