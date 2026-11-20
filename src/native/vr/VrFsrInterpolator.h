// One AMD FidelityFX frame-generation context per eye, driven from the VR compositor path.
//
// The desktop provider presents through a child swapchain; VR has no swapchain at all. SteamVR owns
// presentation, so this class only owns the interpolation: two independent contexts (one per eye),
// each fed that eye's guide textures and real image, each writing one interpolated eye image that
// the compositor is handed with an interpolated pose.
#pragma once

#include "../fg/FsrFgRuntime.h"

#include <d3d12.h>

#include <cstdint>
#include <string>

namespace bigscale
{
// The guide-derived inputs of one real frame, identical for both eyes except for the camera basis.
struct VrFgFrame
{
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float deltaMilliseconds = 0.0f;
    float cameraNear = 0.1f;
    float cameraFar = 1000.0f;
    float verticalFovRadians = 1.0f;
    float cameraPosition[3] = {0.0f, 0.0f, 0.0f};
    float cameraUp[3] = {0.0f, 1.0f, 0.0f};
    float cameraRight[3] = {1.0f, 0.0f, 0.0f};
    float cameraForward[3] = {0.0f, 0.0f, -1.0f};
};

// One eye's dispatch. Every resource is already in the state the descriptor declares.
struct VrFgGenerate
{
    int32_t eye = 0;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* real = nullptr;
    ID3D12Resource* generated = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    VrFgFrame frame{};
    bool reset = false;
};

class VrFsrInterpolator
{
public:
    ~VrFsrInterpolator();

    bool Load(const std::wstring& runtimeDirectory, std::string& reason);
    uint32_t Version() const { return runtime_.Version(); }

    // Pins the eye extent and depth convention, recreating both contexts when either changes.
    bool Ensure(ID3D12Device* device, uint32_t eyeWidth, uint32_t eyeHeight, bool reversedDepth,
                std::string& reason);

    // Dispatches one eye's prepare and generate into the caller's list. The eye's context is created
    // on first use, so a runtime that refuses a second context fails only the second eye.
    bool Record(const VrFgGenerate& generate, std::string& reason);

    // True once this eye's context exists; a skipped pair only resets a provider that has history.
    bool Ready(int32_t eye) const { return eye >= 0 && eye < 2 && contexts_[eye] != nullptr; }

    void NextFrame() { frameId_++; }

    // Destroys both contexts. The caller proves the GPU idle first.
    void Release();

private:
    bool EnsureContext(int32_t eye, std::string& reason);
    void DestroyContexts();

    FsrFgRuntime runtime_;
    ID3D12Device* device_ = nullptr;
    void* contexts_[2] = {nullptr, nullptr};
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool reversedDepth_ = false;
    uint64_t frameId_ = 0;
};
}
