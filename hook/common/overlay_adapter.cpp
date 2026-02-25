/**
 * Overlay Adapter Implementation
 *
 * Creates the appropriate backend and renders overlay using CustomOverlay.
 */

#include "overlay_adapter.h"
#include "custom_font.h"
#include "custom_overlay.h"
#include "fg_detection.h"
#include "hook_common.h"

// Include backends based on build context
// VK_LAYER_CE_OVERLAY is defined when building the Vulkan layer
#ifndef VK_LAYER_CE_OVERLAY
// Full backends for hook DLL
#include "custom_overlay_dx10.h"
#include "custom_overlay_dx11.h"
#include "custom_overlay_dx12.h"
#include "custom_overlay_dx9.h"
#include "custom_overlay_gl.h"
#endif
#include "custom_overlay_vk.h"

#include <cstdio>
#include <cstring>

// Global adapter instance
OverlayAdapter g_OverlayAdapter;

// Helper to detect Windows DPI scaling
static float GetWindowsDpiScale() {
  // Try to get DPI from the active window monitor
  HWND hwnd = GetForegroundWindow();
  if (!hwnd) {
    hwnd = GetDesktopWindow();
  }

  // Try GetDpiForWindow first (Windows 10 1607+)
  typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
  static GetDpiForWindowFn getDpiForWindow = (GetDpiForWindowFn)GetProcAddress(
      GetModuleHandleA("user32.dll"), "GetDpiForWindow");

  if (getDpiForWindow) {
    UINT dpi = getDpiForWindow(hwnd);
    if (dpi > 0) {
      return dpi / 96.0f;
    }
  }

  // Fallback to GetDeviceCaps
  HDC hdc = GetDC(hwnd);
  if (hdc) {
    int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(hwnd, hdc);
    if (dpiX > 0) {
      return dpiX / 96.0f;
    }
  }

  // Default to 1.0 if detection fails
  return 1.0f;
}

OverlayAdapter::OverlayAdapter() {
  memset(graphicsAPI, 0, sizeof(graphicsAPI));
}

OverlayAdapter::~OverlayAdapter() {
  if (skipDeviceRelease) {
    if (renderer) {
      renderer->SetSkipDeviceRelease(true);
      delete renderer;
      renderer = nullptr;
    }
    if (backend) {
      delete backend;
      backend = nullptr;
    }
    initialized = false;
    return;
  }
  Shutdown();
}

void OverlayAdapter::SetGraphicsAPI(const char *api) {
  strncpy(graphicsAPI, api, sizeof(graphicsAPI) - 1);
  graphicsAPI[sizeof(graphicsAPI) - 1] = '\0';
}

bool OverlayAdapter::InitDX9(void *device) {
#ifndef VK_LAYER_CE_OVERLAY
  if (initialized)
    return true;
  if (!device) {
    HookLogImportant("[Overlay] InitDX9 failed: null device pointer");
    return false;
  }

  HookLogImportant("[Overlay] Initializing DX9 backend (device=%p)", device);
  auto dx9Backend = new CustomOverlay::DX9Backend((IDirect3DDevice9 *)device);
  backend = dx9Backend;
  backendType = OverlayBackendType::DX9;

  renderer = new CustomOverlay::Renderer();
  float dpiScale = GetWindowsDpiScale();
  if (!renderer->Initialize(backend, dpiScale)) {
    HookLogImportant("[Overlay] InitDX9: Renderer::Initialize FAILED (dpiScale=%.2f)", dpiScale);
    delete renderer;
    delete backend;
    renderer = nullptr;
    backend = nullptr;
    return false;
  }

  initialized = true;
  HookLogImportant("[Overlay] DX9 backend initialized successfully (dpiScale=%.2f)", dpiScale);
#endif
  return true;
}

bool OverlayAdapter::InitDX10(void *device) {
#ifndef VK_LAYER_CE_OVERLAY
  if (initialized)
    return true;
  if (!device) {
    HookLogImportant("[Overlay] InitDX10 failed: null device pointer");
    return false;
  }

  HookLogImportant("[Overlay] Initializing DX10 backend (device=%p)", device);
  auto dx10Backend = new CustomOverlay::DX10Backend((ID3D10Device *)device);
  backend = dx10Backend;
  backendType = OverlayBackendType::DX10;

  renderer = new CustomOverlay::Renderer();
  float dpiScale = GetWindowsDpiScale();
  if (!renderer->Initialize(backend, dpiScale)) {
    HookLogImportant("[Overlay] InitDX10: Renderer::Initialize FAILED (dpiScale=%.2f)", dpiScale);
    delete renderer;
    delete backend;
    renderer = nullptr;
    backend = nullptr;
    return false;
  }

  initialized = true;
  HookLogImportant("[Overlay] DX10 backend initialized successfully (dpiScale=%.2f)", dpiScale);
#endif
  return true;
}

