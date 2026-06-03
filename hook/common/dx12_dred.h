#pragma once

// DX12 Device Removed Extended Data (DRED) diagnostics.
//
// 0x887A0006 (DXGI_ERROR_DEVICE_HUNG) and friends are GPU-side faults. Without
// DRED the only data we get is the bare HRESULT, which is why the x86 DX12
// focus-loss freeze went through 7 guess-based fix attempts. DRED auto-
// breadcrumbs + page-fault output identify the exact command list/queue that
// hung and the faulting GPU virtual address (including recently-freed
// allocations — the smoking gun for stale-backbuffer faults across the
// fullscreen-optimized <-> composited mode transition).
//
// Auto-breadcrumbs must be armed BEFORE the real device is created, so arming
// happens in Wrapped_D3D12CreateDevice. Dumping happens at the existing
// device-removed detection sites.

struct ID3D12Device;

// The hook DLL is built with ThinLTO + `-Wl,--gc-sections`. ArmBeforeDeviceCreation
// has a single call site (Wrapped_D3D12CreateDevice); on x86 the linker GC'd the
// arming body even with __attribute__((used)) (it was kept on x64 but stripped on
// x86), which silently disabled DRED. Exporting the DRED entry points makes them
// unconditional GC roots on PE for BOTH architectures, guaranteeing the arming
// runs. (Verify with: `llvm-strings capture_hook_x86.dll | grep "DX12 DRED: armed"`.)
#define CE_DRED_API __declspec(dllexport)

namespace ce::dx12_dred {

// Whether DRED arming is enabled. Controlled by env var CE_DX12_DRED:
//   unset / "1" / "on" / "true"  -> enabled (default during this diagnosis cycle)
//   "0" / "off" / "false"        -> disabled
CE_DRED_API bool IsEnabled();

// Arm DRED auto-breadcrumbs + page-fault + breadcrumb-context as FORCED_ON.
// Must be called before D3D12CreateDevice creates the device. Idempotent and
// cheap; logs the first successful arm. Returns true if DRED was armed.
CE_DRED_API bool ArmBeforeDeviceCreation();

// On device-removed, query the device's DRED output and log the hung command
// list/queue, the last completed breadcrumb op, breadcrumb context strings, and
// the page-fault VA + existing/recently-freed allocations. Logs at most once per
// device-removed epoch (see ResetDumpEpoch). Safe if device is null or DRED was
// not armed.
CE_DRED_API void DumpOnDeviceRemoved(ID3D12Device* device, const char* reason);

// Re-arm the once-per-epoch dump guard. Call when a fresh device is adopted so a
// later device-removal on the new device can dump again.
CE_DRED_API void ResetDumpEpoch();

// D3D12 debug-layer diagnostics (env CE_DX12_DEBUG_LAYER: "1" = debug layer,
// "2" = debug layer + GPU-based validation; unset/"0" = off, the default).
// The debug layer changes GPU timing and is ONLY for diagnosing the x86 DX12
// Alt+Tab overlay-draw hang — it surfaces the exact resource-state/hazard that
// CE's overlay submission hits during the iflip<->composited mode switch (which
// DRED can only report as a "pure hang"). Must be armed BEFORE device creation.
CE_DRED_API int DebugLayerLevel();

// Enable the D3D12 debug layer (and GPU-based validation at level 2) before the
// game's device is created. Call from the earliest D3D12 hook init. No-op if off.
CE_DRED_API void ArmDebugLayerBeforeDeviceCreation();

// Drain the device's ID3D12InfoQueue and log every stored debug-layer message via
// HookLogImportant, then clear it. No-op if the debug layer is off or the device
// has no info queue. `context` tags the log lines.
CE_DRED_API void DrainDebugLayerMessages(ID3D12Device* device, const char* context);

}  // namespace ce::dx12_dred
