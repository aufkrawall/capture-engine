#pragma once

#include "fg_runtime_state.h"

namespace ce::overlay_metrics {

enum class FGMetricType {
    kNone = 0,
    kDLSSFG = 1,
    kFSRFG = 2,
    kNvidiaSmoothMotion = 3,
};

inline FGMetricType ResolveFGMetricType(bool effectiveFGActive, fg_runtime::RuntimeMode effectiveRuntimeMode) {
    if (!effectiveFGActive) {
        return FGMetricType::kNone;
    }

    switch (effectiveRuntimeMode) {
        case fg_runtime::RuntimeMode::kDLSSFG:
            return FGMetricType::kDLSSFG;
        case fg_runtime::RuntimeMode::kFSRFG:
            return FGMetricType::kFSRFG;
        case fg_runtime::RuntimeMode::kNvidiaSmoothMotion:
            return FGMetricType::kNvidiaSmoothMotion;
        case fg_runtime::RuntimeMode::kOff:
        case fg_runtime::RuntimeMode::kStreamlineNoFG:
        case fg_runtime::RuntimeMode::kUnknown:
        default:
            return FGMetricType::kNone;
    }
}

inline bool DoPublishedFGTypesDiffer(bool lhsFGActive, fg_runtime::RuntimeMode lhsRuntimeMode, bool rhsFGActive,
                                     fg_runtime::RuntimeMode rhsRuntimeMode) {
    return ResolveFGMetricType(lhsFGActive, lhsRuntimeMode) != ResolveFGMetricType(rhsFGActive, rhsRuntimeMode);
}

}  // namespace ce::overlay_metrics
