#pragma once

#include <cstddef>
#include <cstdint>

namespace ce::streamline_runtime_policy {

inline constexpr uint32_t kMaxRuntimeReflexRetryAttempts = 6;
inline constexpr uint32_t kMaxInlineHookAttemptsPerTarget = 2;
inline constexpr int kReflexFeatureQueryUnavailableLimit = 3;

inline bool ShouldAttemptInlineHookOnTarget(const void* target, const void* failedTarget, uint32_t failedAttempts) {
    if (!target) {
        return false;
    }
    if (failedTarget == target && failedAttempts >= kMaxInlineHookAttemptsPerTarget) {
        return false;
    }
    return true;
}

inline bool IsReflexFeatureResolutionComplete(bool sleepHooked, bool setOptionsHooked, bool setConstantsHooked,
                                              uint32_t sleepFailedAttempts, uint32_t setOptionsFailedAttempts,
                                              uint32_t setConstantsFailedAttempts, int sleepUnavailableQueries,
                                              int setOptionsUnavailableQueries, int setConstantsUnavailableQueries) {
    const bool sleepDone = sleepHooked || sleepFailedAttempts >= kMaxInlineHookAttemptsPerTarget ||
                           sleepUnavailableQueries >= kReflexFeatureQueryUnavailableLimit;
    const bool setOptionsDone = setOptionsHooked || setOptionsFailedAttempts >= kMaxInlineHookAttemptsPerTarget ||
                                setOptionsUnavailableQueries >= kReflexFeatureQueryUnavailableLimit;
    const bool setConstantsDone = setConstantsHooked || setConstantsFailedAttempts >= kMaxInlineHookAttemptsPerTarget ||
                                  setConstantsUnavailableQueries >= kReflexFeatureQueryUnavailableLimit;
    return sleepDone && setOptionsDone && setConstantsDone;
}

inline bool IsPclFeatureResolutionComplete(bool pclModuleLoaded, bool pclHookReady, uint32_t pclFailedAttempts,
                                           int pclUnavailableQueries) {
    if (!pclModuleLoaded) {
        return true;
    }
    return pclHookReady || pclFailedAttempts >= kMaxInlineHookAttemptsPerTarget ||
           pclUnavailableQueries >= kReflexFeatureQueryUnavailableLimit;
}

inline bool ShouldRetryRuntimeReflexResolution(bool reflexComplete, bool pclComplete, uint32_t retryAttempts,
                                               uint64_t nowMs, uint64_t lastRetryMs,
                                               uint64_t retryIntervalMs = 2500) {
    if (reflexComplete && pclComplete) {
        return false;
    }
    if (retryAttempts >= kMaxRuntimeReflexRetryAttempts) {
        return false;
    }
    if (lastRetryMs != 0 && nowMs >= lastRetryMs && (nowMs - lastRetryMs) < retryIntervalMs) {
        return false;
    }
    return true;
}

}  // namespace ce::streamline_runtime_policy
