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

// Model recovery after the encoder grid has fallen behind every retained source frame. One source
// frame arrives per wall-clock loop. The main slot may select fresh history, while additional bounded
// catch-up slots conservatively repeat the last frame. This is the least-capacity path used while fresh
// recovery is still gated off; it must converge once output capacity returns without moving visual
// content ahead of the immutable grid/audio target.
struct GridDebtCatchupStats {
    int freshEmits = 0;
    int repeatEmits = 0;
    int freshAfterDebt = 0;
    int longestRepeatRun = 0;
    int backwardFreshEmits = 0;
    int64_t maxContentLead = INT64_MIN;
    uint32_t finalShortfallTicks = 0;
};
GridDebtCatchupStats RunGridDebtCatchupPlayout(int wallLoops, int64_t interval, int64_t contentDelay,
                                               uint32_t initialShortfallTicks, int reservoirFrames) {
    GridDebtCatchupStats s;
    const int64_t leadTol = policy::GetWgcActiveDelayResidualToleranceQpc(interval);
    const int64_t timeBase = 100000;  // keep wall/grid clocks positive throughout
    int64_t wallNow = timeBase;
    int64_t outputGridQpc = wallNow - static_cast<int64_t>(initialShortfallTicks) * interval;
    std::deque<int64_t> buffer;
    for (int i = reservoirFrames - 1; i >= 0; --i) {
        buffer.push_back(timeBase - static_cast<int64_t>(i) * interval);  // pre-fill a full reserve
    }
    int64_t lastEmitted = outputGridQpc - contentDelay - interval;
    uint32_t shortfallTicks = initialShortfallTicks;
    int repeatRun = 0;
    bool sawDeepGridDebt = false;
    for (int loop = 0; loop < wallLoops; ++loop) {
        wallNow += interval;
        buffer.push_back(wallNow);
        while (static_cast<int>(buffer.size()) > reservoirFrames) {
            buffer.pop_front();
        }

        ++shortfallTicks;  // one more wall-clock CFR slot became due
        const uint32_t ticksThisLoop = policy::GetWgcCatchupTicksThisLoop(
            /*encoderBottlenecked=*/false, /*encoderActivelyTooSlow=*/false, buffer.size(),
            /*frameCreditAccumulator=*/0.0, shortfallTicks, /*outputFps=*/120,
            /*recentDeliveredMin250Fps=*/120, /*recentInputMin250Fps=*/120,
            /*noFreshTickPermille=*/0, /*lowSourceMode=*/false);
        for (uint32_t outputIndex = 0; outputIndex < ticksThisLoop; ++outputIndex) {
            const int64_t selectionTargetQpc = outputGridQpc - contentDelay;
            bool emittedFresh = false;
            if (outputIndex == 0) {
                while (buffer.size() > 1 &&
                       policy::ShouldDropWgcFrontForNearerPlayout(
                           buffer[0], buffer[1], selectionTargetQpc, leadTol)) {
                    buffer.pop_front();
                }
                if (!buffer.empty()) {
                    sawDeepGridDebt =
                        sawDeepGridDebt ||
                        policy::IsWgcFrameTooNewForCfrSlot(buffer.front(), selectionTargetQpc, interval);
                    const auto decision =
                        policy::DecideWgcNearestPlayout(buffer.front(), selectionTargetQpc, leadTol, lastEmitted);
                    if (decision.emit) {
                        const int64_t selectedTimestamp = buffer.front();
                        buffer.pop_front();
                        if (selectedTimestamp <= lastEmitted) {
                            ++s.backwardFreshEmits;
                        }
                        lastEmitted = selectedTimestamp;
                        emittedFresh = true;
                        ++s.freshEmits;
                        if (sawDeepGridDebt) {
                            ++s.freshAfterDebt;
                        }
                    }
                }
            }

            if (emittedFresh) {
                repeatRun = 0;
            } else {
                ++s.repeatEmits;
                ++repeatRun;
                s.longestRepeatRun = std::max(s.longestRepeatRun, repeatRun);
            }
            s.maxContentLead = std::max(s.maxContentLead, lastEmitted - selectionTargetQpc);
            outputGridQpc += interval;
            --shortfallTicks;
        }
    }
    s.finalShortfallTicks = shortfallTicks;
    return s;
}
}  // namespace


























































































