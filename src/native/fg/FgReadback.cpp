#include "FgReadback.h"

#include "../Dx12Interop.h"

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace bigscale
{
namespace
{
constexpr uint32_t kBytesPerPixel = 4;

bool SupportedFormat(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R16G16_FLOAT:
            return true;
        default:
            return false;
    }
}
}

bool FgRecordReadback(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* source,
    D3D12_RESOURCE_STATES sourceState, FgReadbackPlane& plane, std::string& reason)
{
    if (device == nullptr || list == nullptr || source == nullptr)
    {
        reason = "the readback source is unavailable";
        return false;
    }
    const D3D12_RESOURCE_DESC description = source->GetDesc();
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.SampleDesc.Count != 1 ||
        description.MipLevels != 1)
    {
        reason = "the readback source is not a single-mip 2D texture";
        return false;
    }
    if (!SupportedFormat(description.Format))
    {
        reason = "the readback source format is not four bytes per pixel";
        return false;
    }
    UINT64 totalBytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);
    if (totalBytes == 0 || footprint.Footprint.Width == 0 || footprint.Footprint.Height == 0)
    {
        reason = "the readback footprint is empty";
        return false;
    }
    if (!plane.buffer || plane.size < totalBytes)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = totalBytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&plane.buffer))) ||
            !plane.buffer)
        {
            reason = "the readback buffer could not be created";
            return false;
        }
    }
    plane.footprint = footprint;
    plane.size = totalBytes;
    plane.width = static_cast<uint32_t>(description.Width);
    plane.height = static_cast<uint32_t>(description.Height);
    plane.format = description.Format;

    D3D12_RESOURCE_STATES tracked = sourceState;
    if (sourceState != D3D12_RESOURCE_STATE_COPY_SOURCE)
        Dx12Interop::Transition(list, source, tracked, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = plane.buffer.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = plane.footprint;
    D3D12_TEXTURE_COPY_LOCATION origin{};
    origin.pResource = source;
    origin.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    origin.SubresourceIndex = 0;
    list->CopyTextureRegion(&destination, 0, 0, 0, &origin, nullptr);
    if (sourceState != D3D12_RESOURCE_STATE_COPY_SOURCE)
        Dx12Interop::Transition(list, source, tracked, sourceState);
    return true;
}

bool FgMapTightRows(const FgReadbackPlane& plane, std::vector<uint8_t>& bytes, std::string& reason)
{
    if (!plane.buffer || plane.width == 0 || plane.height == 0 || !SupportedFormat(plane.format))
    {
        reason = "the readback plane is not ready to map";
        return false;
    }
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(plane.size)};
    void* mapped = nullptr;
    if (FAILED(plane.buffer->Map(0, &readRange, &mapped)) || mapped == nullptr)
    {
        reason = "the readback buffer could not be mapped";
        return false;
    }
    const uint32_t rowBytes = plane.width * kBytesPerPixel;
    bytes.resize(static_cast<size_t>(rowBytes) * plane.height);
    const auto* source = static_cast<const uint8_t*>(mapped) + plane.footprint.Offset;
    const size_t sourcePitch = plane.footprint.Footprint.RowPitch;
    for (uint32_t y = 0; y < plane.height; y++)
        std::memcpy(bytes.data() + static_cast<size_t>(y) * rowBytes, source + y * sourcePitch, rowBytes);
    const D3D12_RANGE writeRange{0, 0};
    plane.buffer->Unmap(0, &writeRange);
    return true;
}
}
