#include "hook_common.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <mutex>
#include <unordered_map>
#include "hook_context.h"
#include "performance_metrics.h"
#include "system_metrics.h"

char g_ProcessName[260] = "unknown";

// Mutex to protect sync operations - prevents races during state transitions
static std::mutex g_SyncMutex;

// Sync HookContext with legacy global variables
// This provides a bridge during the gradual migration from scattered globals to
// HookContext.
//
// TODO(MIGRATION): This function should be called:
//   1. After HookContext::Initialize() completes
//   2. After any legacy global is modified that has a HookContext equivalent
//   3. Before accessing state that might differ between the two systems
//
// Future migration steps:
//   - Move ownership of IPCClient from g_IPC to HookContext
//   - Move ownership of AppConfig from g_pLocalConfig to HookContext
//   - Move graphics device pointers into HookContext::graphicsData
//   - Remove legacy globals entirely
void ce::SyncWithLegacyGlobals() {
    std::lock_guard<std::mutex> lock(g_SyncMutex);

    auto* ctx = GetHookContext();
    if (!ctx)
        return;

    // Validate HookContext is in a valid state for sync
    if (ctx->shuttingDown.load(std::memory_order_acquire)) {
        CE_LOG_WARN("HookCtx", "sync skipped - shutting down");
        return;
    }

    // Link IPC - HookContext wraps the global, not replaces it yet
    // Validation: Ensure g_IPC and ctx->ipc point to the same shared memory
    if (g_IPC) {
        if (!ctx->ipc) {
            // For now, HookContext doesn't own g_IPC, just references it
            // In future migration, HookContext will own the IPCClient
            ctx->sharedMem = g_IPC->GetSharedMem();
            CE_LOG_DEBUG("HookCtx", "synced IPC shared mem");
        } else {
            // Both exist - validate they match
            auto* legacyMem = g_IPC->GetSharedMem();
            if (ctx->sharedMem != legacyMem) {
                CE_LOG_WARN("HookCtx", "IPC shared mem mismatch detected, re-syncing");
                ctx->sharedMem = legacyMem;
            }
        }
    }

    // Sync config from legacy global to HookContext
    // This prevents config drift between the two systems
    if (g_pLocalConfig) {
        if (!ctx->localConfig) {
            // First time sync: copy the config to HookContext
            ctx->localConfig = std::make_unique<AppConfig>(*g_pLocalConfig);
            CE_LOG_INFO("HookCtx", "initialized localConfig from g_pLocalConfig");
        } else {
            // Subsequent sync: check for changes and update if needed
            // This prevents "config drift" where g_pLocalConfig is modified
            // but HookContext still has old values
            // NOTE: We only sync graphics config for now as that's the main use case
            if (ctx->localConfig->graphics.vsyncMode != g_pLocalConfig->graphics.vsyncMode ||
                ctx->localConfig->graphics.anisotropicFiltering != g_pLocalConfig->graphics.anisotropicFiltering ||
                ctx->localConfig->graphics.mipMapping != g_pLocalConfig->graphics.mipMapping) {
                CE_LOG_WARN("HookCtx", "config drift detected, re-syncing from legacy");
                *ctx->localConfig = *g_pLocalConfig;
            }
        }
    }

    // Sync debug logging flag from shared memory
    if (ctx->sharedMem) {
        bool debugEnabled = ctx->sharedMem->GetDebugLogging();
        if (ctx->debugLoggingEnabled != debugEnabled) {
            ctx->debugLoggingEnabled = debugEnabled;
            g_DebugLoggingEnabled = debugEnabled;
            CE_LOG_DEBUG("HookCtx", "synced debug logging flag: %d", debugEnabled);
        }
    }

    // Copy process info (these rarely change, but ensure consistency)
    if (strncmp(ctx->processName, g_ProcessName, sizeof(ctx->processName)) != 0) {
        strncpy_s(ctx->processName, g_ProcessName, _TRUNCATE);
    }
    ctx->processId = GetCurrentProcessId();

    CE_LOG_DEBUG("HookCtx", "synced with legacy globals (api=%s)", GraphicsAPIName(ctx->activeAPI));
}