bool OverlayAdapter::InitDX11(void *device, void *context) {
#ifndef VK_LAYER_CE_OVERLAY
  if (initialized)
    return true;
  if (!device || !context) {
    HookLogImportant("[Overlay] InitDX11 failed: null device=%p context=%p", device, context);
    return false;
  }

  HookLogImportant("[Overlay] Initializing DX11 backend (device=%p, context=%p)", device, context);
  auto dx11Backend = new CustomOverlay::DX11Backend(
      (ID3D11Device *)device, (ID3D11DeviceContext *)context);
  backend = dx11Backend;
  backendType = OverlayBackendType::DX11;

  renderer = new CustomOverlay::Renderer();
  float dpiScale = GetWindowsDpiScale();
  if (!renderer->Initialize(backend, dpiScale)) {
    HookLogImportant("[Overlay] InitDX11: Renderer::Initialize FAILED (dpiScale=%.2f)", dpiScale);
    delete renderer;
    delete backend;
    renderer = nullptr;
    backend = nullptr;
    return false;
  }

  initialized = true;
  HookLogImportant("[Overlay] DX11 backend initialized successfully (dpiScale=%.2f)", dpiScale);
#endif
  return true;
}

bool OverlayAdapter::InitDX12(void *device, void *queue, int rtvFormat) {
#ifndef VK_LAYER_CE_OVERLAY
  if (initialized)
    return true;
  if (!device || !queue) {
    HookLogImportant("[Overlay] InitDX12 failed: null device=%p queue=%p", device, queue);
    return false;
  }

  HookLogImportant("[Overlay] Initializing DX12 backend (device=%p, queue=%p, fmt=%d)",
                   device, queue, rtvFormat);
  auto dx12Backend = new CustomOverlay::DX12Backend((ID3D12Device *)device,
                                                    (ID3D12CommandQueue *)queue,
                                                    (DXGI_FORMAT)rtvFormat);
  backend = dx12Backend;
  backendType = OverlayBackendType::DX12;

  renderer = new CustomOverlay::Renderer();
  float dpiScale = GetWindowsDpiScale();
  if (!renderer->Initialize(backend, dpiScale)) {
    HookLogImportant("[Overlay] InitDX12: Renderer::Initialize FAILED (dpiScale=%.2f)", dpiScale);
    delete renderer;
    delete backend;
    renderer = nullptr;
    backend = nullptr;
    return false;
  }

  initialized = true;
  HookLogImportant("[Overlay] DX12 backend initialized successfully (fmt=%d, dpiScale=%.2f)",
                   rtvFormat, dpiScale);
#endif
  return true;
}

bool OverlayAdapter::InitOpenGL() {
#ifndef VK_LAYER_CE_OVERLAY
  if (initialized)
    return true;

  HookLogImportant("[Overlay] Initializing OpenGL backend");
  auto glBackend = new CustomOverlay::OpenGLBackend();
  backend = glBackend;
  backendType = OverlayBackendType::OpenGL;

  renderer = new CustomOverlay::Renderer();
  float dpiScale = GetWindowsDpiScale();
  if (!renderer->Initialize(backend, dpiScale)) {
    HookLogImportant("[Overlay] InitOpenGL: Renderer::Initialize FAILED (dpiScale=%.2f)", dpiScale);
    delete renderer;
    delete backend;
    renderer = nullptr;
    backend = nullptr;
    return false;
  }

  initialized = true;
  HookLogImportant("[Overlay] OpenGL backend initialized successfully (dpiScale=%.2f)", dpiScale);
#endif
  return true;
}

bool OverlayAdapter::InitVulkan(void *device, void *physDevice, void *queue,
                                uint32_t queueFamily, void *deviceDispatch,
                                void *instanceDispatch) {
  if (initialized)
    return true;
  if (!device) {
    HookLogImportant("[Overlay] InitVulkan failed: null device pointer");
    return false;
  }

  HookLogImportant("[Overlay] Initializing Vulkan backend (device=%p, queue=%p, family=%u)",
                   device, queue, queueFamily);

  auto vkBackend = new CustomOverlay::VulkanBackend(
      (VkDevice)device, (VkPhysicalDevice)physDevice, (VkQueue)queue,
      queueFamily);

  // Set dispatch tables before initialization
  if (deviceDispatch && instanceDispatch) {
    vkBackend->SetDispatchTable(deviceDispatch, instanceDispatch);
  }

  backend = vkBackend;
  backendType = OverlayBackendType::Vulkan;

  renderer = new CustomOverlay::Renderer();
  float dpiScale = GetWindowsDpiScale();
  if (!renderer->Initialize(backend, dpiScale)) {
    HookLogImportant("[Overlay] InitVulkan: Renderer::Initialize FAILED (dpiScale=%.2f)", dpiScale);
    delete renderer;
    delete backend;
    renderer = nullptr;
    backend = nullptr;
    return false;
  }

  initialized = true;
  HookLogImportant("[Overlay] Vulkan backend initialized successfully (dpiScale=%.2f)", dpiScale);
  return true;
}

void OverlayAdapter::SetShutdownMode(bool skipRelease) {
  skipDeviceRelease = skipRelease;
  if (renderer) {
    renderer->SetSkipDeviceRelease(skipRelease);
  }
#ifndef VK_LAYER_CE_OVERLAY
  if (backend && backendType == OverlayBackendType::DX11) {
    auto dx11Backend = static_cast<CustomOverlay::DX11Backend *>(backend);
    dx11Backend->SetSkipRelease(skipRelease);
  }
#endif
}

