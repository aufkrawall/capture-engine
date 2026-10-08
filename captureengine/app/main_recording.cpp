#include "main_internal.h"
#include "libcaptureengine_controller.h"

#include "host_children.h"

using ce::runtime::HostChild;

void PublishRecordingFailureOverlayNotification(const char* reason, bool streaming) {
    ce::runtime::PublishHostNotification(
        streaming ? OverlayNotificationType::StreamingFailed : OverlayNotificationType::RecordingFailed,
        GetTickCount64() + 7000ULL);
    LogError("[ControllerSession] Recording failure (%s, streaming=%d request=%llu)", reason, streaming ? 1 : 0,
             static_cast<unsigned long long>(ControllerRecordingSnapshot().request));
}

void CheckRecordingFailureState() { ReconcileControllerRecording(false); }

void ToggleRecording() {
    ToggleControllerRecording(RecordingStartIntent::Video, "record hotkey");
}

void ToggleAudioOnlyRecording() {
    ToggleControllerRecording(RecordingStartIntent::AudioOnly, "audio-only hotkey");
}

// Toggle the injected in-game overlay on/off at runtime. The inject process owns
// the shared-memory overlay config, so the controller only forwards the intent;
// this keeps the overlay-config seqlock single-writer.
void ToggleOverlay() {
    if (!ce::runtime::HostChildReady(HostChild::Inject)) {
        LogWarn("[Controller] Overlay toggle hotkey pressed, but no inject process is connected");
        return;
    }

    MainThreadBlockTimer _blk("overlay toggle IPC");
    ProcessResponse response = ProcessResponse::Error;
    if (!ce::runtime::SendHostChildCommand(HostChild::Inject, ProcessCommand::ToggleOverlay, nullptr, &response) ||
        response == ProcessResponse::Error) {
        LogError("[Controller] Inject process did not accept the overlay toggle");
        return;
    }
    LogInfo("[Controller] Overlay toggle hotkey handled");
}

void ToggleBenchmark() {
    if (!ce::runtime::HostChildReady(HostChild::Inject)) {
        LogWarn("[Controller] Benchmark hotkey pressed, but no inject process is connected");
        return;
    }

    MainThreadBlockTimer _blk("benchmark toggle IPC");
    ProcessResponse response = ProcessResponse::Error;
    if (!ce::runtime::SendHostChildCommand(HostChild::Inject, ProcessCommand::ToggleBenchmark, nullptr, &response) ||
        response == ProcessResponse::Error) {
        LogError("[Controller] Inject process did not accept the benchmark toggle");
        return;
    }
    LogInfo("[Controller] Benchmark toggle hotkey handled");
}

// Shutdown all child processes gracefully
void ShutdownChildProcesses() {
    LogInfo("[Controller] Shutting down child processes...");
    ShutdownControllerRecording();
    if (ce::runtime::ShutdownHostChildren()) {
        // Includes old media children still finalizing after an immediate restart.
        ce::av_sync::ReleaseSessionLatencyChannel();
    } else {
        LogError("[Controller] Child shutdown incomplete; retaining session latency ownership");
    }
}

void CheckChildProcessHealth() {
    ce::runtime::ServiceHostChildren(
        {ShouldStartLoggerProcess(RuntimeConfiguration()), ShouldStartSensorProcess(RuntimeConfiguration())},
        []() { ReconcileControllerRecording(true); });
}

