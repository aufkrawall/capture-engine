#pragma once

#include <cstdint>

#include "common/logging/log_meter.h"
#include "handle_failure_cache.h"

// Per-encoder state that used to be process-wide statics: the shared-handle failure cache, the running
// frame statistics and the log throttles and once-only log flags of the encoder's paths. Each VideoEncoder
// owns one, so two encoders never share a cache, a statistic or a gate.
struct VideoEncoderDiagnostics {
    HandleFailureCache handleFailures;  // Shared-handle open failures (CallOpenShared*)

    // Running totals behind the periodic frame-performance averages (LogFramePerformance).
    struct FrameAnalysisStats {
        int64_t lastFramePts = -1;
        int64_t framesEncoded = 0;
        double totalFenceWait = 0;
        double totalColorConvert = 0;
        double totalEncode = 0;
        double maxFrameTime = 0;
        int slowFrameCount = 0;  // Frames taking > 2x expected time
    };
    FrameAnalysisStats frameAnalysis;

    int keyedMutexAcquireFailLogCount = 0;
    bool stagingInputLogged = false;
    bool stagingBypassLogged = false;
    int stagingCopyLogCount = 0;
    int vpBltFailLogCount = 0;
    uint64_t fenceDeferLogTick = 0;
    bool cursorInitLogged = false;
    int overloadLogCount = 0;
    ce::log_meter::ChangeGate sourceReadQueryFailureLog{1000};
    ce::log_meter::ChangeGate deviceRemovalLog{1000};
    ce::log_meter::ChangeGate fenceWaitFailureLog{1000};
    ce::log_meter::ChangeGate allocationFailureLog{1000};
    ce::log_meter::ChangeGate retainedMismatchLog{5000};
    ce::log_meter::ChangeGate retainedKeyedMutexLog{5000};
    ce::log_meter::ChangeGate receiveFailureLog{1000};
    ce::log_meter::ChangeGate aliasFailureLog{5000};
    ce::log_meter::ChangeGate gateFailureLog{1000};
    ce::log_meter::ChangeGate overflowLog{5000};
};
