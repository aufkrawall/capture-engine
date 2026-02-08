#pragma once

// #include <imgui.h>  // REMOVED: Using custom overlay
//  Minimal ImGui type stubs for migration
struct ImVec2 {
    float x, y;
};
struct ImVec4 {
    float x, y, z, w;
};
typedef unsigned int ImU32;
typedef void* ImFont;
typedef void* ImGuiContext;

// Stub constants
#define ImGuiCond_Always                    0
#define ImGuiWindowFlags_NoDecoration       0
#define ImGuiWindowFlags_AlwaysAutoResize   0
#define ImGuiWindowFlags_NoFocusOnAppearing 0
#define ImGuiWindowFlags_NoNav              0
#define ImGuiWindowFlags_NoInputs           0
#define ImGuiStyleVar_WindowRounding        0
#define ImGuiStyleVar_CellPadding           0
#define ImGuiStyleVar_ItemSpacing           0
#define ImGuiCol_WindowBg                   0
#define ImGuiCol_Border                     0
#define IM_COL32(r, g, b, a)                ((ImU32)((a) << 24) | ((b) << 16) | ((g) << 8) | (r))

// Stub functions
namespace ImGui {
inline ImU32 ColorConvertFloat4ToU32(const ImVec4& c)
{
    (void)c;
    return 0;
}
inline ImVec4 ColorConvertU32ToFloat4(ImU32 c)
{
    (void)c;
    return {0, 0, 0, 0};
}
inline void* GetMainViewport() { return nullptr; }
}  // namespace ImGui

// Stub for Win32 DPI function
inline float ImGui_ImplWin32_GetDpiScaleForHwnd(void*) { return 1.0f; }

#include <windows.h>
#include <cstring>
// #include "backends/imgui_impl_win32.h"  // REMOVED: Using custom overlay
#include "ipc_client.h"
#include "performance_metrics.h"
#include "system_metrics.h"

// Shared overlay UI rendering - API-agnostic ImGui widgets
// Each graphics API calls RenderUI() after setting up ImGui frame
class Overlay {
public:
    enum class DrawResult {
        Unknown = 0,
        Drawn,
        SkippedNotInitialized,
        SkippedNoContext,
        SkippedWindowHidden,
        SkippedNoIPC,
        SkippedShowDisabled,
    };

    void SetMetrics(PerformanceMetrics* m) { metrics = m; }
    void SetIPCClient(IPCClient* ipc) { this->ipc = ipc; }
    void SetHwnd(void* hwnd) { this->hwnd = hwnd; }
    void SetGraphicsAPI(const char* api)
    {
        strncpy(graphicsAPI, api, sizeof(graphicsAPI) - 1);
        graphicsAPI[sizeof(graphicsAPI) - 1] = '\0';
    }

    // Get DPI scale for current window
    float GetDpiScale() const
    {
        float dpiScale = 1.0f;
        if (hwnd) {
            dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
        }
        return dpiScale;
    }

    bool IsInitialized() const { return initialized; }

    // Common ImGui initialization (API-agnostic part)
    void InitImGui(void* hwnd);

    // Headless initialization for Vulkan layer (no Win32 backend)
    void InitImGuiHeadless();

    // Common ImGui shutdown
    void ShutdownImGui();

    // Force reinitialization (for swapchain recreation scenarios like DLSS FG)
    void ForceReinit()
    {
        initialized = false;
        context = nullptr;  // Context was destroyed by ShutdownImGui
    }

    // Common ImGui frame lifecycle
    void BeginFrame();
    void EndFrame();

    // Render overlay UI widgets (call between BeginFrame and EndFrame)
    void RenderUI();

    DrawResult GetLastDrawResult() const { return lastDrawResult; }
    const char* GetLastDrawReason() const
    {
        switch (lastDrawResult) {
            case DrawResult::Drawn:
                return "drawn";
            case DrawResult::SkippedNotInitialized:
                return "not_initialized";
            case DrawResult::SkippedNoContext:
                return "no_context";
            case DrawResult::SkippedWindowHidden:
                return "window_hidden";
            case DrawResult::SkippedNoIPC:
                return "no_ipc";
            case DrawResult::SkippedShowDisabled:
                return "show_disabled";
            default:
                return "unknown";
        }
    }

    // Set dropped frames count from capture side (called by capture hooks)
    void SetDroppedFrames(uint32_t count) { localDroppedFrames = count; }

    // Set HDR status (called by backend hooks)
    void SetHDR(bool enabled) { isHDR = enabled; }

private:
    // Helpers
    void RenderTextWithOutline(const char* text, ImU32 color, bool outline, ImU32 outlineColor, float thickness);
    ImU32 GetLoadColor(float load, const struct OverlayConfig& cfg);

    PerformanceMetrics* metrics = nullptr;
    IPCClient* ipc = nullptr;
    void* hwnd = nullptr;
    char graphicsAPI[16] = "";  // "DX12", "DX11", "Vulkan"
    bool initialized = false;
    bool headless = false;
    ImGuiContext* context = nullptr;

    // Text update throttling
    DWORD lastTextUpdateTime = 0;
    bool isHDR = false;

    // Cached values (updated every textUpdateInterval)
    float cachedFPS = 0.0f;
    float cachedAvgFPS = 0.0f;
    float cached1PercentLow = 0.0f;
    float cached01PercentLow = 0.0f;
    int cachedRecordingSeconds = 0;
    bool cachedIsRecording = false;

    // Dropped frames tracking
    uint32_t localDroppedFrames = 0;        // Set by capture hooks via SetDroppedFrames()
    uint32_t cachedTotalDroppedFrames = 0;  // Combined total from all sources

    SystemMetrics cachedMetrics;

    ImFont* mainFont = nullptr;

    float initialDpiScale = 1.0f;  // Scale at initialization time

    DrawResult lastDrawResult = DrawResult::Unknown;
};

// Global overlay instance (defined in overlay.cpp)
extern Overlay g_SharedOverlay;
