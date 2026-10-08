#include "window_heartbeat.h"

#include <dwmapi.h>
#include <algorithm>
#include <mutex>
#include <system_error>
#include <thread>
#include "common/capture/screen_grab_privacy.h"
#include "common/logging/log_meter.h"
#include "common/logging/logging.h"
#include "process_identity.h"
#include "secure_dll_loading.h"

namespace ce::window_heartbeat {

namespace {

DWORD ForegroundPid() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid;
}

bool IsCloakedOrUnknown(HWND window) {
    using QueryAttribute = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
    // Keep our System32 module reference for the cached function's lifetime.
    // Shared common objects also link into binaries without a DWM import library.
    static const auto query = []() -> QueryAttribute {
        const HMODULE module = ce::security::LoadSystemLibrary(L"dwmapi.dll");
        return module ? reinterpret_cast<QueryAttribute>(GetProcAddress(module, "DwmGetWindowAttribute")) : nullptr;
    }();
    DWORD cloaked = 0;
    return !query || FAILED(query(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || cloaked != 0;
}

class WindowsBackend final : public Backend {
public:
    DWORD ForegroundProcessId() override { return ForegroundPid(); }

    bool Enumerate(std::vector<Window>& windows) override {
        return EnumWindows(ObserveWindow, reinterpret_cast<LPARAM>(&windows)) != FALSE;
    }

    DWORD SendNull(const Window& window, ULONG_PTR token, SENDASYNCPROC callback) override {
        return SendAsyncNull(window, token, callback);
    }

private:
    static BOOL CALLBACK ObserveWindow(HWND hwnd, LPARAM data) {
        auto& result = *reinterpret_cast<std::vector<Window>*>(data);
        Window window;
        window.handle = hwnd;
        window.threadId = GetWindowThreadProcessId(hwnd, &window.processId);
        window.visible = IsWindowVisible(hwnd) != FALSE;
        window.minimized = IsIconic(hwnd) != FALSE;
        const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (window.processId != GetCurrentProcessId() && window.visible && !window.minimized &&
            !(style & (WS_CAPTION | WS_CHILD)) && !(exStyle & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))) {
            window.cloaked = IsCloakedOrUnknown(hwnd);
            RECT client = {};
            MONITORINFO monitor = {};
            monitor.cbSize = sizeof(monitor);
            window.borderlessFullscreen = !window.cloaked &&
                ce::screen_grab_privacy::GetWindowClientRectInScreen(hwnd, client) &&
                GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL), &monitor) &&
                ce::screen_grab_privacy::RectNearlyMatches(client, monitor.rcMonitor);
            if (window.borderlessFullscreen)
                window.processName = ce::process::QueryProcessIdentity(window.processId).imageName;
        }
        result.push_back(std::move(window));
        return TRUE;
    }
};

thread_local Pump* currentPump = nullptr;

void CALLBACK Completed(HWND window, UINT, ULONG_PTR token, LRESULT) {
    if (currentPump)
        currentPump->Complete(window, token);
}

}  // namespace

void Pump::Configure(const std::vector<ApplicationProfile>& profiles) {
    processNames_.clear();
    for (const auto& profile : profiles) {
        if (profile.windowHeartbeatEnabled && profile.target.HasProcess())
            processNames_.push_back(profile.target.pattern);
    }
    // Keep outstanding tokens across disable/re-enable. A late completion must
    // never release a newer request, and reconfiguration must not queue duplicates.
}

TickResult Pump::Tick(uint64_t nowMs, Backend& backend, SENDASYNCPROC callback) {
    TickResult result;
    if (!Enabled() || (hasTicked_ && nowMs - lastTickMs_ < kIntervalMs))
        return result;
    lastTickMs_ = nowMs;
    hasTicked_ = true;
    result.ran = true;
    std::vector<Window> windows;
    if (!backend.Enumerate(windows)) {
        result.failed = 1;
        result.lastError = ERROR_GEN_FAILURE;
        return result;
    }
    const DWORD foregroundPid = backend.ForegroundProcessId();
    for (auto it = pending_.begin(); it != pending_.end();) {
        const bool alive = std::any_of(windows.begin(), windows.end(), [&](const Window& window) {
            return window.handle == it->first && window.processId == it->second.processId &&
                   window.threadId == it->second.threadId;
        });
        if (!alive)
            it = pending_.erase(it);
        else
            ++it;
    }
    for (const auto& window : windows) {
        if (!window.handle || !window.processId || !window.threadId || !foregroundPid ||
            window.processId == foregroundPid || window.processId == ownerProcessId_ || !window.visible ||
            window.minimized || window.cloaked || !window.borderlessFullscreen)
            continue;
        const bool matches = std::any_of(processNames_.begin(), processNames_.end(), [&](const std::string& name) {
            WhitelistEntry target;
            target.pattern = name;
            return MatchesProcessName(target, window.processName, true);
        });
        if (!matches)
            continue;
        if (!result.firstWindow) {
            result.firstProcessId = window.processId;
            result.firstWindow = window.handle;
        }
        ++result.eligible;
        if (pending_.contains(window.handle)) {
            ++result.pending;
            continue;
        }
        // Never reuse a token during this worker's lifetime, even after HWND reuse.
        if (nextToken_ == UINTPTR_MAX) {
            ++result.failed;
            result.lastError = ERROR_ARITHMETIC_OVERFLOW;
            continue;
        }
        const ULONG_PTR token = ++nextToken_;
        pending_[window.handle] = {window.processId, window.threadId, token};
        const DWORD error = backend.SendNull(window, token, callback);
        if (error == ERROR_SUCCESS) {
            ++result.sent;
        } else {
            Complete(window.handle, token);
            ++result.failed;
            result.lastError = error;
        }
    }
    return result;
}

