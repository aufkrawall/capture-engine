#include "controller_recording.h"

#include "main_internal.h"
#include "common/config/live_stream_config.h"

namespace {
using namespace ce::controller;
RecordingSession* g_Session = nullptr;

class ControllerEffects final : public RecordingEffects {
public:
    void PrepareIdentity() override { PrepareRecordingDiagnosticIdentity(); }
    bool PublishIntent(RecordingStartIntent intent, const char* reason) override {
        const bool published = WithInjectSharedMem([&](SharedMemoryLayout* memory) {
            memory->runtimeState.SetRecordingStartIntent(intent);
            memory->runtimeState.audioOnly.store(intent == RecordingStartIntent::AudioOnly, std::memory_order_release);
        });
        if (main_g_PseudoOverlay)
            main_g_PseudoOverlay->SetRecordingStartIntent(intent);
        LogInfo("[ControllerSession] request=%llu intent=%u published=%d reason=%s",
                static_cast<unsigned long long>(ControllerRecordingSnapshot().request),
                static_cast<unsigned>(intent), published ? 1 : 0, reason);
        return published;
    }
    bool EnsureMedia() override { return EnsureMediaProcessReady(10000); }
    void EnsureSensor() override {
        if (!EnsureSensorProcessReady())
            LogWarn("[Controller] Display-timing sensor unavailable; inject recording will use virtual final-output timing");
    }
    bool InjectConnected() const override { return main_g_InjectClient && main_g_InjectClient->IsConnected(); }
    CommandOutcome StartInject() override {
        if (!InjectConnected())
            return CommandOutcome::Rejected;
        ProcessResponse response = ProcessResponse::Error;
        if (!main_g_InjectClient->SendCommand(ProcessCommand::StartRecording, nullptr, &response, 5000))
            return CommandOutcome::AcknowledgementUnknown;
        if (response == ProcessResponse::Error)
            return CommandOutcome::Rejected;
        LogInfo("[Controller] Recording start request delivered to inject; waiting for media to go live");
        return CommandOutcome::Accepted;
    }
    CommandOutcome StartAudioMedia() override {
        if (!main_g_MediaClient || !main_g_MediaClient->IsConnected())
            return CommandOutcome::Rejected;
        ProcessResponse response = ProcessResponse::Error;
        if (!main_g_MediaClient->SendCommand(ProcessCommand::StartRecording, "audio_only", &response, 5000))
            return CommandOutcome::AcknowledgementUnknown;
        return response == ProcessResponse::Error ? CommandOutcome::Rejected : CommandOutcome::Accepted;
    }
    CommandOutcome StopChildren(const char* reason, uint32_t timeoutMs) override {
        const bool accepted = RequestRecordingStopAndReleaseMedia(reason, timeoutMs);
        LogInfo("[ControllerSession] request=%llu stop=%s reason=%s",
                static_cast<unsigned long long>(ControllerRecordingSnapshot().request),
                accepted ? "accepted; finalization asynchronous" : "acknowledgement unknown", reason);
        return accepted ? CommandOutcome::Accepted : CommandOutcome::AcknowledgementUnknown;
    }
    void Notice(RecordingNotice notice, const RecordingSnapshot& snapshot, const char* reason,
                uint64_t elapsedMs, bool exact) override {
        switch (notice) {
            case RecordingNotice::Clear:
                WithInjectSharedMem([](SharedMemoryLayout* memory) {
                    memory->runtimeState.notificationExpiry.store(0, std::memory_order_release);
                    memory->runtimeState.notificationType.store(0, std::memory_order_release);
                });
                break;
            case RecordingNotice::Requested:
                if (main_g_Tray)
                    main_g_Tray->SetRecordingState(snapshot.requested);
                break;
            case RecordingNotice::Failed:
                PublishRecordingFailureOverlayNotification(reason, snapshot.streaming);
                break;
            case RecordingNotice::Finalizing:
                if (elapsedMs)
                    LogWarn("[Controller] Stop requested %llu ms after start, before controller observed recording live; awaiting media finalization",
                            static_cast<unsigned long long>(elapsedMs));
                WithInjectSharedMem([](SharedMemoryLayout* memory) {
                    memory->runtimeState.notificationType.store(
                        static_cast<uint32_t>(OverlayNotificationType::RecordingFinalizing), std::memory_order_release);
                    memory->runtimeState.notificationExpiry.store(GetTickCount64() + 60000ULL, std::memory_order_release);
                });
                if (main_g_PseudoOverlay)
                    main_g_PseudoOverlay->ShowRecordingFinalizingNotification();
                break;
            case RecordingNotice::Live:
                LogInfo("[Controller] Recording is live (%s, %llu ms after the start request; liveStamp=%s request=%llu)",
                        snapshot.pendingIntent == RecordingStartIntent::AudioOnly ? "audio-only" : "video",
                        static_cast<unsigned long long>(elapsedMs), exact ? "media" : "unavailable",
                        static_cast<unsigned long long>(snapshot.request));
                break;
        }
    }
    void ClearMediaFailure(uint32_t failure, bool mediaGone) override {
        WithInjectSharedMem([&](SharedMemoryLayout* memory) {
            if (failure) {
                memory->runtimeState.recordingFailureCode.compare_exchange_strong(
                    failure, static_cast<uint32_t>(RecordingFailureCode::None), std::memory_order_acq_rel);
            }
            if (mediaGone) {
                memory->runtimeState.SetRecordingStartIntent(RecordingStartIntent::Idle);
                memory->runtimeState.captureRequested.store(false, std::memory_order_release);
                memory->runtimeState.isRecording.store(false, std::memory_order_release);
                memory->runtimeState.recordingStartTime.store(0, std::memory_order_release);
            }
        });
    }
    void DisableAutomaticRecording() override {
        if (main_g_AutoRecordEnabled) {
            LogError("[Controller] Auto-record disabled after a recording-integrity failure");
            main_g_AutoRecordEnabled = false;
            main_g_AutoRecordStartTime = 0;
        }
    }
};
ControllerEffects g_Effects;
}  // namespace

