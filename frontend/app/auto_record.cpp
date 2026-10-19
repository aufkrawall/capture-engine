// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "frontend/app/auto_record.h"

namespace ce::frontend {

void AutoRecordScript::OnRuntimeState(int32_t state, uint64_t nowMs) {
    if (state == CE_RUNTIME_READY) {
        if (enabled_ && !armed_ && !finished_) {
            armed_ = true;
            readyMs_ = nowMs;
        }
    } else if (state == CE_RUNTIME_STOPPING || state == CE_RUNTIME_STOPPED || state == CE_RUNTIME_FAILED) {
        finished_ = true;
    }
}

bool AutoRecordScript::OnRecordingState(int32_t state, uint32_t failure) {
    ++epoch_;
    requested_ = state == CE_RECORDING_STARTING || state == CE_RECORDING_LIVE;
    if (failure != 0 && Active()) {
        disabled_ = true;
        return true;
    }
    return false;
}

void AutoRecordScript::OnToggleIssued(ce_request_t request) {
    if (offered_ == Toggle::None)
        return;
    inFlight_ = true;
    request_ = request;
    issuedEpoch_ = epoch_;
    if (offered_ == Toggle::Stop)
        stopIssued_ = true;
}

void AutoRecordScript::OnToggleRejected() {
    if (offered_ == Toggle::Stop) {
        stopIssued_ = true;  // nothing to wait for: the old loop left right after the stop attempt
        stopSettled_ = true;
    } else if (offered_ == Toggle::Start) {
        settledAtEpoch_ = epoch_;
    }
    offered_ = Toggle::None;
}

void AutoRecordScript::OnCommandCompleted(ce_request_t request, ce_status_t) {
    if (!inFlight_ || request != request_)
        return;
    inFlight_ = false;
    offered_ = Toggle::None;
    if (stopIssued_)
        stopSettled_ = true;
    else
        settledAtEpoch_ = issuedEpoch_;
}

AutoRecordScript::Step AutoRecordScript::Poll(uint64_t nowMs) {
    if (!Active() || !armed_)
        return Step::None;
    if (stopIssued_) {
        if (!stopSettled_)
            return Step::None;
        finished_ = true;
        return Step::RequestShutdown;
    }
    if (inFlight_ || Blocked())
        return Step::None;
    if (ElapsedMs(nowMs) < DueMs())
        return Step::None;
    offered_ = requested_ ? Toggle::Stop : Toggle::Start;
    return Step::ToggleRecording;
}

uint32_t AutoRecordScript::WaitMs(uint64_t nowMs) const {
    if (!Active() || !armed_)
        return kWaitForever;
    if (stopIssued_)
        return stopSettled_ ? 0 : kWaitForever;
    if (inFlight_ || Blocked())
        return kWaitForever;
    const uint64_t elapsed = ElapsedMs(nowMs);
    const uint64_t due = DueMs();
    if (elapsed >= due)
        return 0;
    const uint64_t remaining = due - elapsed;
    return remaining >= kWaitForever ? kWaitForever - 1 : static_cast<uint32_t>(remaining);
}

}  // namespace ce::frontend
