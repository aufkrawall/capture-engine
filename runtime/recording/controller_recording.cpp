#include "controller_recording.h"

#include <windows.h>

#include <cstdio>

#include "runtime/children/host_children.h"
#include "runtime/settings/runtime_configuration.h"
#include "common/config/live_stream_config.h"
#include "common/ipc/inject_control_channel.h"
#include "common/ipc/process_ipc.h"
#include "common/ipc/shared_defs.h"
#include "common/logging/logging.h"

namespace {
using namespace ce::controller;
RecordingSession* g_Session = nullptr;

CommandOutcome RequestChildRecordingStop(ce::runtime::HostChild child, const char* childName, const char* reason,
                                         DWORD timeoutMs) {
    const auto outcome = ce::runtime::StopHostChildRecording(child, timeoutMs);
    const char* result = outcome == CommandOutcome::Accepted ? "accepted" :
                         outcome == CommandOutcome::Rejected ? "rejected" : "acknowledgement-unknown";
    LogInfo("[ControllerSession] request=%llu child=%s stop=%s reason=%s",
            static_cast<unsigned long long>(ControllerRecordingSnapshot().request), childName, result,
            reason ? reason : "unspecified");
    return outcome;
}

void PublishRecordingFailureOverlayNotification(const char* reason, bool streaming) {
    ce::runtime::PublishHostNotification(
        streaming ? OverlayNotificationType::StreamingFailed : OverlayNotificationType::RecordingFailed,
        GetTickCount64() + 7000ULL);
    LogError("[ControllerSession] Recording failure (%s, streaming=%d request=%llu)", reason, streaming ? 1 : 0,
             static_cast<unsigned long long>(ControllerRecordingSnapshot().request));
}
}  // namespace

namespace ce::controller {

void ControllerRecordingEffects::PrepareIdentity() { PrepareRecordingDiagnosticIdentity(); }

bool ControllerRecordingEffects::PublishIntent(RecordingStartIntent intent, const char* reason) {
    const bool published = static_cast<bool>(ce::runtime::PublishHostRecordingIntent(intent));
    observer_.OnStartIntent(intent);
    LogInfo("[ControllerSession] request=%llu intent=%u published=%d reason=%s",
            static_cast<unsigned long long>(ControllerRecordingSnapshot().request),
            static_cast<unsigned>(intent), published ? 1 : 0, reason);
    return published;
}

bool ControllerRecordingEffects::EnsureMedia() {
    return ce::runtime::EnsureHostChild(ce::runtime::HostChild::Media, 10000);
}

void ControllerRecordingEffects::EnsureSensor() {
    if (!ce::runtime::EnsureHostChild(ce::runtime::HostChild::Sensors))
        LogWarn("[Controller] Display-timing sensor unavailable; inject recording will use virtual final-output timing");
}

bool ControllerRecordingEffects::InjectConnected() const {
    return ce::runtime::HostChildReady(ce::runtime::HostChild::Inject);
}

CommandOutcome ControllerRecordingEffects::StartInject() {
    if (!InjectConnected())
        return CommandOutcome::Rejected;
    ProcessResponse response = ProcessResponse::Error;
    if (!ce::runtime::SendHostChildCommand(ce::runtime::HostChild::Inject, ProcessCommand::StartRecording, nullptr, &response, 5000))
        return CommandOutcome::AcknowledgementUnknown;
    if (response == ProcessResponse::Error)
        return CommandOutcome::Rejected;
    LogInfo("[Controller] Recording start request delivered to inject; waiting for media to go live");
    return CommandOutcome::Accepted;
}

CommandOutcome ControllerRecordingEffects::StartAudioMedia() {
    if (!ce::runtime::HostChildReady(ce::runtime::HostChild::Media))
        return CommandOutcome::Rejected;
    ProcessResponse response = ProcessResponse::Error;
    if (!ce::runtime::SendHostChildCommand(ce::runtime::HostChild::Media, ProcessCommand::StartRecording, "audio_only", &response, 5000))
        return CommandOutcome::AcknowledgementUnknown;
    return response == ProcessResponse::Error ? CommandOutcome::Rejected : CommandOutcome::Accepted;
}

CommandOutcome ControllerRecordingEffects::StopMedia(const char* reason, uint32_t timeoutMs) {
    return RequestChildRecordingStop(ce::runtime::HostChild::Media, "Media", reason, timeoutMs);
}

CommandOutcome ControllerRecordingEffects::StopInject(const char* reason, uint32_t timeoutMs) {
    return RequestChildRecordingStop(ce::runtime::HostChild::Inject, "Inject fallback", reason, timeoutMs);
}

void ControllerRecordingEffects::ReleaseMedia() {
    // Media self-exits after asynchronous finalization. A restart needs a
    // fresh authenticated child even while the previous child is finishing.
    ce::runtime::RetireHostMedia();
}

void ControllerRecordingEffects::Notice(RecordingNotice notice, const RecordingSnapshot& snapshot, const char* reason,
                                        uint64_t elapsedMs, bool exact) {
    switch (notice) {
        case RecordingNotice::StopResult:
            LogInfo("[ControllerSession] request=%llu stop=%s finalization=asynchronous reason=%s",
                    static_cast<unsigned long long>(snapshot.request),
                    snapshot.lastStop == CommandOutcome::Accepted ? "accepted" :
                    snapshot.lastStop == CommandOutcome::Rejected ? "rejected" : "acknowledgement-unknown", reason);
            break;
        case RecordingNotice::Clear:
            ce::runtime::PublishHostNotification(OverlayNotificationType::None, 0);
            break;
        case RecordingNotice::Requested:
            observer_.OnRequestedChanged(snapshot.requested);
            break;
        case RecordingNotice::Failed:
            PublishRecordingFailureOverlayNotification(reason, snapshot.streaming);
            break;
        case RecordingNotice::Finalizing:
            if (elapsedMs)
                LogWarn("[Controller] Stop requested %llu ms after start, before controller observed recording live; awaiting media finalization",
                        static_cast<unsigned long long>(elapsedMs));
            ce::runtime::PublishHostNotification(OverlayNotificationType::RecordingFinalizing, GetTickCount64() + 60000ULL);
            observer_.OnFinalizing();
            break;
        case RecordingNotice::Live:
            LogInfo("[Controller] Recording is live (%s, %llu ms after the start request; liveStamp=%s request=%llu)",
                    snapshot.pendingIntent == RecordingStartIntent::AudioOnly ? "audio-only" : "video",
                    static_cast<unsigned long long>(elapsedMs), exact ? "media" : "unavailable",
                    static_cast<unsigned long long>(snapshot.request));
            break;
    }
}

void ControllerRecordingEffects::ClearMediaFailure(uint32_t failure, bool mediaGone) {
    ce::runtime::ClearHostMediaFailure(failure, mediaGone);
}

void ControllerRecordingEffects::DisableAutomaticRecording() { observer_.OnIntegrityFailure(); }

}  // namespace ce::controller