bool BuildLogFilePathForModuleAddress(const void* address, const char* fileName, char* outPath, size_t outPathLen) {
    if (!outPath || outPathLen == 0)
        return false;
    outPath[0] = '\0';
    if (!fileName || fileName[0] == '\0')
        return false;

    HMODULE hMod = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)address, &hMod) ||
        !hMod) {
        return false;
    }

    char modulePath[MAX_PATH];
    DWORD n = GetModuleFileNameA(hMod, modulePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return false;

    char* lastSlash = strrchr(modulePath, '\\');
    if (!lastSlash)
        return false;
    *lastSlash = '\0';

    char logDir[MAX_PATH];
    int written = snprintf(logDir, sizeof(logDir), "%s\\logs", modulePath);
    if (written <= 0 || written >= (int)sizeof(logDir))
        return false;

    CreateDirectoryA(logDir, NULL);

    written = snprintf(outPath, outPathLen, "%s\\%s", logDir, fileName);
    if (written <= 0 || (size_t)written >= outPathLen) {
        outPath[0] = '\0';
        return false;
    }

    return true;
}

IPCClient* g_IPC = nullptr;
SharedMemoryLayout* g_pSharedMem = nullptr;
std::atomic<bool> g_ShuttingDown{false};
std::atomic<bool> g_GraphicsOverridesActive{false};

// Global sequence counter for log ordering diagnostics
static std::atomic<uint64_t> g_LogSequence{0};

