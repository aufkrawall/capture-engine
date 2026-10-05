#pragma once

#include <windows.h>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "common/config/application_profile.h"

namespace ce::window_heartbeat {

constexpr uint64_t kIntervalMs = 250;

struct Window {
    HWND handle = nullptr;
    DWORD processId = 0;
    DWORD threadId = 0;
    std::string processName;
    bool visible = false;
    bool minimized = false;
    bool cloaked = true;
    bool borderlessFullscreen = false;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual DWORD ForegroundProcessId() = 0;
    virtual bool Enumerate(std::vector<Window>& windows) = 0;
    virtual DWORD SendNull(const Window& window, ULONG_PTR token, SENDASYNCPROC callback) = 0;
};

struct TickResult {
    bool ran = false;
    DWORD firstProcessId = 0;
    HWND firstWindow = nullptr;
    uint32_t eligible = 0;
    uint32_t sent = 0;
    uint32_t pending = 0;
    uint32_t failed = 0;
    DWORD lastError = ERROR_SUCCESS;
};

// The worker owns this pump and its completion callbacks on one thread. Explicit
// time and platform inputs let tests verify cadence and backpressure without sleeps.
class Pump {
public:
    explicit Pump(DWORD ownerProcessId) : ownerProcessId_(ownerProcessId) {}
    void Configure(const std::vector<ApplicationProfile>& profiles);
    bool Enabled() const { return !processNames_.empty(); }
    TickResult Tick(uint64_t nowMs, Backend& backend, SENDASYNCPROC callback);
    void Complete(HWND window, ULONG_PTR token);
    DWORD WaitMs(uint64_t nowMs) const;

private:
    struct Pending {
        DWORD processId;
        DWORD threadId;
        ULONG_PTR token;
    };
    DWORD ownerProcessId_;
    std::vector<std::string> processNames_;
    std::map<HWND, Pending> pending_;
    ULONG_PTR nextToken_ = 0;
    uint64_t lastTickMs_ = 0;
    bool hasTicked_ = false;
};

// Asynchronous system-message delivery, with a fresh HWND/thread/process/focus
// check. A successful return means queued, not that the recipient made progress.
DWORD SendAsyncNull(const Window& window, ULONG_PTR token, SENDASYNCPROC callback);

class Service {
public:
    Service();
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    bool UpdateProfiles(const std::vector<ApplicationProfile>& profiles);
    void Stop();

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace ce::window_heartbeat
