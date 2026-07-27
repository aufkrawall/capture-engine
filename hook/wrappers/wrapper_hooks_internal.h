/**
 * Wrapper entry points — shared state (internal)
 *
 * wrapper_hooks.cpp holds the DXGI factory exports and defines this state;
 * wrapper_hooks_devices.cpp holds the device-creation exports and reads it.
 *
 * The D3D10 create scope is defined here rather than in a sibling .cpp because
 * it guards every D3D10 creation path in both translation units and the x86
 * hook DLL is built without LTO.
 */

#pragma once

#include <windows.h>

#include <atomic>

#include <dxgi.h>

// Defined in wrapper_hooks.cpp.
extern bool g_WrappersActive;
extern std::atomic<bool> g_D3D11Or10DeviceCreated;

// Applies the configured backbuffer-count override to a swap-chain description
// created through D3D11CreateDeviceAndSwapChain. Defined in wrapper_hooks.cpp.
bool ApplyD3D11CreateDeviceSwapChainBackbufferOverride(DXGI_SWAP_CHAIN_DESC& desc);

namespace ce::wrapper_hooks {

inline thread_local int s_D3D10CreateDepth = 0;

class D3D10CreateScope {
public:
    D3D10CreateScope() {
        ++s_D3D10CreateDepth;
    }
    ~D3D10CreateScope() {
        --s_D3D10CreateDepth;
    }

    D3D10CreateScope(const D3D10CreateScope&) = delete;
    D3D10CreateScope& operator=(const D3D10CreateScope&) = delete;
};

inline bool IsInD3D10CreateScope() {
    return s_D3D10CreateDepth > 0;
}

}  // namespace ce::wrapper_hooks

using ce::wrapper_hooks::D3D10CreateScope;
using ce::wrapper_hooks::IsInD3D10CreateScope;