// Early debug log - writes directly to file without IPC dependency
// Used for debugging crashes before IPC connects
// OPTIMIZED: Uses stack buffers and avoids global locks for the hot path (SHM)
static void LogToFileAtomic(const char* baseFilename, const char* fmt, va_list args, bool forceDirectFile = false) {
    // Use stack buffers to allow concurrency without locking
    char formatBuffer[4096];
    char lineBuffer[8192];

    // Initialize Process Name once, thread-safe
    static std::mutex s_NameInitMutex;
    static bool s_NameInitDone = false;

    if (!s_NameInitDone) {
        std::lock_guard<std::mutex> lock(s_NameInitMutex);
        if (!s_NameInitDone) {
            if (g_ProcessName[0] == '\0') {
                char fullPath[MAX_PATH];
                if (GetModuleFileNameA(NULL, fullPath, MAX_PATH)) {
                    char* lastSlash = strrchr(fullPath, '\\');
                    if (lastSlash) {
                        strncpy(g_ProcessName, lastSlash + 1, sizeof(g_ProcessName) - 1);
                    } else {
                        strncpy(g_ProcessName, fullPath, sizeof(g_ProcessName) - 1);
                    }
                    g_ProcessName[sizeof(g_ProcessName) - 1] = '\0';
                }
            }
            s_NameInitDone = true;
        }
    }

    // Format the message
    int len_buf = vsnprintf(formatBuffer, sizeof(formatBuffer), fmt, args);
    if (len_buf < 0)
        len_buf = 0;

    SYSTEMTIME st;
    GetLocalTime(&st);
    DWORD tid = GetCurrentThreadId();

    // Get sequence number for ordering diagnostics
    uint64_t seq = g_LogSequence.fetch_add(1, std::memory_order_relaxed);

    int len =
        snprintf(lineBuffer, sizeof(lineBuffer), "[%02d:%02d:%02d.%03d] [T:%04X] [S:%llu] [%s] %s", st.wHour,
                 st.wMinute, st.wSecond, st.wMilliseconds, tid, (unsigned long long)seq, g_ProcessName, formatBuffer);

    if (len <= 0)
        return;
    if (len >= (int)sizeof(lineBuffer))
        len = (int)sizeof(lineBuffer) - 1;

    // --- PRIMARY: Push to Shared Memory Ring Buffer (Lock-Free) ---
    // When IPC is connected, we ONLY write to shared memory.
    // The logger_service.cpp consumer reads from SHM and writes to file.
    // This prevents duplicate log entries.
    if (!forceDirectFile && g_IPC) {
        // Load pointer atomically in case it's being torn down (unlikely but safe)
        SharedMemoryLayout* shm = g_IPC->GetSharedMem();
        if (shm) {
            auto& logs = shm->logs;
            // Reserve a slot atomically: check capacity BEFORE incrementing to avoid
            // permanently advancing writeIndex when the buffer is full (which would
            // cause the consumer to stall waiting for a committed slot that never
            // arrives).
            uint32_t wIdx = logs.writeIndex.load(std::memory_order_relaxed);
            bool reservedSlot = false;
            for (;;) {
                uint32_t rIdx = logs.readIndex.load(std::memory_order_acquire);
                if ((uint32_t)(wIdx - rIdx) >= SharedMemoryLayout::LogBuffer::SLOT_COUNT) {
                    logs.overflowCount.fetch_add(1, std::memory_order_relaxed);
                    break;  // Buffer full — fall through to file logging
                }
                if (logs.writeIndex.compare_exchange_weak(wIdx, wIdx + 1, std::memory_order_acq_rel,
                                                          std::memory_order_relaxed)) {
                    reservedSlot = true;
                    break;
                }
                // CAS failed (another thread beat us) — retry with updated wIdx
            }
            if (reservedSlot) {
                uint32_t slotIdx = wIdx % SharedMemoryLayout::LogBuffer::SLOT_COUNT;
                char* slot = logs.buffer[slotIdx];
                snprintf(slot, SharedMemoryLayout::LogBuffer::SLOT_SIZE, "[%s] %s", baseFilename, lineBuffer);
                logs.committed[slotIdx].store(1, std::memory_order_release);
                return;  // Done — logger service will write to file.
            }
        }
        // IPC exists but not connected yet (shm is null) or buffer full
        // Fall through to file logging instead of dropping the log
    }

    // --- FALLBACK: Direct File Logging (Before IPC Connects) ---
    // Only lock for file I/O
    static std::mutex s_FileLogMutex;
    static char s_logDir[MAX_PATH] = {0};
    // Cache open handles to avoid open/close per message (SSD wear prevention).
    // Map: filename -> open HANDLE. Closed when the DLL unloads.
    struct FileHandleCache {
        std::unordered_map<std::string, HANDLE> handles;
        ~FileHandleCache() {
            for (auto& kv : handles)
                if (kv.second != INVALID_HANDLE_VALUE)
                    CloseHandle(kv.second);
        }
    };
    static FileHandleCache s_FileCache;

    // Use unique_lock with try_lock to prevent deadlocks in weird re-entrancy
    // cases
    std::unique_lock<std::mutex> lock(s_FileLogMutex, std::defer_lock);
    if (!lock.try_lock())
        return;  // Drop log if locked (avoid stalling)

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

        // Look up or open a cached handle (no FILE_FLAG_WRITE_THROUGH to avoid
        // synchronous per-write flush; the OS page-cache flush on handle close or
        // process exit is sufficient for debug logs).
        auto it = s_FileCache.handles.find(fullLogPath);
        HANDLE hFile = INVALID_HANDLE_VALUE;
        if (it != s_FileCache.handles.end()) {
            hFile = it->second;
        } else {
            hFile = CreateFileA(fullLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                // Write version header on first open (empty file)
                LARGE_INTEGER sz;
                sz.QuadPart = 0;
                if (GetFileSizeEx(hFile, &sz) && sz.QuadPart == 0) {
                    char header[512];
                    int hlen = snprintf(header, sizeof(header), "[BUILD] Version=%s Built=%s\r\n", CAPTURE_VERSION,
                                        BUILD_TIMESTAMP);
                    if (hlen > 0) {
                        DWORD hwritten;
                        WriteFile(hFile, header, (DWORD)hlen, &hwritten, NULL);
                    }
                }
                s_FileCache.handles[fullLogPath] = hFile;
            }
        }

        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(hFile, lineBuffer, len, &written, NULL);
            WriteFile(hFile, "\r\n", 2, &written, NULL);
        }
    }
}

void EarlyLog(const char* fmt, ...) {
    // No hook-side logging when debug logging is disabled.
    if (!g_pLocalConfig || !g_pLocalConfig->debugLogging)
        return;

    va_list args;
    va_start(args, fmt);
    LogToFileAtomic("hook_debug.log", fmt, args);
    va_end(args);
}

