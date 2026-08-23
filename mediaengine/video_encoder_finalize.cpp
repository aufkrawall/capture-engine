#include "video_encoder_internal.h"

constexpr int kPostMuxProbeMaxPackets = 512;

constexpr int kPostMuxProbeMaxTailPackets = 16384;

const char* WriterFinalizePhaseName(uint32_t phase) {
    switch (phase) {
        case kWriterPhaseRunning:
            return "running";
        case kWriterPhaseFinalizeStarting:
            return "finalize_starting";
        case kWriterPhaseFlushingEncoder:
            return "flushing_encoder";
        case kWriterPhaseWritingTrailer:
            return "writing_trailer";
        case kWriterPhasePostMuxProbe:
            return "post_mux_probe";
        case kWriterPhaseCleanup:
            return "cleanup";
        case kWriterPhaseComplete:
            return "complete";
        default:
            return "unknown";
    }
}

// Bounded completion check for the async writer, independent of the MinGW
// threading model. std::thread::native_handle() only yields a waitable Win32
// HANDLE under the Win32 threading model; a winpthreads build returns a
// pthread_t instead, so WaitForSingleObject cannot be used here.
bool WriterFinishedWithin(std::future<void>& finished, uint64_t timeoutMs) {
    // A joinable writer is always paired with a valid future. Should that ever
    // not hold, report "still running" so muxer ownership is never assumed free.
    if (!finished.valid()) {
        return false;
    }
    return finished.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::ready;
}

bool HasValidStreamTimeBase(const AVStream* stream) {
    return stream && stream->time_base.num > 0 && stream->time_base.den > 0;
}

bool ShouldCancelPostMuxProbe(const PostMuxProbeControl* control) {
    if (!control) {
        return false;
    }
    if (control->cancel.load(std::memory_order_acquire)) {
        return true;
    }
    return control->deadlineTickMs > 0 && GetTickCount64() >= control->deadlineTickMs;
}

int PostMuxProbeInterrupt(void* opaque) {
    return ShouldCancelPostMuxProbe(static_cast<PostMuxProbeControl*>(opaque)) ? 1 : 0;
}

int64_t ParseDurationTagUs(const char* value) {
    if (!value || !*value) {
        return 0;
    }

    char* end = nullptr;
    long long hours = std::strtoll(value, &end, 10);
    if (!end || *end != ':') {
        return 0;
    }
    const char* minutesStart = end + 1;
    long long minutes = std::strtoll(minutesStart, &end, 10);
    if (!end || *end != ':') {
        return 0;
    }
    const char* secondsStart = end + 1;
    double seconds = std::strtod(secondsStart, &end);
    if (seconds < 0.0 || hours < 0 || minutes < 0) {
        return 0;
    }

    const double totalSeconds = static_cast<double>(hours * 3600LL + minutes * 60LL) + seconds;
    return static_cast<int64_t>(std::llround(totalSeconds * 1000000.0));
}

int64_t GetStreamStartUs(const AVStream* stream) {
    if (!HasValidStreamTimeBase(stream) || stream->start_time == AV_NOPTS_VALUE) {
        return 0;
    }

    return av_rescale_q(stream->start_time, stream->time_base, AVRational{1, 1000000});
}

int64_t GetStreamDurationUs(const AVStream* stream) {
    if (!HasValidStreamTimeBase(stream)) {
        return 0;
    }

    AVDictionaryEntry* tag = av_dict_get(stream->metadata, "DURATION", nullptr, 0);
    if (tag) {
        const int64_t tagDurationUs = ParseDurationTagUs(tag->value);
        if (tagDurationUs > 0) {
            return tagDurationUs;
        }
    }

    if (stream->duration == AV_NOPTS_VALUE || stream->duration <= 0) {
        return 0;
    }
    return av_rescale_q(stream->duration, stream->time_base, AVRational{1, 1000000});
}

uint32_t GetPacketTerminalDiscardSamples(const AVPacket* packet) {
    if (!packet) {
        return 0;
    }
    size_t sideDataSize = 0;
    const uint8_t* sideData = av_packet_get_side_data(packet, AV_PKT_DATA_SKIP_SAMPLES, &sideDataSize);
    return sideData && sideDataSize >= 8 ? AV_RL32(sideData + 4) : 0;
}