void OverlayAdapter::Shutdown() {
  if (renderer) {
    renderer->Shutdown();
    delete renderer;
    renderer = nullptr;
  }
  if (backend) {
    delete backend;
    backend = nullptr;
  }
  backendType = OverlayBackendType::None;
  initialized = false;
}

void OverlayAdapter::SetDX12RenderTarget(void *cmdList, void *rtvHandle) {
#ifndef VK_LAYER_CE_OVERLAY
  if (backendType == OverlayBackendType::DX12 && backend) {
    auto dx12Backend = static_cast<CustomOverlay::DX12Backend *>(backend);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    rtv.ptr = (SIZE_T)rtvHandle;
    dx12Backend->SetRenderTarget((ID3D12GraphicsCommandList *)cmdList, rtv);
  }
#endif
}

uint32_t OverlayAdapter::GetLoadColor(float load) {
  using namespace CustomOverlay::Colors;
  if (load < 50.0f)
    return LoadLow;
  if (load < 85.0f)
    return LoadMed;
  return LoadHigh;
}

void OverlayAdapter::RenderOverlay(int viewportWidth, int viewportHeight) {
  static int renderLogCount = 0;
  if (renderLogCount < 5) {
    HookLog("[Overlay] RenderOverlay#%d: init=%d renderer=%p ipc=%p shm=%p showOverlay=%d vp=%dx%d",
            renderLogCount, initialized ? 1 : 0, (void *)renderer, (void *)ipc,
            ipc ? (void *)ipc->GetSharedMem() : nullptr,
            (ipc && ipc->GetSharedMem())
                ? ipc->GetSharedMem()->ReadOverlayConfig().showOverlay
                : -1,
            viewportWidth, viewportHeight);
    renderLogCount++;
  }

  if (!initialized || !renderer) {
    if (renderLogCount < 5)
      HookLog("[Overlay] RenderOverlay: early return - not initialized or no renderer");
    return;
  }

  if (!ipc || !ipc->GetSharedMem()) {
    if (renderLogCount < 5)
      HookLog("[Overlay] RenderOverlay: early return - no IPC or shared memory");
    return;
  }
  auto cfg = ipc->GetSharedMem()->ReadOverlayConfig();
  if (!cfg.showOverlay) {
    if (renderLogCount < 5)
      HookLog("[Overlay] RenderOverlay: early return - showOverlay is false");
    return;
  }

  // Update throttling
  DWORD now = GetTickCount();
  bool shouldUpdate = (now - lastUpdateTime) >= cfg.textUpdateInterval;
  if (shouldUpdate) {
    lastUpdateTime = now;
    if (metrics) {
      cachedFPS = metrics->GetCurrentFPS();
      cachedAvgFPS = metrics->GetAverageFPS();
      cached1PercentLow = metrics->Get1PercentLowFPS();
      cached01PercentLow = metrics->Get01PercentLowFPS();
    }
    if (cfg.showCPU || cfg.showRAM || cfg.showGPU || cfg.showVRAM) {
      SystemMetricsCollector::Get().Update();
      cachedSystemMetrics = SystemMetricsCollector::Get().GetMetrics();
    }
  }

  // Pass HDR params to backend for shader constants
  if (backend) {
    float paperWhite = cfg.hdrPaperWhite;
    if (paperWhite <= 0.0f)
      paperWhite = 200.0f; // Default paper white nits
    int mode = 0;          // SDR
    if (isHDR) {
      // Detect HDR10/PQ (R10G10B10A2) vs scRGB (FP16) from render target format
      // DXGI_FORMAT_R10G10B10A2_UNORM = 24
      mode = (renderTargetFormat == 24) ? 2 : 1;
    }
    backend->SetHDRParams(mode, paperWhite);
  }

  renderer->BeginFrame(viewportWidth, viewportHeight);
  RenderContent(viewportWidth, viewportHeight);
  renderer->EndFrame();
}

