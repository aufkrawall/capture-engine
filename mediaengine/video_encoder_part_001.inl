#include "video_encoder.h"
#include "../common/capture_pipeline_policy.h"
#include "../common/frame_timing_utils.h"
#include "../common/path_utils.h"
#include "../common/raii_helpers.h"
#include "../common/reserved_capture_output.h"
#include "../common/secure_dll_loading.h"
#include "../common/shared_defs.h"
#include "audio_time_utils.h"  // For ce::audio::ParseSampleRateOr
#include "matroska_timing.h"
#include "mediaengine.h"
#include "mux_invariants.h"
#include "video_encoder_options.h"
#include "video_color_conversion_shader.h"
#include "video_format_policy.h"
#include "video_metadata.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>

extern "C" {
#include <libavutil/intreadwrite.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include <filesystem>
#include "cursor_renderer.h"

static void TrimD3D11Residency(ID3D11Device* device, ID3D11DeviceContext* context, const char* label) {
    if (context) {
        context->ClearState();
        context->Flush();
    }
    if (!device) {
        return;
    }

    IDXGIDevice3* dxgiDevice3 = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice3))) && dxgiDevice3) {
        dxgiDevice3->Trim();
        dxgiDevice3->Release();
        DLL_Log("[VideoEncoder] Trimmed D3D11 residency for %s", label);
    }
}

namespace {

enum WriterFinalizePhase : uint32_t {
    kWriterPhaseRunning = 0,
    kWriterPhaseFinalizeStarting = 1,
    kWriterPhaseFlushingEncoder = 2,
    kWriterPhaseWritingTrailer = 3,
    kWriterPhasePostMuxProbe = 4,
    kWriterPhaseCleanup = 5,
    kWriterPhaseComplete = 6,
};

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

constexpr uint64_t kPostMuxProbeTimeoutMs = 5000;
constexpr int kPostMuxProbeMaxPackets = 512;
constexpr int kPostMuxProbeMaxTailPackets = 16384;

struct PostMuxProbeControl {
    std::atomic<bool> cancel{false};
    uint64_t deadlineTickMs = 0;
};

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
                             PostMuxProbeControl* control = nullptr) {
    if (filename.empty() || finalDurationUs <= 0) {
        return true;
    }

    AVFormatContext* probeCtx = avformat_alloc_context();
    if (!probeCtx) {
        DLL_Log("[VideoEncoder] WARNING: Post-mux duration probe failed to allocate context for '%s'",
                filename.c_str());
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
        DLL_Log("[VideoEncoder] WARNING: Post-mux duration probe failed to open '%s': %s", filename.c_str(), errbuf);
        avformat_close_input(&probeCtx);
        return false;
    }

    ret = avformat_find_stream_info(probeCtx, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        DLL_Log("[VideoEncoder] WARNING: Post-mux duration probe failed stream info for '%s': %s", filename.c_str(),
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
        DLL_Log("[VideoEncoder] post_mux_probe_cancelled file='%s' packets=%d", filename.c_str(), packetsRead);
        avformat_close_input(&probeCtx);
        return false;
    }
    if (packetsRead >= kPostMuxProbeMaxPackets) {
        DLL_Log("[VideoEncoder] post_mux_probe_packet_limit file='%s' packets=%d", filename.c_str(), packetsRead);
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
                filename.c_str(), ret, tailPacketsRead, kPostMuxProbeMaxTailPackets);
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
                                    uint64_t timeoutMs = kPostMuxProbeTimeoutMs) {
    if (filename.empty() || finalDurationUs <= 0) {
        return;
    }

    PostMuxProbeControl control;
    control.deadlineTickMs = GetTickCount64() + std::max<uint64_t>(1, timeoutMs);
    const uint64_t startMs = GetTickCount64();
    DLL_Log("[VideoEncoder] post_mux_probe_start file='%s' target=%lld timeout=%llums", filename.c_str(),
            (long long)finalDurationUs, (unsigned long long)timeoutMs);
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

}  // namespace

// D3D11 exception safety for MinGW/clang
// MinGW uses DWARF exception handling (libgcc) which cannot catch Windows SEH
// exceptions. D3D11 raises SEH exceptions (e.g., 0xE06D7363) for invalid handles.
//
// Protection strategy:
// 1. HandleFailureCache tracks handles that have failed - skip them on retry
// 2. DuplicateHandle-first validates handles before calling D3D11
// 3. dllexport wrapper functions prevent LTO from stripping error paths

#include <windows.h>

// Handle validation cache: tracks handles that have previously failed D3D11 OpenShared*
// calls, so we don't repeatedly trigger SEH exceptions from invalid handles.
// D3D11 throws SEH exceptions for invalid handles, and MinGW's catch(...) cannot
// catch SEH exceptions. Pre-validation is the only reliable protection.
//
// The cache stores (handle_value, failure_count) pairs. Handles that fail >3 times
// are permanently skipped. Cache is cleared when recording starts.
#include <mutex>
#include <unordered_map>

struct HandleFailureCache {
    std::mutex mutex;
    std::unordered_map<HANDLE, int> fenceFailures;
    std::unordered_map<HANDLE, int> textureFailures;

    bool ShouldSkipFence(HANDLE h) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = fenceFailures.find(h);
        return it != fenceFailures.end() && it->second >= 3;
    }

    bool ShouldSkipTexture(HANDLE h) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = textureFailures.find(h);
        return it != textureFailures.end() && it->second >= 3;
    }

    void RecordFenceFailure(HANDLE h) {
        std::lock_guard<std::mutex> lock(mutex);
        fenceFailures[h]++;
    }

    void RecordTextureFailure(HANDLE h) {
        std::lock_guard<std::mutex> lock(mutex);
        textureFailures[h]++;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex);
        fenceFailures.clear();
        textureFailures.clear();
    }
};

