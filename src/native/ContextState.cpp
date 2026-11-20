#include "ContextState.h"

#include "Log.h"

using Microsoft::WRL::ComPtr;

namespace bigscale
{
bool ContextState::Ensure(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (device == nullptr || context == nullptr) return false;
    if (Ready() && device_.Get() == device) return true;
    Release();
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))) || !device1 ||
        FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))) || !context1)
        return false;
    D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
    D3D_FEATURE_LEVEL selected{};
    ComPtr<ID3DDeviceContextState> state;
    const HRESULT result = device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
        __uuidof(ID3D11Device), &selected, &state);
    if (FAILED(result) || !state)
    {
        Log("context state creation failed result=0x%08x", static_cast<unsigned>(result));
        return false;
    }
    device_ = device;
    context1_ = context1;
    state_ = state;
    return true;
}

void ContextState::Release()
{
    // Swap Unity's state back in before the clean state is destroyed. The guard is scoped, so at
    // this point no plugin work is between the two swaps.
    context1_.Reset();
    state_.Reset();
    device_.Reset();
}

ContextState::Guard::Guard(ContextState& owner) : owner_(owner)
{
    if (!owner_.Ready()) return;
    // SwapDeviceContextState returns void; a removed device is the only observable failure and the
    // callers check their own device state immediately afterwards.
    owner_.context1_->SwapDeviceContextState(owner_.state_.Get(), &previous_);
    valid_ = true;
}

ContextState::Guard::~Guard()
{
    if (!valid_) return;
    if (owner_.context1_) owner_.context1_->SwapDeviceContextState(previous_.Get(), nullptr);
    previous_.Reset();
}

ComputeSlotState::ComputeSlotState(ID3D11DeviceContext* context) : context_(context)
{
    if (context_ == nullptr) return;
    context_->CSGetShader(&shader_, nullptr, nullptr);
    context_->CSGetConstantBuffers(0, kBuffers, constantBuffers_);
    context_->CSGetShaderResources(0, kResources, resources_);
    context_->CSGetUnorderedAccessViews(0, kTargets, targets_);
    context_->CSGetSamplers(0, kSamplers, samplers_);
}

ComputeSlotState::~ComputeSlotState()
{
    Restore();
}

void ComputeSlotState::Restore()
{
    if (context_ == nullptr) return;
    context_->CSSetShader(shader_, nullptr, 0);
    context_->CSSetConstantBuffers(0, kBuffers, constantBuffers_);
    context_->CSSetShaderResources(0, kResources, resources_);
    context_->CSSetUnorderedAccessViews(0, kTargets, targets_, nullptr);
    context_->CSSetSamplers(0, kSamplers, samplers_);
    for (ID3D11Buffer* buffer : constantBuffers_)
        if (buffer != nullptr) buffer->Release();
    for (ID3D11ShaderResourceView* view : resources_)
        if (view != nullptr) view->Release();
    for (ID3D11UnorderedAccessView* view : targets_)
        if (view != nullptr) view->Release();
    for (ID3D11SamplerState* sampler : samplers_)
        if (sampler != nullptr) sampler->Release();
    if (shader_ != nullptr) shader_->Release();
    context_ = nullptr;
}
}
