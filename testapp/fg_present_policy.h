// Pure-logic present policy for the FG switch test app. No D3D/Streamline types so
// tests/test_fg_present_policy.cpp can exercise the exact production logic.
#pragma once

namespace testapp {
namespace fg {

struct ProxyPresentPolicy {
    unsigned syncInterval;
    bool allowTearing;
};

// The Streamline/DLSS-G proxy swapchain is presented uncapped (SyncInterval=0) for the WHOLE DLSS
// mode -- while FG generates frames AND while FG is merely suspended (suspend keeps the proxy and
// its pacer; Reflex stays on and paces the output, exactly like a real DLSS-G game that requests
// SyncInterval=0 and lets Reflex/driver pace). OFF and FSR modes run real (native/FSR) swapchains
// and honor the configured vsync; OFF mode never keeps the proxy because leaving DLSS tears it down
// (required so Reflex can genuinely be turned off -- see the Reflex invariant in
// dx12_fg_switch_streamline.inl). The tearing flag follows the USER's vsync intent, not the forced
// sync interval: a pacer-forced SyncInterval=0 with vsync configured on must not tear.
inline ProxyPresentPolicy ResolveProxyPresentPolicy(bool modeIsDlss, int configuredVsync, bool swapchainAllowsTearing) {
    ProxyPresentPolicy policy = {};
    policy.syncInterval = modeIsDlss ? 0u : static_cast<unsigned>(configuredVsync);
    policy.allowTearing = policy.syncInterval == 0 && configuredVsync == 0 && swapchainAllowsTearing;
    return policy;
}

}  // namespace fg
}  // namespace testapp