// Logs to hook_debug.log (respects the debugLogging flag just like HookLog)
void HookLogImportant(const char* fmt, ...) {
    if (g_pLocalConfig && !g_pLocalConfig->debugLogging)
        return;
    va_list args;
    va_start(args, fmt);
    LogToFileAtomic("hook_debug.log", fmt, args, true);
    va_end(args);
}

void NVNGXLog(const char* fmt, ...) {
    if (!g_pLocalConfig || !g_pLocalConfig->debugLogging)
        return;
    va_list args;
    va_start(args, fmt);
    LogToFileAtomic("nvngx_debug.log", fmt, args);
    va_end(args);
}

void ReportLUID(uint32_t low, uint32_t high) {
    // Always initialize local metrics collector first
    SystemMetricsCollector::Get().Initialize(low, high);

    if (g_IPC && g_IPC->GetSharedMem()) {
        if (g_IPC->GetSharedMem()->GetLuidLowPart() != (int32_t)low ||
            g_IPC->GetSharedMem()->GetLuidHighPart() != (int32_t)high) {
            g_IPC->GetSharedMem()->SetLuidLowPart((int32_t)low);
            g_IPC->GetSharedMem()->SetLuidHighPart((int32_t)high);
            HookLog("Common: Reported LUID to SHM: 0x%08X_%08X", high, low);
        }
    }
}

// Internal worker implementation
static void HookLogInternal(LogLevel level, const char* fmt, va_list args) {
    if (g_IPC && g_IPC->GetSharedMem()) {
        if ((int)level > (int)g_IPC->GetSharedMem()->GetLogLevel())
            return;
        if (!g_IPC->GetSharedMem()->GetDebugLogging())
            return;
    } else if (!g_pLocalConfig || !g_pLocalConfig->debugLogging) {
        return;
    }

    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), fmt, args);

    const char* levelStr = "INFO";
    switch (level) {
        case LogLevel::Error:
            levelStr = "ERROR";
            break;
        case LogLevel::Warn:
            levelStr = "WARN";
            break;
        case LogLevel::Debug:
            levelStr = "DEBUG";
            break;
        default:
            break;
    }

    EarlyLog("[%s] %s", levelStr, buffer);
}

void HookLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    HookLogInternal(LogLevel::Info, fmt, args);
    va_end(args);
}

