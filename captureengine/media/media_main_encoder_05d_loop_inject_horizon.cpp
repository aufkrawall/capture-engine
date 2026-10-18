#include "media_main_internal.h"
#include "media_main_encoder_session.h"

#include <algorithm>

// Pair-avoiding horizon over the inject CFR slot's nearest decision (policy and evidence:
// common/capture/capture_policy/cfr_horizon_playout.h). Nearest selection stays the default and the
// tie-break; the horizon only replaces it when planning the next slots together avoids a repeat or
// a skipped frame within one slot of content timing.
void MediaEncoderSession::applyInjectHorizonPlayout(size_t availableCount, int64_t playoutTargetQpc,
                                                    int64_t leadToleranceQpc, size_t& bestIdx,
                                                    ce::capture_policy::CfrNearestPlayoutDecision& decision) {
    using namespace ce::capture_policy;
    if (playoutTargetQpc <= 0 || targetIntervalTicks <= 0 || bufferedInjectFrames.empty() ||
        (!decision.emit && !decision.hold)) {
        return;
    }

    // Protected-tail frames are not ready for this slot, but their timestamps plan the next ones.
    const size_t count = std::min(bufferedInjectFrames.size(), kCfrHorizonPlayoutMaxCandidates);
    int64_t timestamps[kCfrHorizonPlayoutMaxCandidates];
    bool ready[kCfrHorizonPlayoutMaxCandidates];
    for (size_t i = 0; i < count; ++i) {
        const int64_t timestamp = bufferedInjectFrames[i].timestamp;
        // The plan needs a strictly ordered run of unshown frames; anything else keeps nearest.
        if (!IsInjectFrameFreshAfterLastEmission(timestamp, lastEmittedInjectSourceQpc) ||
            (i > 0 && timestamp <= timestamps[i - 1])) {
            return;
        }
        timestamps[i] = timestamp;
        ready[i] = i < availableCount;
    }

    CfrHorizonPlayoutInput input;
    input.timestampsQpc = timestamps;
    input.readyNow = ready;
    input.count = count;
    input.shownTimestampQpc = lastEmittedInjectSourceQpc;
    input.targetQpc = playoutTargetQpc;
    input.slotQpc = targetIntervalTicks;
    input.leadToleranceQpc = leadToleranceQpc;
    input.lagBoundQpc = targetIntervalTicks;
    const CfrHorizonPlayoutDecision horizon = DecideCfrHorizonPlayout(input, decision.emit, bestIdx);
    if (!horizon.valid || (horizon.emit == decision.emit && (!horizon.emit || horizon.index == bestIdx)))
        return;

    const char* kind = "other";
    if (horizon.emit && !decision.emit) {
        kind = "avoided-repeat";
        ++injectHorizonAvoidedHoldWindow;
        ++injectHorizonAvoidedHoldTotal;
    } else if (horizon.emit && horizon.index < bestIdx) {
        kind = "avoided-skip";
        ++injectHorizonAvoidedSkipWindow;
        ++injectHorizonAvoidedSkipTotal;
    } else {
        ++injectHorizonOtherWindow;
        ++injectHorizonOtherTotal;
    }

    static uint64_t s_horizonLogCount = 0;
    ++s_horizonLogCount;
    if (s_horizonLogCount <= 8 || (s_horizonLogCount % 600) == 0) {
        const int64_t shownQpc = horizon.emit ? timestamps[horizon.index] : lastEmittedInjectSourceQpc;
        LogInfo(
            "[EncoderThread] Horizon playout %s: nearest=%s idx=%zu horizon=%s idx=%zu offsetUs=%lld "
            "buffered=%zu ready=%zu costUs=%lld count=%llu",
            kind, decision.emit ? "show" : "repeat", bestIdx, horizon.emit ? "show" : "repeat", horizon.index,
            static_cast<long long>(qpcToUs(shownQpc - playoutTargetQpc)), bufferedInjectFrames.size(),
            availableCount, static_cast<long long>(qpcToUs(horizon.costQpc)),
            static_cast<unsigned long long>(s_horizonLogCount));
    }

    decision.emit = horizon.emit;
    decision.hold = !horizon.emit;
    if (horizon.emit)
        bestIdx = horizon.index;

    const uint64_t nowTick = GetTickCount64();
    if (injectHorizonWindowStartTick == 0)
        injectHorizonWindowStartTick = nowTick;
    if (nowTick - injectHorizonWindowStartTick >= 5000) {
        LogInfo("[Inject CFR] Horizon playout over %llu ms: avoidedRepeats=%u avoidedSkips=%u other=%u "
                "(totals %llu/%llu/%llu)",
                static_cast<unsigned long long>(nowTick - injectHorizonWindowStartTick),
                injectHorizonAvoidedHoldWindow, injectHorizonAvoidedSkipWindow, injectHorizonOtherWindow,
                static_cast<unsigned long long>(injectHorizonAvoidedHoldTotal),
                static_cast<unsigned long long>(injectHorizonAvoidedSkipTotal),
                static_cast<unsigned long long>(injectHorizonOtherTotal));
        injectHorizonAvoidedHoldWindow = 0;
        injectHorizonAvoidedSkipWindow = 0;
        injectHorizonOtherWindow = 0;
        injectHorizonWindowStartTick = nowTick;
    }
}
