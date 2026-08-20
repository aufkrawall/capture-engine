#pragma once

#include <windows.h>
#include <cstddef>

// Module lifetime for code CE reaches into.
//
// CE captures raw code pointers out of the graphics runtime modules: export
// addresses it caches in long-lived globals and hands back through the
// GetProcAddress detour, entry bytes it inspects before building a bypass
// trampoline, and entry points it inline-patches and calls. A plain
// GetModuleHandle handle does not own the module. Any component in the process
// may FreeLibrary it, and the loader then runs DLL_PROCESS_DETACH and unmaps
// the image while CE still holds pointers into it.
//
// The Witcher 3 startup crash was exactly that, twice over. A transient probe
// load of d3d11.dll made CheckAndInstallHooks commit to the DX11 install, and
// roughly a second later the probe's owner released the module:
//   * session 20260820_031021 / witcher3crash - DX11Hook::Init faulted reading
//     the first byte of d3d11!D3D11CreateDeviceAndSwapChain (0x7ffd84422470)
//     out of the freshly unmapped image.
//   * sessions 20260820_023643 / 20260820_031021 - the unload landed a moment
//     later instead, inside CE's temp D3D11CreateDeviceAndSwapChain call:
//     d3d11's detach had already torn down CCreateDeviceCache and dropped the
//     NVIDIA UMD, so nvwgf2umx entered an all-zero critical section
//     (RtlpWaitOnCriticalSection, DebugInfo == nullptr).
//
// Pinning is the correct lifetime for the named D3D/DXGI runtime modules -
// d3d11, d3d10, d3d10_1, d3d12, dxgi, d3d9, ddraw - because CE caches their
// export addresses in process-lifetime globals, hands them back through the
// GetProcAddress detour, and never withdraws the entry patches taken alongside
// them. Those images have no unload/reload cycle in any design. Resolving
// through the loader is also what keeps this free of a check-then-use window:
// GetModuleHandleEx runs under the loader lock and either pins a live module or
// reports none.
//
// Pinning is deliberately NOT applied to every hook target. Streamline, NGX and
// FFX plugins are loaded and unloaded for real while frame generation switches
// modes, and CE answers that with scoped holds plus teardown latches
// (streamline_hook_resolve.cpp). Code on those paths validates with
// IsReadableCode instead, which changes no third-party lifetime.
namespace ce::module_pin {

// Resolve `moduleName` and pin it. Returns nullptr when the module is not
// loaded (or is already being unloaded), in which case the caller must not
// proceed to resolve or patch anything in it.
HMODULE PinByName(const char* moduleName);

// Pin the image that owns `address`. Returns nullptr when the address is not
// backed by a mapped image - CE's own trampoline pools and other allocated
// thunks are not modules and cannot be pinned; use IsReadableCode for those.
HMODULE PinOwnerOfAddress(const void* address);

// True when `count` bytes at `address` are committed, readable, executable
// memory. This is the guard for a code pointer whose owner CE never resolved
// itself - a shared "original" slot another hook wrote, or a loader-injected
// proxy's export. Pinning remains the invariant; this only keeps CE from
// dereferencing a pointer that has no owning module to pin.
bool IsReadableCode(const void* address, size_t count);

}  // namespace ce::module_pin
