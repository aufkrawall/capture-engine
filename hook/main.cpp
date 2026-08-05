#include "main_internal.h"

HMODULE g_hModule = NULL;

std::atomic<bool> g_ProcessTerminating{false};

bool IsProcessTerminating() { return g_ProcessTerminating.load(std::memory_order_acquire); }

std::atomic<bool> g_HookThreadRunning{false}; // Track if HookThread is active

// Global Hook Pointers
DX12Hook *g_DX12Hook = nullptr;

DX11Hook *g_DX11Hook = nullptr;

DX9Hook *g_DX9Hook = nullptr;

DDrawHook *g_DDrawHook = nullptr;

DX8Hook *g_DX8Hook = nullptr;

OpenGLHook *g_OpenGLHook = nullptr;

// Global Local Config
AppConfig *g_pLocalConfig = nullptr;

std::unique_ptr<AppConfig> g_LocalConfigOwner;

bool IsDXVKD3D11WrapperLoaded() {
  return IsDllFromProject("d3d11.dll", "dxvk");
}

void EnsureLocalConfigAllocated() {
  if (!g_LocalConfigOwner) {
    g_LocalConfigOwner = std::make_unique<AppConfig>();
  }
  g_pLocalConfig = g_LocalConfigOwner.get();
}

std::atomic<GetCommandLineA_t> OriginalGetCommandLineA{nullptr};

std::atomic<GetCommandLineW_t> OriginalGetCommandLineW{nullptr};

std::atomic<CreateProcessA_t> OriginalCreateProcessA{nullptr};

std::atomic<CreateProcessW_t> OriginalCreateProcessW{nullptr};

RegQueryValueExW_t OriginalRegQueryValueExW = nullptr;

std::mutex g_HookMutex;

HANDLE g_hCheckHooksEvent = NULL;
