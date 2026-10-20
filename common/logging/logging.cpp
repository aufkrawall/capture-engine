#include "logging.h"
#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include "common/config/config.h"
#include "log_privacy.h"

static FILE* g_LogFile = nullptr;
static std::mutex g_LogMutex;
static std::atomic<bool> g_LoggingEnabled{false};
static std::atomic<LogLevel> g_LogLevel{LogLevel::Trace};

namespace ce::log {
class Sink : public std::enable_shared_from_this<Sink> {
public:
    explicit Sink(FILE* file) : file(file) {}
    ~Sink() {
        fclose(file);
    }
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;

    FILE* const file;
    std::mutex mutex;  // one Log() call holds it across format, write and flush, as g_LogMutex
};
}  // namespace ce::log

namespace {
// The sink this thread writes, or nullptr for the process log. A raw pointer: the SinkScope that
// installed it owns a reference for as long as it is bound.
thread_local ce::log::Sink* t_Sink = nullptr;

FILE* OpenLogFile(const std::string& filename) {
    FILE* file = fopen(filename.c_str(), "w");  // Overwrite on new run
    if (file) {
        // Line-buffered: each newline flushes the buffer without a blocking
        // fflush() call on the render thread.
        setvbuf(file, nullptr, _IOLBF, 4096);
        fprintf(file, "[BUILD] Version=%s Built=%s\n", GetCaptureVersion(), GetBuildTimestamp());
    }
    return file;
}
}  // namespace

namespace ce::log {
std::shared_ptr<Sink> OpenSink(const std::string& path) {
    FILE* file = OpenLogFile(path);
    return file ? std::make_shared<Sink>(file) : nullptr;
}

std::shared_ptr<Sink> CurrentSink() {
    return t_Sink ? t_Sink->shared_from_this() : nullptr;
}

SinkScope::SinkScope(std::shared_ptr<Sink> sink) : sink_(std::move(sink)), previous_(t_Sink) {
    t_Sink = sink_.get();
}

SinkScope::~SinkScope() {
    t_Sink = previous_;
}
}  // namespace ce::log

void Log_Init(const std::string& filename, LogLevel level) {
    std::lock_guard<std::mutex> lock(g_LogMutex);
    if (g_LogFile)
        fclose(g_LogFile);
    g_LogFile = OpenLogFile(filename);
    g_LogLevel.store(level, std::memory_order_release);
    g_LoggingEnabled.store(g_LogFile != nullptr, std::memory_order_release);
}

void Log_Shutdown() {
    std::lock_guard<std::mutex> lock(g_LogMutex);
    g_LoggingEnabled.store(false, std::memory_order_release);
    if (g_LogFile) {
        fclose(g_LogFile);
        g_LogFile = nullptr;
    }
}

void Log_SetLevel(LogLevel level) {
    g_LogLevel.store(level, std::memory_order_release);
}

LogLevel Log_GetLevel() {
    return g_LogLevel.load(std::memory_order_acquire);
}

bool Log_IsEnabled(LogLevel level) {
    return (t_Sink || g_LoggingEnabled.load(std::memory_order_acquire)) &&
           static_cast<int>(level) <= static_cast<int>(g_LogLevel.load(std::memory_order_acquire));
}

// One Log() call owns its file's mutex across format, redaction, write and flush, so a
// single slow call blocks every logger in the process. When that happens the log
// itself has to say so, or the stall episode leaves no trace in the very output
// at issue. Written directly (never through Log(): the mutex is already held),
// and rate-limited so a stall storm cannot amplify itself.
static void LogSlowCallNotice(FILE* file, int64_t elapsedUs) {
    static std::atomic<uint32_t> notices{0};
    const uint32_t index = notices.fetch_add(1, std::memory_order_relaxed);
    if (index >= 4)
        return;

    char timeBuf[64];
    SYSTEMTIME localTime = {};
    GetLocalTime(&localTime);
    snprintf(timeBuf, sizeof(timeBuf), "%04u-%02u-%02u %02u:%02u:%02u.%03u", localTime.wYear, localTime.wMonth,
             localTime.wDay, localTime.wHour, localTime.wMinute, localTime.wSecond, localTime.wMilliseconds);
    fprintf(file,
            "[%s] [WARN] Single Log() call held the log mutex for %lld ms (lock + format + flush); further slow-log "
            "notices are suppressed\n",
            timeBuf, static_cast<long long>(elapsedUs / 1000));
    // Flush after every write so log is accurate at crash time.
    fflush(file);
}

