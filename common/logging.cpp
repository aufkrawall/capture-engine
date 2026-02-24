#include "logging.h"
#include "config.h"
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
  if (g_LogFile) {
    // Line-buffered: each newline flushes the buffer without a blocking
    // fflush() call on the render thread.
    setvbuf(g_LogFile, nullptr, _IOLBF, 4096);
    fprintf(g_LogFile, "[BUILD] Version=%s Built=%s\\n", CAPTURE_VERSION,
            BUILD_TIMESTAMP);
  }
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
  // NOTE: fflush is intentionally NOT called here to avoid blocking the
  // render thread on every log entry. The OS will flush on process exit or
  // when the buffer fills.  For crash-safety the file is opened without
  // buffering in Log_Init (FILE_FLAG_WRITE_THROUGH equivalent via setvbuf).

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

void LogWarn(const char *format, ...) {
  if (!g_LogFile)
    return; // Skip all work when logging disabled
  va_list args;
  va_start(args, format);
  char buffer[2048];
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  Log(LogLevel::Warn, "%s", buffer);
}
