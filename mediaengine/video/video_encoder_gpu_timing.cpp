#include "video_encoder_internal.h"

// Once-per-second submit/receive timing of the fresh-frame encoder submits.
void VideoEncoder::LogEncoderTimingWindow() {
    const uint64_t timingNow = GetTickCount64();
    if (encoderTimingLastLogTick != 0 && timingNow - encoderTimingLastLogTick < 1000)
        return;
    const uint64_t sendAvgUs = encoderSendCalls > 0 ? encoderSendAccumUs / encoderSendCalls : 0;
    const uint64_t receiveAvgUs = encoderReceiveCalls > 0 ? encoderReceiveAccumUs / encoderReceiveCalls : 0;
    const uint64_t packetLatencyAvgUs =
        encoderPacketLatencySamples > 0 ? encoderPacketLatencyAccumUs / encoderPacketLatencySamples : 0;
    DLL_Log(
        "[VideoEncoder Timing] sendAvg=%lluus receiveAvg=%lluus submitToPacket=%llu/%uus "
        "eagainDrain=%u pendingPts=%zu",
        static_cast<unsigned long long>(sendAvgUs), static_cast<unsigned long long>(receiveAvgUs),
        static_cast<unsigned long long>(packetLatencyAvgUs), encoderPacketLatencyMaxUs, encoderEagainDrainCount,
        encoderSubmitQpcByPts.size());
    encoderSendAccumUs = 0;
    encoderSendCalls = 0;
    encoderReceiveAccumUs = 0;
    encoderReceiveCalls = 0;
    encoderPacketLatencyAccumUs = 0;
    encoderPacketLatencySamples = 0;
    encoderPacketLatencyMaxUs = 0;
    encoderEagainDrainCount = 0;
    encoderTimingLastLogTick = timingNow;
}

// Collects a resolved GPU-stage sample of `path` and logs its window when due
// (at most one line per five samples, i.e. every ~5 s while recording).
void VideoEncoder::LogGpuStageTiming(ce::media::gpu_stage::Path path) {
    std::string line;
    {
        D3D11ScopedLock lock;
        if (!gpuStageMonitor.TakeLogLine(path, d3d11Context, &line))
            return;
    }
    DLL_Log("%s", line.c_str());
}
