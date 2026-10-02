#pragma once

// Where CE hooks DXGI's ResizeBuffers/ResizeBuffers1 for the resize-flag
// reconciliation (DXGIShared::InstallResizeReconciliationHooks).
//
// The Steam overlay hooks a swapchain by patching the entry of each function the
// swapchain's vtable points to, and skips a function whose entry already jumps
// into another module ("points to another module, skipping hooks"). It holds one
// reference on every back buffer it draws on and returns them only in its own
// ResizeBuffers hook. Talos Reawakened, logs/20260927_031545: Present and
// Present1 began with a jump into Steam's relay page, ResizeBuffers began with
// CE's jump, Steam kept its six references and every resolution change was
// refused - with or without frame generation. CE's Present hook has always sat
// below the entry for exactly this reason; the resize hook now does too:
//
//  - the body hook below the entry is the only site CE takes first; a
//    slot-hooking overlay that arrives later patches the untouched entry, and its
//    trampoline resumes in the body and still reaches CE;
//  - when the body hook is refused and a third-party overlay is loaded, CE takes
//    no site at all. The reconciliation is then unavailable, which is safe: CE
//    only adds the waitable-object flag at creation while it can hide it again on
//    resize (swapchain_flag_apply.h);
//  - with no third-party overlay loaded, the entry prepend stays the fallback.
namespace ce::resize_reconcile_hook {

enum class Site {
    kBodyBelowEntry,
    kEntry,
    kNone,
};

// The widest foreign entry patch CE recognizes (FF 25 + 8-byte target). The
// body hook goes past it whether or not an entry patch is visible: deeper than
// a foreign patch is always safe, inside one never is.
inline constexpr int kAssumedForeignEntryPatchSize = 14;

inline Site ChooseSiteAfterBodyHookRefused(bool thirdPartyOverlayLoaded) {
    return thirdPartyOverlayLoaded ? Site::kNone : Site::kEntry;
}

inline const char* SiteName(Site site) {
    switch (site) {
        case Site::kBodyBelowEntry:
            return "body-below-entry";
        case Site::kEntry:
            return "entry";
        case Site::kNone:
            return "none";
    }
    return "?";
}

}  // namespace ce::resize_reconcile_hook
