// The CaptureEngine app as a client of the runtime's public C API (include/cengine/cengine.hpp): the tray, the
// console handler, --launch and --auto-record. Everything else (helpers, recording, hotkeys, desktop overlay,
// screenshots, settings) runs inside the runtime; this thread only reacts to its events and pumps the tray.
#include "frontend/app/frontend_main.h"

#include "frontend/app/auto_record.h"
#include "frontend/tray/tray.h"
#include "include/cengine/cengine.hpp"
#include "captureengine/elevation/startup_control.h"
#include "captureengine/sensors/pawnio_setup.h"
#include "captureengine/sensors/pawnio_workers.h"
#include "captureengine/app/role_configuration.h"

#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

// The frontend's own log lines. Once the runtime exists they go to its log (ce_runtime_log, tagged with the client
// name). Lines written before create are kept and emitted right after it. Without a runtime log (create failed, or
// the runtime is already gone) they go to the debugger instead.
class FrontendLog {
public:
    void Write(cengine::LogLevel level, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        switch (state_) {
            case State::Pending:
                pending_.push_back({level, message});
                break;
            case State::Attached:
                (void)runtime_->log(level, message);
                break;
            case State::Closed:
                OutputDebugStringA((message + "\n").c_str());
                break;
        }
    }

    void Attach(cengine::Runtime& runtime) {
        std::lock_guard<std::mutex> lock(mutex_);
        runtime_ = &runtime;
        state_ = State::Attached;
        for (const Line& line : pending_)
            (void)runtime.log(line.level, line.text);
        pending_.clear();
    }

    // Lines still pending go to the debugger, and so does everything written from now on.
    void Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const Line& line : pending_)
            OutputDebugStringA((line.text + "\n").c_str());
        pending_.clear();
        runtime_ = nullptr;
        state_ = State::Closed;
    }

private:
    enum class State { Pending, Attached, Closed };
    struct Line {
        cengine::LogLevel level;
        std::string text;
    };

    std::mutex mutex_;
    State state_ = State::Pending;
    cengine::Runtime* runtime_ = nullptr;
    std::vector<Line> pending_;
};

FrontendLog& FrontendLogSink() {
    static FrontendLog log;
    return log;
}

// The raw handle, not the wrapper: the handler thread may run while the main thread destroys the runtime, and the
// C ABI answers a stale handle with CE_E_INVALID_ARGUMENT, while the wrapper's own fields are not shared state.
std::atomic<ce_runtime_t*> g_ConsoleRuntime{nullptr};

BOOL WINAPI ControllerConsoleHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT ||
        ctrlType == CTRL_LOGOFF_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT) {
        FrontendLogSink().Write(cengine::LogLevel::Info,
                                "[Controller] Console event " + std::to_string(ctrlType) + " received. Cleaning up...");
        if (ce_runtime_t* runtime = g_ConsoleRuntime.load())
            (void)ce_runtime_request_shutdown(runtime);
        return TRUE;
    }
    return FALSE;
}

// The launch command arrives in the active code page; the runtime API takes UTF-8.
std::string ActiveCodePageToUtf8(const std::string& text) {
    if (text.empty())
        return {};
    const int wideSize = MultiByteToWideChar(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (wideSize <= 0)
        return {};
    std::wstring wide(static_cast<size_t>(wideSize), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), wideSize);
    const int utf8Size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideSize, nullptr, 0, nullptr, nullptr);
    if (utf8Size <= 0)
        return {};
    std::string utf8(static_cast<size_t>(utf8Size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideSize, utf8.data(), utf8Size, nullptr, nullptr);
    return utf8;
}

}  // namespace

