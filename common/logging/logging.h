#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include "common/ipc/shared_defs.h"

namespace ce::log {
// One log file written by the threads bound to it (DR-40). A helper process writes everything to its
// process log (Log_Init); a role running on a runtime thread binds its own sink, so each role keeps its
// file and name (`inject.log`, `media_<id>_<pid>.log`, ...) inside one process. The level stays
// process-wide (Log_SetLevel). Lines are flushed as written, as the process log is.
class Sink;
// Opens (truncates) `path` with the same header line as Log_Init. nullptr when it cannot be opened.
std::shared_ptr<Sink> OpenSink(const std::string& path);
// The sink bound to the calling thread, or nullptr when the thread writes the process log. A thread a
// role starts takes this value at creation and binds it with SinkScope.
std::shared_ptr<Sink> CurrentSink();
// Binds `sink` to the calling thread until destroyed, then restores the previous binding. nullptr binds
// the process log.
class SinkScope {
public:
    explicit SinkScope(std::shared_ptr<Sink> sink);
    ~SinkScope();
    SinkScope(const SinkScope&) = delete;
    SinkScope& operator=(const SinkScope&) = delete;

private:
    std::shared_ptr<Sink> sink_;
    Sink* previous_;
};
}  // namespace ce::log

// Initialize logging (opens file)
void Log_Init(const std::string& filename, LogLevel level = LogLevel::Trace);
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
