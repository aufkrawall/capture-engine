// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include <cstddef>
#include <cstdint>
#include "common/logging/log_meter.h"

namespace ce::audio {

struct CfrAppAudioBacklogDrainDecision;

struct AppAudioDrainLogContext {
    size_t source = 0;
    int track = 0;
    int sampleRate = 48000;
    bool forceDrain = false;
    bool startupSettled = false;
    bool startupProtected = false;
};

// One pull-owner instance per route. Groups routine active/slack oscillations, preserving exact observed
// transitions, wall time and ranges. It never changes a drain decision or accesses audio buffers/resamplers.
class AppAudioDrainDiagnostics {
public:
    using Sink = void (*)(const char*, void*);
    enum class Boundary { Epoch, Stop };
    explicit AppAudioDrainDiagnostics(Sink sink = nullptr, void* sinkContext = nullptr);
    void Observe(const CfrAppAudioBacklogDrainDecision& decision, AppAudioDrainLogContext context, uint64_t nowMs);
    // Flushes once per observed segment, even when its last samples were quieter than the heartbeat.
    void Flush(Boundary boundary, uint64_t nowMs);
    void Reset();

private:
    struct Stats {
        void Include(int64_t delay, int64_t target, int64_t excess, int32_t delta);
        uint64_t observations = 0;
        uint64_t transitions = 0;
        uint64_t activeMs = 0;
        uint64_t inactiveMs = 0;
        int64_t delayMin = 0, delayMax = 0;
        int64_t targetMin = 0, targetMax = 0;
        int64_t excessMin = 0, excessMax = 0;
        uint64_t peakDelta = 0;
        bool hasValues = false;
    };
    void AccumulateTime(uint64_t nowMs);
    void Emit(const char* kind, const char* boundary, uint64_t nowMs, uint64_t suppressed);

    Sink sink_;
    void* sinkContext_;
    ce::log_meter::ChangeGate gate_{5000};
    Stats window_, total_;
    AppAudioDrainLogContext context_;
    bool observed_ = false;
    bool sawActive_ = false;
    bool lastActive_ = false;
    uint32_t lastReason_ = 0;
    uint64_t lastCategory_ = UINT64_MAX;
    uint64_t lastObservationMs_ = 0;
    uint64_t windowStartMs_ = 0;
    int64_t lastDelay_ = 0, lastTarget_ = 0, lastExcess_ = 0;
    int32_t lastDelta_ = 0;
};

}  // namespace ce::audio
