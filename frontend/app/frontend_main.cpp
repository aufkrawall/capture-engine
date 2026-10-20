// The CaptureEngine app as a client of the runtime engine (runtime/core): the tray, the console handler,
// --launch and --auto-record. Everything else (helpers, recording, hotkeys, desktop overlay, screenshots,
// settings) runs on the engine thread; this thread only reacts to engine events and pumps the tray.
#include "captureengine/app/main_internal.h"

#include "frontend/app/frontend_main.h"
#include "frontend/tray/tray.h"
#include "captureengine/elevation/startup_control.h"
#include "captureengine/sensors/pawnio_workers.h"
#include "common/platform/ansi_path.h"
#include "runtime/core/core.h"
#include "runtime/core/event_queue.h"

#include <memory>

namespace {
std::atomic<ce::engine::Runtime*> g_ConsoleRuntime{nullptr};

BOOL WINAPI ControllerConsoleHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT ||
        ctrlType == CTRL_LOGOFF_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT) {
        LogInfo("[Controller] Console event %lu received. Cleaning up...", ctrlType);
        if (ce::engine::Runtime* runtime = g_ConsoleRuntime.load())
            runtime->RequestShutdown();
        return TRUE;
    }
    return FALSE;
}

std::string ActiveCodePageToUtf8(const std::string& text) {
    const std::wstring wide = ce::ansi_path::AnsiToWide(text);
    if (wide.empty())
        return {};
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes > 0 ? bytes : 0), '\0');
    if (bytes > 0)
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), bytes, nullptr,
                            nullptr);
    return utf8;
}

// --auto-record: start a video recording `delay` after the runtime is ready, stop it `duration` later and
// quit. An integrity failure ends the script without a restart.
class AutoRecordScript {
public:
    explicit AutoRecordScript(const FrontendLaunch& launch)
        : enabled_(launch.autoRecord), delayMs_(launch.autoRecordDelayMs), durationMs_(launch.autoRecordDurationMs) {}

    void OnReady(uint64_t nowMs) {
        if (!enabled_)
            return;
        LogInfo("[Controller] Auto-record enabled: delay=%lums, duration=%lums", delayMs_, durationMs_);
        armedAtMs_ = nowMs;
    }
    void OnRecordingState(int32_t state, uint32_t failure) {
        recording_ = state == CE_RECORDING_STARTING || state == CE_RECORDING_LIVE;
        if (failure == ce::engine::kRecordingFailureIntegrity && enabled_) {
            LogError("[Controller] Auto-record disabled after a recording-integrity failure");
            enabled_ = false;
        }
    }
    // Submits what is due: like the old loop, a start whenever nothing is recording after the delay (also
    // again after a failed attempt), and one stop once the duration elapsed while recording.
    void Step(ce::engine::Runtime& runtime, uint64_t nowMs) {
        if (!enabled_ || !armedAtMs_ || stopRequest_)
            return;
        const uint64_t elapsed = nowMs - armedAtMs_;
        if (!recording_ && !startRequest_ && elapsed >= delayMs_) {
            LogInfo("[Controller] Auto-record: starting recording...");
            startRequest_ = runtime.Submit(Toggle()).request;
        } else if (recording_ && !startRequest_ && elapsed >= uint64_t(delayMs_) + durationMs_) {
            LogInfo("[Controller] Auto-record: stopping recording...");
            stopRequest_ = runtime.Submit(Toggle()).request;
        }
    }
    // A request completed. Returns true when it was the final stop: the app exits after auto-record.
    bool OnCompleted(uint64_t request) {
        if (request == startRequest_)
            startRequest_ = 0;
        return stopRequest_ != 0 && request == stopRequest_;
    }
    // Milliseconds until Step has something to do (INFINITE when nothing is scheduled).
    DWORD WaitMs(uint64_t nowMs) const {
        if (!enabled_ || !armedAtMs_ || stopRequest_ || startRequest_)
            return INFINITE;
        const uint64_t due = armedAtMs_ + (recording_ ? uint64_t(delayMs_) + durationMs_ : delayMs_);
        return due <= nowMs ? 0 : static_cast<DWORD>((std::min<uint64_t>)(due - nowMs, INFINITE - 1));
    }

private:
    static ce::engine::Command Toggle() {
        ce::engine::Command command;
        command.kind = ce::engine::CommandKind::ToggleRecording;
        command.mode = CE_MODE_VIDEO;
        command.text = "auto-record";
        return command;
    }
    bool enabled_ = false;
    DWORD delayMs_ = 0;
    DWORD durationMs_ = 0;
    uint64_t armedAtMs_ = 0;
    bool recording_ = false;
    uint64_t startRequest_ = 0;
    uint64_t stopRequest_ = 0;
};
}  // namespace

