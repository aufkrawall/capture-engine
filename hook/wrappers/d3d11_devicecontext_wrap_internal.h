/**
 * D3D11 device-context wrapper — shared internals
 *
 * d3d11_devicecontext_wrap.cpp, d3d11_devicecontext_forward.cpp and
 * d3d11_devicecontext_get.cpp implement one wrapper between them. This header
 * carries what they share. The stage constants and the forwarding scope are
 * defined here rather than in a sibling .cpp on purpose: the x86 hook DLL is
 * built without LTO, so anything on the per-draw or per-bind path must stay
 * inlinable across these translation units.
 */

#pragma once

#include "d3d11_devicecontext_wrap.h"

#include <atomic>
#include <cstdint>
#include "hook/d3d11/dx11_hook.h"
#include "hook/fg/fg_detection.h"

inline constexpr UINT kWrapperStagePS = 0;
inline constexpr UINT kWrapperStageVS = 1;
inline constexpr UINT kWrapperStageGS = 2;
inline constexpr UINT kWrapperStageHS = 3;
inline constexpr UINT kWrapperStageDS = 4;
inline constexpr UINT kWrapperStageCS = 5;

class DX11WrapperForwardingScope {
public:
    DX11WrapperForwardingScope() {
        DX11Hook_BeginWrapperContextForwarding();
    }

    ~DX11WrapperForwardingScope() {
        DX11Hook_EndWrapperContextForwarding();
    }

    DX11WrapperForwardingScope(const DX11WrapperForwardingScope&) = delete;
    DX11WrapperForwardingScope& operator=(const DX11WrapperForwardingScope&) = delete;
};
