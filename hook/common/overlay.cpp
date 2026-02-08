#include "overlay.h"
#include <cstdio>
#include <string>
#include "fg_detection.h"

// Global overlay instance
Overlay g_SharedOverlay;

// Helper to get system DPI scale (without window handle, for Vulkan layer)
// Uses primary monitor DPI
// Define DPI types for mingw compatibility
#ifndef MONITOR_DPI_TYPE
typedef enum {
    MDT_EFFECTIVE_DPI = 0,
    MDT_ANGULAR_DPI = 1,
    MDT_RAW_DPI = 2,
    MDT_DEFAULT = MDT_EFFECTIVE_DPI
} MONITOR_DPI_TYPE;
#endif

static float GetSystemDPIScale()
{
    static float cachedDpi = 0.0f;
    static DWORD lastCheck = 0;
    DWORD now = GetTickCount();
    if (cachedDpi > 0.0f && (now - lastCheck < 5000)) return cachedDpi;

    UINT xdpi = 96, ydpi = 96;

    // Try Windows 8.1+ API first
    typedef HRESULT(WINAPI * PFN_GetDpiForMonitor)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
    static HINSTANCE shcore_dll = ::LoadLibraryA("shcore.dll");
    static PFN_GetDpiForMonitor GetDpiForMonitorFn = nullptr;

    if (GetDpiForMonitorFn == nullptr && shcore_dll != nullptr) {
        GetDpiForMonitorFn = (PFN_GetDpiForMonitor)::GetProcAddress(shcore_dll, "GetDpiForMonitor");
    }

    if (GetDpiForMonitorFn != nullptr) {
        POINT pt = {1, 1};
        HMONITOR monitor = ::MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
        GetDpiForMonitorFn(monitor, MDT_EFFECTIVE_DPI, &xdpi, &ydpi);
    } else {
#ifndef NOGDI
        // Fallback to GDI
        HDC dc = ::GetDC(nullptr);
        xdpi = ::GetDeviceCaps(dc, LOGPIXELSX);
        ydpi = ::GetDeviceCaps(dc, LOGPIXELSY);
        ::ReleaseDC(nullptr, dc);
#endif
    }

    cachedDpi = xdpi / 96.0f;
    lastCheck = now;
    return cachedDpi;
}

// Helper to format bytes to GiB string
static void FormatBytes(char* buf, size_t size, uint64_t bytes)
{
    float gib = (float)bytes / (1024.0f * 1024.0f * 1024.0f);
    snprintf(buf, size, "%.1f GiB", gib);
}

static const char* GetQualityString(int mode)
{
    switch (mode) {
        case 0:
            return "Perf";
        case 1:
            return "Bal";
        case 2:
            return "Qual";
        case 3:
            return "UltPerf";
        case 4:
            return "UltQual";
        case 5:
            return "DLAA";
        default:
            return "";
    }
}

void Overlay::InitImGui(void* hwnd)
{
    // REMOVED: ImGui initialization - Using custom overlay instead
    (void)hwnd;
    if (!initialized) {
        initialized = true;
        headless = false;
        this->hwnd = hwnd;
    }
}

void Overlay::InitImGuiHeadless()
{
    // REMOVED: Using custom overlay instead
    initialized = true;
    headless = true;
}

void Overlay::ShutdownImGui()
{
    // REMOVED: Using custom overlay instead
    initialized = false;
    context = nullptr;
}

void Overlay::BeginFrame()
{
    // REMOVED: Using custom overlay instead
}

void Overlay::EndFrame()
{
    // REMOVED: Using custom overlay instead
}

ImU32 Overlay::GetLoadColor(float load, const OverlayConfig& cfg)
{
    // Check if colors are set (non-zero alpha)
    // If not set, use hardcoded defaults if config parser didn't set them
    // But config parser sets defaults.

    if (load < 50.0f) return cfg.loadColorLow;
    if (load < 85.0f) return cfg.loadColorMed;
    return cfg.loadColorHigh;
}

void Overlay::RenderTextWithOutline(const char* text, ImU32 color, bool outline, ImU32 outlineColor, float thickness)
{
    // REMOVED: Using custom overlay instead
    (void)text;
    (void)color;
    (void)outline;
    (void)outlineColor;
    (void)thickness;
}

void Overlay::RenderUI()
{
    // REMOVED: Using custom overlay instead
    lastDrawResult = DrawResult::Unknown;
    if (!initialized) {
        lastDrawResult = DrawResult::SkippedNotInitialized;
        return;
    }
    // Custom overlay handles rendering directly
    lastDrawResult = DrawResult::Drawn;
}
