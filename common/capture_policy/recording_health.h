#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

#include "constants.h"

// Recording-level health attribution. This policy observes the immutable CFR
// pipeline; it deliberately has no output that can influence capture behavior.

namespace ce::capture_policy {

struct RecordingHealthObservation {
    bool videoLive = false;
    bool cfrEnabled = false;
    bool encoderPressure = false;
    bool muxPressure = false;
    uint32_t timelineDebtMs = 0;
};

struct RecordingHealthState {
    uint32_t flags = 0;
    uint32_t currentDebtMs = 0;
    uint32_t peakDebtMs = 0;
    uint32_t capacityAttributedDebtMs = 0;
    uint32_t pressureEpisodeBaselineDebtMs = 0;
    uint32_t pressureEpisodePeakGrowthMs = 0;
    uint32_t consecutiveEncoderPressureSamples = 0;
    uint32_t consecutiveMuxPressureSamples = 0;
    bool pressureEpisodeActive = false;
};

inline bool HasRecordingHealthFlag(uint32_t flags, uint32_t flag) {
    return (flags & flag) != 0;
}

inline bool HasRecordingCapacityCause(uint32_t flags) {
    return (flags & kRecordingHealthCauseMask) != 0;
}

inline uint32_t SaturatingRecordingHealthIncrement(uint32_t value) {
    return value < std::numeric_limits<uint32_t>::max() ? value + 1u : value;
}

inline bool IsRecordingCapacityDebtDominant(const RecordingHealthState& state) {
    return state.capacityAttributedDebtMs >= kRecordingHealthDegradedDebtMs &&
           (state.peakDebtMs == 0 ||
            static_cast<uint64_t>(state.capacityAttributedDebtMs) * 4u >=
                static_cast<uint64_t>(state.peakDebtMs) * 3u);
}

inline const char* GetRecordingHealthStatus(uint32_t flags) {
    if (HasRecordingHealthFlag(flags, kRecordingHealthFlagVideoDegraded)) {
        return "degraded";
    }
    if (HasRecordingHealthFlag(flags, kRecordingHealthFlagRecovering)) {
        return "recovering";
    }
    if (HasRecordingCapacityCause(flags)) {
        return "capacity_pressure_observed";
    }
    return "healthy";
}

inline const char* GetRecordingHealthCause(uint32_t flags) {
    const bool encoder = HasRecordingHealthFlag(flags, kRecordingHealthFlagEncoderPressureObserved);
    const bool mux = HasRecordingHealthFlag(flags, kRecordingHealthFlagMuxPressureObserved);
    if (encoder && mux) {
        return "encoder_and_mux";
    }
    if (encoder) {
        return "encoder";
    }
    if (mux) {
        return "mux";
    }
    return "none";
}

inline RecordingHealthState UpdateRecordingHealth(RecordingHealthState state,
                                                  const RecordingHealthObservation& observation) {
    const uint32_t previousDebtMs = state.currentDebtMs;
    state.flags &= kRecordingHealthLatchedMask;
    state.currentDebtMs = 0;

    if (!observation.videoLive || !observation.cfrEnabled) {
        state.consecutiveEncoderPressureSamples = 0;
        state.consecutiveMuxPressureSamples = 0;
        state.pressureEpisodeBaselineDebtMs = 0;
        state.pressureEpisodePeakGrowthMs = 0;
        state.pressureEpisodeActive = false;
        return state;
    }

    state.currentDebtMs = observation.timelineDebtMs;
    state.peakDebtMs = std::max(state.peakDebtMs, observation.timelineDebtMs);

    uint32_t currentCauseFlags = 0;
    if (observation.encoderPressure) {
        currentCauseFlags |= kRecordingHealthFlagEncoderPressureObserved;
    }
    if (observation.muxPressure) {
        currentCauseFlags |= kRecordingHealthFlagMuxPressureObserved;
    }

    if (currentCauseFlags != 0) {
        if (!state.pressureEpisodeActive) {
            state.pressureEpisodeActive = true;
            state.pressureEpisodeBaselineDebtMs = previousDebtMs;
            state.pressureEpisodePeakGrowthMs = 0;
        }
        const uint32_t episodeGrowthMs =
            observation.timelineDebtMs > state.pressureEpisodeBaselineDebtMs
                ? observation.timelineDebtMs - state.pressureEpisodeBaselineDebtMs
                : 0;
        if (episodeGrowthMs > state.pressureEpisodePeakGrowthMs) {
            const uint32_t growthDeltaMs = episodeGrowthMs - state.pressureEpisodePeakGrowthMs;
            state.capacityAttributedDebtMs = static_cast<uint32_t>(std::min<uint64_t>(
                static_cast<uint64_t>(state.capacityAttributedDebtMs) + growthDeltaMs,
                std::numeric_limits<uint32_t>::max()));
            state.pressureEpisodePeakGrowthMs = episodeGrowthMs;
        }
    } else {
        state.pressureEpisodeBaselineDebtMs = observation.timelineDebtMs;
        state.pressureEpisodePeakGrowthMs = 0;
        state.pressureEpisodeActive = false;
    }

    state.consecutiveEncoderPressureSamples =
        observation.encoderPressure
            ? SaturatingRecordingHealthIncrement(state.consecutiveEncoderPressureSamples)
            : 0;
    state.consecutiveMuxPressureSamples =
        observation.muxPressure
            ? SaturatingRecordingHealthIncrement(state.consecutiveMuxPressureSamples)
            : 0;
    if (observation.timelineDebtMs >= kRecordingHealthCausalDebtMs) {
        state.flags |= currentCauseFlags;
    }
    if (state.consecutiveEncoderPressureSamples >= kRecordingHealthPressureConfirmationSamples) {
        state.flags |= kRecordingHealthFlagEncoderPressureObserved;
    }
    if (state.consecutiveMuxPressureSamples >= kRecordingHealthPressureConfirmationSamples) {
        state.flags |= kRecordingHealthFlagMuxPressureObserved;
    }

    if (observation.timelineDebtMs >= kRecordingHealthCausalDebtMs) {
        state.flags |= kRecordingHealthFlagTimelineDebt;
    }

    if (HasRecordingCapacityCause(state.flags) &&
        state.capacityAttributedDebtMs >= kRecordingHealthDegradedDebtMs) {
        state.flags |= kRecordingHealthFlagVideoDegraded;
    }
    if (HasRecordingCapacityCause(state.flags) &&
        state.capacityAttributedDebtMs >= kRecordingHealthSevereDebtMs) {
        state.flags |= kRecordingHealthFlagSevere;
    }
    if (HasRecordingCapacityCause(state.flags) &&
        observation.timelineDebtMs >= kRecordingHealthCausalDebtMs && currentCauseFlags == 0) {
        state.flags |= kRecordingHealthFlagRecovering;
    }

    return state;
}

}  // namespace ce::capture_policy
