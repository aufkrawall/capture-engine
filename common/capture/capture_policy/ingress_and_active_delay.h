#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>

#include "smoothness_budget.h"

// WGC ingress admission and active-delay residual/candidate scoring.

namespace ce::capture_policy {

struct WgcIngressAdmissionDecision {
    bool accept = true;
    bool decimated = false;
    bool softReservePressure = false;
    bool hardReservePressure = false;
    const char* reason = "uncapped";
};

inline bool IsWgcIngressSourceBelowCfrTarget(uint32_t outputFps, uint32_t recentInputMin250Fps,
                                             uint32_t recentInputMin500Fps,
                                             uint32_t fpsMargin = kWgcRecoverySourceMarginFps) {
    if (outputFps == 0 || recentInputMin250Fps == 0 || recentInputMin500Fps == 0) {
        return false;
    }

    return recentInputMin250Fps + fpsMargin < outputFps && recentInputMin500Fps + fpsMargin < outputFps;
}

inline bool IsWgcIngressSourceAtOrAboveCfrTarget(uint32_t outputFps, uint32_t recentInputMin250Fps,
                                                 uint32_t recentInputMin500Fps) {
    if (outputFps == 0) {
        return false;
    }

    const bool has250 = recentInputMin250Fps > 0;
    const bool has500 = recentInputMin500Fps > 0;
    if (!has250 && !has500) {
        return false;
    }
    if (has250 && recentInputMin250Fps < outputFps) {
        return false;
    }
    if (has500 && recentInputMin500Fps < outputFps) {
        return false;
    }
    return true;
}

inline WgcIngressAdmissionDecision DecideWgcIngressAdmission(
    uint32_t retainedFrames, uint32_t retainedFrameCap, uint32_t lowWaterFrames, bool recovering, uint32_t outputFps,
    uint32_t recentInputMin250Fps, uint32_t recentInputMin500Fps, double admissionCreditFrames, uint32_t freeCopySlots,
    uint32_t reservedFreeCopySlots, bool uniformPlayoutOwnsSurplus = false) {
    WgcIngressAdmissionDecision decision{};
    if (retainedFrameCap == 0) {
        decision.reason = "uncapped";
        return decision;
    }
    decision.hardReservePressure = freeCopySlots == 0;
    decision.softReservePressure = reservedFreeCopySlots > 0 && freeCopySlots <= reservedFreeCopySlots;

    if (decision.hardReservePressure) {
        decision.accept = false;
        decision.decimated = true;
        decision.reason = "wgc_ingress_decimated_hard_reserve";
        return decision;
    }
    if (decision.softReservePressure) {
        // These slots cover callback/encoder overlap and the selected-frame
        // handoff. Letting low-water or recovery traffic consume them turns a
        // nominal reserve into a full pool exactly when the consumer is late.
        decision.accept = false;
        decision.decimated = true;
        decision.reason = "wgc_ingress_decimated_soft_reserve";
        return decision;
    }

    if (lowWaterFrames > 0 && retainedFrames <= lowWaterFrames) {
        decision.reason = "low_water";
        return decision;
    }
    if (recovering) {
        decision.reason = "recovery";
        return decision;
    }
    if (IsWgcIngressSourceBelowCfrTarget(outputFps, recentInputMin250Fps, recentInputMin500Fps)) {
        decision.reason = "source_below_cfr_target";
        return decision;
    }

    const bool playoutShouldOwnSurplus =
        uniformPlayoutOwnsSurplus &&
        IsWgcIngressSourceAtOrAboveCfrTarget(outputFps, recentInputMin250Fps, recentInputMin500Fps);

    const bool retainedHigh = retainedFrames >= (retainedFrameCap - 1u);
    if (retainedHigh && admissionCreditFrames < 1.0) {
        if (playoutShouldOwnSurplus) {
            decision.reason = "uniform_playout_credit";
            return decision;
        }
        decision.accept = false;
        decision.decimated = true;
        decision.reason = "wgc_ingress_decimated_credit";
        return decision;
    }

    decision.reason = retainedHigh ? "credit" : "healthy";
    return decision;
}

inline int64_t GetWgcActiveDelayResidualToleranceQpc(int64_t targetIntervalTicks) {
    if (targetIntervalTicks <= 0) {
        return 0;
    }

    return std::max<int64_t>(
        1, (targetIntervalTicks * static_cast<int64_t>(kWgcActiveDelayResidualTolerancePermille)) / 1000);
}

inline int64_t GetWgcActiveDelayResidualHardLimitQpc(int64_t targetIntervalTicks, int64_t qpcTicksPerSecond) {
    const int64_t strictToleranceQpc = GetWgcActiveDelayResidualToleranceQpc(targetIntervalTicks);
    if (qpcTicksPerSecond <= 0) {
        return strictToleranceQpc;
    }

    const int64_t hardLimitQpc =
        (qpcTicksPerSecond * static_cast<int64_t>(kWgcActiveDelayResidualHardLimitUs)) / 1000000;
    return std::max<int64_t>(strictToleranceQpc, hardLimitQpc);
}

inline uint32_t GetWgcActiveDelaySoftLateTargetUs(int64_t targetIntervalTicks, int64_t qpcTicksPerSecond) {
    if (targetIntervalTicks <= 0 || qpcTicksPerSecond <= 0) {
        return kWgcActiveDelaySoftLateTargetMinUs;
    }
    const uint64_t frameUs =
        (static_cast<uint64_t>(targetIntervalTicks) * 1000000ull) / static_cast<uint64_t>(qpcTicksPerSecond);
    const uint32_t softTargetUs = static_cast<uint32_t>((frameUs * 3ull) / 4ull);
    return std::clamp(softTargetUs, kWgcActiveDelaySoftLateTargetMinUs, kWgcActiveDelaySoftLateTargetMaxUs);
}

inline bool IsWgcFrameTooNewForActiveDelaySlot(int64_t frameSelectionQpc, int64_t selectionTargetQpc,
                                               int64_t targetIntervalTicks) {
    if (frameSelectionQpc <= 0 || selectionTargetQpc <= 0 || targetIntervalTicks <= 0) {
        return false;
    }

    return frameSelectionQpc > selectionTargetQpc + GetWgcActiveDelayResidualToleranceQpc(targetIntervalTicks);
}

inline bool IsWgcFrameTooNewForActiveDelayHardLimit(int64_t frameSelectionQpc, int64_t selectionTargetQpc,
                                                    int64_t targetIntervalTicks, int64_t qpcTicksPerSecond) {
    if (frameSelectionQpc <= 0 || selectionTargetQpc <= 0 || targetIntervalTicks <= 0) {
        return false;
    }

    return frameSelectionQpc >
           selectionTargetQpc + GetWgcActiveDelayResidualHardLimitQpc(targetIntervalTicks, qpcTicksPerSecond);
}

inline bool IsWgcActiveDelayFinalSelectionWithinHardLimit(int64_t predictedSelectionQpc, int64_t rawSelectionQpc,
                                                          int64_t selectionTargetQpc, int64_t targetIntervalTicks,
                                                          int64_t qpcTicksPerSecond) {
    if (selectionTargetQpc <= 0 || targetIntervalTicks <= 0) {
        return true;
    }
    if (IsWgcFrameTooNewForActiveDelayHardLimit(predictedSelectionQpc, selectionTargetQpc, targetIntervalTicks,
                                                qpcTicksPerSecond)) {
        return false;
    }
    if (rawSelectionQpc > 0 && IsWgcFrameTooNewForActiveDelayHardLimit(rawSelectionQpc, selectionTargetQpc,
                                                                       targetIntervalTicks, qpcTicksPerSecond)) {
        return false;
    }
    return true;
}

inline uint32_t GetWgcActiveDelayFinalSelectionLateResidualUs(int64_t predictedSelectionQpc, int64_t rawSelectionQpc,
                                                              int64_t selectionTargetQpc, int64_t qpcTicksPerSecond) {
    if (selectionTargetQpc <= 0 || qpcTicksPerSecond <= 0) {
        return 0;
    }
    const int64_t predictedLateQpc =
        predictedSelectionQpc > selectionTargetQpc ? (predictedSelectionQpc - selectionTargetQpc) : 0;
    const int64_t rawLateQpc = rawSelectionQpc > selectionTargetQpc ? (rawSelectionQpc - selectionTargetQpc) : 0;
    const int64_t lateQpc = std::max(predictedLateQpc, rawLateQpc);
    if (lateQpc <= 0) {
        return 0;
    }
    const uint64_t lateUs = (static_cast<uint64_t>(lateQpc) * 1000000ull) / static_cast<uint64_t>(qpcTicksPerSecond);
    return lateUs > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(lateUs);
}

inline bool IsWgcActiveDelayFinalSelectionWithinSoftLateTarget(int64_t predictedSelectionQpc, int64_t rawSelectionQpc,
                                                               int64_t selectionTargetQpc, int64_t targetIntervalTicks,
                                                               int64_t qpcTicksPerSecond, uint32_t softLateTargetUs) {
    if (!IsWgcActiveDelayFinalSelectionWithinHardLimit(predictedSelectionQpc, rawSelectionQpc, selectionTargetQpc,
                                                       targetIntervalTicks, qpcTicksPerSecond)) {
        return false;
    }
    return GetWgcActiveDelayFinalSelectionLateResidualUs(predictedSelectionQpc, rawSelectionQpc, selectionTargetQpc,
                                                         qpcTicksPerSecond) <= softLateTargetUs;
}

inline int64_t GetWgcActiveDelayRepeatClusterPenaltyQpc(uint32_t repeatClusterTicks, int64_t targetIntervalTicks) {
    if (repeatClusterTicks == 0 || targetIntervalTicks <= 0) {
        return 0;
    }

    const uint32_t penaltyPermille =
        std::min<uint32_t>(repeatClusterTicks * kWgcActiveDelayRepeatClusterPenaltyPermille,
                           kWgcActiveDelayRepeatClusterPenaltyMaxPermille);
    return std::max<int64_t>(1, (targetIntervalTicks * static_cast<int64_t>(penaltyPermille)) / 1000);
}

enum class WgcActiveDelayRelaxedDecision : uint8_t {
    kRejectInvalid = 0,
    kRejectNotRelaxed = 1,
    kRejectSyncRisk = 2,
    kRejectResidualHeadroom = 3,
    kRejectRepeatCost = 4,
    kAcceptBetterTarget = 5,
    kAcceptRepeatCluster = 6,
    kAcceptSoftRepeatAvoidance = 7,
};

inline const char* WgcActiveDelayRelaxedDecisionToString(WgcActiveDelayRelaxedDecision decision) {
    switch (decision) {
        case WgcActiveDelayRelaxedDecision::kRejectInvalid:
            return "invalid";
        case WgcActiveDelayRelaxedDecision::kRejectNotRelaxed:
            return "not_relaxed";
        case WgcActiveDelayRelaxedDecision::kRejectSyncRisk:
            return "sync_risk";
        case WgcActiveDelayRelaxedDecision::kRejectResidualHeadroom:
            return "residual_headroom";
        case WgcActiveDelayRelaxedDecision::kRejectRepeatCost:
            return "repeat_cost";
        case WgcActiveDelayRelaxedDecision::kAcceptBetterTarget:
            return "better_target";
        case WgcActiveDelayRelaxedDecision::kAcceptRepeatCluster:
            return "repeat_cluster";
        case WgcActiveDelayRelaxedDecision::kAcceptSoftRepeatAvoidance:
            return "soft_repeat_avoidance";
    }
    return "unknown";
}

struct WgcActiveDelayRelaxedCandidateScore {
    WgcActiveDelayRelaxedDecision decision = WgcActiveDelayRelaxedDecision::kRejectInvalid;
    int64_t candidateDamageQpc = 0;
    int64_t repeatDamageQpc = 0;
    int64_t repeatClusterPenaltyQpc = 0;
    int64_t repeatBudgetQpc = 0;
    uint32_t candidateLateResidualUs = 0;

