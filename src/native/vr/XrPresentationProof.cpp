#include "XrPresentationProof.h"

#include "../Log.h"

#include <openvr.h>

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace bigscale
{
namespace XrPresentationProof
{
namespace
{
// The crop is the same 256-pixel box the hashed stage samples use, centred on the identity bar so the
// bar's orientation is decidable from the image alone.
constexpr uint32_t kCrop = 256;
constexpr uint32_t kMarkerCenterPercent = 50;
// Bounded evidence: eight tagged pairs and eight untagged pairs, one every fourth frame.
constexpr uint32_t kMaxTagged = 8;
constexpr uint32_t kMaxUntagged = 8;
constexpr uint32_t kEveryFrames = 4;

struct EyeCapture
{
    ComPtr<ID3D11Texture2D> staging;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bytesPerRow = 0;
};

struct State
{
    bool resolved = false;
    bool unavailable = false;
    bool taggedSeen = false;
    bool finished = false;
    bool settleOpen = true;
    int32_t cameraId = 0;
    uint32_t tagged = 0;
    uint32_t untagged = 0;
    int32_t lastCaptureFrame = -1000;
    vr::IVRCompositor* compositor = nullptr;
    EyeCapture eyes[2];
};

State g_state;

// A mirror view can expose the typeless member of a family; the staging copy needs the concrete one.
DXGI_FORMAT ConcreteFormat(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
        default: return format;
    }
}

uint32_t BytesPerPixel(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return 4;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return 8;
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return 4;
        default:
            return 0;
    }
}

// Resolves the compositor through the already loaded runtime module, exactly like the frame-generation
// path does, without initialising VR, hooking Submit or submitting anything.
bool Resolve()
{
    if (g_state.resolved) return !g_state.unavailable;
    g_state.resolved = true;
    HMODULE module = GetModuleHandleW(L"openvr_api.dll");
    if (module == nullptr)
    {
        g_state.unavailable = true;
        Log("XR proof: openvr_api.dll is not loaded; the compositor image cannot be read.");
        return false;
    }
    using GetInterface = void* (__cdecl*)(const char*, vr::EVRInitError*);
    auto getInterface = reinterpret_cast<GetInterface>(GetProcAddress(module, "VR_GetGenericInterface"));
    if (getInterface == nullptr)
    {
        g_state.unavailable = true;
        Log("XR proof: VR_GetGenericInterface is missing; the compositor image cannot be read.");
        return false;
    }
    vr::EVRInitError error = vr::VRInitError_None;
    void* compositor = getInterface(vr::IVRCompositor_Version, &error);
    if (compositor == nullptr)
    {
        g_state.unavailable = true;
        Log("XR proof: SteamVR did not hand out the compositor (%d); the compositor image cannot be read.",
            static_cast<int>(error));
        return false;
    }
    g_state.compositor = static_cast<vr::IVRCompositor*>(compositor);
    return true;
}

void WriteAll(HANDLE handle, const void* data, size_t size)
{
    const uint8_t* cursor = static_cast<const uint8_t*>(data);
    while (size > 0)
    {
        DWORD written = 0;
        if (!WriteFile(handle, cursor, static_cast<DWORD>(size), &written, nullptr) || written == 0) return;
        cursor += written;
        size -= written;
    }
}

// Top-down 24-bit BMP. The rows arrive top-down from the staging copy, which is what a negative
// biHeight declares.
bool WriteBitmap(const std::wstring& path, const std::vector<uint8_t>& bgr, uint32_t width, uint32_t height)
{
    const uint32_t rowBytes = width * 3;
    const uint32_t stride = (rowBytes + 3) & ~3u;
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(BITMAPINFOHEADER);
    info.biWidth = static_cast<LONG>(width);
    info.biHeight = -static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 24;
    info.biCompression = BI_RGB;
    info.biSizeImage = stride * height;
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + info.biSizeImage;
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        Log("XR proof: cannot create %ls (error %lu)", path.c_str(), GetLastError());
        return false;
    }
    WriteAll(handle, &file, sizeof(file));
    WriteAll(handle, &info, sizeof(info));
    static const uint8_t padding[4] = {0, 0, 0, 0};
    for (uint32_t y = 0; y < height; y++)
    {
        WriteAll(handle, &bgr[static_cast<size_t>(y) * rowBytes], rowBytes);
        WriteAll(handle, padding, stride - rowBytes);
    }
    CloseHandle(handle);
    return true;
}

