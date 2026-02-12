#include "../common/hook_common.h"
#include "layer_main.h"
#include "vulkan_layer.h"
#include <atomic>
#include <stdarg.h>
#include <stdio.h>

// This file provides shims and globals for common code linked into the Vulkan
// layer. Common code (overlay.cpp, fg_detection.cpp) expects C++ linkage for
// these.

void HookLog(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char buf[1024];
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  LayerLog("[Hook] %s", buf);
}

void HookLog(LogLevel level, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char buf[1024];
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  LayerLog("[Hook] %s", buf);
}

void EarlyLog(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char buf[1024];
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  LayerLog("[Early] %s", buf);
}

void NVNGXLog(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char buf[1024];
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  LayerLog("[NVNGX] %s", buf);
}

void ReportLUID(uint32_t low, uint32_t high) {
  LayerLog("ReportLUID: %08x-%08x", high, low);
}

// Graphics API helper stubs
GraphicsConfig GetActiveGraphicsConfig() { return {}; }
float GetActivePrerenderLimit() { return 0.0f; }
VSyncOverride GetVSyncOverride() { return {}; }
void ProcessVSyncOverride(UINT &SyncInterval, UINT &Flags) {}

// Missing utils or other common symbols
bool BuildLogFilePathForModuleAddress(const void *address, const char *fileName,
                                      char *outPath, size_t outPathLen) {
  return false;
}

void TryEnableFrameTimeCSVLogging(SharedMemoryLayout *shm, const void *address,
                                  PerformanceMetrics &metrics,
                                  const char *apiName, bool &inOutInitialized) {
}
