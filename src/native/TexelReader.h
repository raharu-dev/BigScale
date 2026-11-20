// BigScale — raw texel readback for frame diagnostics.
//
// Reads individual texels straight out of an engine texture through a staging copy. No shader view
// is created and no pipeline slot is bound, so neither the renderer's state nor any hook that
// rewrites SRVs can influence what is read. That is what makes the values usable as evidence about
// what the renderer actually wrote.
//
// Formats are decoded explicitly; a format the table does not cover fails with the static reason
// "format" instead of being reported as zero, so a missing sample is never mistaken for content.
#pragma once

#include <d3d11.h>

namespace bigscale
{
// Decodes one texel already copied to CPU memory. `texel` points at the first byte of the texel in
// that format's native layout. Every channel the format does not carry is written as 0, and alpha
// defaults to 1. Returns false for a format the table does not cover.
bool DecodeTexel(DXGI_FORMAT format, const void* texel, float out[4]);

// Reads `count` texels of one array slice. `points` holds {x, y} pairs in texture space. false plus a
// static reason string on an unsupported format, MSAA, an out-of-range point/slice or any D3D
// failure; `out` is zeroed first on every path so a failure can never look like dark content.
bool ReadTexels(ID3D11DeviceContext* context, ID3D11Texture2D* texture, UINT slice,
                const UINT (*points)[2], UINT count, float (*out)[4], const char** reason);
} // namespace bigscale
