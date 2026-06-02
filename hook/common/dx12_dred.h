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

namespace ce::dx12_dred {

// Whether DRED arming is enabled. Controlled by env var CE_DX12_DRED:
//   unset / "1" / "on" / "true"  -> enabled (default during this diagnosis cycle)
//   "0" / "off" / "false"        -> disabled
bool IsEnabled();

// Arm DRED auto-breadcrumbs + page-fault + breadcrumb-context as FORCED_ON.
// Must be called before D3D12CreateDevice creates the device. Idempotent and
// cheap; logs the first successful arm. Returns true if DRED was armed.
bool ArmBeforeDeviceCreation();

// On device-removed, query the device's DRED output and log the hung command
// list/queue, the last completed breadcrumb op, breadcrumb context strings, and
// the page-fault VA + existing/recently-freed allocations. Logs at most once per
// device-removed epoch (see ResetDumpEpoch). Safe if device is null or DRED was
// not armed.
void DumpOnDeviceRemoved(ID3D12Device* device, const char* reason);

// Re-arm the once-per-epoch dump guard. Call when a fresh device is adopted so a
// later device-removal on the new device can dump again.
void ResetDumpEpoch();

}  // namespace ce::dx12_dred
