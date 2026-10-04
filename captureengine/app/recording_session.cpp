#include "recording_session.h"

#include "common/capture/recording_lifecycle.h"

namespace ce::controller {
namespace {
struct CommandGuard {
    bool& flag;
    bool previous;
    explicit CommandGuard(bool& value) : flag(value), previous(value) { flag = true; }
    ~CommandGuard() { flag = previous; }
};
}  // namespace

void RecordingSession::ClearIntent(const char* reason) {
    state_.requested = false;
    state_.pendingIntent = RecordingStartIntent::Idle;
    state_.pendingSince = 0;
    effects_.PublishIntent(RecordingStartIntent::Idle, reason);
    effects_.Notice(RecordingNotice::Requested, state_, reason);
}

void RecordingSession::Fail(const char* reason, uint32_t failure, bool mediaGone, bool stopChild,
                            bool disableAutomatic) {
    if (stopChild)
        effects_.StopChildren(reason, 1000);
    ClearIntent(reason);
    effects_.Notice(RecordingNotice::Failed, state_, reason);
    effects_.ClearMediaFailure(failure, mediaGone);
    if (disableAutomatic)
        effects_.DisableAutomaticRecording();
}

CommandOutcome RecordingSession::Start(RecordingStartIntent intent, bool streaming, uint64_t now,
                                       const char* reason) {
    if (commandInFlight_ || state_.requested || intent == RecordingStartIntent::Idle)
        return CommandOutcome::Rejected;
    CommandGuard command(commandInFlight_);
    state_.requested = true;
    state_.observedLive = false;
    state_.streaming = streaming && intent == RecordingStartIntent::Video;
    state_.pendingIntent = intent;
    state_.pendingSince = now;
    ++state_.request;
    const uint64_t request = state_.request;
    effects_.PrepareIdentity();
    effects_.Notice(RecordingNotice::Clear, state_, reason);
    const bool published = effects_.PublishIntent(intent, reason);
    effects_.Notice(RecordingNotice::Requested, state_, reason);
    if (intent == RecordingStartIntent::Video)
        effects_.EnsureSensor();
    if (!state_.requested || state_.request != request)
        return CommandOutcome::Rejected;
    const bool ready = effects_.EnsureMedia();
    if (!state_.requested || state_.request != request)
        return CommandOutcome::Rejected;
    if (!ready) {
        Fail(intent == RecordingStartIntent::Video ? "media readiness failure" : "audio-only media readiness failure",
             0, false, false, false);
        return CommandOutcome::Rejected;
    }
    CommandOutcome outcome = CommandOutcome::Rejected;
    if (intent == RecordingStartIntent::AudioOnly) {
        if (!published)
            effects_.PublishIntent(intent, "audio-only shared-state retry");
        if (effects_.InjectConnected())
            outcome = effects_.StartInject();
        if (!state_.requested || state_.request != request)
            return CommandOutcome::Rejected;
        const auto direct = effects_.StartAudioMedia();
        if (direct == CommandOutcome::Accepted || outcome != CommandOutcome::Accepted)
            outcome = direct;
    } else if (effects_.InjectConnected()) {
        outcome = effects_.StartInject();
        // Preserve the existing bounded retry; channel teardown may make it a no-op.
        if (state_.requested && state_.request == request && outcome != CommandOutcome::Accepted)
            outcome = effects_.StartInject();
    }
    if (!state_.requested || state_.request != request)
        return CommandOutcome::Rejected;
    if (outcome != CommandOutcome::Accepted) {
        Fail(intent == RecordingStartIntent::AudioOnly ? "audio-only start command failure"
             : effects_.InjectConnected() ? "inject start command failure" : "inject unavailable",
             0, false, false, false);
    }
    return outcome;
}

CommandOutcome RecordingSession::Stop(const char* reason, uint64_t now) {
    CommandGuard command(commandInFlight_);
    const bool active = state_.requested || state_.pendingIntent != RecordingStartIntent::Idle;
    const uint64_t pendingMs = state_.pendingSince && now >= state_.pendingSince ? now - state_.pendingSince : 0;
    ClearIntent(reason);
    if (!active)
        return CommandOutcome::Accepted;
    effects_.Notice(RecordingNotice::Finalizing, state_, reason, pendingMs);
    state_.lastStop = effects_.StopChildren(reason, 5000);
    return state_.lastStop;
}

CommandOutcome RecordingSession::Toggle(RecordingStartIntent intent, bool streaming, uint64_t now,
                                        const char* reason) {
    return state_.requested ? Stop(reason, now) : Start(intent, streaming, now, reason);
}

void RecordingSession::Observe(const RecordingObservation& observation) {
    if (!state_.requested || observation.request != state_.request)
        return;
    // A preceding session's live bit/stamp can outlive the endpoint; it cannot confirm this start.
    const bool staleLiveStamp = observation.liveSince > 0 && state_.pendingSince != 0 &&
                               static_cast<uint64_t>(observation.liveSince) < state_.pendingSince;
    if (observation.live && !staleLiveStamp && state_.pendingIntent != RecordingStartIntent::Idle) {
        const auto timing = ce::recording_lifecycle::ResolveRecordingStartupTiming(
            state_.pendingSince, observation.liveSince, observation.now);
        state_.observedLive = true;
        effects_.Notice(RecordingNotice::Live, state_, "media live", timing.startupMs, timing.exact);
        state_.pendingSince = 0;
        state_.pendingIntent = RecordingStartIntent::Idle;
    }
    if (observation.failure != 0) {
        Fail("recording failure", observation.failure, false, true, true);
    } else if (state_.pendingIntent != RecordingStartIntent::Idle &&
               (!observation.mediaAvailable ||
                (state_.pendingIntent == RecordingStartIntent::Video && !observation.injectAvailable))) {
        Fail("required child exited before recording live", 0, false, true, false);
    } else if (state_.observedLive && !observation.mediaAvailable) {
        Fail("media process exited while recording live", 0, true, false, true);
    }
}

void RecordingSession::Shutdown(const char* reason) {
    ClearIntent(reason);
}

}  // namespace ce::controller