static HandleFailureCache g_HandleFailureCache;

// D3D11 OpenSharedFence/OpenSharedResource can throw SEH exceptions (0xE06D7363)
// for invalid handles. On MinGW, C++ try/catch cannot catch SEH exceptions.
// We use the VEH handler above for diagnostics. The real protection is to validate
// handles BEFORE calling D3D11 APIs - check if the handle is a valid NT handle,
// check if the source process is still alive, and cache results to avoid repeated
// failures that would trigger exceptions.
// for invalid handles. LTO (-flto) was stripping exception tables from lambdas.
// Fixed by: (1) disabling LTO for mediaengine in build.py, (2) using noinline wrappers.
// The noinline wrappers force exception table emission even if LTO is re-enabled later.

// Forward declarations - dllexport forces LTO to keep these functions
// D3D11 OpenShared* calls can throw SEH exceptions (0xE06D7363) for invalid handles.
// On MinGW, catch(...) CANNOT catch SEH exceptions. The failure cache pre-validates
// handles that have previously failed to prevent repeated crashes.
// CRITICAL: OpenSharedFence MUST only be called with handles that are known to be valid.
// The caller must use DuplicateHandle first - if it succeeds, the handle is valid.
// We also use the failure cache as a second line of defense.
extern "C" __declspec(dllexport) HRESULT __cdecl CallOpenSharedFence(ID3D11Device5* dev, HANDLE h, ID3D11Fence** out) {
    if (g_HandleFailureCache.ShouldSkipFence(h)) {
        return E_INVALIDARG;
    }
    // CRITICAL: MinGW catch(...) CANNOT catch D3D11's SEH exceptions (0xE06D7363).
    // D3D11's OpenSharedFence calls __fastfail on invalid handles, killing the process.
    // We use DuplicateHandle to validate the handle BEFORE calling D3D11.
    // If DuplicateHandle fails, the handle is invalid and we skip the call entirely.
    // If it succeeds, the handle is valid in this process and D3D11 should accept it.
    if (h == NULL || h == INVALID_HANDLE_VALUE) {
        return E_INVALIDARG;
    }

    HRESULT hr = E_FAIL;
    try {
        hr = dev->OpenSharedFence(h, IID_PPV_ARGS(out));
