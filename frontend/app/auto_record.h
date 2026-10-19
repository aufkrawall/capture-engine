// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include "include/cengine/cengine_draft.h"

#include <cstdint>
#include <optional>

namespace ce::frontend {

// The `--auto-record=DELAY_MS,DURATION_MS` script of the app (llm-wiki/library/05), as a pure state
// machine: events in, "what now" and "wake me at" out. It owns no thread, timer or runtime handle, so the
// frontend loop keeps a single wait set and the tests drive it with a fake clock.
//
// Semantics are today's (`ControllerMain` auto-record block), clocked from READY:
//  * recording is not requested and `delay` has elapsed since READY: toggle recording (start);
//  * recording is requested (STARTING or LIVE) and `delay + duration` has elapsed since READY: toggle it
//    (stop), then, once that command has completed, request shutdown. The script ends there;
//  * a recording that ended by itself (not requested again, no failure) is started again at once, as the
//    old per-iteration check did; a manual stop is treated the same way;
//  * a RECORDING_STATE event with `failure != 0` ends the script without a shutdown (the old
//    DisableAutomaticRecording). The runtime reports a nonzero failure for exactly the failures that
//    disable automatic recording (integrity failures, media lost while live); a readiness failure
//    (failure == 0) stays retryable.
//
// Commands are asynchronous, so the script never offers a second toggle while one is in flight, and
// after an attempt (accepted, completed or rejected) it waits for a RECORDING_STATE event that arrived
// after the attempt before it decides again. That also keeps a rejected toggle from becoming a busy loop.
//
// The loop wires it like this:
//   RUNTIME_STATE    -> OnRuntimeState(state, nowMs)
//   RECORDING_STATE  -> if (OnRecordingState(state, failure)) log "Auto-record disabled after a
//                       recording-integrity failure"
//   COMMAND_COMPLETED-> OnCommandCompleted(request, status)
//   every iteration  -> Poll(nowMs): ToggleRecording = call ce_recording_toggle(VIDEO, ...) and report
//                       OnToggleIssued(requestId) (admitted) or OnToggleRejected() (not admitted);
//                       RequestShutdown = call ce_runtime_request_shutdown
//   wait timeout     -> WaitMs(nowMs) (kWaitForever = events only)
class AutoRecordScript {
public:
    enum class Step { None, ToggleRecording, RequestShutdown };
    static constexpr uint32_t kWaitForever = UINT32_MAX;  // INFINITE

    AutoRecordScript() = default;  // inactive: Poll() is always None, WaitMs() always kWaitForever
    AutoRecordScript(uint32_t delayMs, uint32_t durationMs)
        : enabled_(true),
          delayMs_(delayMs),
          durationMs_(durationMs) {}

    uint32_t delayMs() const {
        return delayMs_;
    }
    uint32_t durationMs() const {
        return durationMs_;
    }
    // Enabled and neither ended by an integrity failure, by shutdown nor by the runtime going away.
    bool Active() const {
        return enabled_ && !disabled_ && !finished_;
    }

    // CE_EVENT_RUNTIME_STATE: READY starts the clock (once); STOPPING / STOPPED / FAILED end the script.
    void OnRuntimeState(int32_t state, uint64_t nowMs);
    // CE_EVENT_RECORDING_STATE. true exactly once: this event made the script give up.
    bool OnRecordingState(int32_t state, uint32_t failure);
    // The toggle Poll() asked for was admitted (ce_recording_toggle returned CE_OK with `request`).
    void OnToggleIssued(ce_request_t request);
    // ... or was refused synchronously.
    void OnToggleRejected();
    // CE_EVENT_COMMAND_COMPLETED for any request; only the in-flight toggle matters.
    void OnCommandCompleted(ce_request_t request, ce_status_t status);

    Step Poll(uint64_t nowMs);
    // Milliseconds until Poll() has a time-based step, 0 when it is due, kWaitForever when only an event
    // can move the script on. Clamped below kWaitForever so a very long delay wakes up and asks again.
    uint32_t WaitMs(uint64_t nowMs) const;

private:
    enum class Toggle { None, Start, Stop };

    bool Blocked() const {
        return settledAtEpoch_ && epoch_ <= *settledAtEpoch_;
    }
    uint64_t DueMs() const {
        return requested_ ? uint64_t{delayMs_} + durationMs_ : uint64_t{delayMs_};
    }
    uint64_t ElapsedMs(uint64_t nowMs) const {
        return nowMs >= readyMs_ ? nowMs - readyMs_ : 0;
    }

    bool enabled_ = false;
    uint32_t delayMs_ = 0;
    uint32_t durationMs_ = 0;
    bool armed_ = false;  // READY seen
    uint64_t readyMs_ = 0;
    bool requested_ = false;  // recording STARTING or LIVE
    bool disabled_ = false;   // ended by a failure
    bool finished_ = false;   // shutdown requested, or the runtime is going away
    uint64_t epoch_ = 0;      // RECORDING_STATE events seen
    Toggle offered_ = Toggle::None;
    bool inFlight_ = false;
    ce_request_t request_ = 0;
    uint64_t issuedEpoch_ = 0;
    std::optional<uint64_t> settledAtEpoch_;  // an attempt settled at this epoch: wait for a newer event
    bool stopIssued_ = false;
    bool stopSettled_ = false;
};

}  // namespace ce::frontend