bool LogPostMuxDurationProbe(const std::string& filename, int64_t finalDurationUs,
                             PostMuxProbeControl* control ) {
    if (filename.empty() || finalDurationUs <= 0) {
        return true;
    }
    // Log privacy: probe diagnostics correlate by timestamped leaf name only.
    const std::string logFilename = ce::privacy::CollapsePathForLog(filename);

    AVFormatContext* probeCtx = avformat_alloc_context();
    if (!probeCtx) {
        DLL_Log("[VideoEncoder] WARNING: Post-mux duration probe failed to allocate context for '%s'",
                logFilename.c_str());
        return false;
    }
    if (control) {
        probeCtx->interrupt_callback.callback = PostMuxProbeInterrupt;
        probeCtx->interrupt_callback.opaque = control;
    }
    int ret = avformat_open_input(&probeCtx, filename.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        DLL_Log("[VideoEncoder] WARNING: Post-mux duration probe failed to open '%s': %s", logFilename.c_str(), errbuf);
        avformat_close_input(&probeCtx);
        return false;
    }

    ret = avformat_find_stream_info(probeCtx, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        DLL_Log("[VideoEncoder] WARNING: Post-mux duration probe failed stream info for '%s': %s", logFilename.c_str(),
                errbuf);
        avformat_close_input(&probeCtx);
        return false;
    }

    int64_t maxVideoEndUs = 0;
    int64_t minAudioEndUs = 0;
    int64_t maxAudioEndUs = 0;
    int64_t maxAudioDeltaUs = 0;
    int64_t maxAudioRoundingToleranceUs = 1;
    uint32_t videoStreamCount = 0;
    uint32_t audioStreamCount = 0;
    int64_t minRawAudioEndUs = 0;
    int64_t maxRawAudioEndUs = 0;
    uint64_t totalInitialPaddingSamples = 0;
    uint64_t totalTerminalPaddingSamples = 0;
    uint32_t codecPaddedAudioStreamCount = 0;
    std::vector<int64_t> firstPacketStartUs(probeCtx->nb_streams, INT64_MAX);
    av_seek_frame(probeCtx, -1, 0, AVSEEK_FLAG_BACKWARD);
    AVPacket* pkt = av_packet_alloc();
    int packetsRead = 0;
    while (pkt && packetsRead < kPostMuxProbeMaxPackets && !ShouldCancelPostMuxProbe(control)) {
        ret = av_read_frame(probeCtx, pkt);
        if (ret < 0) {
            break;
        }
        ++packetsRead;
        if (pkt->stream_index >= 0 && static_cast<unsigned int>(pkt->stream_index) < probeCtx->nb_streams &&
            pkt->pts != AV_NOPTS_VALUE) {
            const AVStream* packetStream = probeCtx->streams[pkt->stream_index];
            if (HasValidStreamTimeBase(packetStream)) {
                const int64_t packetStartUs = av_rescale_q(pkt->pts, packetStream->time_base, AVRational{1, 1000000});
                firstPacketStartUs[pkt->stream_index] = std::min(firstPacketStartUs[pkt->stream_index], packetStartUs);
            }
        }
        av_packet_unref(pkt);
        bool allStartsKnown = true;
        for (unsigned int i = 0; i < probeCtx->nb_streams; ++i) {
            const AVStream* stream = probeCtx->streams[i];
            if (!stream || !stream->codecpar || stream->codecpar->codec_type == AVMEDIA_TYPE_UNKNOWN) {
                continue;
            }
            const bool hasStreamStart = HasValidStreamTimeBase(stream) && stream->start_time != AV_NOPTS_VALUE;
            if (!hasStreamStart && firstPacketStartUs[i] == INT64_MAX) {
                allStartsKnown = false;
                break;
            }
        }
        if (allStartsKnown) {
            break;
        }
    }
    if (pkt) {
        av_packet_free(&pkt);
    }
    if (ShouldCancelPostMuxProbe(control)) {
        DLL_Log("[VideoEncoder] post_mux_probe_cancelled file='%s' packets=%d", logFilename.c_str(), packetsRead);
        avformat_close_input(&probeCtx);
        return false;
    }
    if (packetsRead >= kPostMuxProbeMaxPackets) {
        DLL_Log("[VideoEncoder] post_mux_probe_packet_limit file='%s' packets=%d", logFilename.c_str(), packetsRead);
    }

    std::vector<uint32_t> terminalDiscardSamples(probeCtx->nb_streams, 0);
    std::vector<int64_t> terminalPacketPts(probeCtx->nb_streams, INT64_MIN);
    bool tailScanComplete = false;
    int tailPacketsRead = 0;
    const int64_t tailSeekUs = std::max<int64_t>(0, finalDurationUs - 5000000);
    ret = avformat_seek_file(probeCtx, -1, INT64_MIN, tailSeekUs, INT64_MAX, AVSEEK_FLAG_BACKWARD);
    if (ret >= 0) {
        pkt = av_packet_alloc();
        while (pkt && tailPacketsRead < kPostMuxProbeMaxTailPackets && !ShouldCancelPostMuxProbe(control)) {
            ret = av_read_frame(probeCtx, pkt);
            if (ret < 0) {
                tailScanComplete = ret == AVERROR_EOF;
                break;
            }
            ++tailPacketsRead;
            if (pkt->stream_index >= 0 && static_cast<unsigned int>(pkt->stream_index) < probeCtx->nb_streams &&
                pkt->pts != AV_NOPTS_VALUE && pkt->pts >= terminalPacketPts[pkt->stream_index]) {
                const AVStream* packetStream = probeCtx->streams[pkt->stream_index];
                if (packetStream && packetStream->codecpar &&
                    packetStream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                    terminalPacketPts[pkt->stream_index] = pkt->pts;
                    terminalDiscardSamples[pkt->stream_index] = GetPacketTerminalDiscardSamples(pkt);
                }
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }
    if (!tailScanComplete) {
        DLL_Log("[VideoEncoder] post_mux_probe_tail_incomplete file='%s' seekRet=%d packets=%d limit=%d",
                logFilename.c_str(), ret, tailPacketsRead, kPostMuxProbeMaxTailPackets);
    }

    for (unsigned int i = 0; i < probeCtx->nb_streams; ++i) {
        const AVStream* probedStream = probeCtx->streams[i];
        if (!probedStream || !probedStream->codecpar) {
            continue;
        }

        const int64_t durationUs = GetStreamDurationUs(probedStream);
        if (durationUs <= 0) {
            continue;
        }
        const bool hasStreamStart = HasValidStreamTimeBase(probedStream) && probedStream->start_time != AV_NOPTS_VALUE;
        const bool hasFirstPacketStart = firstPacketStartUs[i] != INT64_MAX;
        const int64_t startUs =
            ce::mux::ChoosePostMuxStreamStartUs(hasStreamStart ? GetStreamStartUs(probedStream) : 0, hasStreamStart,
                                                hasFirstPacketStart ? firstPacketStartUs[i] : 0, hasFirstPacketStart);
        const int64_t endUs = startUs + durationUs;
        if (probedStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            ++videoStreamCount;
            maxVideoEndUs = std::max(maxVideoEndUs, endUs);
        } else if (probedStream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            const int sampleRate = probedStream->codecpar->sample_rate;
            const uint64_t initialPaddingSamples =
                static_cast<uint64_t>(std::max(0, probedStream->codecpar->initial_padding));
            const uint64_t endPaddingSamples = tailScanComplete ? terminalDiscardSamples[i] : 0;
            const int64_t presentationStartUs =
                hasStreamStart ? std::max<int64_t>(0, GetStreamStartUs(probedStream)) : 0;
            const int64_t decodedDurationUs = ce::mux::ComputeDecodedAudioDurationUs(
                durationUs, sampleRate, initialPaddingSamples, endPaddingSamples);
            const int64_t decodedEndUs = presentationStartUs + decodedDurationUs;
            ++audioStreamCount;
            if (minAudioEndUs == 0 || decodedEndUs < minAudioEndUs) {
                minAudioEndUs = decodedEndUs;
            }
            maxAudioEndUs = std::max(maxAudioEndUs, decodedEndUs);
            if (minRawAudioEndUs == 0 || endUs < minRawAudioEndUs) {
                minRawAudioEndUs = endUs;
            }
            maxRawAudioEndUs = std::max(maxRawAudioEndUs, endUs);
            totalInitialPaddingSamples += initialPaddingSamples;
            totalTerminalPaddingSamples += endPaddingSamples;
            codecPaddedAudioStreamCount += (initialPaddingSamples > 0 || endPaddingSamples > 0) ? 1u : 0u;
            maxAudioDeltaUs = std::max(maxAudioDeltaUs, ce::mux::ComputeDurationDeltaUs(decodedEndUs, finalDurationUs));
            maxAudioRoundingToleranceUs = std::max(
                maxAudioRoundingToleranceUs,
                ce::mux::ComputeAudioMuxRoundingToleranceUs(probedStream->codecpar->sample_rate,
                                                            probedStream->time_base.num, probedStream->time_base.den));
        }
    }

    DLL_Log(
        "[VideoEncoder] Post-mux duration probe: target=%lld us videoEnd=%lld us audioMinEnd=%lld us "
        "audioMaxEnd=%lld us maxAudioDelta=%lld us streams(v=%u a=%u) rawAudioMinEnd=%lld us "
        "rawAudioMaxEnd=%lld us paddingSamples(initial=%llu terminal=%llu streams=%u tailComplete=%d)",
        finalDurationUs, maxVideoEndUs, minAudioEndUs, maxAudioEndUs, maxAudioDeltaUs, videoStreamCount,
        audioStreamCount, minRawAudioEndUs, maxRawAudioEndUs,
        static_cast<unsigned long long>(totalInitialPaddingSamples),
        static_cast<unsigned long long>(totalTerminalPaddingSamples), codecPaddedAudioStreamCount,
        tailScanComplete ? 1 : 0);

    if (codecPaddedAudioStreamCount > 0) {
        DLL_Log(
            "[VideoEncoder] Post-mux audio codec-padding applied: decodedMinEnd=%lld decodedMaxEnd=%lld "
            "rawMinEnd=%lld rawMaxEnd=%lld initialSamples=%llu terminalSamples=%llu streams=%u tailComplete=%d",
            minAudioEndUs, maxAudioEndUs, minRawAudioEndUs, maxRawAudioEndUs,
            static_cast<unsigned long long>(totalInitialPaddingSamples),
            static_cast<unsigned long long>(totalTerminalPaddingSamples), codecPaddedAudioStreamCount,
            tailScanComplete ? 1 : 0);
    }

    if (audioStreamCount > 0 && maxAudioDeltaUs > 0 && maxAudioDeltaUs <= maxAudioRoundingToleranceUs) {
        DLL_Log(
            "[VideoEncoder] Post-mux audio duration rounding evidence (target=%lld audioMinEnd=%lld "
            "audioMaxEnd=%lld maxDelta=%lld tolerance=%lld)",
            finalDurationUs, minAudioEndUs, maxAudioEndUs, maxAudioDeltaUs, maxAudioRoundingToleranceUs);
    } else if (audioStreamCount > 0 && maxAudioDeltaUs > maxAudioRoundingToleranceUs) {
        DLL_Log(
            "[VideoEncoder] WARNING: Post-mux audio duration mismatch (target=%lld audioMinEnd=%lld "
            "audioMaxEnd=%lld maxDelta=%lld tolerance=%lld)",
            finalDurationUs, minAudioEndUs, maxAudioEndUs, maxAudioDeltaUs, maxAudioRoundingToleranceUs);
    }

    avformat_close_input(&probeCtx);
    return true;
}

void RunPostMuxDurationProbeBounded(const std::string& filename, int64_t finalDurationUs,
                                    uint64_t timeoutMs) {
    if (filename.empty() || finalDurationUs <= 0) {
        return;
    }

    PostMuxProbeControl control;
    control.deadlineTickMs = GetTickCount64() + std::max<uint64_t>(1, timeoutMs);
    const uint64_t startMs = GetTickCount64();
    DLL_Log("[VideoEncoder] post_mux_probe_start file='%s' target=%lld timeout=%llums",
            ce::privacy::CollapsePathForLog(filename).c_str(), (long long)finalDurationUs,
            (unsigned long long)timeoutMs);
    // Run on the already-owned writer/finalizer thread. Every potentially
    // blocking demux operation sees the deadline through interrupt_callback,
    // and packet inspection has a hard count bound. A nested worker cannot be
    // abandoned safely because it could continue executing mediaengine/FFmpeg
    // code after the DLL is unloaded.
    const bool ok = LogPostMuxDurationProbe(filename, finalDurationUs, &control);
    const uint64_t elapsedMs = GetTickCount64() - startMs;
    DLL_Log("[VideoEncoder] post_mux_probe_complete ok=%d timedOut=%d elapsed=%llums", ok ? 1 : 0,
            ShouldCancelPostMuxProbe(&control) ? 1 : 0, (unsigned long long)elapsedMs);
}

bool ValidateFormatContextForHeader(const AVFormatContext* fmtCtx) {
    if (!fmtCtx) {
        DLL_Log("[VideoEncoder] ERROR: Refusing to write header with null format context");
        return false;
    }

    if (fmtCtx->nb_streams == 0) {
        DLL_Log("[VideoEncoder] ERROR: Refusing to write header with no streams");
        return false;
    }

    for (unsigned int i = 0; i < fmtCtx->nb_streams; ++i) {
        const AVStream* stream = fmtCtx->streams[i];
        const ce::mux::HeaderValidationIssue issue =
            ce::mux::ValidateStreamForHeader(stream != nullptr, stream && stream->codecpar != nullptr,
                                             stream ? stream->time_base.num : 0, stream ? stream->time_base.den : 0);
        if (issue != ce::mux::HeaderValidationIssue::kNone) {
            DLL_Log("[VideoEncoder] ERROR: Refusing to write header: stream %u invalid (%s)", i,
                    ce::mux::HeaderValidationIssueToString(issue));
            return false;
        }
    }

    return true;
}

int64_t ComputeTargetVideoPts(int64_t timestampUs, bool useVfr, int fps, int64_t startPts, int64_t lastAssignedVideoPts,
                              bool useExplicitCfrTimeline) {
    if (useVfr && startPts >= 0) {
        int64_t elapsedUs = timestampUs - startPts;
        if (elapsedUs < 0) {
            elapsedUs = 0;
        }
        return elapsedUs;
    }

    if (useExplicitCfrTimeline) {
        // The explicit screen-grab timestamp selects content on the immutable
        // wall grid; it must never punch a hole in the encoded CFR PTS prefix.
        // Late ticks are represented by fresh/held-frame choice and bounded
        // catch-up submissions, each of which still owns exactly one next PTS.
        return ComputeNextCfrFrameIndex(lastAssignedVideoPts);
    }

    // Generic CFR/inject mode also owns one explicit encoder call per output
    // slot here.
    return ComputeNextCfrFrameIndex(lastAssignedVideoPts);
}

bool IsConfiguredNvencLookaheadActive(const std::string& value) {
    return !value.empty() && _stricmp(value.c_str(), "off") != 0 && _stricmp(value.c_str(), "false") != 0 &&
           _stricmp(value.c_str(), "disabled") != 0 && value != "0";
}

bool IsConfiguredNvencMultipassActive(const VideoConfig& config) {
    if (_stricmp(config.multipass.c_str(), "qres") == 0 || _stricmp(config.multipass.c_str(), "fullres") == 0) {
        return true;
    }
    if (!config.multipass.empty() && _stricmp(config.multipass.c_str(), "auto") != 0) {
        return false;
    }
    return config.bFrames > 0 || _stricmp(config.rateControl.c_str(), "cbr") == 0;
}

void LogFinalDurationSummary(AVFormatContext* fmtCtx, int64_t finalDurationUs, uint32_t muxBackpressureEvents,
                             uint32_t peakQueueBytes, uint32_t peakQueuePackets, bool encoderOverloaded,
                             bool muxOverloaded) {
    if (!fmtCtx || finalDurationUs <= 0) {
        return;
    }

    int64_t maxStreamDeltaUs = 0;
    int64_t maxVideoDurationUs = 0;
    int64_t minAudioDurationUs = 0;
    int64_t maxAudioDurationUs = 0;
    uint32_t videoStreamCount = 0;
    uint32_t audioStreamCount = 0;
    uint32_t declaredVideoStreamCount = 0;
    uint32_t declaredAudioStreamCount = 0;
    for (unsigned int i = 0; i < fmtCtx->nb_streams; ++i) {
        AVStream* stream = fmtCtx->streams[i];
        if (!stream || !stream->codecpar) {
            continue;
        }
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            ++declaredVideoStreamCount;
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++declaredAudioStreamCount;
        }
        if (!HasValidStreamTimeBase(stream) || stream->duration <= 0) {
            continue;
        }

        const int64_t streamDurationUs = av_rescale_q(stream->duration, stream->time_base, AVRational{1, 1000000});
        const int64_t durationDeltaUs = ce::mux::ComputeDurationDeltaUs(streamDurationUs, finalDurationUs);
        maxStreamDeltaUs = std::max(maxStreamDeltaUs, durationDeltaUs);

        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            ++videoStreamCount;
            maxVideoDurationUs = std::max(maxVideoDurationUs, streamDurationUs);
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++audioStreamCount;
            if (minAudioDurationUs == 0 || streamDurationUs < minAudioDurationUs) {
                minAudioDurationUs = streamDurationUs;
            }
            maxAudioDurationUs = std::max(maxAudioDurationUs, streamDurationUs);
        }
    }

    DLL_Log(
        "[VideoEncoder] Final metadata durations: target=%lld us video=%lld us audioMin=%lld us audioMax=%lld us "
        "maxDelta=%lld us available(v=%u/%u a=%u/%u) overload(encoder=%d mux=%d) backpressure=%u "
        "peakMux=%uKB peakPkts=%u",
        finalDurationUs, maxVideoDurationUs, minAudioDurationUs, maxAudioDurationUs, maxStreamDeltaUs, videoStreamCount,
        declaredVideoStreamCount, audioStreamCount, declaredAudioStreamCount, encoderOverloaded ? 1 : 0,
        muxOverloaded ? 1 : 0, muxBackpressureEvents, peakQueueBytes / 1024u, peakQueuePackets);

    const bool metadataComplete = videoStreamCount == declaredVideoStreamCount &&
                                  audioStreamCount == declaredAudioStreamCount && videoStreamCount > 0;
    if (!metadataComplete) {
        DLL_Log(
            "[VideoEncoder] Final in-memory AVStream durations are incomplete; packet timelines and the bounded "
            "post-mux probe remain authoritative (available v=%u/%u a=%u/%u).",
            videoStreamCount, declaredVideoStreamCount, audioStreamCount, declaredAudioStreamCount);
        return;
    }

    constexpr int64_t kDurationWarningToleranceUs = 1000;
    if (!ce::mux::IsDurationWithinToleranceUs(maxVideoDurationUs, finalDurationUs, kDurationWarningToleranceUs) ||
        (declaredAudioStreamCount > 0 &&
         !ce::mux::IsDurationWithinToleranceUs(minAudioDurationUs, maxAudioDurationUs, kDurationWarningToleranceUs)) ||
        maxStreamDeltaUs > kDurationWarningToleranceUs) {
        DLL_Log(
            "[VideoEncoder] WARNING: Final metadata stream durations exceeded %lld us tolerance (target=%lld "
            "video=%lld "
            "audioMin=%lld audioMax=%lld maxDelta=%lld)",
            kDurationWarningToleranceUs, finalDurationUs, maxVideoDurationUs, minAudioDurationUs, maxAudioDurationUs,
            maxStreamDeltaUs);
    }
}