// Converts one mapped staging row into 24-bit BGR. Only the four formats a SteamVR mirror realistically
// uses are accepted; anything else is reported instead of being written as a wrong-coloured image.
bool ConvertRow(const uint8_t* source, DXGI_FORMAT format, uint32_t width, uint8_t* destination)
{
    for (uint32_t x = 0; x < width; x++)
    {
        switch (format)
        {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                destination[x * 3 + 0] = source[x * 4 + 2];
                destination[x * 3 + 1] = source[x * 4 + 1];
                destination[x * 3 + 2] = source[x * 4 + 0];
                break;
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                destination[x * 3 + 0] = source[x * 4 + 0];
                destination[x * 3 + 1] = source[x * 4 + 1];
                destination[x * 3 + 2] = source[x * 4 + 2];
                break;
            case DXGI_FORMAT_R11G11B10_FLOAT:
            {
                // 5/6/5-bit shared-exponent channels; SteamVR hands its mirror out in this format on this
                // runtime, which is why the first proof attempt could not read it.
                const uint32_t bits = *reinterpret_cast<const uint32_t*>(source + x * 4);
                const auto decode = [](uint32_t mantissa, uint32_t exponent, uint32_t bitsCount)
                {
                    if (exponent == 0) return mantissa == 0 ? 0.0f : std::ldexp(static_cast<float>(mantissa), -24);
                    if (exponent == 31) return 1.0f;
                    return std::ldexp(static_cast<float>((1u << bitsCount) | mantissa), static_cast<int>(exponent) - 25);
                };
                const float r = decode(bits & 0x3F, (bits >> 6) & 0x1F, 6);
                const float g = decode((bits >> 11) & 0x3F, (bits >> 17) & 0x1F, 6);
                const float b = decode((bits >> 22) & 0x1F, (bits >> 27) & 0x1F, 5);
                const auto toByte = [](float linear)
                {
                    const float encoded = std::pow(std::min(std::max(linear, 0.0f), 1.0f), 1.0f / 2.2f);
                    return static_cast<uint8_t>(encoded * 255.0f + 0.5f);
                };
                destination[x * 3 + 0] = toByte(b);
                destination[x * 3 + 1] = toByte(g);
                destination[x * 3 + 2] = toByte(r);
                break;
            }
            case DXGI_FORMAT_R10G10B10A2_UNORM:
            {
                const uint32_t bits = *reinterpret_cast<const uint32_t*>(source + x * 4);
                const auto toByte = [](uint32_t value)
                {
                    const float linear = static_cast<float>(value) / 1023.0f;
                    const float encoded = std::pow(linear, 1.0f / 2.2f);
                    return static_cast<uint8_t>(encoded * 255.0f + 0.5f);
                };
                destination[x * 3 + 0] = toByte(bits & 0x3FF);
                destination[x * 3 + 1] = toByte((bits >> 10) & 0x3FF);
                destination[x * 3 + 2] = toByte((bits >> 20) & 0x3FF);
                break;
            }
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            {
                const uint16_t* half = reinterpret_cast<const uint16_t*>(source + x * 8);
                const auto toByte = [](uint16_t value)
                {
                    const uint32_t sign = value >> 15;
                    const uint32_t exponent = (value >> 10) & 0x1F;
                    const uint32_t mantissa = value & 0x3FF;
                    if (sign != 0) return static_cast<uint8_t>(0);
                    float linear = exponent == 0 ? static_cast<float>(mantissa) * 5.9604645e-8f
                                                 : std::ldexp(static_cast<float>(0x400 | mantissa),
                                                              static_cast<int>(exponent) - 25);
                    if (exponent == 31) linear = 1.0f;
                    const float encoded = std::pow(std::min(std::max(linear, 0.0f), 1.0f), 1.0f / 2.2f);
                    return static_cast<uint8_t>(encoded * 255.0f + 0.5f);
                };
                destination[x * 3 + 0] = toByte(half[2]);
                destination[x * 3 + 1] = toByte(half[1]);
                destination[x * 3 + 2] = toByte(half[0]);
                break;
            }
            default:
                return false;
        }
    }
    return true;
}

