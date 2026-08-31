#include "video_encoder_internal.h"

bool VideoEncoder::NormalizeHdrPacketIfNeeded(AVPacket* packet) {
    if (!packet || !stream || packet->stream_index != stream->index || !ShouldEncodeHdrOutput()) {
        return true;
    }

    const int result =
        ce::video_metadata::NormalizeHdrPacketMetadata(packet, stream->codecpar, codecCtx->time_base);
    if (result < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(result, error, sizeof(error));
        DLL_Log("[HDR Metadata] ERROR: Failed to normalize packet-carried sequence header: %d (%s)", result, error);
        discardOutputRequested.store(true, std::memory_order_release);
        return false;
    }
    if (result > 0 && !hdrPacketMetadataLogged) {
        DLL_Log(
            "[HDR Metadata] Normalized packet-carried sequence header and NEW_EXTRADATA; ordinary packets bypass "
            "the metadata filter");
        hdrPacketMetadataLogged = true;
    }
    return true;
}

void VideoEncoder::WriteFrame(AVPacket* pkt) {
    if (!fileOpened || !fmtCtx)
        return;
    if (liveOutput && liveOutputFailed.load(std::memory_order_acquire))
        return;

    if (!NormalizeHdrPacketIfNeeded(pkt)) {
        return;
    }

    // Rescale timestamps from codec time_base to stream time_base
    AVStream* st = fmtCtx->streams[pkt->stream_index];
    AVRational codec_tb;

    if (pkt->stream_index == stream->index) {
        // Video packet - use video codec time_base
        codec_tb = codecCtx->time_base;
    } else {
        // Audio packet - audio time_base is typically 1/sample_rate
        // The audio encoder uses PTS = sample_count, so time_base is {1,
        // sample_rate}
        codec_tb = {1, st->codecpar->sample_rate};

        // Verify audio packet flow on a bounded cadence (~5-10 s at typical
        // 47-94 pkt/s); per-packet lines are unnecessary at trace level.
        if (audioPacketCount++ % 500 == 0) {
            DLL_Log(
                "[VideoEncoder] Queuing audio pkt #%d size=%d pts=%lld "
                "dur=%lld stream_idx=%d",
                audioPacketCount, pkt->size, (long long)pkt->pts, (long long)pkt->duration, pkt->stream_index);
        }
    }

    // Debug: log first 20 video packets with DTS to verify B-frame ordering
    if (pkt->stream_index == stream->index && videoPacketCount++ < 20) {
        bool isKeyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        bool isTiny = (pkt->size <= 5 && codecCtx->max_b_frames > 0);
        bool isTemporalDelimiter = false;
        if (isTiny && pkt->size >= 3 && pkt->data) {
            uint8_t obuType = (pkt->data[0] >> 3) & 0x0F;
            if (obuType == 2)
                isTemporalDelimiter = true;
        }
        const char* type = isKeyframe ? "KEY" : (isTemporalDelimiter ? "TD" : (isTiny ? "SEF" : "DATA"));
        DLL_Log(
            "[VideoEncoder] Queuing video pkt #%d: pts=%lld dts=%lld dur=%lld "
            "size=%d %s codec_tb=%d/%d st_tb=%d/%d",
            videoPacketCount, (long long)pkt->pts, (long long)pkt->dts, (long long)pkt->duration, pkt->size, type,
            codec_tb.num, codec_tb.den, st->time_base.num, st->time_base.den);
    }

    // Track packet types for B-frame quality diagnostics
    if (pkt->stream_index == stream->index) {
        packetStats.totalPackets++;
        if (pkt->flags & AV_PKT_FLAG_KEY) {
            packetStats.keyframeBytes += pkt->size;
            packetStats.keyframeCount++;
        } else if (pkt->size <= 5 && codecCtx->max_b_frames > 0) {
            // Check for AV1 temporal delimiter OBUs.
            // Temporal delimiters (OBU type 2) have header byte 0x12 at pkt->data[0].
            // They are normal AV1 frame-boundary markers that players ignore.
            bool isTemporalDelimiter = false;
            if (pkt->size >= 3 && pkt->data) {
                // OBU header byte: bits 3-6 = obu_type, type 2 = temporal delimiter
                uint8_t obuType = (pkt->data[0] >> 3) & 0x0F;
                if (obuType == 2) {
                    isTemporalDelimiter = true;
                }
            }
            if (!isTemporalDelimiter) {
                packetStats.sefBytes += pkt->size;
                packetStats.sefCount++;
            }
        } else if (pkt->size < 2000 && codecCtx->max_b_frames > 0) {
            // Likely a leaf B-frame with near-zero bit allocation.
            // Only classify when B-frames are active — small P-frames are
            // normal in non-B-frame mode and shouldn't be flagged.
            packetStats.bframeBytes += pkt->size;
            packetStats.bframeCount++;
        } else {
            packetStats.refBytes += pkt->size;

            packetStats.refCount++;
        }

        // Log packet type distribution every 600 packets (~5 seconds at 120fps)
        if (packetStats.totalPackets > 0 && packetStats.totalPackets % 600 == 0) {
            int total = packetStats.totalPackets;
            int64_t avgKey = packetStats.keyframeCount > 0 ? packetStats.keyframeBytes / packetStats.keyframeCount : 0;
            int64_t avgRef = packetStats.refCount > 0 ? packetStats.refBytes / packetStats.refCount : 0;
            int64_t avgB = packetStats.bframeCount > 0 ? packetStats.bframeBytes / packetStats.bframeCount : 0;
            DLL_Log(
                "[VideoEncoder] PACKET STATS (%d pkts): "
                "Key=%d(avg %lldKB) Ref=%d(avg %lldKB) "
                "SEF=%d(%d%%) B-small=%d(avg %lldB)",
                total, packetStats.keyframeCount, avgKey / 1024, packetStats.refCount, avgRef / 1024,
                packetStats.sefCount, total > 0 ? packetStats.sefCount * 100 / total : 0, packetStats.bframeCount,
                avgB);

            // Warn about B-frame quality oscillation
            if (packetStats.sefCount + packetStats.bframeCount > total / 3 && avgRef > 0 && avgB > 0 &&
                avgB < avgRef / 50) {
                DLL_Log(
                    "[VideoEncoder] WARNING: B-frame quality oscillation detected! "
                    "B-frames average %lldB vs reference frames %lldKB (ratio 1:%lld). "
                    "Consider b_frames=0 for smoothest capture.",
                    avgB, avgRef / 1024, avgRef / (avgB > 0 ? avgB : 1));
            }
        }
    }

    // Rescale timestamps properly using FFmpeg's exact rational math
    av_packet_rescale_ts(pkt, codec_tb, st->time_base);
    if (pkt->stream_index == stream->index && pkt->dts == AV_NOPTS_VALUE) {
        pkt->dts = pkt->pts;
    }

    // DEBUG: Log PTS after rescaling and detect corruption
    if (pkt->stream_index == stream->index) {
        if (vidDebugCount++ < 20 || pkt->pts < 0) {
            DLL_Log("[VideoEncoder] PTS PRECISE: frame=%lld pts_us=%lld st_tb=%d/%d", pkt->pts, pkt->pts,
                    st->time_base.num, st->time_base.den);
        }

        // DEBUG LEAK: Log queue stats every 600 video frames (~5 s at 120 fps).
        // The CRITICAL overflow line below fires on the first cadence hit after
        // the condition appears; the condition is persistent, so a ≤5 s delay
        // does not lose it. The trend line needs only enough samples to show a
        // queue that grows over time.
        if (vidDebugCount % 600 == 0) {
            size_t qBytes = currentQueueBytes.load();
            size_t qSize = 0;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                qSize = packetQueue.size();
            }
            const size_t queueLimit = ActiveQueueLimitBytes();
            DLL_Log("[VideoEncoder] QUEUE STATS: Count=%zu Bytes=%zu (Max=%zu)", qSize, qBytes, queueLimit);

            // Memory safety check
            if (qBytes > queueLimit) {
                DLL_Log("[VideoEncoder] CRITICAL: Queue exceeds limit! Dropping disabled?");
            }
        }
    }

    // CRITICAL: For video packets, explicitly set duration after rescaling.
    // In CFR mode, the Bresenham PTS distribution (e.g. 120fps at 1/1000 time_base
    // produces gaps of 8,8,9,8,8,9...) means a fixed duration of 8 leaves a 1ms
    // gap for every 9ms step.  Compute each frame's exact duration from the
    // sequential PTS difference so it always matches the actual PTS spacing.
    if (pkt->stream_index == stream->index) {
        int64_t preClampDuration = pkt->duration;
        int fps = codecCtx->framerate.num;
        if (fps <= 0)
            fps = 60;
        if (!savedConfig.useVFR) {
            // CFR: derive per-frame duration from the Bresenham PTS sequence.
            // Round-trip the already-rescaled PTS back to the codec frame number
            // so the calculation is independent of packet arrival order (critical
            // for B-frame codecs that output packets in decode order).
            //
            // Try codec-provided duration first (NVENC sets this correctly including
            // for AV1 B-frames and SEF packets), fall back to round-trip rescaling.
            if (pkt->duration > 0) {
                // Codec provided a valid duration — use it directly
            } else {
                int64_t frameNum = av_rescale_q_rnd(pkt->pts, st->time_base, codecCtx->time_base, AV_ROUND_NEAR_INF);
                int64_t nextPts = av_rescale_q(frameNum + 1, codecCtx->time_base, st->time_base);
                pkt->duration = nextPts - pkt->pts;
            }
            // Clamp duration to sane range for CFR: [1, fps] to prevent
            // 0-duration or extreme-duration packets from corrupting the
            // MKV container timeline.  AV1 SEF packets can produce duration=0
            // from the codec, and round-trip rescaling can produce 0 or 2
            // due to integer rounding at non-power-of-2 FPS.
            int64_t maxDuration = av_rescale_q(2, codecCtx->time_base, st->time_base);
            if (maxDuration < 2)
                maxDuration = 2;
            if (pkt->duration <= 0)
                pkt->duration = 1;
            if (pkt->duration > maxDuration)
                pkt->duration = maxDuration;
        } else {
            pkt->duration = av_rescale(1, st->time_base.den, fps);
        }
        if (pkt->duration <= 0)
            pkt->duration = 1;
        if (pkt->duration != preClampDuration) {
            packetDurationClampCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Track authoritative encoded video duration from packet timeline.
    if (pkt->stream_index == stream->index) {
        if (pkt->pts < 0 || pkt->dts < 0) {
            negativePtsCount.fetch_add(1, std::memory_order_relaxed);
        }
        int64_t packetTimelinePts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
        if (lastQueuedVideoPts != AV_NOPTS_VALUE && packetTimelinePts != AV_NOPTS_VALUE &&
            packetTimelinePts < lastQueuedVideoPts) {
            nonMonotonicPtsCount.fetch_add(1, std::memory_order_relaxed);
            DLL_Log("[VideoEncoder] WARNING: non-monotonic packet pts prev=%lld cur=%lld dur=%lld",
                    static_cast<long long>(lastQueuedVideoPts), static_cast<long long>(packetTimelinePts),
                    static_cast<long long>(pkt->duration));
        }
        if (packetTimelinePts != AV_NOPTS_VALUE) {
            lastQueuedVideoPts = packetTimelinePts;
        }
        int64_t packetPts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
        if (packetPts != AV_NOPTS_VALUE) {
            int64_t packetDuration = pkt->duration;
            if (packetDuration <= 0) {
                packetDuration = av_rescale_q(1, codec_tb, st->time_base);
                if (packetDuration <= 0) {
                    packetDuration = 1;
                }
            }
            int64_t packetEnd = packetPts + packetDuration;
            int64_t packetEndUs = av_rescale_q(packetEnd, st->time_base, AVRational{1, 1000000});
            int64_t prevEndUs = encodedDurationUs.load(std::memory_order_relaxed);
            if (packetEndUs > prevEndUs) {
                encodedDurationUs.store(packetEndUs, std::memory_order_relaxed);
            }
        }
    }

    // ASYNC WRITE: Push to queue instead of writing directly

    // A recording preserves bitstream validity with backpressure. A live stream
    // cannot do that: blocking capture would move video away from the existing
    // audio/CFR clocks. If two seconds of encoded output cannot drain, fail the
    // session as a unit rather than dropping packets into a corrupt stream.
    const size_t queueLimit = ActiveQueueLimitBytes();
    const size_t packetBytes = static_cast<size_t>(std::max(pkt->size, 0)) + sizeof(AVPacket);
    if (liveOutput) {
        const size_t queueBytes = currentQueueBytes.load(std::memory_order_relaxed);
        if (ce::live_stream::ExceedsQueueBudget(queueBytes, packetBytes, queueLimit)) {
            lastMuxOverloadTickMs.store(GetTickCount64(), std::memory_order_relaxed);
            muxBackpressureCount.fetch_add(1, std::memory_order_relaxed);
            PublishRuntimeState();
            DLL_Log("[LiveStream] Encoded output queue exceeded its bounded latency budget (%zu/%zu bytes)",
                    queueBytes, queueLimit);
            RequestLiveOutputFailure("queue_budget", AVERROR_BUFFER_TOO_SMALL);
            return;
        }
    }

    // IMPORTANT: Never drop encoded packets from a recording, because that
    // causes visible corruption. Apply backpressure to the encode thread.
    // If storage is extremely slow, this will manifest as stutter/dropped input
    // frames (FrameQueue will drop/duplicate), but the bitstream stays valid.
    uint64_t backpressureWaitUs = 0;
    while (!liveOutput) {
        size_t qBytes = currentQueueBytes.load(std::memory_order_relaxed);
        if (qBytes <= queueLimit) {
            break;
        }

        lastMuxOverloadTickMs.store(GetTickCount64(), std::memory_order_relaxed);
        muxBackpressureCount.fetch_add(1, std::memory_order_relaxed);
        PublishRuntimeState();

        static int overloadLogCount = 0;
        if (overloadLogCount++ % 60 == 0) {
            DLL_Log(
                "[VideoEncoder] WARNING: Packet queue overloaded (%zu bytes) - "
                "applying backpressure",
                qBytes);
        }

        // Wait briefly for writer to drain.
        const auto waitStart = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(queueMutex);
        queueCV.wait_for(lock, std::chrono::milliseconds(2), [this] {
            return currentQueueBytes.load(std::memory_order_relaxed) <= ActiveQueueLimitBytes() || isStopping ||
                   !writerRunning;
        });
        backpressureWaitUs += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - waitStart)
                .count());
        if (isStopping || !writerRunning) {
            break;
        }
    }
    if (backpressureWaitUs > 0) {
        const uint32_t waitUs32 = SaturatingToUint32(backpressureWaitUs);
        muxBackpressureWaitUs.store(waitUs32, std::memory_order_relaxed);
        UpdateAtomicPeak(muxBackpressureMaxWaitUs, waitUs32);
    }

    AVPacket* clonePkt = av_packet_clone(pkt);
    if (clonePkt) {
        bool rejectedByLiveBudget = false;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            const size_t queuedBytes = currentQueueBytes.load(std::memory_order_relaxed);
            const size_t clonedBytes = static_cast<size_t>(std::max(clonePkt->size, 0)) + sizeof(AVPacket);
            rejectedByLiveBudget =
                liveOutput && (liveOutputFailed.load(std::memory_order_acquire) ||
                               ce::live_stream::ExceedsQueueBudget(queuedBytes, clonedBytes, queueLimit));
            if (!rejectedByLiveBudget) {
                packetQueue.push(clonePkt);
                currentQueueBytes += clonedBytes;
                currentQueuePackets.store(SaturatingToUint32(packetQueue.size()), std::memory_order_relaxed);
            }
        }
        if (rejectedByLiveBudget) {
            av_packet_free(&clonePkt);
            RequestLiveOutputFailure("queue_budget", AVERROR_BUFFER_TOO_SMALL);
            return;
        }
        UpdateAtomicPeak(peakQueueBytes, SaturatingToUint32(currentQueueBytes.load(std::memory_order_relaxed)));
        UpdateAtomicPeak(peakQueuePackets, currentQueuePackets.load(std::memory_order_relaxed));
        PublishRuntimeState();
        queueCV.notify_one();
    } else if (liveOutput) {
        RequestLiveOutputFailure("clone_packet", AVERROR(ENOMEM));
    }
}

