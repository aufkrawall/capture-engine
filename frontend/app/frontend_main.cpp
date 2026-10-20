// The CaptureEngine app as a client of the runtime's public C API (include/cengine/cengine.hpp): the tray, the
// console handler, --launch and --auto-record. Everything else (helpers, recording, hotkeys, desktop overlay,
// screenshots, settings) runs inside the runtime; this thread only reacts to its events and pumps the tray.
#include "frontend/app/frontend_main.h"

#include "frontend/app/auto_record.h"
#include "frontend/app/setup_requests.h"
#include "frontend/app/command_line.h"
#include "frontend/tray/tray.h"
#include "include/cengine/cengine.hpp"

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

// The runtime API takes UTF-8 paths; an unpaired surrogate yields an empty string (the runtime then refuses it).
std::string WideToUtf8(const std::wstring& text) {
    if (text.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), utf8.data(), size,
                        nullptr, nullptr);
    return utf8;
}

// The folder of captureengine.exe: the package the runtime runs from.
std::wstring PackageDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0)
            return {};
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// A relative --config= names a file relative to the working directory, as it always did.
std::wstring AbsolutePath(const std::wstring& path) {
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0)
        return {};
    std::wstring absolute(required, L'\0');
    const DWORD length = GetFullPathNameW(path.c_str(), required, absolute.data(), nullptr);
    if (length == 0 || length >= required)
        return {};
    absolute.resize(length);
    return absolute;
}

// --restart-from-pid=: the instance being replaced still holds the session; give it 5 s to leave, then end it. The
// frontend's own policy (the runtime never terminates a process).
void WaitForPriorInstance(uint32_t priorPid, FrontendLog& log) {
    const HANDLE prior = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, priorPid);
    if (!prior)
        return;
    log.Write(cengine::LogLevel::Info, "[Controller] Waiting for prior instance (PID " + std::to_string(priorPid) +
                                           ") to exit...");
    if (WaitForSingleObject(prior, 5000) == WAIT_TIMEOUT) {
        log.Write(cengine::LogLevel::Warning, "[Controller] Prior instance did not exit in 5s; terminating it");
        TerminateProcess(prior, 0);
        WaitForSingleObject(prior, 1000);
    }
    CloseHandle(prior);
}

// Prefer performance cores for the app's own threads (Windows 11 EcoQoS); the helpers opt out themselves.
void DisablePowerThrottling() {
    PROCESS_POWER_THROTTLING_STATE state = {};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
}

}  // namespace

