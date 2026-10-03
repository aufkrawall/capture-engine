#pragma once

// Diagnostics of the fake FG runtimes: one line per call that matters, appended to fake_runtimes.log in
// the scenario's log directory (CE_FLOW_LOG_DIR), next to CE's own hook_debug.log.

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace ce::flow::fake {

inline void Log(const char* module, const char* format, ...) {
    static std::mutex s_mutex;
    char directory[MAX_PATH] = {};
    if (!GetEnvironmentVariableA("CE_FLOW_LOG_DIR", directory, MAX_PATH))
        return;
    char line[1024];
    SYSTEMTIME now;
    GetLocalTime(&now);
    int length = std::snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u T%04lX [%s] ", now.wHour, now.wMinute,
                               now.wSecond, now.wMilliseconds, GetCurrentThreadId(), module);
    va_list args;
    va_start(args, format);
    length += std::vsnprintf(line + length, sizeof(line) - static_cast<size_t>(length) - 2, format, args);
    va_end(args);
    if (length < 0 || length > static_cast<int>(sizeof(line)) - 2)
        length = static_cast<int>(sizeof(line)) - 2;
    line[length++] = '\n';
    std::lock_guard<std::mutex> lock(s_mutex);
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s\\fake_runtimes.log", directory);
    if (FILE* file = std::fopen(path, "ab")) {
        std::fwrite(line, 1, static_cast<size_t>(length), file);
        std::fclose(file);
    }
}

}  // namespace ce::flow::fake
