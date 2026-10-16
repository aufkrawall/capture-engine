#include "video_encoder_internal.h"
#include "video_encoder_options.h"

// Once-per-second submit/receive timing of every encoder submit (inject,
// screen-grab and repeat frames). inFlight=current/peak against pipeline (the
// encoder's in-flight depth): a peak at the depth with a large sendAvg means
// each submit waited for the oldest frame's packet. Under the encoder device
// bridge, `gate` adds the frames held at their fence (current/peak/depth) and
// the CPU waits for a conversion that was still running when the gate was full.
void VideoEncoder::LogEncoderTimingWindow() {
    const uint64_t timingNow = GetTickCount64();
    if (encoderTimingLastLogTick != 0 && timingNow - encoderTimingLastLogTick < 1000)
        return;
    const uint64_t sendAvgUs = encoderSendCalls > 0 ? encoderSendAccumUs / encoderSendCalls : 0;
    const uint64_t receiveAvgUs = encoderReceiveCalls > 0 ? encoderReceiveAccumUs / encoderReceiveCalls : 0;
    const uint64_t packetLatencyAvgUs =
        encoderPacketLatencySamples > 0 ? encoderPacketLatencyAccumUs / encoderPacketLatencySamples : 0;
    char gate[128] = "";
    if (encoderBridgeInUse) {
        const uint64_t waitAvgUs =
            submitGateStats.waits > 0 ? submitGateStats.waitAccumUs / submitGateStats.waits : 0;
        std::snprintf(gate, sizeof(gate), " gate=%zu/%zu/%zu gateWaits=%llu gateWait=%llu/%lluus",
                      pendingEncoderSubmits.size(), submitGateStats.pendingPeak, deferredSubmitDepth,
                      static_cast<unsigned long long>(submitGateStats.waits),
                      static_cast<unsigned long long>(waitAvgUs),
                      static_cast<unsigned long long>(submitGateStats.waitMaxUs));
    }
    DLL_Log(
        "[VideoEncoder Timing] sendAvg=%lluus sends=%llu receiveAvg=%lluus submitToPacket=%llu/%uus "
        "inFlight=%llu/%llu pipeline=%lld eagainDrain=%u pendingPts=%zu%s",
        static_cast<unsigned long long>(sendAvgUs), static_cast<unsigned long long>(encoderSendCalls),
        static_cast<unsigned long long>(receiveAvgUs), static_cast<unsigned long long>(packetLatencyAvgUs),
        encoderPacketLatencyMaxUs, static_cast<unsigned long long>(GetEncoderInFlightFrames()),
        static_cast<unsigned long long>(encoderInFlightPeak), static_cast<long long>(encoderPipelineInFlight),
        encoderEagainDrainCount, encoderSubmitQpcByPts.size(), gate);
    submitGateStats.Reset(pendingEncoderSubmits.size());
    encoderSendAccumUs = 0;
    encoderSendCalls = 0;
    encoderReceiveAccumUs = 0;
    encoderReceiveCalls = 0;
    encoderPacketLatencyAccumUs = 0;
    encoderPacketLatencySamples = 0;
    encoderPacketLatencyMaxUs = 0;
    encoderEagainDrainCount = 0;
    encoderInFlightPeak = GetEncoderInFlightFrames();
    encoderTimingLastLogTick = timingNow;
}

// Reads the depth the encoder actually opened with: FFmpeg's NVENC wrapper
// writes its final surface count and in-flight depth back into the "surfaces"
// and "delay" options (raised for lookahead/B-frames); AMF and Quick Sync use
// "async_depth".
void VideoEncoder::LogEncoderPipelineDepth(int requestedSurfaces) {
    encoderPipelineInFlight = 0;
    if (!codecCtx || !codecCtx->priv_data)
        return;
    int64_t surfaces = 0;
    int64_t delay = 0;
    if (av_opt_get_int(codecCtx->priv_data, "surfaces", 0, &surfaces) >= 0 &&
        av_opt_get_int(codecCtx->priv_data, "delay", 0, &delay) >= 0) {
        encoderPipelineInFlight = delay;
        DLL_Log("[VideoEncoder]   pipeline: surfaces=%lld inFlight=%lld (requested surfaces=%d = %dms at %dfps; "
                "FFmpeg default 4/3)",
                static_cast<long long>(surfaces), static_cast<long long>(delay), requestedSurfaces,
                ce::video::kNvencPipelineLatencyBudgetMs, savedConfig.fps);
        return;
    }
    int64_t asyncDepth = 0;
    if (av_opt_get_int(codecCtx->priv_data, "async_depth", 0, &asyncDepth) >= 0) {
        encoderPipelineInFlight = asyncDepth;
        DLL_Log("[VideoEncoder]   pipeline: async_depth=%lld", static_cast<long long>(asyncDepth));
    }
}

// Collects a resolved GPU-stage sample of `path` and logs its window when due
// (at most one line per five samples, i.e. every ~5 s while recording), or with
// `flush` whenever it holds a sample.
void VideoEncoder::LogGpuStageTiming(ce::media::gpu_stage::Path path, bool flush) {
    std::string stageLine;
    std::string timelineLine;
    bool stageDue = false;
    bool timelineDue = false;
    {
        D3D11ScopedLock lock;
        stageDue = gpuStageMonitor.TakeLogLine(path, d3d11Context, &stageLine, flush);
        timelineDue = gpuStageMonitor.TakeTimelineLine(path, d3d11Context, &timelineLine, flush);
    }
    if (stageDue)
        DLL_Log("%s", stageLine.c_str());
    if (timelineDue)
        DLL_Log("%s", timelineLine.c_str());
}

void VideoEncoder::FlushGpuStageTiming() {
    if (!d3d11Context)
        return;
    for (size_t index = 0; index < ce::media::gpu_stage::kPathCount; ++index)
        LogGpuStageTiming(static_cast<ce::media::gpu_stage::Path>(index), true);
}
