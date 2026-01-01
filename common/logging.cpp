#include "logging.h"
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>

static FILE *g_LogFile = nullptr;
static std::mutex g_LogMutex;

void Log_Init(const std::string &filename) {
  std::lock_guard<std::mutex> lock(g_LogMutex);
  if (g_LogFile)
    fclose(g_LogFile);
  // Use filesystem to resolve absolute if needed, but relative usually works
  // for cwd
  g_LogFile = fopen(filename.c_str(), "w"); // Overwrite on new run
}

void Log_Shutdown() {
  std::lock_guard<std::mutex> lock(g_LogMutex);
  if (g_LogFile) {
    fclose(g_LogFile);
    g_LogFile = nullptr;
  }
}

void Log(LogLevel level, const char *format, ...) {
  std::lock_guard<std::mutex> lock(g_LogMutex);
  if (!g_LogFile)
    return;

  va_list args;
  va_start(args, format);

  // Timestamp
  time_t now = time(nullptr);
  struct tm t;
  localtime_s(&t, &now);
  char timeBuf[64];
  strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &t);

  const char *levelStr = "[INFO]";
  if (level == LogLevel::Debug)
    levelStr = "[DEBUG]";
  else if (level == LogLevel::Error)
    levelStr = "[ERROR]";
  else if (level == LogLevel::Warn)
    levelStr = "[WARN]";

  fprintf(g_LogFile, "[%s] %s ", timeBuf, levelStr);
  vfprintf(g_LogFile, format, args);
  fprintf(g_LogFile, "\n");
  fflush(g_LogFile); // Ensure written immediately for crash debugging

  va_end(args);
}

void LogInfo(const char *format, ...) {
  if (!g_LogFile)
    return; // Skip all work when logging disabled
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

void LogError(const char *format, ...) {
  if (!g_LogFile)
    return; // Skip all work when logging disabled
  va_list args;
  va_start(args, format);
  char buffer[2048];
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  Log(LogLevel::Error, "%s", buffer);
}

void LogDebug(const char *format, ...) {
  if (!g_LogFile)
    return; // Skip all work when logging disabled
  va_list args;
  va_start(args, format);
  char buffer[2048];
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  Log(LogLevel::Debug, "%s", buffer);
}
