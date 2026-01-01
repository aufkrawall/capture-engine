#pragma once

#include "ipc_client.h"
#include "performance_metrics.h"
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

  // Render overlay UI widgets (call between ImGui::NewFrame and ImGui::Render)
  void RenderUI() {
    // Skip if window is minimized or hidden (save resources)
    if (hwnd && IsWindow((HWND)hwnd) && !IsWindowVisible((HWND)hwnd))
      return;

    // Get DPI scale
    float dpiScale = GetDpiScale();
    ImGui::GetIO().FontGlobalScale = dpiScale;

    // Read config from shared memory
    bool showOverlay = true;
    bool showFPS = true;
    if (ipc && ipc->GetSharedMem()) {
      showOverlay = ipc->GetSharedMem()->overlayConfig.showOverlay;
      showFPS = ipc->GetSharedMem()->overlayConfig.showFPS;
    }

    if (!showOverlay)
      return;

    // Throttle text updates to reduce overhead (0.5 second interval)
    DWORD now = GetTickCount();
    bool shouldUpdateText =
        (now - lastTextUpdateTime) >= TEXT_UPDATE_INTERVAL_MS;
    if (shouldUpdateText) {
      lastTextUpdateTime = now;

      // Update cached FPS value
      if (metrics) {
        cachedFPS = metrics->GetCurrentFPS();
      }

      // Update cached recording time
      if (ipc && ipc->IsRecording() && ipc->GetSharedMem()) {
        int64_t duration = GetTickCount64() -
                           ipc->GetSharedMem()->runtimeState.recordingStartTime;
        cachedRecordingSeconds = (int)(duration / 1000);
        cachedIsRecording = true;
        
        // Read ALL dropped frame sources:
        // 1. localDroppedFrames - set by capture hooks (CaptureBase::droppedFrames)
        // 2. Ring buffer drops - in shared memory (frameRing.droppedFrames)
        // 3. Encoder/host drops - in shared memory (hostDroppedFrames)
        uint32_t ringDrops = ipc->GetSharedMem()->frameRing.droppedFrames.load(std::memory_order_relaxed);
        uint32_t hostDrops = ipc->GetSharedMem()->runtimeState.hostDroppedFrames;
        cachedTotalDroppedFrames = localDroppedFrames + ringDrops + hostDrops;
        
        // Read watertight smoothness indicators (dropped frames only)
        // Late frame counter disabled - unreliable with async GPU wait
      } else {
        cachedIsRecording = false;
      }
    }

    // Positioning/Sizing (use dynamic DPI scale)
    ImGui::SetNextWindowPos(ImVec2(10.0f * dpiScale, 10.0f * dpiScale), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.35f);

    if (ImGui::Begin(
            "Overlay", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {

      ImGui::Text("CaptureEngine");

      // Recording timer (uses cached value, updated every 0.5s)
      if (cachedIsRecording) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
        ImGui::Text("%02d:%02d:%02d", cachedRecordingSeconds / 3600,
                    (cachedRecordingSeconds % 3600) / 60,
                    cachedRecordingSeconds % 60);
        ImGui::PopStyleColor();
      } else {
        ImGui::Text("Ready");
      }

      // FPS counter with API name
      if (showFPS && metrics) {
        // Show "API: X FPS" format (e.g., "DX12: 120 FPS")
        if (graphicsAPI[0] != '\0') {
          ImGui::Text("%s: %.0f FPS", graphicsAPI, cachedFPS);
        } else {
          ImGui::Text("FPS: %.1f", cachedFPS);
        }

        // Frame time graph (updates every frame for smooth visualization)
        float minScale, maxScale;
        metrics->GetSmartScale(minScale, maxScale);
        ImGui::PlotLines("##FrameTime", metrics->GetHistoryArray(),
                         PerformanceMetrics::HISTORY_SIZE,
                         metrics->GetHistoryIndex(), nullptr, minScale,
                         maxScale, ImVec2(0, 50));

        // Show smoothness indicators during recording
        if (cachedIsRecording) {
          if (cachedTotalDroppedFrames > 0) {
            // Yellow warning for dropped frames
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "Dropped: %u", cachedTotalDroppedFrames);
          }
        }
      }
    }
    ImGui::End();
  }
  
  // Set dropped frames count from capture side (called by capture hooks)
  void SetDroppedFrames(uint32_t count) { localDroppedFrames = count; }

private:
  PerformanceMetrics *metrics = nullptr;
  IPCClient *ipc = nullptr;
  void *hwnd = nullptr;
  char graphicsAPI[16] = "";  // "DX12", "DX11", "Vulkan"

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
};

// Global overlay instance (defined in overlay.cpp)
extern Overlay g_SharedOverlay;

