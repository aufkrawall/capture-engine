#pragma once

#include <cstdint>

enum class RecordingStartIntent : uint8_t;

namespace ce::controller {

enum class CommandOutcome { Accepted, Rejected, AcknowledgementUnknown };
enum class RecordingNotice { Clear, Requested, Finalizing, Failed, Live, StopResult };

struct RecordingSnapshot {
    bool requested = false;
    bool streaming = false;
    bool observedLive = false;
    RecordingStartIntent pendingIntent{};
    uint64_t request = 0;
    uint64_t pendingSince = 0;
    CommandOutcome lastStop = CommandOutcome::Accepted;
};

// Local request identity scopes delayed observations; it is not a new shared-memory epoch.
// Live/failure fields are independently published observations, not an atomic snapshot.
struct RecordingObservation {
    uint64_t request = 0;
    uint64_t now = 0;
    bool live = false;
    int64_t liveSince = 0;
    uint32_t failure = 0;
    bool mediaAvailable = true;
    bool injectAvailable = true;
};

// Implementation/test transport and presentation seam. No renderer/config/codec dependencies.
class RecordingEffects {
public:
    virtual ~RecordingEffects() = default;
    virtual void PrepareIdentity() = 0;
    virtual bool PublishIntent(RecordingStartIntent intent, const char* reason) = 0;
    virtual bool EnsureMedia() = 0;
    virtual void EnsureSensor() = 0;
    virtual bool InjectConnected() const = 0;
    virtual CommandOutcome StartInject() = 0;
    virtual CommandOutcome StartAudioMedia() = 0;
    virtual CommandOutcome StopMedia(const char* reason, uint32_t timeoutMs) = 0;
    virtual CommandOutcome StopInject(const char* reason, uint32_t timeoutMs) = 0;
    virtual void ReleaseMedia() = 0;
    virtual void Notice(RecordingNotice notice, const RecordingSnapshot& snapshot, const char* reason,
                        uint64_t elapsedMs = 0, bool exact = false) = 0;
    virtual void ClearMediaFailure(uint32_t failure, bool mediaGone) = 0;
    virtual void DisableAutomaticRecording() = 0;
};

// All operations run on the controller thread. Media phase/finalization stay media-owned.
class RecordingSession {
public:
    explicit RecordingSession(RecordingEffects& effects) : effects_(effects) {}
    RecordingSession(const RecordingSession&) = delete;
    RecordingSession& operator=(const RecordingSession&) = delete;

    RecordingSnapshot Snapshot() const { return state_; }
    uint32_t NextDiagnosticSerial() { return ++diagnosticSerial_; }
    CommandOutcome Start(RecordingStartIntent intent, bool streaming, uint64_t now, const char* reason);
    CommandOutcome Stop(const char* reason, uint64_t now);
    CommandOutcome Toggle(RecordingStartIntent intent, bool streaming, uint64_t now, const char* reason);
    void Observe(const RecordingObservation& observation);
    void Shutdown(const char* reason);

private:
    void ClearIntent(const char* reason);
    CommandOutcome StopChildEndpoints(const char* reason, uint32_t timeoutMs);
    void Fail(const char* reason, uint32_t failure, bool mediaGone, bool stopChild, bool disableAutomatic);
    RecordingEffects& effects_;
    RecordingSnapshot state_;
    uint32_t diagnosticSerial_ = 0;
    bool commandInFlight_ = false;
};

}  // namespace ce::controller