void VideoEncoder::ResetPacketTimelineDiagnostics() {
    writtenPacketTimelines.clear();
    lastMuxerVideoPtsUs.store(0, std::memory_order_relaxed);
}

void VideoEncoder::RecordWrittenPacketTimeline(int streamIndex, int64_t pts, int64_t dts, int64_t duration,
                                               AVRational timeBase, uint32_t terminalDiscardSamples, int sampleRate) {
    if (streamIndex < 0 || !fmtCtx || static_cast<unsigned int>(streamIndex) >= fmtCtx->nb_streams ||
        !HasValidStreamTimeBase(fmtCtx->streams[streamIndex])) {
        return;
    }

    const int64_t packetPts = pts != AV_NOPTS_VALUE ? pts : dts;
    if (packetPts == AV_NOPTS_VALUE) {
        return;
    }

    if (writtenPacketTimelines.size() < fmtCtx->nb_streams) {
        writtenPacketTimelines.resize(fmtCtx->nb_streams);
    }
    if (static_cast<size_t>(streamIndex) >= writtenPacketTimelines.size()) {
        return;
    }

    const int64_t packetStartUs = av_rescale_q(packetPts, timeBase, AVRational{1, 1000000});
    const int64_t packetDurationUs = duration > 0 ? av_rescale_q(duration, timeBase, AVRational{1, 1000000}) : 0;
    const int64_t terminalDiscardUs = ce::mux::ComputeAudioPaddingDurationUs(terminalDiscardSamples, sampleRate);
    ce::mux::ObservePacketTimeline(writtenPacketTimelines[streamIndex], packetStartUs, packetDurationUs,
                                   terminalDiscardUs);
}

