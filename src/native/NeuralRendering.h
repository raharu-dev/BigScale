// BigScale — DLSS5 Neural Rendering stage.
//
// One opt-in stage that runs NGX feature 18 (neural rendering) on the converted per-eye scratch
// textures. It can execute before the selected super-resolution backend (returning the neural image
// as the backend's colour input) or after it (writing the composited image into the backend output).
//
// The stage is best-effort by construction: a missing runtime, an unsupported GPU, a failed feature
// creation or a layout it cannot mask reports a neural state and returns no substitution, so the
// selected upscaler and frame generation keep producing the frame. It never latches the bridge's
// super-resolution fault.
#pragma once

#include "Bridge.h"
#include "DlssNrRuntime.h"
#include "Dx12Interop.h"
#include "Upscaler.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bigscale
{
class NeuralRenderingStage
{
public:
    // Reference neural rendering runs three sequential layers; this stage hard-caps at five and
    // never creates, configures or accepts a sixth.
    static constexpr int32_t kMaxPasses = 5;

    NeuralRenderingStage();
    ~NeuralRenderingStage();
    NeuralRenderingStage(const NeuralRenderingStage&) = delete;
    NeuralRenderingStage& operator=(const NeuralRenderingStage&) = delete;

    // Called once from BIGSCALE_Initialize, before any packet can run.
    void SetEnvironment(const std::wstring& runtimeDirectory, const std::wstring& dataDirectory,
                        const std::string& engineVersion);
    // Called from BIGSCALE_ConfigureNeuralRendering. Copies the block, bumps the generation when any
    // field changes, and never blocks on GPU work.
    void Configure(const BIGSCALE_NeuralConfig& config);
    int32_t State() const { return state_.load(); }

    // Returns an RGBA16F D3D11 texture the super-resolution backend must use as its colour input, or
    // nullptr when the stage did not run, did not create this frame, or does not own this timing.
    ID3D11Resource* BeforeUpscale(ID3D11Device*, ID3D11DeviceContext*, const ViewKey&,
                                  const EvaluationRequest&, void* mask, uint32_t maskWidth, uint32_t maskHeight);
    // Runs after the super-resolution backend and writes the final image into request.output.
    bool AfterUpscale(ID3D11Device*, ID3D11DeviceContext*, const ViewKey&, const EvaluationRequest&,
                      void* mask, uint32_t maskWidth, uint32_t maskHeight);

    // The packet layout cannot run neural rendering (VR, single-pass stereo, packed views). The
    // stage reports Unsupported and the super-resolution and frame-generation paths are untouched.
    void MarkUnsupported();
    // Releases every feature, texture, parameter map and the runtime. The caller proved GPU idle.
    void Release();

    const std::string& LastReason() const { return lastReason_; }

private:
    struct Geometry;
    struct Kernel;
    struct Pass;
    struct Snapshot;
    struct View;

    Snapshot Capture() const;
    ID3D11Resource* RunFrame(ID3D11Device* device, ID3D11DeviceContext* context, const ViewKey& key,
                             const EvaluationRequest& request, void* mask, uint32_t maskWidth, uint32_t maskHeight,
                             const Snapshot& snapshot, bool beforeUpscaling);
    bool EnsureRuntime(ID3D11Device* device, ID3D11DeviceContext* context, std::string& reason);
    bool EnsureKernels(ID3D11Device* device, std::string& reason);
    bool EnsureBlackMask(ID3D11Device* device, std::string& reason);
    bool Context4(ID3D11DeviceContext* context, std::string& reason);
    View* AcquireView(const ViewKey& key, std::string& reason);
    bool NeedsRebuild(const View& view, const Snapshot& snapshot, const Geometry& geometry) const;
    bool EnsureTextures(ID3D11Device* device, View& view, const Geometry& geometry, std::string& reason);
    bool EnsureMask(ID3D11Device* device, View& view, uint32_t width, uint32_t height, std::string& reason);
    bool CopyInputs(ID3D11DeviceContext* context, View& view, const EvaluationRequest& request,
                    ID3D11Texture2D* colourSource, const Geometry& geometry, ID3D11Texture2D* maskTexture,
                    bool useMask, std::string& reason);
    bool CreateFeatureFrame(ID3D11DeviceContext* context, View& view, const Snapshot& snapshot,
                            const Geometry& geometry, std::string& reason);
    bool EvaluateFrame(ID3D11DeviceContext* context, View& view, const EvaluationRequest& request,
                       const Snapshot& snapshot, const Geometry& geometry, std::string& reason);
    bool Composite(ID3D11DeviceContext* context, View& view, const Geometry& geometry, int32_t finalOutput,
                   bool hasMask, std::string& reason);
    void ReleaseFeatures(View& view);
    void ReleaseTextures(View& view);
    void Fail(const std::string& reason);
    void NoteOnce(const std::string& message);
    void SetState(int32_t state);

    mutable std::mutex configMutex_;
    BIGSCALE_NeuralConfig config_{};
    uint32_t generation_ = 0;

    std::mutex frameMutex_;
    Dx12Interop interop_;
    DlssNrRuntime runtime_;
    std::wstring runtimeDirectory_;
    std::wstring dataDirectory_;
    std::string engineVersion_;

    std::vector<View*> views_;
    uint32_t useCounter_ = 0;

    std::unique_ptr<Kernel> kernel_;
    ID3D11Device* kernelDevice_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> blackMask_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> blackMaskSrv_;
    ID3D11Device* blackMaskDevice_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4_;
    ID3D11DeviceContext* context4Source_ = nullptr;

    // A failed runtime initialisation is retried only after this many frames or a configuration
    // change; it must never call NGX or log once per packet.
    uint32_t runtimeFailedGeneration_ = 0xffffffffu;
    int32_t runtimeRetryFrame_ = 0;

    std::atomic<int32_t> state_{BIGSCALE_NEURAL_OFF};
    std::string lastFailure_;
    std::string lastReason_;
};
}
