#pragma once

#include <windows.h>

#include <cstdint>

#include "common/capture/frame_queue.h"
#include "common/capture/inject_source_read_release.h"
#include "mediaengine/engine/mediaengine_abi.h"

namespace ce::media { struct FrameSubmissionResultV1; }

// Completion token of the encoder's GPU reads of one inject source slot: a
// sequence the encoder publishes once its marker query completed
// (MediaEngine_TakeInjectSourceReadToken), of the media pipeline `engine` that issued it. It never touches D3D11: the
// encoder's immediate context is shared with NVENC on the encoder thread, and
// polling it from here removed the device (session 20261016_004939).
class InjectSourceReadToken final {
public:
    using IsCompleteFn = bool (*)(MediaEngineHandle engine, uint64_t sequence);
    using SetWakeEventFn = void (*)(MediaEngineHandle engine, HANDLE event);
    InjectSourceReadToken() = default;
    InjectSourceReadToken(MediaEngineHandle engine, uint64_t sequence, IsCompleteFn isComplete,
                          SetWakeEventFn setWakeEvent) noexcept
        : engine_(engine), sequence_(sequence), isComplete_(isComplete), setWakeEvent_(setWakeEvent) {}

    bool Completed() const { return sequence_ != 0 && isComplete_ && isComplete_(engine_, sequence_); }
    // The encoder signals one wake event per completion advance; registering it
    // again on every token keeps the latest owner's event current.
    bool ArmEvent(HANDLE event) const {
        if (!setWakeEvent_)
            return false;
        setWakeEvent_(engine_, event);
        return true;
    }
    uint64_t Value() const { return sequence_; }

private:
    MediaEngineHandle engine_ = 0;
    uint64_t sequence_ = 0;
    IsCompleteFn isComplete_ = nullptr;
    SetWakeEventFn setWakeEvent_ = nullptr;
};

using InjectSourceReadReleaseQueue = ce::SourceReadReleaseQueue<InjectSourceReadToken>;

// Call right after every inject submission (fresh or catch-up). When the
// encoder still has GPU reads of the frame's slot in flight, the frame's ring
// lease moves into the session's injectSourceReadReleases, leaving the frame
// without a lease, so the existing release calls on it become no-ops. A frame
// retained for retry keeps its lease.
void DeferInjectLeaseUntilSourceRead(QueuedFrame& frame, const ce::media::FrameSubmissionResultV1& submission,
                                     const char* site);

// Releases leases whose reads finished; cheap when nothing is pending.
void ReleaseCompletedInjectSourceReads();

// One summary line per window with activity, plus the recording totals.
void LogInjectSourceReadWindow(int64_t qpcFrequency);
void LogInjectSourceReadSummary(int64_t qpcFrequency);
