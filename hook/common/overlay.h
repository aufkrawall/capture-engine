#pragma once

#include "ipc_client.h"
#include "performance_metrics.h"
#include "system_metrics.h"
#include "backends/imgui_impl_win32.h"
#include <imgui.h>
#include <windows.h>
#include <cstring>

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

  void SetMetrics(PerformanceMetrics *m) { metrics = m; }
  void SetIPCClient(IPCClient *ipc) { this->ipc = ipc; }
  void SetHwnd(void *hwnd) { this->hwnd = hwnd; }
  void SetGraphicsAPI(const char* api) { 
    strncpy(graphicsAPI, api, sizeof(graphicsAPI) - 1);
    graphicsAPI[sizeof(graphicsAPI) - 1] = '\0';
  }

  // Get DPI scale for current window
  float GetDpiScale() const {
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

  // Common ImGui frame lifecycle
  void BeginFrame();
  void EndFrame();

  // Render overlay UI widgets (call between BeginFrame and EndFrame)
  void RenderUI();

  DrawResult GetLastDrawResult() const { return lastDrawResult; }
  const char* GetLastDrawReason() const {
    switch (lastDrawResult) {
    case DrawResult::Drawn: return "drawn";
    case DrawResult::SkippedNotInitialized: return "not_initialized";
    case DrawResult::SkippedNoContext: return "no_context";
    case DrawResult::SkippedWindowHidden: return "window_hidden";
    case DrawResult::SkippedNoIPC: return "no_ipc";
    case DrawResult::SkippedShowDisabled: return "show_disabled";
    default: return "unknown";
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

  PerformanceMetrics *metrics = nullptr;
  IPCClient *ipc = nullptr;
  void *hwnd = nullptr;
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
  uint32_t localDroppedFrames = 0;      // Set by capture hooks via SetDroppedFrames()
  uint32_t cachedTotalDroppedFrames = 0; // Combined total from all sources
  
  SystemMetrics cachedMetrics; 

  ImFont* mainFont = nullptr;

  float initialDpiScale = 1.0f; // Scale at initialization time

  DrawResult lastDrawResult = DrawResult::Unknown;
};

// Global overlay instance (defined in overlay.cpp)
extern Overlay g_SharedOverlay;
