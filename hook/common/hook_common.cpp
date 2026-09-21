#include "hook_common.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <mutex>
#include <unordered_map>
#include "../../common/log_privacy.h"
#include "../../common/shared_defs.h"
#include "fps_limiter.h"
#include "hook_context.h"
#include "performance_metrics.h"
#include "system_metrics.h"

char g_ProcessName[260] = "unknown";

// Mutex to protect sync operations - prevents races during state transitions
static std::mutex g_SyncMutex;

// Sync HookContext with legacy global variables
// This provides a bridge during the gradual migration from scattered globals to
// HookContext.
// Callers already sync after HookContext initialization and when shared legacy
// state changes; this bridge remains until the remaining globals are retired.
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

bool GetSessionLogsDirectory(char* outDir, size_t outDirLen) {
    if (!outDir || outDirLen == 0)
        return false;
    outDir[0] = '\0';

    // Try session-specific logs path from DiscoveryInfo first
    char logDir[MAX_PATH] = {};
    HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (hDisc) {
        DiscoveryInfo* pDisc = (DiscoveryInfo*)MapViewOfFile(hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
        if (ValidateDiscoveryInfo(pDisc) && pDisc->logsPath[0]) {
            strncpy(logDir, pDisc->logsPath, sizeof(logDir) - 1);
        }
        if (pDisc)
            UnmapViewOfFile(pDisc);
        CloseHandle(hDisc);
    }

    // Fall back to {moduleDir}\logs if DiscoveryInfo unavailable
    if (logDir[0] == '\0') {
        HMODULE hMod = NULL;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)&GetSessionLogsDirectory, &hMod) ||
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

        int written = snprintf(logDir, sizeof(logDir), "%s\\logs", modulePath);
        if (written <= 0 || written >= (int)sizeof(logDir))
            return false;
    }

    int written = snprintf(outDir, outDirLen, "%s", logDir);
    if (written <= 0 || (size_t)written >= outDirLen) {
        outDir[0] = '\0';
        return false;
    }

    return true;
}