void VideoEncoder::PublishRuntimeState() {
    if (!pSharedMem) {
        return;
    }

    // Keep this cheap and lock-free: only atomics.
    uint32_t flags = 0;
    uint64_t nowMs = GetTickCount64();

    constexpr uint64_t kOverloadHoldMs = 1000;
    uint64_t encTick = lastEncoderOverloadTickMs.load(std::memory_order_relaxed);
    uint64_t muxTick = lastMuxOverloadTickMs.load(std::memory_order_relaxed);

    if (encTick != 0 && (nowMs - encTick) <= kOverloadHoldMs) {
        flags |= ce::capture_policy::kEncoderOverloadFlagEncoder;
    }
    if (pSharedMem->runtimeState.encoderBottlenecked.load(std::memory_order_relaxed) != 0) {
        flags |= ce::capture_policy::kEncoderOverloadFlagEncoder;
    }
    if (muxTick != 0 && (nowMs - muxTick) <= kOverloadHoldMs) {
        flags |= ce::capture_policy::kEncoderOverloadFlagMux;
    }

    pSharedMem->runtimeState.encoderOverloadFlags.store(flags, std::memory_order_relaxed);
    const double encodeMs = static_cast<double>(std::max<int64_t>(lastEncodeTimeUs, 0)) / 1000.0;
    UpdateAdaptiveGpuThreadPriority(nowMs, encodeMs, (flags & ce::capture_policy::kEncoderOverloadFlagEncoder) != 0);
    const double sustainFps = ce::capture_policy::GetEncoderSustainableOutputFps(encodeMs);
    const uint32_t sustainFpsX100 =
        sustainFps > 0.0 ? static_cast<uint32_t>(std::clamp(sustainFps * 100.0, 0.0, 4294967295.0)) : 0u;
    pSharedMem->runtimeState.encoderSustainFpsX100.store(sustainFpsX100, std::memory_order_relaxed);

    size_t qBytes = currentQueueBytes.load(std::memory_order_relaxed);
    uint32_t qBytes32 = (qBytes > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)qBytes;
    pSharedMem->runtimeState.muxQueueBytes.store(qBytes32, std::memory_order_relaxed);
    pSharedMem->runtimeState.muxQueuePackets.store(currentQueuePackets.load(std::memory_order_relaxed),
                                                   std::memory_order_relaxed);
    pSharedMem->runtimeState.muxQueuePeakBytes.store(peakQueueBytes.load(std::memory_order_relaxed),
                                                     std::memory_order_relaxed);
    pSharedMem->runtimeState.muxQueuePeakPackets.store(peakQueuePackets.load(std::memory_order_relaxed),
                                                       std::memory_order_relaxed);
    pSharedMem->runtimeState.muxBackpressureCount.store(muxBackpressureCount.load(std::memory_order_relaxed),
                                                        std::memory_order_relaxed);
    pSharedMem->runtimeState.muxBackpressureWaitUs.store(muxBackpressureWaitUs.load(std::memory_order_relaxed),
                                                         std::memory_order_relaxed);
    pSharedMem->runtimeState.muxBackpressureMaxWaitUs.store(muxBackpressureMaxWaitUs.load(std::memory_order_relaxed),
                                                            std::memory_order_relaxed);
    pSharedMem->runtimeState.packetDurationClamps.store(packetDurationClampCount.load(std::memory_order_relaxed),
                                                        std::memory_order_relaxed);
    pSharedMem->runtimeState.negativePtsCount.store(negativePtsCount.load(std::memory_order_relaxed),
                                                    std::memory_order_relaxed);
    pSharedMem->runtimeState.nonMonotonicPtsCount.store(nonMonotonicPtsCount.load(std::memory_order_relaxed),
                                                        std::memory_order_relaxed);
}
