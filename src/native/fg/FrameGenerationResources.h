// Frame-generation capture resources.
//
// The game keeps rendering on D3D11. Each capture slot owns shared textures written by D3D11:
// output-sized RGBA8 real colour (and, for providers that extract the HUD, a second output-sized
// RGBA8 HUD-less colour), plus render-sized R32_FLOAT depth and R16G16_FLOAT motion. D3D12 opens the
// same allocations on the frame-generation device.
#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace bigscale
{
// One slot's GPU resources on both APIs. The D3D11 texture owns the allocation; the D3D12 resource
// is the same memory opened on the FG device.
struct FgSlotTextures
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> color;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> colorSrv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> colorUav;

    // Final scene colour immediately before the URP overlay UI. Output sized. Only allocated for
    // providers whose SDK consumes a HUD-less colour buffer.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> hudless;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> hudlessUav;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depthSrv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> depthUav;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> motion;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> motionSrv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> motionUav;

    // A game backbuffer usually has no shader-resource bind flag, so a same-format copy into this
    // staging texture is the only legal way to read it. Shared by the real and HUD-less colour
    // conversions on the same slot because both are output sized.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> colorStaging;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> colorStagingSrv;
    DXGI_FORMAT colorStagingFormat = DXGI_FORMAT_UNKNOWN;

    // The R32 depth fallback cannot copy straight into `depth`, because the transport flips rows and
    // a copy cannot. The active render region is staged here and dispatched through the flip kernel.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depthStaging;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depthStagingSrv;

    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;

    void Reset();
};

// Owns the conversion kernels shared by every capture slot.
class FrameGenerationResources
{
public:
    bool EnsureKernels(ID3D11Device* device, std::string& reason);

    // Creates or resizes the slot's textures. Real/HUD-less colour is output sized; depth and motion
    // are render sized. The source formats are validated at conversion time.
    bool EnsureSlot(ID3D11Device* device, FgSlotTextures& slot, uint32_t renderWidth, uint32_t renderHeight,
                    uint32_t outputWidth, uint32_t outputHeight, bool needsHudless, std::string& reason);

    // Copies the final colour source into the slot's shared real-colour texture. Same-format RGBA8
    // copies directly; BGRA8 is converted through the UNORM view so encoded SDR values are preserved.
    // The backbuffer already has presented rows, so no row conversion happens here.
    bool ConvertColor(ID3D11DeviceContext* context, FgSlotTextures& slot, ID3D11Texture2D* source,
                      uint32_t width, uint32_t height, std::string& reason);

    // Copies the pre-overlay colour source into the slot's shared HUD-less texture with the exact
    // same encoding path as the real colour. `graphRows` converts URP graph rows into presented rows
    // and encodes the linear-light graph colour into the backbuffer's display transfer function,
    // and is false for a backbuffer source that already has both. Fails when the slot has no
    // HUD-less plane.
    bool ConvertHudless(ID3D11DeviceContext* context, FgSlotTextures& slot, ID3D11Texture2D* source,
                        uint32_t width, uint32_t height, bool graphRows, std::string& reason);

    // Copies nonlinear device depth and raw UV motion into the slot's render-sized shared textures,
    // converting URP graph rows into presented-backbuffer rows and negating motion y, resampling a
    // half-resolution motion field. Real and HUD-less colour are handled separately.
    bool ConvertDepthMotion(ID3D11DeviceContext* context, FgSlotTextures& slot, ID3D11Texture2D* depth,
                            ID3D11Texture2D* motion, uint32_t width, uint32_t height, std::string& reason);

private:
    struct Kernel
    {
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
        Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
    };

    bool Ready() const
    {
        return convertColor_.shader && graphColor_.shader && graphDepth_.shader && graphMotion_.shader;
    }
    bool Dispatch(Kernel& kernel, ID3D11DeviceContext* context, ID3D11ShaderResourceView* srv,
                  ID3D11UnorderedAccessView* uav, const int32_t (&values)[4], uint32_t width, uint32_t height);
    bool DispatchMotionHalf(ID3D11DeviceContext* context, ID3D11ShaderResourceView* srv,
                            ID3D11UnorderedAccessView* uav, const float (&values)[8], uint32_t width,
                            uint32_t height);
    bool ConvertColorInto(ID3D11DeviceContext* context, FgSlotTextures& slot, ID3D11Texture2D* destination,
                          Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>& targetUav,
                          ID3D11Texture2D* source, uint32_t width, uint32_t height, bool graphRows,
                          const char* label, std::string& reason);

    Kernel convertColor_;
    Kernel graphColor_;
    Kernel graphDepth_;
    Kernel graphMotion_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> graphMotionHalfShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> motionHalfConstants_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearSampler_;
    bool kernelsReady_ = false;
};
}