void Log(LogLevel level, const char* format, ...) {
    const int64_t startUs = Log_GetQpcUs();
    ce::log::Sink* const sink = t_Sink;
    std::lock_guard<std::mutex> lock(sink ? sink->mutex : g_LogMutex);
    FILE* const file = sink ? sink->file : g_LogFile;
    if (!file)
        return;
    if (static_cast<int>(level) > static_cast<int>(g_LogLevel.load(std::memory_order_relaxed)))
        return;

    va_list args;
    va_start(args, format);

    // Format into a buffer first so every line passes through log privacy
    // redaction before it reaches the file. Oversized messages fall back to a
    // heap buffer instead of being truncated.
    char stackBuffer[4096];
    std::unique_ptr<char[]> heapBuffer;
    va_list retryArgs;
    va_copy(retryArgs, args);
    int formatted = vsnprintf(stackBuffer, sizeof(stackBuffer), format, args);
    if (formatted < 0) {
        va_end(args);
        va_end(retryArgs);
        return;
    }
    char* message = stackBuffer;
    if (static_cast<size_t>(formatted) >= sizeof(stackBuffer)) {
        heapBuffer.reset(new char[static_cast<size_t>(formatted) + 1]);
        vsnprintf(heapBuffer.get(), static_cast<size_t>(formatted) + 1, format, retryArgs);
        message = heapBuffer.get();
    }
    const size_t messageLen =
        ce::privacy::RedactUserAccountComponents(message, static_cast<size_t>(formatted));
    va_end(args);
    va_end(retryArgs);

    char timeBuf[64];
    SYSTEMTIME localTime = {};
    GetLocalTime(&localTime);
    snprintf(timeBuf, sizeof(timeBuf), "%04u-%02u-%02u %02u:%02u:%02u.%03u", localTime.wYear, localTime.wMonth,
             localTime.wDay, localTime.wHour, localTime.wMinute, localTime.wSecond, localTime.wMilliseconds);

    const char* levelStr = "[INFO]";
    if (level == LogLevel::Trace)
        levelStr = "[TRACE]";
    else if (level == LogLevel::Debug)
        levelStr = "[DEBUG]";
    else if (level == LogLevel::Error)
        levelStr = "[ERROR]";
    else if (level == LogLevel::Warn)
        levelStr = "[WARN]";

    fprintf(file, "[%s] %s ", timeBuf, levelStr);
    fwrite(message, 1, messageLen, file);
    fprintf(file, "\n");
    // Flush after every write so log is accurate at crash time.
    // On Windows _IOLBF behaves as full buffering so explicit fflush is needed.
    fflush(file);

    // Self-timing: this measures the lock wait as well as the write, so the
    // calls queued behind a slow one report the same episode.
    const int64_t elapsedUs = Log_GetQpcUs() - startUs;
    if (elapsedUs >= 50'000) {
        LogSlowCallNotice(file, elapsedUs);
    }
}

void LogInfo(const char* format, ...) {
    if (!Log_IsEnabled(LogLevel::Info))
        return;
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
    if (!Log_IsEnabled(LogLevel::Error))
        return;
    va_list args;
    va_start(args, format);
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log(LogLevel::Error, "%s", buffer);
}

void LogDebug(const char* format, ...) {
    if (!Log_IsEnabled(LogLevel::Debug))
        return;
    va_list args;
    va_start(args, format);
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log(LogLevel::Debug, "%s", buffer);
}

void LogWarn(const char* format, ...) {
    if (!Log_IsEnabled(LogLevel::Warn))
        return;
    va_list args;
    va_start(args, format);
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log(LogLevel::Warn, "%s", buffer);
}

int64_t Log_GetQpcUs() {
    static const int64_t qpcFreq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();

    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    return (now.QuadPart * 1000000) / qpcFreq;
}