    bool Accepted() const {
        return decision == WgcActiveDelayRelaxedDecision::kAcceptBetterTarget ||
               decision == WgcActiveDelayRelaxedDecision::kAcceptRepeatCluster ||
               decision == WgcActiveDelayRelaxedDecision::kAcceptSoftRepeatAvoidance;
    }
};

inline bool HasWgcActiveDelayResidualHeadroom(
    uint32_t candidateLateResidualUs, uint32_t residualAvgAbsUs, uint32_t residualP95Us, uint32_t residualLateMaxUs,
    WgcActiveDelayWindowClass windowClass = WgcActiveDelayWindowClass::kSourceLimited,
    uint32_t softLateTargetUs = kWgcActiveDelaySoftLateTargetMaxUs) {
    if (candidateLateResidualUs > kWgcActiveDelayResidualHardLimitUs) {
        return false;
    }
    if (windowClass == WgcActiveDelayWindowClass::kPostStallRecovery && candidateLateResidualUs <= softLateTargetUs) {
        return true;
    }
    if (!IsWgcActiveDelaySourceLimitedClass(windowClass) && candidateLateResidualUs > softLateTargetUs) {
        return false;
    }
    if (residualAvgAbsUs > kWgcActiveDelayResidualMeanTargetUs) {
        return false;
    }
    if (residualP95Us > kWgcActiveDelayResidualP95TargetUs) {
        return false;
    }
    return residualLateMaxUs <= kWgcActiveDelayResidualHardLimitUs;
}

inline WgcActiveDelayRelaxedCandidateScore ScoreWgcActiveDelayRelaxedCandidate(
    int64_t candidateSelectionQpc, int64_t repeatSelectionQpc, int64_t selectionTargetQpc, int64_t targetIntervalTicks,
    int64_t qpcTicksPerSecond, uint32_t repeatClusterTicks = 0, uint32_t residualAvgAbsUs = 0,
    uint32_t residualP95Us = 0, uint32_t residualLateMaxUs = 0,
    WgcActiveDelayWindowClass windowClass = WgcActiveDelayWindowClass::kSourceLimited,
    uint32_t softLateTargetUs = kWgcActiveDelaySoftLateTargetMaxUs) {
    WgcActiveDelayRelaxedCandidateScore result{};
    if (candidateSelectionQpc <= 0 || repeatSelectionQpc <= 0 || selectionTargetQpc <= 0 || targetIntervalTicks <= 0) {
        result.decision = WgcActiveDelayRelaxedDecision::kRejectInvalid;
        return result;
    }
    if (!IsWgcFrameTooNewForActiveDelaySlot(candidateSelectionQpc, selectionTargetQpc, targetIntervalTicks)) {
        result.decision = WgcActiveDelayRelaxedDecision::kRejectNotRelaxed;
        return result;
    }
    if (IsWgcFrameTooNewForActiveDelayHardLimit(candidateSelectionQpc, selectionTargetQpc, targetIntervalTicks,
                                                qpcTicksPerSecond)) {
        result.decision = WgcActiveDelayRelaxedDecision::kRejectSyncRisk;
        return result;
    }

    const int64_t candidateLateQpc =
        candidateSelectionQpc > selectionTargetQpc ? (candidateSelectionQpc - selectionTargetQpc) : 0;
    if (candidateLateQpc > 0 && qpcTicksPerSecond > 0) {
        const uint64_t candidateLateUs =
            (static_cast<uint64_t>(candidateLateQpc) * 1000000ull) / static_cast<uint64_t>(qpcTicksPerSecond);
        result.candidateLateResidualUs =
            candidateLateUs > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(candidateLateUs);
    }
    if (!HasWgcActiveDelayResidualHeadroom(result.candidateLateResidualUs, residualAvgAbsUs, residualP95Us,
                                           residualLateMaxUs, windowClass, softLateTargetUs)) {
        result.decision = WgcActiveDelayRelaxedDecision::kRejectResidualHeadroom;
        return result;
    }

    result.repeatDamageQpc = repeatSelectionQpc >= selectionTargetQpc ? (repeatSelectionQpc - selectionTargetQpc)
                                                                      : (selectionTargetQpc - repeatSelectionQpc);
    result.candidateDamageQpc = candidateSelectionQpc >= selectionTargetQpc
                                    ? (candidateSelectionQpc - selectionTargetQpc)
                                    : (selectionTargetQpc - candidateSelectionQpc);
    result.repeatClusterPenaltyQpc = GetWgcActiveDelayRepeatClusterPenaltyQpc(repeatClusterTicks, targetIntervalTicks);
    result.repeatBudgetQpc = INT64_MAX - result.repeatDamageQpc < result.repeatClusterPenaltyQpc
                                 ? INT64_MAX
                                 : result.repeatDamageQpc + result.repeatClusterPenaltyQpc;

    if (result.candidateDamageQpc < result.repeatDamageQpc) {
        result.decision = WgcActiveDelayRelaxedDecision::kAcceptBetterTarget;
        return result;
    }
    if (!IsWgcActiveDelaySourceLimitedClass(windowClass) && result.candidateLateResidualUs <= softLateTargetUs &&
        qpcTicksPerSecond > 0) {
        const uint64_t softBudgetQpc =
            (static_cast<uint64_t>(softLateTargetUs) * static_cast<uint64_t>(qpcTicksPerSecond)) / 1000000ull;
        const int64_t softRepeatBudgetQpc = INT64_MAX - result.repeatDamageQpc < static_cast<int64_t>(softBudgetQpc)
                                                ? INT64_MAX
                                                : result.repeatDamageQpc + static_cast<int64_t>(softBudgetQpc);
        if (result.candidateDamageQpc <= softRepeatBudgetQpc) {
            result.decision = WgcActiveDelayRelaxedDecision::kAcceptSoftRepeatAvoidance;
            return result;
        }
    }
    if (result.repeatClusterPenaltyQpc > 0 && result.candidateDamageQpc <= result.repeatBudgetQpc) {
        result.decision = WgcActiveDelayRelaxedDecision::kAcceptRepeatCluster;
        return result;
    }

    result.decision = WgcActiveDelayRelaxedDecision::kRejectRepeatCost;
    return result;
}

inline bool IsWgcActiveDelayRelaxedCandidateUseful(
    int64_t candidateSelectionQpc, int64_t repeatSelectionQpc, int64_t selectionTargetQpc, int64_t targetIntervalTicks,
    int64_t qpcTicksPerSecond, uint32_t repeatClusterTicks = 0, uint32_t residualAvgAbsUs = 0,
    uint32_t residualP95Us = 0, uint32_t residualLateMaxUs = 0,
    WgcActiveDelayWindowClass windowClass = WgcActiveDelayWindowClass::kSourceLimited,
    uint32_t softLateTargetUs = kWgcActiveDelaySoftLateTargetMaxUs) {
    return ScoreWgcActiveDelayRelaxedCandidate(
               candidateSelectionQpc, repeatSelectionQpc, selectionTargetQpc, targetIntervalTicks, qpcTicksPerSecond,
               repeatClusterTicks, residualAvgAbsUs, residualP95Us, residualLateMaxUs, windowClass, softLateTargetUs)
        .Accepted();
}

inline WgcActiveDelayRelaxedCandidateScore ScoreWgcActiveDelayRepeatRescueCandidate(
    int64_t candidateSelectionQpc, int64_t candidateRawSelectionQpc, int64_t repeatSelectionQpc,
    int64_t selectionTargetQpc, int64_t targetIntervalTicks, int64_t qpcTicksPerSecond, uint32_t repeatClusterTicks = 0,
    uint32_t residualAvgAbsUs = 0, uint32_t residualP95Us = 0, uint32_t residualLateMaxUs = 0,
    WgcActiveDelayWindowClass windowClass = WgcActiveDelayWindowClass::kSourceLimited,
    uint32_t softLateTargetUs = kWgcActiveDelaySoftLateTargetMaxUs) {
    WgcActiveDelayRelaxedCandidateScore result{};
    if (candidateSelectionQpc <= 0 || repeatSelectionQpc <= 0 || selectionTargetQpc <= 0 || targetIntervalTicks <= 0) {
        result.decision = WgcActiveDelayRelaxedDecision::kRejectInvalid;
        return result;
    }
    if (!IsWgcActiveDelayFinalSelectionWithinHardLimit(candidateSelectionQpc, candidateRawSelectionQpc,
                                                       selectionTargetQpc, targetIntervalTicks, qpcTicksPerSecond)) {
        result.decision = WgcActiveDelayRelaxedDecision::kRejectSyncRisk;
        return result;
    }

    const int64_t predictedLateQpc =
        candidateSelectionQpc > selectionTargetQpc ? (candidateSelectionQpc - selectionTargetQpc) : 0;
    const int64_t rawLateQpc =
        candidateRawSelectionQpc > selectionTargetQpc ? (candidateRawSelectionQpc - selectionTargetQpc) : 0;
    const int64_t lateQpc = std::max(predictedLateQpc, rawLateQpc);
    if (lateQpc > 0 && qpcTicksPerSecond > 0) {
        const uint64_t candidateLateUs =
            (static_cast<uint64_t>(lateQpc) * 1000000ull) / static_cast<uint64_t>(qpcTicksPerSecond);
        result.candidateLateResidualUs =
            candidateLateUs > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(candidateLateUs);
    }
    if (!HasWgcActiveDelayResidualHeadroom(result.candidateLateResidualUs, residualAvgAbsUs, residualP95Us,
                                           residualLateMaxUs, windowClass, softLateTargetUs)) {
        result.decision = WgcActiveDelayRelaxedDecision::kRejectResidualHeadroom;
        return result;
    }

    result.repeatDamageQpc = repeatSelectionQpc >= selectionTargetQpc ? (repeatSelectionQpc - selectionTargetQpc)
                                                                      : (selectionTargetQpc - repeatSelectionQpc);
    result.candidateDamageQpc = candidateSelectionQpc >= selectionTargetQpc
                                    ? (candidateSelectionQpc - selectionTargetQpc)
                                    : (selectionTargetQpc - candidateSelectionQpc);
    result.repeatClusterPenaltyQpc = GetWgcActiveDelayRepeatClusterPenaltyQpc(repeatClusterTicks, targetIntervalTicks);
    result.repeatBudgetQpc = INT64_MAX - result.repeatDamageQpc < result.repeatClusterPenaltyQpc
                                 ? INT64_MAX
                                 : result.repeatDamageQpc + result.repeatClusterPenaltyQpc;

    if (result.candidateDamageQpc < result.repeatDamageQpc) {
        result.decision = WgcActiveDelayRelaxedDecision::kAcceptBetterTarget;
        return result;
    }
    if (!IsWgcActiveDelaySourceLimitedClass(windowClass) && result.candidateLateResidualUs <= softLateTargetUs &&
        qpcTicksPerSecond > 0) {
        const uint64_t softBudgetQpc =
            (static_cast<uint64_t>(softLateTargetUs) * static_cast<uint64_t>(qpcTicksPerSecond)) / 1000000ull;
        const int64_t softRepeatBudgetQpc = INT64_MAX - result.repeatDamageQpc < static_cast<int64_t>(softBudgetQpc)
                                                ? INT64_MAX
                                                : result.repeatDamageQpc + static_cast<int64_t>(softBudgetQpc);
        if (result.candidateDamageQpc <= softRepeatBudgetQpc) {
            result.decision = WgcActiveDelayRelaxedDecision::kAcceptSoftRepeatAvoidance;
            return result;
        }
    }
    if (result.repeatClusterPenaltyQpc > 0 && result.candidateDamageQpc <= result.repeatBudgetQpc) {
        result.decision = WgcActiveDelayRelaxedDecision::kAcceptRepeatCluster;
        return result;
    }

    result.decision = WgcActiveDelayRelaxedDecision::kRejectRepeatCost;
    return result;
}

inline bool IsWgcActiveDelayMixedPolicyPressureFault(
    uint32_t sourceLimitedHolds, uint32_t policyHolds, uint32_t totalHolds,
    uint32_t minPolicyHolds = kWgcActiveDelayPolicyHoldFaultMinCount,
    uint32_t minPolicyPermille = kWgcActiveDelayPolicyHoldFaultPermille) {
    if (policyHolds < minPolicyHolds || totalHolds == 0) {
        return false;
    }
    const uint64_t policyPermille = (static_cast<uint64_t>(policyHolds) * 1000ull) / static_cast<uint64_t>(totalHolds);
    return policyPermille >= minPolicyPermille || policyHolds > sourceLimitedHolds;
}

}  // namespace ce::capture_policy