void OverlayAdapter::RenderContent(int viewportWidth, int viewportHeight) {
  using namespace CustomOverlay;

  if (!ipc || !ipc->GetSharedMem())
    return;
  auto &mem = *ipc->GetSharedMem();
  auto cfg = mem.ReadOverlayConfig();

  // Get DPI scale for consistent sizing
  float dpiScale = renderer->GetDpiScale();

  // Calculate position (DPI-aware padding)
  float padding = (float)cfg.padding * dpiScale;
  float x = padding, y = padding;

  // Log position info on first few renders
  static int posLogCount = 0;
  if (posLogCount < 3) {
    HookLog(
        "[Overlay] RenderContent#%d: dpiScale=%.2f, vp=%dx%d, x=%.1f, y=%.1f, padding=%.1f",
        posLogCount, dpiScale, viewportWidth, viewportHeight, x, y, padding);
    posLogCount++;
  }

  float lineHeight = (float)renderer->GetLineHeight();

  // Check dynamic states used by both sizing and rendering
  bool fgActive = metrics && metrics->IsFGActive();
  bool isRecording =
      mem.runtimeState.isRecording.load(std::memory_order_acquire);

  // Adaptive overlay width: measure visible labels/values and size to content.
  const float kShadowPad = 1.0f;
  const float kBgLeftPad = 4.0f * dpiScale;
  const float kBgRightPad = 4.0f * dpiScale + kShadowPad;
  float kBgTopPad = 2.0f * dpiScale;
  if (cfg.showVRAM || cfg.showRAM) {
    kBgTopPad = (std::max)(kBgTopPad, lineHeight * 0.20f + kShadowPad);
  }
  const float kBgBottomPad = 2.0f * dpiScale + kShadowPad;
  const float kColumnGap = 10.0f * dpiScale;
  const float kMinContentWidth = 210.0f * dpiScale;
  const float kMemorySuffixScale = 0.75f;
  const float kMemoryGap = 2.0f * dpiScale;

  auto MeasureTextWidth = [&](const char *text) -> float {
    float w = 0, h = 0;
    renderer->CalcTextSize(text ? text : "", &w, &h);
    return w;
  };
  auto MeasureTextWidthScaled = [&](const char *text, float scale) -> float {
    float w = 0, h = 0;
    renderer->CalcTextSizeScaled(text ? text : "", &w, &h, scale);
    return w;
  };

  float maxLabelWidth = 0.0f;
  float maxValueWidth = 0.0f;
  char measureBuf[96];

  if (cfg.showGPU) {
    snprintf(measureBuf, sizeof(measureBuf), "%.0f%%",
             cachedSystemMetrics.gpuUsage);
    maxLabelWidth = (std::max)(maxLabelWidth,
                               MeasureTextWidth(SystemMetricsCollector::Get()
                                                    .GetGPUName()));
    maxValueWidth =
        (std::max)(maxValueWidth, MeasureTextWidth(measureBuf) + kShadowPad);
  }
  if (cfg.showCPU) {
    snprintf(measureBuf, sizeof(measureBuf), "%.0f%% (%.0f%%)",
             cachedSystemMetrics.cpuUsage, cachedSystemMetrics.cpuMaxCoreUsage);
    maxLabelWidth = (std::max)(maxLabelWidth,
                               MeasureTextWidth(SystemMetricsCollector::Get()
                                                    .GetCPUName()));
    maxValueWidth =
        (std::max)(maxValueWidth, MeasureTextWidth(measureBuf) + kShadowPad);
  }
  if (cfg.showVRAM) {
    float gbUsed =
        (float)cachedSystemMetrics.vramUsed / (1024.0f * 1024.0f * 1024.0f);
    float gbTotal =
        (float)cachedSystemMetrics.vramTotal / (1024.0f * 1024.0f * 1024.0f);
    if (gbTotal < 0.1f)
      gbTotal = 11.66f;
    char usedBuf[32], totalBuf[32];
    snprintf(usedBuf, sizeof(usedBuf), "%.2f GB", gbUsed);
    snprintf(totalBuf, sizeof(totalBuf), "of %.2f GB", gbTotal);
    float valueWidth = MeasureTextWidth(usedBuf) + kMemoryGap +
                       MeasureTextWidthScaled(totalBuf, kMemorySuffixScale) +
                       kShadowPad;
    maxLabelWidth = (std::max)(maxLabelWidth, MeasureTextWidth("VRAM"));
    maxValueWidth = (std::max)(maxValueWidth, valueWidth);
  }
  if (cfg.showRAM) {
    float gbUsed =
        (float)cachedSystemMetrics.ramUsed / (1024.0f * 1024.0f * 1024.0f);
    float gbTotal =
        (float)cachedSystemMetrics.ramTotal / (1024.0f * 1024.0f * 1024.0f);
    if (gbTotal < 0.1f)
      gbTotal = 31.93f;
    char usedBuf[32], totalBuf[32];
    snprintf(usedBuf, sizeof(usedBuf), "%.2f GB", gbUsed);
    snprintf(totalBuf, sizeof(totalBuf), "of %.2f GB", gbTotal);
    float valueWidth = MeasureTextWidth(usedBuf) + kMemoryGap +
                       MeasureTextWidthScaled(totalBuf, kMemorySuffixScale) +
                       kShadowPad;
    maxLabelWidth = (std::max)(maxLabelWidth, MeasureTextWidth("RAM"));
    maxValueWidth = (std::max)(maxValueWidth, valueWidth);
  }
  if (cfg.showFPS) {
    const char *apiLabel = graphicsAPI[0] ? graphicsAPI : "FPS";
    snprintf(measureBuf, sizeof(measureBuf), "%.0f FPS", cachedFPS);
    maxLabelWidth = (std::max)(maxLabelWidth, MeasureTextWidth(apiLabel));
    maxValueWidth =
        (std::max)(maxValueWidth, MeasureTextWidth(measureBuf) + kShadowPad);

    if (fgActive) {
      float baseFPS = metrics->GetFGBaseFPS();
      float outputFPS = metrics->GetFGOutputFPS();
      if (baseFPS < 1.0f)
        baseFPS = cachedFPS;
      if (outputFPS < 1.0f)
        outputFPS = cachedFPS;
      snprintf(measureBuf, sizeof(measureBuf), "%.0f / %.0f FPS", baseFPS,
               outputFPS);
      maxLabelWidth =
          (std::max)(maxLabelWidth, MeasureTextWidth("Base/Display"));
      maxValueWidth =
          (std::max)(maxValueWidth, MeasureTextWidth(measureBuf) + kShadowPad);
    }

    if (cachedAvgFPS > 0 && cached1PercentLow > 0) {
      snprintf(measureBuf, sizeof(measureBuf), "%.0f / %.0f / %.0f",
               cachedAvgFPS, cached1PercentLow, cached01PercentLow);
      maxLabelWidth =
          (std::max)(maxLabelWidth, MeasureTextWidth("Avg/1%/0.1%"));
      maxValueWidth =
          (std::max)(maxValueWidth, MeasureTextWidth(measureBuf) + kShadowPad);
    }
  }
  if (cfg.showFG && fgActive) {
    int multiplier = metrics->GetFGMultiplier();
    snprintf(measureBuf, sizeof(measureBuf), "FG %dx", multiplier);
    maxLabelWidth = (std::max)(maxLabelWidth, MeasureTextWidth("FG"));
    maxValueWidth =
        (std::max)(maxValueWidth, MeasureTextWidth(measureBuf) + kShadowPad);
  }

  float contentWidth = kMinContentWidth;
  if (maxLabelWidth > 0.0f || maxValueWidth > 0.0f) {
    contentWidth =
        (std::max)(contentWidth, maxLabelWidth + kColumnGap + maxValueWidth);
  }

  if (cfg.showRecording && isRecording) {
    int64_t startTime =
        mem.runtimeState.recordingStartTime.load(std::memory_order_acquire);
    int64_t elapsed = 0;
    if (startTime > 0) {
      elapsed = (GetTickCount64() - startTime) / 1000;
    }
    int hours = (int)(elapsed / 3600);
    int minutes = (int)((elapsed % 3600) / 60);
    int seconds = (int)(elapsed % 60);
    char recBuf[96];
    snprintf(recBuf, sizeof(recBuf), "REC %02d:%02d:%02d", hours, minutes,
             seconds);
    contentWidth =
        (std::max)(contentWidth, MeasureTextWidth(recBuf) + kShadowPad);
    snprintf(recBuf, sizeof(recBuf), "REC %02d:%02d:%02d !ENCODER OVERLOAD!",
             hours, minutes, seconds);
    contentWidth =
        (std::max)(contentWidth, MeasureTextWidth(recBuf) + kShadowPad);
  }

  float bgWidth = contentWidth + kBgLeftPad + kBgRightPad;
  float maxBgWidth = (float)viewportWidth - 2.0f * padding;
  maxBgWidth = (std::max)(maxBgWidth, 100.0f * dpiScale);
  if (bgWidth > maxBgWidth) {
    bgWidth = maxBgWidth;
    contentWidth =
        (std::max)(80.0f * dpiScale, bgWidth - kBgLeftPad - kBgRightPad);
  }

  // Right edge for right-aligned values (set after position calculation)
  float valueRightEdge = 0;

  switch (cfg.position) {
  case OverlayPosition::TopLeft:
    x = padding;
    y = padding;
    break;
  case OverlayPosition::TopRight:
    x = viewportWidth - padding - bgWidth;
    y = padding;
    break;
  case OverlayPosition::BottomLeft:
    x = padding;
    y = viewportHeight - padding - 200;
    break;
  case OverlayPosition::BottomRight:
    x = viewportWidth - padding - bgWidth;
    y = viewportHeight - padding - 200;
    break;
  }

  // Right edge for right-aligned values, derived from measured content width.
  valueRightEdge = x + contentWidth;

  // Calculate required height for background
  float requiredHeight = lineHeight; // Minimal top padding

  if (cfg.showGPU)
    requiredHeight += lineHeight;
  if (cfg.showCPU)
    requiredHeight += lineHeight;
  if (cfg.showVRAM)
    requiredHeight += lineHeight;
  if (cfg.showRAM)
    requiredHeight += lineHeight;

  if (cfg.showFPS) {
    requiredHeight += lineHeight;
    if (cachedAvgFPS > 0)
      requiredHeight += lineHeight;
    // Base/Display line when FG is active
    if (fgActive)
      requiredHeight += lineHeight;
  }

  // FG status line
  if (cfg.showFG && fgActive) {
    requiredHeight += lineHeight;
  }

  // Recording status line
  if (cfg.showRecording && isRecording) {
    requiredHeight += lineHeight;
  }

  // Pre-calculate graph layout for height calculation
  constexpr int GRAPH_SAMPLES = 180;
  float graphData[GRAPH_SAMPLES] = {};
  float graphY = 0, graphHeight = 0, graphWidth = 0, graphX = 0;
  float graphMinVal = 0, graphMaxVal = 0;
  bool showGraph = cfg.showFrameTime && metrics;

  if (showGraph) {
    requiredHeight += 4 * dpiScale;     // Gap before graph
    requiredHeight += 50.0f * dpiScale; // Graph height
    metrics->GetLastHistory(graphData, GRAPH_SAMPLES);
  }

  float bgHeight = requiredHeight + kBgTopPad + kBgBottomPad;

  // --- PASS 1: All solid geometry (background + graph) ---
  uint8_t bgAlpha = (uint8_t)(cfg.bgAlpha * 255);
  uint32_t bgColor = (bgAlpha << 24) | (cfg.bgColor & 0x00FFFFFF);
  renderer->DrawRectFilled(x - kBgLeftPad, y - kBgTopPad, bgWidth, bgHeight,
                           bgColor);

  // Calculate cursorY for graph position (same layout as text pass)
  float graphCursorY = y;
  if (cfg.showGPU)
    graphCursorY += lineHeight;
  if (cfg.showCPU)
    graphCursorY += lineHeight;
  if (cfg.showVRAM)
    graphCursorY += lineHeight;
  if (cfg.showRAM)
    graphCursorY += lineHeight;
  if (cfg.showFPS) {
    graphCursorY += lineHeight;
    if (cachedAvgFPS > 0 && cached1PercentLow > 0)
      graphCursorY += lineHeight;
    if (fgActive)
      graphCursorY += lineHeight;
  }
  if (cfg.showFG && fgActive)
    graphCursorY += lineHeight;
  if (cfg.showRecording && isRecording)
    graphCursorY += lineHeight;

  // Calculate max frame time from last ~2 seconds for display
  float recentMaxFrameTime = 0.0f;
  float recentAvgFrameTime = 0.0f;
  if (showGraph) {
    // Calculate how many samples represent ~2 seconds based on current FPS
    int samplesPerSecond = (cachedFPS > 0) ? (int)cachedFPS : 60;
    int samplesFor2Seconds = (std::min)(GRAPH_SAMPLES, samplesPerSecond * 2);
    int startIdx = GRAPH_SAMPLES - samplesFor2Seconds;

    float sum = 0.0f;
    int count = 0;
    for (int i = startIdx; i < GRAPH_SAMPLES; i++) {
      float val = graphData[i];
      if (val > recentMaxFrameTime)
        recentMaxFrameTime = val;
      sum += val;
      count++;
    }
    if (count > 0)
      recentAvgFrameTime = sum / count;

    // Calculate graph scaling based on all samples - use exact peak value
    float peakVal = 0.0f;
    for (int i = 0; i < GRAPH_SAMPLES; i++) {
      if (graphData[i] > peakVal)
        peakVal = graphData[i];
    }
    // Set max to peak value (rounded up nicely), with minimum of 20ms
    graphMaxVal = (std::max)(peakVal, 20.0f);

    graphWidth = bgWidth;
    graphHeight = 50.0f * dpiScale;
    graphX = x - 4 * dpiScale;
    graphY = graphCursorY + 4 * dpiScale; // Small gap below stats

    // Top padding keeps the graph line below the scale marker and frame time
    // labels that are drawn at graphY + ~6px.
    float graphTopPad = 14.0f * dpiScale;

    uint32_t graphColor =
        cfg.frametimeColor ? cfg.frametimeColor : Colors::Yellow;
    renderer->DrawFrameTimeGraph(graphX, graphY + graphTopPad, graphWidth,
                                 graphHeight - graphTopPad, graphData,
                                 GRAPH_SAMPLES, graphMinVal, graphMaxVal,
                                 graphColor);
  }

  // --- PASS 2: All text (single textured batch) ---
  float cursorY = y;
  char buf[64];

  uint32_t textColor = cfg.textColor ? cfg.textColor : Colors::White;
  uint32_t shadowColor =
      cfg.textOutlineColor ? cfg.textOutlineColor : Colors::Black;

  // Column positions for alignment (DPI-aware)
  float labelCol = x;

  // GPU - Name in green, % in yellow (based on load)
  if (cfg.showGPU) {
    snprintf(buf, 64, "%.0f%%", cachedSystemMetrics.gpuUsage);
    renderer->DrawTextWithShadow(labelCol, cursorY,
                                 SystemMetricsCollector::Get().GetGPUName(),
                                 Colors::LabelGreen, shadowColor);
    renderer->DrawTextRightAligned(valueRightEdge, cursorY, buf,
                                   GetLoadColor(cachedSystemMetrics.gpuUsage),
                                   shadowColor);
    cursorY += lineHeight;
  }

  // CPU - Name in green, % (maxCore%) in cyan
  if (cfg.showCPU) {
    snprintf(buf, 64, "%.0f%% (%.0f%%)", cachedSystemMetrics.cpuUsage,
             cachedSystemMetrics.cpuMaxCoreUsage);
    renderer->DrawTextWithShadow(labelCol, cursorY,
                                 SystemMetricsCollector::Get().GetCPUName(),
                                 Colors::LabelGreen, shadowColor);
    renderer->DrawTextRightAligned(valueRightEdge, cursorY, buf,
                                   Colors::ValueCyan, shadowColor);
    cursorY += lineHeight;
  }

  // VRAM - Label in orange, "X.XX GB" in white, "of Y.YY GB" in smaller raised
  // text
  if (cfg.showVRAM) {
    float gbUsed =
        (float)cachedSystemMetrics.vramUsed / (1024.0f * 1024.0f * 1024.0f);
    float gbTotal =
        (float)cachedSystemMetrics.vramTotal / (1024.0f * 1024.0f * 1024.0f);
    if (gbTotal < 0.1f)
      gbTotal = 11.66f; // Fallback if not detected

    renderer->DrawTextWithShadow(labelCol, cursorY, "VRAM", Colors::LabelOrange,
                                 shadowColor);

    char usedBuf[32];
    snprintf(usedBuf, 32, "%.2f GB", gbUsed);
    char totalBuf[32];
    snprintf(totalBuf, 32, "of %.2f GB", gbTotal);

    // Right-align the whole "used + of total" composite so it always fits.
    float usedWidth = 0, usedHeight = 0;
    float totalWidth = 0;
    renderer->CalcTextSize(usedBuf, &usedWidth, &usedHeight);
    float smallScale = 0.75f;    // Smaller scale for superscript
    float gap = 2.0f * dpiScale; // Minimal gap between segments
    renderer->CalcTextSizeScaled(totalBuf, &totalWidth, nullptr, smallScale);
    float usedX = valueRightEdge - (usedWidth + gap + totalWidth);
    renderer->DrawTextWithShadow(usedX, cursorY, usedBuf, Colors::LabelOrange,
                                 shadowColor);

    float raisedY = cursorY - usedHeight * 0.20f; // Raised 20% of line height
    renderer->DrawTextScaledWithShadow(usedX + usedWidth + gap, raisedY, totalBuf,
                                       textColor, shadowColor, smallScale);

    cursorY += lineHeight;
  }

  // RAM - Label in pink, "X.XX GB" in white, "of Y.YY GB" in smaller raised
  // text
  if (cfg.showRAM) {
    float gbUsed =
        (float)cachedSystemMetrics.ramUsed / (1024.0f * 1024.0f * 1024.0f);
    float gbTotal =
        (float)cachedSystemMetrics.ramTotal / (1024.0f * 1024.0f * 1024.0f);
    if (gbTotal < 0.1f)
      gbTotal = 31.93f; // Fallback if not detected

    renderer->DrawTextWithShadow(labelCol, cursorY, "RAM", Colors::LabelPink,
                                 shadowColor);

    char usedBuf[32];
    snprintf(usedBuf, 32, "%.2f GB", gbUsed);
    char totalBuf[32];
    snprintf(totalBuf, 32, "of %.2f GB", gbTotal);

    // Right-align the whole "used + of total" composite so it always fits.
    float usedWidth = 0, usedHeight = 0;
    float totalWidth = 0;
    renderer->CalcTextSize(usedBuf, &usedWidth, &usedHeight);
    float smallScale = 0.75f;    // Smaller scale for superscript
    float gap = 2.0f * dpiScale; // Minimal gap between segments
    renderer->CalcTextSizeScaled(totalBuf, &totalWidth, nullptr, smallScale);
    float usedX = valueRightEdge - (usedWidth + gap + totalWidth);
    renderer->DrawTextWithShadow(usedX, cursorY, usedBuf, Colors::LabelPink,
                                 shadowColor);

    float raisedY = cursorY - usedHeight * 0.20f; // Raised 20% of line height
    renderer->DrawTextScaledWithShadow(usedX + usedWidth + gap, raisedY, totalBuf,
                                       textColor, shadowColor, smallScale);

    cursorY += lineHeight;
  }

  // FPS Section
  if (cfg.showFPS) {
    // Graphics API label (if set) with current FPS
    const char *apiLabel = graphicsAPI[0] ? graphicsAPI : "FPS";
    snprintf(buf, 64, "%.0f FPS", cachedFPS);
    renderer->DrawTextWithShadow(labelCol, cursorY, apiLabel, textColor,
                                 shadowColor);
    renderer->DrawTextRightAligned(valueRightEdge, cursorY, buf, cfg.fpsColor,
                                   shadowColor);
    cursorY += lineHeight;

    // Base/Display FPS when FG is active (shown first as in reference)
    if (fgActive) {
      float baseFPS = metrics->GetFGBaseFPS();
      float outputFPS = metrics->GetFGOutputFPS();
      if (baseFPS < 1.0f)
        baseFPS = cachedFPS;
      if (outputFPS < 1.0f)
        outputFPS = cachedFPS;

      snprintf(buf, 64, "%.0f / %.0f FPS", baseFPS, outputFPS);
      renderer->DrawTextWithShadow(labelCol, cursorY, "Base/Display",
                                   Colors::LabelYellow, shadowColor);
      renderer->DrawTextRightAligned(valueRightEdge, cursorY, buf,
                                     Colors::ValueYellow, shadowColor);
      cursorY += lineHeight;
    }

    // Avg/1%/0.1%
    if (cachedAvgFPS > 0 && cached1PercentLow > 0) {
      snprintf(buf, 64, "%.0f / %.0f / %.0f", cachedAvgFPS, cached1PercentLow,
               cached01PercentLow);
      renderer->DrawTextWithShadow(labelCol, cursorY, "Avg/1%/0.1%", textColor,
                                   shadowColor);
      renderer->DrawTextRightAligned(valueRightEdge, cursorY, buf,
                                     Colors::ValueYellow, shadowColor);
      cursorY += lineHeight;
    }
  }

  // FG Status line
  if (cfg.showFG && fgActive) {
    int multiplier = metrics->GetFGMultiplier();
    snprintf(buf, 64, "FG %dx", multiplier);
    renderer->DrawTextWithShadow(labelCol, cursorY, "FG", Colors::LabelCyan,
                                 shadowColor);
    renderer->DrawTextRightAligned(valueRightEdge, cursorY, buf,
                                   Colors::LabelCyan, shadowColor);
    cursorY += lineHeight;
  }

  // Recording status line
  if (cfg.showRecording && isRecording) {
    int64_t startTime =
        mem.runtimeState.recordingStartTime.load(std::memory_order_acquire);
    int64_t elapsed = 0;
    if (startTime > 0) {
      elapsed = (GetTickCount64() - startTime) / 1000;
    }
    int hours = (int)(elapsed / 3600);
    int minutes = (int)((elapsed % 3600) / 60);
    int seconds = (int)(elapsed % 60);

    // Check for encoder overload (flags: bit 0 = encoder, bit 1 = mux)
    uint32_t overloadFlags =
        mem.runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
    uint64_t nowTick = GetTickCount64();
    if (overloadFlags & 1u) {
      // Encoder overload detected - update last detection time
      lastEncoderOverloadTick = nowTick;
    }

    // Show overload warning if within 5 seconds of last detection
    bool showOverloadWarning =
        (lastEncoderOverloadTick != 0) &&
        ((nowTick - lastEncoderOverloadTick) <= 5000);

    if (showOverloadWarning) {
      // Show recording time with overload warning
      snprintf(buf, 64, "REC %02d:%02d:%02d !ENCODER OVERLOAD!", hours, minutes,
               seconds);
      renderer->DrawTextWithShadow(labelCol, cursorY, buf, Colors::Red,
                                   shadowColor);
    } else {
      // Normal recording display
      snprintf(buf, 64, "REC %02d:%02d:%02d", hours, minutes, seconds);
      renderer->DrawTextWithShadow(labelCol, cursorY, buf, Colors::Red,
                                   shadowColor);
    }
    cursorY += lineHeight;
  } else {
    // Reset overload tracking when not recording
    lastEncoderOverloadTick = 0;
  }

  // Frame time graph labels and markers
  if (showGraph) {
    uint32_t graphLabelColor = Colors::Green;
    uint32_t grayColor = 0xFF808080; // Gray for scale marker
    float smallFontScale = 0.75f;    // Smaller font for graph labels

    // Scale marker: small gray line at top left with ceiling value (with ms
    // unit)
    float scaleLineLength = 15.0f * dpiScale;
    float scaleLineY = graphY + 1.0f * dpiScale;
    renderer->DrawLine(graphX + 4 * dpiScale, scaleLineY,
                       graphX + 4 * dpiScale + scaleLineLength, scaleLineY,
                       grayColor, 1.0f * dpiScale);

    // Scale marker text (ceiling value with ms unit in small gray font)
    snprintf(buf, 64, "%.0f ms", graphMaxVal);
    float scaleTextWidth = 0, scaleTextHeight = 0;
    renderer->CalcTextSizeScaled(buf, &scaleTextWidth, &scaleTextHeight,
                                 smallFontScale);
    renderer->DrawTextScaledWithShadow(graphX + 6 * dpiScale + scaleLineLength,
                                       scaleLineY - scaleTextHeight * 0.5f, buf,
                                       grayColor, shadowColor, smallFontScale);

    // Current frame time display at top right of graph
    // Color based on comparison with average: green (close), yellow (spike),
    // red (stutter)
    uint32_t frameTimeColor = Colors::Green;
    if (recentAvgFrameTime > 0.001f) {
      float ratio = recentMaxFrameTime / recentAvgFrameTime;
      if (ratio > 2.0f) {
        frameTimeColor = Colors::Red; // Bad stutter (2x average)
      } else if (ratio > 1.5f) {
        frameTimeColor = Colors::Yellow; // Moderate spike (1.5x average)
      }
    }

    snprintf(buf, 64, "%.1f ms", recentMaxFrameTime);
    float ftTextWidth = 0, ftTextHeight = 0;
    renderer->CalcTextSizeScaled(buf, &ftTextWidth, &ftTextHeight,
                                 smallFontScale);
    renderer->DrawTextScaledWithShadow(
        graphX + graphWidth - ftTextWidth - 4 * dpiScale,
        scaleLineY - ftTextHeight * 0.5f, buf, frameTimeColor, shadowColor,
        smallFontScale);
  }

  (void)viewportWidth;
  (void)viewportHeight;
}
