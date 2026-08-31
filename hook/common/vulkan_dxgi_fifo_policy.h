#pragma once

#include <cstdint>
#include <string_view>

namespace ce::vulkan_dxgi_fifo_policy {

inline constexpr uint32_t kDxgiPresentTest = 0x1u;
inline constexpr uint32_t kDxgiPresentDoNotSequence = 0x2u;
inline constexpr uint32_t kDxgiPresentRestart = 0x4u;
inline constexpr uint32_t kDxgiPresentDoNotWait = 0x8u;
inline constexpr uint32_t kDxgiPresentAllowTearing = 0x200u;

// **Retired fallback: the Vulkan presentation engine owns scheduling.**
//
// Forcing SyncInterval=1 below NVIDIA's WSI replaced VRR with a fixed grid.
// With 4x MFG, a variable-duration generated group was then consumed in exactly
// four refreshes, producing the measured fast-then-freeze judder and apparent
// overlay flicker. The layer now uses VK_EXT_present_timing relative scheduling:
// the presentation engine retains the generated group's native spacing while
// its reported minimum refresh duration supplies the maximum-rate ceiling.
// Keeping this hook path disarmed is essential; it must not restate that native
// schedule as a uniform DXGI interval one layer below Vulkan.
inline bool ShouldArmFinalDxgiPresent(bool vulkanLayerModuleLoaded, std::string_view vsyncMode) {
    (void)vulkanLayerModuleLoaded;
    (void)vsyncMode;
    return false;
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
inline bool ShouldRewriteFinalPresent(bool forceFifo, bool presentedSwapchainRegistered) {
    return forceFifo && presentedSwapchainRegistered;
}

}  // namespace ce::vulkan_dxgi_fifo_policy