bool CompleteControllerStartup() {
    if (main_g_ControllerStartupTiming.complete) {
        return true;
    }

    LogInfo("[Controller] Completing deferred startup...");
    LogInfo("[Controller] Spawning child processes...");

    const int64_t injectSpawnStartUs = Log_GetQpcUs();
    const bool injectReady = ce::runtime::EnsureHostChild(HostChild::Inject);
    const int64_t injectSpawnUs = Log_GetQpcUs() - injectSpawnStartUs;
    if (!injectReady) {
        LogError("[Controller] Failed to spawn inject process");
        return false;
    }

    int64_t mediaSpawnUs = 0;
    if (ShouldStartMediaProcessAtStartup()) {
        PrepareRecordingDiagnosticIdentity();
        const int64_t mediaSpawnStartUs = Log_GetQpcUs();
        const bool mediaReady = ce::runtime::EnsureHostChild(HostChild::Media);
        mediaSpawnUs = Log_GetQpcUs() - mediaSpawnStartUs;
        if (!mediaReady) {
            LogError("[Controller] Failed to spawn media process");
            return false;
        }
    } else {
        LogInfo("[Controller] Deferring media process startup until recording begins");
    }

    const int64_t auxSpawnStartUs = Log_GetQpcUs();
    if (ShouldStartLoggerProcess(RuntimeConfiguration())) {
        const bool loggerReady = ce::runtime::EnsureHostChild(HostChild::Logger);
        if (!loggerReady) {
            LogError("[Controller] Failed to spawn logger process");
        }
    }
    if (ShouldStartSensorProcess(RuntimeConfiguration())) {
        const bool sensorReady = ce::runtime::EnsureHostChild(HostChild::Sensors);
        if (!sensorReady) {
            LogError("[Controller] Failed to spawn sensor process");
        }
    }
    const int64_t auxSpawnUs = Log_GetQpcUs() - auxSpawnStartUs;
    PumpStartupMessages();

    LogInfo("[Controller] Waiting for child processes to connect...");
    const int64_t ipcConnectStartUs = Log_GetQpcUs();
    if (!ConnectToChildProcesses(10000)) {
        LogError("[Controller] Failed to connect to all child processes");
        return false;
    }
    const int64_t ipcConnectUs = Log_GetQpcUs() - ipcConnectStartUs;
    LogInfo("[Controller] All child processes connected");
    PumpStartupMessages();

    if (!main_g_DeferredLaunchPath.empty()) {
        LogInfo("[Controller] Launching deferred game: %s", main_g_DeferredLaunchPath.c_str());
        LaunchGameSuspended(main_g_DeferredLaunchPath);
    }

    LogInfo("[Controller] Registering hotkeys...");
    const int64_t hotkeyStartUs = Log_GetQpcUs();

    main_g_HotkeyOwnership.record =
        RegisterConfiguredHotkey(HOTKEY_ID_RECORD, RuntimeConfiguration().hotkeyStartStop, "recording");
    main_g_HotkeyOwnership.screenshot =
        RegisterConfiguredHotkey(HOTKEY_ID_SCREENSHOT, RuntimeConfiguration().hotkeyScreenshot, "screenshot");
    main_g_HotkeyOwnership.audioOnly =
        RegisterConfiguredHotkey(HOTKEY_ID_AUDIO_ONLY, RuntimeConfiguration().hotkeyAudioOnly, "audio-only");
    main_g_HotkeyOwnership.toggleOverlay =
        RegisterConfiguredHotkey(HOTKEY_ID_TOGGLE_OVERLAY, RuntimeConfiguration().hotkeyToggleOverlay, "overlay toggle");
    main_g_HotkeyOwnership.benchmark =
        RegisterConfiguredHotkey(HOTKEY_ID_BENCHMARK, RuntimeConfiguration().hotkeyBenchmark, "benchmark");

    // RegisterHotKey stops being delivered to anyone while a foreground
    // application registers its raw-input keyboard with RIDEV_NOHOTKEYS, so the
    // same hotkeys are also recognized on a low-level keyboard hook. This runs
    // on the controller thread, which is the thread RegisterHotKey posts to and
    // therefore the thread the hook has to post to as well.
    PublishHotkeyBindings(RuntimeConfiguration(), main_g_HotkeyOwnership);
    StartHotkeyInputHook({GetCurrentThreadId(), main_kMsgHotkeyFromInputHook});
    const int64_t hotkeyUs = Log_GetQpcUs() - hotkeyStartUs;

    SyncPseudoOverlayConfiguration("startup");
    PumpStartupMessages();

    LogInfo("[Controller] Ready. Press hotkey to start recording.");
    PrimeStartupCursor();
    LogInfo(
        "[StartupPerf] Controller startup: VulkanRegistration=%.3f ms, SpawnInject=%.3f ms, "
        "SpawnMedia=%.3f ms, SpawnAux=%.3f ms, IPCConnect=%.3f ms, TrayCreate=%.3f ms, "
        "RegisterHotkeys=%.3f ms, TotalToReady=%.3f ms",
        QpcDeltaToMs(main_g_ControllerStartupTiming.vulkanRegUs), QpcDeltaToMs(injectSpawnUs), QpcDeltaToMs(mediaSpawnUs),
        QpcDeltaToMs(auxSpawnUs), QpcDeltaToMs(ipcConnectUs),
        QpcDeltaToMs(main_g_ControllerStartupTiming.trayCreateUs), QpcDeltaToMs(hotkeyUs),
        QpcDeltaToMs(Log_GetQpcUs() - main_g_ControllerStartupTiming.controllerStartUs));

    if (main_g_AutoRecordEnabled) {
        LogInfo("[Controller] Auto-record enabled: delay=%lums, duration=%lums", main_g_AutoRecordDelayMs,
                main_g_AutoRecordDurationMs);
        main_g_AutoRecordStartTime = GetTickCount();
    }

    main_g_ControllerStartupTiming.complete = true;
    return true;
}
