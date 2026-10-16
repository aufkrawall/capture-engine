#pragma once

// GPU for the shared screen-grab D3D11 device (WGC, DXGI duplication and the
// encoder's work device when no inject LUID is known).
//
// D3D11CreateDevice(nullptr) takes DXGI adapter 0, which Windows orders by the
// app's GPU preference: on a hybrid laptop that is normally the integrated GPU
// that drives the panel, so a vendor encoder of the other GPU (NVENC on the
// NVIDIA dGPU) could not open on the device at all. On a multi-GPU desktop the
// captured monitor may also hang off a GPU other than adapter 0, which turns
// every captured frame into a cross-adapter copy and makes DXGI duplication
// unavailable.
//
// The choice: a vendor-specific encoder needs an adapter of its vendor; among
// the eligible adapters the one that drives the target monitor wins (capture
// without a cross-adapter copy), otherwise the first in high-performance order.
// Software adapters are never chosen. With one GPU this is adapter 0 as before.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct IDXGIAdapter1;

namespace ce::media {

inline constexpr uint32_t kVendorNvidia = 0x10DE;
inline constexpr uint32_t kVendorAmd = 0x1002;
inline constexpr uint32_t kVendorIntel = 0x8086;

// The vendor a hardware encoder wrapper is tied to, 0 for vendor-neutral ones
// (Media Foundation, software encoders).
uint32_t EncoderVendorId(const std::string& encoderName);

struct AdapterCandidate {
    LUID luid = {};
    uint32_t vendorId = 0;
    bool software = false;
    bool drivesTargetMonitor = false;
};

struct AdapterChoice {
    // Index into the candidates, or SIZE_MAX when none is usable.
    size_t index = SIZE_MAX;
    const char* reason = "none";
    // The chosen adapter does not drive the target monitor (captured frames
    // cross adapters; DXGI duplication falls back to WGC).
    bool crossAdapterCapture = false;
    // No adapter of the encoder's vendor exists; the encoder will not open.
    bool encoderVendorMissing = false;
};

// `candidates` in high-performance order.
AdapterChoice ChooseScreenGrabAdapter(const std::vector<AdapterCandidate>& candidates, uint32_t encoderVendor);

// Enumerates the adapters and returns the chosen one (owned reference), or
// nullptr to keep the default adapter. `captureMonitor` is the configured
// selector; "auto" and other non-explicit selectors use the primary monitor.
// `log` receives one line describing the decision.
IDXGIAdapter1* SelectScreenGrabAdapter(const std::string& encoderName, const std::string& captureMonitor,
                                       std::string* log);

}  // namespace ce::media
