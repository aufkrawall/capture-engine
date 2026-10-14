#pragma once

namespace ce::injection_policy {

// The only WMI query CE issues for process discovery. It is event-driven and
// costs the system nothing while idle, but subscribing requires Administrators
// membership; an unelevated run is refused with WBEM_E_ACCESS_DENIED.
//
// There is deliberately no WMI fallback query any more. The previous one,
// `SELECT * FROM __InstanceCreationEvent WITHIN 0.5 WHERE TargetInstance ISA
// 'Win32_Process'`, does not poll inside CE: it asks the WMI service to
// enumerate and fully materialise every process instance twice a second for the
// whole session. The unelevated path is now ce::process_start::Poller, which
// reads the same information from the native call underneath that provider and
// takes only the two fields CE uses.
inline constexpr wchar_t kRealtimeProcessStartQuery[] =
    L"SELECT ProcessName, ProcessID FROM Win32_ProcessStartTrace";

// What the pre-injection module probe established, for the "Injecting (...)"
// log line. With early injection the probe usually runs while the loader is
// still building the module list (ERROR_PARTIAL_COPY), so it proves nothing
// about the graphics API; session 20261014_204616 still labelled Witcher 3
// (D3D12) "non-D3D12 (DX11/DX9/Vulkan)".
inline const char* GraphicsProbeLabel(bool d3d12Loaded, bool modulesEnumerated, unsigned long enumError) {
    if (d3d12Loaded)
        return "D3D12 detected";
    if (modulesEnumerated)
        return "no D3D12 module loaded yet: DX11/DX9/Vulkan or D3D12 loaded later";
    if (enumError == 299 /* ERROR_PARTIAL_COPY */)
        return "graphics API not probed: module list still being built during loader init";
    return "graphics API not probed: module list unavailable";
}

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
