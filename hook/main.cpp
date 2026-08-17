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

// Storage for the process-lifetime config. The image is pinned and CE's loader
// and graphics hooks stay callable until the process dies, so an entry point can
// still run after the CRT has destroyed this module's globals. Owning the config
// through a global smart pointer meant its destructor freed the AppConfig while
// g_pLocalConfig kept pointing at it, and the next LoadLibrary faulted in
// GetRedirectedPath reading graphics.streamlineDllPath (Black Myth: Wukong exit
// crash, session 20260817_052857: AV at GetRedirectedPath+0x282, [config+0x4B0]).
// The config therefore lives in this module's own storage and is constructed
// once and never destroyed; the OS reclaims it when the image is unmapped.
alignas(AppConfig) static unsigned char g_LocalConfigStorage[sizeof(AppConfig)];

bool IsDXVKD3D11WrapperLoaded() {
  return IsDllFromProject("d3d11.dll", "dxvk");
}

void EnsureLocalConfigAllocated() {
  if (!g_pLocalConfig) {
    g_pLocalConfig = ::new (static_cast<void *>(g_LocalConfigStorage)) AppConfig();
  }
}

std::atomic<CreateProcessA_t> OriginalCreateProcessA{nullptr};

std::atomic<CreateProcessW_t> OriginalCreateProcessW{nullptr};

std::mutex g_HookMutex;

HANDLE g_hCheckHooksEvent = NULL;
