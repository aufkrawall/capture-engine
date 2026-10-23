#pragma once

#include <cstdint>

// ProcessFrame's rules for swapchains a frame-generation runtime created (ce::fg_presenter): a runtime-owned output
// chain (XeFG) is a swapchain identity of its own, rebuilt at once on the queue its creation proved, and no
// runtime's chain is ever drawn from a queue CE merely guessed.
namespace ce::dx12_overlay_policy {

// A registered runtime-presented chain whose registration lifetime is not the one ProcessFrame tracks is a new
// swapchain, also at the previous chain's address (the game's released chain, or XeFG's previous one).
inline bool IsNewRuntimePresentedSwapchainLifetime(uint64_t presenterLifetime, uint64_t trackedPresenterLifetime) {
    return presenterLifetime != 0 && presenterLifetime != trackedPresenterLifetime;
}

// A runtime-presented chain's queue was proven at its creation (and every ResizeBuffers1): the overlay is rebuilt on
// it at once, as after a finalized FFX takeover, never held for an FG transition cooldown.
inline bool ShouldReinitOverlayImmediatelyOnRuntimePresentedSwapchain(bool runtimePresentedChain,
                                                                     bool creationProvenOutputQueue,
                                                                     bool deviceRemoved) {
    return runtimePresentedChain && creationProvenOutputQueue && !deviceRemoved;
}

// A Streamline FG OFF observed on a runtime-presented chain (the DLSS-G signal turned out to be a front once XeFG
// registered its chain): CE's render targets there are that chain's own, built for its lifetime, never Streamline's
// back buffers. Tearing them down blanked XeFG's first output (FlowXeFGSwitch.AStreamlineFGSignalBesideXeFGIsAFront).
inline bool ShouldKeepOverlayLiveAcrossStreamlineOffOnRuntimePresentedSwapchain(bool streamlineTurnedOff,
                                                                                bool runtimePresentedChainTracked,
                                                                                bool overlayInit, bool syncInit,
                                                                                bool deviceRemoved) {
    return streamlineTurnedOff && runtimePresentedChainTracked && overlayInit && syncInit && !deviceRemoved;
}

// No queue was captured for the presented swapchain and the routing would fall back to the last direct queue the
// game used. Only the swapchain's own queue may write its back buffers; any other removes the device with
// DXGI_ERROR_ACCESS_DENIED. A readable queue decides by identity. An unreadable one is a guess, which a swapchain a
// frame-generation runtime created never gets: OptiScaler's FFX swapchain lost its staged queue to a DLSS-G "enable"
// that was a front, and the guessed game queue removed the device on its first overlay frame
// (20261023_221541: 0x887A002B).
enum class UncapturedQueueFallback : uint8_t { kDraw, kSkipQueueMismatch, kSkipRuntimeChainUnproven };
inline UncapturedQueueFallback DecideUncapturedQueueFallback(bool ownQueueReadable, bool ownQueueIsFallbackQueue,
                                                             bool createdByFrameGenerationRuntime) {
    if (ownQueueReadable) {
        return ownQueueIsFallbackQueue ? UncapturedQueueFallback::kDraw : UncapturedQueueFallback::kSkipQueueMismatch;
    }
    return createdByFrameGenerationRuntime ? UncapturedQueueFallback::kSkipRuntimeChainUnproven
                                           : UncapturedQueueFallback::kDraw;
}

}  // namespace ce::dx12_overlay_policy