DWORD Pump::WaitMs(uint64_t nowMs) const {
    if (!Enabled())
        return INFINITE;
    const uint64_t elapsedMs = nowMs - lastTickMs_;
    return !hasTicked_ || elapsedMs >= kIntervalMs ? 0 : static_cast<DWORD>(kIntervalMs - elapsedMs);
}

void Pump::Complete(HWND window, ULONG_PTR token) {
    const auto it = pending_.find(window);
    if (it != pending_.end() && it->second.token == token)
        pending_.erase(it);
}

DWORD SendAsyncNull(const Window& window, ULONG_PTR token, SENDASYNCPROC callback, DWORD foregroundPid) {
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(window.handle, &pid);
    const DWORD effectiveForegroundPid = foregroundPid != 0 ? foregroundPid : ForegroundPid();
    if (!tid || tid != window.threadId || pid != window.processId || tid == GetCurrentThreadId() ||
        !effectiveForegroundPid || pid == effectiveForegroundPid)
        return ERROR_RETRY;
    SetLastError(ERROR_SUCCESS);
    if (SendMessageCallbackW(window.handle, WM_NULL, 0, 0, callback, token))
        return ERROR_SUCCESS;
    const DWORD error = GetLastError();
    return error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
}

struct Service::State {
    std::mutex lifecycleMutex;
    std::mutex mutex;
    HANDLE wake = nullptr;
    std::thread worker;
    std::vector<ApplicationProfile> profiles;
    uint64_t generation = 0;
    bool stopping = false;

    void Run() {
        Pump pump(GetCurrentProcessId());
        WindowsBackend backend;
        ce::log_meter::ChangeGate diagnostics(60000);
        uint64_t appliedGeneration = UINT64_MAX;
        currentPump = &pump;
        while (true) {
            DWORD waitMs = INFINITE;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stopping)
                    break;
                if (appliedGeneration != generation) {
                    pump.Configure(profiles);
                    appliedGeneration = generation;
                    LogDebug("[WindowHeartbeat] configured enabled=%d cadenceMs=%llu", pump.Enabled() ? 1 : 0,
                             static_cast<unsigned long long>(kIntervalMs));
                }
                MSG message = {};
                // PeekMessage also dispatches asynchronous completion callbacks.
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {}
                if (pump.Enabled()) {
                    const uint64_t nowMs = GetTickCount64();
                    const TickResult result = pump.Tick(nowMs, backend, Completed);
                    if (result.ran) {
                        const auto verdict = diagnostics.Observe(ce::log_meter::FieldKey(
                            result.firstProcessId, result.firstWindow, result.eligible, result.sent, result.pending,
                            result.failed, result.lastError), nowMs);
                        if (verdict) {
                            const ce::log_meter::SuppressedNote repeats(verdict.suppressed);
                            LogDebug("[WindowHeartbeat] firstPid=%lu firstWindow=%p eligible=%u sent=%u "
                                     "pending=%u failed=%u error=%lu%s", result.firstProcessId,
                                     static_cast<void*>(result.firstWindow), result.eligible, result.sent,
                                     result.pending, result.failed, result.lastError, repeats.c_str());
                        }
                    }
                    waitMs = pump.WaitMs(GetTickCount64());
                }
            }
            if (MsgWaitForMultipleObjectsEx(1, &wake, waitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED) {
                LogWarn("[WindowHeartbeat] worker wait failed error=%lu", GetLastError());
                break;
            }
        }
        currentPump = nullptr;
        LogDebug("[WindowHeartbeat] worker stopped");
    }
};

Service::Service() : state_(std::make_unique<State>()) {}
Service::~Service() { Stop(); }

bool Service::UpdateProfiles(const std::vector<ApplicationProfile>& profiles) {
    std::lock_guard<std::mutex> lifecycleLock(state_->lifecycleMutex);
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->profiles = profiles;
    ++state_->generation;
    const bool enabled = std::any_of(profiles.begin(), profiles.end(), [](const ApplicationProfile& profile) {
        return profile.windowHeartbeatEnabled && profile.target.HasProcess();
    });
    if (enabled && !state_->worker.joinable()) {
        state_->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!state_->wake) {
            LogWarn("[WindowHeartbeat] cannot create worker event error=%lu", GetLastError());
            return false;
        }
        state_->stopping = false;
        try {
            state_->worker = std::thread([this]() { state_->Run(); });
        } catch (const std::system_error& error) {
            CloseHandle(state_->wake);
            state_->wake = nullptr;
            LogWarn("[WindowHeartbeat] cannot start worker error=%d", error.code().value());
            return false;
        }
    }
    if (state_->wake)
        SetEvent(state_->wake);
    return true;
}

void Service::Stop() {
    std::lock_guard<std::mutex> lifecycleLock(state_->lifecycleMutex);
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stopping = true;
        if (state_->wake)
            SetEvent(state_->wake);
    }
    if (state_->worker.joinable())
        state_->worker.join();
    if (state_->wake) {
        CloseHandle(state_->wake);
        state_->wake = nullptr;
    }
}

}  // namespace ce::window_heartbeat