void VideoEncoder::LogPacketTimelineSummary(int64_t finalDurationUs) const {
    if (!fmtCtx || finalDurationUs <= 0 || writtenPacketTimelines.empty()) {
        return;
    }

    int64_t maxVideoEndUs = 0;
    int64_t minAudioEndUs = 0;
    int64_t maxAudioEndUs = 0;
    int64_t maxRawAudioEndUs = 0;
    int64_t maxPacketDeltaUs = 0;
    uint32_t videoStreamCount = 0;
    uint64_t videoPacketCount = 0;
    int64_t maxVideoPtsGapUs = 0;
    uint32_t audioStreamCount = 0;
    uint32_t audioPastTargetCount = 0;

    for (unsigned int i = 0; i < fmtCtx->nb_streams && i < writtenPacketTimelines.size(); ++i) {
        const AVStream* st = fmtCtx->streams[i];
        if (!st || !st->codecpar) {
            continue;
        }
        const auto& timeline = writtenPacketTimelines[i];
        if (!timeline.seen) {
            continue;
        }

        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            ++videoStreamCount;
            videoPacketCount += timeline.packetCount;
            maxVideoEndUs = std::max(maxVideoEndUs, timeline.lastEndUs);
            maxVideoPtsGapUs = std::max(maxVideoPtsGapUs, timeline.maxForwardStartGapUs);
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++audioStreamCount;
            if (minAudioEndUs == 0 || timeline.lastDecodedEndUs < minAudioEndUs) {
                minAudioEndUs = timeline.lastDecodedEndUs;
            }
            maxAudioEndUs = std::max(maxAudioEndUs, timeline.lastDecodedEndUs);
            maxRawAudioEndUs = std::max(maxRawAudioEndUs, timeline.lastEndUs);
            if (timeline.lastDecodedEndUs > finalDurationUs + 1000) {
                ++audioPastTargetCount;
            }
        }
    }

    const int64_t referenceVideoEndUs = maxVideoEndUs > 0 ? maxVideoEndUs : finalDurationUs;
    if (audioStreamCount > 0) {
        maxPacketDeltaUs = std::max(ce::mux::ComputeDurationDeltaUs(referenceVideoEndUs, minAudioEndUs),
                                    ce::mux::ComputeDurationDeltaUs(referenceVideoEndUs, maxAudioEndUs));
    }

    DLL_Log(
        "[VideoEncoder] Final packet timeline: target=%lld us videoEnd=%lld us audioMinEnd=%lld us "
        "audioMaxEnd=%lld us maxPacketDelta=%lld us streams(v=%u a=%u) audioPastTarget=%u rawAudioMaxEnd=%lld us",
        finalDurationUs, maxVideoEndUs, minAudioEndUs, maxAudioEndUs, maxPacketDeltaUs, videoStreamCount,
        audioStreamCount, audioPastTargetCount, maxRawAudioEndUs);
    if (!savedConfig.useVFR && savedConfig.fps > 0 && videoStreamCount > 0) {
        const int64_t expectedPackets = av_rescale_rnd(finalDurationUs, savedConfig.fps, 1000000, AV_ROUND_NEAR_INF);
        const int64_t emittedPackets = static_cast<int64_t>(videoPacketCount);
        const int64_t missingPackets = std::max<int64_t>(0, expectedPackets - emittedPackets);
        const double maxPtsGapTicks = static_cast<double>(maxVideoPtsGapUs) * savedConfig.fps / 1000000.0;
        const bool coverageComplete = missingPackets == 0 && maxPtsGapTicks <= 1.01;
        DLL_Log(
            "[VideoEncoder] CFR packet coverage: expected=%lld emitted=%lld missing=%lld maxPtsGapUs=%lld "
            "maxPtsGapTicks=%.3f complete=%d fps=%d",
            expectedPackets, emittedPackets, missingPackets, maxVideoPtsGapUs, maxPtsGapTicks, coverageComplete ? 1 : 0,
            savedConfig.fps);
        if (!coverageComplete) {
            DLL_Log(
                "[VideoEncoder] ERROR: CFR artifact failed packet-continuity validation: expected=%lld emitted=%lld "
                "missing=%lld maxPtsGapTicks=%.3f (required <=1.01)",
                expectedPackets, emittedPackets, missingPackets, maxPtsGapTicks);
        }
    }

    constexpr int64_t kPacketDurationWarningToleranceUs = 1000;
    if (audioPastTargetCount > 0 || (audioStreamCount > 0 && maxPacketDeltaUs > kPacketDurationWarningToleranceUs)) {
        DLL_Log(
            "[VideoEncoder] WARNING: Packet-level A/V duration mismatch exceeded %lld us tolerance "
            "(target=%lld videoEnd=%lld audioMinEnd=%lld audioMaxEnd=%lld maxPacketDelta=%lld)",
            kPacketDurationWarningToleranceUs, finalDurationUs, maxVideoEndUs, minAudioEndUs, maxAudioEndUs,
            maxPacketDeltaUs);
    }
}

