#pragma once

#include <cstdint>
#include <string>
#include "shared_defs.h"

// Initialize logging (opens file)
void Log_Init(const std::string& filename, LogLevel level = LogLevel::Debug);
void Log_Shutdown();
void Log_SetLevel(LogLevel level);
LogLevel Log_GetLevel();
bool Log_IsEnabled(LogLevel level);

void Log(LogLevel level, const char* format, ...) __attribute__((format(printf, 2, 3)));
void LogInfo(const char* format, ...) __attribute__((format(printf, 1, 2)));
void LogError(const char* format, ...) __attribute__((format(printf, 1, 2)));
void LogDebug(const char* format, ...) __attribute__((format(printf, 1, 2)));
void LogWarn(const char* format, ...) __attribute__((format(printf, 1, 2)));

int64_t Log_GetQpcUs();
