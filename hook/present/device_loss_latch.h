#pragma once

#include <windows.h>
#include <dxgi.h>

#include <cstdint>

// The process-wide device-loss latch (DXGIShared::g_SharedState.deviceRemovedFatal).
//
// A Present that reports the device lost, or CE's own D3D12 device check, latches
// it: CE's Present processing passes through and the freeze watchdog's heartbeat
// stops, so a game spinning on a dead device gets a freeze dump instead of CE work
// submitted into a torn-down driver.
//
// The latch describes the device that was lost, not the process. A game recovers
// by presenting on a new device, possibly through another API: Heroes of the Storm
// on DXUP, logs/20261028_092712, lost its D3D12 device and went on presenting a
// plain D3D11 swapchain at 60 fps on another thread. Only a new D3D12 device used to
// clear the latch, so every later heartbeat was suppressed and the watchdog dumped a
// "Render thread frozen for 30 seconds" while the game ran. A Present on a lost
// device fails with DXGI_ERROR_DEVICE_REMOVED, so a Present that succeeds after the
// latch proves its device alive, and clears it.
namespace ce::device_loss {

inline bool IsDeviceLostPresentResult(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG;
}

enum class PresentResultTransition {
    kNone,
    kLatch,
    kRecover,
};

inline PresentResultTransition DecidePresentResultTransition(bool latched, HRESULT presentResult) {
    if (IsDeviceLostPresentResult(presentResult)) {
        return latched ? PresentResultTransition::kNone : PresentResultTransition::kLatch;
    }
    if (latched && SUCCEEDED(presentResult)) {
        return PresentResultTransition::kRecover;
    }
    return PresentResultTransition::kNone;
}

// Transition log lines: the first few of each kind, then every 64th, so a process
// that keeps alternating between a dead and a live swapchain cannot flood the log.
inline bool ShouldLogTransition(uint64_t transitionCount) {
    return transitionCount <= 8 || (transitionCount % 64) == 0;
}

}  // namespace ce::device_loss
