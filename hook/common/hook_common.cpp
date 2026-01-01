#include "hook_common.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

char g_ProcessName[260] = "unknown";

IPCClient *g_IPC = nullptr;
std::atomic<bool> g_ShuttingDown{false};

// Early debug log - writes directly to file without IPC dependency
// Used for debugging crashes before IPC connects
void EarlyLog(const char *fmt, ...) {
  char buffer[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  
  // Write to logs/ directory relative to the hook DLL
  char logPath[MAX_PATH];
  HMODULE hMod = NULL;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCSTR)&EarlyLog, &hMod);
  GetModuleFileNameA(hMod, logPath, MAX_PATH);
  
  // Find directory part
  char *lastSlash = strrchr(logPath, '\\');
  if (lastSlash) {
    *lastSlash = '\0'; // Cut off filename
    strcat(logPath, "\\logs"); // Append logs dir
    
    // Create directory only once
    static bool logDirCreated = false;
    if (!logDirCreated) {
      CreateDirectoryA(logPath, NULL); // Ensure directory exists
      logDirCreated = true;
    }
    
    strcat(logPath, "\\hook_debug.log"); // Append filename
  } else {
    strcpy(logPath, "hook_debug.log");
  }
  
  HANDLE hFile = CreateFileA(logPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (hFile != INVALID_HANDLE_VALUE) {
    SetFilePointer(hFile, 0, NULL, FILE_END);
    
    char logLine[1024];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int len = snprintf(logLine, sizeof(logLine),
                       "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%-20s] %s\r\n",
                       st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, 
                       g_ProcessName, buffer);
    
    DWORD written;
    WriteFile(hFile, logLine, len, &written, NULL);
    CloseHandle(hFile);
  }
}

