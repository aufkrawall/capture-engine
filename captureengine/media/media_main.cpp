#include "media_main_internal.h"

UINT GetCursorDpiAtPoint(POINT point) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    using GetDpiForSystemFn = UINT(WINAPI*)();
    static const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    static const auto getDpiForWindow =
        reinterpret_cast<GetDpiForWindowFn>(user32 ? GetProcAddress(user32, "GetDpiForWindow") : nullptr);
    static const auto getDpiForSystem =
        reinterpret_cast<GetDpiForSystemFn>(user32 ? GetProcAddress(user32, "GetDpiForSystem") : nullptr);

    static thread_local HMONITOR cachedMonitor = nullptr;
    static thread_local UINT cachedDpi = 0;
    const HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    if (monitor && monitor == cachedMonitor && cachedDpi != 0) {
        return cachedDpi;
    }

    if (getDpiForWindow) {
        const HWND pointWindow = WindowFromPoint(point);
        if (pointWindow) {
            const UINT dpi = getDpiForWindow(pointWindow);
            if (dpi != 0) {
                cachedMonitor = monitor;
                cachedDpi = dpi;
                return cachedDpi;
            }
        }
    }
    if (getDpiForSystem) {
        const UINT dpi = getDpiForSystem();
        if (dpi != 0) {
            cachedMonitor = monitor;
            cachedDpi = dpi;
            return cachedDpi;
        }
    }
    return 96;
}

int GetCursorMetricForDpi(int metric, UINT dpi) {
    using GetSystemMetricsForDpiFn = int(WINAPI*)(int, UINT);
    static const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    static const auto getSystemMetricsForDpi =
        reinterpret_cast<GetSystemMetricsForDpiFn>(user32 ? GetProcAddress(user32, "GetSystemMetricsForDpi") : nullptr);
    return getSystemMetricsForDpi ? getSystemMetricsForDpi(metric, dpi) : GetSystemMetrics(metric);
}

void AtomicMax(std::atomic<uint32_t>& target, uint32_t value) {
    uint32_t current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
}

void AtomicMin(std::atomic<uint32_t>& target, uint32_t value) {
    uint32_t current = target.load(std::memory_order_relaxed);
    while (value < current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
}
