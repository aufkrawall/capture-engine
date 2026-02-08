#pragma once

/**
 * DPI Scaling Utilities
 * 
 * Provides DPI awareness and scaling support for high-DPI displays.
 * Automatically handles different Windows versions (Win10 1607+, Win8.1, Win7).
 */

#include <windows.h>
#include <shellscalingapi.h>  // For GetDpiForWindow (Win10 1607+)

namespace ce {

// DPI awareness context for Windows 10 1607+
enum class DpiAwarenessContext {
    Unaware,           // DPI unaware (bitmap scaled by OS)
    SystemAware,       // System DPI aware (single monitor)
    PerMonitorAware,   // Per-monitor DPI aware (legacy)
    PerMonitorAwareV2  // Per-monitor V2 (recommended, Win10 1703+)
};

// Per-monitor DPI awareness helper
class DpiHelper {
public:
    // Initialize DPI awareness for the process
    // Call this early in main() or DLLMain
    static bool Initialize(DpiAwarenessContext context = DpiAwarenessContext::PerMonitorAwareV2);
    
    // Get DPI for a specific window (0 if failed)
    static UINT GetDpi(HWND hwnd);
    
    // Get system DPI
    static UINT GetSystemDpi();
    
    // Convert pixels from 96 DPI (standard) to target DPI
    static int Scale(int pixels96, UINT dpi);
    static float Scale(float pixels96, UINT dpi);
    
    // Convert pixels from target DPI to 96 DPI
    static int Unscale(int pixels, UINT dpi);
    static float Unscale(float pixels, UINT dpi);
    
    // Get scaling factor (1.0 = 96 DPI, 1.5 = 144 DPI, etc.)
    static float GetScaleFactor(UINT dpi);
    static float GetWindowScaleFactor(HWND hwnd);
    
    // Scale a RECT/POINT/SIZE from 96 DPI to target
    static void ScaleRect(RECT& rect, UINT dpi);
    static void ScalePoint(POINT& pt, UINT dpi);
    static void ScaleSize(SIZE& size, UINT dpi);

private:
    static bool s_initialized;
    static UINT s_systemDpi;
    
    // Dynamic function pointers for backward compatibility
    typedef UINT (WINAPI* PFN_GetDpiForWindow)(HWND hwnd);
    typedef UINT (WINAPI* PFN_GetDpiForSystem)();
    typedef DPI_AWARENESS_CONTEXT (WINAPI* PFN_SetThreadDpiAwarenessContext)(DPI_AWARENESS_CONTEXT context);
    typedef BOOL (WINAPI* PFN_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT context);
    typedef HRESULT (WINAPI* PFN_SetProcessDpiAwareness)(PROCESS_DPI_AWARENESS awareness);
    
    static PFN_GetDpiForWindow pGetDpiForWindow;
    static PFN_GetDpiForSystem pGetDpiForSystem;
    static PFN_SetThreadDpiAwarenessContext pSetThreadDpiAwarenessContext;
    static PFN_SetProcessDpiAwarenessContext pSetProcessDpiAwarenessContext;
    static PFN_SetProcessDpiAwareness pSetProcessDpiAwareness;
    
    static void LoadDpiFunctions();
};

// RAII DPI context switcher for Windows 10 1607+
class ScopedDpiContext {
public:
    explicit ScopedDpiContext(DPI_AWARENESS_CONTEXT context);
    ~ScopedDpiContext();
    
    // Non-copyable
    ScopedDpiContext(const ScopedDpiContext&) = delete;
    ScopedDpiContext& operator=(const ScopedDpiContext&) = delete;

private:
    DPI_AWARENESS_CONTEXT m_previousContext;
    bool m_valid;
};

// High-DPI aware window wrapper
// Ensures window is created with proper DPI awareness
class DpiAwareWindow {
public:
    // Create a DPI-aware window
    static HWND Create(
        LPCWSTR lpClassName,
        LPCWSTR lpWindowName,
        DWORD dwStyle,
        int x,
        int y,
        int nWidth,
        int nHeight,
        HWND hWndParent,
        HMENU hMenu,
        HINSTANCE hInstance,
        LPVOID lpParam
    );
};

} // namespace ce
