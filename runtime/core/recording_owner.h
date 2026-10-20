// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include "runtime/children/host_children.h"
#include "runtime/core/clock.h"
#include "runtime/core/settings_source.h"
#include "runtime/recording/recording_session.h"

namespace ce::engine {

// What the recording owner reports to the engine's presentation and automation (desktop overlay, events).
// One fixed implementer (the engine); calls are synchronous on the engine thread and happen at the exact
// point of the corresponding RecordingEffects step, so the desktop overlay learns the start intent before
// the media child is spawned (its status-dark handshake depends on that order; DR-20).
class RecordingObserver {
public:
    virtual ~RecordingObserver() = default;
    // The intent was published to the children (Idle on clear), whether or not publication worked.
    virtual void OnStartIntent(RecordingStartIntent intent) = 0;
    // The "recording requested" ownership changed.
    virtual void OnRequestedChanged(bool requested) = 0;
    // A stop arrived while the start was still pending; media is finalizing asynchronously.
    virtual void OnFinalizing() = 0;
    // The session ended on a recording failure; `integrity` = automation must not restart it.
    virtual void OnFailed(bool integrity) = 0;
};

// The recording state machine (RecordingSession, unchanged) wired to the helper children: the
// RecordingEffects implementation that spawns media, publishes the start intent, starts and stops the
// children and publishes in-game notifications. Presentation and automation go to the observer.
// Engine-thread object; the effects stay private so only the session drives them.
class RecordingOwner final : private ce::controller::RecordingEffects {
public:
    RecordingOwner(ce::runtime::HostChildren& children, RecordingObserver& observer, const SettingsSource& settings,
                   const Clock& clock);
    RecordingOwner(const RecordingOwner&) = delete;
    RecordingOwner& operator=(const RecordingOwner&) = delete;

    ce::controller::RecordingSnapshot Snapshot() const { return session_.Snapshot(); }
    // The live-stream target is read from the settings at the moment of the call.
    ce::controller::CommandOutcome Start(RecordingStartIntent intent, const char* reason);
    ce::controller::CommandOutcome Toggle(RecordingStartIntent intent, const char* reason);
    bool Stop(const char* reason);
    // Folds the published live/failure observations into the session. Without `includeChildHealth`
    // only a media transport failure is applied: it must clear ownership before child recovery can
    // mistake the intentional failed stop for a crash.
    void Reconcile(bool includeChildHealth);
    void PrepareDiagnosticIdentity();
    void Shutdown(const char* reason);
#ifdef CE_UNIT_TESTS
    // The effects the session drives, for tests that pin what each step forwards.
    ce::controller::RecordingEffects& EffectsForTesting() { return *this; }
#endif

private:
    void PrepareIdentity() override;
    bool PublishIntent(RecordingStartIntent intent, const char* reason) override;
    bool EnsureMedia() override;
    void EnsureSensor() override;
    bool InjectConnected() const override;
    ce::controller::CommandOutcome StartInject() override;
    ce::controller::CommandOutcome StartAudioMedia() override;
    ce::controller::CommandOutcome StopMedia(const char* reason, uint32_t timeoutMs) override;
    ce::controller::CommandOutcome StopInject(const char* reason, uint32_t timeoutMs) override;
    void ReleaseMedia() override;
    void Notice(ce::controller::RecordingNotice notice, const ce::controller::RecordingSnapshot& snapshot,
                const char* reason, uint64_t elapsedMs, bool exact) override;
    void ClearMediaFailure(uint32_t failure, bool mediaGone) override;
    void DisableAutomaticRecording() override;

    ce::controller::CommandOutcome RequestChildRecordingStop(ce::runtime::HostChild child, const char* childName,
                                                             const char* reason, uint32_t timeoutMs);
    void PublishRecordingFailureOverlayNotification(const char* reason, bool streaming);

    ce::runtime::HostChildren& children_;
    RecordingObserver& observer_;
    const SettingsSource& settings_;
    Clock clock_;
    ce::controller::RecordingSession session_;
};

}  // namespace ce::engine
