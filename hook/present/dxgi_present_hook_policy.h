#pragma once

namespace ce::dxgi_present_hook {
enum class PresentInlineHookRoute { kEntryPrepend, kReserveNativeEntry, kBelowForeignEntry };

// Present and Present1 share forwarding policy. Once either method has an entry prepend,
// a retry must use that route for the other method even if another overlay has since loaded.
// With no prepend, reserve native entries before a late overlay can save CE's patch.
inline PresentInlineHookRoute SelectPresentInlineHookRoute(bool entryViewInstalled, bool bodyViewInstalled,
                                                           bool nativeSystemMethods, bool leaveEntryToOverlays) {
    if (entryViewInstalled)
        return PresentInlineHookRoute::kEntryPrepend;
    if (nativeSystemMethods)
        return PresentInlineHookRoute::kReserveNativeEntry;
    if (bodyViewInstalled || leaveEntryToOverlays)
        return PresentInlineHookRoute::kBelowForeignEntry;
    return PresentInlineHookRoute::kEntryPrepend;
}

// A native body view covers native slots. A frontend or independent vtable layer still
// needs its captured predecessor, unless an already loaded overlay owns the shared chain.
inline bool ShouldKeepVTablePristineForReservedEntry(bool nativeSystemMethods, bool foreignOverlayLoaded) {
    return nativeSystemMethods || foreignOverlayLoaded;
}

// Keep the body patch outside every recognized foreign entry form: E9 or FF 25 + 8-byte
// target on x64; on x86 E9 rel32 (5) or FF 25 abs32 / push imm32; ret (6).
#ifdef _WIN64
constexpr int kReservedEntryBytes = 14;
#else
constexpr int kReservedEntryBytes = 6;
#endif

// The span of a foreign entry patch seen at sampling time. An FF 25 form and an invisible
// patch both take the reserved width (the FF 25 form is the widest on either architecture).
constexpr int ObservedEntryPatchSpan(bool entryUsesE9) {
    return entryUsesE9 ? 5 : kReservedEntryBytes;
}
}  // namespace ce::dxgi_present_hook
