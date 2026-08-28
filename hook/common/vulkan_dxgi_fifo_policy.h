#pragma once

#include <cstdint>
#include <string_view>

namespace ce::vulkan_dxgi_fifo_policy {

inline constexpr uint32_t kDxgiPresentAllowTearing = 0x200u;

// A resident CaptureEngine Vulkan layer is authoritative startup evidence that
// Vulkan owns presentation. Only the explicit FIFO mode arms the narrow DXGI
// path; adaptive and mailbox retain their existing behavior.
inline bool ShouldArmFinalDxgiPresent(bool vulkanLayerModuleLoaded, std::string_view vsyncMode) {
    return vulkanLayerModuleLoaded && vsyncMode == "fifo";
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
