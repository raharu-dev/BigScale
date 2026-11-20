// Shared CPU readback primitives for frame-generation planes.
//
// The diagnostics capture and the offline checks both need the exact bytes the GPU holds in a
// frame-generation resource. Recording a copy into a READBACK buffer wrapped in the D3D12 copyable
// footprint is the only portable path; tightening the rows here keeps every caller's on-disk and
// in-memory layout identical (row 0 first, four bytes per pixel or scalar).
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bigscale
{
struct FgReadbackPlane
{
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

// Records the barrier into COPY_SOURCE, the copy, and the barrier back into `sourceState`. The
// caller owns submission and completion; nothing here waits on the GPU.
bool FgRecordReadback(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* source,
    D3D12_RESOURCE_STATES sourceState, FgReadbackPlane& plane, std::string& reason);

// Maps the readback buffer and copies its rows without padding into `bytes`. Accepts the four-byte
// formats the frame-generation path produces: the RGBA8 family, R32_FLOAT and R16G16_FLOAT.
bool FgMapTightRows(const FgReadbackPlane& plane, std::vector<uint8_t>& bytes, std::string& reason);
}