int FrontendMain(HINSTANCE hInstance) {
    using AutoRecordStep = ce::frontend::AutoRecordScript::Step;
    // An external launcher may have requested process-start feedback: clear it before anything slow.
    PrimeStartupCursor();
    const ce::frontend::FrontendOptions cli = ce::frontend::ParseCommandLine(GetCommandLineW());
    const std::wstring packageDir = PackageDirectory();
    if (cli.showLicense) {
        ShellExecuteW(nullptr, L"open", (packageDir + L"\\LICENSE.txt").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
    }
    const std::wstring settingsFile =
        cli.config == ce::frontend::ConfigArgumentStatus::Supplied ? AbsolutePath(cli.configPath)
                                                                  : packageDir + L"\\config.ini";
    if (cli.config == ce::frontend::ConfigArgumentStatus::Invalid || settingsFile.empty() || packageDir.empty()) {
        OutputDebugStringA("[CaptureEngine] Invalid runtime executable/configuration paths\n");
        return 1;
    }
    FrontendLog& frontendLog = FrontendLogSink();
    frontendLog.Write(cengine::LogLevel::Info, "[Controller] Starting...");
    for (const std::string& warning : cli.warnings)
        frontendLog.Write(cengine::LogLevel::Warning, warning);
    if (!cli.launchCommand.empty())
        frontendLog.Write(cengine::LogLevel::Info, "[Launcher] Deferred launch path: " + cli.launchCommand);
    DisablePowerThrottling();
    if (cli.restartFromPid != 0)
        WaitForPriorInstance(cli.restartFromPid, frontendLog);

    cengine::Runtime runtime;
    bool quitRequested = false;
    // The app's setup requests (tray items, startup driver offer); every answer is handled on this thread.
    ce::frontend::SetupRequests setup;

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
        ShellExecuteW(nullptr, L"open", settingsFile.c_str(), nullptr, nullptr, SW_SHOW);
    };
    trayCallbacks.setupState = [&]() { return setup.State(runtime); };
    trayCallbacks.onSetupCommand = [&](ce::frontend::TrayCommand command) {
        if (runtime)
            setup.Command(runtime, command);
    };
    trayCallbacks.log = [](int32_t level, const char* message) {
        FrontendLogSink().Write(static_cast<cengine::LogLevel>(level), message);
    };
    auto tray = std::make_unique<TrayIcon>(hInstance, std::move(trayCallbacks));
    PrimeStartupCursor();

    // The package (captureengine.exe) is the runtime's own module directory; --config= names another settings file.
    cengine::Options options;
    options.clientName = "CaptureEngine";
    if (cli.config == ce::frontend::ConfigArgumentStatus::Supplied)
        options.settingsPath = WideToUtf8(settingsFile);
    options.features = CE_FEATURE_GLOBAL_HOTKEYS | CE_FEATURE_DESKTOP_OVERLAY | CE_FEATURE_CRASH_HANDLER |
                       CE_FEATURE_WATCH_SETTINGS;
    const cengine::Status created = cengine::Runtime::create(options, runtime);
    if (!created) {
        frontendLog.Write(cengine::LogLevel::Error, std::string("[Controller] Failed to create the runtime: ") +
                                                        created.message());
        frontendLog.Close();
        tray->Remove();
        if (created.code() == CE_E_BUSY) {
            MessageBoxA(nullptr, "CaptureEngine is already running.", "Error", MB_ICONERROR);
        } else if (created.code() == CE_E_PACKAGE) {
            // Before the runtime refused such a folder, the app ran with its settings silently at defaults.
            MessageBoxA(nullptr,
                        "CaptureEngine cannot run from this folder: its path cannot be expressed in the Windows code "
                        "page and has no 8.3 short name. Install CaptureEngine to a folder with Latin characters or "
                        "enable 8.3 names on that volume.",
                        "CaptureEngine", MB_ICONERROR);
        }
        return 1;
    }
    frontendLog.Attach(runtime);
    g_ConsoleRuntime.store(runtime.raw());
    SetConsoleCtrlHandler(ControllerConsoleHandler, TRUE);

    ce::frontend::AutoRecordScript autoRecord =
        cli.autoRecordRequested ? ce::frontend::AutoRecordScript(cli.autoRecord.delayMs, cli.autoRecord.durationMs)
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
                        setup.OnReady(runtime);
                        if (!cli.launchCommand.empty()) {
                            frontendLog.Write(cengine::LogLevel::Info,
                                              "[Controller] Launching deferred game: " + cli.launchCommand);
                            const cengine::Status launched = runtime.launch(cli.launchCommand);
                            if (!launched)
                                frontendLog.Write(cengine::LogLevel::Error,
                                                  std::string("[Controller] Deferred game launch refused: ") +
                                                      launched.message());
                        }
                        if (cli.autoRecordRequested)
                            frontendLog.Write(cengine::LogLevel::Info,
                                              "[Controller] Auto-record enabled: delay=" +
                                                  std::to_string(cli.autoRecord.delayMs) + "ms, duration=" +
                                                  std::to_string(cli.autoRecord.durationMs) + "ms");
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
                    (void)setup.Completed(runtime, event.request(), event.status().code(), raw->message);
                    autoRecord.OnCommandCompleted(event.request(), event.status().code());
                    break;
                default:
                    break;
            }
        }

        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == ce::frontend::kOfferAnswerMessage && !msg.hwnd) {
                setup.OfferAnswered(runtime, static_cast<ce::frontend::OfferAnswer>(msg.wParam));
                continue;
            }
            if (msg.message == WM_QUIT) {
                quitRequested = true;
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

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
