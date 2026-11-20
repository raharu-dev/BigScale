#include "StageProbe.h"

#include "Log.h"

#include <wrl/client.h>

#include <windows.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace bigscale
{
namespace StageProbe
{
namespace
{
// Two views (one camera, both eyes) is the documented VR arrangement; the extra slots absorb a
// camera transition without dropping the history the comparison needs.
constexpr int kMaxEntries = 4;
constexpr uint32_t kBox = 256;
// One full-length band identifies each eye: eye 0 gets a vertical band spanning the whole eye height,
// eye 1 a horizontal band spanning the whole width. A band cannot hide in a lens corner the way a small
// square could, and its aspect ratio is the identity, so a compositor image that repeats one slice in
// both eyes is decidable from pixels alone. It crosses the centre, so the saved crop pairs are taken
// only from untagged frames and stay clean.
constexpr uint32_t kMarkerThickness = 24;
// The centre of the panel: a band there cannot fall inside the compositor's hidden-area mesh, which is
// what the 10% placement did.
constexpr float kMarkerCenterX = 0.5f;
constexpr float kMarkerCenterY = 0.5f;
// One sample per 30 image operations, four samples per log line: about every 120 operations. While the
// identity marker is stamped the period drops to one frame, which is the bounded burst the presentation
// proof uses to measure frame-to-frame edge movement.
constexpr uint32_t kSamplePeriod = 30;
constexpr uint32_t kMarkerSamplePeriod = 1;
constexpr uint32_t kSamplesPerLog = 4;
// The two operations whose collected pair is written as images: one early sample, one late enough
// that a stalled image cannot hide behind the first.
constexpr uint64_t kCaptureOrdinals[] = {120, 600};

// One sampled stage: a pair of read-back targets plus the comparison counters it feeds.
struct Stage
{
    ComPtr<ID3D11Texture2D> staging[2];
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t bytesPerPixel = 0;
    int32_t pending = -1;
    int32_t next = 0;
    bool primed = false;
    uint64_t hash = 0;
    uint32_t samples = 0;
    uint32_t changes = 0;
    // Identity of the copy currently pending. Recorded when that copy is queued and never refreshed
    // from a later frame, so an image is always labelled with the operation it came from.
    uint64_t ordinal = 0;
    bool capture = false;
    D3D11_BOX box{};
    DXGI_FORMAT sourceFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    uint32_t slice = 0;
    uint32_t regionX = 0;
    uint32_t regionY = 0;
    uint32_t regionWidth = 0;
    uint32_t regionHeight = 0;
};

struct Entry
{
    bool used = false;
    bool disabled = false;
    bool hashing = true;
    bool captureDisabled = false;
    bool smallRegionNoted = false;
    int32_t cameraId = 0;
    int32_t eye = 0;
    uint64_t lastUse = 0;
    uint64_t token = 0;
    uint32_t operations = 0;
    Stage input;
    Stage output;
    ComPtr<ID3D11Texture2D> marker;
    DXGI_FORMAT markerFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t markerWidth = 0;
    uint32_t markerHeight = 0;
    bool markerUnavailable = false;
    bool markerLogged = false;
    bool markerLast = false;
    bool forceCapture = false;
    uint32_t captures = 0;
};

Entry g_entries[kMaxEntries];
uint64_t g_useCounter = 0;
uint64_t g_lastToken = 0;

// Staging and marker textures need a concrete format. Copying stays legal across a typeless
// family, so the concrete member of the same family is the one that can be created and mapped.
DXGI_FORMAT Concrete(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default: return format;
    }
}

// Active texel bytes per pixel of a sampled row. Zero means the hash would have to invent a size.
uint32_t BytesPerPixel(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R11G11B10_FLOAT:
            return 4;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
            return 8;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
            return 16;
        default:
            return 0;
    }
}

// Builds one band texture for this eye and region: 24 x regionHeight magenta for eye 0, or
// regionWidth x 24 for eye 1. One copy per frame then draws the whole band.
bool FillBand(DXGI_FORMAT format, int32_t eye, uint32_t width, uint32_t height, std::vector<uint8_t>& pixels,
              uint32_t& bytesPerRow)
{
    const uint32_t texels = width * height;
    const auto store = [&](uint8_t* destination, size_t stride, const void* source, size_t size)
    {
        for (uint32_t i = 0; i < texels; i++)
            std::memcpy(destination + static_cast<size_t>(i) * stride, source, size);
    };
    switch (format)
    {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        {
            bytesPerRow = width * 4;
            pixels.assign(static_cast<size_t>(bytesPerRow) * height, 0);
            // R8G8B8A8 and B8G8R8A8 both need 255 in the red and blue bytes, which is magenta either way.
            const uint8_t channel[4] = {255, 0, 255, 255};
            store(pixels.data(), 4, channel, sizeof(channel));
            return true;
        }
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        {
            bytesPerRow = width * 8;
            pixels.assign(static_cast<size_t>(bytesPerRow) * height, 0);
            const uint16_t half[4] = {0x3C00, 0x0000, 0x3C00, 0x3C00};
            store(pixels.data(), 8, half, sizeof(half));
            return true;
        }
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        {
            bytesPerRow = width * 16;
            pixels.assign(static_cast<size_t>(bytesPerRow) * height, 0);
            const float values[4] = {1.0f, 0.0f, 1.0f, 1.0f};
            store(pixels.data(), 16, values, sizeof(values));
            return true;
        }
        default:
            return false;
    }
}

HRESULT CreateReadBack(ID3D11Device* device, DXGI_FORMAT format, ComPtr<ID3D11Texture2D>& target)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kBox;
    desc.Height = kBox;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return device->CreateTexture2D(&desc, nullptr, &target);
}

