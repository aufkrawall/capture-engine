#include "video_encoder_internal.h"
#include "common/logging/log_meter.h"

#include <utility>

// The single encoder submit/drain of every capture method. Inject, screen-grab
// (WGC / DXGI duplication) and repeat frames used to carry three copies of this
// loop, and only the inject copy measured send/receive time and packet latency,
// so screen-grab overload could not be told apart from inject overload.

namespace {

uint64_t QpcNowTicks() {
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart);
}

uint64_t QpcTicksPerSecond() {
    LARGE_INTEGER frequency = {};
    QueryPerformanceFrequency(&frequency);
    return static_cast<uint64_t>(frequency.QuadPart);
}

}  // namespace

uint64_t VideoEncoder::GetEncoderInFlightFrames() const {
    const uint64_t submitted = encoderFramesSubmitted.load(std::memory_order_acquire);
    const uint64_t received = encoderPacketsReceived.load(std::memory_order_acquire);
    // Frames handed over at their fence are accepted too (encoder_device_bridge.h).
    const uint64_t pending = pendingEncoderSubmitCount.load(std::memory_order_acquire);
    // AV1 show-existing slots wait behind the packet of the frame they re-show.
    return (submitted > received ? submitted - received : 0) + pending + av1Repeats.PendingSlots();
}

void VideoEncoder::ResetEncoderSubmitAccounting() {
    encoderSubmitQpcByPts.clear();
    encoderSendAccumUs = 0;
    encoderSendCalls = 0;
    encoderReceiveAccumUs = 0;
    encoderReceiveCalls = 0;
    encoderPacketLatencyAccumUs = 0;
    encoderPacketLatencySamples = 0;
    encoderPacketLatencyMaxUs = 0;
    encoderEagainDrainCount = 0;
    encoderTimingLastLogTick = 0;
    encoderInFlightPeak = 0;
    encoderFramesSubmitted.store(0, std::memory_order_release);
    encoderPacketsReceived.store(0, std::memory_order_release);
}

int VideoEncoder::DrainEncoderPackets(AVPacket* pkt, const char* label, bool logPacketDebug) {
    static ce::log_meter::ChangeGate receiveFailureLog(1000);
    int drained = 0;
    while (true) {
        const auto receiveStart = PerfTimer::now();
        const int ret = avcodec_receive_packet(codecCtx, pkt);
        const auto receiveEnd = PerfTimer::now();
        encoderReceiveAccumUs +=
            static_cast<uint64_t>(std::max(0.0, PerfTimer::elapsed_ms(receiveStart, receiveEnd) * 1000.0));
        ++encoderReceiveCalls;
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            const auto log = receiveFailureLog.Observe(ce::log_meter::FieldKey(ret), GetTickCount64());
            if (log) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
                DLL_Log("[VideoEncoder] %s receive_packet failed: %d (%s)%s", label, ret, errbuf,
                        ce::log_meter::SuppressedNote(log.suppressed).c_str());
            }
            break;
        }

        ++drained;
        encoderPacketsReceived.fetch_add(1, std::memory_order_acq_rel);
        const int64_t packetKey = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
        // The encoder has read this frame: its pool texture may be reused now.
        if (encoderInputRetention.Size() > 0)
            encoderInputRetention.ReleaseForPacket(packetKey);
        const auto submitIt = encoderSubmitQpcByPts.find(packetKey);
        if (submitIt != encoderSubmitQpcByPts.end()) {
            const uint64_t nowQpc = QpcNowTicks();
            const uint64_t frequency = QpcTicksPerSecond();
            const uint64_t submittedQpc = static_cast<uint64_t>(submitIt->second);
            if (frequency > 0 && nowQpc >= submittedQpc) {
                const uint64_t latencyUs = (nowQpc - submittedQpc) * 1000000ull / frequency;
                encoderPacketLatencyAccumUs += latencyUs;
                ++encoderPacketLatencySamples;
                encoderPacketLatencyMaxUs = std::max(encoderPacketLatencyMaxUs, SaturatingToUint32(latencyUs));
            }
            encoderSubmitQpcByPts.erase(submitIt);
        }
        if (logPacketDebug) {
            DLL_Log("[VideoEncoder] %s received pkt: pts=%lld dts=%lld size=%d flags=%d", label, pkt->pts,
                    pkt->dts, pkt->size, pkt->flags);
        }
        pkt->stream_index = stream->index;
        // CFR: time_base is 1/fps, so one unit. VFR: time_base is 1 us.
        pkt->duration = savedConfig.useVFR ? 1000000 / (savedConfig.fps > 0 ? savedConfig.fps : 60) : 1;

        if (onPacket)
            av1Repeats.WritePacket(pkt, [this](AVPacket* out) {
                out->stream_index = stream->index;
                onPacket(out);
            });
        av_packet_unref(pkt);
    }
    return drained;
}

