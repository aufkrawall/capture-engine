#pragma once

#include "ipc_client.h"
#include <atomic>
#include <format>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>


// Global IPC defined in centralized location (main.cpp or here)
extern IPCClient *g_IPC;
extern std::atomic<bool> g_ShuttingDown;
extern std::atomic<bool> g_GraphicsOverridesActive;

// Logging Helper
void HookLog(const char *fmt, ...);
void HookLog(LogLevel level, const char *fmt, ...);
void EarlyLog(const char *fmt, ...);
extern char g_ProcessName[260];
  // Debug log independent of IPC

// Constants
#define HOOK_LOG_FILE "hook.log"

// Local Config (Loaded by Hook for Per-App Overrides)
#include "../../common/config.h"
extern AppConfig g_LocalConfig;

// Helper to get active config (Local > IPC)
GraphicsConfig GetActiveGraphicsConfig();
float GetActivePrerenderLimit();

// Helper to apply VSync override (reduces duplication across DX9/11/12)
// Returns the presentation interval value to use
// For DX9: D3DPRESENT_INTERVAL_* constants
// For DX11/12: DXGI_SWAP_EFFECT and sync interval
struct VSyncOverride {
    bool shouldOverride = false;
    int presentInterval = 0;  // DX9: D3DPRESENT_INTERVAL_*, DX11/12: sync interval (0 or 1)
    bool useMailbox = false;  // DX11/12: use DXGI_SWAP_EFFECT_FLIP_DISCARD for mailbox
};
VSyncOverride GetVSyncOverride();
