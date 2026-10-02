#pragma once

#include <cstdint>

namespace ce::pseudo_overlay {

// DPI resolution policy for the controller-side pseudo-overlay.
//
// The pseudo-overlay runs inside captureengine.exe (PerMonitorV2 via its manifest) and
// draws layered topmost popup windows in physical pixels on the anchor monitor. The scale
// used for fonts and circle insets must therefore come from the monitor where the overlay
// is displayed, NOT from the DPI of the anchor window as reported by GetDpiForWindow():
// GetDpiForWindow returns the DPI as seen through the DPI-awareness context of the thread
// that created that window (always 96 for DPI-unaware apps, the system DPI for
// system-DPI-aware apps, and the monitor DPI only for per-monitor-aware apps). Basing the
// font size on the anchor window previously made the overlay text change size on the same
// monitor whenever the foreground app switched between differently-aware processes, and it
// made the startup fallback depend on a hardcoded 96 sentinel.
//
// The monitor's effective DPI (GetDpiForMonitor / MDT_EFFECTIVE_DPI) is the one DPI value
// that describes how large a physical pixel content is presented on that display,
// independent of any window's awareness. The fallback chain keeps older callers and exotic
// failure states (monitor handle gone, API unavailable) on a sane default:
//   monitor DPI != 0 -> monitor DPI
//   system DPI != 0  -> system DPI
//   otherwise        -> 96 (the classic 100% scale)
inline uint32_t ResolveOverlayDpi(uint32_t monitorDpi, uint32_t systemDpi) {
    if (monitorDpi != 0) {
        return monitorDpi;
    }
    return systemDpi != 0 ? systemDpi : 96u;
}

}  // namespace ce::pseudo_overlay