bool VideoEncoder::SendFrameToEncoder(AVFrame* frame, ce::media::gpu_stage::Path path, const char* label,
                                      AVPacket* pkt, int* packetCount, bool logPacketDebug) {
    av1Repeats.PrepareSubmit(codecCtx, frame);
    const bool success = (frame && encoderBridgeInUse)
                             ? HandOffFrameToEncoder(frame, path, label, pkt, packetCount, logPacketDebug)
                             : SendFrameNow(frame, path, label, pkt, packetCount, logPacketDebug, true);
    if (!success && frame)
        av1Repeats.OnSubmitDropped(frame->pts);
    LogEncoderTimingWindow();
    LogGpuStageTiming(path);
    // Inject source-read markers are polled here and after AV1 show-existing
    // repeats (RepeatLastFrame), both on the thread that also drives NVENC, so
    // no other thread touches the immediate context while an encoder submit
    // runs (source_read_markers.h).
    PollSourceReadCompletion();
    return success;
}

bool VideoEncoder::SendFrameNow(AVFrame* frame, ce::media::gpu_stage::Path path, const char* label, AVPacket* pkt,
                                int* packetCount, bool logPacketDebug, bool probeSend) {
    int drained = DrainEncoderPackets(pkt, label, logPacketDebug);
    bool firstSend = probeSend;
    auto timedSend = [&]() {
        if (firstSend) {
            D3D11ScopedLock lock;
            gpuStageMonitor.BeforeSend(path, d3d11Context);
        }
        const auto sendStart = PerfTimer::now();
        const int result = avcodec_send_frame(codecCtx, frame);
        const auto sendEnd = PerfTimer::now();
        const auto sendUs = static_cast<uint64_t>(std::max(0.0, PerfTimer::elapsed_ms(sendStart, sendEnd) * 1000.0));
        encoderSendAccumUs += sendUs;
        ++encoderSendCalls;
        if (std::exchange(firstSend, false)) {
            D3D11ScopedLock lock;
            gpuStageMonitor.AfterSend(path, d3d11Context, sendUs);
        }
        return result;
    };

    int ret = timedSend();
    int retries = 0;
    if (ret == AVERROR(EAGAIN))
        ++encoderEagainDrainCount;
    while (ret == AVERROR(EAGAIN) && retries < 10) {
        if (retries == 0) {
            lastEncoderOverloadTickMs.store(GetTickCount64(), std::memory_order_relaxed);
            PublishRuntimeState();
        }
        drained += DrainEncoderPackets(pkt, label, logPacketDebug);
        ret = timedSend();
        retries++;
    }
    bool success = true;
    if (ret == AVERROR(EAGAIN)) {
        DLL_Log("[VideoEncoder] %s send_frame remained EAGAIN after %d drain attempts", label, retries);
        success = false;
    } else if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        DLL_Log("[VideoEncoder] %s send_frame failed: %d (%s)", label, ret, errbuf);
        success = false;
    }
    if (success && frame) {
        encoderFramesSubmitted.fetch_add(1, std::memory_order_acq_rel);
        if (ce::media::EncoderReleasesInputOnSubmit(savedConfig.encoder.c_str()))
            RetainEncoderInputFrame(frame);
        if (frame->pts != AV_NOPTS_VALUE) {
            encoderSubmitQpcByPts[frame->pts] = static_cast<int64_t>(QpcNowTicks());
            while (encoderSubmitQpcByPts.size() > 256)
                encoderSubmitQpcByPts.erase(encoderSubmitQpcByPts.begin());
        }
        encoderInFlightPeak = std::max(encoderInFlightPeak, GetEncoderInFlightFrames());
        drained += DrainEncoderPackets(pkt, label, logPacketDebug);
    }
    if (packetCount)
        *packetCount += drained;
    return success;
}