// One token per entry lifetime. FILETIME keeps runs apart and the process id keeps concurrent
// processes apart; the counter keeps two entries acquired inside one 100-ns tick apart.
uint64_t EntryToken()
{
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    uint64_t token = (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    token ^= static_cast<uint64_t>(GetCurrentProcessId()) << 48;
    if (token <= g_lastToken) token = g_lastToken + 1;
    g_lastToken = token;
    return token;
}

bool IsCaptureOrdinal(uint64_t ordinal)
{
    for (uint64_t candidate : kCaptureOrdinals)
    {
        if (ordinal == candidate) return true;
    }
    return false;
}

Entry& Acquire(int32_t cameraId, int32_t eye)
{
    for (Entry& entry : g_entries)
    {
        if (entry.used && entry.cameraId == cameraId && entry.eye == eye)
        {
            entry.lastUse = ++g_useCounter;
            return entry;
        }
    }
    Entry* chosen = nullptr;
    for (Entry& entry : g_entries)
    {
        if (!entry.used)
        {
            chosen = &entry;
            break;
        }
    }
    if (chosen == nullptr)
    {
        chosen = &g_entries[0];
        for (Entry& entry : g_entries)
        {
            if (entry.lastUse < chosen->lastUse) chosen = &entry;
        }
        *chosen = Entry{};
    }
    chosen->used = true;
    chosen->cameraId = cameraId;
    chosen->eye = eye;
    chosen->lastUse = ++g_useCounter;
    chosen->token = EntryToken();
    return *chosen;
}

void Disable(Entry& entry, const char* what, HRESULT result)
{
    entry.disabled = true;
    entry.input = Stage{};
    entry.output = Stage{};
    entry.marker.Reset();
    Log("stage probe disabled camera=%d eye=%d: %s failed hr=0x%08x", entry.cameraId, entry.eye, what,
        static_cast<unsigned>(result));
}

// Creates (or reuses) the read-back pair for one stage, in the source's own format family.
HRESULT EnsureStage(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& source, Stage& stage)
{
    const DXGI_FORMAT format = Concrete(source.Format);
    if (stage.staging[0] && stage.format == format) return S_OK;
    stage = Stage{};
    HRESULT result = CreateReadBack(device, format, stage.staging[0]);
    if (SUCCEEDED(result)) result = CreateReadBack(device, format, stage.staging[1]);
    if (FAILED(result))
    {
        stage = Stage{};
        return result;
    }
    stage.format = format;
    stage.bytesPerPixel = BytesPerPixel(format);
    return S_OK;
}

bool CentredBox(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const D3D11_TEXTURE2D_DESC& texture,
                D3D11_BOX& box)
{
    if (width < kBox || height < kBox) return false;
    const uint32_t left = x + width / 2 - kBox / 2;
    const uint32_t top = y + height / 2 - kBox / 2;
    if (static_cast<uint64_t>(left) + kBox > texture.Width || static_cast<uint64_t>(top) + kBox > texture.Height)
        return false;
    box = D3D11_BOX{left, top, 0, left + kBox, top + kBox, 1};
    return true;
}

// A region smaller than the crop is reported once per entry instead of being sampled elsewhere,
// because a crop taken from outside the declared region would not describe this view.
void NoteSmallRegion(Entry& entry, const char* what, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (entry.smallRegionNoted) return;
    entry.smallRegionNoted = true;
    Log("stage probe unsampled camera=%d eye=%d: %s region %ux%u at (%u,%u) is smaller than the %u-pixel crop",
        entry.cameraId, entry.eye, what, width, height, x, y, kBox);
}

HRESULT PrepareStage(ID3D11Device* device, Entry& entry, Stage& stage, const D3D11_TEXTURE2D_DESC& source)
{
    const HRESULT result = EnsureStage(device, source, stage);
    if (FAILED(result)) return result;
    if (stage.bytesPerPixel == 0 && entry.hashing)
    {
        // Inventing a texel size would hash padding or truncate texels; unsupported formats keep the
        // marker and the image capture and lose only the comparison columns.
        entry.hashing = false;
        Log("stage probe hashing unavailable camera=%d eye=%d: format %d has no texel size", entry.cameraId,
            entry.eye, static_cast<int>(stage.format));
    }
    return S_OK;
}

void Queue(ID3D11DeviceContext* context, ID3D11Texture2D* source, UINT subresource, const D3D11_BOX& box, Stage& stage)
{
    const int32_t index = stage.next;
    stage.next ^= 1;
    context->CopySubresourceRegion(stage.staging[index].Get(), 0, 0, 0, 0, source, subresource, &box);
    stage.pending = index;
}

// Hashes the active texels of every row. The mapped pitch may exceed the active row, and hashing the
// padding would mix driver-specific bytes into a comparison that is meant to follow the image.
uint64_t HashReadBack(const D3D11_MAPPED_SUBRESOURCE& mapped, uint32_t activeRowBytes)
{
    const uint8_t* rows = static_cast<const uint8_t*>(mapped.pData);
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t y = 0; y < kBox; y++)
    {
        const uint8_t* row = rows + static_cast<size_t>(y) * mapped.RowPitch;
        for (uint32_t i = 0; i < activeRowBytes; i++)
        {
            hash ^= row[i];
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

void Record(Stage& stage, uint64_t hash)
{
    if (!stage.primed)
    {
        stage.primed = true;
        stage.hash = hash;
        return;
    }
    stage.samples++;
    if (hash != stage.hash) stage.changes++;
    stage.hash = hash;
}

void LogSample(Entry& entry)
{
    if (entry.input.samples < kSamplesPerLog || entry.output.samples < kSamplesPerLog) return;
    Log("stage probe camera=%d eye=%d input changed=%u/%u output changed=%u/%u input=%016llx output=%016llx",
        entry.cameraId, entry.eye, entry.input.changes, entry.input.samples, entry.output.changes,
        entry.output.samples, static_cast<unsigned long long>(entry.input.hash),
        static_cast<unsigned long long>(entry.output.hash));
    entry.input.samples = 0;
    entry.input.changes = 0;
    entry.output.samples = 0;
    entry.output.changes = 0;
}

// IEEE-754 binary16 to float. Zero, subnormals, signed values, infinities and NaN are explicit, so a
// nonfinite texel is counted as such instead of being folded into zero.
float HalfToFloat(uint16_t bits)
{
    const uint32_t sign = bits >> 15;
    const uint32_t exponent = (bits >> 10) & 0x1F;
    const uint32_t mantissa = bits & 0x3FF;
    float value;
    if (exponent == 0)
    {
        value = mantissa == 0 ? 0.0f : std::ldexp(static_cast<float>(mantissa), -24);
    }
    else if (exponent == 31)
    {
        value = mantissa == 0 ? INFINITY : NAN;
    }
    else
    {
        value = std::ldexp(static_cast<float>(0x400 | mantissa), static_cast<int>(exponent) - 25);
    }
    if (sign) value = -value;
    return value;
}

// IEC 61966-2-1 encode, the inverse of what the preview is meant to look like on a desktop viewer.
float LinearToSrgb(float value)
{
    if (value <= 0.0031308f) return value * 12.92f;
    return 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

// Nonfinite values have no displayable colour, so the preview shows them as black. The statistics
// below still count them, which is what keeps a clipped preview from hiding invalid data.
uint8_t PreviewByte(float value)
{
    if (!std::isfinite(value)) return 0;
    const float clamped = value <= 0.0f ? 0.0f : (value >= 1.0f ? 1.0f : value);
    const int encoded = static_cast<int>(LinearToSrgb(clamped) * 255.0f + 0.5f);
    return static_cast<uint8_t>(encoded < 0 ? 0 : (encoded > 255 ? 255 : encoded));
}

struct CaptureStats
{
    uint32_t finiteCount[3] = {0, 0, 0};
    double minimum[3] = {0.0, 0.0, 0.0};
    double maximum[3] = {0.0, 0.0, 0.0};
    double sum[3] = {0.0, 0.0, 0.0};
    uint32_t nonfinite = 0;
    uint32_t luminanceCount = 0;
    double luminanceSum = 0.0;
    double luminanceSquares = 0.0;
};

void Accumulate(CaptureStats& stats, int channel, float value)
{
    if (!std::isfinite(value))
    {
        stats.nonfinite++;
        return;
    }
    if (stats.finiteCount[channel] == 0)
    {
        stats.minimum[channel] = value;
        stats.maximum[channel] = value;
    }
    else
    {
        if (value < stats.minimum[channel]) stats.minimum[channel] = value;
        if (value > stats.maximum[channel]) stats.maximum[channel] = value;
    }
    stats.finiteCount[channel]++;
    stats.sum[channel] += value;
}

std::string ChannelText(const CaptureStats& stats, int channel)
{
    if (stats.finiteCount[channel] == 0) return "no-finite-samples";
    char text[96];
    std::snprintf(text, sizeof(text), "%.4f/%.4f/%.4f", stats.minimum[channel], stats.maximum[channel],
        stats.sum[channel] / stats.finiteCount[channel]);
    return text;
}

std::string LuminanceStdDevText(const CaptureStats& stats)
{
    if (stats.luminanceCount == 0) return "no-finite-samples";
    const double mean = stats.luminanceSum / stats.luminanceCount;
    const double variance = stats.luminanceSquares / stats.luminanceCount - mean * mean;
    char text[48];
    std::snprintf(text, sizeof(text), "%.4f", variance > 0.0 ? std::sqrt(variance) : 0.0);
    return text;
}

// Decodes one mapped crop straight from the mapped rows into top-down 24-bit BGR, accumulating the
// unclamped statistics over the same pass. Alpha is ignored: the preview and the statistics are RGB.
void DecodeCrop(const D3D11_MAPPED_SUBRESOURCE& mapped, std::vector<uint8_t>& preview, CaptureStats& stats)
{
    preview.assign(static_cast<size_t>(kBox) * kBox * 3, 0);
    for (uint32_t y = 0; y < kBox; y++)
    {
        const uint8_t* sourceRow = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        const uint16_t* texels = reinterpret_cast<const uint16_t*>(sourceRow);
        uint8_t* destinationRow = &preview[static_cast<size_t>(y) * kBox * 3];
        for (uint32_t x = 0; x < kBox; x++)
        {
            const float red = HalfToFloat(texels[x * 4 + 0]);
            const float green = HalfToFloat(texels[x * 4 + 1]);
            const float blue = HalfToFloat(texels[x * 4 + 2]);
            Accumulate(stats, 0, red);
            Accumulate(stats, 1, green);
            Accumulate(stats, 2, blue);
            if (std::isfinite(red) && std::isfinite(green) && std::isfinite(blue))
            {
                const double luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
                stats.luminanceCount++;
                stats.luminanceSum += luminance;
                stats.luminanceSquares += luminance * luminance;
            }
            destinationRow[x * 3 + 0] = PreviewByte(blue);
            destinationRow[x * 3 + 1] = PreviewByte(green);
            destinationRow[x * 3 + 2] = PreviewByte(red);
        }
    }
}

std::wstring Widen(const std::string& text)
{
    return std::wstring(text.begin(), text.end());
}

std::string CaptureName(const Entry& entry, uint64_t ordinal, const char* stage)
{
    char name[192];
    std::snprintf(name, sizeof(name), "sr-probe-%016llx-camera%d-eye%d-sample%llu-%s.bmp",
        static_cast<unsigned long long>(entry.token), entry.cameraId, entry.eye,
        static_cast<unsigned long long>(ordinal), stage);
    return name;
}

bool WriteAll(HANDLE handle, const void* data, size_t bytes, DWORD& failure)
{
    const auto* cursor = static_cast<const uint8_t*>(data);
    size_t remaining = bytes;
    while (remaining > 0)
    {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(remaining > 0x1000000 ? 0x1000000 : remaining);
        if (!WriteFile(handle, cursor, chunk, &written, nullptr))
        {
            failure = GetLastError();
            return false;
        }
        if (written == 0)
        {
            failure = ERROR_WRITE_FAULT;
            return false;
        }
        cursor += written;
        remaining -= written;
    }
    return true;
}

// Top-down 24-bit BMP: negative biHeight makes the first stored row the top row, and every row is
// padded to a four-byte boundary. CREATE_NEW keeps an earlier capture from being overwritten.
bool WriteBitmap(const std::wstring& path, const std::vector<uint8_t>& preview, DWORD& failure)
{
    const uint32_t rowBytes = kBox * 3;
    const uint32_t stride = (rowBytes + 3) & ~3u;
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(BITMAPINFOHEADER);
    info.biWidth = static_cast<LONG>(kBox);
    info.biHeight = -static_cast<LONG>(kBox);
    info.biPlanes = 1;
    info.biBitCount = 24;
    info.biCompression = BI_RGB;
    info.biSizeImage = stride * kBox;
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + info.biSizeImage;

    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        failure = GetLastError();
        return false;
    }
    bool written = WriteAll(handle, &file, sizeof(file), failure) && WriteAll(handle, &info, sizeof(info), failure);
    static const uint8_t padding[4] = {0, 0, 0, 0};
    for (uint32_t y = 0; written && y < kBox; y++)
    {
        written = WriteAll(handle, &preview[static_cast<size_t>(y) * rowBytes], rowBytes, failure) &&
                  WriteAll(handle, padding, stride - rowBytes, failure);
    }
    if (!CloseHandle(handle) && written)
    {
        failure = GetLastError();
        written = false;
    }
    return written;
}

// Writes the bounded image evidence for one collected pair. False disables image dumping for this
// entry: the hashes and the marker stay, and no file is ever offered as evidence for a later frame.
bool Capture(Entry& entry, const D3D11_MAPPED_SUBRESOURCE& inputMap, const D3D11_MAPPED_SUBRESOURCE& outputMap)
{
    entry.captureDisabled = true;
    if (entry.input.format != DXGI_FORMAT_R16G16B16A16_FLOAT || entry.output.format != DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        Log("stage probe image capture unavailable camera=%d eye=%d: formats %d/%d are not RGBA16F", entry.cameraId,
            entry.eye, static_cast<int>(entry.input.format), static_cast<int>(entry.output.format));
        return false;
    }
    const std::wstring directory = LogDirectory();
    if (directory.empty())
    {
        Log("stage probe image capture unavailable camera=%d eye=%d: no log directory", entry.cameraId, entry.eye);
        return false;
    }

    CaptureStats inputStats;
    CaptureStats outputStats;
    std::vector<uint8_t> inputPreview;
    std::vector<uint8_t> outputPreview;
    try
    {
        DecodeCrop(inputMap, inputPreview, inputStats);
        DecodeCrop(outputMap, outputPreview, outputStats);
    }
    catch (...)
    {
        Log("stage probe image capture unavailable camera=%d eye=%d: preview allocation failed", entry.cameraId,
            entry.eye);
        return false;
    }

    const uint64_t ordinal = entry.input.ordinal;
    const std::string inputName = CaptureName(entry, ordinal, "input");
    const std::string outputName = CaptureName(entry, ordinal, "output");
    const std::wstring inputPath = directory + L"\\" + Widen(inputName);
    const std::wstring outputPath = directory + L"\\" + Widen(outputName);
    DWORD failure = 0;
    if (!WriteBitmap(inputPath, inputPreview, failure) || !WriteBitmap(outputPath, outputPreview, failure))
    {
        // A half-written pair would be read as evidence of a frame that was never captured.
        DeleteFileW(inputPath.c_str());
        DeleteFileW(outputPath.c_str());
        Log("stage probe image capture failed camera=%d eye=%d sample=%llu: write failed error=%lu", entry.cameraId,
            entry.eye, static_cast<unsigned long long>(ordinal), static_cast<unsigned long>(failure));
        return false;
    }
    entry.captureDisabled = false;

    Log("stage probe capture camera=%d eye=%d sample=%llu source=%d %ux%u crop=%u,%u,%ux%u "
        "output=%d %ux%u slice=%u region=%u,%u,%ux%u crop=%u,%u,%ux%u input=%s output=%s",
        entry.cameraId, entry.eye, static_cast<unsigned long long>(ordinal),
        static_cast<int>(entry.input.sourceFormat), entry.input.sourceWidth, entry.input.sourceHeight,
        entry.input.box.left, entry.input.box.top, kBox, kBox,
        static_cast<int>(entry.output.sourceFormat), entry.output.sourceWidth, entry.output.sourceHeight,
        entry.output.slice, entry.output.regionX, entry.output.regionY, entry.output.regionWidth,
        entry.output.regionHeight, entry.output.box.left, entry.output.box.top, kBox, kBox, inputName.c_str(),
        outputName.c_str());
    const std::string inputRed = ChannelText(inputStats, 0);
    const std::string inputGreen = ChannelText(inputStats, 1);
    const std::string inputBlue = ChannelText(inputStats, 2);
    const std::string outputRed = ChannelText(outputStats, 0);
    const std::string outputGreen = ChannelText(outputStats, 1);
    const std::string outputBlue = ChannelText(outputStats, 2);
    const std::string inputLuminance = LuminanceStdDevText(inputStats);
    const std::string outputLuminance = LuminanceStdDevText(outputStats);
    Log("stage probe stats camera=%d eye=%d sample=%llu stage=input r=(%s) g=(%s) b=(%s) luma_stddev=%s nonfinite=%u",
        entry.cameraId, entry.eye, static_cast<unsigned long long>(ordinal), inputRed.c_str(), inputGreen.c_str(),
        inputBlue.c_str(), inputLuminance.c_str(), inputStats.nonfinite);
    Log("stage probe stats camera=%d eye=%d sample=%llu stage=output r=(%s) g=(%s) b=(%s) luma_stddev=%s nonfinite=%u",
        entry.cameraId, entry.eye, static_cast<unsigned long long>(ordinal), outputRed.c_str(), outputGreen.c_str(),
        outputBlue.c_str(), outputLuminance.c_str(), outputStats.nonfinite);
    return true;
}

enum class CollectResult
{
    Collected,
    Waiting,
    Failed,
};

// Reads both queued copies without stalling the render thread. `Waiting` keeps them pending so a
// busy GPU is retried instead of losing the sample; any other map failure is a real fault.
CollectResult Collect(ID3D11DeviceContext* context, Entry& entry, const char*& what, HRESULT& failure)
{
    if (entry.input.pending < 0 || entry.output.pending < 0) return CollectResult::Waiting;
    ID3D11Resource* input = entry.input.staging[entry.input.pending].Get();
    ID3D11Resource* output = entry.output.staging[entry.output.pending].Get();
    D3D11_MAPPED_SUBRESOURCE inputMap{};
    const HRESULT inputResult = context->Map(input, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &inputMap);
    if (inputResult == DXGI_ERROR_WAS_STILL_DRAWING) return CollectResult::Waiting;
    if (FAILED(inputResult))
    {
        what = "input read-back map";
        failure = inputResult;
        return CollectResult::Failed;
    }
    D3D11_MAPPED_SUBRESOURCE outputMap{};
    const HRESULT outputResult = context->Map(output, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &outputMap);
    if (outputResult == DXGI_ERROR_WAS_STILL_DRAWING)
    {
        context->Unmap(input, 0);
        return CollectResult::Waiting;
    }
    if (FAILED(outputResult))
    {
        context->Unmap(input, 0);
        what = "output read-back map";
        failure = outputResult;
        return CollectResult::Failed;
    }

    if (entry.hashing)
    {
        Record(entry.input, HashReadBack(inputMap, entry.input.bytesPerPixel * kBox));
        Record(entry.output, HashReadBack(outputMap, entry.output.bytesPerPixel * kBox));
    }
    if ((entry.input.capture || entry.output.capture) && !entry.captureDisabled)
    {
        if (entry.input.ordinal == entry.output.ordinal)
        {
            Capture(entry, inputMap, outputMap);
        }
        else
        {
            Log("stage probe capture skipped camera=%d eye=%d: the queued pair spans operations %llu and %llu",
                entry.cameraId, entry.eye, static_cast<unsigned long long>(entry.input.ordinal),
                static_cast<unsigned long long>(entry.output.ordinal));
        }
    }
    context->Unmap(input, 0);
    context->Unmap(output, 0);
    entry.input.pending = -1;
    entry.output.pending = -1;
    return CollectResult::Collected;
}

// Stamps this eye's identity bar into the declared region, so a compositor image can be matched to the
// eye that produced it. The position is fixed per eye; the shape carries the identity.
void Stamp(ID3D11Device* device, ID3D11DeviceContext* context, Entry& entry, ID3D11Texture2D* outputTexture,
           const D3D11_TEXTURE2D_DESC& output, uint32_t outputSlice, uint32_t outputX, uint32_t outputY,
           uint32_t outputWidth, uint32_t outputHeight)
{
    if (outputWidth < kMarkerThickness + 2 || outputHeight < kMarkerThickness + 2) return;
    if (entry.markerUnavailable) return;
    const DXGI_FORMAT format = Concrete(output.Format);
    const uint32_t bandWidth = entry.eye == 0 ? kMarkerThickness : outputWidth;
    const uint32_t bandHeight = entry.eye == 0 ? outputHeight : kMarkerThickness;
    const bool resize = entry.marker && (entry.markerFormat != format || entry.markerWidth != bandWidth ||
                                         entry.markerHeight != bandHeight);
    if (resize) entry.marker.Reset();
    if (!entry.marker)
    {
        std::vector<uint8_t> pixels;
        uint32_t bytesPerRow = 0;
        if (!FillBand(format, entry.eye, bandWidth, bandHeight, pixels, bytesPerRow))
        {
            entry.markerUnavailable = true;
            Log("stage probe marker unavailable camera=%d eye=%d: format %d has no magenta, hr=0x%08x",
                entry.cameraId, entry.eye, static_cast<int>(format), static_cast<unsigned>(E_INVALIDARG));
            return;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = bandWidth;
        desc.Height = bandHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = pixels.data();
        data.SysMemPitch = bytesPerRow;
        const HRESULT result = device->CreateTexture2D(&desc, &data, &entry.marker);
        if (FAILED(result))
        {
            // The hash columns are the load-bearing evidence, so a marker failure never costs them.
            entry.markerUnavailable = true;
            Log("stage probe marker unavailable camera=%d eye=%d: marker failed hr=0x%08x", entry.cameraId,
                entry.eye, static_cast<unsigned>(result));
            return;
        }
        entry.markerFormat = format;
        entry.markerWidth = bandWidth;
        entry.markerHeight = bandHeight;
    }
    const uint32_t centreX = outputX + static_cast<uint32_t>(outputWidth * kMarkerCenterX + 0.5f);
    const uint32_t centreY = outputY + static_cast<uint32_t>(outputHeight * kMarkerCenterY + 0.5f);
    const uint32_t left = entry.eye == 0
        ? static_cast<uint32_t>(std::max<int64_t>(outputX,
              std::min<int64_t>(static_cast<int64_t>(outputX + outputWidth - bandWidth),
                  static_cast<int64_t>(centreX) - static_cast<int64_t>(bandWidth / 2))))
        : outputX;
    const uint32_t top = entry.eye == 0
        ? outputY
        : static_cast<uint32_t>(std::max<int64_t>(outputY,
              std::min<int64_t>(static_cast<int64_t>(outputY + outputHeight - bandHeight),
                  static_cast<int64_t>(centreY) - static_cast<int64_t>(bandHeight / 2))));
    const UINT subresource = D3D11CalcSubresource(0, outputSlice, output.MipLevels);
    context->CopySubresourceRegion(outputTexture, subresource, left, top, 0, entry.marker.Get(), 0, nullptr);
    if (!entry.markerLogged)
    {
        entry.markerLogged = true;
        Log("stage probe identity band camera=%d eye=%d at=(%u,%u) size=%ux%u orientation=%s", entry.cameraId,
            entry.eye, left, top, bandWidth, bandHeight, entry.eye == 0 ? "vertical" : "horizontal");
    }
}
} // namespace

void AfterWriteBack(ID3D11Device* device, ID3D11DeviceContext* context, int32_t cameraId, int32_t eye,
                    ID3D11Texture2D* inputScratch, ID3D11Texture2D* outputTexture, uint32_t outputSlice,
                    uint32_t outputX, uint32_t outputY, uint32_t outputWidth, uint32_t outputHeight, bool marker)
{
    if (device == nullptr || context == nullptr || outputTexture == nullptr) return;
    Entry& entry = Acquire(cameraId, eye);
    if (entry.disabled) return;

    D3D11_TEXTURE2D_DESC outputDescription{};
    outputTexture->GetDesc(&outputDescription);
    if (outputDescription.SampleDesc.Count != 1)
    {
        Disable(entry, "multisampled output", E_INVALIDARG);
        return;
    }

    entry.operations++;
    const uint64_t ordinal = entry.operations;
    // A proof window opens its own capture budget: the burst below would otherwise be spent during the
    // first seconds of the session and leave the tagged frames with no image evidence at all.
    if (marker && !entry.markerLast)
    {
        entry.captures = 0;
        // The window can be short or cut by a dashboard visit: take its first frame, not the first frame
        // that happens to land on the cadence.
        entry.forceCapture = true;
    }
    entry.markerLast = marker;
    const uint32_t period = marker ? kMarkerSamplePeriod : kSamplePeriod;
    // A proof run is a few hundred frames and cannot wait for the scheduled capture ordinals, so frames
    // capture in a bounded burst of their own; the burst forces the sample that carries the copy. Tagged
    // frames are included on purpose: their crop is the only place the identity band can be seen in the
    // engine output, which is what the array comparison is anchored to.
    const bool burstCapture = (entry.forceCapture || ordinal % 24 == 5) && entry.captures < 6;
    entry.forceCapture = false;
    if (burstCapture) entry.captures++;
    const bool due = ordinal % period == 0 || burstCapture;
    const bool waiting = entry.input.pending >= 0 || entry.output.pending >= 0;
    // A queued copy is drained as soon as the GPU has finished it, not on the next 30-frame tick:
    // losing one tick to a busy GPU would cost a third of a second of evidence.
    if (due || waiting)
    {
        const char* what = "read-back map";
        HRESULT failure = S_OK;
        const CollectResult collected = Collect(context, entry, what, failure);
        if (collected == CollectResult::Failed)
        {
            Disable(entry, what, failure);
            return;
        }
        if (collected == CollectResult::Collected) LogSample(entry);
    }
    const bool captureDue = burstCapture || IsCaptureOrdinal(ordinal);
    if (due)
    {
        if (captureDue && (entry.input.pending >= 0 || entry.output.pending >= 0) && !entry.captureDisabled)
        {
            // Labelling a later frame with a scheduled ordinal would invent evidence; a missed
            // capture is reported instead.
            Log("stage probe capture missed camera=%d eye=%d sample=%llu: the previous pair is still pending",
                entry.cameraId, entry.eye, static_cast<unsigned long long>(ordinal));
        }
        const bool capture = captureDue && !entry.captureDisabled;
        if (entry.input.pending < 0 && inputScratch != nullptr)
        {
            D3D11_TEXTURE2D_DESC inputDescription{};
            inputScratch->GetDesc(&inputDescription);
            D3D11_BOX box{};
            if (CentredBox(0, 0, inputDescription.Width, inputDescription.Height, inputDescription, box))
            {
                const HRESULT result = PrepareStage(device, entry, entry.input, inputDescription);
                if (FAILED(result))
                {
                    Disable(entry, "input read-back", result);
                    return;
                }
                Queue(context, inputScratch, 0, box, entry.input);
                entry.input.ordinal = ordinal;
                entry.input.capture = capture;
                entry.input.box = box;
                entry.input.sourceFormat = inputDescription.Format;
                entry.input.sourceWidth = inputDescription.Width;
                entry.input.sourceHeight = inputDescription.Height;
                entry.input.slice = 0;
                entry.input.regionX = 0;
                entry.input.regionY = 0;
                entry.input.regionWidth = inputDescription.Width;
                entry.input.regionHeight = inputDescription.Height;
            }
            else
            {
                NoteSmallRegion(entry, "input", 0, 0, inputDescription.Width, inputDescription.Height);
            }
        }
        if (entry.output.pending < 0)
        {
            D3D11_BOX box{};
            if (CentredBox(outputX, outputY, outputWidth, outputHeight, outputDescription, box))
            {
                const HRESULT result = PrepareStage(device, entry, entry.output, outputDescription);
                if (FAILED(result))
                {
                    Disable(entry, "output read-back", result);
                    return;
                }
                const UINT subresource = D3D11CalcSubresource(0, outputSlice, outputDescription.MipLevels);
                Queue(context, outputTexture, subresource, box, entry.output);
                entry.output.ordinal = ordinal;
                entry.output.capture = capture;
                entry.output.box = box;
                entry.output.sourceFormat = outputDescription.Format;
                entry.output.sourceWidth = outputDescription.Width;
                entry.output.sourceHeight = outputDescription.Height;
                entry.output.slice = outputSlice;
                entry.output.regionX = outputX;
                entry.output.regionY = outputY;
                entry.output.regionWidth = outputWidth;
                entry.output.regionHeight = outputHeight;
            }
            else
            {
                NoteSmallRegion(entry, "output", outputX, outputY, outputWidth, outputHeight);
            }
        }
    }

    if (marker)
        Stamp(device, context, entry, outputTexture, outputDescription, outputSlice, outputX, outputY, outputWidth,
              outputHeight);
}

int ClassifyIdentityMarker(const float* rgba, uint32_t width, uint32_t height, uint32_t strideTexels,
                          float centerX, float centerY, float radius, int32_t* outLeft, int32_t* outTop,
                          int32_t* outRight, int32_t* outBottom)
{
    if (rgba == nullptr || width == 0 || height == 0) return 0;
    const int32_t x0 = static_cast<int32_t>(std::max(0.0f, centerX - radius));
    const int32_t x1 = static_cast<int32_t>(std::min(static_cast<float>(width) - 1.0f, centerX + radius));
    const int32_t y0 = static_cast<int32_t>(std::max(0.0f, centerY - radius));
    const int32_t y1 = static_cast<int32_t>(std::min(static_cast<float>(height) - 1.0f, centerY + radius));
    int32_t minX = INT32_MAX;
    int32_t minY = INT32_MAX;
    int32_t maxX = -1;
    int32_t maxY = -1;
    uint32_t count = 0;
    for (int32_t y = y0; y <= y1; y++)
        for (int32_t x = x0; x <= x1; x++)
        {
            const size_t index = (static_cast<size_t>(y) * strideTexels + static_cast<size_t>(x)) * 4;
            if (!(rgba[index] > 0.5f && rgba[index + 1] < 0.25f && rgba[index + 2] > 0.5f)) continue;
            count++;
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    if (count < 32 || maxX < 0) return 0;
    if (outLeft) *outLeft = minX;
    if (outTop) *outTop = minY;
    if (outRight) *outRight = maxX;
    if (outBottom) *outBottom = maxY;
    const int32_t barWidth = maxX - minX + 1;
    const int32_t barHeight = maxY - minY + 1;
    if (barHeight >= 2 * barWidth) return 1;
    if (barWidth >= 2 * barHeight) return 2;
    return 0;
}

void Reset()
{
    for (Entry& entry : g_entries) entry = Entry{};
    g_useCounter = 0;
    g_lastToken = 0;
}
} // namespace StageProbe
} // namespace bigscale
