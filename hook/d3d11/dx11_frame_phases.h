#pragma once

// CE's own work in a DX11 frame, split for the perf CSV (overlay_us, capture_us, overlay_gpu_us).
// The frame's present hook - CE's swapchain wrapper or the DXGI Present detour - takes them after
// the frame processing and adds its own present-call timing. Zero while the perf CSV is off: the
// clocks are read only then.

#include <cstdint>

struct DX11FramePhases {
    int32_t overlayUs = 0;     // CPU: CE's overlay draw
    int32_t captureUs = 0;     // CPU: the recording's frame copy and any screenshot
    int32_t overlayGpuUs = 0;  // GPU: the overlay's draw, from a few frames back
};

// The phases of the last DX11 frame this thread processed; reading resets them.
DX11FramePhases TakeDX11FramePhases();