int FrontendMain(HINSTANCE hInstance, const ce::runtime::RuntimePackagePaths& paths, const FrontendLaunch& launch) {
    const int64_t controllerStartUs = Log_GetQpcUs();
    LogInfo("[Controller] Starting...");
    PrimeStartupCursor();
    // The engine thread owns the settings from here on; keep what this thread needs.
    const HardwareSensorsConfig sensorSettings = RuntimeConfiguration().hardwareSensors;

    std::unique_ptr<ce::engine::Runtime> runtime;
    uint64_t sensorStopRequest = 0;
    bool quitRequested = false;

    // Give Explorer a real UI owner before registration or child startup can extend the launch-feedback
    // cursor. The window remains hidden and inactive.
    LogInfo("[Controller] Creating tray icon...");
    const int64_t trayCreateStartUs = Log_GetQpcUs();
    TrayIcon::Callbacks trayCallbacks;
    trayCallbacks.onQuit = [&]() {
        quitRequested = true;
        if (runtime)
            runtime->RequestShutdown();
    };
    trayCallbacks.onOpenConfig = [&]() { ShellExecuteA(NULL, "open", launch.configPath.c_str(), NULL, NULL, SW_SHOW); };
    trayCallbacks.onInstallPawnIo = []() { ce::pawnio::InstallDriverAsync(); };
    trayCallbacks.onUninstallPawnIo = [&]() {
        if (!runtime)
            return;
        // The engine stops the sensor helper; the driver is removed once that completed.
        ce::engine::Command command;
        command.kind = ce::engine::CommandKind::StopSensorsForSetup;
        const ce::engine::Admission admission = runtime->Submit(command);
        if (admission.status == CE_OK)
            sensorStopRequest = admission.request;
        else
            LogError("[PawnIO] Cannot uninstall driver before the sensor process has exited");
    };
    trayCallbacks.isPawnIoInstalled = []() { return ce::pawnio::IsDriverInstalled(); };
    trayCallbacks.onToggleStartup = ce::startup::Toggle;
    trayCallbacks.startupPreferences = ce::startup::DisplayPreferences;
    trayCallbacks.elevationServiceStatus = ce::startup::ServiceStatusText;
    trayCallbacks.startupBusy = ce::startup::Busy;
    auto tray = std::make_unique<TrayIcon>(hInstance, std::move(trayCallbacks));
    const int64_t trayCreateUs = Log_GetQpcUs() - trayCreateStartUs;
    PrimeStartupCursor();

    ce::engine::RuntimeOptions options;
    options.configPath = launch.configPath;
    options.roleHost = paths.Executable();
    options.windowModule = hInstance;
    options.features = CE_FEATURE_GLOBAL_HOTKEYS | CE_FEATURE_DESKTOP_OVERLAY | CE_FEATURE_WATCH_SETTINGS;
    options.startMediaWithRuntime = launch.autoRecord;
    options.clientStartUs = controllerStartUs;
    options.clientUiUs = trayCreateUs;
    ce_status_t createStatus = CE_OK;
    runtime = ce::engine::Runtime::Create(options, &createStatus);
    if (!runtime) {
        LogError("[Controller] Failed to create the runtime (status=%d)", static_cast<int>(createStatus));
        tray->Remove();
        return 1;
    }
    g_ConsoleRuntime.store(runtime.get());
    SetConsoleCtrlHandler(ControllerConsoleHandler, TRUE);

    AutoRecordScript autoRecord(launch);
    ce::engine::EventQueue& events = runtime->Events();
    HANDLE eventHandle = static_cast<HANDLE>(events.Handle());
    bool shutdownRequested = false;
    bool runtimeEnded = false;
    bool runtimeFailed = false;
    while (!runtimeEnded) {
        const DWORD waitMs = (std::min<DWORD>)(2000, autoRecord.WaitMs(GetTickCount64()));
        MsgWaitForMultipleObjectsEx(1, &eventHandle, waitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        const ce_event_t* event = nullptr;
        while (events.Next(0, &event) == CE_OK) {
            switch (event->type) {
                case CE_EVENT_RUNTIME_STATE:
                    if (event->data.runtime.state == CE_RUNTIME_READY) {
                        // Offered once startup is complete so the prompt cannot delay child spawning.
                        ce::pawnio::OfferInstallationAsync(sensorSettings);
                        if (!launch.deferredLaunch.empty()) {
                            LogInfo("[Controller] Launching deferred game: %s", launch.deferredLaunch.c_str());
                            ce::engine::Command command;
                            command.kind = ce::engine::CommandKind::LaunchTarget;
                            command.text = ActiveCodePageToUtf8(launch.deferredLaunch);
                            runtime->Submit(command);
                        }
                        autoRecord.OnReady(GetTickCount64());
                        PrimeStartupCursor();
                    } else if (event->data.runtime.state == CE_RUNTIME_STOPPED ||
                               event->data.runtime.state == CE_RUNTIME_FAILED) {
                        runtimeFailed = event->data.runtime.state == CE_RUNTIME_FAILED;
                        runtimeEnded = true;
                    }
                    break;
                case CE_EVENT_RECORDING_STATE: {
                    const int32_t state = event->data.recording.state;
                    tray->SetRecordingState(state == CE_RECORDING_STARTING || state == CE_RECORDING_LIVE);
                    autoRecord.OnRecordingState(state, event->data.recording.failure);
                    break;
                }
                case CE_EVENT_COMMAND_COMPLETED:
                    if (sensorStopRequest && event->request == sensorStopRequest) {
                        sensorStopRequest = 0;
                        if (event->status == CE_OK)
                            ce::pawnio::UninstallDriverAsync();
                        else
                            LogError("[PawnIO] Cannot uninstall driver before the sensor process has exited");
                    }
                    if (autoRecord.OnCompleted(event->request))
                        quitRequested = true;  // exit after auto-record completes
                    break;
                default:
                    break;
            }
            events.Release(event);
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
        autoRecord.Step(*runtime, GetTickCount64());

        if (quitRequested && !shutdownRequested) {
            shutdownRequested = true;
            // The tray blinks while the engine stops the helpers; this loop keeps pumping its timer.
            tray->StartShutdownAnimation();
            runtime->RequestShutdown();
        }
    }

    g_ConsoleRuntime.store(nullptr);
    tray->StartShutdownAnimation();
    ce::startup::Shutdown();
    ce::pawnio::ShutdownSetupWorkers();
    runtime.reset();
    tray->Remove();
    tray.reset();

    LogInfo("[Controller] Exiting");
    return runtimeFailed ? 1 : 0;
}