ControllerRecordingSessionScope::ControllerRecordingSessionScope(ce::controller::RecordingObserver& observer)
    : effects_(observer), session_(effects_) {
    g_Session = &session_;
}
ControllerRecordingSessionScope::~ControllerRecordingSessionScope() { g_Session = nullptr; }

ce::controller::RecordingSnapshot ControllerRecordingSnapshot() {
    return g_Session ? g_Session->Snapshot() : ce::controller::RecordingSnapshot{};
}

void PrepareRecordingDiagnosticIdentity() {
    if (!g_Session || (ce::runtime::HostChildRunning(ce::runtime::HostChild::Media) && !g_RecordingId.empty()))
        return;
    char recordingId[24]{};
    snprintf(recordingId, sizeof(recordingId), "r%04lu", static_cast<unsigned long>(g_Session->NextDiagnosticSerial()));
    g_RecordingId = recordingId;
    LogInfo("[Controller] Recording diagnostic identity allocated: %s", recordingId);
}

ce::controller::CommandOutcome StartControllerRecording(RecordingStartIntent intent, const char* reason) {
    return g_Session ? g_Session->Start(intent, ce::live_stream::IsLiveStreamTarget(ce::runtime::RuntimeConfiguration().video.outputDir),
                                       GetTickCount64(), reason)
                     : ce::controller::CommandOutcome::Rejected;
}

ce::controller::CommandOutcome ToggleControllerRecording(RecordingStartIntent intent, const char* reason) {
    return g_Session ? g_Session->Toggle(intent, ce::live_stream::IsLiveStreamTarget(ce::runtime::RuntimeConfiguration().video.outputDir),
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
    ce::ipc::RecordingHealthObservation health;
    ce::runtime::ReadHostRecordingHealth(health);
    observation.failure = health.failure;
    if (includeChildHealth) {
        observation.live = health.live;
        observation.liveSince = health.liveSince;
        observation.mediaAvailable = ce::runtime::HostChildReady(ce::runtime::HostChild::Media);
        observation.injectAvailable = ce::runtime::HostChildReady(ce::runtime::HostChild::Inject);
    }
    g_Session->Observe(observation);
}

void ShutdownControllerRecording() {
    if (g_Session)
        g_Session->Shutdown("controller shutdown");
}