ControllerRecordingSessionScope::ControllerRecordingSessionScope() : session_(g_Effects) { g_Session = &session_; }
ControllerRecordingSessionScope::~ControllerRecordingSessionScope() { g_Session = nullptr; }

ce::controller::RecordingSnapshot ControllerRecordingSnapshot() {
    return g_Session ? g_Session->Snapshot() : ce::controller::RecordingSnapshot{};
}

void PrepareRecordingDiagnosticIdentity() {
    if (!g_Session || (main_g_hMediaProcess && IsProcessRunning(main_g_hMediaProcess) && !g_RecordingId.empty()))
        return;
    char recordingId[24]{};
    snprintf(recordingId, sizeof(recordingId), "r%04lu", static_cast<unsigned long>(g_Session->NextDiagnosticSerial()));
    g_RecordingId = recordingId;
    LogInfo("[Controller] Recording diagnostic identity allocated: %s", recordingId);
}

ce::controller::CommandOutcome StartControllerRecording(RecordingStartIntent intent, const char* reason) {
    return g_Session ? g_Session->Start(intent, ce::live_stream::IsLiveStreamTarget(main_g_Config.video.outputDir),
                                       GetTickCount64(), reason)
                     : ce::controller::CommandOutcome::Rejected;
}

bool StopControllerRecording(const char* reason) {
    return g_Session && g_Session->Stop(reason, GetTickCount64()) == ce::controller::CommandOutcome::Accepted;
}

void ReconcileControllerRecording(bool includeChildHealth) {
    if (!g_Session || !g_Session->Snapshot().requested)
        return;
    ce::controller::RecordingObservation observation;
    observation.request = g_Session->Snapshot().request;
    observation.now = GetTickCount64();
    WithInjectSharedMem([&](SharedMemoryLayout* memory) {
        observation.failure = memory->runtimeState.recordingFailureCode.load(std::memory_order_acquire);
        if (includeChildHealth) {
            observation.live = memory->runtimeState.isRecording.load(std::memory_order_acquire);
            observation.liveSince = memory->runtimeState.recordingStartTime.load(std::memory_order_acquire);
        }
    });
    if (includeChildHealth) {
        observation.mediaAvailable = main_g_hMediaProcess && IsProcessRunning(main_g_hMediaProcess) &&
                                     main_g_MediaClient && main_g_MediaClient->IsConnected();
        observation.injectAvailable = main_g_hInjectProcess && IsProcessRunning(main_g_hInjectProcess) &&
                                      main_g_InjectClient && main_g_InjectClient->IsConnected();
    }
    g_Session->Observe(observation);
}

void ShutdownControllerRecording() {
    if (g_Session)
        g_Session->Shutdown("controller shutdown");
}
