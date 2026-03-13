#pragma once

#include <cstdint>
#include <string>
#include "shared_defs.h"

// Initialize logging (opens file)
void Log_Init(const std::string& filename);
void Log_Shutdown();

void Log(LogLevel level, const char* format, ...) __attribute__((format(printf, 2, 3)));
void LogInfo(const char* format, ...) __attribute__((format(printf, 1, 2)));
void LogError(const char* format, ...) __attribute__((format(printf, 1, 2)));
void LogDebug(const char* format, ...) __attribute__((format(printf, 1, 2)));
void LogWarn(const char* format, ...) __attribute__((format(printf, 1, 2)));

int64_t Log_GetQpcUs();
