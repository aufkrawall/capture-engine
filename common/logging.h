#pragma once

#include <cstdint>
#include <string>
#include "shared_defs.h"

// Initialize logging (opens file)
void Log_Init(const std::string& filename);
void Log_Shutdown();

void Log(LogLevel level, const char* format, ...);
void LogInfo(const char* format, ...);
void LogError(const char* format, ...);
void LogDebug(const char* format, ...);
void LogWarn(const char* format, ...);

int64_t Log_GetQpcUs();
