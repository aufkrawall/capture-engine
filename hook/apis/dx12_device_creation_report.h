#pragma once

#include <windows.h>

// One-shot forensics for a failing D3D12 device creation inside the injected process.
//
// Hardware-device failures can be expanded into entry-integrity and adapter
// capability evidence. The temp-swapchain bootstrap intentionally uses WARP
// and must not call this report: its hardware probes would re-enter the vendor
// UMD during the application's graphics startup.
namespace ce::dx12_device_creation_report {

// Emit the report for `observedHr`, at most `kMaxDeviceCreationReports` times per process
// and at most once per distinct HRESULT. Safe to call from the hook thread's service pass;
// it creates no device and leaves no D3D12 state behind.
void ReportDeviceCreationFailure(HRESULT observedHr, const char* callSite);

// True while the temp-swapchain route should still pay for a device-creation attempt.
// Terminal WARP failures stop being retried after a small budget.
bool ShouldAttemptTempDeviceCreation();

// Record the outcome of one temp-swapchain device-creation attempt.
void NoteTempDeviceCreationResult(HRESULT hr);

}  // namespace ce::dx12_device_creation_report
