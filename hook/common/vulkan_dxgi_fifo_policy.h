#pragma once

#include <cstdint>
#include <string_view>

// The canonical `RequestsVblankPacedPresentation` lives in the layer's present
// metering policy and is included here rather than duplicated: the two modes
// that arm this backstop are exactly the two the layer's metering withholding
// keys on, so one implementation is the only way they cannot drift apart. The
// header is leaf-only (<vulkan/vulkan.h> plus the standard library), and every
// consumer of this header (the hook DLL wrapper target and the unit tests)
// already compiles Vulkan headers, so the shared include adds no new build
// dependency.
#include "../vulkan_layer/vulkan_present_metering_policy.h"

namespace ce::vulkan_dxgi_fifo_policy {

inline constexpr uint32_t kDxgiPresentTest = 0x1u;
inline constexpr uint32_t kDxgiPresentDoNotSequence = 0x2u;
inline constexpr uint32_t kDxgiPresentRestart = 0x4u;
inline constexpr uint32_t kDxgiPresentDoNotWait = 0x8u;
inline constexpr uint32_t kDxgiPresentAllowTearing = 0x200u;

// **Scoped backstop: restore the final native DXGI vblank contract.**
//
// Arming requires the resident CE Vulkan layer - its WSI is the only caller
// whose final presents this path may rewrite - and a configured mode that
// explicitly asks for vertical-blank pacing (the canonical
// RequestsVblankPacedPresentation from the metering policy, included above).
// Under Portal RTX / RTX Remix 4x MFG the group-aware Remix CPU pacer spreads
// generated groups evenly across the rendered frame interval; that is a
// frame-time pacer, not VSync, and the presented output then runs past the
// refresh rate with tearing (~190 presents per second in the measured
// session). CE already forces VK_PRESENT_MODE_FIFO_KHR at swapchain creation,
// withholds VK_NV_present_metering at device level and sets
// rtx.dlfg.enablePresentMetering=False so that pacer engages; arming here
// re-creates the missing native guarantee one layer down: the final system
// DXGI present waits for the vertical blank and is not allowed to tear.
// `off`, `mailbox` and `default` deliberately never arm, so this backstop
// never touches a swapchain whose contract CE did not configure as
// vblank-paced.
inline bool ShouldArmFinalDxgiPresent(bool vulkanLayerModuleLoaded, std::string_view vsyncMode) {
    return vulkanLayerModuleLoaded && ce::vulkan_present_metering_policy::RequestsVblankPacedPresentation(vsyncMode);
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