bool PrepareStaging(ID3D11Device* device, EyeCapture& capture, const D3D11_TEXTURE2D_DESC& source)
{
    if (capture.staging && capture.format == source.Format) return true;
    capture = EyeCapture{};
    const uint32_t bytesPerPixel = BytesPerPixel(source.Format);
    if (bytesPerPixel == 0) return false;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kCrop;
    desc.Height = kCrop;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = source.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    const HRESULT result = device->CreateTexture2D(&desc, nullptr, &capture.staging);
    if (FAILED(result)) return false;
    capture.format = source.Format;
    capture.width = kCrop;
    capture.height = kCrop;
    capture.bytesPerRow = kCrop * bytesPerPixel;
    return true;
}

// Copies the marker-centred crop of one compositor eye and writes it. False is a reported failure, not
// an image: a missing file must never be read as "the tag was absent".
bool CaptureEye(ID3D11Device* device, ID3D11DeviceContext* context, vr::EVREye eyeIndex, int32_t eye,
                int32_t frameIndex, bool tagged)
{
    if (g_state.compositor == nullptr) return false;
    void* view = nullptr;
    const vr::EVRCompositorError result = g_state.compositor->GetMirrorTextureD3D11(eyeIndex, device, &view);
    if (result != vr::VRCompositorError_None || view == nullptr)
    {
        Log("XR proof: SteamVR did not provide the eye %d mirror image (%d); presentation stays unverified.",
            eye, static_cast<int>(result));
        g_state.unavailable = true;
        return false;
    }
    const auto release = [&](ID3D11ShaderResourceView* srv)
    {
        g_state.compositor->ReleaseMirrorTextureD3D11(srv);
    };

    auto* srv = static_cast<ID3D11ShaderResourceView*>(view);
    ComPtr<ID3D11Resource> resource;
    srv->GetResource(&resource);
    ComPtr<ID3D11Texture2D> mirror;
    if (resource == nullptr || FAILED(resource.As(&mirror)) || !mirror)
    {
        release(srv);
        Log("XR proof: the eye %d mirror view is not a 2D texture.", eye);
        g_state.unavailable = true;
        return false;
    }
    D3D11_TEXTURE2D_DESC source{};
    mirror->GetDesc(&source);
    source.Format = ConcreteFormat(source.Format);
    if (source.SampleDesc.Count != 1 || !PrepareStaging(device, g_state.eyes[eye], source))
    {
        release(srv);
        Log("XR proof: the eye %d mirror image format %d cannot be read.", eye, static_cast<int>(source.Format));
        g_state.unavailable = true;
        return false;
    }
    EyeCapture& capture = g_state.eyes[eye];
    const uint32_t centreX = static_cast<uint32_t>(source.Width * kMarkerCenterPercent / 100.0f + 0.5f);
    const uint32_t centreY = static_cast<uint32_t>(source.Height * kMarkerCenterPercent / 100.0f + 0.5f);
    const uint32_t left = std::min(source.Width > kCrop ? source.Width - kCrop : 0,
                                   centreX > kCrop / 2 ? centreX - kCrop / 2 : 0);
    const uint32_t top = std::min(source.Height > kCrop ? source.Height - kCrop : 0,
                                  centreY > kCrop / 2 ? centreY - kCrop / 2 : 0);
    D3D11_BOX box{left, top, 0, left + std::min(kCrop, source.Width), top + std::min(kCrop, source.Height), 1};
    context->CopySubresourceRegion(capture.staging.Get(), 0, 0, 0, 0, mirror.Get(), 0, &box);
    release(srv);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(capture.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
    {
        Log("XR proof: the eye %d mirror crop could not be read back.", eye);
        g_state.unavailable = true;
        return false;
    }
    const uint32_t width = box.right - box.left;
    const uint32_t height = box.bottom - box.top;
    std::vector<uint8_t> bgr(static_cast<size_t>(width) * height * 3);
    bool converted = true;
    for (uint32_t y = 0; y < height && converted; y++)
        converted = ConvertRow(static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
            capture.format, width, &bgr[static_cast<size_t>(y) * width * 3]);
    context->Unmap(capture.staging.Get(), 0);
    if (!converted)
    {
        Log("XR proof: the eye %d mirror crop was not converted.", eye);
        g_state.unavailable = true;
        return false;
    }
    const std::wstring directory = LogDirectory();
    if (directory.empty()) return false;
    wchar_t name[160];
    swprintf_s(name, L"xr-proof-%d-eye%d-frame%d-%ls.bmp", g_state.cameraId, eye, frameIndex,
        tagged ? L"tagged" : L"untagged");
    const std::wstring path = directory + L"\\" + name;
    if (!WriteBitmap(path, bgr, width, height)) return false;
    Log("XR proof camera=%d eye=%d frame=%d tagged=%d mirror=%ux%u crop=(%u,%u) %ux%u file=%ls", g_state.cameraId,
        eye, frameIndex, tagged ? 1 : 0, source.Width, source.Height, left, top, width, height, name);
    return true;
}
} // namespace

