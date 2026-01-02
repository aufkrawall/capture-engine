#include "hook_common.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

char g_ProcessName[260] = "unknown";

IPCClient *g_IPC = nullptr;
std::atomic<bool> g_ShuttingDown{false};
std::atomic<bool> g_GraphicsOverridesActive{false};

// Early debug log - writes directly to file without IPC dependency
// Used for debugging crashes before IPC connects
// Only logs if debug_logging is enabled in local config
void EarlyLog(const char *fmt, ...) {
  // Check if debug logging is enabled (default: disabled)
  // Use local config since IPC may not be available yet
  if (!g_LocalConfig.debugLogging) {
    return;
  }
  
  char buffer[512];
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

// Internal worker implementation
static void HookLogInternal(LogLevel level, const char *fmt, va_list args) {
  // Check filtering
  if (g_IPC && g_IPC->GetSharedMem()) {
      if ((int)level > (int)g_IPC->GetSharedMem()->logLevel) {
          return;
      }
      if (!g_IPC->GetSharedMem()->debugLogging) {
          return;
      }
  } else {
      return; // No IPC
  }

  char buffer[1024]; // Increased buffer size
  vsnprintf(buffer, sizeof(buffer), fmt, args);

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

  // Log Rotation
  const DWORD MAX_LOG_SIZE = 5 * 1024 * 1024; // 5MB
  if (hLogFile != INVALID_HANDLE_VALUE) {
    DWORD size = GetFileSize(hLogFile, NULL);
    if (size != INVALID_FILE_SIZE && size > MAX_LOG_SIZE) {
      CloseHandle(hLogFile);
      hLogFile = INVALID_HANDLE_VALUE;
      
      char backupPath[280];
      strcpy(backupPath, hookLogPath);
      strcat(backupPath, ".old");
      
      DeleteFileA(backupPath);
      MoveFileA(hookLogPath, backupPath);
    }
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
  
  const char* levelStr = "INFO";
  switch(level) {
      case LogLevel::Error: levelStr = "ERROR"; break;
      case LogLevel::Warn:  levelStr = "WARN"; break;
      case LogLevel::Debug: levelStr = "DEBUG"; break;
      default: break;
  }
  
  int len =
      snprintf(logLine, sizeof(logLine),
               "[%04d-%02d-%02d %02d:%02d:%02d] [%s] [Hook] [%-20s] %s\r\n", 
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, 
               levelStr, g_ProcessName, buffer);

  DWORD written;
  WriteFile(hLogFile, logLine, len, &written, NULL);
}

void HookLog(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  HookLogInternal(LogLevel::Info, fmt, args);
  va_end(args);
}

void HookLog(LogLevel level, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  HookLogInternal(level, fmt, args);
  va_end(args);
}

// Helpers for Config Overrides
GraphicsConfig GetActiveGraphicsConfig() {
    static GraphicsConfig mergedConfig;
    static uint32_t lastVersion = 0xFFFFFFFF;
    static uint32_t lastUpdateTick = 0;
    static std::mutex configMutex;
    
    std::lock_guard<std::mutex> lock(configMutex);

    uint32_t currentVersion = 0;
    if (g_IPC && g_IPC->GetSharedMem()) {
        currentVersion = g_IPC->GetSharedMem()->configVersion.load(std::memory_order_acquire);
    }
    
    DWORD now = GetTickCount();
    if (currentVersion == lastVersion && (now - lastUpdateTick < 1000)) {
        return mergedConfig;
    }
    
    lastVersion = currentVersion;
    lastUpdateTick = now;

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
        mergedConfig.disableAutoMipBias = shmGfx.disableAutoMipBias;
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
    if (g_LocalConfig.graphics.sgssaa) {
        mergedConfig.sgssaa = g_LocalConfig.graphics.sgssaa;
    }
    if (g_LocalConfig.graphics.disableAutoMipBias) {
        mergedConfig.disableAutoMipBias = g_LocalConfig.graphics.disableAutoMipBias;
    }
    
    // Missing overrides added to fix regression
    if (g_LocalConfig.graphics.anisotropicFiltering != "default" && !g_LocalConfig.graphics.anisotropicFiltering.empty()) {
        mergedConfig.anisotropicFiltering = g_LocalConfig.graphics.anisotropicFiltering;
    }
    if (g_LocalConfig.graphics.mipMapping != "default" && !g_LocalConfig.graphics.mipMapping.empty()) {
        mergedConfig.mipMapping = g_LocalConfig.graphics.mipMapping;
    }
    if (g_LocalConfig.graphics.mipBias != "default" && !g_LocalConfig.graphics.mipBias.empty()) {
        mergedConfig.mipBias = g_LocalConfig.graphics.mipBias;
    }
    if (g_LocalConfig.graphics.msaaSamples != "default" && !g_LocalConfig.graphics.msaaSamples.empty()) {
        mergedConfig.msaaSamples = g_LocalConfig.graphics.msaaSamples;
    }
    // Add other fields as needed
    
    // Update global performance gating flag
    bool anyActive = false;
    if (mergedConfig.vsyncMode != "default" && !mergedConfig.vsyncMode.empty()) anyActive = true;
    else if (mergedConfig.anisotropicFiltering != "default" && !mergedConfig.anisotropicFiltering.empty()) anyActive = true;
    else if (mergedConfig.mipMapping != "default" && !mergedConfig.mipMapping.empty()) anyActive = true;
    else if (mergedConfig.mipBias != "default" && !mergedConfig.mipBias.empty()) anyActive = true;
    else if (mergedConfig.msaaSamples != "default" && !mergedConfig.msaaSamples.empty()) anyActive = true;
    else if (mergedConfig.cpuPrerenderLimit > -0.5f) anyActive = true;
    else if (mergedConfig.backbufferCount > 0) anyActive = true;
    else if (mergedConfig.sgssaa) anyActive = true;
    
    g_GraphicsOverridesActive.store(anyActive, std::memory_order_release);

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
