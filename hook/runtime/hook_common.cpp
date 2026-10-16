#include "hook_common.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <mutex>
#include <unordered_map>
#include "common/platform/ansi_path.h"
#include "common/logging/log_privacy.h"
#include "common/logging/log_record.h"
#include "common/logging/log_ring.h"
#include "common/ipc/shared_defs.h"
#include "hook/pacing/fps_limiter.h"
#include "hook_context.h"
#include "hook/metrics/performance_metrics.h"
#include "hook/metrics/system_metrics.h"

void HookLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void HookLog(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void EarlyLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void HookLogImportant(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void NVNGXLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

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

static std::atomic<bool> g_HookIsolatedFromHost{false};
static char g_IsolatedLogsDirectory[MAX_PATH] = {};

HANDLE OpenHostDiscoveryMapping() {
    if (g_HookIsolatedFromHost.load(std::memory_order_acquire))
        return NULL;
    return OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
}

void IsolateHookFromCaptureEngineHost(const char* logsDirectory) {
    snprintf(g_IsolatedLogsDirectory, sizeof(g_IsolatedLogsDirectory), "%s", logsDirectory ? logsDirectory : "");
    g_HookIsolatedFromHost.store(true, std::memory_order_release);
}

bool IsHookIsolatedFromHost() {
    return g_HookIsolatedFromHost.load(std::memory_order_acquire);
}

static std::atomic<HWND> g_IsolatedHostForegroundWindow{nullptr};

HWND HookForegroundWindow() {
    if (g_HookIsolatedFromHost.load(std::memory_order_acquire))
        return g_IsolatedHostForegroundWindow.load(std::memory_order_acquire);
    return GetForegroundWindow();
}

void SetIsolatedHostForegroundWindow(HWND window) {
    g_IsolatedHostForegroundWindow.store(window, std::memory_order_release);
}

bool GetSessionLogsDirectory(char* outDir, size_t outDirLen) {
    if (!outDir || outDirLen == 0)
        return false;
    outDir[0] = '\0';

    // An isolated test host's directory first, then the session-specific logs path from DiscoveryInfo
    char logDir[MAX_PATH] = {};
    strncpy(logDir, g_IsolatedLogsDirectory, sizeof(logDir) - 1);
    HANDLE hDisc = logDir[0] ? NULL : OpenHostDiscoveryMapping();
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

        // Derived in UTF-16: GetModuleFileNameA '?'-mangles an install folder
        // outside the code page and every log file then failed to open.
        const std::string moduleDir = ce::ansi_path::ModuleDirectoryAnsi(hMod);
        if (moduleDir.empty())
            return false;

        int written = snprintf(logDir, sizeof(logDir), "%s\\logs", moduleDir.c_str());
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
std::atomic<bool> g_D3D11SamplerOverridesActive{false};

// Per-file line sequence numbers. A gap in one file's `#N` column is a line that file lost (ring
// overflow or a contended fallback write), not a line written to another file.
static std::atomic<uint64_t> g_HookDebugLogSequence{0};
static std::atomic<uint64_t> g_OtherLogSequence{0};

static std::atomic<uint64_t>& LogSequenceFor(const char* baseFilename) {
    return strcmp(baseFilename, "hook_debug.log") == 0 ? g_HookDebugLogSequence : g_OtherLogSequence;
}

// Early debug log - writes directly to file without IPC dependency
// Used for debugging crashes before IPC connects
// OPTIMIZED: Uses stack buffers and avoids global locks for the hot path (SHM)
static void LogToFileAtomic(const char* baseFilename, const char* fmt, va_list args, bool forceDirectFile = false)
    __attribute__((format(printf, 2, 0)));
static void LogToFileAtomic(const char* baseFilename, const char* fmt, va_list args, bool forceDirectFile) {
    // Use stack buffers to allow concurrency without locking
    char formatBuffer[4096];
    char lineBuffer[8192];

    // Initialize Process Name once, thread-safe
    static std::mutex s_NameInitMutex;
    static std::atomic<bool> s_NameInitDone{false};

    if (!s_NameInitDone.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(s_NameInitMutex);
        if (!s_NameInitDone.load(std::memory_order_relaxed)) {
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
            s_NameInitDone.store(true, std::memory_order_release);
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

    const uint64_t seq = LogSequenceFor(baseFilename).fetch_add(1, std::memory_order_relaxed);

    // "HH:MM:SS.mmm T<tid> #<seq> p<pid> message". Several hooked processes can share one session file;
    // each process's first line ("DllMain: Process '<exe>' pid=<pid> ...") maps its pid to the exe.
    int len = snprintf(lineBuffer, sizeof(lineBuffer), "%02d:%02d:%02d.%03d T%04lX #%llu p%lu %s", st.wHour,
                       st.wMinute, st.wSecond, st.wMilliseconds, tid, (unsigned long long)seq,
                       static_cast<unsigned long>(GetCurrentProcessId()), formatBuffer);

    if (len <= 0)
        return;
    if (len >= (int)sizeof(lineBuffer))
        len = (int)sizeof(lineBuffer) - 1;
    lineBuffer[len] = '\0';

    // --- PRIMARY: Push to Shared Memory Ring Buffer (Lock-Free) ---
    // Accepted complete records go only through the ring. Full/oversized records
    // go only through direct append, so fallback cannot duplicate an accepted line.
    // An isolated test host has no logger service draining the ring: its lines go straight to the file.
    if (!forceDirectFile && g_IPC && !g_HookIsolatedFromHost.load(std::memory_order_acquire)) {
        // Load pointer atomically in case it's being torn down (unlikely but safe)
        SharedMemoryLayout* shm = g_IPC->GetSharedMem();
        if (shm) {
            if (ce::logging::TryPushLogRecord(shm->logs, baseFilename, lineBuffer) ==
                ce::logging::LogRingPush::Published) {
                return;  // Done — logger service will write to file.
            }
        }
        // IPC is not connected yet, the ring is full, or the complete record does not fit.
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
    // File/path failures remain visible on the next successful cached writer.
    static std::atomic<uint32_t> s_DroppedFileLogs{0};

    // Use unique_lock with try_lock to prevent deadlocks in weird re-entrancy
    // cases
    std::unique_lock<std::mutex> lock(s_FileLogMutex, std::defer_lock);
    if (!lock.try_lock()) {
        // An independent append handle needs no cache lock. The message and its
        // newline are one write, so this cannot splice a cached/service record.
        char independentPath[MAX_PATH] = {};
        if (BuildLogFilePathForModuleAddress((const void*)&EarlyLog, baseFilename, independentPath,
                                             sizeof(independentPath))) {
            HANDLE independentFile = CreateFileA(independentPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (independentFile != INVALID_HANDLE_VALUE) {
                const bool appended = ce::logging::AppendLogRecord(independentFile, lineBuffer);
                CloseHandle(independentFile);
                if (appended) return;
            }
        }
        s_DroppedFileLogs.fetch_add(1, std::memory_order_relaxed);
        return;
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
            // Restore the loss count if its diagnostic cannot be written either.
            const uint32_t droppedByLock = s_DroppedFileLogs.exchange(0, std::memory_order_relaxed);
            if (droppedByLock != 0) {
                char dropLine[256];
                const int dropLen =
                    snprintf(dropLine, sizeof(dropLine),
                             "[LOGGING] %u line(s) dropped after a log path/open/write failure",
                             droppedByLock);
                if (dropLen > 0) {
                    if (!ce::logging::AppendLogRecord(hFile, dropLine)) {
                        s_DroppedFileLogs.fetch_add(droppedByLock, std::memory_order_relaxed);
                    }
                }
            }

            if (!ce::logging::AppendLogRecord(hFile, lineBuffer)) {
                s_DroppedFileLogs.fetch_add(1, std::memory_order_relaxed);
                CloseHandle(hFile);
                if (it != s_FileCache.handles.end()) {
                    s_FileCache.handles.erase(it);
                } else if (insertedHandle) {
                    s_FileCache.handles.erase(fullLogPath);
                }
            }
            return;
        }
    }
    s_DroppedFileLogs.fetch_add(1, std::memory_order_relaxed);
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
static void HookLogInternal(LogLevel level, const char* fmt, va_list args) __attribute__((format(printf, 2, 0)));
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

    // Only levels that mean something get a tag; HookLogImportant/EarlyLog lines carry none either.
    switch (level) {
        case LogLevel::Error:
            EarlyLog("[ERROR] %s", buffer);
            break;
        case LogLevel::Warn:
            EarlyLog("[WARN] %s", buffer);
            break;
        default:
            EarlyLog("%s", buffer);
            break;
    }
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
