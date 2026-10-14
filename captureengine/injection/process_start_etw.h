#pragma once

#include <windows.h>
#include <evntrace.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "captureengine/elevation/elevation_client.h"
#include "process_start_trace_consumer.h"

namespace ce::process_start {

// Extracts the file name (basename) from an NT, DOS, or relative path.
inline std::string ExtractFileName(const wchar_t* fullPath) {
    if (!fullPath || !*fullPath)
        return {};
    const wchar_t* slash = wcsrchr(fullPath, L'\\');
    if (!slash)
        slash = wcsrchr(fullPath, L'/');
    const wchar_t* name = slash ? (slash + 1) : fullPath;
    if (!*name)
        return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return {};
    std::string result(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, name, -1, result.data(), len, nullptr, nullptr);
    return result;
}

// Called once per newly started process, on the ETW consumer thread.
using StartCallback = std::function<void(DWORD pid, const std::string& imageName)>;

// Called on the manager's owning thread, never from an ETW callback. A failed
// fallback remains pending for a later Update; a successful handoff rescans once.
template <typename Stop, typename Fallback, typename Rescan>
bool RecoverStoppedTrace(bool& selected, bool running, Stop stop, Fallback fallback, Rescan rescan) {
    if (!selected || running)
        return false;
    stop();
    if (!fallback())
        return false;
    selected = false;
    rescan();
    return true;
}

class EtwMonitor {
public:
    EtwMonitor();
    ~EtwMonitor() noexcept;

    EtwMonitor(const EtwMonitor&) = delete;
    EtwMonitor& operator=(const EtwMonitor&) = delete;

    // Connects to the elevation service, leases the real-time ETW trace session,
    // and begins consuming kernel process-start events.
    bool Start(StartCallback callback);

    // Stops consumption, closes the trace handle, joins the consumer thread,
    // and releases the trace lease. Safe to call multiple times.
    void Stop() noexcept;

    bool IsRunning() const;

    // Dispatches an ETW event record to the registered callback if it represents
    // a process start. Exposed for testing and the ETW thunk.
    void OnEvent(const EVENT_RECORD* event);

private:
    static void WINAPI EventRecordCallback(EVENT_RECORD* event);
    static ULONG WINAPI BufferCallback(EVENT_TRACE_LOGFILEW* logfile);

    ce::elevation::Client broker_;
    TraceConsumer consumer_;
    StartCallback callback_;
};

}  // namespace ce::process_start
