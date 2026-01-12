#include "hook_common.h"
#include "performance_metrics.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

char g_ProcessName[260] = "unknown";

bool BuildLogFilePathForModuleAddress(const void* address, const char* fileName, char* outPath, size_t outPathLen) {
  if (!outPath || outPathLen == 0) return false;
  outPath[0] = '\0';
  if (!fileName || fileName[0] == '\0') return false;

  HMODULE hMod = NULL;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCSTR)address, &hMod) || !hMod) {
    return false;
  }

  char modulePath[MAX_PATH];
  DWORD n = GetModuleFileNameA(hMod, modulePath, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return false;

  char* lastSlash = strrchr(modulePath, '\\');
  if (!lastSlash) return false;
  *lastSlash = '\0';

  char logDir[MAX_PATH];
  int written = snprintf(logDir, sizeof(logDir), "%s\\logs", modulePath);
  if (written <= 0 || written >= (int)sizeof(logDir)) return false;

  CreateDirectoryA(logDir, NULL);

  written = snprintf(outPath, outPathLen, "%s\\%s", logDir, fileName);
  if (written <= 0 || (size_t)written >= outPathLen) {
    outPath[0] = '\0';
    return false;
  }

  return true;
}

void TryEnableFrameTimeCSVLogging(SharedMemoryLayout* shm, const void* address, PerformanceMetrics& metrics, const char* apiName, bool& inOutInitialized) {
  if (inOutInitialized) return;
  if (!shm || !shm->debugLogging) return;

  char csvPath[MAX_PATH];
  if (BuildLogFilePathForModuleAddress(address, "frame_times.csv", csvPath, sizeof(csvPath))) {
    metrics.EnableCSVLogging(csvPath);
    HookLog("%s: Frame time CSV logging enabled (%s)", apiName ? apiName : "API", csvPath);
  }

  inOutInitialized = true;
}

IPCClient *g_IPC = nullptr;
std::atomic<bool> g_ShuttingDown{false};
std::atomic<bool> g_GraphicsOverridesActive{false};

