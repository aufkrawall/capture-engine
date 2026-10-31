#include "window_heartbeat.h"
#include "common/platform/thread_label.h"

#include <dwmapi.h>
#include <algorithm>
#include <mutex>
#include <system_error>
#include <thread>
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

// Window rectangles are read and placed in physical pixels, whatever awareness
// the hosting executable chose. Resolved at run time like the role host's call.
void UsePhysicalPixelsOnThisThread() {
    using SetAwareness = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (const HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        if (const auto set =
                reinterpret_cast<SetAwareness>(GetProcAddress(user32, "SetThreadDpiAwarenessContext")))
            set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
}

class WindowsBackend final : public Backend {
public:
    DWORD ForegroundProcessId() override { return ForegroundPid(); }

    bool Enumerate(std::vector<Window>& windows) override {
        return EnumWindows(ObserveWindow, reinterpret_cast<LPARAM>(&windows)) != FALSE;
    }

    std::string ProcessName(DWORD processId) override {
        return ce::process::QueryProcessIdentity(processId).imageName;
    }

    std::string ProcessPath(DWORD processId) override {
        return ce::process::QueryProcessIdentity(processId).imagePath;
    }

    DWORD SendNull(const Window& window, ULONG_PTR token, SENDASYNCPROC callback) override {
        return SendAsyncNull(window, token, callback);
    }

    DWORD Place(const Window& window, const RECT& rect) override { return PlaceAsync(window, rect); }

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
        window.frame = !(style & WS_CHILD) && !(exStyle & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE));
        if (window.processId != GetCurrentProcessId() && window.visible && !window.minimized && window.frame) {
            window.cloaked = IsCloakedOrUnknown(hwnd);
            if (!window.cloaked && !GetWindowRect(hwnd, &window.rect))
                window.rect = {};
        }
        result.push_back(std::move(window));
        return TRUE;
    }
};

thread_local Pump* currentPump = nullptr;

void CALLBACK Completed(HWND window, UINT, ULONG_PTR token, LRESULT) {
    if (currentPump)
        currentPump->Complete(window, token, GetTickCount64());
}

int64_t QpcNow() {
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

int64_t QpcFrequency() {
    LARGE_INTEGER frequency = {};
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart;
}

}  // namespace

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

DWORD PlaceAsync(const Window& window, const RECT& rect) {
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(window.handle, &pid);
    if (!tid || tid != window.threadId || pid != window.processId || tid == GetCurrentThreadId())
        return ERROR_RETRY;
    SetLastError(ERROR_SUCCESS);
    // Posted to the window's own thread: CE never waits for a stalled game.
    if (SetWindowPos(window.handle, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                     SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER))
        return ERROR_SUCCESS;
    const DWORD error = GetLastError();
    return error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
}

uint64_t GpuTimeoutUs() {
    // Windows' documented TdrDelay default, used when the value is not set.
    constexpr DWORD kDefaultSeconds = 2;
    DWORD seconds = 0;
    DWORD size = sizeof(seconds);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"TdrDelay",
                     RRF_RT_REG_DWORD, nullptr, &seconds, &size) != ERROR_SUCCESS || seconds == 0)
        seconds = kDefaultSeconds;
    return static_cast<uint64_t>(seconds) * 1'000'000ull;
}

void FeedControl::Steer(const std::vector<DWORD>& background, present_progress::Tracker& tracker) {
    if (!feed_)
        return;
    if (background.empty()) {
        if (live_ || feed_->Running()) {
            feed_->Stop();
            LogInfo("[WindowHeartbeat] Present trace released: no profiled process has a background window");
        }
        live_ = false;
        attempted_.clear();
        return;
    }
    if (feed_->Running()) {
        live_ = true;
        return;
    }
    if (live_)
        LogWarn("[WindowHeartbeat] Present trace ended unexpectedly; restarting it");
    if (live_ || background != attempted_) {
        attempted_ = background;
        live_ = feed_->Start(tracker);
    }
}