bool Requested()
{
    // One switch, set by the managed side from Diagnostics/XrProof before the bridge loads.
    static const bool requested = []
    {
        char value[8]{};
        const DWORD length = GetEnvironmentVariableA("BIGSCALE_XR_PROOF", value, sizeof(value));
        return length > 0 && value[0] == '1';
    }();
    return requested;
}

void ObserveFrame(ID3D11Device* device, ID3D11DeviceContext* context, int32_t cameraId, int32_t eye,
                  int32_t frameIndex, bool tagged)
{
    if (!Requested() || g_state.unavailable || g_state.finished) return;
    if (device == nullptr || context == nullptr || eye < 0 || eye > 1) return;
    if (!Resolve()) return;
    if (tagged)
    {
        if (!g_state.taggedSeen)
        {
            g_state.taggedSeen = true;
            g_state.cameraId = cameraId;
            Log("XR proof: tagged window opened camera=%d frame=%d.", cameraId, frameIndex);
        }
        if (g_state.tagged >= kMaxTagged * 2) return;
    }
    else
    {
        if (!g_state.taggedSeen || g_state.untagged >= kMaxUntagged * 2) return;
    }
    if (g_state.taggedSeen && g_state.settleOpen && !tagged)
    {
        g_state.settleOpen = false;
        Log("XR proof: tagged window closed camera=%d frame=%d; collecting untagged control pairs.", cameraId,
            frameIndex);
    }
    if (frameIndex - g_state.lastCaptureFrame < static_cast<int32_t>(kEveryFrames)) return;
    if (!CaptureEye(device, context, eye == 0 ? vr::Eye_Left : vr::Eye_Right, eye, frameIndex, tagged)) return;
    g_state.lastCaptureFrame = frameIndex;
    if (tagged) g_state.tagged++;
    else g_state.untagged++;
    if (g_state.tagged >= kMaxTagged * 2 && g_state.untagged >= kMaxUntagged * 2)
    {
        g_state.finished = true;
        Log("XR proof: complete camera=%d tagged=%u untagged=%u.", cameraId, g_state.tagged, g_state.untagged);
        Release();
    }
}

void Release()
{
    for (EyeCapture& capture : g_state.eyes) capture = EyeCapture{};
    g_state.compositor = nullptr;
}
} // namespace XrPresentationProof
} // namespace bigscale
