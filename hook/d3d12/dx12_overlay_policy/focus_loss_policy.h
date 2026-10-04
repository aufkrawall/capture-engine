#pragma once

#include <windows.h>
#include <cstdint>

struct ID3D12CommandQueue;
struct ID3D12Fence;

namespace ce::dx12_overlay_policy {

struct D3D12DeferredOverlaySignalFlushInfo {
    bool hadDeferredSignal = false;
    bool hasFence = false;
    bool hasFenceEvent = false;
    bool signalSucceeded = false;
    HRESULT signalHr = S_OK;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;
    UINT64 completedValue = 0;
};

// Deferred-signal flush failure accounting. currentFenceValue is the value every
// later overlay wait blocks on, so it may only hold a value the fence will
// actually REACH: a failed Signal never reaches the fence, and committing its
// value would leave currentFenceValue permanently ahead of the fence — every
// subsequent wait then runs to its liveness timeout against an unreachable
// value. Never rewind the accounting either; a stale/out-of-order deferred value
// must not pull it below values already signaled.
inline bool ShouldCommitDeferredOverlayFenceSignalValue(bool signalSucceeded, UINT64 deferredValue,
                                                        UINT64 currentFenceValue) {
    return signalSucceeded && deferredValue > currentFenceValue;
}

struct D3D12FocusLossOverlayFenceWaitContext {
    const char* presentName = nullptr;
    int callCount = 0;
    bool isD3D12Swapchain = false;
    bool isFullscreen = false;
    bool processHasForeground = true;
    bool isIconic = false;
    bool hasZeroSize = false;
    bool presentSucceeded = false;
    bool presentDeviceLost = false;
    bool frameGenerationActive = false;
    bool runtimeOwnedPresentation = false;
    bool usingDedicatedQueue = false;
    HWND foregroundWindow = nullptr;
    DWORD foregroundPid = 0;
    HWND gameWindow = nullptr;
    DWORD processId = 0;
    UINT syncInterval = 0;
    UINT presentFlags = 0;
    HRESULT presentHr = S_OK;
};

// Parameter descriptor for immediate focus-loss overlay fence signaling decisions.
struct D3D12FocusLossImmediateFenceDesc {
    bool isWrappedD3D12Present = false;
    bool isFullscreen = false;
    bool processHasForeground = true;
    bool isIconic = false;
    bool hasZeroSize = false;
    bool overlaySubmitSucceeded = false;
    bool deviceLost = false;
    bool frameGenerationActive = false;
    bool runtimeOwnedPresentation = false;
    bool usingDedicatedQueue = false;
    bool steamDeferredOverlaySubmit = false;
    bool hasFence = false;
    bool hasFenceEvent = false;
    bool hasQueue = false;
    UINT64 fenceValue = 0;
};

// Parameter descriptor for non-presentable swapchain overlay backbuffer hold decisions.
struct D3D12NonPresentableSwapchainHoldDesc {
    bool isWrappedD3D12Present = false;
    bool isFullscreen = false;
    bool isOccluded = false;
    bool isIconic = false;
    bool hasZeroSize = false;
    bool frameGenerationActive = false;
    bool runtimeOwnedPresentation = false;
    bool usingDedicatedQueue = false;
    bool steamDeferredOverlaySubmit = false;
    bool deviceLost = false;
    bool hasQueue = false;
};

inline bool ShouldWaitForD3D12FocusLossPostPresentOverlayFence(bool isD3D12Swapchain, bool isFullscreen,
                                                               bool processHasForeground, bool isIconic,
                                                               bool hasZeroSize, bool presentSucceeded,
                                                               bool presentDeviceLost, bool frameGenerationActive,
                                                               bool runtimeOwnedPresentation, bool usingDedicatedQueue,
                                                               bool hadDeferredOverlaySignal, bool signalSucceeded,
                                                               bool hasFence, bool hasFenceEvent, UINT64 fenceValue) {
    return isD3D12Swapchain && !isFullscreen && !processHasForeground && !isIconic && !hasZeroSize &&
           presentSucceeded && !presentDeviceLost && !frameGenerationActive && !runtimeOwnedPresentation &&
           !usingDedicatedQueue && hadDeferredOverlaySignal && signalSucceeded && hasFence && hasFenceEvent &&
           fenceValue != 0;
}

inline bool ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
    const D3D12FocusLossOverlayFenceWaitContext& ctx,
    const D3D12DeferredOverlaySignalFlushInfo& info) {
    return ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        ctx.isD3D12Swapchain, ctx.isFullscreen, ctx.processHasForeground, ctx.isIconic, ctx.hasZeroSize,
        ctx.presentSucceeded, ctx.presentDeviceLost, ctx.frameGenerationActive, ctx.runtimeOwnedPresentation,
        ctx.usingDedicatedQueue, info.hadDeferredSignal, info.signalSucceeded, info.hasFence, info.hasFenceEvent,
        info.fenceValue);
}

inline bool ShouldSignalD3D12FocusLossOverlayFenceImmediately(const D3D12FocusLossImmediateFenceDesc& desc) {
    return desc.isWrappedD3D12Present && !desc.isFullscreen && !desc.processHasForeground && !desc.isIconic &&
           !desc.hasZeroSize && desc.overlaySubmitSucceeded && !desc.deviceLost && !desc.frameGenerationActive &&
           !desc.runtimeOwnedPresentation && !desc.usingDedicatedQueue && !desc.steamDeferredOverlaySubmit &&
           desc.hasFence && desc.hasFenceEvent && desc.hasQueue && desc.fenceValue != 0;
}

