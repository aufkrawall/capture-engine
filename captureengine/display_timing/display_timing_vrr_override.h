#pragma once

// Windows' "Variable refresh rate" graphics setting (Settings > System > Display > Graphics; stored as
// `VRROptimizeEnable=1` in HKCU\Software\Microsoft\DirectX\UserGpuPreferences) makes DXGI replace SyncInterval=1 with 0
// on a flip present to a variable-refresh output: dxgi!CDXGISwapChain::FlipPresentToDWM asks
// IsVariableRefreshRateOverrideEligible (SyncInterval == 1, the process flag the setting raises, the output is VRR,
// no DXGI_PRESENT_USE_DURATION), rewrites the flip interval and writes EventOverrideSyncInterval (Microsoft-Windows-DXGI
// event 0x4A). The flip then takes the newest frame at the next refresh instead of queueing every frame: V-Sync no
// longer holds the producer back, and above the display's maximum refresh rate the frames in between are cancelled
// unshown. No DXGI API opts a process out; only the setting decides.
//
// GTA V Enhanced, DLSS FG switched off by its menu on sl.dlss_g's own chain (session 20261113_185020, 144 Hz G-Sync
// panel): CE's vsync_mode=fifo handed DXGI SyncInterval=1 on every present and DXGI overrode every one (3685 of 3685 in
// the sealed trace); 160 presents/s rendered, 144 shown, 132 cancelled flips in 8 s - a visible judder. FidelityFX FG's
// chains received the same override but pace their own presents at the refresh rate, which hid it there.
//
// Remembers which followed processes Windows did this to, so the service says it once per process, and counts the
// overrides for the health line.

#include <array>
#include <cstddef>
#include <cstdint>

#include "common/logging/logging.h"

namespace display_timing_vrr_override {

class Monitor {
public:
    // Processes remembered for the once-per-process line; a further one replaces the oldest.
    static constexpr std::size_t kProcessCapacity = 16;

    // True the first time `processId` is seen (since it was last evicted).
    bool Observe(uint32_t processId) {
        ++overrides_;
        for (std::size_t i = 0; i < seenCount_; ++i) {
            if (seen_[i] == processId)
                return false;
        }
        if (seenCount_ < kProcessCapacity) {
            seen_[seenCount_++] = processId;
        } else {
            seen_[nextEviction_] = processId;
            nextEviction_ = (nextEviction_ + 1) % kProcessCapacity;
        }
        ++processes_;
        return true;
    }

    uint64_t overrides() const { return overrides_; }
    uint64_t processes() const { return processes_; }

private:
    std::array<uint32_t, kProcessCapacity> seen_{};
    std::size_t seenCount_ = 0;
    std::size_t nextEviction_ = 0;
    uint64_t overrides_ = 0;
    uint64_t processes_ = 0;
};

inline void LogVrrSyncIntervalOverride(uint32_t processId, uint32_t newSyncInterval) {
    LogWarn("[DisplayTiming] Windows replaces SyncInterval=1 with %u on the presents of PID %u: the \"Variable refresh "
            "rate\" graphics setting is on and the display runs variable refresh. V-Sync, vsync_mode=fifo included, "
            "then no longer holds frames back - each refresh shows the newest frame and, above the display's maximum "
            "refresh rate, the frames in between are dropped unshown. Queued V-Sync needs that Windows setting off "
            "(Settings > System > Display > Graphics), or a frame-rate cap below the refresh rate. Said once per "
            "process; counted as vrrSyncOverrides=",
            static_cast<unsigned>(newSyncInterval), static_cast<unsigned>(processId));
}

}  // namespace display_timing_vrr_override
