// BigScale — per-eye scratch textures and the GPU copies that move a logical view's
// colour, depth and motion out of a packed/array Unity target and back into its own slice.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

#include "NgxBackend.h"

namespace bigscale
{
// True when the DXGI format stores HDR (linear, >1.0 capable) colour.
bool IsHdrColorFormat(DXGI_FORMAT format);

// Owns one scratch texture set per (camera, eye) plus the shared conversion kernels.
class EyeResources
{
public:
    bool EnsureKernels(ID3D11Device* device);
    // Binds (creating or resizing as needed) the scratch set for this view. `source*` are the
    // Unity textures currently holding that view; nullptr is legal for probe operations.
    bool Prepare(ID3D11Device* device, const ViewKey& key, const EvaluationRequest& request,
                 ID3D11Texture2D* sourceColor, ID3D11Texture2D* sourceDepth, ID3D11Texture2D* sourceMotion);
    // Copies this view's slice of each engine source into private single-slice mirrors. Every later
    // read goes through the mirrors, so no view is ever created on, and no pipeline slot ever holds,
    // an engine texture. Must run after Prepare and before WriteSourceBaseline/Extract.
    bool MirrorSources(ID3D11DeviceContext* context, const ViewKey& key, const EvaluationRequest& request);
    // Copies the view's colour, depth and motion into the scratch set.
    bool Extract(ID3D11DeviceContext* context, const ViewKey& key, const EvaluationRequest& request);
    // Bilinear upsample of the extracted colour into the view's NGX output texture.
    bool SpatialUpsample(ID3D11DeviceContext* context, const ViewKey& key, const EvaluationRequest& request);
    bool CopyBack(ID3D11DeviceContext* context, const ViewKey& key, const EvaluationRequest& request,
                  ID3D11Texture2D* outputTexture);
    // Spatial baseline written straight from the view's own colour region, before any extraction.
    // It therefore covers allocation, conversion and state-isolation failures too.
    bool WriteSourceBaseline(ID3D11DeviceContext* context, const ViewKey& key, const EvaluationRequest& request,
                             ID3D11Texture2D* outputTexture);
    ID3D11Resource* ColorInput(const ViewKey& key);
    ID3D11Resource* DepthInput(const ViewKey& key);
    ID3D11Resource* MotionInput(const ViewKey& key);
    ID3D11Resource* OutputTarget(const ViewKey& key);
    // Drops one view's scratch. The caller guarantees the GPU has finished with it.
    void Forget(const ViewKey& key);
    // Drops every scratch texture. The caller guarantees the GPU has finished with them.
    void Release();
    const std::string& LastReason() const { return reason_; }

private:
    // A private single-slice copy of one engine source, created once per format and extent.
    struct Mirror
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        UINT width = 0;
        UINT height = 0;
        // Format the mirror was sized for. Any change recreates it.
        DXGI_FORMAT source = DXGI_FORMAT_UNKNOWN;
    };

    struct Set
    {
        ViewKey key{};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> color;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> depth;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> motion;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> output;
        Mirror colorMirror;
        Mirror depthMirror;
        Mirror motionMirror;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> colorSrv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depthSrv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> motionSrv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> colorUav;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> depthUav;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> motionUav;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> outputUav;
        // Source formats the current scratch set was built for. Any change recreates the set so a
        // format-switching camera can never read through a stale view.
        DXGI_FORMAT colorSource = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT depthSource = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT motionSource = DXGI_FORMAT_UNKNOWN;
        uint32_t scratchWidth = 0;
        uint32_t scratchHeight = 0;
        uint32_t inputWidth = 0;
        uint32_t inputHeight = 0;
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
        uint32_t lastFrame = 0;
    };

    struct Kernel
    {
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
        Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
    };

    Set* Find(const ViewKey& key);
    Set* Ensure(Set* set, const EvaluationRequest& request, DXGI_FORMAT colorSource,
                DXGI_FORMAT depthSource, DXGI_FORMAT motionSource);
    bool CreateScratch(ID3D11Device* device, Set& set, const EvaluationRequest& request,
                       DXGI_FORMAT colorSource, DXGI_FORMAT depthSource, DXGI_FORMAT motionSource);
    bool EnsureMirror(ID3D11Device* device, Set& set, Mirror& mirror, const D3D11_TEXTURE2D_DESC& source,
                      DXGI_FORMAT readFormat, const char* what);
    bool Reset(Set& set);
    bool Dispatch(Microsoft::WRL::ComPtr<ID3D11ComputeShader>& shader,
                  Microsoft::WRL::ComPtr<ID3D11Buffer>& constants, ID3D11DeviceContext* context,
                  ID3D11ShaderResourceView* srv, ID3D11UnorderedAccessView* uav, const int32_t (&values)[4],
                  uint32_t width, uint32_t height);
    bool DispatchScale(ID3D11DeviceContext* context, ID3D11ShaderResourceView* srv,
                       ID3D11UnorderedAccessView* uav, const float (&values)[8], uint32_t width, uint32_t height);
    // Bilinear region resample used when the motion field is smaller than the active input.
    bool DispatchResample(ID3D11DeviceContext* context, ID3D11ShaderResourceView* srv,
                          ID3D11UnorderedAccessView* uav, const float (&values)[8], uint32_t width, uint32_t height);
    // Dispatches the spatial upsample from one shader-readable region into the view's output scratch.
    bool UpsampleRegion(ID3D11DeviceContext* context, Set& set, ID3D11ShaderResourceView* srv,
                        uint32_t sourceX, uint32_t sourceY, uint32_t sourceWidth, uint32_t sourceHeight,
                        uint32_t inputWidth, uint32_t inputHeight);

    std::vector<Set*> sets_;
    Kernel convertColor_;
    Kernel convertDepth_;
    Kernel convertMotion_;
    Kernel upsample_;
    Kernel resampleMotion_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearSampler_;
    bool kernelsReady_ = false;
    std::string reason_ = "not initialised";
};
}
