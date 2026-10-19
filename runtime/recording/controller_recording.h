#pragma once

#include "recording_session.h"

namespace ce::runtime {
class HostChildren;
}

namespace ce::controller {

// What the recording owner reports to the controller's presentation and automation (tray, pseudo
// overlay, auto-record). One fixed implementer; calls are synchronous on the controller thread and
// happen at the exact point of the corresponding RecordingEffects step, so e.g. the overlay learns
// the start intent before the media child is spawned (its status-dark wait depends on that order).
class RecordingObserver {
public:
    virtual ~RecordingObserver() = default;
    // The intent was published to the children (Idle on clear), whether or not publication worked.
    virtual void OnStartIntent(RecordingStartIntent intent) = 0;
    // The controller's "recording requested" ownership changed.
    virtual void OnRequestedChanged(bool requested) = 0;
    // A stop arrived while the start was still pending; media is finalizing asynchronously.
    virtual void OnFinalizing() = 0;
    // The session ended on a recording-integrity failure; automatic recording must not restart.
    virtual void OnIntegrityFailure() = 0;
};

// RecordingEffects over the host children (spawn, IPC, in-game notifications); presentation and
// automation effects are forwarded to the observer.
class ControllerRecordingEffects final : public RecordingEffects {
public:
    ControllerRecordingEffects(RecordingObserver& observer, ce::runtime::HostChildren& children)
        : observer_(observer), children_(children) {}
    void PrepareIdentity() override;
    bool PublishIntent(RecordingStartIntent intent, const char* reason) override;
    bool EnsureMedia() override;
    void EnsureSensor() override;
    bool InjectConnected() const override;
    CommandOutcome StartInject() override;
    CommandOutcome StartAudioMedia() override;
    CommandOutcome StopMedia(const char* reason, uint32_t timeoutMs) override;
    CommandOutcome StopInject(const char* reason, uint32_t timeoutMs) override;
    void ReleaseMedia() override;
    void Notice(RecordingNotice notice, const RecordingSnapshot& snapshot, const char* reason,
                uint64_t elapsedMs = 0, bool exact = false) override;
    void ClearMediaFailure(uint32_t failure, bool mediaGone) override;
    void DisableAutomaticRecording() override;

private:
    RecordingObserver& observer_;
    ce::runtime::HostChildren& children_;
};

}  // namespace ce::controller

ce::controller::RecordingSnapshot ControllerRecordingSnapshot();
ce::controller::CommandOutcome StartControllerRecording(RecordingStartIntent intent, const char* reason);
ce::controller::CommandOutcome ToggleControllerRecording(RecordingStartIntent intent, const char* reason);
bool StopControllerRecording(const char* reason);
void ReconcileControllerRecording(bool includeChildHealth);
void PrepareRecordingDiagnosticIdentity();
void ShutdownControllerRecording();

// The session exists for ControllerMain, including frontend initialization and shutdown.
class ControllerRecordingSessionScope {
public:
    ControllerRecordingSessionScope(ce::controller::RecordingObserver& observer, ce::runtime::HostChildren& children);
    ~ControllerRecordingSessionScope();
    ControllerRecordingSessionScope(const ControllerRecordingSessionScope&) = delete;
    ControllerRecordingSessionScope& operator=(const ControllerRecordingSessionScope&) = delete;

private:
    ce::controller::ControllerRecordingEffects effects_;
    ce::controller::RecordingSession session_;
};
