// Hardware-accelerated GPU scheduling probe.
//
// DLSS Frame Generation and NGX-compatible providers (including ANR) require HAGS. When it is off
// the driver invalidates the device instead of returning a clean error, so the transport reports it
// as an explicit prerequisite before any provider call.
//
// The state is queried from the rendering adapter itself (KMTQAITYPE_WDDM_2_7_CAPS). A missing
// registry override does not mean disabled, and the OS default policy bit does not mean enabled:
// only the live per-GPU HwSchEnabled bit authorizes NGX initialization.
#pragma once

#include <windows.h>
// This SDK defines NTSTATUS only in winternl.h/ntdef.h, not in winnt.h; d3dkmthk.h needs it.
#include <winternl.h>

#include <d3d12.h>
#include <d3dkmthk.h>

#include <cstdio>
#include <string>

namespace bigscale
{
namespace scheduling_detail
{
inline std::string StatusText(long status)
{
    char text[16]{};
    std::snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(status));
    return text;
}
}

// True only when the bound adapter reports hardware scheduling as currently enabled. Every failure
// of the query itself is a false result with the failing operation and native status in `reason`.
inline bool HardwareSchedulingEnabled(ID3D12Device* device, std::string& reason)
{
    if (device == nullptr)
    {
        reason = "hardware-accelerated GPU scheduling could not be queried: no D3D12 rendering adapter is bound";
        return false;
    }
    D3DKMT_OPENADAPTERFROMLUID open{};
    open.AdapterLuid = device->GetAdapterLuid();
    const NTSTATUS opened = D3DKMTOpenAdapterFromLuid(&open);
    if (opened < 0)
    {
        reason = "hardware-accelerated GPU scheduling could not be queried: D3DKMTOpenAdapterFromLuid failed (" +
            scheduling_detail::StatusText(opened) + ")";
        return false;
    }
    D3DKMT_WDDM_2_7_CAPS caps{};
    D3DKMT_QUERYADAPTERINFO query{};
    query.hAdapter = open.hAdapter;
    query.Type = KMTQAITYPE_WDDM_2_7_CAPS;
    query.pPrivateDriverData = &caps;
    query.PrivateDriverDataSize = sizeof(caps);
    const NTSTATUS queried = D3DKMTQueryAdapterInfo(&query);
    D3DKMT_CLOSEADAPTER close{};
    close.hAdapter = open.hAdapter;
    D3DKMTCloseAdapter(&close);
    if (queried < 0)
    {
        reason = "hardware-accelerated GPU scheduling could not be queried: "
                 "D3DKMTQueryAdapterInfo(KMTQAITYPE_WDDM_2_7_CAPS) failed (" +
            scheduling_detail::StatusText(queried) + ")";
        return false;
    }
    if (!caps.HwSchEnabled)
    {
        reason = "frame generation requires hardware-accelerated GPU scheduling "
                 "(Windows Settings > System > Display > Graphics > Change default graphics settings)";
        return false;
    }
    return true;
}
}
