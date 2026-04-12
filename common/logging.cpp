#include "logging.h"
#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include "config.h"

static FILE* g_LogFile = nullptr;
static std::mutex g_LogMutex;
static std::atomic<bool> g_LoggingEnabled{false};
static std::atomic<LogLevel> g_LogLevel{LogLevel::Debug};

void Log_Init(const std::string& filename, LogLevel level) {
    std::lock_guard<std::mutex> lock(g_LogMutex);
    if (g_LogFile)
        fclose(g_LogFile);
    // Use filesystem to resolve absolute if needed, but relative usually works
    // for cwd
    g_LogFile = fopen(filename.c_str(), "w");  // Overwrite on new run
    if (g_LogFile) {
        // Line-buffered: each newline flushes the buffer without a blocking
        // fflush() call on the render thread.
        setvbuf(g_LogFile, nullptr, _IOLBF, 4096);
        fprintf(g_LogFile, "[BUILD] Version=%s Built=%s\n", CAPTURE_VERSION, BUILD_TIMESTAMP);
    }
    g_LogLevel.store(level, std::memory_order_release);
    g_LoggingEnabled.store(g_LogFile != nullptr, std::memory_order_release);
}

void Log_Shutdown() {
    std::lock_guard<std::mutex> lock(g_LogMutex);
    g_LoggingEnabled.store(false, std::memory_order_release);
    if (g_LogFile) {
        fclose(g_LogFile);
        g_LogFile = nullptr;
    }
}

void Log_SetLevel(LogLevel level) {
    g_LogLevel.store(level, std::memory_order_release);
}

LogLevel Log_GetLevel() {
    return g_LogLevel.load(std::memory_order_acquire);
}

bool Log_IsEnabled(LogLevel level) {
    return g_LoggingEnabled.load(std::memory_order_acquire) &&
           static_cast<int>(level) <= static_cast<int>(g_LogLevel.load(std::memory_order_acquire));
}

void Log(LogLevel level, const char* format, ...) {
    std::lock_guard<std::mutex> lock(g_LogMutex);
    if (!g_LogFile)
        return;
    if (static_cast<int>(level) > static_cast<int>(g_LogLevel.load(std::memory_order_relaxed)))
        return;

    va_list args;
    va_start(args, format);

    char timeBuf[64];
    SYSTEMTIME localTime = {};
    GetLocalTime(&localTime);
    snprintf(timeBuf, sizeof(timeBuf), "%04u-%02u-%02u %02u:%02u:%02u.%03u", localTime.wYear, localTime.wMonth,
             localTime.wDay, localTime.wHour, localTime.wMinute, localTime.wSecond, localTime.wMilliseconds);

    const char* levelStr = "[INFO]";
    if (level == LogLevel::Trace)
        levelStr = "[TRACE]";
    else if (level == LogLevel::Debug)
        levelStr = "[DEBUG]";
    else if (level == LogLevel::Error)
        levelStr = "[ERROR]";
    else if (level == LogLevel::Warn)
        levelStr = "[WARN]";

    fprintf(g_LogFile, "[%s] %s ", timeBuf, levelStr);
    vfprintf(g_LogFile, format, args);
    fprintf(g_LogFile, "\n");
    // Flush after every write so log is accurate at crash time.
    // On Windows _IOLBF behaves as full buffering so explicit fflush is needed.
    fflush(g_LogFile);

    va_end(args);
}

void LogInfo(const char* format, ...) {
    if (!Log_IsEnabled(LogLevel::Info))
        return;
    va_list args;
    va_start(args, format);
    // Helper to avoid duplicate va_list logic, but we can't forward va_list
    // easily to Log(...) without vLog So implementing directly or forwarding
    // string
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log(LogLevel::Info, "%s", buffer);
}

void LogError(const char* format, ...) {
    if (!Log_IsEnabled(LogLevel::Error))
        return;
    va_list args;
    va_start(args, format);
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log(LogLevel::Error, "%s", buffer);
}

void LogDebug(const char* format, ...) {
    if (!Log_IsEnabled(LogLevel::Debug))
        return;
    va_list args;
    va_start(args, format);
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log(LogLevel::Debug, "%s", buffer);
}

void LogWarn(const char* format, ...) {
    if (!Log_IsEnabled(LogLevel::Warn))
        return;
    va_list args;
    va_start(args, format);
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log(LogLevel::Warn, "%s", buffer);
}

int64_t Log_GetQpcUs() {
    static const int64_t qpcFreq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();

    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    return (now.QuadPart * 1000000) / qpcFreq;
}
