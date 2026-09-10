#pragma once

namespace ce::injection_policy {

inline constexpr wchar_t kRealtimeProcessStartQuery[] =
    L"SELECT ProcessName, ProcessID FROM Win32_ProcessStartTrace";
inline constexpr wchar_t kPolledProcessStartFallbackQuery[] =
    L"SELECT * FROM __InstanceCreationEvent WITHIN 0.5 WHERE "
    L"TargetInstance ISA 'Win32_Process'";

inline bool ShouldInjectAfterGraphicsProbe(bool d3d12Loaded) {
    // The hook is designed to install before the first real swapchain whenever
    // possible. A fixed post-D3D12 delay can miss early Presents and leaves the
    // overlay visibly late in games that initialize Streamline/FFX before the
    // main loop starts.
    (void)d3d12Loaded;
    return true;
}

inline bool ShouldLaunchPendingInjection(bool whitelisted, bool alreadyInjected, bool recentlyFailed) {
    return whitelisted && !alreadyInjected && !recentlyFailed;
}

}  // namespace ce::injection_policy
