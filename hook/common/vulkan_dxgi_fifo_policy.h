#pragma once

#include <cstdint>
#include <string_view>

namespace ce::vulkan_dxgi_fifo_policy {

inline constexpr uint32_t kDxgiPresentAllowTearing = 0x200u;

// **Retired: this path is never armed, and the reason is the bug it caused.**
//
// It existed because a Portal RTX session presented above the refresh rate on a
// swapchain CE had created as FIFO, so CE forced `SyncInterval=1` and cleared
// `DXGI_PRESENT_ALLOW_TEARING` on the final system present. That rate escape
// was hardware present metering, which the Vulkan layer now declines at the
// device level (`vulkan_present_metering_policy.h`), so the symptom this path
// answered no longer has a cause.
//
// What it left behind was worse than what it fixed. Session `20260830_185703`
// shows NVIDIA's WSI choosing the DXGI parameters *per present* on one and the
// same FIFO swapchain: `final Present1 #1024 force=1 SyncInterval=1->1
// Flags=0x0->0x0`, then `#2048 force=1 SyncInterval=0->1 Flags=0x200->0x0`.
// Those are not a driver that forgot to synchronize - they are how variable
// refresh is driven on Windows. A flip that the display is meant to stretch has
// to be presented with `SyncInterval=0` and tearing allowed; the panel then
// refreshes on the flip instead of the flip waiting for the panel. Forcing
// every present to `SyncInterval=1` and clearing the flag replaces variable
// refresh with fixed-rate vertical-blank pacing, which is exactly why the same
// title is smooth when the *driver* forces the mode - `vsync_mode` is then not
// `fifo`, so this path never armed - and judders when CaptureEngine does.
// Under frame generation the rendered frame period is not constant, so a fixed
// grid turns that variation into visible stutter while every present-side frame
// time stays flat.
//
// `VK_PRESENT_MODE_FIFO_KHR` on the swapchain is the whole contract, and the
// same WSI honours it: the application's own FIFO swapchain in that session was
// presented the same adaptive way. CE has no business restating it one layer
// down in terms the display no longer uses.
inline bool ShouldArmFinalDxgiPresent(bool vulkanLayerModuleLoaded, std::string_view vsyncMode) {
    (void)vulkanLayerModuleLoaded;
    (void)vsyncMode;
    return false;
}

inline bool ShouldForceFinalDxgiFifo(bool fifoRequested, bool vulkanPresentationActive, bool hookShuttingDown) {
    return fifoRequested && vulkanPresentationActive && !hookShuttingDown;
}

// DXGI_PRESENT_ALLOW_TEARING is invalid with a non-zero synchronization
// interval. SyncInterval=1 is the DXGI contract for presenting at the next
// vertical blank.
inline bool ApplyFinalDxgiFifoParameters(bool forceFifo, uint32_t& syncInterval, uint32_t& flags) {
    if (!forceFifo)
        return false;

    const uint32_t previousSyncInterval = syncInterval;
    const uint32_t previousFlags = flags;
    syncInterval = 1;
    flags &= ~kDxgiPresentAllowTearing;
    return syncInterval != previousSyncInterval || flags != previousFlags;
}

}  // namespace ce::vulkan_dxgi_fifo_policy