uint64_t VideoEncoder::GetWrittenVideoPacketCount() const {
    if (!fmtCtx || !stream || stream->index < 0 ||
        static_cast<size_t>(stream->index) >= writtenPacketTimelines.size()) {
        return 0;
    }
    return writtenPacketTimelines[static_cast<size_t>(stream->index)].packetCount;
}

bool VideoEncoder::FinalizeOutputPublication(int trailerResult, int closeResult, int64_t finalDurationUs) {
    outputPublished.store(false, std::memory_order_relaxed);
    const uint64_t writtenVideoPackets = GetWrittenVideoPacketCount();
    const auto disposition = ce::mux::SelectVideoOutputDisposition(
        discardOutputRequested.load(std::memory_order_acquire), trailerResult, closeResult, finalDurationUs,
        writtenVideoPackets);
    if (disposition != ce::mux::VideoOutputDisposition::kPublish) {
        const bool removed = outputReservation.CleanupOwnedFile();
        DLL_Log(
            "[VideoEncoder] output_discarded reason=%s durationUs=%lld videoPackets=%llu trailer=%d close=%d "
            "removed=%d staging='%s'",
            ce::mux::VideoOutputDispositionToString(disposition), static_cast<long long>(finalDurationUs),
            static_cast<unsigned long long>(writtenVideoPackets), trailerResult, closeResult, removed ? 1 : 0,
            outputFilename.c_str());
        return false;
    }

    const fs::path outputDirectory = ce::capture_output::ResolveCaptureDirectory(
        savedConfig.outputDir, ce::capture_output::GetExecutableDirectory());
    const std::wstring extension(savedConfig.container.begin(), savedConfig.container.end());
    if (!outputReservation.PublishToNewPath(outputDirectory, L"capture", extension)) {
        const DWORD publishError = GetLastError();
        outputReservation.Publish();
        DLL_Log(
            "[VideoEncoder] output_renamed_to_staging error=%lu durationUs=%lld videoPackets=%llu "
            "staging='%s' (file is playable but was not renamed to final name)",
            publishError, static_cast<long long>(finalDurationUs),
            static_cast<unsigned long long>(writtenVideoPackets), outputFilename.c_str());
        outputPublished.store(true, std::memory_order_release);
        return true;
    }

    outputFilename = outputReservation.Utf8Path();
    DLL_Log("[VideoEncoder] output_published file='%s' durationUs=%lld videoPackets=%llu",
            ce::privacy::CollapsePathForLog(outputFilename).c_str(),
            static_cast<long long>(finalDurationUs), static_cast<unsigned long long>(writtenVideoPackets));
    outputPublished.store(true, std::memory_order_release);
    return true;
}