bool BuildLogFilePathForModuleAddress(const void* address, const char* fileName, char* outPath, size_t outPathLen) {
    if (!outPath || outPathLen == 0)
        return false;
    outPath[0] = '\0';
    if (!fileName || fileName[0] == '\0')
        return false;
    (void)address;

    char logDir[MAX_PATH] = {};
    if (!GetSessionLogsDirectory(logDir, sizeof(logDir)))
        return false;

    if (HookDebugLoggingEnabled()) {
        CreateDirectoryA(logDir, NULL);
    }

    int written = snprintf(outPath, outPathLen, "%s\\%s", logDir, fileName);
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
    // Log privacy: strip user-profile account names before the line reaches
    // either the SHM ring (consumed by the logger service) or direct file I/O.
    len_buf = static_cast<int>(ce::privacy::RedactUserAccountComponents(
        formatBuffer, static_cast<size_t>(len_buf) < sizeof(formatBuffer) ? static_cast<size_t>(len_buf)
                                                                          : sizeof(formatBuffer) - 1));

    SYSTEMTIME st;
    GetLocalTime(&st);
    DWORD tid = GetCurrentThreadId();

    // Get sequence number for ordering diagnostics
    uint64_t seq = g_LogSequence.fetch_add(1, std::memory_order_relaxed);

    int len =
        snprintf(lineBuffer, sizeof(lineBuffer), "[%02d:%02d:%02d.%03d] [T:%04lX] [S:%llu] [%s] %s", st.wHour,
                 st.wMinute, st.wSecond, st.wMilliseconds, tid, (unsigned long long)seq, g_ProcessName, formatBuffer);

    if (len <= 0)
        return;
    if (len >= (int)sizeof(lineBuffer))
        len = (int)sizeof(lineBuffer) - 1;
    lineBuffer[len] = '\0';

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

    // Messages that reached neither the shared-memory ring nor the file.
    //
    // The file fallback must never stall a game thread, so a contended lock
    // gives up - that part is right. Giving up *silently* is not. The loss is
    // small (6 lines in session `20260921_183446`) but it was unmeasurable:
    // only the per-process `[S:N]` sequence gaps hinted at it, and those are
    // ambiguous because the counter spans every log file a process writes.
    // The count is reported by the next writer instead of vanishing.
    static std::atomic<uint32_t> s_DroppedFileLogs{0};

    // Use unique_lock with try_lock to prevent deadlocks in weird re-entrancy
    // cases
    std::unique_lock<std::mutex> lock(s_FileLogMutex, std::defer_lock);
    if (!lock.try_lock()) {
        s_DroppedFileLogs.fetch_add(1, std::memory_order_relaxed);
        return;  // Never stall a game thread on a contended log write
    }

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
        bool insertedHandle = false;
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
                    int hlen = snprintf(header, sizeof(header), "[BUILD] Version=%s Built=%s\r\n",
                                        GetCaptureVersion(), GetBuildTimestamp());
                    if (hlen > 0) {
                        DWORD hwritten;
                        WriteFile(hFile, header, (DWORD)hlen, &hwritten, NULL);
                    }
                }
                s_FileCache.handles[fullLogPath] = hFile;
                it = s_FileCache.handles.find(fullLogPath);
                insertedHandle = true;
            }
        }

        if (hFile != INVALID_HANDLE_VALUE) {
            // Report what the contended-lock and ring-full paths gave up on, so
            // a gap in the sequence numbers always has a line explaining it.
            const uint32_t droppedByLock = s_DroppedFileLogs.exchange(0, std::memory_order_relaxed);
            if (droppedByLock != 0) {
                char dropLine[256];
                const int dropLen =
                    snprintf(dropLine, sizeof(dropLine),
                             "[LOGGING] %u line(s) dropped: the shared-memory ring was full and the file "
                             "fallback lock was contended\r\n",
                             droppedByLock);
                if (dropLen > 0) {
                    DWORD dropWritten = 0;
                    WriteFile(hFile, dropLine, static_cast<DWORD>(dropLen), &dropWritten, NULL);
                }
            }

            const DWORD lineLen = static_cast<DWORD>(strlen(lineBuffer));
            DWORD written = 0;
            bool lineOk = WriteFile(hFile, lineBuffer, lineLen, &written, NULL) && written == lineLen;
            bool newlineOk = lineOk && WriteFile(hFile, "\r\n", 2, &written, NULL) && written == 2;
            if (!newlineOk) {
                CloseHandle(hFile);
                if (it != s_FileCache.handles.end()) {
                    s_FileCache.handles.erase(it);
                } else if (insertedHandle) {
                    s_FileCache.handles.erase(fullLogPath);
                }
            }
        }
    }
}

void EarlyLog(const char* fmt, ...) {
    // No hook-side logging when debug logging is disabled.
    if (!HookDebugLoggingEnabled())
        return;

    va_list args;
    va_start(args, fmt);
    LogToFileAtomic("hook_debug.log", fmt, args);
    va_end(args);
}

// Logs to hook_debug.log (respects the debugLogging flag just like HookLog)
void HookLogImportant(const char* fmt, ...) {
    if (!HookDebugLoggingEnabled())
        return;
    va_list args;
    va_start(args, fmt);
    LogToFileAtomic("hook_debug.log", fmt, args, true);
    va_end(args);
}

void NVNGXLog(const char* fmt, ...) {
    if (!HookDebugLoggingEnabled())
        return;
    va_list args;
    va_start(args, fmt);
    LogToFileAtomic("nvngx_debug.log", fmt, args);
    va_end(args);
}

void ReportLUID(uint32_t low, uint32_t high) {
    // Always initialize local metrics collector first
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    SystemMetricsCollector::Get().Initialize(low, high);

    if (g_IPC && g_IPC->GetSharedMem()) {
        if (g_IPC->GetSharedMem()->GetLuidLowPart() != (int32_t)low ||
            g_IPC->GetSharedMem()->GetLuidHighPart() != (int32_t)high) {
            g_IPC->GetSharedMem()->SetLuidLowPart((int32_t)low);
            g_IPC->GetSharedMem()->SetLuidHighPart((int32_t)high);
            HookLog("Common: Reported LUID to SHM: 0x%08X_%08X", high, low);
        }
        g_IPC->GetSharedMem()->SetLuidSourcePid(GetCurrentProcessId());
    }
}

// Internal worker implementation
static void HookLogInternal(LogLevel level, const char* fmt, va_list args) {
    if (g_IPC && g_IPC->GetSharedMem()) {
        if ((int)level > (int)g_IPC->GetSharedMem()->GetLogLevel())
            return;
    }
    if (!HookDebugLoggingEnabled()) {
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
