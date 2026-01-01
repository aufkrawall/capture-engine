#pragma once

#include <string>

// Initialize logging (opens file)
void Log_Init(const std::string &filename);
void Log_Shutdown();

enum class LogLevel { Debug, Info, Error };

void Log(LogLevel level, const char *format, ...);
void LogInfo(const char *format, ...);
void LogError(const char *format, ...);
void LogDebug(const char *format, ...);
