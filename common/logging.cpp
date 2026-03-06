#include "logging.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include "config.h"

static FILE* g_LogFile = nullptr;
static std::mutex g_LogMutex;

void Log_Init(const std::string& filename) {
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
}

void Log_Shutdown() {
    std::lock_guard<std::mutex> lock(g_LogMutex);
    if (g_LogFile) {
        fclose(g_LogFile);
        g_LogFile = nullptr;
    }
}

void Log(LogLevel level, const char* format, ...) {
    std::lock_guard<std::mutex> lock(g_LogMutex);
    if (!g_LogFile)
        return;

    va_list args;
    va_start(args, format);

    char timeBuf[64];
    SYSTEMTIME localTime = {};
    GetLocalTime(&localTime);
    snprintf(timeBuf, sizeof(timeBuf), "%04u-%02u-%02u %02u:%02u:%02u.%03u", localTime.wYear, localTime.wMonth,
             localTime.wDay, localTime.wHour, localTime.wMinute, localTime.wSecond, localTime.wMilliseconds);

    const char* levelStr = "[INFO]";
    if (level == LogLevel::Debug)
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
    if (!g_LogFile)
        return;  // Skip all work when logging disabled
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
    if (!g_LogFile)
        return;  // Skip all work when logging disabled
    va_list args;
    va_start(args, format);
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log(LogLevel::Error, "%s", buffer);
}

void LogDebug(const char* format, ...) {
    if (!g_LogFile)
        return;  // Skip all work when logging disabled
    va_list args;
    va_start(args, format);
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log(LogLevel::Debug, "%s", buffer);
}

void LogWarn(const char* format, ...) {
    if (!g_LogFile)
        return;  // Skip all work when logging disabled
    va_list args;
    va_start(args, format);
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log(LogLevel::Warn, "%s", buffer);
}

int64_t Log_GetQpcUs() {
    LARGE_INTEGER now = {};
    LARGE_INTEGER freq = {};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return (now.QuadPart * 1000000) / freq.QuadPart;
}