inline bool ShouldSignalD3D12FocusLossOverlayFenceImmediately(
    bool isWrappedD3D12Present, bool isFullscreen, bool processHasForeground, bool isIconic,
    bool hasZeroSize, bool overlaySubmitSucceeded, bool deviceLost, bool frameGenerationActive,
    bool runtimeOwnedPresentation, bool usingDedicatedQueue, bool steamDeferredOverlaySubmit,
    bool hasFence, bool hasFenceEvent, bool hasQueue, UINT64 fenceValue) {
    const D3D12FocusLossImmediateFenceDesc desc{
        isWrappedD3D12Present, isFullscreen, processHasForeground, isIconic,
        hasZeroSize, overlaySubmitSucceeded, deviceLost, frameGenerationActive,
        runtimeOwnedPresentation, usingDedicatedQueue, steamDeferredOverlaySubmit,
        hasFence, hasFenceEvent, hasQueue, fenceValue};
    return ShouldSignalD3D12FocusLossOverlayFenceImmediately(desc);
}

inline bool ShouldWaitForD3D12FocusLossImmediateOverlayFence(bool immediateFencePolicyAccepted, bool signalSucceeded,
                                                             bool hasFence, bool hasFenceEvent, bool hasQueue,
                                                             UINT64 fenceValue) {
    return immediateFencePolicyAccepted && signalSucceeded && hasFence && hasFenceEvent && hasQueue && fenceValue != 0;
}

inline bool ShouldRequestImmediateDumpForD3D12FocusLossImmediateFenceWait(bool fenceWaitCompleted,
                                                                          bool dumpAlreadyRequested) {
    return !fenceWaitCompleted && !dumpAlreadyRequested;
}

// --- DescFree overlay UPLOAD-ring per-slot GPU-completion guard ---------------
inline uint64_t DecideOverlayUploadSlotGuardValue(bool fgActive, bool hasOverlayFence, uint64_t currentFenceValue) {
    if (fgActive || !hasOverlayFence) {
        return 0;
    }
    return currentFenceValue + 1;
}

inline bool IsOverlayUploadSlotInFlight(uint64_t slotGuardFenceValue, uint64_t gpuCompletedFenceValue) {
    return slotGuardFenceValue != 0 && gpuCompletedFenceValue < slotGuardFenceValue;
}

inline bool ShouldRecordDescFreeFontUpload(bool uploadPending, bool hasDefaultFontBuffer, bool hasUploadBuffer) {
    return uploadPending && hasDefaultFontBuffer && hasUploadBuffer;
}

inline bool ShouldUseTextureDx12OverlayBackendForProcess(bool is32BitProcess) {
    return is32BitProcess;
}

inline bool ShouldUseSolidDx12TextGeometryForProcess(bool is32BitProcess) {
    return is32BitProcess;
}

inline bool ShouldHoldD3D12OverlayBackbufferWorkForNonPresentableSwapchain(
    const D3D12NonPresentableSwapchainHoldDesc& desc) {
    if (!desc.isWrappedD3D12Present || desc.isFullscreen) {
        return false;
    }
    const bool notPresentable = desc.isOccluded || desc.isIconic || desc.hasZeroSize;
    return notPresentable && !desc.frameGenerationActive && !desc.runtimeOwnedPresentation &&
           !desc.usingDedicatedQueue && !desc.steamDeferredOverlaySubmit && !desc.deviceLost && desc.hasQueue;
}

inline bool ShouldHoldD3D12OverlayBackbufferWorkForNonPresentableSwapchain(
    bool isWrappedD3D12Present, bool isFullscreen, bool isOccluded, bool isIconic, bool hasZeroSize,
    bool frameGenerationActive, bool runtimeOwnedPresentation, bool usingDedicatedQueue,
    bool steamDeferredOverlaySubmit, bool deviceLost, bool hasQueue) {
    const D3D12NonPresentableSwapchainHoldDesc desc{
        isWrappedD3D12Present, isFullscreen, isOccluded, isIconic, hasZeroSize,
        frameGenerationActive, runtimeOwnedPresentation, usingDedicatedQueue,
        steamDeferredOverlaySubmit, deviceLost, hasQueue};
    return ShouldHoldD3D12OverlayBackbufferWorkForNonPresentableSwapchain(desc);
}

inline bool ShouldRequestImmediateDumpForD3D12FocusTransitionDeviceRemoval(bool deviceLost,
                                                                           bool focusTransitionRecentlyActive,
                                                                           bool dumpAlreadyRequested) {
    return deviceLost && focusTransitionRecentlyActive && !dumpAlreadyRequested;
}

inline bool IsD3D12FocusTransitionTelemetryActive(bool isWindowed, int transitionFramesRemaining,
                                                  bool frameGenerationActive, bool runtimeOwnedPresentation,
                                                  bool usingDedicatedQueue, bool steamDeferredOverlaySubmit,
                                                  bool deviceLost, bool hasQueue) {
    return isWindowed && transitionFramesRemaining > 0 && !frameGenerationActive && !runtimeOwnedPresentation &&
           !usingDedicatedQueue && !steamDeferredOverlaySubmit && !deviceLost && hasQueue;
}

inline bool ShouldHoldD3D12FocusLossOverlayDrawForPendingFence(bool processHasForeground, bool hasPendingFocusLossFence,
                                                               bool pendingFenceComplete) {
    return !processHasForeground && hasPendingFocusLossFence && !pendingFenceComplete;
}

inline bool ShouldHoldD3D12FocusLossBackbufferWorkForPendingFence(bool processHasForeground,
                                                                  bool hasPendingFocusLossFence,
                                                                  bool pendingFenceComplete) {
    return ShouldHoldD3D12FocusLossOverlayDrawForPendingFence(processHasForeground, hasPendingFocusLossFence,
                                                              pendingFenceComplete);
}

}  // namespace ce::dx12_overlay_policy
