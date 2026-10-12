#pragma once

namespace ce::dxgi_present_hook {
// Reserve native x64 entries before a late overlay can save CE's prepend as its predecessor.
// Foreign implementations and unsupported body hooks keep their established entry fallback.
inline bool ShouldReserveEntryForLateOverlays(bool nativeSystemMethods, bool bodyHooksSupported,
                                               bool entryViewAlreadyInstalled) {
    return nativeSystemMethods && bodyHooksSupported && !entryViewAlreadyInstalled;
}

// A native body view covers native slots. A frontend or independent vtable layer still
// needs its captured predecessor, unless an already loaded overlay owns the shared chain.
inline bool ShouldKeepVTablePristineForReservedEntry(bool nativeSystemMethods, bool foreignOverlayLoaded) {
    return nativeSystemMethods || foreignOverlayLoaded;
}

// Keep the body patch outside either recognized foreign entry form (E9 or x64 FF 25).
constexpr int kReservedEntryBytes = 14;
}  // namespace ce::dxgi_present_hook
