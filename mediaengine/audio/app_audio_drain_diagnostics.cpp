// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "app_audio_drain_diagnostics.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "audio_sync_utils.h"
#include "mediaengine/engine/mediaengine.h"

namespace ce::audio {
namespace {
void LogDrainDiagnostic(const char* line, void*) {
    DLL_Log("%s", line);
}
}  // namespace

AppAudioDrainDiagnostics::AppAudioDrainDiagnostics(Sink sink, void* sinkContext)
    : sink_(sink ? sink : LogDrainDiagnostic),
      sinkContext_(sinkContext) {}

void AppAudioDrainDiagnostics::Stats::Include(int64_t delay, int64_t target, int64_t excess, int32_t delta) {
    if (!hasValues) {
        delayMin = delayMax = delay;
        targetMin = targetMax = target;
        excessMin = excessMax = excess;
        hasValues = true;
    } else {
        delayMin = std::min(delayMin, delay);
        delayMax = std::max(delayMax, delay);
        targetMin = std::min(targetMin, target);
        targetMax = std::max(targetMax, target);
        excessMin = std::min(excessMin, excess);
        excessMax = std::max(excessMax, excess);
    }
    peakDelta = std::max(peakDelta, static_cast<uint64_t>(std::llabs(delta)));
}

void AppAudioDrainDiagnostics::Reset() {
    gate_.Reset();
    window_ = {};
    total_ = {};
    observed_ = false;
    sawActive_ = false;
    lastCategory_ = UINT64_MAX;
}

void AppAudioDrainDiagnostics::AccumulateTime(uint64_t nowMs) {
    if (!observed_)
        return;
    const uint64_t elapsed = nowMs >= lastObservationMs_ ? nowMs - lastObservationMs_ : 0;
    (lastActive_ ? window_.activeMs : window_.inactiveMs) += elapsed;
    (lastActive_ ? total_.activeMs : total_.inactiveMs) += elapsed;
    // The state carried from the previous report still applies during this window's quiet tail.
    if (elapsed > 0)
        window_.Include(lastDelay_, lastTarget_, lastExcess_, lastDelta_);
    lastObservationMs_ = nowMs;
}

void AppAudioDrainDiagnostics::Observe(const CfrAppAudioBacklogDrainDecision& decision, AppAudioDrainLogContext context,
                                       uint64_t nowMs) {
    AccumulateTime(nowMs);
    const auto reason = static_cast<uint32_t>(decision.reason);
    const bool transition = observed_ && (lastActive_ != decision.active || lastReason_ != reason);
    for (Stats* stats : {&window_, &total_}) {
        ++stats->observations;
        stats->Include(decision.backlogSamples, decision.targetLeadSamples, decision.excessSamples,
                       decision.compensationDelta);
        stats->transitions += transition ? 1 : 0;
    }
    if (!observed_) {
        windowStartMs_ = nowMs;
        lastObservationMs_ = nowMs;
    }
    observed_ = true;
    context_ = context;
    lastActive_ = decision.active;
    lastReason_ = reason;
    lastDelay_ = decision.backlogSamples;
    lastTarget_ = decision.targetLeadSamples;
    lastExcess_ = decision.excessSamples;
    lastDelta_ = decision.compensationDelta;
    const bool firstActive = decision.active && !sawActive_;
    sawActive_ = sawActive_ || decision.active;
    const bool routine = decision.reason == CfrAppAudioBacklogDrainReason::Active ||
                         decision.reason == CfrAppAudioBacklogDrainReason::WithinSlack;
    // Active/slack changes are the normal controller cycle, not distinct diagnostic events.
    const uint64_t category =
        ce::log_meter::FieldKey(routine, routine ? 0u : reason, routine ? false : decision.active, context.forceDrain,
                                context.startupSettled, context.startupProtected);
    const bool categoryChanged = category != lastCategory_;
    lastCategory_ = category;
    const auto verdict = firstActive ? gate_.Force(category, nowMs) : gate_.Observe(category, nowMs);
    if (verdict)
        Emit(categoryChanged || firstActive ? "state" : "summary", "live", nowMs, verdict.suppressed);
}

void AppAudioDrainDiagnostics::Flush(Boundary boundary, uint64_t nowMs) {
    if (!observed_)
        return;
    AccumulateTime(nowMs);
    const auto verdict = gate_.Force(lastCategory_, nowMs);
    Emit("summary", boundary == Boundary::Epoch ? "epoch" : "stop", nowMs, verdict.suppressed);
    observed_ = false;
    lastCategory_ = UINT64_MAX;
    gate_.Reset();
}

void AppAudioDrainDiagnostics::Emit(const char* kind, const char* boundary, uint64_t nowMs, uint64_t suppressed) {
    const double samplesPerMs = static_cast<double>(std::max(1, context_.sampleRate)) / 1000.0;
    const double compensationWindow = static_cast<double>(std::max(1, context_.sampleRate)) * 10.0;
    const auto milliseconds = [samplesPerMs](int64_t samples) {
        return static_cast<double>(samples) / samplesPerMs;
    };
    const auto compensationPercent = [compensationWindow](uint64_t samples) {
        return static_cast<double>(samples) * 100.0 / compensationWindow;
    };
    const uint64_t spanMs = nowMs >= windowStartMs_ ? nowMs - windowStartMs_ : 0;
    const int64_t delayMin = window_.hasValues ? window_.delayMin : lastDelay_;
    const int64_t delayMax = window_.hasValues ? window_.delayMax : lastDelay_;
    const int64_t targetMin = window_.hasValues ? window_.targetMin : lastTarget_;
    const int64_t targetMax = window_.hasValues ? window_.targetMax : lastTarget_;
    const int64_t excessMin = window_.hasValues ? window_.excessMin : lastExcess_;
    const int64_t excessMax = window_.hasValues ? window_.excessMax : lastExcess_;
    const uint64_t peakDelta = window_.hasValues ? window_.peakDelta : static_cast<uint64_t>(std::llabs(lastDelta_));
    char line[1536]{};
    std::snprintf(
        line, sizeof(line),
        "[AppDrain] %s src=%zu track=%d active=%d reason=%s delayMs=%lld targetMs=%lld excessMs=%lld "
        "rb=%lld target=%lld delta=%d comp=%.4f%% forceDrain=%d startupSettled=%d startupProtected=%d "
        "boundary=%s windowMs=%llu observations=%llu transitions=%llu activeMs=%llu inactiveMs=%llu "
        "delayRangeMs=%.3f..%.3f targetRangeMs=%.3f..%.3f excessRangeMs=%.3f..%.3f peakComp=%.4f%% "
        "totalObservations=%llu totalTransitions=%llu totalActiveMs=%llu totalInactiveMs=%llu "
        "totalDelayRangeMs=%.3f..%.3f totalTargetRangeMs=%.3f..%.3f totalExcessRangeMs=%.3f..%.3f "
        "totalPeakComp=%.4f%% suppressed=%llu",
        kind, context_.source, context_.track, lastActive_ ? 1 : 0,
        CfrAppAudioBacklogDrainReasonName(static_cast<CfrAppAudioBacklogDrainReason>(lastReason_)),
        static_cast<long long>(milliseconds(lastDelay_)), static_cast<long long>(milliseconds(lastTarget_)),
        static_cast<long long>(milliseconds(lastExcess_)), static_cast<long long>(lastDelay_),
        static_cast<long long>(lastTarget_), lastDelta_, lastDelta_ * 100.0 / compensationWindow,
        context_.forceDrain ? 1 : 0, context_.startupSettled ? 1 : 0, context_.startupProtected ? 1 : 0, boundary,
        static_cast<unsigned long long>(spanMs), static_cast<unsigned long long>(window_.observations),
        static_cast<unsigned long long>(window_.transitions), static_cast<unsigned long long>(window_.activeMs),
        static_cast<unsigned long long>(window_.inactiveMs), milliseconds(delayMin), milliseconds(delayMax),
        milliseconds(targetMin), milliseconds(targetMax), milliseconds(excessMin), milliseconds(excessMax),
        compensationPercent(peakDelta), static_cast<unsigned long long>(total_.observations),
        static_cast<unsigned long long>(total_.transitions), static_cast<unsigned long long>(total_.activeMs),
        static_cast<unsigned long long>(total_.inactiveMs), milliseconds(total_.delayMin),
        milliseconds(total_.delayMax), milliseconds(total_.targetMin), milliseconds(total_.targetMax),
        milliseconds(total_.excessMin), milliseconds(total_.excessMax), compensationPercent(total_.peakDelta),
        static_cast<unsigned long long>(suppressed));
    sink_(line, sinkContext_);
    window_ = {};
    windowStartMs_ = nowMs;
}

}  // namespace ce::audio
