#include "dxgi_shared_internal.h"
#include "common/logging/log_meter.h"

namespace DXGIShared {
namespace {
// Who called into the Present detour is the only per-frame evidence of the
// shared five-byte entry dance: a foreign overlay that joined CE's prepended
// entry restores its saved bytes and re-patches around every call, so frames
// reach CE through that overlay on some calls and directly from the game on
// others. A run of alternating classes names that state instead of leaving it
// to be re-diagnosed from overlay-disappearance reports. Metered like the
// foreign entry-ownership watch (first transitions, then rarely) so a
// per-frame alternation cannot flood the log.
void NotePresentChainParticipant(bool callerFromThirdPartyOverlay, const char* callerModulePath) {
    static std::atomic<int> s_participantClass{-1};
    static std::atomic<unsigned int> s_participantTransitions{0};
    const int participantClass = callerFromThirdPartyOverlay ? 1 : 0;
    const int previous = s_participantClass.exchange(participantClass, std::memory_order_relaxed);
    if (previous == participantClass) {
        return;
    }
    const unsigned int transition = s_participantTransitions.fetch_add(1, std::memory_order_relaxed) + 1;
    if (transition <= 8 || (transition % 1024) == 0) {
        HookLogImportant("DetourPresent: Present chain participant changed #%u: caller=%s foreignOverlay=%d",
                         transition, callerModulePath && callerModulePath[0] ? callerModulePath : "<unresolved>",
                         participantClass);
    }
}
}  // namespace

bool ShouldBypassThirdPartyOverlayPresent(IDXGISwapChain* pSwapChain, bool callerFromThirdPartyOverlay,
                                          const char* callerModulePath, const char* presentName) {
    NotePresentChainParticipant(callerFromThirdPartyOverlay, callerModulePath);
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
            pSwapChain, skipOverlayHelper, knownThirdPartyOverlaySwapchain, startupBlockingOverlayTaggedSwapchain,
            startupBlockingOverlaySwapchainStillOwnsPresent));
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
