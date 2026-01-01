#pragma once

#include "ipc_client.h"
#include "performance_metrics.h"
#include "system_metrics.h"
#include <backends/imgui_impl_win32.h>
#include <imgui.h>
#include <windows.h>
#include <cstring>

// Shared overlay UI rendering - API-agnostic ImGui widgets
// Each graphics API calls RenderUI() after setting up ImGui frame
class Overlay {
public:
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

  // Common ImGui initialization (API-agnostic part)
  void InitImGui(void* hwnd);

  // Common ImGui shutdown
  void ShutdownImGui();

  // Common ImGui frame lifecycle
  void BeginFrame();
  void EndFrame();

  // Render overlay UI widgets (call between BeginFrame and EndFrame)
  void RenderUI();
  
  // Set dropped frames count from capture side (called by capture hooks)
  void SetDroppedFrames(uint32_t count) { localDroppedFrames = count; }

private:
  // Helpers
  void RenderTextWithOutline(const char* text, ImU32 color, bool outline, ImU32 outlineColor, float thickness);
  ImU32 GetLoadColor(float load, const struct OverlayConfig& cfg);

  PerformanceMetrics *metrics = nullptr;
  IPCClient *ipc = nullptr;
  void *hwnd = nullptr;
  char graphicsAPI[16] = "";  // "DX12", "DX11", "Vulkan"
  bool initialized = false;

  // Text update throttling (500ms interval)
  static constexpr DWORD TEXT_UPDATE_INTERVAL_MS = 500;
  DWORD lastTextUpdateTime = 0;

  // Cached values (updated every TEXT_UPDATE_INTERVAL_MS)
  float cachedFPS = 0.0f;
  int cachedRecordingSeconds = 0;
  bool cachedIsRecording = false;
  
  // Dropped frames tracking
  uint32_t localDroppedFrames = 0;      // Set by capture hooks via SetDroppedFrames()
  uint32_t cachedTotalDroppedFrames = 0; // Combined total from all sources
  
  ImFont* mainFont = nullptr;
};

// Global overlay instance (defined in overlay.cpp)
extern Overlay g_SharedOverlay;