int FrontendMain(HINSTANCE hInstance, const FrontendLaunch& launch) {
    using AutoRecordStep = ce::frontend::AutoRecordScript::Step;
    FrontendLog& frontendLog = FrontendLogSink();
    frontendLog.Write(cengine::LogLevel::Info, "[Controller] Starting...");
    PrimeStartupCursor();
    // The runtime owns the settings from here on; keep what this thread needs, read before it exists.
    const auto sensorSettings = ce::runtime::RuntimeConfiguration().hardwareSensors;

    cengine::Runtime runtime;
    bool quitRequested = false;

    // Give Explorer a real UI owner before registration or child startup can extend the launch-feedback cursor.
    // The window remains hidden and inactive.
    frontendLog.Write(cengine::LogLevel::Info, "[Controller] Creating tray icon...");
    TrayIcon::Callbacks trayCallbacks;
    trayCallbacks.onQuit = [&]() {
        quitRequested = true;
        if (runtime)
            (void)runtime.requestShutdown();
    };
    trayCallbacks.onOpenConfig = [&]() {
        ShellExecuteA(NULL, "open", launch.configPath.c_str(), NULL, NULL, SW_SHOW);
    };
    trayCallbacks.onInstallPawnIo = []() { ce::pawnio::InstallDriverAsync(); };
    // Transitional (M9): the public API has no setup request yet, so the uninstall runs directly and does not stop
    // the sensor helper first, as the controller used to.
    trayCallbacks.onUninstallPawnIo = []() { ce::pawnio::UninstallDriverAsync(); };
    trayCallbacks.isPawnIoInstalled = []() { return ce::pawnio::IsDriverInstalled(); };
    trayCallbacks.onToggleStartup = ce::startup::Toggle;
    trayCallbacks.startupPreferences = ce::startup::DisplayPreferences;
    trayCallbacks.elevationServiceStatus = ce::startup::ServiceStatusText;
    trayCallbacks.startupBusy = ce::startup::Busy;
    trayCallbacks.log = [](int32_t level, const char* message) {
        FrontendLogSink().Write(static_cast<cengine::LogLevel>(level), message);
    };
    auto tray = std::make_unique<TrayIcon>(hInstance, std::move(trayCallbacks));
    PrimeStartupCursor();

    // The package (captureengine.exe) is the runtime's own module directory; --config= names another settings file.
    cengine::Options options;
    options.clientName = "CaptureEngine";
    if (!launch.settingsPath.empty())
        options.settingsPath = launch.settingsPath;
    options.features = CE_FEATURE_GLOBAL_HOTKEYS | CE_FEATURE_DESKTOP_OVERLAY | CE_FEATURE_WATCH_SETTINGS;
    const cengine::Status created = cengine::Runtime::create(options, runtime);
    if (!created) {
        frontendLog.Write(cengine::LogLevel::Error,
                          "[Controller] Failed to create the runtime (status=" + std::to_string(created.code()) + ")");
        frontendLog.Close();
        tray->Remove();
        return 1;
    }
    frontendLog.Attach(runtime);
    g_ConsoleRuntime.store(runtime.raw());
    SetConsoleCtrlHandler(ControllerConsoleHandler, TRUE);

    ce::frontend::AutoRecordScript autoRecord =
        launch.autoRecord ? ce::frontend::AutoRecordScript(launch.autoRecordDelayMs, launch.autoRecordDurationMs)
                          : ce::frontend::AutoRecordScript();
    const HANDLE eventHandle = static_cast<HANDLE>(runtime.eventHandle());
    bool shutdownRequested = false;
    bool runtimeEnded = false;
    bool runtimeFailed = false;
    while (!runtimeEnded) {
        const DWORD waitMs = (std::min<DWORD>)(2000, autoRecord.WaitMs(GetTickCount64()));
        MsgWaitForMultipleObjectsEx(1, &eventHandle, waitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        for (;;) {
            cengine::Event event;
            if (!runtime.nextEvent(std::chrono::milliseconds(0), event))
                break;
            const ce_event_t* raw = event.raw();
            switch (event.type()) {
                case CE_EVENT_RUNTIME_STATE: {
                    const int32_t state = raw->data.runtime.state;
                    autoRecord.OnRuntimeState(state, GetTickCount64());
                    if (state == CE_RUNTIME_READY) {
                        // Offered once startup is complete so the prompt cannot delay child spawning.
                        ce::pawnio::OfferInstallationAsync(sensorSettings);
                        if (!launch.deferredLaunch.empty()) {
                            frontendLog.Write(cengine::LogLevel::Info,
                                              "[Controller] Launching deferred game: " + launch.deferredLaunch);
                            const cengine::Status launched =
                                runtime.launch(ActiveCodePageToUtf8(launch.deferredLaunch));
                            if (!launched)
                                frontendLog.Write(cengine::LogLevel::Error,
                                                  std::string("[Controller] Deferred game launch refused: ") +
                                                      launched.message());
                        }
                        if (launch.autoRecord)
                            frontendLog.Write(cengine::LogLevel::Info,
                                              "[Controller] Auto-record enabled: delay=" +
                                                  std::to_string(launch.autoRecordDelayMs) + "ms, duration=" +
                                                  std::to_string(launch.autoRecordDurationMs) + "ms");
                        PrimeStartupCursor();
                    } else if (state == CE_RUNTIME_STOPPED || state == CE_RUNTIME_FAILED) {
                        runtimeFailed = state == CE_RUNTIME_FAILED;
                        runtimeEnded = true;
                    }
                    break;
                }
                case CE_EVENT_RECORDING_STATE: {
                    const int32_t state = raw->data.recording.state;
                    tray->SetRecordingState(state == CE_RECORDING_STARTING || state == CE_RECORDING_LIVE);
                    if (autoRecord.OnRecordingState(state, raw->data.recording.failure))
                        frontendLog.Write(cengine::LogLevel::Error,
                                          "[Controller] Auto-record disabled after a recording-integrity failure");
                    break;
                }
                case CE_EVENT_COMMAND_COMPLETED:
                    autoRecord.OnCommandCompleted(event.request(), event.status().code());
                    break;
                default:
                    break;
            }
        }

        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quitRequested = true;
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        ce::startup::Pump();

        // Like the controller loop it replaces: a toggle whenever the script is due, one stop, then quit.
        const AutoRecordStep step = autoRecord.Poll(GetTickCount64());
        if (step == AutoRecordStep::ToggleRecording) {
            frontendLog.Write(cengine::LogLevel::Info,
                              autoRecord.RecordingRequested() ? "[Controller] Auto-record: stopping recording..."
                                                              : "[Controller] Auto-record: starting recording...");
            ce_request_t request = 0;
            if (runtime.toggleRecording(cengine::Mode::Video, "auto-record", &request))
                autoRecord.OnToggleIssued(request);
            else
                autoRecord.OnToggleRejected();
        } else if (step == AutoRecordStep::RequestShutdown) {
            quitRequested = true;  // exit after auto-record completes
        }

        if (quitRequested && !shutdownRequested) {
            shutdownRequested = true;
            // The tray blinks while the engine stops the helpers; this loop keeps pumping its timer.
            tray->StartShutdownAnimation();
            (void)runtime.requestShutdown();
        }
    }

    g_ConsoleRuntime.store(nullptr);
    tray->StartShutdownAnimation();
    ce::startup::Shutdown();
    ce::pawnio::ShutdownSetupWorkers();
    frontendLog.Write(cengine::LogLevel::Info, "[Controller] Exiting");
    frontendLog.Close();
    const cengine::Status destroyed = runtime.destroy();
    if (!destroyed)
        frontendLog.Write(cengine::LogLevel::Error,
                          std::string("[Controller] Runtime destroy failed: ") + destroyed.message());
    tray->Remove();
    tray.reset();
    return runtimeFailed ? 1 : 0;
}
