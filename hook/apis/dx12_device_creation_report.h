#pragma once

#include <windows.h>

// One-shot forensics for a failing D3D12 device creation inside the injected process.
//
// The temp-swapchain bootstrap is usually the first thing in a process to call
// `D3D12CreateDevice`, which makes it the first thing to notice that D3D12 is broken here -
// often seconds before the game's own renderer reaches the same conclusion and aborts. That
// head start is only worth something if the log says *why*, so a failure emits an entry
// integrity check, a per-adapter feature-level matrix, an unpatched-body retry, and a
// verdict, instead of one bare HRESULT repeated until the process dies.
namespace ce::dx12_device_creation_report {

// Emit the report for `observedHr`, at most `kMaxDeviceCreationReports` times per process
// and at most once per distinct HRESULT. Safe to call from the hook thread's service pass;
// it creates no device and leaves no D3D12 state behind.
void ReportDeviceCreationFailure(HRESULT observedHr, const char* callSite);

// True while the temp-swapchain route should still pay for a device-creation attempt.
// Terminal failures stop being retried after a small budget: each attempt maps and unmaps
// the vendor UMD inside the game's startup for a result that cannot change.
bool ShouldAttemptTempDeviceCreation();

// Record the outcome of one temp-swapchain device-creation attempt.
void NoteTempDeviceCreationResult(HRESULT hr);

}  // namespace ce::dx12_device_creation_report
