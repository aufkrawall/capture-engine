#pragma once

#include <windows.h>

// Ownership and safety rules for CE's DXGI flip-queue pacing.
//
// `backbuffer_count` is implemented without changing the physical BufferCount:
// CE requests DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT at creation and
// waits on that object before each Present, so the vsync queue never grows past
// the configured depth.  Both halves describe the swapchain the *application*
// presents through, and both are wrong on a swapchain that only exists as some
// other runtime's transport.
//
// DOOM Eternal (Vulkan) session `20260819_020933` is that failure.  NVIDIA's
// Vulkan ICD implements VkSwapchainKHR on a DXGI flip swapchain it creates and
// presents from a driver-owned thread.  CE saw the DXGI create, added the
// waitable flag, called SetMaximumFrameLatency(1) on it, and then blocked that
// driver thread in WaitForSingleObject(..., INFINITE) inside Present1.  The game
// meanwhile destroyed the Vulkan swapchain from its window thread, where the ICD
// joins its presenter thread (GetExitCodeThread) - which could never exit.  The
// render thread was already blocked in SendMessage to that window thread, so the
// whole process wedged and CE's own freeze watchdog dumped it 26 s later.
//
// CE already decides this question once, evidence-based, in CheckAndInstallHooks
// ("Vulkan layer ownership established, skipping D3D/DXGI hooks").  The pacing
// paths simply did not honour that answer.

namespace ce::present_pacing_policy {

// CE owns D3D presentation policy only while the CE Vulkan layer does not own
// presentation for this process.  When it does, every DXGI swapchain CE can
// reach belongs to the ICD's WSI implementation, not to the game, and the layer
// has already applied vsync mode, image count, and prerender depth on the real
// VkSwapchainKHR.  Pacing the ICD's transport on top is both a second
// application of the same user setting and a block on a thread CE does not own.
inline bool ShouldApplyCePresentationPolicy(bool vulkanLayerOwnsPresentation) {
    return !vulkanLayerOwnsPresentation;
}

// Ceiling for the flip-queue pacing wait.  A pacing wait is a throttle, never a
// lock: CE must not be able to hold a present thread indefinitely no matter what
// the presentation manager does with the waitable object.
//
// The value is a hang ceiling, not a tuning knob, and it is deliberately far
// above every healthy wait: the slowest presentation Windows drives is a 24 Hz
// mode (~41.7 ms per frame) and `backbuffer_count` accepts at most 6, so a
// healthy chain cannot make CE wait more than ~250 ms.  Commit ccbdeac5 fixed
// this same freeze with a 16 ms ceiling and commit dd30a5b6 reverted it to
// INFINITE, because 16 ms is *below* a healthy wait and therefore silently
// escaped the pacing whenever the game was GPU- or vblank-bound.  This ceiling
// keeps the pacing intact and still cannot freeze the process.
constexpr DWORD kFlipQueuePacingWaitCeilingMs = 1000;

// Guards the ceiling against being lowered back into the range of a healthy
// wait, which is how the pacing silently stopped working before.
inline bool IsPacingCeilingAboveHealthyWait(DWORD ceilingMs, DWORD slowestHealthyWaitMs) {
    return ceilingMs > slowestHealthyWaitMs;
}

// The slowest wait a healthy chain can impose: one presentation interval per
// queued frame at the configured depth.
inline DWORD SlowestHealthyPacingWaitMs(DWORD presentationIntervalMs, DWORD configuredQueueDepth) {
    return presentationIntervalMs * configuredQueueDepth;
}

// A waitable object that missed the ceiling once is not a working pacing source.
// Waiting on it again would pay the ceiling on every later present, turning one
// hitch into a permanent stall, so pacing latches off instead.
inline bool ShouldDisablePacingAfterWait(DWORD waitResult) {
    return waitResult != WAIT_OBJECT_0;
}

// Whether the pacing wait may run at all this present.
inline bool ShouldWaitForFlipQueueRoom(bool backbufferCountOverrideActive, bool vulkanLayerOwnsPresentation,
                                       bool pacingLatchedOff) {
    return backbufferCountOverrideActive && ShouldApplyCePresentationPolicy(vulkanLayerOwnsPresentation) &&
           !pacingLatchedOff;
}

} // namespace ce::present_pacing_policy
