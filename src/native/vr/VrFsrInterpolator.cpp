#include "VrFsrInterpolator.h"

#include "../Log.h"

#include <cstring>

namespace bigscale
{
VrFsrInterpolator::~VrFsrInterpolator()
{
    DestroyContexts();
}

bool VrFsrInterpolator::Load(const std::wstring& runtimeDirectory, std::string& reason)
{
    return runtime_.Load(runtimeDirectory, reason);
}

bool VrFsrInterpolator::Ensure(ID3D12Device* device, uint32_t eyeWidth, uint32_t eyeHeight, bool reversedDepth,
                               std::string& reason)
{
    if (device == nullptr)
    {
        reason = "frame generation needs a D3D12 device";
        return false;
    }
    if (!runtime_.Loaded())
    {
        reason = "the FidelityFX frame-generation runtime is not loaded";
        return false;
    }
    if (device_ == device && width_ == eyeWidth && height_ == eyeHeight && reversedDepth_ == reversedDepth)
        return true;
    if (device_ != device)
    {
        if (!runtime_.Select(device, reason)) return false;
        if (contexts_[0] != nullptr || contexts_[1] != nullptr)
            Log("VRFG: the D3D12 device changed; frame-generation contexts are recreated.");
    }
    DestroyContexts();
    device_ = device;
    width_ = eyeWidth;
    height_ = eyeHeight;
    reversedDepth_ = reversedDepth;
    frameId_ = 0;
    return true;
}

bool VrFsrInterpolator::EnsureContext(int32_t eye, std::string& reason)
{
    if (eye < 0 || eye > 1)
    {
        reason = "frame generation received an eye index outside 0..1";
        return false;
    }
    if (contexts_[eye] != nullptr) return true;
    std::string local;
    if (!runtime_.CreateContext(device_, width_, height_, reversedDepth_, &contexts_[eye], local))
    {
        // The right eye is created only after the left one succeeded, so this exact reason means the
        // runtime accepted one context but refused a second; the left eye must stop with it.
        reason = eye == 1
            ? "AMD FidelityFX frame generation could not create a second (right-eye) context"
            : "AMD FidelityFX frame generation is unavailable: " + local;
        Log("VRFG: eye %d context creation failed: %s", eye, local.c_str());
        return false;
    }
    Log("VRFG: frame-generation context ready for eye %d at %ux%u.", eye, width_, height_);
    return true;
}

bool VrFsrInterpolator::Record(const VrFgGenerate& generate, std::string& reason)
{
    if (generate.list == nullptr || generate.real == nullptr || generate.generated == nullptr ||
        generate.depth == nullptr || generate.motion == nullptr)
    {
        reason = "VR frame generation needs the list, both eye images and both guide textures";
        return false;
    }
    if (!EnsureContext(generate.eye, reason)) return false;
    const FsrFgRuntime::Api& api = runtime_.api();

    ffxConfigureDescFrameGeneration configure{};
    configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    configure.swapChain = nullptr;
    configure.frameGenerationEnabled = true;
    configure.allowAsyncWorkloads = false;
    configure.HUDLessColor = ffxApiGetResourceDX12(nullptr);
    configure.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    configure.generationRect = {0, 0, static_cast<int32_t>(width_), static_cast<int32_t>(height_)};
    configure.frameID = frameId_;
    if (api.configure(&contexts_[generate.eye], &configure.header) != FFX_API_RETURN_OK)
    {
        reason = "ffxConfigure failed for the VR frame-generation context";
        return false;
    }

    ffxDispatchDescFrameGenerationPrepareV2 prepare{};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prepare.frameID = frameId_;
    prepare.commandList = generate.list;
    prepare.renderSize = {generate.frame.renderWidth, generate.frame.renderHeight};
    prepare.jitterOffset = {generate.frame.jitterX, generate.frame.jitterY};
    // The super-resolution packet carries the same bitmap motion the guides hold, so the VR dispatch
    // uses the identical convention: UV displacement negated into current-to-previous pixels.
    prepare.motionVectorScale = {-static_cast<float>(generate.frame.renderWidth),
        -static_cast<float>(generate.frame.renderHeight)};
    prepare.frameTimeDelta = generate.frame.deltaMilliseconds;
    prepare.reset = generate.reset;
    prepare.cameraNear = generate.frame.cameraNear;
    prepare.cameraFar = generate.frame.cameraFar;
    prepare.cameraFovAngleVertical = generate.frame.verticalFovRadians;
    prepare.viewSpaceToMetersFactor = 1.0f;
    prepare.depth = ffxApiGetResourceDX12(generate.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    prepare.motionVectors = ffxApiGetResourceDX12(generate.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    std::memcpy(prepare.cameraPosition, generate.frame.cameraPosition, sizeof(prepare.cameraPosition));
    std::memcpy(prepare.cameraUp, generate.frame.cameraUp, sizeof(prepare.cameraUp));
    std::memcpy(prepare.cameraRight, generate.frame.cameraRight, sizeof(prepare.cameraRight));
    std::memcpy(prepare.cameraForward, generate.frame.cameraForward, sizeof(prepare.cameraForward));
    const ffxReturnCode_t prepared = api.dispatch(&contexts_[generate.eye], &prepare.header);
    if (prepared != FFX_API_RETURN_OK)
    {
        reason = "ffxDispatch failed for the VR frame-generation prepare";
        Log("VRFG: prepare dispatch failed eye=%d code=%u", generate.eye, static_cast<unsigned>(prepared));
        return false;
    }

    ffxDispatchDescFrameGeneration generateDesc{};
    generateDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    generateDesc.commandList = generate.list;
    generateDesc.presentColor = ffxApiGetResourceDX12(generate.real, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    generateDesc.outputs[0] = ffxApiGetResourceDX12(generate.generated, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    generateDesc.numGeneratedFrames = 1;
    generateDesc.reset = generate.reset;
    generateDesc.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    generateDesc.minMaxLuminance[0] = 0.0f;
    generateDesc.minMaxLuminance[1] = 0.0f;
    generateDesc.generationRect = configure.generationRect;
    generateDesc.frameID = frameId_;
    const ffxReturnCode_t generated = api.dispatch(&contexts_[generate.eye], &generateDesc.header);
    if (generated != FFX_API_RETURN_OK)
    {
        reason = "ffxDispatch failed for the VR frame-generation dispatch";
        Log("VRFG: generation dispatch failed eye=%d code=%u", generate.eye, static_cast<unsigned>(generated));
        return false;
    }
    return true;
}

void VrFsrInterpolator::DestroyContexts()
{
    for (int32_t eye = 0; eye < 2; eye++)
    {
        if (contexts_[eye] == nullptr) continue;
        if (!runtime_.DestroyContext(&contexts_[eye]))
            Log("VRFG: eye %d context destroy failed; it stays allocated.", eye);
    }
}

void VrFsrInterpolator::Release()
{
    DestroyContexts();
    runtime_.Unload();
    device_ = nullptr;
    width_ = 0;
    height_ = 0;
    reversedDepth_ = false;
    frameId_ = 0;
}
}