// Early debug log - writes directly to file without IPC dependency
// Used for debugging crashes before IPC connects
static void LogToFileAtomic(const char* baseFilename, const char* fmt, va_list args) {
  static CRITICAL_SECTION s_cs;
  static volatile LONG s_csInitialized = 0;
  static char s_logDir[MAX_PATH] = {0};
  static HANDLE s_hMutex = NULL;
  
  static thread_local bool s_isLogging = false;
  if (s_isLogging) return;
  s_isLogging = true;

  if (InterlockedCompareExchange(&s_csInitialized, 1, 0) == 0) {
      InitializeCriticalSection(&s_cs);
      InterlockedExchange(&s_csInitialized, 2);
  }
  while (s_csInitialized < 2) { Sleep(0); }
  
  EnterCriticalSection(&s_cs);
  
  if (!s_hMutex) {
      s_hMutex = CreateMutexA(NULL, FALSE, "Global\\Antigravity_Log_Mutex_v5");
      if (!s_hMutex) s_hMutex = CreateMutexA(NULL, FALSE, "Local\\Antigravity_Log_Mutex_v5");
  }
  if (s_hMutex) WaitForSingleObject(s_hMutex, INFINITE);

  static char s_formatBuffer[4096];
  static char s_lineBuffer[8192];

  if (g_ProcessName[0] == '\0') {
      char fullPath[MAX_PATH];
      if (GetModuleFileNameA(NULL, fullPath, MAX_PATH)) {
          char* lastSlash = strrchr(fullPath, '\\');
          if (lastSlash) {
              strncpy(g_ProcessName, lastSlash + 1, sizeof(g_ProcessName) - 1);
              g_ProcessName[sizeof(g_ProcessName) - 1] = '\0';
          } else {
              strncpy(g_ProcessName, fullPath, sizeof(g_ProcessName) - 1);
              g_ProcessName[sizeof(g_ProcessName) - 1] = '\0';
          }
      }
  }

  int len_buf = vsnprintf(s_formatBuffer, sizeof(s_formatBuffer), fmt, args);
  if (len_buf < 0) len_buf = 0;
  if (len_buf >= (int)sizeof(s_formatBuffer)) len_buf = (int)sizeof(s_formatBuffer) - 1;

  SYSTEMTIME st;
  GetLocalTime(&st);
  DWORD tid = GetCurrentThreadId();
  
  int len = snprintf(s_lineBuffer, sizeof(s_lineBuffer),
                     "[%02d:%02d:%02d.%03d] [T:%04X] [%s] %s",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     tid, g_ProcessName, s_formatBuffer);
  
  if (len > 0) {
      if (len >= (int)sizeof(s_lineBuffer)) len = (int)sizeof(s_lineBuffer) - 1;

      // --- PRIMARY: Push to Shared Memory Ring Buffer ---
      if (g_IPC && g_IPC->GetSharedMem()) {
          auto& logs = g_IPC->GetSharedMem()->logs;
          uint32_t wIdx = logs.writeIndex.load(std::memory_order_relaxed);
          uint32_t rIdx = logs.readIndex.load(std::memory_order_acquire);
          
          if ((uint32_t)(wIdx - rIdx) < SharedMemoryLayout::LogBuffer::SLOT_COUNT) {
              char* slot = logs.buffer[wIdx % SharedMemoryLayout::LogBuffer::SLOT_COUNT];
              snprintf(slot, SharedMemoryLayout::LogBuffer::SLOT_SIZE, "[%s] %s", baseFilename, s_lineBuffer);
              logs.writeIndex.store(wIdx + 1, std::memory_order_release);
              
              if (s_hMutex) ReleaseMutex(s_hMutex);
              LeaveCriticalSection(&s_cs);
              s_isLogging = false;
              return;
          } else {
              logs.overflowCount.fetch_add(1, std::memory_order_relaxed);
          }
      }

      // --- FALLBACK: Direct File Logging (Early Init or Buffer Full) ---
      if (s_logDir[0] == '\0') {
          char tmpPath[MAX_PATH];
          if (BuildLogFilePathForModuleAddress((const void*)&EarlyLog, baseFilename, tmpPath, sizeof(tmpPath))) {
              char* lastSlash = strrchr(tmpPath, '\\');
              if (lastSlash) {
                  *lastSlash = '\0';
                  strncpy(s_logDir, tmpPath, sizeof(s_logDir) - 1);
                  s_logDir[sizeof(s_logDir) - 1] = '\0';
              }
          }
      }

      if (s_logDir[0] != '\0') {
          char fullLogPath[MAX_PATH];
          snprintf(fullLogPath, sizeof(fullLogPath), "%s\\%s", s_logDir, baseFilename);
          HANDLE hFile = CreateFileA(fullLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
          if (hFile != INVALID_HANDLE_VALUE) {
              LARGE_INTEGER sz;
              sz.QuadPart = 0;
              if (GetFileSizeEx(hFile, &sz) && sz.QuadPart == 0) {
                  char header[512];
                  int hlen = snprintf(header, sizeof(header), "[BUILD] Version=%s Built=%s\r\n", CAPTURE_VERSION, BUILD_TIMESTAMP);
                  if (hlen > 0) {
                      DWORD hwritten;
                      WriteFile(hFile, header, (DWORD)hlen, &hwritten, NULL);
                  }
              }
              DWORD written;
              WriteFile(hFile, s_lineBuffer, len, &written, NULL);
              WriteFile(hFile, "\r\n", 2, &written, NULL);
              CloseHandle(hFile);
          }
      }
  }

  if (s_hMutex) ReleaseMutex(s_hMutex);
  LeaveCriticalSection(&s_cs);
  s_isLogging = false;
}

void EarlyLog(const char *fmt, ...) {
  if (!g_LocalConfig.debugLogging) return;
  va_list args;
  va_start(args, fmt);
  LogToFileAtomic("hook_debug.log", fmt, args);
  va_end(args);
}

void NVNGXLog(const char *fmt, ...) {
  if (!g_LocalConfig.debugLogging) return;
  va_list args;
  va_start(args, fmt);
  LogToFileAtomic("nvngx_debug.log", fmt, args);
  va_end(args);
}

void ReportLUID(uint32_t low, uint32_t high) {
  if (g_IPC && g_IPC->GetSharedMem()) {
      if (g_IPC->GetSharedMem()->luidLowPart != (int32_t)low || 
          g_IPC->GetSharedMem()->luidHighPart != (int32_t)high) {
          g_IPC->GetSharedMem()->luidLowPart = (int32_t)low;
          g_IPC->GetSharedMem()->luidHighPart = (int32_t)high;
          HookLog("Common: Reported LUID to SHM: 0x%08X_%08X", high, low);
      }
  }
}

// Internal worker implementation
static void HookLogInternal(LogLevel level, const char *fmt, va_list args) {
  if (g_IPC && g_IPC->GetSharedMem()) {
      if ((int)level > (int)g_IPC->GetSharedMem()->logLevel) return;
      if (!g_IPC->GetSharedMem()->debugLogging) return;
  }
  
  static char buffer[4096];
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  
  const char* levelStr = "INFO";
  switch(level) {
      case LogLevel::Error: levelStr = "ERROR"; break;
      case LogLevel::Warn:  levelStr = "WARN"; break;
      case LogLevel::Debug: levelStr = "DEBUG"; break;
      default: break;
  }
  
  EarlyLog("[%s] %s", levelStr, buffer);
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
        mergedConfig.mipBiasMode = shmGfx.mipBiasMode;
        mergedConfig.msaaSamples = shmGfx.msaaSamples;
        mergedConfig.cpuPrerenderLimit = shmGfx.prerenderLimit;
        mergedConfig.backbufferCount = shmGfx.backbufferCount;
        mergedConfig.sgssaa = shmGfx.sgssaa;
        mergedConfig.disableAutoMipBias = shmGfx.disableAutoMipBias;
        mergedConfig.dlssAutoExposure = shmGfx.dlssAutoExposure;
        mergedConfig.dlssExposureNormalization = shmGfx.dlssExposureNormalization;
        
        mergedConfig.parsed.presetDLAA = shmGfx.dlssPresetDLAA;
        mergedConfig.parsed.presetQuality = shmGfx.dlssPresetQuality;
        mergedConfig.parsed.presetBalanced = shmGfx.dlssPresetBalanced;
        mergedConfig.parsed.presetPerformance = shmGfx.dlssPresetPerformance;
        mergedConfig.parsed.presetUltraPerformance = shmGfx.dlssPresetUltraPerformance;
        mergedConfig.parsed.presetUltraQuality = shmGfx.dlssPresetUltraQuality;

        mergedConfig.parsed.rrPresetDLAA = shmGfx.dlssRRPresetDLAA;
        mergedConfig.parsed.rrPresetQuality = shmGfx.dlssRRPresetQuality;
        mergedConfig.parsed.rrPresetBalanced = shmGfx.dlssRRPresetBalanced;
        mergedConfig.parsed.rrPresetPerformance = shmGfx.dlssRRPresetPerformance;
        mergedConfig.parsed.rrPresetUltraPerformance = shmGfx.dlssRRPresetUltraPerformance;
        mergedConfig.parsed.rrPresetUltraQuality = shmGfx.dlssRRPresetUltraQuality;

        mergedConfig.parsed.srPreset = shmGfx.dlssSRPreset;
        mergedConfig.parsed.rrPreset = shmGfx.dlssRRPreset;

        mergedConfig.parsed.dlssSharpening = shmGfx.dlssSharpening;
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
    if (g_LocalConfig.graphics.dlssAutoExposure != "default" && !g_LocalConfig.graphics.dlssAutoExposure.empty()) {
        mergedConfig.dlssAutoExposure = g_LocalConfig.graphics.dlssAutoExposure;
    }
    if (g_LocalConfig.graphics.dlssExposureNormalization != "default" && !g_LocalConfig.graphics.dlssExposureNormalization.empty()) {
        mergedConfig.dlssExposureNormalization = g_LocalConfig.graphics.dlssExposureNormalization;
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
    
    // Apply Preset Overrides from g_LocalConfig
    if (g_LocalConfig.graphics.parsed.presetDLAA > 0) mergedConfig.parsed.presetDLAA = g_LocalConfig.graphics.parsed.presetDLAA;
    if (g_LocalConfig.graphics.parsed.presetQuality > 0) mergedConfig.parsed.presetQuality = g_LocalConfig.graphics.parsed.presetQuality;
    if (g_LocalConfig.graphics.parsed.presetBalanced > 0) mergedConfig.parsed.presetBalanced = g_LocalConfig.graphics.parsed.presetBalanced;
    if (g_LocalConfig.graphics.parsed.presetPerformance > 0) mergedConfig.parsed.presetPerformance = g_LocalConfig.graphics.parsed.presetPerformance;
    if (g_LocalConfig.graphics.parsed.presetUltraPerformance > 0) mergedConfig.parsed.presetUltraPerformance = g_LocalConfig.graphics.parsed.presetUltraPerformance;
    if (g_LocalConfig.graphics.parsed.presetUltraQuality > 0) mergedConfig.parsed.presetUltraQuality = g_LocalConfig.graphics.parsed.presetUltraQuality;

    if (g_LocalConfig.graphics.parsed.rrPresetDLAA > 0) mergedConfig.parsed.rrPresetDLAA = g_LocalConfig.graphics.parsed.rrPresetDLAA;
    if (g_LocalConfig.graphics.parsed.rrPresetQuality > 0) mergedConfig.parsed.rrPresetQuality = g_LocalConfig.graphics.parsed.rrPresetQuality;
    if (g_LocalConfig.graphics.parsed.rrPresetBalanced > 0) mergedConfig.parsed.rrPresetBalanced = g_LocalConfig.graphics.parsed.rrPresetBalanced;
    if (g_LocalConfig.graphics.parsed.rrPresetPerformance > 0) mergedConfig.parsed.rrPresetPerformance = g_LocalConfig.graphics.parsed.rrPresetPerformance;
    if (g_LocalConfig.graphics.parsed.rrPresetUltraPerformance > 0) mergedConfig.parsed.rrPresetUltraPerformance = g_LocalConfig.graphics.parsed.rrPresetUltraPerformance;
    if (g_LocalConfig.graphics.parsed.rrPresetUltraQuality > 0) mergedConfig.parsed.rrPresetUltraQuality = g_LocalConfig.graphics.parsed.rrPresetUltraQuality;

    // Apply Global Preset Overrides from g_LocalConfig
    if (g_LocalConfig.graphics.parsed.srPreset > 0) mergedConfig.parsed.srPreset = g_LocalConfig.graphics.parsed.srPreset;
    if (g_LocalConfig.graphics.parsed.rrPreset > 0) mergedConfig.parsed.rrPreset = g_LocalConfig.graphics.parsed.rrPreset;

    if (g_LocalConfig.graphics.parsed.dlssSharpening > -1.5f) {
        mergedConfig.parsed.dlssSharpening = g_LocalConfig.graphics.parsed.dlssSharpening;
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
    else if (mergedConfig.dlssAutoExposure != "default" && !mergedConfig.dlssAutoExposure.empty()) anyActive = true;
    else if (mergedConfig.dlssExposureNormalization != "default" && !mergedConfig.dlssExposureNormalization.empty()) anyActive = true;
    else if (mergedConfig.parsed.presetDLAA > 0 || mergedConfig.parsed.presetQuality > 0) anyActive = true;
    else if (mergedConfig.parsed.rrPresetDLAA > 0 || mergedConfig.parsed.rrPresetQuality > 0) anyActive = true;
    else if (mergedConfig.parsed.srPreset > 0 || mergedConfig.parsed.rrPreset > 0) anyActive = true;
    else if (mergedConfig.parsed.dlssSharpening > -1.5f) anyActive = true;
    
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
