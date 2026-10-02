#pragma once

#include <atomic>
#include <cstdint>


// When a bridged 2.x call's result is worth a log line. A one-shot latch hid whether a failure
// persisted: session 20261002_043740 logged `slEvaluateFeature returned sl::Result=38` once, and
// whether every later DLSS evaluate up to the game's crash failed too was unknowable. The tracker
// logs the 1st, 2nd, 4th, 8th... failure, every change of the failing result, and the first
// success after a failure run, so a log shows how long the title went without the feature.
namespace ce::streamline_bridge {

enum class ResultLogEvent : uint8_t {
    kNone,
    kFailure,    // failure number `failures` (or a new failing result)
    kRecovered,  // first success after `failures` consecutive failures
};

struct ResultLogDecision {
    ResultLogEvent event = ResultLogEvent::kNone;
    uint32_t failures = 0;  // consecutive failures, including this one for kFailure
    uint32_t total = 0;     // failures over the process lifetime
};

class ResultTracker {
 public:
    // `code` is the sl::Result value; 0 is eOk.
    ResultLogDecision Observe(int code) {
        const int value = code;
        if (code == 0) {
            const uint32_t run = run_.exchange(0, std::memory_order_relaxed);
            last_.store(0, std::memory_order_relaxed);
            if (run == 0) {
                return {};
            }
            return {ResultLogEvent::kRecovered, run, total_.load(std::memory_order_relaxed)};
        }
        const uint32_t run = run_.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint32_t total = total_.fetch_add(1, std::memory_order_relaxed) + 1;
        const int previous = last_.exchange(value, std::memory_order_relaxed);
        const bool changed = run > 1 && previous != value;
        if (changed || (run & (run - 1)) == 0) {
            return {ResultLogEvent::kFailure, run, total};
        }
        return {ResultLogEvent::kNone, run, total};
    }

 private:
    std::atomic<uint32_t> run_{0};
    std::atomic<uint32_t> total_{0};
    std::atomic<int> last_{0};
};

// Names in sl::Result order (sl_result.h, unchanged from 2.11.1 through 2.14.1); streamline_bridge_diag.h
// pins the numbering against the SDK header.
inline const char* ResultCodeName(int code) {
    static constexpr const char* kNames[] = {
        "eOk",
        "eErrorIO",
        "eErrorDriverOutOfDate",
        "eErrorOSOutOfDate",
        "eErrorOSDisabledHWS",
        "eErrorDeviceNotCreated",
        "eErrorNoSupportedAdapterFound",
        "eErrorAdapterNotSupported",
        "eErrorNoPlugins",
        "eErrorVulkanAPI",
        "eErrorDXGIAPI",
        "eErrorD3DAPI",
        "eErrorNRDAPI",
        "eErrorNVAPI",
        "eErrorReflexAPI",
        "eErrorNGXFailed",
        "eErrorJSONParsing",
        "eErrorMissingProxy",
        "eErrorMissingResourceState",
        "eErrorInvalidIntegration",
        "eErrorMissingInputParameter",
        "eErrorNotInitialized",
        "eErrorComputeFailed",
        "eErrorInitNotCalled",
        "eErrorExceptionHandler",
        "eErrorInvalidParameter",
        "eErrorMissingConstants",
        "eErrorDuplicatedConstants",
        "eErrorMissingOrInvalidAPI",
        "eErrorCommonConstantsMissing",
        "eErrorUnsupportedInterface",
        "eErrorFeatureMissing",
        "eErrorFeatureNotSupported",
        "eErrorFeatureMissingHooks",
        "eErrorFeatureFailedToLoad",
        "eErrorFeatureWrongPriority",
        "eErrorFeatureMissingDependency",
        "eErrorFeatureManagerInvalidState",
        "eErrorInvalidState",
        "eWarnOutOfVRAM",
    };
    constexpr int kCount = static_cast<int>(sizeof(kNames) / sizeof(kNames[0]));
    return code >= 0 && code < kCount ? kNames[code] : "unknown";
}

}  // namespace ce::streamline_bridge
