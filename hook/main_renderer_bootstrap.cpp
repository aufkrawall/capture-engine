#include "main_internal.h"

std::atomic<bool> g_InheritedRendererProcess{false};

namespace {

std::atomic<int> g_RendererBootstrapResult{0};
HANDLE g_RendererBootstrapEvent = nullptr;

void CopyFixedString(std::string& destination, const char* source, size_t capacity) {
  destination.assign(source ? source : "", source ? strnlen(source, capacity) : 0);
}

}  // namespace

void InitializeInheritedRendererBootstrapSignal() {
  if (!g_InheritedRendererProcess.load(std::memory_order_acquire) ||
      g_RendererBootstrapEvent) {
    return;
  }
  g_RendererBootstrapEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

void CompleteInheritedRendererBootstrap(bool success) {
  if (!g_InheritedRendererProcess.load(std::memory_order_acquire)) {
    return;
  }
  int expected = 0;
  g_RendererBootstrapResult.compare_exchange_strong(
      expected, success ? 1 : -1, std::memory_order_acq_rel,
      std::memory_order_acquire);
  if (g_RendererBootstrapEvent) {
    SetEvent(g_RendererBootstrapEvent);
  }
}

void SyncInheritedRendererRuntimeConfig(SharedMemoryLayout* sharedMemory) {
  if (!g_InheritedRendererProcess.load(std::memory_order_acquire) ||
      !sharedMemory || !g_pLocalConfig) {
    return;
  }

  const SharedGraphicsConfig& shared = sharedMemory->graphicsConfig;
  GraphicsConfig& local = g_pLocalConfig->graphics;
  CopyFixedString(local.dlssSrDllPath, shared.dlssSrDllPath,
                  sizeof(shared.dlssSrDllPath));
  CopyFixedString(local.dlssRrDllPath, shared.dlssRrDllPath,
                  sizeof(shared.dlssRrDllPath));
  CopyFixedString(local.dlssFgDllPath, shared.dlssFgDllPath,
                  sizeof(shared.dlssFgDllPath));
  CopyFixedString(local.streamlineDllPath, shared.streamlineDllPath,
                  sizeof(shared.streamlineDllPath));
  CopyFixedString(local.dlssDebugOverlay, shared.dlssDebugOverlay,
                  sizeof(shared.dlssDebugOverlay));
  HookLogImportant(
      "Inherited renderer: synchronized process-local DLSS/Streamline profile "
      "controls from shared memory (SR=%d RR=%d FG=%d SL=%d indicator=%s)",
      local.dlssSrDllPath.empty() ? 0 : 1,
      local.dlssRrDllPath.empty() ? 0 : 1,
      local.dlssFgDllPath.empty() ? 0 : 1,
      local.streamlineDllPath.empty() ? 0 : 1,
      local.dlssDebugOverlay.empty() ? "default" : local.dlssDebugOverlay.c_str());
}

bool CurrentProcessOwnsProcessLocalRuntimeOverrides() {
  SharedMemoryLayout* sharedMemory = g_IPC ? g_IPC->GetSharedMem() : nullptr;
  const uint32_t rendererPid =
      sharedMemory
          ? sharedMemory->runtimeState.inheritedRendererProcessPid.load(
                std::memory_order_acquire)
          : 0;
  return ce::vulkan_renderer_policy::ShouldApplyProcessLocalRuntimeOverrides(
      GetCurrentProcessId(), rendererPid);
}

extern "C" __declspec(dllexport) BOOL
CE_WaitForInheritedRendererBootstrap(DWORD timeoutMs) {
  if (!g_InheritedRendererProcess.load(std::memory_order_acquire)) {
    return FALSE;
  }
  const int immediate = g_RendererBootstrapResult.load(std::memory_order_acquire);
  if (immediate != 0) {
    return immediate > 0 ? TRUE : FALSE;
  }
  if (!g_RendererBootstrapEvent ||
      WaitForSingleObject(g_RendererBootstrapEvent, timeoutMs) != WAIT_OBJECT_0) {
    return FALSE;
  }
  return g_RendererBootstrapResult.load(std::memory_order_acquire) > 0 ? TRUE
                                                                      : FALSE;
}
