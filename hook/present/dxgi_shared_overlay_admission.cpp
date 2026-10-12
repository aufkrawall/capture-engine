#include "dxgi_shared_internal.h"
#include "common/logging/log_meter.h"

namespace DXGIShared {
bool ShouldBypassThirdPartyOverlayPresent(IDXGISwapChain* pSwapChain, bool callerFromThirdPartyOverlay,
                                         const char* presentName) {
    const bool knownThirdPartyOverlaySwapchain = DXGIShared::DX12_IsThirdPartyOverlaySwapchain(pSwapChain);
    const bool startupBlockingOverlayTaggedSwapchain =
        DXGIShared::DX12_IsStartupBlockingOverlayTaggedSwapchain(pSwapChain);
    const bool startupBlockingOverlaySwapchainStillOwnsPresent =
        ce::dx12_overlay_policy::ShouldKeepStartupBlockingOverlaySwapchainBypass(
            knownThirdPartyOverlaySwapchain, startupBlockingOverlayTaggedSwapchain,
            HasStartupBlockingOverlayModuleInCurrentStack());
    // A late overlay can forward the game's Present into CE even when our
    // entry hook was originally installed first. Creation identity, rather
    // than the immediate caller or installation order, decides ownership.
    const bool skipOverlayHelper = ce::dx12_overlay_policy::ShouldSkipPresentProcessingForThirdPartyOverlaySwapchain(
        knownThirdPartyOverlaySwapchain, startupBlockingOverlayTaggedSwapchain,
        startupBlockingOverlaySwapchainStillOwnsPresent);
    if (callerFromThirdPartyOverlay || knownThirdPartyOverlaySwapchain) {
        static ce::log_meter::ChangeGate ownershipLog;
        const auto verdict = ownershipLog.Observe(ce::log_meter::FieldKey(
            pSwapChain, skipOverlayHelper, knownThirdPartyOverlaySwapchain, startupBlockingOverlayTaggedSwapchain));
        if (verdict) {
            HookLogImportant(
                "%s: Overlay chain ownership sc=%p helper=%d tagged=%d startup=%d action=%s%s",
                presentName, pSwapChain, knownThirdPartyOverlaySwapchain ? 1 : 0, startupBlockingOverlayTaggedSwapchain ? 1 : 0,
                startupBlockingOverlaySwapchainStillOwnsPresent ? 1 : 0,
                skipOverlayHelper ? "bypass-helper" : "process-output", ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }
    return skipOverlayHelper;
}
}  // namespace DXGIShared
