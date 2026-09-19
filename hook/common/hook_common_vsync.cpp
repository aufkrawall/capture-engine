// VSync override resolution for the graphics hooks.
//
// Split out of hook_common.cpp (2026-09-19): the file reached the 800-line
// ceiling, and `vsync_mode` is its own decision with three consumers - the DX9
// / DX11 / DXGI present paths, and now the DLSS-G driver-settings answer, which
// needs the same resolution without re-entering GetActiveGraphicsConfig.

#include <string>

#include "hook_common.h"

// Helper to get VSync override settings
// Reduces code duplication across DX9/DX11/DX12 hooks
// Split from GetVSyncOverride so the resolved config can also be published to
// the DLSS-G driver-settings answer without re-entering GetActiveGraphicsConfig.
VSyncOverride ResolveVSyncOverrideForMode(const std::string& vsyncMode) {
    VSyncOverride result;

    if (vsyncMode == "default" || vsyncMode.empty()) {
        result.shouldOverride = false;
        return result;
    }

    result.shouldOverride = true;

    if (vsyncMode == "off") {
        result.presentInterval = 0;  // DX9: D3DPRESENT_INTERVAL_IMMEDIATE, DX11/12: sync interval 0
        result.useMailbox = false;
    } else if (vsyncMode == "fifo" || vsyncMode == "adaptive") {
        result.presentInterval = 1;  // DX9: D3DPRESENT_INTERVAL_ONE, DX11/12: sync interval 1
        result.useMailbox = false;
    } else if (vsyncMode == "mailbox") {
        result.presentInterval = 0;  // DX9: immediate (no true mailbox), DX11/12: sync 0 + flip_discard
        result.useMailbox = true;
    } else {
        // Unknown mode, don't override
        result.shouldOverride = false;
    }

    return result;
}

VSyncOverride GetVSyncOverride() {
    return ResolveVSyncOverrideForMode(GetActiveGraphicsConfig().vsyncMode);
}

// Process VSync override on Present parameters
void ProcessVSyncOverride(UINT& SyncInterval, UINT& Flags) {
    // When the FPS limiter is actively pacing frames, disable vsync so the limiter
    // controls frame timing. With FLIP model, SyncInterval=0 alone still throttles
    // at vblank rate; the wrapper path adds DXGI_PRESENT_ALLOW_TEARING for swap chains
    // it created with that flag. Here we only set SyncInterval=0 since we don't know
    // if the swap chain supports ALLOW_TEARING.
    VSyncOverride override = GetVSyncOverride();

    // DXGI spec: ALLOW_TEARING is only valid with SyncInterval == 0.
    // Sanitize invalid combinations even when no explicit override is active.
    if (SyncInterval > 0) {
        Flags &= ~0x200;  // Clear DXGI_PRESENT_ALLOW_TEARING
    }

    if (!override.shouldOverride)
        return;

    // Apply the sync interval override
    SyncInterval = override.presentInterval;

    // Flag manipulation for mailbox mode
    if (override.useMailbox) {
        // DXGI_PRESENT_ALLOW_TEARING requires sync interval 0
        SyncInterval = 0;
        Flags |= 0x200;  // DXGI_PRESENT_ALLOW_TEARING
    } else if (SyncInterval > 0) {
        // VSync enabled: MUST clear ALLOW_TEARING flag
        // DXGI spec: ALLOW_TEARING is only valid with SyncInterval=0
        Flags &= ~0x200;  // Clear DXGI_PRESENT_ALLOW_TEARING
    }
}