void HookLog(const char *fmt, ...) {
  // Only log if IPC is connected AND debugLogging is true
  if (!g_IPC || !g_IPC->GetSharedMem() ||
      !g_IPC->GetSharedMem()->debugLogging) {
    return; // No IPC yet, or debug logging disabled
  }

  char buffer[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  // Keep handle open to avoid open/close overhead every line
  // Thread-safe: protect static file handle with mutex
  static std::mutex s_logMutex;
  static HANDLE hLogFile = INVALID_HANDLE_VALUE;
  static char lastLogPath[280] = {0};
  
  std::lock_guard<std::mutex> lock(s_logMutex);

  const char *logPath = g_IPC->GetSharedMem()->logFilePath;
  if (!logPath || logPath[0] == '\0')
    return;

  char hookLogPath[280];
  const char *lastSlash = strrchr(logPath, '\\');
  if (!lastSlash)
    lastSlash = strrchr(logPath, '/');
  if (lastSlash) {
    size_t dirLen = lastSlash - logPath + 1;
    memcpy(hookLogPath, logPath, dirLen);
    strcpy(hookLogPath + dirLen, "hook.log");
  } else {
    strcpy(hookLogPath, "hook.log");
  }

  if (hLogFile == INVALID_HANDLE_VALUE ||
      strcmp(hookLogPath, lastLogPath) != 0) {
    if (hLogFile != INVALID_HANDLE_VALUE)
      CloseHandle(hLogFile);
    hLogFile = CreateFileA(hookLogPath, GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hLogFile == INVALID_HANDLE_VALUE)
      return;
    strcpy(lastLogPath, hookLogPath);
    SetFilePointer(hLogFile, 0, NULL, FILE_END);
  }

  char logLine[1024];
  SYSTEMTIME st;
  GetLocalTime(&st);
  int len =
      snprintf(logLine, sizeof(logLine),
               "[%04d-%02d-%02d %02d:%02d:%02d] [INFO] [Hook] [%-20s] %s\r\n", 
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, 
               g_ProcessName, buffer);

  DWORD written;
  WriteFile(hLogFile, logLine, len, &written, NULL);
}

// Helpers for Config Overrides
const GraphicsConfig& GetActiveGraphicsConfig() {
    // If local override is used/loaded, it might have valid data.
    // However, AppConfig init sets defaults (-1.0f).
    // We want to merge: Local Override > Global IPC.
    // Since we return a reference, we can't merge on the fly easily without a static buffer.
    // But most hooks just read fields.
    // Let's rely on g_IPC for base, and g_LocalConfig for specific overrides.
    // Actually, to keep it simple: return g_IPC's config if available, but
    // we need to access fields individually.
    // Wait, the callers use `const auto& gfx = GetActiveGraphicsConfig();`
    // If we return one or the other, we miss partial overrides.
    // But our config system works on "Section Overrides".
    // If [Graphics] is overridden in config.ini, it overrides ALL of [Graphics]?
    // No, LoadConfig logic reads defaults. If config.ini has [Graphics], it fills it.
    // But it fills defaults (-1.0) if missing in ini.
    // Global config has values.
    
    // PROBLEM: g_LocalConfig might be partial.
    // If we return g_LocalConfig, we might get -1.0 for some fields if they were missing in local ini,
    // while Global config has proper values.
    
    // We should probably merge them into a static instance?
    static GraphicsConfig mergedConfig;
    static uint32_t lastUpdateTick = 0;
    
    // Only merge occasionally or if needed. For now, simple logic:
    // This function signature returns a reference, implying a persistent object.
    
    if (g_IPC && g_IPC->GetSharedMem()) {
        const auto& shmGfx = g_IPC->GetSharedMem()->graphicsConfig;
        mergedConfig.vsyncMode = shmGfx.vsyncMode;
        mergedConfig.anisotropicFiltering = shmGfx.anisotropicFiltering;
        mergedConfig.mipMapping = shmGfx.mipMapping;
        mergedConfig.mipBias = shmGfx.mipBias;
        mergedConfig.msaaSamples = shmGfx.msaaSamples;
        mergedConfig.cpuPrerenderLimit = shmGfx.prerenderLimit;
        mergedConfig.backbufferCount = shmGfx.backbufferCount;
        mergedConfig.sgssaa = shmGfx.sgssaa;
    } else {
        // No IPC, stick to defaults
        mergedConfig = GraphicsConfig(); 
    }
    
    // Apply Overrides from g_LocalConfig
    if (g_LocalConfig.graphics.cpuPrerenderLimit > -0.5f) {
        mergedConfig.cpuPrerenderLimit = g_LocalConfig.graphics.cpuPrerenderLimit;
    }
    if (g_LocalConfig.graphics.vsyncMode != "default" && !g_LocalConfig.graphics.vsyncMode.empty()) {
        mergedConfig.vsyncMode = g_LocalConfig.graphics.vsyncMode;
    }
    if (g_LocalConfig.graphics.backbufferCount > 0) {
        mergedConfig.backbufferCount = g_LocalConfig.graphics.backbufferCount;
    }
    // Add other fields as needed
    
    return mergedConfig;
}

float GetActivePrerenderLimit() {
    const auto& cfg = GetActiveGraphicsConfig();
    return cfg.cpuPrerenderLimit;
}

// Helper to get VSync override settings
// Reduces code duplication across DX9/DX11/DX12 hooks
VSyncOverride GetVSyncOverride() {
    VSyncOverride result;
    const auto& cfg = GetActiveGraphicsConfig();
    
    if (cfg.vsyncMode == "default" || cfg.vsyncMode.empty()) {
        result.shouldOverride = false;
        return result;
    }
    
    result.shouldOverride = true;
    
    if (cfg.vsyncMode == "off") {
        result.presentInterval = 0;  // DX9: D3DPRESENT_INTERVAL_IMMEDIATE, DX11/12: sync interval 0
        result.useMailbox = false;
    } else if (cfg.vsyncMode == "fifo") {
        result.presentInterval = 1;  // DX9: D3DPRESENT_INTERVAL_ONE, DX11/12: sync interval 1
        result.useMailbox = false;
    } else if (cfg.vsyncMode == "mailbox") {
        result.presentInterval = 0;  // DX9: immediate (no true mailbox), DX11/12: sync 0 + flip_discard
        result.useMailbox = true;
    } else if (cfg.vsyncMode == "adaptive") {
        result.presentInterval = 1;  // DX9: no adaptive (use fifo), DX11/12: sync interval 1
        result.useMailbox = false;
    } else {
        // Unknown mode, don't override
        result.shouldOverride = false;
    }
    
    return result;
}