void HookLog(LogLevel level, const char* fmt, ...) {
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
        mergedConfig.frameLatency = shmGfx.frameLatency;
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
        mergedConfig.parsed.dlssFGFactor = shmGfx.dlssFGFactor;

        if (shmGfx.dlssSRPreset > 0) {
            static uint32_t lastLoggedSHM = 0;
            if (shmGfx.dlssSRPreset != lastLoggedSHM) {
                HookLog("Config: Received SRPreset %u from SHM", shmGfx.dlssSRPreset);
                lastLoggedSHM = shmGfx.dlssSRPreset;
            }
        }
    } else {
        // No IPC, stick to defaults
        mergedConfig = GraphicsConfig();
    }

    // Apply Overrides from g_pLocalConfig
    if (g_pLocalConfig) {
        if (g_pLocalConfig->graphics.parsed.srPreset > 0) {
            static uint32_t lastLoggedLocal = 0;
            if (g_pLocalConfig->graphics.parsed.srPreset != lastLoggedLocal) {
                HookLog("Config: Local srPreset is %u", g_pLocalConfig->graphics.parsed.srPreset);
                lastLoggedLocal = g_pLocalConfig->graphics.parsed.srPreset;
            }
        }
        if (g_pLocalConfig->graphics.cpuPrerenderLimit > -0.5f) {
            mergedConfig.cpuPrerenderLimit = g_pLocalConfig->graphics.cpuPrerenderLimit;
        }
        if (g_pLocalConfig->graphics.vsyncMode != "default" && !g_pLocalConfig->graphics.vsyncMode.empty()) {
            mergedConfig.vsyncMode = g_pLocalConfig->graphics.vsyncMode;
        }
        if (g_pLocalConfig->graphics.backbufferCount > 0) {
            mergedConfig.backbufferCount = g_pLocalConfig->graphics.backbufferCount;
        }
        if (g_pLocalConfig->graphics.frameLatency > 0) {
            mergedConfig.frameLatency = g_pLocalConfig->graphics.frameLatency;
        }
        if (g_pLocalConfig->graphics.sgssaa) {
            mergedConfig.sgssaa = g_pLocalConfig->graphics.sgssaa;
        }
        if (g_pLocalConfig->graphics.disableAutoMipBias) {
            mergedConfig.disableAutoMipBias = g_pLocalConfig->graphics.disableAutoMipBias;
        }
        if (g_pLocalConfig->graphics.dlssAutoExposure != "default" &&
            !g_pLocalConfig->graphics.dlssAutoExposure.empty()) {
            mergedConfig.dlssAutoExposure = g_pLocalConfig->graphics.dlssAutoExposure;
        }
        if (g_pLocalConfig->graphics.dlssExposureNormalization != "default" &&
            !g_pLocalConfig->graphics.dlssExposureNormalization.empty()) {
            mergedConfig.dlssExposureNormalization = g_pLocalConfig->graphics.dlssExposureNormalization;
        }

        // Missing overrides added to fix regression
        if (g_pLocalConfig->graphics.anisotropicFiltering != "default" &&
            !g_pLocalConfig->graphics.anisotropicFiltering.empty()) {
            mergedConfig.anisotropicFiltering = g_pLocalConfig->graphics.anisotropicFiltering;
        }
        if (g_pLocalConfig->graphics.mipMapping != "default" && !g_pLocalConfig->graphics.mipMapping.empty()) {
            mergedConfig.mipMapping = g_pLocalConfig->graphics.mipMapping;
        }
        if (g_pLocalConfig->graphics.mipBias != "default" && !g_pLocalConfig->graphics.mipBias.empty()) {
            mergedConfig.mipBias = g_pLocalConfig->graphics.mipBias;
        }
        if (g_pLocalConfig->graphics.msaaSamples != "default" && !g_pLocalConfig->graphics.msaaSamples.empty()) {
            mergedConfig.msaaSamples = g_pLocalConfig->graphics.msaaSamples;
        }

        // Apply Preset Overrides from g_pLocalConfig
        if (g_pLocalConfig->graphics.parsed.presetDLAA > 0)
            mergedConfig.parsed.presetDLAA = g_pLocalConfig->graphics.parsed.presetDLAA;
        if (g_pLocalConfig->graphics.parsed.presetQuality > 0)
            mergedConfig.parsed.presetQuality = g_pLocalConfig->graphics.parsed.presetQuality;
        if (g_pLocalConfig->graphics.parsed.presetBalanced > 0)
            mergedConfig.parsed.presetBalanced = g_pLocalConfig->graphics.parsed.presetBalanced;
        if (g_pLocalConfig->graphics.parsed.presetPerformance > 0)
            mergedConfig.parsed.presetPerformance = g_pLocalConfig->graphics.parsed.presetPerformance;
        if (g_pLocalConfig->graphics.parsed.presetUltraPerformance > 0)
            mergedConfig.parsed.presetUltraPerformance = g_pLocalConfig->graphics.parsed.presetUltraPerformance;
        if (g_pLocalConfig->graphics.parsed.presetUltraQuality > 0)
            mergedConfig.parsed.presetUltraQuality = g_pLocalConfig->graphics.parsed.presetUltraQuality;

        if (g_pLocalConfig->graphics.parsed.rrPresetDLAA > 0)
            mergedConfig.parsed.rrPresetDLAA = g_pLocalConfig->graphics.parsed.rrPresetDLAA;
        if (g_pLocalConfig->graphics.parsed.rrPresetQuality > 0)
            mergedConfig.parsed.rrPresetQuality = g_pLocalConfig->graphics.parsed.rrPresetQuality;
        if (g_pLocalConfig->graphics.parsed.rrPresetBalanced > 0)
            mergedConfig.parsed.rrPresetBalanced = g_pLocalConfig->graphics.parsed.rrPresetBalanced;
        if (g_pLocalConfig->graphics.parsed.rrPresetPerformance > 0)
            mergedConfig.parsed.rrPresetPerformance = g_pLocalConfig->graphics.parsed.rrPresetPerformance;
        if (g_pLocalConfig->graphics.parsed.rrPresetUltraPerformance > 0)
            mergedConfig.parsed.rrPresetUltraPerformance = g_pLocalConfig->graphics.parsed.rrPresetUltraPerformance;
        if (g_pLocalConfig->graphics.parsed.rrPresetUltraQuality > 0)
            mergedConfig.parsed.rrPresetUltraQuality = g_pLocalConfig->graphics.parsed.rrPresetUltraQuality;

        // Apply Global Preset Overrides from g_pLocalConfig
        if (g_pLocalConfig->graphics.parsed.srPreset > 0)
            mergedConfig.parsed.srPreset = g_pLocalConfig->graphics.parsed.srPreset;
        if (g_pLocalConfig->graphics.parsed.rrPreset > 0)
            mergedConfig.parsed.rrPreset = g_pLocalConfig->graphics.parsed.rrPreset;

        if (g_pLocalConfig->graphics.parsed.dlssSharpening > -1.5f) {
            mergedConfig.parsed.dlssSharpening = g_pLocalConfig->graphics.parsed.dlssSharpening;
        }
        if (g_pLocalConfig->graphics.parsed.dlssFGFactor > 0) {
            mergedConfig.parsed.dlssFGFactor = g_pLocalConfig->graphics.parsed.dlssFGFactor;
        }
    }
    // Add other fields as needed

    // Update global performance gating flag
    bool anyActive = false;
    if (mergedConfig.vsyncMode != "default" && !mergedConfig.vsyncMode.empty())
        anyActive = true;
    else if (mergedConfig.anisotropicFiltering != "default" && !mergedConfig.anisotropicFiltering.empty())
        anyActive = true;
    else if (mergedConfig.mipMapping != "default" && !mergedConfig.mipMapping.empty())
        anyActive = true;
    else if (mergedConfig.mipBias != "default" && !mergedConfig.mipBias.empty())
        anyActive = true;
    else if (mergedConfig.msaaSamples != "default" && !mergedConfig.msaaSamples.empty())
        anyActive = true;
    else if (mergedConfig.cpuPrerenderLimit > -0.5f)
        anyActive = true;
    else if (mergedConfig.backbufferCount > 0)
        anyActive = true;
    else if (mergedConfig.frameLatency > 0)
        anyActive = true;
    else if (mergedConfig.sgssaa)
        anyActive = true;
    else if (mergedConfig.dlssAutoExposure != "default" && !mergedConfig.dlssAutoExposure.empty())
        anyActive = true;
    else if (mergedConfig.dlssExposureNormalization != "default" && !mergedConfig.dlssExposureNormalization.empty())
        anyActive = true;
    else if (mergedConfig.parsed.presetDLAA > 0 || mergedConfig.parsed.presetQuality > 0)
        anyActive = true;
    else if (mergedConfig.parsed.rrPresetDLAA > 0 || mergedConfig.parsed.rrPresetQuality > 0)
        anyActive = true;
    else if (mergedConfig.parsed.srPreset > 0 || mergedConfig.parsed.rrPreset > 0)
        anyActive = true;
    else if (mergedConfig.parsed.dlssSharpening > -1.5f)
        anyActive = true;
    else if (mergedConfig.parsed.dlssFGFactor > 0)
        anyActive = true;

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

// Process VSync override on Present parameters
void ProcessVSyncOverride(UINT& SyncInterval, UINT& Flags) {
    VSyncOverride override = GetVSyncOverride();
    if (!override.shouldOverride)
        return;

    // Apply the sync interval override
    SyncInterval = override.presentInterval;

    // Flag manipulation for mailbox mode
    if (override.useMailbox) {
        // DXGI_PRESENT_ALLOW_TEARING requires sync interval 0
        SyncInterval = 0;
        Flags |= 0x200;  // DXGI_PRESENT_ALLOW_TEARING
    } else if (SyncInterval > 0) {
        // VSync enabled: MUST clear ALLOW_TEARING flag
        // DXGI spec: ALLOW_TEARING is only valid with SyncInterval=0
        Flags &= ~0x200;  // Clear DXGI_PRESENT_ALLOW_TEARING
    }
}
