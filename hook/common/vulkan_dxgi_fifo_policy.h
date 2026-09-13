#pragma once

#include <cstdint>
#include <string_view>

#include "../vulkan_layer/vulkan_present_metering_policy.h"

namespace ce::vulkan_dxgi_fifo_policy {

inline constexpr uint32_t kDxgiPresentTest = 0x1u;
inline constexpr uint32_t kDxgiPresentDoNotSequence = 0x2u;
inline constexpr uint32_t kDxgiPresentRestart = 0x4u;
inline constexpr uint32_t kDxgiPresentDoNotWait = 0x8u;
inline constexpr uint32_t kDxgiPresentAllowTearing = 0x200u;

// The last vertical-blank contract CE can still state, and the only one that is
// a vertical blank rather than a clock.
//
// NVIDIA's Vulkan WSI terminates in a DXGI flip swapchain. `SyncInterval=1`
// with `DXGI_PRESENT_ALLOW_TEARING` cleared is that flip presenting at the next
// vertical blank - the DXGI spelling of V-Sync, and on a G-SYNC panel the
// spelling of "G-SYNC + V-Sync On", where the panel still varies its refresh
// below its maximum and the flip waits only at the ceiling. No timer, no rate,
// no driver profile write, and no swapchain parameter is changed.
//
// **Why it is armed again (2026-09-13).** It was retired because forcing the
// interval was measured turning a generated group into fast-then-freeze judder
// (`20260830_175147`, `20260830_182939`). Both of those sessions forced the
// Vulkan *present mode* to FIFO as well, and that is what actually unpaced the
// group: NVIDIA's announced flip lead collapses from 6842 us to 141 us when a
// metered swapchain is forced to FIFO (`20260913_184745`, see
// vulkan_present_metering_policy.h). CE no longer touches the present mode
// there, so the group now arrives at DXGI already spread across its rendered
// interval (`20260913_194420`: announced lead 6066 us, frame-time stddev
// 154 us). Quantizing an already-correctly-spread group onto vertical blanks is
// vertical-blank synchronization; quantizing an unpaced burst was the judder.
// The earlier conclusion was measured on the unpaced burst.
//
// Arming installs the observation route. Whether a present is actually
// rewritten is a separate, narrower question - see ShouldRewriteFinalPresent.
inline bool ShouldArmFinalDxgiPresent(bool vulkanLayerModuleLoaded, std::string_view vsyncMode) {
    return vulkanLayerModuleLoaded &&
           ce::vulkan_present_metering_policy::RequestsVblankPacedPresentation(vsyncMode);
}

inline bool ShouldForceFinalDxgiFifo(bool fifoRequested, bool vulkanPresentationActive, bool hookShuttingDown) {
    return fifoRequested && vulkanPresentationActive && !hookShuttingDown;
}

// Rewrite contract for the final system present:
// - Without force, and for a DXGI_PRESENT_TEST (0x1) query that never actually
//   presents, both arguments are passed through byte-identical.
// - Otherwise SyncInterval becomes 1 (the DXGI contract for presenting at the
//   next vertical blank), DXGI_PRESENT_ALLOW_TEARING (0x200) is cleared
//   because it is invalid with a non-zero interval,
//   DXGI_PRESENT_DO_NOT_WAIT (0x8) is cleared so the forced FIFO present may
//   block on the vblank, and DXGI_PRESENT_RESTART (0x4) is cleared because it
//   discards all outstanding queued presents - the one behavior strict FIFO
//   exists to prevent. DXGI_PRESENT_DO_NOT_SEQUENCE (0x2 - not 0x8) and all
//   unrelated flags are preserved.
// - An already-correct interval=1/flags call remains byte-identical and
//   reports no change.
inline bool ApplyFinalDxgiFifoParameters(bool forceFifo, uint32_t& syncInterval, uint32_t& flags) {
    if (!forceFifo)
        return false;
    if (flags & kDxgiPresentTest)
        return false;
    if (syncInterval == 1 &&
        (flags & (kDxgiPresentAllowTearing | kDxgiPresentRestart | kDxgiPresentDoNotWait)) == 0)
        return false;

    syncInterval = 1;
    flags &= ~(kDxgiPresentAllowTearing | kDxgiPresentRestart | kDxgiPresentDoNotWait);
    return true;
}

// The rewrite is additionally scoped to swapchain instances the creation
// detours actually observed; see hook/common/vulkan_dxgi_fifo_registry.h. A
// foreign swapchain always passes through, so an armed backstop never
// restates a pacing contract on a present it did not watch being created.
// The rewrite is scoped to the one case whose vertical blank nothing else
// supplies: a device that enabled VK_NV_present_metering, where CE's
// creation-time present-mode override has stood down. An ordinary Vulkan title
// still gets its vertical-blank wait from the FIFO present mode CE did force,
// and restating that as a DXGI interval below the WSI would only take away the
// per-present choice the WSI makes for variable refresh.
inline bool ShouldRewriteFinalPresent(bool forceFifo, bool presentedSwapchainRegistered,
                                      bool deviceEnabledPresentMetering) {
    return forceFifo && presentedSwapchainRegistered && deviceEnabledPresentMetering;
}

}  // namespace ce::vulkan_dxgi_fifo_policy