struct Service::State {
    std::mutex lifecycleMutex;
    std::mutex mutex;
    HANDLE wake = nullptr;
    std::thread worker;
    std::vector<ApplicationProfile> profiles;
    uint64_t generation = 0;
    bool stopping = false;
    // Declared before the feed, which delivers into it until the feed stops.
    present_progress::Tracker tracker{QpcFrequency()};
    std::unique_ptr<present_progress::Feed> feed;

    void Run() {
        ce::platform::NameCurrentThread("CE window heartbeat");
        UsePhysicalPixelsOnThisThread();
        const uint64_t thresholdUs = GpuTimeoutUs();
        Pump pump(GetCurrentProcessId(), thresholdUs);
        WindowsBackend backend;
        ce::log_meter::ChangeGate diagnostics(60000);
        uint64_t appliedGeneration = UINT64_MAX;
        FeedControl trace(feed.get());
        currentPump = &pump;
        while (true) {
            DWORD waitMs = INFINITE;
            bool steer = false;
            std::vector<DWORD> background;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stopping)
                    break;
                if (appliedGeneration != generation) {
                    pump.Configure(profiles);
                    appliedGeneration = generation;
                    LogDebug("[WindowHeartbeat] configured enabled=%d cadenceMs=%llu stallThresholdMs=%llu trace=%s",
                             pump.Enabled() ? 1 : 0, static_cast<unsigned long long>(kIntervalMs),
                             static_cast<unsigned long long>(thresholdUs / 1000), feed ? "service" : "none");
                }
                MSG message = {};
                // PeekMessage also dispatches asynchronous completion callbacks.
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {}
                if (pump.Enabled()) {
                    const uint64_t nowMs = GetTickCount64();
                    const TickResult result = pump.Tick(nowMs, QpcNow(), backend, tracker, Completed);
                    if (result.enumerated) {
                        steer = true;
                        background = result.background;
                    }
                    if (result.ran) {
                        const present_progress::Totals totals = tracker.Snapshot();
                        const auto verdict = diagnostics.Observe(
                            ce::log_meter::FieldKey(result.windows, static_cast<uint32_t>(result.background.size()),
                                                    result.episodes, trace.Live(), totals.returnsDelivered,
                                                    result.failed,
                                                    result.lastError),
                            nowMs);
                        if (verdict) {
                            const ce::log_meter::SuppressedNote repeats(verdict.suppressed);
                            LogDebug("[WindowHeartbeat] windows=%u background=%zu firstPid=%lu recovering=%u trace=%d "
                                     "returnsDelivered=%d presents=%llu returns=%llu losses=%llu failed=%u "
                                     "error=%lu%s", result.windows, result.background.size(),
                                     result.background.empty() ? 0ul : result.background.front(), result.episodes,
                                     trace.Live() ? 1 : 0, totals.returnsDelivered ? 1 : 0,
                                     static_cast<unsigned long long>(totals.starts),
                                     static_cast<unsigned long long>(totals.returns),
                                     static_cast<unsigned long long>(totals.losses), result.failed, result.lastError,
                                     repeats.c_str());
                        }
                    }
                    waitMs = pump.WaitMs(GetTickCount64());
                } else {
                    steer = true;
                }
            }
            // The trace lease talks to the elevation service: never under the configuration lock.
            if (steer)
                trace.Steer(background, tracker);
            if (MsgWaitForMultipleObjectsEx(1, &wake, waitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED) {
                LogWarn("[WindowHeartbeat] worker wait failed error=%lu", GetLastError());
                break;
            }
        }
        pump.Stop(backend);
        if (feed)
            feed->Stop();
        currentPump = nullptr;
        LogDebug("[WindowHeartbeat] worker stopped");
    }
};

Service::Service(std::unique_ptr<present_progress::Feed> feed) : state_(std::make_unique<State>()) {
    state_->feed = std::move(feed);
}
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
