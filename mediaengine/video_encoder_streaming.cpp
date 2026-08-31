#include "video_encoder_internal.h"

std::string VideoEncoder::OutputTargetForLog() const {
    return liveOutput ? "<live-stream-endpoint>" : ce::privacy::CollapsePathForLog(outputFilename);
}

void VideoEncoder::ArmOutputIoDeadline() {
    if (liveOutput)
        outputIoDeadlineMs.store(GetTickCount64() + 5000, std::memory_order_release);
}

void VideoEncoder::ClearOutputIoDeadline() {
    outputIoDeadlineMs.store(0, std::memory_order_release);
}

void VideoEncoder::ConfigureLiveMuxTimestampOffset() {
    if (!liveOutput || !fmtCtx)
        return;

    int64_t offsetUs = 0;
    for (unsigned int i = 0; i < fmtCtx->nb_streams; ++i) {
        const AVStream* currentStream = fmtCtx->streams[i];
        if (!currentStream || !currentStream->codecpar ||
            currentStream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO ||
            currentStream->codecpar->initial_padding <= 0 || currentStream->codecpar->sample_rate <= 0) {
            continue;
        }
        const int64_t audioOffsetUs = av_rescale_q_rnd(
            currentStream->codecpar->initial_padding, AVRational{1, currentStream->codecpar->sample_rate},
            AV_TIME_BASE_Q, static_cast<AVRounding>(AV_ROUND_UP | AV_ROUND_PASS_MINMAX));
        offsetUs = std::max(offsetUs, audioOffsetUs);
    }

    if (codecCtx && codecCtx->has_b_frames > 0 && codecCtx->time_base.num > 0 && codecCtx->time_base.den > 0) {
        const int64_t videoOffsetUs =
            av_rescale_q_rnd(codecCtx->has_b_frames, codecCtx->time_base, AV_TIME_BASE_Q,
                             static_cast<AVRounding>(AV_ROUND_UP | AV_ROUND_PASS_MINMAX));
        offsetUs = std::max(offsetUs, videoOffsetUs);
    }

    // FLV cannot represent negative timestamps. Reserving the known codec priming/reorder interval
    // up front keeps all streams on one origin even if the low-latency interleaver has to emit video
    // before the first AAC packet arrives. This is the deterministic equivalent of FFmpeg's normal
    // "avoid negative timestamps" shift when every stream is already present in its packet queue.
    fmtCtx->output_ts_offset = offsetUs;
    fmtCtx->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_NON_NEGATIVE;
    DLL_Log("[LiveStream] mux timestamp origin shift=%lldus", static_cast<long long>(offsetUs));
}

int VideoEncoder::InterruptOutputIo(void* opaque) {
    auto* encoder = static_cast<VideoEncoder*>(opaque);
    if (!encoder)
        return 0;
    if (encoder->outputIoAbort.load(std::memory_order_acquire))
        return 1;
    const uint64_t deadline = encoder->outputIoDeadlineMs.load(std::memory_order_acquire);
    return deadline != 0 && GetTickCount64() >= deadline ? 1 : 0;
}

int VideoEncoder::WriteInterleavedPacket(AVPacket* packet) {
    ArmOutputIoDeadline();
    const int result = av_interleaved_write_frame(fmtCtx, packet);
    ClearOutputIoDeadline();
    if (liveOutput && result < 0)
        outputIoAbort.store(true, std::memory_order_release);
    return result;
}

void VideoEncoder::RequestLiveOutputFailure(const char* operation, int errorCode) {
    if (!liveOutput || liveOutputFailed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    discardOutputRequested.store(true, std::memory_order_release);
    outputIoAbort.store(true, std::memory_order_release);
    DLL_Log(
        "[LiveStream] ERROR: operation=%s error=%d endpoint=<redacted>; requesting an orderly recording stop",
        operation ? operation : "unknown", errorCode);
    // During a user-requested Stop, still record that the final queue drain failed, but do not
    // re-issue the stop command. While running, the controller command is what terminates all
    // audio/video producers on the same timeline after a terminal transport failure.
    if (!isStopping.load(std::memory_order_acquire) && pSharedMem) {
        pSharedMem->runtimeState.recordingFailureCode.store(
            static_cast<uint32_t>(RecordingFailureCode::LiveStreamOutputFailed), std::memory_order_release);
        pSharedMem->runtimeState.cmdStopRecording.store(true, std::memory_order_release);
    }
    queueCV.notify_all();
}

size_t VideoEncoder::ActiveQueueLimitBytes() const {
    return liveOutput ? liveQueueLimitBytes : MAX_QUEUE_BYTES;
}
