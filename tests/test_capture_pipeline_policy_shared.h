#pragma once

// Shared includes and helpers for the test_capture_pipeline_policy suite, which is split
// across several .cpp files to stay under the AGENTS.md size ceiling.

#include <gtest/gtest.h>

#include <cstdint>
#include <deque>
#include <vector>

#include "../common/capture_pipeline_policy.h"
#include "../common/frame_timing_utils.h"

namespace policy = ce::capture_policy;





















































































namespace {
// Minimal nearest-target playout driver mirroring the media_main integration: per output tick it
// stale-drops already-past frames, then emits the slot frame or holds. Returns realized-delay and
// cadence statistics so tests can assert smoothness + bounded delay without the encoder.
struct PlayoutStats {
    int emits = 0;
    int holds = 0;
    int staleDrops = 0;
    int longestHoldRun = 0;
    int64_t maxRealizedDelay = 0;
    int64_t minRealizedDelay = INT64_MAX;
    int dropDupSameTickViolations = 0;  // a tick must never both stale-drop AND hold (churn signature)
};
PlayoutStats RunNearestPlayout(int ticks, int64_t interval, int64_t contentDelay, int deliveryBatchTicks,
                               int64_t startupFill, bool liveRecoveryLatched = false, bool uniformCadence = true) {
    PlayoutStats s;
    std::deque<int64_t> buffer;  // frame source timestamps, oldest first
    const int64_t leadTol = policy::GetWgcActiveDelayResidualToleranceQpc(interval);
    int64_t lastEmitted = 0;
    int64_t lastDeliveredTs = -interval;  // source timestamp delivered up to (exclusive of next)
    int64_t lastBurstTick = 0;
    int holdRun = 0;
    // Pre-roll: deliver enough startup frames so the first slot is covered.
    for (int64_t i = 0; i < startupFill; ++i) {
        lastDeliveredTs += interval;
        buffer.push_back(lastDeliveredTs);
    }
    for (int tick = 0; tick < ticks; ++tick) {
        const int64_t now = (startupFill + tick) * interval;  // grid time advances one frame/tick
        // Bursty delivery: every deliveryBatchTicks, deliver every source frame produced since the
        // last burst (smooth source, batched delivery). The source presents one frame per interval.
        if (tick - lastBurstTick >= deliveryBatchTicks || tick == 0) {
            while (lastDeliveredTs + interval <= now) {
                lastDeliveredTs += interval;
                buffer.push_back(lastDeliveredTs);
            }
            lastBurstTick = tick;
        }
        // Compute the playout target through the SAME composition the encoder thread uses, so a
        // regression in the live-recovery delay decision (the realized-delay collapse) shows up here
        // as a collapsed realized delay, not only in the isolated helper test. With the delay applied
        // this equals now-contentDelay; if the delay is wrongly suppressed it jumps to ~now (live).
        const int64_t target = policy::GetWgcActiveDelaySelectionTargetQpc(
            /*scheduledSampleQpc=*/now, /*fallbackTargetQpc=*/now - contentDelay,
            /*targetIntervalTicks=*/interval, /*recordingOutputLive=*/true, /*applyLiveDelay=*/true,
            liveRecoveryLatched, uniformCadence, /*contentDelayQpc=*/contentDelay);
        bool staleDroppedThisTick = false;
        // monotonic obsolete-drop (mirrors media_main's pre-pace loop)
        while (!buffer.empty() && buffer.front() <= lastEmitted) {
            buffer.pop_front();
        }
        while (buffer.size() > 1 && policy::ShouldDropWgcFrontForNearerPlayout(buffer[0], buffer[1], target, leadTol)) {
            buffer.pop_front();
            ++s.staleDrops;
            staleDroppedThisTick = true;
        }
        bool held = true;
        if (!buffer.empty()) {
            auto d = policy::DecideWgcNearestPlayout(buffer.front(), target, leadTol, lastEmitted);
            if (d.emit) {
                const int64_t ts = buffer.front();
                buffer.pop_front();
                lastEmitted = ts;
                ++s.emits;
                held = false;
                const int64_t realized = now - ts;
                s.maxRealizedDelay = std::max(s.maxRealizedDelay, realized);
                s.minRealizedDelay = std::min(s.minRealizedDelay, realized);
            }
        }
        if (held) {
            ++s.holds;
            ++holdRun;
            s.longestHoldRun = std::max(s.longestHoldRun, holdRun);
            if (staleDroppedThisTick) {
                ++s.dropDupSameTickViolations;  // never drop fresh content AND repeat in one tick
            }
        } else {
            holdRun = 0;
        }
    }
    return s;
}

PlayoutStats RunNearestPlayoutResample(int ticks, int64_t outputInterval, int64_t sourceInterval, int64_t contentDelay,
                                       int deliveryBatchTicks, int64_t startupFill) {
    PlayoutStats s;
    std::deque<int64_t> buffer;
    const int64_t leadTol = policy::GetWgcActiveDelayResidualToleranceQpc(outputInterval);
    int64_t lastEmitted = 0;
    int64_t lastDeliveredTs = -sourceInterval;
    int64_t lastBurstTick = 0;
    int holdRun = 0;
    for (int64_t i = 0; i < startupFill; ++i) {
        lastDeliveredTs += sourceInterval;
        buffer.push_back(lastDeliveredTs);
    }
    for (int tick = 0; tick < ticks; ++tick) {
        const int64_t now = startupFill * sourceInterval + static_cast<int64_t>(tick) * outputInterval;
        if (tick - lastBurstTick >= deliveryBatchTicks || tick == 0) {
            while (lastDeliveredTs + sourceInterval <= now) {
                lastDeliveredTs += sourceInterval;
                buffer.push_back(lastDeliveredTs);
            }
            lastBurstTick = tick;
        }
        const int64_t target = now - contentDelay;
        bool staleDroppedThisTick = false;
        while (!buffer.empty() && buffer.front() <= lastEmitted) {
            buffer.pop_front();
        }
        while (buffer.size() > 1 && policy::ShouldDropWgcFrontForNearerPlayout(buffer[0], buffer[1], target, leadTol)) {
            buffer.pop_front();
            ++s.staleDrops;
            staleDroppedThisTick = true;
        }
        bool held = true;
        if (!buffer.empty()) {
            auto d = policy::DecideWgcNearestPlayout(buffer.front(), target, leadTol, lastEmitted);
            if (d.emit) {
                const int64_t ts = buffer.front();
                buffer.pop_front();
                lastEmitted = ts;
                ++s.emits;
                held = false;
                const int64_t realized = now - ts;
                s.maxRealizedDelay = std::max(s.maxRealizedDelay, realized);
                s.minRealizedDelay = std::min(s.minRealizedDelay, realized);
            }
        }
        if (held) {
            ++s.holds;
            ++holdRun;
            s.longestHoldRun = std::max(s.longestHoldRun, holdRun);
            if (staleDroppedThisTick) {
                ++s.dropDupSameTickViolations;
            }
        } else {
            holdRun = 0;
        }
    }
    return s;
}

PlayoutStats RunInjectTargetPlayout(int ticks, int64_t outputInterval, int64_t initialSourceInterval,
                                    int64_t contentDelay, bool varySourceRate = false) {
    PlayoutStats s;
    constexpr size_t kFenceTailFrames = 1;
    const int64_t leadTol = policy::GetInjectCfrSelectionLeadToleranceQpc(outputInterval);
    const int64_t timeBase = 100000;
    std::deque<int64_t> buffer;
    int64_t sourceInterval = initialSourceInterval;
    int64_t nextSourceTimestamp = timeBase - contentDelay - 3 * sourceInterval;
    while (nextSourceTimestamp <= timeBase) {
        buffer.push_back(nextSourceTimestamp);
        nextSourceTimestamp += sourceInterval;
    }
    int64_t lastEmitted = 0;
    int holdRun = 0;
    for (int tick = 0; tick < ticks; ++tick) {
        if (varySourceRate) {
            sourceInterval = tick < ticks / 3 ? 133 : (tick < (ticks * 2) / 3 ? 83 : 167);
        }
        const int64_t now = timeBase + static_cast<int64_t>(tick) * outputInterval;
        while (nextSourceTimestamp <= now) {
            buffer.push_back(nextSourceTimestamp);
            nextSourceTimestamp += sourceInterval;
        }
        while (buffer.size() > kFenceTailFrames && buffer.front() <= lastEmitted) {
            buffer.pop_front();
        }
        const size_t available = buffer.size() > kFenceTailFrames ? buffer.size() - kFenceTailFrames : 0;
        const int64_t target = now - contentDelay;
        size_t best = available;
        uint64_t bestDistance = UINT64_MAX;
        for (size_t index = 0; index < available; ++index) {
            const uint64_t distance = policy::GetCfrTimestampDistanceQpc(buffer[index], target);
            if (distance <= bestDistance) {
                bestDistance = distance;
                best = index;
            }
        }
        bool droppedThisTick = false;
        bool held = true;
        if (best < available) {
            const auto decision = policy::DecideCfrNearestPlayout(buffer[best], target, leadTol, lastEmitted);
            if (decision.emit) {
                for (size_t index = 0; index < best; ++index) {
                    buffer.pop_front();
                    ++s.staleDrops;
                    droppedThisTick = true;
                }
                const int64_t timestamp = buffer.front();
                buffer.pop_front();
                lastEmitted = timestamp;
                ++s.emits;
                held = false;
                const int64_t realizedDelay = now - timestamp;
                s.maxRealizedDelay = std::max(s.maxRealizedDelay, realizedDelay);
                s.minRealizedDelay = std::min(s.minRealizedDelay, realizedDelay);
            }
        }
        if (held) {
            ++s.holds;
            ++holdRun;
            s.longestHoldRun = std::max(s.longestHoldRun, holdRun);
            if (droppedThisTick) {
                ++s.dropDupSameTickViolations;
            }
        } else {
            holdRun = 0;
        }
    }
    return s;
}

PlayoutStats RunCaptureSyncPhaseBoundaryPlayout(int ticks, bool enablePhaseLock) {
    PlayoutStats s;
    constexpr int64_t outputInterval = 100;
    constexpr int64_t contentDelay = 400;
    constexpr int64_t timeBase = 100000;
    const int64_t leadTolerance = policy::GetInjectCfrSelectionLeadToleranceQpc(outputInterval);
    policy::CfrCadencePhaseLockState phaseLock;
    std::deque<int64_t> buffer;
    int64_t nextSourceIndex = -10;
    int64_t lastEmitted = 0;
    int holdRun = 0;
    const auto sourceTimestamp = [=](int64_t index) {
        const int64_t phase = (index & 1) == 0 ? 47 : 53;
        return timeBase - contentDelay + index * outputInterval + phase;
    };

    for (int tick = 0; tick < ticks; ++tick) {
        const int64_t now = timeBase + static_cast<int64_t>(tick) * outputInterval;
        while (sourceTimestamp(nextSourceIndex) <= now) {
            const int64_t timestamp = sourceTimestamp(nextSourceIndex++);
            buffer.push_back(timestamp);
            policy::ObserveCfrCaptureSyncSourceTimestamp(phaseLock, timestamp, outputInterval);
        }
        while (!buffer.empty() && buffer.front() <= lastEmitted) {
            buffer.pop_front();
        }

        const int64_t baseTarget = now - contentDelay;
        const int64_t reference = buffer.empty() ? 0 : buffer.back();
        const int64_t target =
            policy::ApplyCfrCaptureSyncPhaseLock(phaseLock, baseTarget, reference, outputInterval, enablePhaseLock);
        size_t best = buffer.size();
        uint64_t bestDistance = UINT64_MAX;
        for (size_t index = 0; index < buffer.size(); ++index) {
            const uint64_t distance = policy::GetCfrTimestampDistanceQpc(buffer[index], target);
            if (distance <= bestDistance) {
                bestDistance = distance;
                best = index;
            }
        }

        bool held = true;
        if (best < buffer.size()) {
            const auto decision = policy::DecideCfrNearestPlayout(buffer[best], target, leadTolerance, lastEmitted);
            if (decision.emit) {
                for (size_t index = 0; index < best; ++index) {
                    buffer.pop_front();
                    ++s.staleDrops;
                }
                lastEmitted = buffer.front();
                buffer.pop_front();
                ++s.emits;
                held = false;
            }
        }
        if (held) {
            ++s.holds;
            ++holdRun;
            s.longestHoldRun = std::max(s.longestHoldRun, holdRun);
        } else {
            holdRun = 0;
        }
    }
    return s;
}

// Encoder-overload grid drift: the CFR encoder grid runs slower than wall-clock (it cannot sustain the
// output rate) so the grid-anchored playout target sits a fixed `gridLag` behind the real-time frame
// timestamps, on top of the content delay. A bounded reservoir keeps only the newest `reservoirFrames`
// frames -- older fresh frames pile up and drop as stale. When gridLag+contentDelay exceeds the reservoir
// span the target falls below the ENTIRE reserve, so without the anti-freeze floor every tick holds and
// the video freezes while fresh frames keep arriving. The reservoir is pre-filled so there is no
// unrealistic negative-time warmup transient. Realized delay is measured against the grid/audio slot
// (gridNow - emittedTs): positive => video is BEHIND the co-timed audio (correct), negative => ahead.
struct GridDriftStats {
    int emits = 0;
    int holds = 0;
    int longestHoldRun = 0;
    int backwardEmits = 0;      // monotonicity violations (must be 0)
    int aheadOfAudioEmits = 0;  // emitted frame newer than the grid/audio slot (must be 0)
    int64_t maxRealizedDelay = 0;
    int64_t minRealizedDelay = INT64_MAX;
};
GridDriftStats RunGridDriftPlayout(int ticks, int64_t interval, int64_t contentDelay, int64_t gridLag,
                                   int reservoirFrames, bool applyAntiFreezeFloor) {
    GridDriftStats s;
    const int64_t leadTol = policy::GetWgcActiveDelayResidualToleranceQpc(interval);
    const int64_t timeBase = 100000;  // keep wall/grid clocks positive throughout
    std::deque<int64_t> buffer;       // source (wall-clock) timestamps, oldest first
    for (int i = reservoirFrames - 1; i >= 0; --i) {
        buffer.push_back(timeBase - static_cast<int64_t>(i) * interval);  // pre-fill a full reserve
    }
    int64_t lastEmitted = timeBase - static_cast<int64_t>(reservoirFrames) * interval;
    int holdRun = 0;
    for (int tick = 1; tick <= ticks; ++tick) {
        const int64_t wallNow = timeBase + static_cast<int64_t>(tick) * interval;  // real time
        const int64_t gridNow = wallNow - gridLag;                                 // grid lags wall-clock
        buffer.push_back(wallNow);                                                 // one fresh frame/tick
        while (static_cast<int>(buffer.size()) > reservoirFrames) {
            buffer.pop_front();  // bounded reservoir: piled-up fresh frames drop as stale
        }
        int64_t target = gridNow - contentDelay;
        if (applyAntiFreezeFloor && !buffer.empty()) {
            target = policy::ApplyWgcUniformPlayoutAntiFreezeFloor(target, buffer.front(), interval);
        }
        while (buffer.size() > 1 && policy::ShouldDropWgcFrontForNearerPlayout(buffer[0], buffer[1], target, leadTol)) {
            buffer.pop_front();
        }
        bool held = true;
        if (!buffer.empty()) {
            auto d = policy::DecideWgcNearestPlayout(buffer.front(), target, leadTol, lastEmitted);
            if (d.emit) {
                const int64_t ts = buffer.front();
                buffer.pop_front();
                if (ts <= lastEmitted) {
                    ++s.backwardEmits;
                }
                lastEmitted = ts;
                ++s.emits;
                held = false;
                const int64_t realized = gridNow - ts;  // >0 => video behind the audio/grid slot
                if (realized < 0) {
                    ++s.aheadOfAudioEmits;
                }
                s.maxRealizedDelay = std::max(s.maxRealizedDelay, realized);
                s.minRealizedDelay = std::min(s.minRealizedDelay, realized);
            }
        }
        if (held) {
            ++s.holds;
            ++holdRun;
            s.longestHoldRun = std::max(s.longestHoldRun, holdRun);
        } else {
            holdRun = 0;
        }
    }
    return s;
}
}  // namespace




























































































