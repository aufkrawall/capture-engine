#include "dxgi_swapchain_wrap_internal.h"

#include "../common/dxgi_shared_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

// Diagnostic-only lifetime attribution for wrapped real swapchains. In a CE-only process (no
// foreign overlay modules loaded — their vtable hooks must never be disturbed) the real chain's
// IUnknown vtable slots 1/2 (AddRef/Release) are temporarily replaced with forwarding hooks that
// attribute every remaining reference to the module that took it. The post-destruction probe then
// reports which modules still hold references (session 20260813_231429: the game and the wrapper
// released everything, yet the real chain kept refs=1 and the next FFX create failed
// E_ACCESSDENIED — the holder had to be a CE-internal subsystem).

namespace {

constexpr int kMaxRecordedCalls = 384;

// One attribution state per process: the swapchain vtable CE patches is shared by every chain the
// owning factory creates, so the hook must forward AddRef/Release for ALL chains — including chains
// CE never wrapped (the temp swapchain used for hook installation). Recording only applies to the
// single tracked chain; forwarding to the saved original always applies.
struct AttributionState {
  void* trackedChain = nullptr;
  ULONG(STDMETHODCALLTYPE* origAddRef)(void*);
  ULONG(STDMETHODCALLTYPE* origRelease)(void*);
  std::unordered_map<std::string, long> netByModule;
  std::vector<std::string> sampleCalls;
  int recorded = 0;
};

std::mutex g_attributionLock;
AttributionState g_state;

void ClassifyAttributionCaller(void* returnAddress, char* buffer, size_t bufferSize) {
  if (!buffer || bufferSize == 0) {
    return;
  }
  buffer[0] = '\0';
  HMODULE module = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(returnAddress), &module) &&
      module) {
    char modulePath[MAX_PATH] = {};
    if (GetModuleFileNameA(module, modulePath, sizeof(modulePath))) {
      const char* baseName = modulePath;
      for (const char* cursor = modulePath; *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
          baseName = cursor + 1;
        }
      }
      snprintf(buffer, bufferSize, "%s+0x%llX", baseName,
               static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(returnAddress) -
                                               reinterpret_cast<uintptr_t>(module)));
      return;
    }
  }
  snprintf(buffer, bufferSize, "unknown+0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(returnAddress)));
}

void RecordAttributionCall(void* self, bool addRef, void* returnAddress) {
  std::lock_guard<std::mutex> lock(g_attributionLock);
  if (g_state.trackedChain != self) {
    return;
  }

  char moduleTag[256] = {};
  ClassifyAttributionCaller(returnAddress, moduleTag, sizeof(moduleTag));
  if (g_state.recorded < kMaxRecordedCalls) {
    ++g_state.recorded;
    char sample[320] = {};
    snprintf(sample, sizeof(sample), "%s %s", addRef ? "add" : "rel", moduleTag);
    g_state.sampleCalls.push_back(sample);
  }
  const char* moduleName = moduleTag;
  char* plus = strchr(moduleTag, '+');
  if (plus) {
    *plus = '\0';
  }
  g_state.netByModule[moduleName] += addRef ? 1 : -1;
}

ULONG STDMETHODCALLTYPE AttributionAddRefHook(void* self) {
  ULONG(STDMETHODCALLTYPE * original)(void*) = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_attributionLock);
    original = g_state.origAddRef;
  }
  if (!original) {
    return 0;
  }
  RecordAttributionCall(self, true, __builtin_return_address(0));
  return original(self);
}

ULONG STDMETHODCALLTYPE AttributionReleaseHook(void* self) {
  ULONG(STDMETHODCALLTYPE * original)(void*) = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_attributionLock);
    original = g_state.origRelease;
  }
  if (!original) {
    return 0;
  }
  RecordAttributionCall(self, false, __builtin_return_address(0));
  return original(self);
}

}  // namespace

void TryInstallSwapchainLifetimeAttribution(IDXGISwapChain* realChain) {
  if (!realChain || ce::overlay_compat::IsThirdPartyOverlayLoaded()) {
    return;
  }

  void** vtable = *reinterpret_cast<void***>(realChain);
  if (!DXGIShared::IsReadableMemory(reinterpret_cast<const void*>(vtable), 3 * sizeof(void*))) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(g_attributionLock);
    if (vtable[1] == reinterpret_cast<void*>(&AttributionAddRefHook) &&
        vtable[2] == reinterpret_cast<void*>(&AttributionReleaseHook)) {
      // The shared vtable is already patched (a previous wrapped chain from the same factory).
      g_state.trackedChain = realChain;
      g_state.netByModule.clear();
      g_state.sampleCalls.clear();
      g_state.recorded = 0;
      return;
    }
    g_state.trackedChain = realChain;
    g_state.origAddRef = reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(void*)>(vtable[1]);
    g_state.origRelease = reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(void*)>(vtable[2]);
    g_state.netByModule.clear();
    g_state.sampleCalls.clear();
    g_state.recorded = 0;
  }

  DWORD oldProtect = 0;
  if (!VirtualProtect(reinterpret_cast<void*>(&vtable[1]), 2 * sizeof(void*), PAGE_READWRITE, &oldProtect)) {
    std::lock_guard<std::mutex> lock(g_attributionLock);
    g_state.trackedChain = nullptr;
    return;
  }
  vtable[1] = reinterpret_cast<void*>(&AttributionAddRefHook);
  vtable[2] = reinterpret_cast<void*>(&AttributionReleaseHook);
  VirtualProtect(reinterpret_cast<void*>(&vtable[1]), 2 * sizeof(void*), oldProtect, &oldProtect);
  FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(&vtable[1]), 2 * sizeof(void*));
}

void FinishSwapchainLifetimeAttribution(IDXGISwapChain* realChain) {
  if (!realChain) {
    return;
  }

  std::vector<std::pair<std::string, long>> positiveNets;
  std::vector<std::string> samples;
  int recorded = 0;
  {
    std::lock_guard<std::mutex> lock(g_attributionLock);
    if (g_state.trackedChain != realChain) {
      return;
    }
    for (const auto& [moduleName, net] : g_state.netByModule) {
      if (net > 0) {
        positiveNets.emplace_back(moduleName, net);
      }
    }
    samples = g_state.sampleCalls;
    recorded = g_state.recorded;
    // The hooks stay installed for the remainder of the process: the shared vtable can still
    // receive releases from the chain's remaining holders, and removing the forwarding hooks
    // concurrently with those releases would drop a release (the hook cannot resolve the original
    // after the entry is gone). Only the tracked-chain identity is cleared.
    g_state.trackedChain = nullptr;
  }

  std::sort(positiveNets.begin(), positiveNets.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  WrapperLog("SwapChain: lifetime attribution summary real=%p recorded=%d", realChain, recorded);
  for (const auto& [moduleName, net] : positiveNets) {
    WrapperLog("SwapChain: lifetime attribution holder module=%s net=%+ld", moduleName.c_str(), net);
  }
  for (const std::string& sample : samples) {
    WrapperLog("SwapChain: lifetime attribution sample %s", sample.c_str());
  }
}
