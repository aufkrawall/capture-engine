#pragma once
// log_facade.h - Unified logging interface for capture project
// Provides consistent logging across hook DLL and capture engine

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

// Log levels
enum class LogLevel { Debug = 0, Info = 1, Warning = 2, Error = 3 };

// Global log configuration
struct LogConfig {
    LogLevel minLevel = LogLevel::Info;
    bool debugOutput = true;     // OutputDebugStringA
    bool fileOutput = false;     // Log to file
    bool consoleOutput = false;  // Log to console
    char filePath[260] = {0};
};

// Thread-safe logger singleton
class Logger {
public:
    static Logger& Instance()
    {
        static Logger instance;
        return instance;
    }

    void Configure(const LogConfig& config)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;

        if (config_.fileOutput && config_.filePath[0] != '\0') {
            if (file_) fclose(file_);
            file_ = fopen(config_.filePath, "a");
        }
    }

    void Log(LogLevel level, const char* prefix, const char* format, va_list args)
    {
        if (level < config_.minLevel) return;

        std::lock_guard<std::mutex> lock(mutex_);

        // Format message
        char buffer[2048];
        int prefixLen = snprintf(buffer, sizeof(buffer), "[%s] ", prefix);
        vsnprintf(buffer + prefixLen, sizeof(buffer) - prefixLen, format, args);

        // Output to debug output (visible in debugger)
        if (config_.debugOutput) {
            OutputDebugStringA(buffer);
            OutputDebugStringA("\n");
        }

        // Output to file
        if (config_.fileOutput && file_) {
            // Add timestamp
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(file_, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buffer);
            fflush(file_);
        }

        // Output to console (if attached)
        if (config_.consoleOutput) {
            printf("%s\n", buffer);
        }
    }

    ~Logger()
    {
        if (file_) fclose(file_);
    }

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::mutex mutex_;
    LogConfig config_;
    FILE* file_ = nullptr;
};

// Convenience macros for hook DLL
#define LOG_DEBUG(prefix, fmt, ...)                                 \
    do {                                                            \
        va_list args;                                               \
        Logger::Instance().Log(LogLevel::Debug, prefix, fmt, args); \
    } while (0)

// Function-style logging (preferred)
inline void LogDebug(const char* prefix, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    Logger::Instance().Log(LogLevel::Debug, prefix, format, args);
    va_end(args);
}

inline void LogInfo(const char* prefix, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    Logger::Instance().Log(LogLevel::Info, prefix, format, args);
    va_end(args);
}

inline void LogWarning(const char* prefix, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    Logger::Instance().Log(LogLevel::Warning, prefix, format, args);
    va_end(args);
}

inline void LogError(const char* prefix, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    Logger::Instance().Log(LogLevel::Error, prefix, format, args);
    va_end(args);
}

// Hook-specific wrapper (maintains compatibility with existing HookLog calls)
inline void HookLogFacade(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    Logger::Instance().Log(LogLevel::Info, "Hook", format, args);
    va_end(args);
}

// Engine-specific wrapper (maintains compatibility with existing LogInfo calls)
inline void EngineLogFacade(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    Logger::Instance().Log(LogLevel::Info, "Engine", format, args);
    va_end(args);
}
