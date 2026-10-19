#pragma once

#include <string>

// Controller-side pseudo-overlay for WGC capture (no injection required).
// Uses layered desktop windows. The implementation keeps the legacy ghost
// keepalive and animated warning text behavior, while trying to avoid extra z-order
// churn and improve monitor anchoring.
struct PseudoOverlayConfig {
    bool enabled = false;
    int size = 30;              // Indicator circle diameter (10-200)
    int pad = 20;               // Padding from screen edge (0-100)
    int pos = 0;                // 0=BR, 1=BL, 2=TR, 3=TL
    int mode = 0;               // 0=InformationIndicator, 1=WarningAndIndicator, 2=WarningOnly
    bool alwaysRender = false;  // Keep indicator window alive with a 1x1 alpha=1 ghost pixel when idle
    bool alwaysRenderOnlyWhenGame = false;
    bool showEncoderOverloadWarn = true;
    int foregroundAcquireGraceMs = 2000;  // Suppress visible overlay for N ms after the whitelisted
                                          // game (re)acquires foreground focus. Avoids racing Windows
                                          // MPO / fullscreen buffer rebinds on Alt+Tab-in. 0 = off.
    std::string processList;              // Pipe-delimited process names for foreground detection
};
