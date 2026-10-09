#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Steady-state packet placement drift lane for device-clock sources (system loopback, microphone).
//
// Packet placement re-snaps every source to its QPC timestamps with a 1 ms slop. A device clock that
// runs apart from QPC (typically 20-100 ppm) used to leave through that re-snap: each time the
// accumulated error crossed 1 ms the packet's leading overlap was deleted (or silence inserted) and a
// 1.3 ms fade-in followed, a tiny cut every 40-60 s (session 20260926: -28 ppm loopback, -25 ppm
// microphone). The seam error is now left in place and removed by a first-order lane that bends the
// intake resampler's rate through swr_set_compensation(): at 28 ppm that is ~0.05 cent, inaudible, and
// there is no cut. Only real discontinuities (> kPlacementDriftAbsorbMaxMs) still use the cut/insert
// path, and the lane is capped to the same 0.05% pitch budget as the CFR Tier1 source-clock lane.

namespace ce::audio {

constexpr int64_t kPlacementDriftAbsorbMaxMs = 5;  // larger seams are real discontinuities
constexpr int64_t kPlacementDriftUpdateIntervalMs = 250;
constexpr int64_t kPlacementDriftHorizonSec = 10;        // swr compensation distance (output seconds)
constexpr int32_t kPlacementDriftMaxSlewPerUpdate = 24;  // 10% of the 0.05% budget per update
constexpr int32_t kPlacementDriftGain = 1;               // delta per horizon per seam sample (10 s time constant)
constexpr int kPlacementDriftConfirmPackets = 3;         // over-slop packets of one sign before the lane engages

// Lane state for one source. `appliedDelta` is the last swr_set_compensation() sample_delta over one
// horizon: positive adds output samples (device slower than QPC), negative removes them.
struct PlacementDriftLane {
    bool engaged = false;
    bool disabled = false;  // swr_set_compensation() failed: the legacy cut/insert path owns the seams
    int32_t appliedDelta = 0;
    int pendingSign = 0;
    int pendingPackets = 0;
    uint64_t lastUpdateMs = 0;
    uint64_t lastLogMs = 0;
    int32_t lastLoggedDelta = 0;
    // Diagnostics for the stop summary.
    uint64_t updates = 0;
    uint64_t overSlopPackets = 0;
    uint64_t hardSeams = 0;
    int64_t maxAbsSeamError = 0;
    int64_t weightedDelta = 0;
    uint64_t weightedSamples = 0;
};

struct PlacementDriftPlan {
    bool absorbSeam = false;         // leave the seam alone: no silence insertion, no overlap trim
    bool applyCompensation = false;  // call swr_set_compensation(compensationDelta, horizon)
    bool justEngaged = false;
    int32_t compensationDelta = 0;
};

inline int64_t PlacementDriftHorizonSamples(int64_t sampleRate) {
    return std::max<int64_t>(1, sampleRate) * kPlacementDriftHorizonSec;
}

// 0.05% of the horizon: the Tier1 pitch budget.
inline int32_t PlacementDriftMaxDelta(int64_t sampleRate) {
    return static_cast<int32_t>(PlacementDriftHorizonSamples(sampleRate) / 2000);
}

// Positive: the device delivers fewer samples than QPC time implies (samples are being added).
inline double PlacementDriftDeltaToPpm(double delta, int64_t sampleRate) {
    return delta * 1000000.0 / static_cast<double>(PlacementDriftHorizonSamples(sampleRate));
}

inline double ComputePlacementDriftLanePpm(const PlacementDriftLane& lane, int64_t sampleRate) {
    if (lane.weightedSamples == 0) {
        return 0.0;
    }
    return PlacementDriftDeltaToPpm(static_cast<double>(lane.weightedDelta) / static_cast<double>(lane.weightedSamples),
                                    sampleRate);
}

// `seamErrorSamples` is packetStart - writtenTimeline: negative means the packet starts before the
// samples already written (device fast), positive means a gap (device slow).
inline PlacementDriftPlan PlanPlacementDrift(PlacementDriftLane& lane, bool eligible, bool steadyState,
                                             int64_t seamErrorSamples, int64_t packetSamples, int64_t sampleRate,
                                             uint64_t nowMs) {
    PlacementDriftPlan plan;
    if (!eligible || !steadyState || lane.disabled || sampleRate <= 0) {
        return plan;
    }

    const int64_t slopSamples = sampleRate / 1000;
    const int64_t absorbMaxSamples = (sampleRate * kPlacementDriftAbsorbMaxMs) / 1000;
    const int64_t absError = std::abs(seamErrorSamples);
    lane.maxAbsSeamError = std::max(lane.maxAbsSeamError, absError);

    if (lane.engaged) {
        lane.weightedDelta += static_cast<int64_t>(lane.appliedDelta) * std::max<int64_t>(0, packetSamples);
        lane.weightedSamples += static_cast<uint64_t>(std::max<int64_t>(0, packetSamples));
    }

    // A seam that swallows the whole packet or exceeds the window is a real discontinuity: the legacy
    // path repositions the source onto QPC, so the lane's error input restarts from zero.
    const bool hardSeam = absError > absorbMaxSamples || (seamErrorSamples < 0 && -seamErrorSamples >= packetSamples);
    int64_t controlError = seamErrorSamples;
    if (hardSeam) {
        if (lane.engaged) {
            ++lane.hardSeams;
        }
        lane.pendingSign = 0;
        lane.pendingPackets = 0;
        controlError = 0;
    } else if (!lane.engaged) {
        if (absError <= slopSamples) {
            lane.pendingSign = 0;
            lane.pendingPackets = 0;
            return plan;
        }
        // One jittery timestamp is not drift: wait until the error persists with one sign.
        const int sign = seamErrorSamples > 0 ? 1 : -1;
        lane.pendingPackets = (sign == lane.pendingSign) ? lane.pendingPackets + 1 : 1;
        lane.pendingSign = sign;
        ++lane.overSlopPackets;
        plan.absorbSeam = true;
        if (lane.pendingPackets < kPlacementDriftConfirmPackets) {
            return plan;
        }
        lane.engaged = true;
        plan.justEngaged = true;
    } else {
        plan.absorbSeam = true;
        if (absError > slopSamples) {
            ++lane.overSlopPackets;
        }
    }

    if (!lane.engaged) {
        return plan;
    }
    if (!plan.justEngaged && nowMs - lane.lastUpdateMs < static_cast<uint64_t>(kPlacementDriftUpdateIntervalMs)) {
        return plan;
    }

    const int32_t maxDelta = PlacementDriftMaxDelta(sampleRate);
    const int32_t target = static_cast<int32_t>(
        std::clamp<int64_t>(controlError * kPlacementDriftGain, -static_cast<int64_t>(maxDelta), maxDelta));
    const int32_t step =
        std::clamp(target - lane.appliedDelta, -kPlacementDriftMaxSlewPerUpdate, kPlacementDriftMaxSlewPerUpdate);
    lane.appliedDelta += step;
    lane.lastUpdateMs = nowMs;
    ++lane.updates;
    plan.applyCompensation = true;
    plan.compensationDelta = lane.appliedDelta;
    return plan;
}

// A fresh resampler carries no compensation: restart the update cadence, keep the engaged state.
inline void NotePlacementDriftResamplerReset(PlacementDriftLane& lane) {
    lane.appliedDelta = 0;
    lane.lastUpdateMs = 0;
    lane.pendingSign = 0;
    lane.pendingPackets = 0;
}

// Rate-limited log gate: first engagement, or a >= 6 unit (~12 ppm) change at most every 5 s.
inline bool PlacementDriftLogDue(PlacementDriftLane& lane, bool justEngaged, uint64_t nowMs) {
    const int32_t change = std::abs(lane.appliedDelta - lane.lastLoggedDelta);
    if (!justEngaged && (change < 6 || nowMs - lane.lastLogMs < 5000)) {
        return false;
    }
    lane.lastLogMs = nowMs;
    lane.lastLoggedDelta = lane.appliedDelta;
    return true;
}

}  // namespace ce::audio
