/**
 * Overlay Adapter Implementation
 *
 * Creates the appropriate backend and renders overlay using CustomOverlay.
 */

#include "overlay_adapter.h"
#include "custom_font.h"
#include "custom_overlay.h"
#include "fg_detection.h"

// Include backends based on build context
// VK_LAYER_CE_OVERLAY is defined when building the Vulkan layer
#ifndef VK_LAYER_CE_OVERLAY
// Full backends for hook DLL
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

OverlayAdapter::OverlayAdapter() {
  memset(graphicsAPI, 0, sizeof(graphicsAPI));
}

OverlayAdapter::~OverlayAdapter() { Shutdown(); }

void OverlayAdapter::SetGraphicsAPI(const char *api) {
  strncpy(graphicsAPI, api, sizeof(graphicsAPI) - 1);
  graphicsAPI[sizeof(graphicsAPI) - 1] = '\0';
}

bool OverlayAdapter::InitDX9(void *device) {
#ifndef VK_LAYER_CE_OVERLAY
  if (initialized)
    return true;
  if (!device)
    return false;

  auto dx9Backend = new CustomOverlay::DX9Backend((IDirect3DDevice9 *)device);
  backend = dx9Backend;
  backendType = OverlayBackendType::DX9;

  renderer = new CustomOverlay::Renderer();
  float dpiScale = 1.0f; // DX9 games typically don't support high DPI
  if (!renderer->Initialize(backend, dpiScale)) {
    delete renderer;
    delete backend;
    renderer = nullptr;
    backend = nullptr;
    return false;
  }

  initialized = true;
  OutputDebugStringA("[OverlayAdapter] Initialized DX9 backend\n");
#endif
  return true;
}

bool OverlayAdapter::InitDX11(void *device, void *context) {
#ifndef VK_LAYER_CE_OVERLAY
  if (initialized)
    return true;
  if (!device || !context)
    return false;

  auto dx11Backend = new CustomOverlay::DX11Backend(
      (ID3D11Device *)device, (ID3D11DeviceContext *)context);
  backend = dx11Backend;
  backendType = OverlayBackendType::DX11;

  renderer = new CustomOverlay::Renderer();
  if (!renderer->Initialize(backend, 1.0f)) {
    delete renderer;
    delete backend;
    renderer = nullptr;
    backend = nullptr;
    return false;
  }

  initialized = true;
  OutputDebugStringA("[OverlayAdapter] Initialized DX11 backend\n");
#endif
  return true;
}

bool OverlayAdapter::InitDX12(void *device, void *queue, int rtvFormat) {
#ifndef VK_LAYER_CE_OVERLAY
  if (initialized)
    return true;
  if (!device || !queue)
    return false;

  auto dx12Backend = new CustomOverlay::DX12Backend((ID3D12Device *)device,
                                                    (ID3D12CommandQueue *)queue,
                                                    (DXGI_FORMAT)rtvFormat);
  backend = dx12Backend;
  backendType = OverlayBackendType::DX12;

  renderer = new CustomOverlay::Renderer();
  if (!renderer->Initialize(backend, 1.0f)) {
    delete renderer;
    delete backend;
    renderer = nullptr;
    backend = nullptr;
    return false;
  }

  initialized = true;
  OutputDebugStringA("[OverlayAdapter] Initialized DX12 backend\n");
#endif
  return true;
}

bool OverlayAdapter::InitOpenGL() {
#ifndef VK_LAYER_CE_OVERLAY
  if (initialized)
    return true;

  auto glBackend = new CustomOverlay::OpenGLBackend();
  backend = glBackend;
  backendType = OverlayBackendType::OpenGL;

  renderer = new CustomOverlay::Renderer();
  if (!renderer->Initialize(backend, 1.0f)) {
    delete renderer;
    delete backend;
    renderer = nullptr;
    backend = nullptr;
    return false;
  }

  initialized = true;
  OutputDebugStringA("[OverlayAdapter] Initialized OpenGL backend\n");
#endif
  return true;
}

