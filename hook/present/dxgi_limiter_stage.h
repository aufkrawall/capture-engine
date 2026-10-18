#pragma once

#include <dxgi.h>

#include "hook/pacing/fps_limiter_policy.h"

namespace DXGIShared {

// The FPS limiter stage of a DXGI Present: g_SharedFpsLimiter.Apply() (capture sync, general cap)
// and the frame-latency overrides, a no-op without the IPC client. While the limiter blocks, a
// D3D10/11 frame the game recorded is already queued on the GPU (FpsLimiter::PreWaitSubmission):
// the swapchain device's immediate context is flushed once, right before the wait, as Present would
// flush it after the wait. D3D12 swapchains have nothing to submit.
void ApplyFpsLimiterStage(IDXGISwapChain* pSwapChain, bool allowPostPresentReflexCadence = false,
                          ce::fps_limiter_policy::PresentSite site =
                              ce::fps_limiter_policy::PresentSite::kDuplicateProne);

// The submission itself: flushes the D3D10/11 immediate context of `swapChain` (an IDXGISwapChain*).
void SubmitD3D1xRecordedWork(void* swapChain);

}  // namespace DXGIShared
