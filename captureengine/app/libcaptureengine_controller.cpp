#include "libcaptureengine_controller.h"

#include "libcaptureengine_internal.h"
#include "main_internal.h"
#include "common/ipc/inject_control_channel.h"

namespace {
ce_engine_t* g_ControllerEngine = nullptr;

ce_status_t TakeControllerScreenshot() {
    bool screenshotSaved = false;
    {
        struct CaptureScope {
            PseudoOverlay* overlay;
            const bool& saved;
            ~CaptureScope() {
                if (overlay) {
                    overlay->EndScreenshotCapture();
                    overlay->ShowScreenshotNotification(saved);
                }
            }
        } scope{main_g_PseudoOverlay.get(), screenshotSaved};
        if (scope.overlay)
            scope.overlay->BeginScreenshotCapture();
        screenshotSaved = TakeScreenshot(main_g_Config.screenshotDir, main_g_Config.screenshotColorSpace);
    }
    // Show the same result in the inject overlay (hooked game).
    ce::runtime::PublishHostNotification(
        screenshotSaved ? OverlayNotificationType::ScreenshotSaved : OverlayNotificationType::ScreenshotFailed,
        GetTickCount64() + 2000ULL);
    return screenshotSaved ? CE_SUCCESS : CE_ERROR_IO_FAILURE;
}

ce_recording_intent_t ControllerRecordingIntent() {
    const auto intent = ControllerRecordingSnapshot().pendingIntent;
    switch (intent) {
        case RecordingStartIntent::Video:
            return CE_RECORDING_INTENT_VIDEO;
        case RecordingStartIntent::AudioOnly:
            return CE_RECORDING_INTENT_AUDIO_ONLY;
        default:
            return CE_RECORDING_INTENT_IDLE;
    }
}

ce_status_t ControllerCommand(ce::api::Command command, ce_recording_intent_t intent, const char* reason) {
    using ce::api::Command;
    if (!main_g_Running)
        return CE_ERROR_INVALID_STATE;
    switch (command) {
        case Command::Start:
            if (ControllerRecordingSnapshot().requested || ControllerRecordingIntent() != CE_RECORDING_INTENT_IDLE)
                return CE_ERROR_INVALID_STATE;
            LogDebug("[EngineAPI] Recording start requested (intent=%d reason=%s)", static_cast<int>(intent),
                     reason && *reason ? reason : "engine API start");
            return StartControllerRecording(
                       intent == CE_RECORDING_INTENT_AUDIO_ONLY ? RecordingStartIntent::AudioOnly
                                                              : RecordingStartIntent::Video,
                       reason && *reason ? reason : "engine API start") == ce::controller::CommandOutcome::Accepted
                       ? CE_SUCCESS : CE_ERROR_PROCESS_FAILURE;
        case Command::Stop:
            return StopControllerRecording(reason && *reason ? reason : "engine API stop") ? CE_SUCCESS
                                                                                           : CE_ERROR_IPC_FAILURE;
        case Command::ToggleVideo:
        case Command::ToggleAudio: {
            // The snapshot selects the public error code; session policy owns
            // whether this command starts or stops recording.
            const bool stopping = ControllerRecordingSnapshot().requested;
            const auto outcome = ToggleControllerRecording(
                command == Command::ToggleAudio ? RecordingStartIntent::AudioOnly : RecordingStartIntent::Video,
                "engine API toggle");
            return outcome == ce::controller::CommandOutcome::Accepted ? CE_SUCCESS :
                   stopping ? CE_ERROR_IPC_FAILURE : CE_ERROR_PROCESS_FAILURE;
        }
        case Command::Overlay:
        case Command::Benchmark: {
            if (!ce::runtime::HostChildReady(ce::runtime::HostChild::Inject))
                return CE_ERROR_IPC_FAILURE;
            ProcessResponse response = ProcessResponse::Error;
            const auto ipcCommand =
                command == Command::Overlay ? ProcessCommand::ToggleOverlay : ProcessCommand::ToggleBenchmark;
            if (!ce::runtime::SendHostChildCommand(ce::runtime::HostChild::Inject, ipcCommand, nullptr, &response) ||
                response == ProcessResponse::Error) {
                LogWarn("[EngineAPI] Inject rejected toggle command %u", static_cast<unsigned>(ipcCommand));
                return CE_ERROR_IPC_FAILURE;
            }
            LogDebug("[EngineAPI] Inject accepted toggle command %u", static_cast<unsigned>(ipcCommand));
            return CE_SUCCESS;
        }
        case Command::Screenshot:
            return TakeControllerScreenshot();
    }
    return CE_ERROR_INVALID_ARGUMENT;
}

ce_status_t PollControllerEvents(uint32_t timeoutMs) {
    // INFINITE is a sentinel to Win32, but the API promises a bounded timeout.
    if (timeoutMs == INFINITE)
        return CE_ERROR_INVALID_ARGUMENT;
    MSG msg = {};
    if (!PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE) && timeoutMs != 0) {
        const DWORD result = MsgWaitForMultipleObjectsEx(0, nullptr, timeoutMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_FAILED)
            return CE_ERROR_PROCESS_FAILURE;
        if (result == WAIT_TIMEOUT)
            return CE_SUCCESS;
    }
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        DispatchControllerMessage(msg);
        if (!main_g_Running)
            break;
    }
    return CE_SUCCESS;
}
}  // namespace

ce_engine_t* GetControllerEngine() {
    return g_ControllerEngine;
}

ControllerApiSession::ControllerApiSession() {
    const ce::api::ControllerBackend backend{ControllerCommand, ControllerRecordingIntent, PollControllerEvents};
    if (!ce::api::BindControllerBackend(backend))
        return;
    ready_ = ce_engine_create(nullptr, &g_ControllerEngine) == CE_SUCCESS;
    if (!ready_)
        ce::api::UnbindControllerBackend();
}

ControllerApiSession::~ControllerApiSession() {
    if (ready_) {
        ce_engine_destroy(g_ControllerEngine);
        g_ControllerEngine = nullptr;
        ce::api::UnbindControllerBackend();
    }
}
