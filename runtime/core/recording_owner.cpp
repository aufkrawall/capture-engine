// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "runtime/core/recording_owner.h"

#include <windows.h>

#include <cstdio>

#include "common/config/live_stream_config.h"
#include "common/ipc/inject_control_channel.h"
#include "common/ipc/process_ipc.h"
#include "common/ipc/shared_defs.h"
#include "common/logging/logging.h"

namespace ce::engine {
using namespace ce::controller;

RecordingOwner::RecordingOwner(ce::runtime::HostChildren& children, RecordingObserver& observer,
                               const SettingsSource& settings, const Clock& clock)
    : children_(children), observer_(observer), settings_(settings), clock_(clock), session_(*this) {}

CommandOutcome RecordingOwner::Start(RecordingStartIntent intent, const char* reason) {
    return session_.Start(intent, ce::live_stream::IsLiveStreamTarget(settings_.Current().video.outputDir),
                          clock_.nowMs(), reason);
}

CommandOutcome RecordingOwner::Toggle(RecordingStartIntent intent, const char* reason) {
    return session_.Toggle(intent, ce::live_stream::IsLiveStreamTarget(settings_.Current().video.outputDir),
                           clock_.nowMs(), reason);
}

bool RecordingOwner::Stop(const char* reason) {
    return session_.Stop(reason, clock_.nowMs()) == CommandOutcome::Accepted;
}

void RecordingOwner::Reconcile(std::optional<ChildReadiness> children) {
    if (!session_.Snapshot().requested)
        return;
    RecordingObservation observation;
    observation.request = session_.Snapshot().request;
    observation.now = clock_.nowMs();
    ce::ipc::RecordingHealthObservation health;
    children_.ReadRecordingHealth(health);
    observation.failure = health.failure;
    if (children) {
        observation.live = health.live;
        observation.liveSince = health.liveSince;
        observation.mediaAvailable = children->media;
        observation.injectAvailable = children->inject;
    }
    session_.Observe(observation);
}

void RecordingOwner::PrepareDiagnosticIdentity() {
    if (children_.Running(ce::runtime::HostChild::Media) && !g_RecordingId.empty())
        return;
    char recordingId[24]{};
    snprintf(recordingId, sizeof(recordingId), "r%04lu", static_cast<unsigned long>(session_.NextDiagnosticSerial()));
    g_RecordingId = recordingId;
    LogInfo("[Controller] Recording diagnostic identity allocated: %s", recordingId);
}

void RecordingOwner::Shutdown(const char* reason) {
    session_.Shutdown(reason);
}

CommandOutcome RecordingOwner::RequestChildRecordingStop(ce::runtime::HostChild child, const char* childName,
                                                         const char* reason, uint32_t timeoutMs) {
    const auto outcome = children_.StopRecording(child, timeoutMs);
    const char* result = outcome == CommandOutcome::Accepted ? "accepted" :
                         outcome == CommandOutcome::Rejected ? "rejected" : "acknowledgement-unknown";
    LogInfo("[ControllerSession] request=%llu child=%s stop=%s reason=%s",
            static_cast<unsigned long long>(session_.Snapshot().request), childName, result,
            reason ? reason : "unspecified");
    return outcome;
}

void RecordingOwner::PublishRecordingFailureOverlayNotification(const char* reason, bool streaming) {
    children_.PublishNotification(
        streaming ? OverlayNotificationType::StreamingFailed : OverlayNotificationType::RecordingFailed,
        clock_.nowMs() + 7000ULL);
    LogError("[ControllerSession] Recording failure (%s, streaming=%d request=%llu)", reason, streaming ? 1 : 0,
             static_cast<unsigned long long>(session_.Snapshot().request));
}

void RecordingOwner::PrepareIdentity() { PrepareDiagnosticIdentity(); }

bool RecordingOwner::PublishIntent(RecordingStartIntent intent, const char* reason) {
    const bool published = static_cast<bool>(children_.PublishRecordingIntent(intent));
    observer_.OnStartIntent(intent);
    LogInfo("[ControllerSession] request=%llu intent=%u published=%d reason=%s",
            static_cast<unsigned long long>(session_.Snapshot().request),
            static_cast<unsigned>(intent), published ? 1 : 0, reason);
    return published;
}

bool RecordingOwner::EnsureMedia() {
    return children_.Ensure(ce::runtime::HostChild::Media, 10000);
}

void RecordingOwner::EnsureSensor() {
    if (!children_.Ensure(ce::runtime::HostChild::Sensors))
        LogWarn("[Controller] Display-timing sensor unavailable; inject recording will use virtual final-output timing");
}

bool RecordingOwner::InjectConnected() const {
    return children_.Ready(ce::runtime::HostChild::Inject);
}

CommandOutcome RecordingOwner::StartInject() {
    if (!InjectConnected())
        return CommandOutcome::Rejected;
    ProcessResponse response = ProcessResponse::Error;
    if (!children_.Send(ce::runtime::HostChild::Inject, ProcessCommand::StartRecording, nullptr, &response, 5000))
        return CommandOutcome::AcknowledgementUnknown;
    if (response == ProcessResponse::Error)
        return CommandOutcome::Rejected;
    LogInfo("[Controller] Recording start request delivered to inject; waiting for media to go live");
    return CommandOutcome::Accepted;
}

CommandOutcome RecordingOwner::StartAudioMedia() {
    if (!children_.Ready(ce::runtime::HostChild::Media))
        return CommandOutcome::Rejected;
    ProcessResponse response = ProcessResponse::Error;
    if (!children_.Send(ce::runtime::HostChild::Media, ProcessCommand::StartRecording, "audio_only", &response, 5000))
        return CommandOutcome::AcknowledgementUnknown;
    return response == ProcessResponse::Error ? CommandOutcome::Rejected : CommandOutcome::Accepted;
}

CommandOutcome RecordingOwner::StopMedia(const char* reason, uint32_t timeoutMs) {
    return RequestChildRecordingStop(ce::runtime::HostChild::Media, "Media", reason, timeoutMs);
}

CommandOutcome RecordingOwner::StopInject(const char* reason, uint32_t timeoutMs) {
    return RequestChildRecordingStop(ce::runtime::HostChild::Inject, "Inject fallback", reason, timeoutMs);
}

void RecordingOwner::ReleaseMedia() {
    // Media self-exits after asynchronous finalization. A restart needs a
    // fresh authenticated child even while the previous child is finishing.
    // The retired child's exit finalizes the request that is current here.
    children_.RetireMedia(session_.Snapshot().request);
}

void RecordingOwner::Notice(RecordingNotice notice, const RecordingSnapshot& snapshot, const char* reason,
                            uint64_t elapsedMs, bool exact) {
    switch (notice) {
        case RecordingNotice::StopResult:
            LogInfo("[ControllerSession] request=%llu stop=%s finalization=asynchronous reason=%s",
                    static_cast<unsigned long long>(snapshot.request),
                    snapshot.lastStop == CommandOutcome::Accepted ? "accepted" :
                    snapshot.lastStop == CommandOutcome::Rejected ? "rejected" : "acknowledgement-unknown", reason);
            break;
        case RecordingNotice::Clear:
            children_.PublishNotification(OverlayNotificationType::None, 0);
            break;
        case RecordingNotice::Requested:
            observer_.OnRequestedChanged(snapshot.requested);
            break;
        case RecordingNotice::Failed:
            PublishRecordingFailureOverlayNotification(reason, snapshot.streaming);
            observer_.OnFailed(false);
            break;
        case RecordingNotice::Finalizing:
            if (elapsedMs)
                LogWarn("[Controller] Stop requested %llu ms after start, before controller observed recording live; awaiting media finalization",
                        static_cast<unsigned long long>(elapsedMs));
            children_.PublishNotification(OverlayNotificationType::RecordingFinalizing, clock_.nowMs() + 60000ULL);
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

void RecordingOwner::ClearMediaFailure(uint32_t failure, bool mediaGone) {
    children_.ClearMediaFailure(failure, mediaGone);
}

void RecordingOwner::DisableAutomaticRecording() { observer_.OnFailed(true); }

}  // namespace ce::engine
