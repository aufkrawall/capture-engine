                            maxDuration = 2;
                        if (pkt->duration > maxDuration)
                            pkt->duration = maxDuration;
                    }

                    if (pkt->pts != AV_NOPTS_VALUE) {
                        int64_t packetEnd = pkt->pts + pkt->duration;
                        int64_t packetEndUs = av_rescale_q(packetEnd, stream->time_base, AVRational{1, 1000000});
                        int64_t prevEndUs = encodedDurationUs.load(std::memory_order_relaxed);
                        if (packetEndUs > prevEndUs) {
                            encodedDurationUs.store(packetEndUs, std::memory_order_relaxed);
                        }
                    }

                    const int flushedStreamIndex = pkt->stream_index;
                    const int64_t flushedPts = pkt->pts;
                    const int64_t flushedDts = pkt->dts;
                    const int64_t flushedDuration = pkt->duration;
                    const AVRational flushedTimeBase = stream->time_base;
                    if (av_interleaved_write_frame(fmtCtx, pkt) >= 0) {
                        RecordWrittenPacketTimeline(flushedStreamIndex, flushedPts, flushedDts, flushedDuration,
                                                    flushedTimeBase, 0, 0);
                    }
                    av_packet_unref(pkt);
                    flushedCount++;
                }
                av_packet_free(&pkt);
                DLL_Log("[VideoEncoder] Async Finalize: Flushed %d remaining packets", flushedCount);
            }

            // 2. Write Trailer and Close File
            if (fmtCtx && fileOpened) {
                writerFinalizePhase.store(kWriterPhaseWritingTrailer, std::memory_order_release);
                DLL_Log("[VideoEncoder] Async Finalize: Writing Trailer...");
                int64_t finalDurationUs = encodedDurationUs.load(std::memory_order_relaxed);
                if (finalDurationUs > 0) {
                    LogPacketTimelineSummary(finalDurationUs);
                    DLL_Log("[VideoEncoder] Async Finalize: packet-derived duration target was %lld us",
                            finalDurationUs);
                }
                const int trailerResult = av_write_trailer(fmtCtx);
                if (trailerResult < 0) {
                    DLL_Log("[VideoEncoder] Async Finalize: ERROR av_write_trailer failed: %d", trailerResult);
                }
                if (finalDurationUs > 0) {
                    for (unsigned s = 0; s < fmtCtx->nb_streams; s++) {
                        AVStream* st = fmtCtx->streams[s];
                        int64_t firstPts = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
                        int64_t lastPts = firstPts;
                        if (st->duration > 0) {
                            lastPts = firstPts + st->duration;
                        }
                        int64_t firstPtsUs = av_rescale_q(firstPts, st->time_base, AVRational{1, 1000000});
                        int64_t lastPtsUs = av_rescale_q(lastPts, st->time_base, AVRational{1, 1000000});
                        DLL_Log(
                            "[PTS ALIGN METADATA] Stream %u (codec=%s): first=%lldus last=%lldus dur=%lldus tb=%d/%lld",
                            s,
                            st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO
                                ? "video"
                                : (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ? "audio" : "unknown"),
                            (long long)firstPtsUs, (long long)lastPtsUs, (long long)(lastPtsUs - firstPtsUs),
                            st->time_base.num, (long long)st->time_base.den);
                    }
                    LogFinalDurationSummary(fmtCtx, finalDurationUs,
                                            muxBackpressureCount.load(std::memory_order_relaxed),
                                            peakQueueBytes.load(std::memory_order_relaxed),
                                            peakQueuePackets.load(std::memory_order_relaxed),
                                            lastEncoderOverloadTickMs.load(std::memory_order_relaxed) > 0,
                                            lastMuxOverloadTickMs.load(std::memory_order_relaxed) > 0);
                }
                int closeResult = 0;
                if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
                    closeResult = avio_closep(&fmtCtx->pb);
                    if (closeResult < 0) {
                        DLL_Log("[VideoEncoder] Async Finalize: ERROR avio_closep failed: %d", closeResult);
                    }
                }
                fileOpened = false;
                const bool published = FinalizeOutputPublication(trailerResult, closeResult, finalDurationUs);
                DLL_Log("[VideoEncoder] mux_closed file='%s' finalDurationUs=%lld", outputFilename.c_str(),
                        (long long)finalDurationUs);
                if (published && finalDurationUs > 0) {
                    writerFinalizePhase.store(kWriterPhasePostMuxProbe, std::memory_order_release);
                    RunPostMuxDurationProbeBounded(outputFilename, finalDurationUs);
                }
                DLL_Log("[VideoEncoder] Async Finalize: Output file closed.");
            }

            // IMPORTANT: we still hold queueMutex (lock) here.
            // CleanupResources() also locks queueMutex to drain packetQueue.
            // Unlock first to avoid self-deadlock during finalize.
            lock.unlock();

            writerFinalizePhase.store(kWriterPhaseCleanup, std::memory_order_release);
            CleanupResources();

            isStopping = false;
            writerRunning = false;
            writerFinalizePhase.store(kWriterPhaseComplete, std::memory_order_release);
            DLL_Log("[VideoEncoder] Async Finalize: Complete.");
            break;  // Exit thread
        }
    }

    DLL_Log("[VideoEncoder] Async Writer Thread Stopped");
}