bool OverlayAdapter::InitVulkan(void *device, void *physDevice, void *queue,
                                uint32_t queueFamily, void *deviceDispatch,
                                void *instanceDispatch) {
  if (initialized)
    return true;
  if (!device)
    return false;

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
  HookLog("OverlayAdapter::InitVulkan - Created renderer, initializing...");
  if (!renderer->Initialize(backend, 1.0f)) {
    HookLog("OverlayAdapter::InitVulkan - Renderer initialization failed");
    delete renderer;
    delete backend;
    renderer = nullptr;
    backend = nullptr;
    return false;
  }

  HookLog("OverlayAdapter::InitVulkan - Renderer initialized successfully");
  initialized = true;
  HookLog("OverlayAdapter::InitVulkan - Set initialized=true");
  OutputDebugStringA("[OverlayAdapter] Initialized Vulkan backend\n");
  HookLog("OverlayAdapter::InitVulkan - About to return true");
  return true;
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
  // DIAGNOSTIC: Log guard condition outcomes for first 20 calls
  static int s_renderCallCount = 0;
  bool shouldLog = (++s_renderCallCount <= 20);

  if (!initialized || !renderer) {
    if (shouldLog)
      HookLog(
          "OverlayAdapter::RenderOverlay - SKIP: initialized=%d, renderer=%p",
          initialized, renderer);
    return;
  }

  // Check if overlay should be shown
  if (!ipc || !ipc->GetSharedMem()) {
    if (shouldLog)
      HookLog("OverlayAdapter::RenderOverlay - SKIP: ipc=%p, sharedMem=%p", ipc,
              ipc ? ipc->GetSharedMem() : nullptr);
    return;
  }
  auto &cfg = ipc->GetSharedMem()->overlayConfig;
  if (!cfg.showOverlay) {
    if (shouldLog)
      HookLog("OverlayAdapter::RenderOverlay - SKIP: showOverlay=false");
    return;
  }

  if (shouldLog)
    HookLog("OverlayAdapter::RenderOverlay - RENDERING (%dx%d)", viewportWidth,
            viewportHeight);

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

  renderer->BeginFrame(viewportWidth, viewportHeight);
  RenderContent(viewportWidth, viewportHeight);
  renderer->EndFrame();
}

