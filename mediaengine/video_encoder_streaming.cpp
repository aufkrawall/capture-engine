#include "video_encoder_internal.h"

std::string VideoEncoder::OutputTargetForLog() const {
    return liveOutput ? "<live-stream-endpoint>" : ce::privacy::CollapsePathForLog(outputFilename);
}

namespace {
// A live stream that cannot write for 5 s has lost its bounded-latency
// contract. A local file gets longer - a slow disk can legitimately stall on a
// large keyframe flush - but never unbounded: a hung write (dead network share,
// dying disk) must not wedge the writer thread forever, because Stop() gives up
// waiting for finalize and the unpublished staging file would never reach the
// user. FFmpeg's interrupt callback only refuses the next transfer; a write
// already blocked in the kernel is broken by CancelExpiredOutputIo.
constexpr uint64_t kLiveOutputIoTimeoutMs = 5000;
constexpr uint64_t kLocalOutputIoTimeoutMs = 30000;
}  // namespace

void VideoEncoder::ArmOutputIoDeadline() {
    std::lock_guard<std::mutex> lock(outputIoCancelMutex);
    outputIoDeadlineMs.store(GetTickCount64() + (liveOutput ? kLiveOutputIoTimeoutMs : kLocalOutputIoTimeoutMs),
                             std::memory_order_release);
}

void VideoEncoder::ClearOutputIoDeadline() {
    std::lock_guard<std::mutex> lock(outputIoCancelMutex);
    outputIoDeadlineMs.store(0, std::memory_order_release);
}

void VideoEncoder::RegisterOutputIoThread() {
    HANDLE self = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, THREAD_TERMINATE, FALSE,
                         0)) {
        DLL_Log("[VideoEncoder] WARNING: writer thread handle unavailable (error=%lu); a hung output write cannot be "
                "cancelled",
                GetLastError());
        return;
    }
    std::lock_guard<std::mutex> lock(outputIoCancelMutex);
    if (outputIoThread) {
        CloseHandle(outputIoThread);
    }
    outputIoThread = self;
}

void VideoEncoder::UnregisterOutputIoThread() {
    std::lock_guard<std::mutex> lock(outputIoCancelMutex);
    if (outputIoThread) {
        CloseHandle(outputIoThread);
        outputIoThread = nullptr;
    }
}

bool VideoEncoder::CancelExpiredOutputIo(const char* context) {
    // Lock-free pre-check: the common case (no write in flight, or one within
    // its deadline) must not touch the mutex on the encoder's per-packet path.
    if (!ce::mux::IsOutputIoDeadlineExpired(outputIoDeadlineMs.load(std::memory_order_acquire), GetTickCount64())) {
        return false;
    }
    // Arm/Clear take the same lock, so the operation seen expired here is still
    // the one in flight: the writer cannot finish it and start another in between.
    std::lock_guard<std::mutex> lock(outputIoCancelMutex);
    const uint64_t deadline = outputIoDeadlineMs.load(std::memory_order_acquire);
    if (!outputIoThread || !ce::mux::IsOutputIoDeadlineExpired(deadline, GetTickCount64())) {
        return false;
    }
    const BOOL cancelled = CancelSynchronousIo(outputIoThread);
    const DWORD error = cancelled ? ERROR_SUCCESS : GetLastError();
    static std::atomic<uint32_t> s_cancelLogCount{0};
    const uint32_t logCount = s_cancelLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 8 || (logCount & (logCount + 1)) == 0) {
        DLL_Log("[VideoEncoder] ERROR: output I/O exceeded its deadline by %llums (%s); CancelSynchronousIo=%d "
                "error=%lu (#%u)",
                static_cast<unsigned long long>(GetTickCount64() - deadline), context ? context : "unknown",
                cancelled ? 1 : 0, error, logCount + 1);
    }
    return cancelled != FALSE;
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
    return ce::mux::IsOutputIoDeadlineExpired(encoder->outputIoDeadlineMs.load(std::memory_order_acquire),
                                              GetTickCount64())
               ? 1
               : 0;
}

int VideoEncoder::WriteInterleavedPacket(AVPacket* packet) {
    ArmOutputIoDeadline();
    const int result = av_interleaved_write_frame(fmtCtx, packet);
    ClearOutputIoDeadline();
    if (liveOutput && result < 0)
        outputIoAbort.store(true, std::memory_order_release);
    return result;
}

void VideoEncoder::RequestOutputFailure(const char* operation, int errorCode) {
    if (liveOutput) {
        RequestLiveOutputFailure(operation, errorCode);
    } else {
        RequestLocalOutputFailure(operation, errorCode);
    }
}

void VideoEncoder::RequestLocalOutputFailure(const char* operation, int errorCode) {
    const uint32_t errors = muxOutputErrorCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (errors != 1) {
        return;  // the stop was already requested; keep counting losses only
    }
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errorCode, errbuf, sizeof(errbuf));
    DLL_Log(
        "[VideoEncoder] ERROR: recording output operation=%s failed: %d (%s); the committed part of the file is "
        "kept and the completion is reported as degraded%s",
        operation ? operation : "unknown", errorCode, errbuf,
        isStopping.load(std::memory_order_acquire) ? "" : " - stopping the recording now");
    // FFmpeg's AVIO error is sticky (see ce::mux::SelectVideoOutputDisposition),
    // so every later packet would be lost as well. End the recording at the
    // first loss - the orderly stop finalizes and publishes what is committed -
    // instead of silently dropping the rest of the session.
    if (!isStopping.load(std::memory_order_acquire) && pSharedMem) {
        pSharedMem->runtimeState.cmdStopRecording.store(true, std::memory_order_release);
    }
    queueCV.notify_all();
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