void OverlayAdapter::RenderContent(int viewportWidth, int viewportHeight) {
  using namespace CustomOverlay;

  if (!ipc || !ipc->GetSharedMem())
    return;
  auto &mem = *ipc->GetSharedMem();
  auto &cfg = mem.overlayConfig;

  // Calculate position
  float padding = (float)cfg.padding;
  float x = padding, y = padding;

  switch (cfg.position) {
  case OverlayPosition::TopLeft:
    x = padding;
    y = padding;
    break;
  case OverlayPosition::TopRight:
    x = viewportWidth - padding - 200; // Approximate width
    y = padding;
    break;
  case OverlayPosition::BottomLeft:
    x = padding;
    y = viewportHeight - padding - 200; // Approximate height (increased for graph)
    break;
  case OverlayPosition::BottomRight:
    x = viewportWidth - padding - 200;
    y = viewportHeight - padding - 200;
    break;
  }

  // Calculate required height for background
  float bgWidth = 200, bgHeight = 150;
  float lineHeight = (float)renderer->GetLineHeight();
  float requiredHeight = lineHeight * 2; // Base padding

  if (cfg.showGPU) requiredHeight += lineHeight;
  if (cfg.showCPU) requiredHeight += lineHeight;
  if (cfg.showVRAM) requiredHeight += lineHeight;
  if (cfg.showRAM) requiredHeight += lineHeight;
  if (cfg.showFPS) {
    requiredHeight += lineHeight;
    if (cachedAvgFPS > 0) requiredHeight += lineHeight;
  }
  if (cfg.showFrameTime) {
    requiredHeight += 40.0f * renderer->GetDpiScale() + lineHeight + 8;
  }

  bgHeight = (std::max)(bgHeight, requiredHeight + 8);

  // Draw background
  uint8_t bgAlpha = (uint8_t)(cfg.bgAlpha * 255);
  uint32_t bgColor = (bgAlpha << 24) | (cfg.bgColor & 0x00FFFFFF);
  renderer->DrawRectFilled(x - 8, y - 4, bgWidth, bgHeight, bgColor);

  float cursorY = y;
  char buf[64];

  uint32_t textColor = cfg.textColor ? cfg.textColor : Colors::White;
  uint32_t shadowColor =
      cfg.textOutlineColor ? cfg.textOutlineColor : Colors::Black;

  // GPU
  if (cfg.showGPU) {
    snprintf(buf, 64, "%.0f%%", cachedSystemMetrics.gpuUsage);
    renderer->DrawTextWithShadow(x, cursorY,
                                 SystemMetricsCollector::Get().GetGPUName(),
                                 cfg.gpuColor, shadowColor);
    renderer->DrawTextWithShadow(x + 100, cursorY, buf,
                                 GetLoadColor(cachedSystemMetrics.gpuUsage),
                                 shadowColor);
    cursorY += lineHeight;
  }

  // CPU
  if (cfg.showCPU) {
    snprintf(buf, 64, "%.0f%%", cachedSystemMetrics.cpuUsage);
    renderer->DrawTextWithShadow(x, cursorY,
                                 SystemMetricsCollector::Get().GetCPUName(),
                                 cfg.cpuColor, shadowColor);
    renderer->DrawTextWithShadow(x + 100, cursorY, buf,
                                 GetLoadColor(cachedSystemMetrics.cpuUsage),
                                 shadowColor);
    cursorY += lineHeight;
  }

  // VRAM
  if (cfg.showVRAM) {
    float gbUsed =
        (float)cachedSystemMetrics.vramUsed / (1024.0f * 1024.0f * 1024.0f);
    snprintf(buf, 64, "%.2f GB", gbUsed);
    renderer->DrawTextWithShadow(x, cursorY, "VRAM", cfg.vramColor,
                                 shadowColor);
    renderer->DrawTextWithShadow(x + 100, cursorY, buf, textColor, shadowColor);
    cursorY += lineHeight;
  }

  // RAM
  if (cfg.showRAM) {
    float gbUsed =
        (float)cachedSystemMetrics.ramUsed / (1024.0f * 1024.0f * 1024.0f);
    snprintf(buf, 64, "%.2f GB", gbUsed);
    renderer->DrawTextWithShadow(x, cursorY, "RAM", cfg.ramColor, shadowColor);
    renderer->DrawTextWithShadow(x + 100, cursorY, buf, textColor, shadowColor);
    cursorY += lineHeight;
  }

  // FPS
  if (cfg.showFPS) {
    snprintf(buf, 64, "%.0f FPS", cachedFPS);
    const char *label = graphicsAPI[0] ? graphicsAPI : "FPS";
    renderer->DrawTextWithShadow(x, cursorY, label, textColor, shadowColor);
    renderer->DrawTextWithShadow(x + 100, cursorY, buf, cfg.fpsColor,
                                 shadowColor);
    cursorY += lineHeight;

    // FPS stats
    if (cachedAvgFPS > 0 && cached1PercentLow > 0) {
      snprintf(buf, 64, "%.0f/%.0f/%.0f", cachedAvgFPS, cached1PercentLow,
               cached01PercentLow);
      renderer->DrawTextWithShadow(x, cursorY, "Avg/1%/0.1%", textColor,
                                   shadowColor);
      renderer->DrawTextWithShadow(x + 100, cursorY, buf, cfg.fpsColor,
                                   shadowColor);
      cursorY += lineHeight;
    }
  }

  // Frame Time Graph (updates every frame for smooth animation)
  if (cfg.showFrameTime && metrics) {
    constexpr int GRAPH_SAMPLES = 180;
    float graphData[GRAPH_SAMPLES];
    metrics->GetLastHistory(graphData, GRAPH_SAMPLES);

    // Calculate smart scale with minimum range to prevent over-dramatization
    float minVal = 0.0f;
    float maxVal = 0.0f;
    float peakVal = 0.0f;

    for (int i = 0; i < GRAPH_SAMPLES; i++) {
      if (graphData[i] > peakVal)
        peakVal = graphData[i];
    }

    // Minimum range of 20ms to prevent jittery appearance
    float minRange = 20.0f;
    // Scale to 110% of peak, but at least minRange
    maxVal = (std::max)(peakVal * 1.1f, minRange);
    // Cap at 66ms (~15 FPS) to handle outliers gracefully
    maxVal = (std::min)(maxVal, 66.0f);

    // Graph dimensions - full width of overlay
    float graphWidth = bgWidth;
    float graphHeight = 50.0f * renderer->GetDpiScale();
    float graphX = x - 8;
    float graphY = cursorY + 4;

    // Draw the graph
    uint32_t graphColor = cfg.frametimeColor ? cfg.frametimeColor : Colors::Yellow;
    renderer->DrawFrameTimeGraph(graphX, graphY, graphWidth, graphHeight,
                                  graphData, GRAPH_SAMPLES, minVal, maxVal, graphColor);

    // Show current frame time value below graph
    float currentFrameTime = graphData[GRAPH_SAMPLES - 1];
    snprintf(buf, 64, "%.2f ms", currentFrameTime);
    renderer->DrawTextWithShadow(x, graphY + graphHeight + 2, "Frametime", textColor, shadowColor);
    renderer->DrawTextWithShadow(x + 100, graphY + graphHeight + 2, buf, graphColor, shadowColor);
  }

  (void)viewportWidth;
  (void)viewportHeight;
}
