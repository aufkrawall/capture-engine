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

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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
    return static_cast<int64_t>(totalSeconds * 1000000.0 + 0.5);
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
    } catch (...) {
        hr = E_FAIL;
    }
    if (FAILED(hr)) {
        g_HandleFailureCache.RecordFenceFailure(h);
    }
    return hr;
}

extern "C" __declspec(dllexport) HRESULT __cdecl CallOpenSharedResource(ID3D11Device5* dev, HANDLE h, REFIID riid,
                                                                        void** out) {
    if (h == NULL || h == INVALID_HANDLE_VALUE) {
        return E_INVALIDARG;
    }
    HRESULT hr = E_FAIL;
    try {
        hr = dev->OpenSharedResource(h, riid, out);
    } catch (...) {
        hr = E_FAIL;
    }
    if (FAILED(hr)) {
        g_HandleFailureCache.RecordTextureFailure(h);
    }
    return hr;
}

extern "C" __declspec(dllexport) HRESULT __cdecl CallOpenSharedResource1(ID3D11Device5* dev, HANDLE h, REFIID riid,
                                                                         void** out) {
    if (h == NULL || h == INVALID_HANDLE_VALUE) {
        return E_INVALIDARG;
    }
    HRESULT hr = E_FAIL;
    try {
        hr = dev->OpenSharedResource1(h, riid, out);
    } catch (...) {
        hr = E_FAIL;
    }
    if (FAILED(hr)) {
        g_HandleFailureCache.RecordTextureFailure(h);
    }
    return hr;
}

namespace fs = std::filesystem;

#ifndef D3D11_FORMAT_SUPPORT_SHAREABLE
#define D3D11_FORMAT_SUPPORT_SHAREABLE 0x2000
#endif

namespace {
enum class OutputRangeMode { kLimited, kFull };

template <typename AtomicT>
void UpdateAtomicPeak(AtomicT& peak, uint32_t value) {
    uint32_t current = peak.load(std::memory_order_relaxed);
    while (value > current &&
           !peak.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

uint32_t SaturatingToUint32(uint64_t value) {
    return value > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(value);
}

struct ResolvedVideoFormat {
    AVPixelFormat codecPixFmt = AV_PIX_FMT_NONE;
    AVPixelFormat d3d11SwFormat = AV_PIX_FMT_NONE;
    DXGI_FORMAT directDxgiFormat = DXGI_FORMAT_UNKNOWN;
    std::string bitDepth;
    std::string chroma;
    bool use10Bit = false;
    bool usesVideoProcessor = true;
    bool requiresEvenDimensions = true;
};

const char* GetPixFmtNameSafe(AVPixelFormat pixFmt) {
    const char* name = av_get_pix_fmt_name(pixFmt);
    return name ? name : "unknown";
}

bool SupportsCodecPixelFormat(const AVCodec* codec, AVPixelFormat pixFmt) {
    if (!codec) {
        return false;
    }

#if LIBAVCODEC_VERSION_MAJOR >= 62
    const void* configs = nullptr;
    int numConfigs = 0;
    const int ret = avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &numConfigs);
    if (ret < 0) {
        return false;
    }
    if (!configs) {
        return true;
    }

    const auto* formats = static_cast<const AVPixelFormat*>(configs);
    for (int i = 0; i < numConfigs; ++i) {
        if (formats[i] == pixFmt) {
            return true;
        }
    }
    return false;
#else
    if (!codec->pix_fmts) {
        return true;
    }

    for (const AVPixelFormat* fmt = codec->pix_fmts; *fmt != AV_PIX_FMT_NONE; ++fmt) {
        if (*fmt == pixFmt) {
            return true;
        }
    }
    return false;
#endif
}

bool SupportsD3D11HwInputFormat(const AVCodec* codec, AVPixelFormat swFormat) {
    return SupportsCodecPixelFormat(codec, AV_PIX_FMT_D3D11) && SupportsCodecPixelFormat(codec, swFormat);
}

bool DeviceSupportsHwFrameSwFormat(AVBufferRef* deviceCtx, AVPixelFormat swFormat) {
    if (!deviceCtx) {
        return false;
    }

    AVHWFramesConstraints* constraints = av_hwdevice_get_hwframe_constraints(deviceCtx, nullptr);
    if (!constraints) {
        return false;
    }

    bool supported = true;
    if (constraints->valid_sw_formats) {
        supported = false;
        for (const AVPixelFormat* fmt = constraints->valid_sw_formats; *fmt != AV_PIX_FMT_NONE; ++fmt) {
            if (*fmt == swFormat) {
                supported = true;
                break;
            }
        }
    }

    av_hwframe_constraints_free(&constraints);
    return supported;
}

bool IsDirectRgbD3D11SwFormat(AVPixelFormat swFormat) {
    return swFormat == AV_PIX_FMT_BGRA || swFormat == AV_PIX_FMT_X2BGR10;
}

std::string ResolveRequestedBitDepth(const VideoConfig& config, bool prefer10Bit) {
    if (config.bitDepth.empty() || _stricmp(config.bitDepth.c_str(), "auto") == 0) {
        return prefer10Bit ? "10" : "8";
    }
    return config.bitDepth;
}

std::string ResolveRequestedChroma(const VideoConfig& config) {
    if (config.chromaSubsampling.empty() || _stricmp(config.chromaSubsampling.c_str(), "auto") == 0) {
        return "420";
    }
    return config.chromaSubsampling;
}

bool ResolveVideoFormat(const VideoConfig& config, bool isHDR, bool prefer10Bit, const AVCodec* codec,
                        ResolvedVideoFormat* out, std::string* error, std::string* warning) {
    if (!out) {
        if (error) {
            *error = "[VideoEncoder] Internal error: missing format resolution output";
        }
        return false;
    }

    ResolvedVideoFormat resolved;
    resolved.bitDepth = ResolveRequestedBitDepth(config, prefer10Bit);
    resolved.chroma = ResolveRequestedChroma(config);
    resolved.use10Bit = (_stricmp(resolved.bitDepth.c_str(), "10") == 0);
    if (!ce::video_format::IsOutputBitDepthCompatibleWithHdr(isHDR, resolved.use10Bit)) {
        if (error) {
            *error =
                "[VideoEncoder] HDR capture requires bit_depth=auto or 10; an 8-bit output cannot preserve the "
                "BT.2020/PQ contract";
        }
        return false;
    }

    if (_stricmp(resolved.chroma.c_str(), "420") == 0) {
        resolved.chroma = "420";
        resolved.codecPixFmt = AV_PIX_FMT_D3D11;
        resolved.d3d11SwFormat = resolved.use10Bit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
        resolved.directDxgiFormat = DXGI_FORMAT_UNKNOWN;
        resolved.usesVideoProcessor = true;
        resolved.requiresEvenDimensions = true;
        *out = resolved;
        return true;
    }

    if (_stricmp(resolved.chroma.c_str(), "422") == 0) {
        if (error) {
            *error = "[VideoEncoder] chroma_subsampling=422 is not supported by the current D3D11 capture pipeline";
        }
        return false;
    }

    if (_stricmp(resolved.chroma.c_str(), "444") == 0) {
        if (error) {
            *error =
                "[VideoEncoder] chroma_subsampling=444 is not supported yet by the current capture pipeline; "
                "the shipped FFmpeg/NVENC path cannot produce correct true 4:4:4 output here";
        }
        return false;
    }

    if (error) {
        *error = "[VideoEncoder] Unsupported chroma_subsampling value in video config";
    }
    return false;
}

bool WantsFullOutputRange(const std::string& colorRange) {
    return !colorRange.empty() && _stricmp(colorRange.c_str(), "full") == 0;
}

OutputRangeMode GetEffectiveOutputRange(const std::string& colorRange, bool isHDR) {
    if (WantsFullOutputRange(colorRange) && !isHDR) {
        return OutputRangeMode::kFull;
    }
    return OutputRangeMode::kLimited;
}

AVColorRange GetAVColorRange(OutputRangeMode range) {
    return range == OutputRangeMode::kFull ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
}

const char* DescribeOutputRange(OutputRangeMode range) {
    return range == OutputRangeMode::kFull ? "full" : "limited";
}

void ApplyFrameColorMetadata(AVFrame* frame, const AVCodecContext* codec) {
    if (!frame || !codec) {
        return;
    }

    frame->color_range = codec->color_range;
    frame->color_primaries = codec->color_primaries;
    frame->color_trc = codec->color_trc;
    frame->colorspace = codec->colorspace;
    frame->chroma_location = codec->chroma_sample_location;
}

DXGI_COLOR_SPACE_TYPE GetVideoProcessorInputColorSpace(DXGI_FORMAT format, bool isHDR, bool forceLinear = false) {
    if (forceLinear || ce::video_format::IsFp16RgbInputFormat(format)) {
        return DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    }
    if (isHDR) {
        return DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    }
    if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    }
    return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}

DXGI_COLOR_SPACE_TYPE GetVideoProcessorOutputColorSpace(bool use10Bit, bool isHDR, const std::string& colorSpace,
                                                        OutputRangeMode outputRange) {
    if (isHDR) {
        return DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
    }
    if (use10Bit) {
        if (colorSpace == "bt2020") {
            return outputRange == OutputRangeMode::kFull ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020
                                                         : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
        }
        return outputRange == OutputRangeMode::kFull ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                                                     : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    }
    if (colorSpace == "bt2020") {
        return outputRange == OutputRangeMode::kFull ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020
                                                     : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
    }
    return outputRange == OutputRangeMode::kFull ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                                                 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
}

bool QuerySdrWhiteLevelNits(HMONITOR monitor, float* nits, ULONG* rawLevel) {
    if (!monitor || !nits || !rawLevel) {
        return false;
    }

    MONITORINFOEXW monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        return false;
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        LONG result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (result != ERROR_SUCCESS || pathCount == 0) {
            return false;
        }

        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }
        if (result != ERROR_SUCCESS) {
            return false;
        }

        paths.resize(pathCount);
        for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
            sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sourceName.header.size = sizeof(sourceName);
            sourceName.header.adapterId = path.sourceInfo.adapterId;
            sourceName.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS ||
                lstrcmpiW(sourceName.viewGdiDeviceName, monitorInfo.szDevice) != 0) {
                continue;
            }

            DISPLAYCONFIG_SDR_WHITE_LEVEL whiteLevel = {};
            whiteLevel.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            whiteLevel.header.size = sizeof(whiteLevel);
            whiteLevel.header.adapterId = path.targetInfo.adapterId;
            whiteLevel.header.id = path.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&whiteLevel.header) != ERROR_SUCCESS || whiteLevel.SDRWhiteLevel == 0) {
                return false;
            }

            *rawLevel = whiteLevel.SDRWhiteLevel;
            *nits = static_cast<float>(whiteLevel.SDRWhiteLevel) * (80.0f / 1000.0f);
            return true;
        }
        return false;
    }
    return false;
}
}  // namespace

static HANDLE NormalizeSourceHandleForWow64(HANDLE handle, uint32_t sourcePid) {
#if defined(_WIN64)
    if (!handle || sourcePid == 0) {
        return handle;
    }

    static std::mutex s_bitnessMutex;
    static std::unordered_map<uint32_t, bool> s_isWow64ByPid;

    bool isWow64Source = false;
    {
        std::lock_guard<std::mutex> lock(s_bitnessMutex);
        auto it = s_isWow64ByPid.find(sourcePid);
        if (it != s_isWow64ByPid.end()) {
            isWow64Source = it->second;
        } else {
            ce::HandleGuard hProcess(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, sourcePid));
            if (hProcess) {
                BOOL wow64 = FALSE;
                if (IsWow64Process(hProcess.get(), &wow64)) {
                    isWow64Source = (wow64 == TRUE);
                }
            }
            s_isWow64ByPid[sourcePid] = isWow64Source;
        }
    }

    const uint64_t rawHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
    if (!isWow64Source) {
        // Some drivers publish KMT handles with bit31 set but without canonical
        // sign-extension in 64-bit IPC transport.
        if ((rawHandle >> 32) == 0 && (rawHandle & 0x80000000ull) != 0) {
            const int64_t signExtended = static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(rawHandle)));
            if (signExtended != static_cast<int64_t>(rawHandle)) {
                static std::atomic<int> s_canonicalizeLogCount{0};
                if (s_canonicalizeLogCount.fetch_add(1, std::memory_order_relaxed) < 6) {
                    DLL_Log("[VideoEncoder] Canonicalizing shared handle for PID %u: %p -> %p", sourcePid,
                            (HANDLE)(uintptr_t)rawHandle, (HANDLE)(uint64_t)signExtended);
                }
                return reinterpret_cast<HANDLE>(static_cast<uint64_t>(signExtended));
            }
        }
        return handle;
    }

    const int64_t signExtended = static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(rawHandle)));
    if (signExtended != static_cast<int64_t>(rawHandle)) {
        static std::atomic<int> s_normalizeLogCount{0};
        if (s_normalizeLogCount.fetch_add(1, std::memory_order_relaxed) < 6) {
            DLL_Log("[VideoEncoder] WOW64 handle normalized for PID %u: %p -> %p", sourcePid,
                    (HANDLE)(uintptr_t)rawHandle, (HANDLE)(uint64_t)signExtended);
        }
    }
    return reinterpret_cast<HANDLE>(static_cast<uint64_t>(signExtended));
#else
    (void)sourcePid;
    return handle;
#endif
}

static ce::capture_output::ReservedCaptureOutput ReserveOutputStagingFile(const VideoConfig& config) {
    const fs::path exeDir = ce::capture_output::GetExecutableDirectory();
    const fs::path outDir = ce::capture_output::ResolveCaptureDirectory(config.outputDir, exeDir);
    auto reservation = ce::capture_output::ReservedCaptureOutput::Reserve(outDir, L"capture_stage", L"part");
    if (reservation) {
        DLL_Log("[VideoEncoder] Reserved unpublished staging output: %s", reservation.Utf8Path().c_str());
    } else {
        DLL_Log("[VideoEncoder] ERROR: Could not reserve a collision-safe staging output in: %s",
                outDir.string().c_str());
    }
    return reservation;
}

static int AllocateOutputContextForContainer(AVFormatContext** formatContext, const VideoConfig& config) {
    const std::string formatHint = "capture." + config.container;
    return avformat_alloc_output_context2(formatContext, nullptr, nullptr, formatHint.c_str());
}

// RAII Wrapper for MediaEngine D3D11 Guard
class D3D11ScopedLock {
public:
    D3D11ScopedLock() {
        MediaEngine_LockD3D11();
    }
    ~D3D11ScopedLock() {
        MediaEngine_UnlockD3D11();
    }
};

// Performance timing helper for pipeline analysis
class PerfTimer {
public:
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;

    static TimePoint now() {
        return Clock::now();
    }

    static double elapsed_ms(const TimePoint& start, const TimePoint& end) {
        return std::chrono::duration<double, std::milli>(end - start).count();
    }
};

// Frame statistics for performance monitoring
struct FrameStats {
    int64_t frameNumber = 0;
    int64_t ptsMs = 0;
    double fenceWaitMs = 0;
    double textureOpenMs = 0;
    double colorConvertMs = 0;
    double encodeMs = 0;
    double totalMs = 0;
    int packetsProduced = 0;
    int64_t expectedPtsDiff = 0;  // Expected ms between frames
    int64_t actualPtsDiff = 0;    // Actual ms between frames
};

static int64_t RoundUsToMs(int64_t valueUs) {
    if (valueUs >= 0) {
        return (valueUs + 500) / 1000;
    }
    return (valueUs - 500) / 1000;
}

// Global stats for frame analysis
static int64_t g_lastFramePts = -1;
static int64_t g_framesEncoded = 0;
// static int64_t g_framesDropped = 0;
static double g_totalFenceWait = 0;
static double g_totalColorConvert = 0;
static double g_totalEncode = 0;
static double g_maxFrameTime = 0;
static int g_slowFrameCount = 0;  // Frames taking > 2x expected time

static void FreeScopedAvFrame(AVFrame** frame) {
    if (frame && *frame) {
        av_frame_free(frame);
    }
}

VideoEncoder::VideoEncoder()
    : fmtCtx(nullptr),
      codecCtx(nullptr),
      stream(nullptr),
      hwDeviceCtx(nullptr),
      hwFramesCtx(nullptr),
      d3d11DeviceCtx(nullptr),
      d3d11FramesCtx(nullptr),
      d3d11Device(nullptr),
      d3d11Context(nullptr),
      luidLow(0),
      luidHigh(0),
      initDone(false),
      currentIsHDR(false),
      currentUse10BitInput(false),
      fileOpened(false),
      recordingRequested(false),
      isStopping(false),
      flushRequested(false),
      codecOpenFailed(false),
      startPts(-1),
      width(0),
      height(0),
      cachedSourcePid(0),
      lastEncodeTimeUs(0),
      fenceEvent(nullptr),
      videoDevice(nullptr),
      videoContext(nullptr),
      videoProcessor(nullptr),
      videoProcessorEnum(nullptr),
      inputView(nullptr),
      videoProcessorInit(false) {}

VideoEncoder::~VideoEncoder() {
    Stop();  // Triiger async stop

    // Destructor MUST be synchronous to ensure no threads are running
    // and all resources are safely released.
    if (writerThread.joinable()) {
        DLL_Log("[VideoEncoder] Destructor: Waiting for async writer to finish...");
        writerThread.join();
    }

    if (fenceEvent) {
        CloseHandle(fenceEvent);
        fenceEvent = nullptr;
    }
}

bool VideoEncoder::ShouldEncodeHdrOutput() const {
    return ce::video_format::ShouldEncodeHdrOutput(currentIsHDR, savedConfig.colorSpace);
}

void VideoEncoder::UpdateSdrWhiteLevelForCaptureArea(int captureOriginX, int captureOriginY, UINT captureWidth,
                                                     UINT captureHeight) {
    if (!currentIsHDR) {
        return;
    }

    POINT center = {captureOriginX + static_cast<LONG>(captureWidth / 2),
                    captureOriginY + static_cast<LONG>(captureHeight / 2)};
    HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    if (!monitor || monitor == sdrWhiteMonitor) {
        return;
    }

    sdrWhiteMonitor = monitor;
    float queriedNits = 0.0f;
    ULONG rawLevel = 0;
    if (QuerySdrWhiteLevelNits(monitor, &queriedNits, &rawLevel)) {
        sdrWhiteNits = std::clamp(queriedNits, 80.0f, 1000.0f);
        DLL_Log("[HDR Color] Windows SDR white level: raw=%lu nits=%.1f captureCenter=(%ld,%ld)", rawLevel,
                sdrWhiteNits, center.x, center.y);
    } else {
        sdrWhiteNits = 203.0f;
        DLL_Log(
            "[HDR Color] Windows SDR white-level query unavailable; using %.1f-nit fallback for captureCenter=(%ld,%ld)",
            sdrWhiteNits, center.x, center.y);
    }
}

void VideoEncoder::ApplyGpuThreadPriority(int priority, const char* reason) {
    if (!d3d11Device) {
        return;
    }

    priority = std::clamp(priority, -7, 7);
    if (priority == currentGpuThreadPriority && reason && std::strcmp(reason, "initial") != 0) {
        return;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    if (SUCCEEDED(d3d11Device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice)) && dxgiDevice) {
        HRESULT phr = dxgiDevice->SetGPUThreadPriority(priority);
        if (SUCCEEDED(phr)) {
            INT actual = 0;
            const HRESULT readbackHr = dxgiDevice->GetGPUThreadPriority(&actual);
            if (SUCCEEDED(readbackHr) && actual == priority) {
                currentGpuThreadPriority = priority;
                DLL_Log("[VideoEncoder] GPU Thread Priority requested=%d actual=%d verified=1 (%s)", priority, actual,
                        reason ? reason : "update");
            } else {
                DLL_Log(
                    "[VideoEncoder] GPU Thread Priority readback mismatch requested=%d actual=%d setHr=%x "
                    "readbackHr=%x verified=0 (%s)",
                    priority, actual, phr, readbackHr, reason ? reason : "update");
            }
        } else {
            DLL_Log("[VideoEncoder] Failed to set GPU Thread Priority %d (%s): HR=%x", priority,
                    reason ? reason : "update", phr);
        }
        dxgiDevice->Release();
    }
}

void VideoEncoder::UpdateAdaptiveGpuThreadPriority(uint64_t nowMs, double encodeMs, bool encoderPressureActive) {
    if (gpuPriority != 0 || savedConfig.fps <= 0 || !d3d11Device) {
        return;
    }

    const double frameIntervalMs = 1000.0 / static_cast<double>(savedConfig.fps);
    if (ce::capture_policy::IsAdaptiveEncoderGpuPriorityPressureActive(encodeMs, frameIntervalMs,
                                                                       encoderPressureActive)) {
        if (gpuPriorityPressureSinceMs == 0) {
            gpuPriorityPressureSinceMs = nowMs;
            DLL_Log("[VideoEncoder] Adaptive GPU priority pressure started: encode=%.2fms budget=%.2fms flag=%d",
                    encodeMs, frameIntervalMs, encoderPressureActive ? 1 : 0);
        }
        gpuPriorityHealthySinceMs = 0;
        if (currentGpuThreadPriority < 1 && nowMs - gpuPriorityPressureSinceMs >= 2000) {
            ApplyGpuThreadPriority(1, "adaptive encoder pressure");
        }
        return;
    }

    if (ce::capture_policy::ShouldResetAdaptiveEncoderGpuPriorityPressure(encodeMs, frameIntervalMs,
                                                                          encoderPressureActive)) {
        gpuPriorityPressureSinceMs = 0;
        if (gpuPriorityHealthySinceMs == 0) {
            gpuPriorityHealthySinceMs = nowMs;
        }
        if (currentGpuThreadPriority != 0 && nowMs - gpuPriorityHealthySinceMs >= 5000) {
            ApplyGpuThreadPriority(0, "adaptive encoder recovered");
        }
    } else {
        gpuPriorityHealthySinceMs = 0;
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
        const bool removed = outputReservation.CleanupOwnedFile();
        DLL_Log(
            "[VideoEncoder] ERROR: output_publish_failed error=%lu durationUs=%lld videoPackets=%llu removed=%d "
            "staging='%s'",
            publishError, static_cast<long long>(finalDurationUs), static_cast<unsigned long long>(writtenVideoPackets),
            removed ? 1 : 0, outputFilename.c_str());
        return false;
    }

    outputFilename = outputReservation.Utf8Path();
    DLL_Log("[VideoEncoder] output_published file='%s' durationUs=%lld videoPackets=%llu", outputFilename.c_str(),
            static_cast<long long>(finalDurationUs), static_cast<unsigned long long>(writtenVideoPackets));
    return true;
}

bool VideoEncoder::Init(const VideoConfig& config, int width, int height, int fps,
                        std::function<void(AVPacket*)> packetCallback) {
    // Clear handle failure cache from previous recording session
    g_HandleFailureCache.Clear();
    DLL_Log("[VideoEncoder] Init Entry - config.encoder=%s w=%d h=%d fps=%d", config.encoder.c_str(), width, height,
            fps);

    // Disable buffering to see logs immediately
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    DLL_Log("[VideoEncoder] Step 1: Setting member variables");
    this->width = width;
    this->height = height;
    this->captureCursor = config.captureCursor;
    this->gpuPriority = config.gpuPriority;
    this->onPacket = packetCallback;

    // Initialize cursor renderer if cursor capture enabled
    if (captureCursor) {
        cursorRenderer = std::make_unique<CursorRenderer>();
        DLL_Log("[VideoEncoder] Cursor capture enabled (renderer created)");
    }

    DLL_Log("[VideoEncoder] Step 2: Setting av_log level");
    av_log_set_level(AV_LOG_WARNING);

    DLL_Log("[VideoEncoder] Step 3: Deferring staging output reservation until recording start");
    outputReservation.CleanupOwnedFile();
    outputFilename.clear();

    DLL_Log("[VideoEncoder] Step 4: Calling avformat_alloc_output_context2");
    if (AllocateOutputContextForContainer(&fmtCtx, config) < 0) {
        DLL_Log("[VideoEncoder] Failed to alloc output context");
        return false;
    }
    DLL_Log("[VideoEncoder] Step 4 done, fmtCtx=%p", (void*)fmtCtx);

    DLL_Log("[VideoEncoder] Step 5: Finding encoder: %s", config.encoder.c_str());
    const AVCodec* codec = avcodec_find_encoder_by_name(config.encoder.c_str());
    if (!codec) {
        DLL_Log("[VideoEncoder] Codec not found: %s", config.encoder.c_str());
        return false;
    }
    DLL_Log("[VideoEncoder] Step 5 done, codec=%p name=%s", (void*)codec, codec->name);

    DLL_Log("[VideoEncoder] Step 6: Allocating codec context");
    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        DLL_Log("[VideoEncoder] Failed to alloc codec context");
        return false;
    }
    DLL_Log("[VideoEncoder] Step 6 done, codecCtx=%p", (void*)codecCtx);

    // Store config for use in EnsureDevice()
    savedConfig = config;

    DLL_Log("[VideoEncoder] Init Complete - returning true");
    // Defer device creation to EnsureDevice()
    return true;
}

void VideoEncoder::SetAdapterLUID(int32_t low, int32_t high) {
    this->luidLow = low;
    this->luidHigh = high;
}

void VideoEncoder::SetDimensions(uint32_t w, uint32_t h) {
    if (w > 0 && h > 0) {
        this->width = w;
        this->height = h;
        DLL_Log("[VideoEncoder] SetDimensions: %dx%d", w, h);
    }
}

bool VideoEncoder::AdoptTextureDevice(ID3D11Texture2D* texture) {
    if (!texture) {
        return false;
    }

    ID3D11Device* texDevice = nullptr;
    texture->GetDevice(&texDevice);
    if (!texDevice) {
        DLL_Log("[VideoEncoder] Framegrab: Failed to get D3D11 device from texture");
        return false;
    }

    ID3D11Device5* adoptedDevice = nullptr;
    HRESULT hr = texDevice->QueryInterface(__uuidof(ID3D11Device5), (void**)&adoptedDevice);
    if (FAILED(hr) || !adoptedDevice) {
        DLL_Log("[VideoEncoder] Framegrab: Failed to query ID3D11Device5 from texture device. HR=%x", hr);
        texDevice->Release();
        return false;
    }

    ID3D11DeviceContext* immediateContext = nullptr;
    texDevice->GetImmediateContext(&immediateContext);
    ID3D11DeviceContext4* adoptedContext = nullptr;
    if (immediateContext) {
        hr = immediateContext->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&adoptedContext);
        immediateContext->Release();
    } else {
        hr = E_NOINTERFACE;
    }
    texDevice->Release();

    if (FAILED(hr) || !adoptedContext) {
        DLL_Log("[VideoEncoder] Framegrab: Failed to query ID3D11DeviceContext4 from texture device. HR=%x", hr);
        adoptedDevice->Release();
        return false;
    }

    if (d3d11Context) {
        d3d11Context->Release();
    }
    if (d3d11Device) {
        d3d11Device->Release();
    }

    d3d11Device = adoptedDevice;
    d3d11Context = adoptedContext;
    return true;
}

void VideoEncoder::ReleaseInjectDeviceStateForScreenGrab() {
    const bool hadInjectLuid = (luidLow != 0 || luidHigh != 0);
    const bool hadSharedCapture = sharedCaptureTexturesCreated;
    if (!hadInjectLuid && !hadSharedCapture) {
        return;
    }

    DLL_Log("[VideoEncoder] ScreenGrab: Releasing inject device state (luid=%08x %08x shared=%d)", luidLow, luidHigh,
            hadSharedCapture ? 1 : 0);
    luidLow = 0;
    luidHigh = 0;

    if (pSharedMem) {
        pSharedMem->useEncoderTextures.store(false, std::memory_order_release);
        pSharedMem->encoderTextures.ready.store(false, std::memory_order_release);
        pSharedMem->encoderTextures.kmtReady.store(false, std::memory_order_release);
    }

    if (hadSharedCapture) {
        ReleasePreservedEncoderTextures();
        return;
    }

    for (int i = 0; i < SHARED_TEXTURE_SLOT_COUNT; i++) {
        if (cachedSharedTextures[i]) {
            cachedSharedTextures[i]->Release();
            cachedSharedTextures[i] = nullptr;
        }
        cachedTextureHandles[i] = nullptr;
    }

    if (cachedD3D11Fence) {
        cachedD3D11Fence->Release();
        cachedD3D11Fence = nullptr;
    }
    cachedFenceHandle = nullptr;
    cachedSourcePid = 0;

    if (bgraStagingTexture) {
        bgraStagingTexture->Release();
        bgraStagingTexture = nullptr;
    }

    CleanupVideoProcessor();
    if (cursorRenderer) {
        cursorRenderer->Cleanup();
    }

    TrimD3D11Residency(d3d11Device, d3d11Context, "screen-grab-switch");
    if (d3d11Context) {
        d3d11Context->Release();
        d3d11Context = nullptr;
    }
    if (d3d11Device) {
        d3d11Device->Release();
        d3d11Device = nullptr;
    }
    if (d3d11DeviceCtx) {
        av_buffer_unref(&d3d11DeviceCtx);
    }
    if (d3d11FramesCtx) {
        av_buffer_unref(&d3d11FramesCtx);
    }
    initDone = false;
}

AVPixelFormat VideoEncoder::GetActiveD3D11SwFormat() const {
    if (!d3d11FramesCtx) {
        return AV_PIX_FMT_NONE;
    }

    const auto* framesCtx = reinterpret_cast<const AVHWFramesContext*>(d3d11FramesCtx->data);
    if (!framesCtx) {
        return AV_PIX_FMT_NONE;
    }
    return framesCtx->sw_format;
}

bool VideoEncoder::PrepareD3D11TextureForEncode(ID3D11Texture2D* srcTexture, ID3D11Texture2D* dstTexture,
                                                bool overlayCursor, int captureOriginX, int captureOriginY,
                                                bool allowCursorHandleVisibilityFallback,
                                                uint64_t keyedMutexAcquireKey) {
    if (!srcTexture || !dstTexture || !d3d11Device || !d3d11Context) {
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc = {};
    srcTexture->GetDesc(&srcDesc);
    UpdateSdrWhiteLevelForCaptureArea(captureOriginX, captureOriginY, srcDesc.Width, srcDesc.Height);
    if (currentIsHDR && !ce::video_format::IsFp16RgbInputFormat(srcDesc.Format) &&
        !ce::video_format::IsHdr10RgbInputFormat(srcDesc.Format)) {
        DLL_Log("[HDR Color] Direct encode path refuses unsupported HDR source format %d", srcDesc.Format);
        return false;
    }

    struct KeyedMutexGuard {
        IDXGIKeyedMutex* mutex = nullptr;
        bool acquired = false;

        ~KeyedMutexGuard() {
            if (!mutex) {
                return;
            }
            if (acquired) {
                mutex->ReleaseSync(0);
            }
            mutex->Release();
        }
    } keyedMutexGuard;

    srcTexture->QueryInterface(IID_PPV_ARGS(&keyedMutexGuard.mutex));
    if (keyedMutexGuard.mutex) {
        const HRESULT kmHr = keyedMutexGuard.mutex->AcquireSync(keyedMutexAcquireKey, 1000);
        if (kmHr != S_OK) {
            DLL_Log("[VideoEncoder] Direct D3D11 encode path could not acquire keyed mutex: HR=%x", kmHr);
            keyedMutexGuard.mutex->Release();
            keyedMutexGuard.mutex = nullptr;
            return false;
        }
        keyedMutexGuard.acquired = true;
    }

    const DXGI_FORMAT inputSrvFormat = ce::video_format::GetRgbShaderResourceViewFormat(srcDesc.Format);
    if (inputSrvFormat == DXGI_FORMAT_UNKNOWN) {
        DLL_Log("[VideoEncoder] Direct D3D11 encode path does not support source format %d", srcDesc.Format);
        return false;
    }
    const ce::video_format::RgbColorTransform colorTransform =
        ce::video_format::GetRgbColorTransform(srcDesc.Format, currentIsHDR, ShouldEncodeHdrOutput());

    ID3D11Texture2D* srvSourceTexture = srcTexture;
    ID3D11Texture2D* srvCompatTexture = nullptr;
    if ((srcDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
        D3D11_TEXTURE2D_DESC srvDesc = srcDesc;
        srvDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        srvDesc.MiscFlags = 0;
        srvDesc.CPUAccessFlags = 0;
        srvDesc.Usage = D3D11_USAGE_DEFAULT;

        HRESULT hr = d3d11Device->CreateTexture2D(&srvDesc, nullptr, &srvCompatTexture);
        if (FAILED(hr)) {
            DLL_Log("[VideoEncoder] Failed to create SRV-compatible staging texture: HR=%x", hr);
            return false;
        }
        d3d11Context->CopyResource(srvCompatTexture, srcTexture);
        srvSourceTexture = srvCompatTexture;
    }

    D3D11_TEXTURE2D_DESC dstDesc = {};
    dstTexture->GetDesc(&dstDesc);

    ID3D11Texture2D* normalizedTexture = nullptr;
    if (dstDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        normalizedTexture = RenderFullscreenCopy(srvSourceTexture, dstDesc.Width, dstDesc.Height, inputSrvFormat,
                                                 DXGI_FORMAT_B8G8R8A8_UNORM, swapRBTexture, swapRBTextureRTV,
                                                 swapRBTexWidth, swapRBTexHeight, "RGB444-BGRA", colorTransform,
                                                 sdrWhiteNits);
    } else if (dstDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        normalizedTexture =
            RenderFullscreenCopy(srvSourceTexture, dstDesc.Width, dstDesc.Height, inputSrvFormat,
                                 DXGI_FORMAT_R10G10B10A2_UNORM, rgb10IntermediateTexture, rgb10IntermediateRTV,
                                 rgb10IntermediateWidth, rgb10IntermediateHeight, "RGB444-RGB10", colorTransform,
                                 sdrWhiteNits);
    } else {
        DLL_Log("[VideoEncoder] Direct D3D11 encode path encountered unsupported destination format %d",
                dstDesc.Format);
    }

    if (srvCompatTexture) {
        srvCompatTexture->Release();
    }
    if (!normalizedTexture) {
        return false;
    }

    if (overlayCursor && captureCursor && cursorRenderer) {
        if (!cursorRenderer->Init(d3d11Device, d3d11Context)) {
            static bool cursorInitLogged = false;
            if (!cursorInitLogged) {
                DLL_Log("[VideoEncoder] Failed to initialize cursor renderer for direct D3D11 encode path");
                cursorInitLogged = true;
            }
        } else {
            const CursorColorMode cursorColorMode =
                ShouldEncodeHdrOutput() && dstDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ? CursorColorMode::Hdr10Pq
                                                                                           : CursorColorMode::Sdr;
            cursorRenderer->CompositeOntoFrame(normalizedTexture, (int)dstDesc.Width, (int)dstDesc.Height,
                                               cursorCaptureState, cursorColorMode);
        }
    }

    d3d11Context->CopyResource(dstTexture, normalizedTexture);
    normalizedTexture->Release();
    return true;
}

bool VideoEncoder::CacheRepeatFrameTexture(ID3D11Texture2D* sourceTexture) {
    if (!sourceTexture || !d3d11Device || !d3d11Context) {
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc = {};
    sourceTexture->GetDesc(&srcDesc);

    D3D11_TEXTURE2D_DESC cacheDesc = srcDesc;
    cacheDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    cacheDesc.MiscFlags = 0;
    cacheDesc.CPUAccessFlags = 0;
    cacheDesc.Usage = D3D11_USAGE_DEFAULT;

    bool needsRecreate = true;
    if (repeatFrameTexture) {
        D3D11_TEXTURE2D_DESC existingDesc = {};
        repeatFrameTexture->GetDesc(&existingDesc);
        needsRecreate = existingDesc.Width != cacheDesc.Width || existingDesc.Height != cacheDesc.Height ||
                        existingDesc.Format != cacheDesc.Format || existingDesc.BindFlags != cacheDesc.BindFlags ||
                        existingDesc.ArraySize != cacheDesc.ArraySize ||
                        existingDesc.MipLevels != cacheDesc.MipLevels ||
                        existingDesc.SampleDesc.Count != cacheDesc.SampleDesc.Count ||
                        existingDesc.SampleDesc.Quality != cacheDesc.SampleDesc.Quality;
    }

    if (needsRecreate) {
        if (repeatFrameTexture) {
            repeatFrameTexture->Release();
            repeatFrameTexture = nullptr;
        }

        HRESULT hr = d3d11Device->CreateTexture2D(&cacheDesc, nullptr, &repeatFrameTexture);
        if (FAILED(hr)) {
            DLL_Log("[VideoEncoder] Failed to create repeat-frame texture: HR=%x fmt=%d %ux%u", hr, cacheDesc.Format,
                    cacheDesc.Width, cacheDesc.Height);
            return false;
        }
    }

    D3D11ScopedLock lock;
    d3d11Context->CopyResource(repeatFrameTexture, sourceTexture);
    return true;
}

void VideoEncoder::InvalidateRepeatSourceFrameTexture() {
    if (repeatSourceFrameTexture) {
        repeatSourceFrameTexture->Release();
        repeatSourceFrameTexture = nullptr;
    }
    repeatSourceNeedsCursorRecompose = false;
    repeatSourceFrameWidth = 0;
    repeatSourceFrameHeight = 0;
    repeatSourceFrameIsHDR = false;
    repeatSourceCaptureOriginX = 0;
    repeatSourceCaptureOriginY = 0;
}

bool VideoEncoder::CacheRepeatSourceFrameTexture(ID3D11Texture2D* sourceTexture, uint32_t frameWidth,
                                                 uint32_t frameHeight, bool isHDR, int captureOriginX,
                                                 int captureOriginY) {
    if (!sourceTexture || !d3d11Device || !d3d11Context) {
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc = {};
    sourceTexture->GetDesc(&srcDesc);

    struct KeyedSourceGuard {
        IDXGIKeyedMutex* mutex = nullptr;
        bool acquired = false;

        ~KeyedSourceGuard() {
            if (!mutex) {
                return;
            }
            if (acquired) {
                mutex->ReleaseSync(0);
            }
            mutex->Release();
        }
    } keyedSourceGuard;

    sourceTexture->QueryInterface(IID_PPV_ARGS(&keyedSourceGuard.mutex));
    if (keyedSourceGuard.mutex) {
        // Fresh split-device screen frames have already been consumed at key
        // 1 by the conversion path, which returns ownership to key 0. The
        // cursor-aware repeat cache is a second read and must explicitly own
        // that key; copying without it produced black repeat frames while
        // every fresh frame remained valid.
        const HRESULT kmHr = keyedSourceGuard.mutex->AcquireSync(0, 0);
        if (kmHr != S_OK) {
            ++repeatSourceCacheKeyedAcquireFailCount;
            if (repeatSourceCacheKeyedAcquireFailCount <= 5) {
                DLL_Log(
                    "[VideoEncoder] Cursor-aware repeat source cache keyed-mutex acquire failed: "
                    "HR=%x failures=%llu",
                    kmHr, static_cast<unsigned long long>(repeatSourceCacheKeyedAcquireFailCount));
            }
            return false;
        }
        keyedSourceGuard.acquired = true;
        if (!repeatSourceCacheKeyedMutexLogged) {
            DLL_Log("[VideoEncoder] Cursor-aware repeat source cache synchronized at keyed mutex 0->0");
            repeatSourceCacheKeyedMutexLogged = true;
        }
    }

    D3D11_TEXTURE2D_DESC cacheDesc = srcDesc;
    cacheDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    cacheDesc.MiscFlags = 0;
    cacheDesc.CPUAccessFlags = 0;
    cacheDesc.Usage = D3D11_USAGE_DEFAULT;

    bool needsRecreate = true;
    if (repeatSourceFrameTexture) {
        D3D11_TEXTURE2D_DESC existingDesc = {};
        repeatSourceFrameTexture->GetDesc(&existingDesc);
        needsRecreate = existingDesc.Width != cacheDesc.Width || existingDesc.Height != cacheDesc.Height ||
                        existingDesc.Format != cacheDesc.Format || existingDesc.BindFlags != cacheDesc.BindFlags ||
                        existingDesc.ArraySize != cacheDesc.ArraySize ||
                        existingDesc.MipLevels != cacheDesc.MipLevels ||
                        existingDesc.SampleDesc.Count != cacheDesc.SampleDesc.Count ||
                        existingDesc.SampleDesc.Quality != cacheDesc.SampleDesc.Quality;
    }

    if (needsRecreate) {
        InvalidateRepeatSourceFrameTexture();
        HRESULT hr = d3d11Device->CreateTexture2D(&cacheDesc, nullptr, &repeatSourceFrameTexture);
        if (FAILED(hr)) {
            if (!repeatSourceCacheFailureLogged) {
                DLL_Log("[VideoEncoder] Failed to create cursor-aware repeat source texture: HR=%x fmt=%d %ux%u", hr,
                        cacheDesc.Format, cacheDesc.Width, cacheDesc.Height);
                repeatSourceCacheFailureLogged = true;
            }
            return false;
        }
    }

    {
        D3D11ScopedLock lock;
        d3d11Context->CopyResource(repeatSourceFrameTexture, sourceTexture);
        if (keyedSourceGuard.acquired) {
            // Submit the read before publishing key 0 back to the producer.
            // This is a queue flush, not a CPU/GPU completion wait.
            d3d11Context->Flush();
        }
    }

    repeatSourceNeedsCursorRecompose = true;
    repeatSourceFrameWidth = frameWidth;
    repeatSourceFrameHeight = frameHeight;
    repeatSourceFrameIsHDR = isHDR;
    repeatSourceCaptureOriginX = captureOriginX;
    repeatSourceCaptureOriginY = captureOriginY;
    return true;
}

bool VideoEncoder::PopulateD3D11FrameFromRepeatSource(AVFrame* d3d11Frame) {
    if (!d3d11Frame || !repeatSourceFrameTexture || !repeatSourceNeedsCursorRecompose) {
        return false;
    }

    const AVPixelFormat activeSwFormat = GetActiveD3D11SwFormat();
    const bool useDirectRgbPath = IsDirectRgbD3D11SwFormat(activeSwFormat);

    if (!useDirectRgbPath && captureCursor && !videoProcessorInit) {
        if (!InitVideoProcessor()) {
            return false;
        }
    }

    if (useDirectRgbPath) {
        const int frameRet = av_hwframe_get_buffer(d3d11FramesCtx, d3d11Frame, 0);
        if (frameRet < 0 || !d3d11Frame->data[0]) {
            DLL_Log("[VideoEncoder] RepeatLastFrame failed to allocate direct RGB repeat frame: %d", frameRet);
            return false;
        }

        return PrepareD3D11TextureForEncode(
            repeatSourceFrameTexture, reinterpret_cast<ID3D11Texture2D*>(d3d11Frame->data[0]),
            CursorCompositionActive(), repeatSourceCaptureOriginX, repeatSourceCaptureOriginY, true);
    }

    if (!ConvertBGRAtoNV12(repeatSourceFrameTexture, d3d11Frame, CursorCompositionActive(), false,
                           repeatSourceCaptureOriginX, repeatSourceCaptureOriginY)) {
        return false;
    }

    d3d11Frame->width = scalingEnabled ? outputWidth : width;
    d3d11Frame->height = scalingEnabled ? outputHeight : height;
    return true;
}

bool VideoEncoder::CreateSharedCaptureTextures(uint32_t w, uint32_t h, uint32_t fmt, SharedMemoryLayout* sharedMem) {
    if (sharedCaptureTexturesCreated) {
        if (sharedCaptureTextureFormat == fmt) {
            return true;  // Already created with same format
        }
        // Format changed (e.g. DX9 BGRA→DX11 RGBA) — destroy and recreate
        DLL_Log("[VideoEncoder] KMT texture format changed %d -> %d, recreating", sharedCaptureTextureFormat, fmt);
        for (int i = 0; i < ENCODER_TEXTURE_SLOT_COUNT; i++) {
            if (sharedCaptureTextures[i]) {
                sharedCaptureTextures[i]->Release();
                sharedCaptureTextures[i] = nullptr;
            }
            sharedCaptureKmtHandles[i] = nullptr;
        }
        if (sharedCaptureFence) {
            sharedCaptureFence->Release();
            sharedCaptureFence = nullptr;
        }
        if (sharedCaptureFenceHandle) {
            CloseHandle(sharedCaptureFenceHandle);
            sharedCaptureFenceHandle = nullptr;
        }
        sharedCaptureTexturesCreated = false;
    }

    if (!d3d11Device) {
        DLL_Log("[VideoEncoder] CreateSharedCaptureTextures: No D3D11 device");
        return false;
    }

    DLL_Log("[VideoEncoder] Creating shared capture textures: %dx%d format=%d", w, h, fmt);

    // Create encoder-owned KMT shared textures (global WDDM handles for DXVK Vulkan import).
    for (int i = 0; i < ENCODER_TEXTURE_SLOT_COUNT; i++) {
        // KMT-only texture (D3D11_RESOURCE_MISC_SHARED only)
        D3D11_TEXTURE2D_DESC kmtDesc = {};
        kmtDesc.Width = w;
        kmtDesc.Height = h;
        kmtDesc.MipLevels = 1;
        kmtDesc.ArraySize = 1;
        kmtDesc.Format = (DXGI_FORMAT)fmt;
        kmtDesc.SampleDesc.Count = 1;
        kmtDesc.SampleDesc.Quality = 0;
        kmtDesc.Usage = D3D11_USAGE_DEFAULT;
        kmtDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        kmtDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        HRESULT hr = d3d11Device->CreateTexture2D(&kmtDesc, nullptr, &sharedCaptureTextures[i]);
        if (FAILED(hr)) {
            DLL_Log("[VideoEncoder] Failed to create KMT shared texture %d: HR=%x", i, hr);
            return false;
        }

        // Get KMT handle via IDXGIResource::GetSharedHandle
        IDXGIResource* dxgiRes = nullptr;
        hr = sharedCaptureTextures[i]->QueryInterface(IID_PPV_ARGS(&dxgiRes));
        if (FAILED(hr) || !dxgiRes) {
            DLL_Log("[VideoEncoder] Failed to get IDXGIResource for KMT texture %d: HR=%x", i, hr);
            return false;
        }

        hr = dxgiRes->GetSharedHandle(&sharedCaptureKmtHandles[i]);
        dxgiRes->Release();

        if (FAILED(hr) || !sharedCaptureKmtHandles[i]) {
            DLL_Log("[VideoEncoder] Failed to get KMT handle for texture %d: HR=%x", i, hr);
            return false;
        }

        DLL_Log("[VideoEncoder] Created KMT shared texture %d, kmtHandle=%p", i, sharedCaptureKmtHandles[i]);
    }

    // Create event for CPU-side fence waiting
    fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // Create shared fence
    HRESULT hr = d3d11Device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&sharedCaptureFence));
    if (FAILED(hr)) {
        DLL_Log("[VideoEncoder] Failed to create shared fence: HR=%x", hr);
        return false;
    }

    // Export fence handle - CreateSharedHandle is on the fence object, not the
    // device
    hr = sharedCaptureFence->CreateSharedHandle(nullptr,      // Security attributes
                                                GENERIC_ALL,  // Access rights
                                                nullptr,      // Name (optional)
                                                &sharedCaptureFenceHandle);
    if (FAILED(hr)) {
        DLL_Log("[VideoEncoder] Failed to export fence handle: HR=%x", hr);
        return false;
    }

    DLL_Log("[VideoEncoder] Created shared fence, handle=%p", sharedCaptureFenceHandle);

    // Publish to shared memory
    if (sharedMem) {
        this->pSharedMem = sharedMem;
        for (int i = 0; i < ENCODER_TEXTURE_SLOT_COUNT; i++) {
            sharedMem->encoderTextures.SetKmtTextureHandle(i, (uint64_t)sharedCaptureKmtHandles[i]);
        }
        sharedMem->encoderTextures.SetFenceHandle((uint64_t)sharedCaptureFenceHandle);
        sharedMem->encoderTextures.SetWidth(w);
        sharedMem->encoderTextures.SetHeight(h);
        sharedMem->encoderTextures.SetFormat(fmt);
        sharedMem->encoderTextures.kmtReady.store(true, std::memory_order_release);
        sharedMem->encoderTextures.ready.store(true, std::memory_order_release);
        DLL_Log("[VideoEncoder] Published encoder KMT textures to shared memory");
    }

    sharedCaptureTextureFormat = fmt;
    sharedCaptureTexturesCreated = true;
    return true;
}

bool VideoEncoder::ConfigureAndOpenCodec() {
    if (!codecCtx || !fmtCtx) {
        DLL_Log("[VideoEncoder] ConfigureAndOpenCodec: Missing context(s)");
        return false;
    }

    const AVCodec* codec = codecCtx->codec;
    if (!codec) {
        codec = avcodec_find_encoder_by_name(savedConfig.encoder.c_str());
        if (!codec) {
            DLL_Log("[VideoEncoder] ConfigureAndOpenCodec: Codec not found");
            return false;
        }
    }

    // Build encoder options from savedConfig
    AVDictionary* opts = nullptr;

    // Log all config settings for debugging
    DLL_Log("[VideoEncoder] ===== ENCODER SETTINGS FROM CONFIG =====");
    DLL_Log("[VideoEncoder] encoder=%s", savedConfig.encoder.c_str());
    DLL_Log("[VideoEncoder] fps=%d", savedConfig.fps);
    DLL_Log("[VideoEncoder] preset=%s", savedConfig.preset.c_str());
    DLL_Log("[VideoEncoder] tuning=%s", savedConfig.tuning.c_str());
    DLL_Log("[VideoEncoder] rate_control=%s", savedConfig.rateControl.c_str());
    DLL_Log("[VideoEncoder] bitrate=%s", savedConfig.bitrate.c_str());
    DLL_Log("[VideoEncoder] max_bitrate=%s", savedConfig.maxBitrate.c_str());
    DLL_Log("[VideoEncoder] profile=%s", savedConfig.profile.c_str());
    DLL_Log("[VideoEncoder] lookahead=%s", savedConfig.lookahead.c_str());
    DLL_Log("[VideoEncoder] spatial_aq=%s temporal_aq=%s aq_strength=%d", savedConfig.spatialAq ? "true" : "false",
            savedConfig.temporalAq ? "true" : "false", savedConfig.aqStrength);
    DLL_Log("[VideoEncoder] b_frames=%d", savedConfig.bFrames);
    DLL_Log("[VideoEncoder] b_ref_mode=%s", savedConfig.bRefMode.empty() ? "(auto)" : savedConfig.bRefMode.c_str());
    DLL_Log("[VideoEncoder] multipass=%s", savedConfig.multipass.c_str());
    DLL_Log("[VideoEncoder] split_encode=%s", savedConfig.splitEncode.c_str());
    DLL_Log("[VideoEncoder] keyframe_interval=%d", savedConfig.keyframeInterval);
    DLL_Log("[VideoEncoder] qp=%d", savedConfig.qp);
    DLL_Log("[VideoEncoder] bit_depth=%s color_space=%s color_range=%s chroma=%s", savedConfig.bitDepth.c_str(),
            savedConfig.colorSpace.c_str(), savedConfig.colorRange.c_str(), savedConfig.chromaSubsampling.c_str());
    if (!savedConfig.customOptions.empty()) {
        DLL_Log("[VideoEncoder] custom_options=%s", savedConfig.customOptions.c_str());
    }
    DLL_Log("[VideoEncoder] ==============================================");

    // Check encoder type for option compatibility
    bool isMF = (savedConfig.encoder.find("_mf") != std::string::npos);

    // Set color properties from config (with auto-detection defaults)
    // Color space
    std::string cs = savedConfig.colorSpace;
    if (!cs.empty() && _stricmp(cs.c_str(), "auto") != 0 && _stricmp(cs.c_str(), "bt709") != 0 &&
        _stricmp(cs.c_str(), "bt2020") != 0) {
        DLL_Log("[VideoEncoder] Unsupported color_space='%s'; expected auto, bt709, or bt2020", cs.c_str());
        return false;
    }
    const bool outputIsHDR = ShouldEncodeHdrOutput();
    if (cs.empty() || _stricmp(cs.c_str(), "auto") == 0) {
        cs = outputIsHDR ? "bt2020" : "bt709";
    } else if (_stricmp(cs.c_str(), "bt2020") == 0) {
        cs = "bt2020";
    } else {
        cs = "bt709";
    }
    if (currentIsHDR && !outputIsHDR) {
        DLL_Log(
            "[HDR->SDR] color_space=bt709 explicitly requests SDR; enabling whole-frame GPU tone mapping and SDR "
            "metadata");
    }
    if (cs == "bt2020") {
        codecCtx->color_primaries = AVCOL_PRI_BT2020;
        codecCtx->color_trc = outputIsHDR ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_BT2020_10;
        codecCtx->colorspace = AVCOL_SPC_BT2020_NCL;
    } else {
        codecCtx->color_primaries = AVCOL_PRI_BT709;
        codecCtx->color_trc = AVCOL_TRC_BT709;
        codecCtx->colorspace = AVCOL_SPC_BT709;
    }

    // Color range
    std::string cr = savedConfig.colorRange;
    const OutputRangeMode outputRange = GetEffectiveOutputRange(cr, outputIsHDR);
    if (WantsFullOutputRange(cr) && outputIsHDR) {
        DLL_Log("[VideoEncoder] color_range=full requested for HDR, but VP/YCbCr output stays limited-range");
    }
    codecCtx->color_range = GetAVColorRange(outputRange);

    // Bit depth and chroma subsampling → pixel format
    std::string bd = savedConfig.bitDepth;
    if (bd == "auto" || bd.empty()) {
        bd = ShouldUse10BitOutput() ? "10" : "8";
    }
    std::string chroma = savedConfig.chromaSubsampling;
    if (chroma == "auto" || chroma.empty()) {
        chroma = "420";
    }

    ResolvedVideoFormat resolvedFormat;
    std::string resolvedError;
    std::string resolvedWarning;
    if (!ResolveVideoFormat(savedConfig, outputIsHDR, ShouldUse10BitOutput(), codec, &resolvedFormat, &resolvedError,
                            &resolvedWarning)) {
        DLL_Log("%s", resolvedError.c_str());
        return false;
    }
    if (!resolvedWarning.empty()) {
        DLL_Log("%s", resolvedWarning.c_str());
    }
    codecCtx->pix_fmt = resolvedFormat.codecPixFmt;
    bd = resolvedFormat.bitDepth;
    chroma = resolvedFormat.chroma;
    bool use10bit = resolvedFormat.use10Bit;
    codecCtx->chroma_sample_location = (resolvedFormat.chroma == "420") ? AVCHROMA_LOC_LEFT : AVCHROMA_LOC_UNSPECIFIED;

    DLL_Log(
        "[VideoEncoder] Color config: space=%s range=%s bitDepth=%s chroma=%s "
        "pixFmt=%d hwSwFmt=%s path=%s sourceHdr=%d outputHdr=%d",
        cs.c_str(), DescribeOutputRange(outputRange), bd.c_str(), chroma.c_str(), codecCtx->pix_fmt,
        GetPixFmtNameSafe(resolvedFormat.d3d11SwFormat), resolvedFormat.usesVideoProcessor ? "vp-yuv" : "direct-rgb",
        currentIsHDR ? 1 : 0, outputIsHDR ? 1 : 0);

    const ce::video::EncoderOptionPlan optionPlan = ce::video::BuildEncoderOptionPlan(savedConfig, use10bit, chroma);
    for (const auto& warning : optionPlan.warnings) {
        DLL_Log("[VideoEncoder] %s", warning.c_str());
    }
    if (!optionPlan.errors.empty()) {
        for (const auto& error : optionPlan.errors) {
            DLL_Log("[VideoEncoder] %s", error.c_str());
        }
        return false;
    }
    for (const auto& option : optionPlan.generatedOptions) {
        av_dict_set(&opts, option.key.c_str(), option.value.c_str(), 0);
    }

    // Log all generated and custom encoder options
    DLL_Log("[VideoEncoder] ===== GENERATED ENCODER OPTIONS =====");
    for (const auto& option : optionPlan.generatedOptions) {
        DLL_Log("[VideoEncoder]   %s=%s", option.key.c_str(), option.value.c_str());
    }
    if (!optionPlan.customOptions.empty()) {
        DLL_Log("[VideoEncoder]   --- custom overrides ---");
        for (const auto& option : optionPlan.customOptions) {
            DLL_Log("[VideoEncoder]   %s=%s (custom)", option.key.c_str(), option.value.c_str());
        }
    }
    if (!optionPlan.requiredOptions.empty()) {
        DLL_Log("[VideoEncoder]   --- required safety overrides (applied last) ---");
        for (const auto& option : optionPlan.requiredOptions) {
            DLL_Log("[VideoEncoder]   %s=%s (required)", option.key.c_str(), option.value.c_str());
        }
    }
    DLL_Log("[VideoEncoder]   bitRate=%lld maxBitRate=%lld maxBFrames=%d", optionPlan.bitRate.value_or(0),
            optionPlan.maxBitRate.value_or(0), optionPlan.maxBFrames);
    DLL_Log("[VideoEncoder] ======================================");

    codecCtx->bit_rate = optionPlan.bitRate.value_or(0);
    codecCtx->rc_max_rate = optionPlan.maxBitRate.value_or(0);
    codecCtx->max_b_frames = optionPlan.maxBFrames;

    // Equalize B-frame quality with P-frames.  For software encoders this
    // directly controls the inter-frame QP relationship.  For hardware
    // encoders (NVENC, AMF, QSV) the FFmpeg wrappers use b_quant_factor to
    // compute initialRCQP.qpInterB in VBR mode — setting it to 1.0 makes
    // the initial B-frame QP equal to the P-frame QP, giving the rate
    // controller a better starting point instead of the FFmpeg default of
    // b_quant_factor=1.25 / b_quant_offset=1.25 which biases B-frames
    // towards lower quality from the start.
    if (optionPlan.maxBFrames > 0) {
        codecCtx->b_quant_factor = 1.0f;
        codecCtx->b_quant_offset = 0.0f;
        DLL_Log(
            "[VideoEncoder] B-frame initial QP hint aligned with P-frames "
            "(b_quant_factor=1.0, b_quant_offset=0.0)");
    }

    if (savedConfig.keyframeInterval > 0) {
        codecCtx->gop_size = savedConfig.fps * savedConfig.keyframeInterval;
    } else if (savedConfig.keyframeInterval < 0) {
        DLL_Log("[VideoEncoder] keyframe_interval=%d is invalid; using encoder default", savedConfig.keyframeInterval);
    }

    if (isMF) {
        if (!savedConfig.mfRateControl.empty())
            av_dict_set(&opts, "rate_control", savedConfig.mfRateControl.c_str(), 0);
        if (savedConfig.mfQuality >= 0 && savedConfig.mfQuality <= 100)
            av_dict_set_int(&opts, "quality", savedConfig.mfQuality, 0);
        if (!savedConfig.mfScenario.empty())
            av_dict_set(&opts, "scenario", savedConfig.mfScenario.c_str(), 0);
        av_dict_set_int(&opts, "hw_encoding", savedConfig.mfHwEncoding ? 1 : 0, 0);
    }

    for (const auto& option : optionPlan.customOptions) {
        av_dict_set(&opts, option.key.c_str(), option.value.c_str(), 0);
    }
    for (const auto& option : optionPlan.requiredOptions) {
        av_dict_set(&opts, option.key.c_str(), option.value.c_str(), 0);
    }

    if (savedConfig.useVFR) {
        codecCtx->time_base = {1, 1000000};
        codecCtx->framerate = {savedConfig.fps, 1};
    } else {
        codecCtx->time_base = {1, savedConfig.fps};
        codecCtx->framerate = {savedConfig.fps, 1};
    }

    codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    DLL_Log("[VideoEncoder] Opening Codec with options...");
    int ret = avcodec_open2(codecCtx, codec, &opts);

    // Log any options that the encoder didn't consume
    if (opts) {
        const AVDictionaryEntry* entry = nullptr;
        while ((entry = av_dict_iterate(opts, entry))) {
            DLL_Log("[VideoEncoder] WARNING: Unused encoder option: %s=%s", entry->key, entry->value);
        }
        av_dict_free(&opts);
    }

    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        DLL_Log("[VideoEncoder] Failed to open codec: %d. Error details: %s", ret, errbuf);
        codecOpenFailed = true;
        return false;
    }

    DLL_Log("[VideoEncoder] Codec Opened Successfully.");
    DLL_Log("[VideoEncoder] ===== ACTIVE CODEC CONTEXT =====");
    DLL_Log("[VideoEncoder]   codec=%s", codecCtx->codec->name);
    DLL_Log("[VideoEncoder]   resolution=%dx%d", codecCtx->width, codecCtx->height);
    DLL_Log("[VideoEncoder]   pix_fmt=%s sw_pix_fmt=%s", av_get_pix_fmt_name(codecCtx->pix_fmt),
            GetPixFmtNameSafe(codecCtx->sw_pix_fmt));
    DLL_Log("[VideoEncoder]   time_base=%d/%d framerate=%d/%d", codecCtx->time_base.num, codecCtx->time_base.den,
            codecCtx->framerate.num, codecCtx->framerate.den);
    DLL_Log("[VideoEncoder]   bit_rate=%lld rc_max_rate=%lld", (long long)codecCtx->bit_rate,
            (long long)codecCtx->rc_max_rate);
    DLL_Log("[VideoEncoder]   gop_size=%d max_b_frames=%d", codecCtx->gop_size, codecCtx->max_b_frames);
    DLL_Log("[VideoEncoder]   b_quant_factor=%.2f b_quant_offset=%.2f", codecCtx->b_quant_factor,
            codecCtx->b_quant_offset);
    DLL_Log("[VideoEncoder]   i_quant_factor=%.2f i_quant_offset=%.2f", codecCtx->i_quant_factor,
            codecCtx->i_quant_offset);
    DLL_Log("[VideoEncoder]   has_b_frames=%d (encoder-reported reorder depth)", codecCtx->has_b_frames);
    DLL_Log("[VideoEncoder] ================================");

    if (codec && codec->id == AV_CODEC_ID_AV1) {
        DLL_Log(
            "[VideoEncoder] AV1 duplicate frames will be re-encoded from the cached texture (packet replay disabled)");
    }

    stream = avformat_new_stream(fmtCtx, codec);
    avcodec_parameters_from_context(stream->codecpar, codecCtx);
    stream->codecpar->chroma_location = codecCtx->chroma_sample_location;
    stream->time_base = codecCtx->time_base;
    stream->avg_frame_rate = codecCtx->framerate;
    stream->r_frame_rate = codecCtx->framerate;

    for (auto& actx : audioContexts) {
        if (actx.codecCtx) {
            actx.streamIndex = AddAudioStream(actx.config, actx.codecCtx, actx.track);
            if (actx.streamIndex >= 0 && audioStreamIndex < 0)
                audioStreamIndex = actx.streamIndex;
        }
    }

    initDone = true;
    return true;
}

bool VideoEncoder::EnsureDevice() {
    if (initDone)
        return true;

    // Don't retry if codec already failed - prevents infinite loop and device
    // leak
    if (codecOpenFailed) {
        return false;
    }

    const bool hasInjectLuid = (luidLow != 0 || luidHigh != 0);
    DLL_Log("[VideoEncoder] EnsureDevice with LUID: %08x %08x", luidLow, luidHigh);
    if (!hasInjectLuid) {
        DLL_Log("[VideoEncoder] EnsureDevice using shared framegrab device (no inject LUID)");
    }

    // D3D11 Video Processor is the only supported color conversion path
    // (D3D12 does not have an equivalent VideoProcessorBlt API)

    // 1. Find Adapter by LUID
    IDXGIAdapter* targetAdapter = nullptr;
    if (hasInjectLuid) {
        LUID searchLuid;
        searchLuid.LowPart = (DWORD)luidLow;
        searchLuid.HighPart = (LONG)luidHigh;

        DLL_Log("[VideoEncoder] Searching for Adapter with LUID: %08x-%08x", searchLuid.HighPart, searchLuid.LowPart);

        IDXGIFactory4* factory4 = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4)))) {
            if (SUCCEEDED(factory4->EnumAdapterByLuid(searchLuid, IID_PPV_ARGS(&targetAdapter)))) {
                DLL_Log("[VideoEncoder] Found Adapter matching LUID via IDXGIFactory4");
            }
            factory4->Release();
        }

        if (!targetAdapter) {
            IDXGIFactory1* factory = nullptr;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
                IDXGIAdapter* adapter = nullptr;
                for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                    DXGI_ADAPTER_DESC desc;
                    adapter->GetDesc(&desc);
                    if (desc.AdapterLuid.LowPart == searchLuid.LowPart &&
                        desc.AdapterLuid.HighPart == searchLuid.HighPart) {
                        targetAdapter = adapter;
                        DLL_Log("[VideoEncoder] Found Adapter matching LUID via manual scan");
                        break;
                    }
                    adapter->Release();
                }
                factory->Release();
            }
        }
    }

    // 1. Create D3D11 Device Manually
    // For screengrab mode (LUID=0), we use the shared device created in
    // MediaEngine_GetD3D11Device This ensures ScreenCapture and VideoEncoder
    // share the same device for CopyResource compatibility

    // Declare these for extern access to shared device from mediaengine.cpp
    extern ID3D11Device* g_SharedD3D11Device;
    extern ID3D11DeviceContext* g_SharedD3D11Context;

    // Skip device creation if already preserved (DXVK zero-copy across recordings)
    if (d3d11Device && d3d11Context) {
        DLL_Log("[VideoEncoder] Reusing existing D3D11 device (preserved for encoder textures)");
    } else if (!hasInjectLuid && g_SharedD3D11Device) {
        // Framegrab mode - use the shared device that ScreenCapture also uses
        DLL_Log("[VideoEncoder] Framegrab using dimensions: %dx%d", width, height);
        g_SharedD3D11Device->QueryInterface(IID_PPV_ARGS(&d3d11Device));
        g_SharedD3D11Context->QueryInterface(IID_PPV_ARGS(&d3d11Context));
        DLL_Log("[VideoEncoder] Using shared D3D11 device for framegrab");
    } else {
        if (!hasInjectLuid) {
            DLL_Log("[VideoEncoder] WARNING: no inject LUID and no shared framegrab device are available");
        }
        // Inject mode - create device on specific adapter
        DLL_Log("[VideoEncoder] Creating D3D11 Device (Flags: BGRA + VIDEO)...");

        UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
// createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG; // Optional
#endif

        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL featureLevel;
        ID3D11Device* baseDevice = nullptr;
        ID3D11DeviceContext* baseContext = nullptr;

        HRESULT hr = D3D11CreateDevice(
            targetAdapter, targetAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, 0, createDeviceFlags,
            featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, &baseDevice, &featureLevel, &baseContext);

        if (targetAdapter)
            targetAdapter->Release();

        if (FAILED(hr)) {
            DLL_Log("[VideoEncoder] D3D11CreateDevice Failed: 0x%x (Target: %p)", hr, targetAdapter);
            return false;
        }
        DLL_Log("[VideoEncoder] D3D11 Device Created (Feature Level: 0x%x)", featureLevel);

        // Use RAII to prevent leaks on error paths
        ce::ComGuard<ID3D11Device> baseDeviceGuard(baseDevice);
        ce::ComGuard<ID3D11DeviceContext> baseContextGuard(baseContext);

        // QI for Interfaces
        if (FAILED(baseDevice->QueryInterface(IID_PPV_ARGS(&d3d11Device)))) {
            return false;
        }

        if (FAILED(baseContext->QueryInterface(IID_PPV_ARGS(&d3d11Context)))) {
            return false;
        }

        // 2. Wrap in AVHWDeviceContext
        d3d11DeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!d3d11DeviceCtx)
            return false;

        AVHWDeviceContext* deviceCtx = (AVHWDeviceContext*)d3d11DeviceCtx->data;
        AVD3D11VADeviceContext* d3d11Ctx = (AVD3D11VADeviceContext*)deviceCtx->hwctx;
        d3d11Ctx->device = baseDevice;
        baseDevice->AddRef();

        if (av_hwdevice_ctx_init(d3d11DeviceCtx) < 0)
            return false;

        // baseDeviceGuard and baseContextGuard will auto-release on scope exit
    }  // End of else block (inject mode device creation)

    // Apply explicit GPU priority only. With gpu_priority=0 the encoder starts
    // neutral and raises priority adaptively only if encode time sustains real
    // pressure, so capture does not fight the game during healthy 10-bit runs.
    if (d3d11Device) {
        if (gpuPriority != 0) {
            DLL_Log(
                "[VideoEncoder] Explicit gpu_priority=%d configured; adaptive encoder GPU priority is bypassed for "
                "this recording",
                gpuPriority);
        }
        ApplyGpuThreadPriority(gpuPriority, "initial");
    }

    // CreateSharedCaptureTextures can run before Start() recreates codec/container
    // contexts after a previous Stop(). In that pre-start phase we only need the
    // D3D11 device for texture allocation; defer FFmpeg HW context wiring until
    // Start() has rebuilt fmtCtx/codecCtx.
    if (!codecCtx || !fmtCtx) {
        if (!recordingRequested) {
            DLL_Log(
                "[VideoEncoder] EnsureDevice: device-only init (fmtCtx=%p codecCtx=%p), "
                "deferring codec prewarm to Start()",
                (void*)fmtCtx, (void*)codecCtx);
            return true;
        }

        DLL_Log("[VideoEncoder] EnsureDevice failed: missing contexts while recording (fmtCtx=%p codecCtx=%p)",
                (void*)fmtCtx, (void*)codecCtx);
        return false;
    }

    // Set up FFmpeg HW device context with our D3D11 device (shared for both
    // paths)
    if (!d3d11DeviceCtx) {
        // 2. Wrap in AVHWDeviceContext - for screengrab mode using shared device
        d3d11DeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!d3d11DeviceCtx)
            return false;

        AVHWDeviceContext* deviceCtx = (AVHWDeviceContext*)d3d11DeviceCtx->data;
        AVD3D11VADeviceContext* d3d11Ctx = (AVD3D11VADeviceContext*)deviceCtx->hwctx;

        // Get base device from our QI'd interface
        ce::ComGuard<ID3D11Device> baseDevice;
        if (FAILED(d3d11Device->QueryInterface(__uuidof(ID3D11Device), (void**)baseDevice.addressof()))) {
            return false;
        }

        d3d11Ctx->device = baseDevice.get();
        // FFmpeg expects to own a reference, so we AddRef.
        // The ComPtr will release our local reference when it goes out of scope.
        baseDevice->AddRef();

        if (av_hwdevice_ctx_init(d3d11DeviceCtx) < 0) {
            // If init fails, we rely on ComPtr to release baseDevice.
            // We also need to clean up the partially created context.
            av_buffer_unref(&d3d11DeviceCtx);
            return false;
        }
        // baseDevice releases its ref here, but FFmpeg holds one via AddRef above.
    }

    const AVCodec* codec = codecCtx->codec;
    if (!codec) {
        codec = avcodec_find_encoder_by_name(savedConfig.encoder.c_str());
    }
    if (!codec) {
        DLL_Log("[VideoEncoder] EnsureDevice: Codec not found for format resolution");
        return false;
    }

    ResolvedVideoFormat resolvedFormat;
    std::string resolvedError;
    std::string resolvedWarning;
    if (!ResolveVideoFormat(savedConfig, ShouldEncodeHdrOutput(), ShouldUse10BitOutput(), codec, &resolvedFormat,
                            &resolvedError,
                            &resolvedWarning)) {
        DLL_Log("%s", resolvedError.c_str());
        return false;
    }
    if (!resolvedWarning.empty()) {
        DLL_Log("%s", resolvedWarning.c_str());
    }

    codecCtx->pix_fmt = resolvedFormat.codecPixFmt;
    if (codecCtx->hw_device_ctx) {
        av_buffer_unref(&codecCtx->hw_device_ctx);
    }
    codecCtx->hw_device_ctx = av_buffer_ref(d3d11DeviceCtx);

    // 3. D3D11 Frames Context
    if (d3d11FramesCtx) {
        av_buffer_unref(&d3d11FramesCtx);
    }
    d3d11FramesCtx = av_hwframe_ctx_alloc(d3d11DeviceCtx);
    if (!d3d11FramesCtx) {
        DLL_Log("[VideoEncoder] Failed to allocate D3D11 frames context");
        return false;
    }
    AVHWFramesContext* d11Frames = (AVHWFramesContext*)d3d11FramesCtx->data;
    AVD3D11VAFramesContext* d11FramesHw = (AVD3D11VAFramesContext*)d11Frames->hwctx;
    d11Frames->format = AV_PIX_FMT_D3D11;
    // RGB->YUV output is written directly into AVHWFrame-owned textures by
    // ID3D11VideoProcessor. NVENC then retains the AVFrame until that input is
    // no longer in flight, so lookahead/B-frame depth cannot recycle a surface
    // that the encoder still references.
    d11FramesHw->BindFlags |= D3D11_BIND_RENDER_TARGET;

    d11Frames->sw_format = resolvedFormat.d3d11SwFormat;
    if (!DeviceSupportsHwFrameSwFormat(d3d11DeviceCtx, resolvedFormat.d3d11SwFormat)) {
        DLL_Log("[VideoEncoder] D3D11 HW frames do not support sw_format=%s on this device",
                GetPixFmtNameSafe(resolvedFormat.d3d11SwFormat));
        return false;
    }
    if (resolvedFormat.usesVideoProcessor) {
        if (resolvedFormat.use10Bit) {
            DLL_Log("[VideoEncoder] Using P010 (10-bit) sw_format for D3D11 HW frames");
        }
    } else {
        DLL_Log("[VideoEncoder] Using direct D3D11 RGB 4:4:4 path with sw_format=%s",
                GetPixFmtNameSafe(resolvedFormat.d3d11SwFormat));
    }

    int framesWidth = width;
    int framesHeight = height;
    if (savedConfig.scaling.enabled && savedConfig.scaling.outputWidth > 0 && savedConfig.scaling.outputHeight > 0) {
        framesWidth = savedConfig.scaling.outputWidth;
        framesHeight = savedConfig.scaling.outputHeight;
    }

    if (resolvedFormat.requiresEvenDimensions) {
        framesWidth = framesWidth & ~1;
        framesHeight = framesHeight & ~1;
    }

    d11Frames->width = framesWidth;
    d11Frames->height = framesHeight;
    d11Frames->initial_pool_size = 0;

    if (av_hwframe_ctx_init(d3d11FramesCtx) < 0) {
        DLL_Log("[VideoEncoder] Failed to init D3D11 frames context");
        return false;
    }
    if (codecCtx->hw_frames_ctx) {
        av_buffer_unref(&codecCtx->hw_frames_ctx);
    }
    codecCtx->hw_frames_ctx = av_buffer_ref(d3d11FramesCtx);
    codecCtx->extra_hw_frames = 5;
    codecCtx->width = framesWidth;
    codecCtx->height = framesHeight;

    return ConfigureAndOpenCodec();
}

int VideoEncoder::AddAudioStream(const AudioConfig& config, AVCodecContext* audioCtx, int track) {
    if (!fmtCtx)
        return -1;

    const AVCodec* codec = nullptr;
    if (audioCtx) {
        codec = audioCtx->codec;
    } else {
        std::string codecName = config.codec.empty() ? "aac" : config.codec;
        std::transform(codecName.begin(), codecName.end(), codecName.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (codecName == "pcm") {
            codecName = config.bitDepth == "16" ? "pcm_s16le" : (config.bitDepth == "32" ? "pcm_f32le" : "pcm_s24le");
        } else if (codecName == "opus") {
            codecName = "libopus";
        }
        codec = avcodec_find_encoder_by_name(codecName.c_str());
    }

    if (!codec)
        return -1;
    AVStream* st = avformat_new_stream(fmtCtx, codec);
    if (!st)
        return -1;

    if (audioCtx) {
        // Correct way: copy parameters including extradata
        avcodec_parameters_from_context(st->codecpar, audioCtx);
        int sampleRate = audioCtx->sample_rate > 0 ? audioCtx->sample_rate : st->codecpar->sample_rate;
        if (sampleRate <= 0) {
            sampleRate = 48000;
        }
        st->time_base = {1, sampleRate};
    } else {
        // Fallback (might fail for extradata-dependent codecs). ParseSampleRateOr
        // never throws on a malformed config sample_rate (unlike std::stoi).
        int sampleRate = ce::audio::ParseSampleRateOr(config.sampleRate, 48000);
        st->time_base = {1, sampleRate};
        st->codecpar->codec_id = codec->id;
        st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        st->codecpar->sample_rate = sampleRate;
        const int channels =
            std::clamp(config.downmix ? 2 : (config.outputChannels > 0 ? config.outputChannels : 2), 1, 8);
        if (config.outputChannelMask != 0 && !config.downmix) {
            av_channel_layout_from_mask(&st->codecpar->ch_layout, config.outputChannelMask);
        } else {
            av_channel_layout_default(&st->codecpar->ch_layout, channels);
        }
    }

    if (track > 0) {
        std::string title = "Track " + std::to_string(track);
        av_dict_set(&st->metadata, "title", title.c_str(), 0);
    }
    return st->index;
}

void VideoEncoder::SetAudioContext(const AudioConfig& config, AVCodecContext* audioCtx) {
    savedAudioConfig = config;
    savedAudioCodecCtx = audioCtx;

    // Also add to multi-source array for compatibility
    // Clear previous contexts first (SetAudioContext is for single-source mode)
    audioContexts.clear();

    AudioStreamContext ctx;
    ctx.config = config;
    ctx.codecCtx = audioCtx;
    ctx.track = config.tracks.empty() ? 0 : config.tracks[0];
    ctx.streamIndex = -1;
    audioContexts.push_back(ctx);
}

int VideoEncoder::AddAudioContext(const AudioConfig& config, AVCodecContext* audioCtx, int track) {
    AudioStreamContext ctx;
    ctx.config = config;
    ctx.codecCtx = audioCtx;
    ctx.track = track;
    ctx.streamIndex = -1;

    for (auto it = audioContexts.begin(); it != audioContexts.end(); ++it) {
        if (it->track == track) {
            it->config = config;
            it->codecCtx = audioCtx;
            it->streamIndex = -1;
            DLL_Log("[VideoEncoder] AddAudioContext: track=%d replaced existing entry", track);
            return track;
        }
        if (it->track > track) {
            audioContexts.insert(it, ctx);
            DLL_Log("[VideoEncoder] AddAudioContext: track=%d, total=%d", ctx.track, (int)audioContexts.size());
            return ctx.track;
        }
    }

    audioContexts.push_back(ctx);

    DLL_Log("[VideoEncoder] AddAudioContext: track=%d, total=%d", ctx.track, (int)audioContexts.size());

    return ctx.track;
}

void VideoEncoder::ClearAudioContexts() {
    audioContexts.clear();
    audioStreamIndex = -1;
}

int VideoEncoder::GetAudioStreamIndex(int track) const {
    // Backward compatible: track -1 returns first stream index
    if (track < 0) {
        if (!audioContexts.empty()) {
            return audioContexts[0].streamIndex;
        }
        return audioStreamIndex;
    }

    // Find stream index for specific track
    for (const auto& ctx : audioContexts) {
        if (ctx.track == track) {
            return ctx.streamIndex;
        }
    }

    return -1;
}

void VideoEncoder::BeginDeferredRecording() {
    codecOpenFailed = false;
    writerFinalizeTimedOut.store(false, std::memory_order_relaxed);
    writerFinalizeSlowWarningLogged.store(false, std::memory_order_relaxed);
    writerFinalizePhase.store(kWriterPhaseRunning, std::memory_order_relaxed);
    discardOutputRequested.store(false, std::memory_order_relaxed);
    encodedDurationUs.store(0, std::memory_order_relaxed);
    lastAssignedVideoPts = -1;
    lastFrameDeferred.store(false, std::memory_order_relaxed);
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
    if (repeatFrameTexture) {
        repeatFrameTexture->Release();
        repeatFrameTexture = nullptr;
    }
    InvalidateRepeatSourceFrameTexture();

    audioPacketCount = 0;
    videoPacketCount = 0;
    vidDebugCount = 0;
    asyncWriteErrorCount = 0;
    packetStats.Reset();
    ResetPacketTimelineDiagnostics();

    recordingRequested = true;
    needsCounterReset = true;
    DLL_Log("[VideoEncoder] Start Recording Requested (Deferred).");

    g_lastFramePts = -1;
    g_framesEncoded = 0;
    g_totalFenceWait = 0.0;
    g_totalColorConvert = 0.0;
    g_totalEncode = 0.0;
    g_maxFrameTime = 0.0;
    g_slowFrameCount = 0;

    if (!writerRunning) {
        writerRunning = true;
        writerFinalizePhase.store(kWriterPhaseRunning, std::memory_order_relaxed);
        writerThread = std::thread(&VideoEncoder::AsyncWriteLoop, this);
        DLL_Log("[VideoEncoder] Started Writer Thread");
    }
}

bool VideoEncoder::Start() {
    // Ensure previous recording is fully finalized and resources cleaned up.
    // Stop() will signal the async finalize if needed, then we wait for it to
    // finish.
    Stop();
    if (writerThread.joinable()) {
        if (writerFinalizeTimedOut.load(std::memory_order_acquire)) {
            HANDLE hThread = writerThread.native_handle();
            DWORD waitResult = WaitForSingleObject(hThread, 0);
            if (waitResult != WAIT_OBJECT_0) {
                DLL_Log(
                    "[VideoEncoder] Start: ERROR previous writer finalize is still running (result=%lu); refusing "
                    "new recording to preserve muxer ownership",
                    waitResult);
                return false;
            }
            DLL_Log("[VideoEncoder] Start: Previous timed-out writer completed before restart; joining now.");
            writerThread.join();
            writerFinalizeTimedOut.store(false, std::memory_order_release);
        } else {
            DLL_Log("[VideoEncoder] Start: Waiting for previous recording to finalize...");
            writerThread.join();
        }
    }

    // If fmtCtx was freed by Stop(), recreate it for the new recording
    if (!fmtCtx) {
        DLL_Log("[VideoEncoder] Creating new output format context for container: %s", savedConfig.container.c_str());

        if (AllocateOutputContextForContainer(&fmtCtx, savedConfig) < 0) {
            DLL_Log("[VideoEncoder] Failed to allocate new format context");
            return false;
        }
    }

    // If codecCtx was freed by Stop(), recreate it
    if (!codecCtx) {
        const AVCodec* codec = avcodec_find_encoder_by_name(savedConfig.encoder.c_str());
        if (!codec) {
            DLL_Log("[VideoEncoder] Codec not found: %s", savedConfig.encoder.c_str());
            return false;
        }

        codecCtx = avcodec_alloc_context3(codec);
        if (!codecCtx) {
            DLL_Log("[VideoEncoder] Failed to alloc new codec context");
            return false;
        }

        codecCtx->width = width;
        codecCtx->height = height;

        // Apply configured pixel format
        ResolvedVideoFormat resolvedFormat;
        std::string resolvedError;
        std::string resolvedWarning;
        if (!ResolveVideoFormat(savedConfig, ShouldEncodeHdrOutput(), ShouldUse10BitOutput(), codec, &resolvedFormat,
                                &resolvedError, &resolvedWarning)) {
            DLL_Log("%s", resolvedError.c_str());
            avcodec_free_context(&codecCtx);
            return false;
        }
        if (!resolvedWarning.empty()) {
            DLL_Log("%s", resolvedWarning.c_str());
        }
        codecCtx->pix_fmt = resolvedFormat.codecPixFmt;

        DLL_Log("[VideoEncoder] Recreated codec context for new recording");

        if (d3d11FramesCtx) {
            codecCtx->hw_frames_ctx = av_buffer_ref(d3d11FramesCtx);
            codecCtx->extra_hw_frames = 5;
        }
    }

    // Pre-warm device and codec to reduce first-frame latency
    // This moves heavy initialization (D3D11 device, codec open, video processor)
    // from first frame to Start() call, avoiding game stutter on recording start
    // IMPORTANT: Only pre-warm if we already have valid dimensions from common
    // discovery
    if ((luidLow != 0 || luidHigh != 0) && width > 0 && height > 0 && !initDone) {
        DLL_Log("[VideoEncoder] Pre-warming device and codec (%dx%d)...", width, height);
        auto prewarmStart = PerfTimer::now();

        if (!EnsureDevice()) {
            DLL_Log("[VideoEncoder] Pre-warm failed, will retry on first frame");
        } else {
            auto prewarmEnd = PerfTimer::now();
            double prewarmMs = PerfTimer::elapsed_ms(prewarmStart, prewarmEnd);
            DLL_Log(
                "[VideoEncoder] Pre-warm complete in %.2fms (device init, codec "
                "open)",
                prewarmMs);
        }
    }

    if (!outputReservation) {
        outputReservation = ReserveOutputStagingFile(savedConfig);
        if (!outputReservation) {
            return false;
        }
        outputFilename = outputReservation.Utf8Path();
        DLL_Log("[VideoEncoder] Reserved staging output for recording: %s", outputFilename.c_str());
    }

    BeginDeferredRecording();

    return true;
}

void VideoEncoder::WriteFrame(AVPacket* pkt) {
    if (!fileOpened || !fmtCtx)
        return;

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

        if (audioPacketCount++ % 100 == 0) {
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

        // DEBUG LEAK: Log queue stats every 100 video frames
        if (vidDebugCount % 100 == 0) {
            size_t qBytes = currentQueueBytes.load();
            size_t qSize = 0;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                qSize = packetQueue.size();
            }
            DLL_Log("[VideoEncoder] QUEUE STATS: Count=%zu Bytes=%zu (Max=%zu)", qSize, qBytes, MAX_QUEUE_BYTES);

            // Memory safety check
            if (qBytes > MAX_QUEUE_BYTES) {
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

    // IMPORTANT: Never drop encoded packets, it causes visible corruption.
    // Instead apply backpressure to the encode thread.
    // If storage is extremely slow, this will manifest as stutter/dropped input
    // frames (FrameQueue will drop/duplicate), but the bitstream stays valid.
    uint64_t backpressureWaitUs = 0;
    for (;;) {
        size_t qBytes = currentQueueBytes.load(std::memory_order_relaxed);
        if (qBytes <= MAX_QUEUE_BYTES) {
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
            return currentQueueBytes.load(std::memory_order_relaxed) <= MAX_QUEUE_BYTES || isStopping || !writerRunning;
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
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            packetQueue.push(clonePkt);
            currentQueueBytes += clonePkt->size + sizeof(AVPacket);
            currentQueuePackets.store(SaturatingToUint32(packetQueue.size()), std::memory_order_relaxed);
        }
        UpdateAtomicPeak(peakQueueBytes, SaturatingToUint32(currentQueueBytes.load(std::memory_order_relaxed)));
        UpdateAtomicPeak(peakQueuePackets, currentQueuePackets.load(std::memory_order_relaxed));
        PublishRuntimeState();
        queueCV.notify_one();
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

bool VideoEncoder::EncodeFrame(HANDLE sharedHandle, HANDLE fenceHandle, uint64_t fenceValue, int64_t timestamp,
                               uint32_t sourcePid, int width, int height, int format, bool isHDR, bool isShmem,
                               int shmemSlot) {
    if (!recordingRequested)
        return false;

    lastFrameDeferred.store(false, std::memory_order_relaxed);

    // Debug: Log every 60th frame entry to verify loop
    if (encodeFrameCounter % 60 == 0) {
        DLL_Log("[VideoEncoder] EncodeFrame Entry: PID=%u Handle=%p FenceVal=%llu", sourcePid, sharedHandle,
                fenceValue);
    }

    const bool wants10BitInput =
        isHDR || ce::video_format::IsHighPrecisionRgbInputFormat(static_cast<DXGI_FORMAT>(format));
    if (!initDone || isHDR != currentIsHDR || wants10BitInput != currentUse10BitInput) {
        const bool reinitializingActiveRecording = initDone;
        const std::string preservedOutputFilename = outputFilename;
        auto preservedOutputReservation = std::move(outputReservation);
        if (reinitializingActiveRecording) {
            DLL_Log("[VideoEncoder] Format mode changed (hdr=%d->%d use10bit=%d->%d). Re-initializing...", currentIsHDR,
                    isHDR, currentUse10BitInput, wants10BitInput);
            Stop();  // Clean up existing encoder
            initDone = false;
            // Also need to clear codecOpenFailed?
            codecOpenFailed = false;
        }

        currentIsHDR = isHDR;
        currentUse10BitInput = wants10BitInput;
        // Re-Init with saved config (Init uses currentIsHDR to pick format)
        if (!Init(savedConfig, width, height, savedConfig.fps ? savedConfig.fps : 60, onPacket)) {
            DLL_Log("[VideoEncoder] Failed to Re-Init for format mode change");
            return false;
        }
        if (reinitializingActiveRecording) {
            if (!preservedOutputFilename.empty()) {
                outputReservation = std::move(preservedOutputReservation);
                outputFilename = preservedOutputFilename;
                DLL_Log("[VideoEncoder] Preserving output filename across format mode re-init: %s",
                        outputFilename.c_str());
            }
            BeginDeferredRecording();
        } else if (!preservedOutputFilename.empty()) {
            outputReservation = std::move(preservedOutputReservation);
            outputFilename = preservedOutputFilename;
            DLL_Log("[VideoEncoder] Restored deferred staging output for first frame: %s",
                    outputFilename.c_str());
            BeginDeferredRecording();
        }
    }

    // Use captured frame dimensions if not yet set or changed
    if (this->width != width || this->height != height) {
        if (this->width == 0) {
            DLL_Log("[VideoEncoder] Initial resolution discovered: %dx%d (Input: %dx%d)", width, height, width, height);
        } else {
            DLL_Log("[VideoEncoder] Resolution CHANGE detected: %dx%d -> %dx%d", this->width, this->height, width,
                    height);
            if (!fileOpened && initDone) {
                // Pre-warm used stale/wrong dimensions. Reset codec and container
                // so EnsureDevice() reinitializes them at the correct resolution
                // before the file header is written.
                DLL_Log("[VideoEncoder] Reinitializing encoder at correct resolution (pre-file-open)");
                CleanupVideoProcessor();
                avcodec_free_context(&codecCtx);
                if (d3d11FramesCtx) {
                    av_buffer_unref(&d3d11FramesCtx);
                    d3d11FramesCtx = nullptr;
                }
                stream = nullptr;
                if (fmtCtx) {
                    avformat_free_context(fmtCtx);
                    fmtCtx = nullptr;
                    AllocateOutputContextForContainer(&fmtCtx, savedConfig);
                }
                const AVCodec* c = avcodec_find_encoder_by_name(savedConfig.encoder.c_str());
                if (c)
                    codecCtx = avcodec_alloc_context3(c);
                audioStreamIndex = -1;
                initDone = false;
            }
        }
        this->width = width;
        this->height = height;
    }

    if (!EnsureDevice())
        return false;

    // Fall through to D3D11 path below

    if (!fileOpened) {
        DLL_Log("[VideoEncoder] Opening Output File: %s", outputFilename.c_str());
        if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
            if (!outputReservation.ReleaseToWriter()) {
                DLL_Log("[VideoEncoder] ERROR: Reserved output identity changed before mux open: %s",
                        outputFilename.c_str());
                return false;
            }
            // Use 256KB buffer for better performance on slow storage (HDD/network)
            // Default is 32KB which causes many small writes
            int ret = avio_open2(&fmtCtx->pb, outputFilename.c_str(), AVIO_FLAG_WRITE, nullptr, nullptr);
            if (ret < 0) {
                DLL_Log("Failed to open output file: %d", ret);
                return false;
            }

            // Allocate custom buffer (256KB) for improved write performance
            const int bufferSize = 256 * 1024;
            [[maybe_unused]] unsigned char* buffer = nullptr;
        }

        // Debug: Log stream info before write_header
        DLL_Log("[VideoEncoder] fmtCtx has %d streams before write_header", fmtCtx->nb_streams);
        for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
            AVStream* s = fmtCtx->streams[i];
            AVCodecParameters* cp = s->codecpar;
            DLL_Log(
                "[VideoEncoder] Stream %d: type=%d codec_id=%d w=%d h=%d "
                "extradata=%p extradata_size=%d",
                i, cp->codec_type, cp->codec_id, cp->width, cp->height, cp->extradata, cp->extradata_size);
        }

        // Pre-allocate space for MKV cues (seek index) at the front of the file.
        // Without this, cues are written at the END and many players can't seek
        // or show correct duration without reading the whole file first.
        if (fmtCtx->priv_data) {
            av_opt_set(fmtCtx->priv_data, "reserve_index_space", "2000000", 0);  // 2MB
        }
        if (!ce::media::RequireMicrosecondMatroskaTimestampPrecision(fmtCtx)) {
            DLL_Log("[VideoEncoder] ERROR: Matroska timestamp_precision=1000 is required but unavailable");
            if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
                const int closeResult = avio_closep(&fmtCtx->pb);
                if (closeResult < 0)
                    DLL_Log("[VideoEncoder] ERROR: Failed to close rejected output: %d", closeResult);
            }
            return false;
        }

        if (!ValidateFormatContextForHeader(fmtCtx)) {
            if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
                const int closeResult = avio_closep(&fmtCtx->pb);
                if (closeResult < 0)
                    DLL_Log("[VideoEncoder] ERROR: Failed to close invalid output context: %d", closeResult);
            }
            return false;
        }

        int ret = avformat_write_header(fmtCtx, nullptr);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            DLL_Log("Failed to write header: %d (%s)", ret, errbuf);
            if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
                const int closeResult = avio_closep(&fmtCtx->pb);
                if (closeResult < 0)
                    DLL_Log("[VideoEncoder] ERROR: Failed to close output after header failure: %d", closeResult);
            }
            return false;
        }

        // Log actual stream time_base after muxer init (MKV may override)
        DLL_Log("[VideoEncoder] Stream time_base after write_header: %d/%d (codec: %d/%d)", stream->time_base.num,
                stream->time_base.den, codecCtx->time_base.num, codecCtx->time_base.den);

        // Force header to hit disk immediately. This prevents 0KB files when
        // subsequent writes fail and makes I/O errors surface at the true failure
        // point.
        if (fmtCtx->pb) {
            avio_flush(fmtCtx->pb);
            if (fmtCtx->pb->error < 0) {
                DLL_Log("Failed to flush header: %d", fmtCtx->pb->error);
                if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
                    const int closeResult = avio_closep(&fmtCtx->pb);
                    if (closeResult < 0)
                        DLL_Log("[VideoEncoder] ERROR: Failed to close output after flush failure: %d", closeResult);
                }
                return false;
            }
        }
        fileOpened = true;
    }

    // Frame rate control is now handled by capture engine (time-based sampling)
    // We just encode every frame we receive using frame counter for CFR output
    inputFrameCount++;

    const int fpsLogIntervalFrames = (savedConfig.fps > 0) ? savedConfig.fps : 60;

    // Log frame stats periodically (about once per second at the configured FPS)
    // Detect new recording start (startPts is -1) and reset counters
    if (startPts < 0) {
        needsCounterReset = true;  // Mark that we need to reset on first frame
    }

    if (outputFrameCount - lastLogFrameCount >= fpsLogIntervalFrames) {
        if (startPts >= 0 && timestamp > startPts) {
            // Inject-mode timestamps are in microseconds.
            double elapsedSec = (double)(timestamp - startPts) / 1000000.0;
            double outputFps = (elapsedSec > 0.001) ? ((double)outputFrameCount / elapsedSec) : 0.0;
            DLL_Log("[FPS] Output: %.1f frames, %.1f fps over %.1fs", (double)outputFrameCount, outputFps, elapsedSec);
        }
        lastLogFrameCount = outputFrameCount;
    }

    // Reset counters on new recording
    if (needsCounterReset) {
        encodeFrameCounter = 0;
        lastLogFrameCount = 0;
        needsCounterReset = false;
        DLL_Log("[VideoEncoder] Reset frameCounter for new recording");
    }

    encodeFrameCounter++;

    // Performance timing for this frame
    FrameStats stats;
    stats.frameNumber = encodeFrameCounter;
    stats.ptsMs = RoundUsToMs(timestamp);

    // Calculate frame timing for smoothness analysis
    double expectedFrameMs = 1000.0 / codecCtx->framerate.num;
    if (g_lastFramePts >= 0) {
        stats.actualPtsDiff = RoundUsToMs(timestamp - g_lastFramePts);
        stats.expectedPtsDiff = RoundUsToMs(static_cast<int64_t>(expectedFrameMs * 1000.0));
    }

    auto frameStart = PerfTimer::now();

    ID3D11Texture2D* bgraTex = nullptr;
    ID3D11Fence* d3d11Fence = nullptr;
    int cacheSlot = -1;

    if (isShmem) {
        if (pShmem && pSharedMem && pSharedMem->GetShmemMappingCreated()) {
            // Shmem Path: Upload pixels to our owned texture
            int texIdx = 0;  // Reuse first shared capture texture (we own it)
            bgraTex = sharedCaptureTextures[texIdx];

            if (bgraTex) {
                // Validation of slot
                int slot = (shmemSlot >= 0 && shmemSlot < 2) ? shmemSlot : 0;
                uint8_t* pSrc = pShmem->GetData(slot);

                if (pSrc) {
                    D3D11_BOX box;
                    box.left = 0;
                    box.right = pSharedMem->GetWidth();  // Use current frame resolution
                    box.top = 0;
                    box.bottom = pSharedMem->GetHeight();
                    box.front = 0;
                    box.back = 1;

                    // We need a pitch. Use pSharedMem->width * 4 if not stored in
                    // ShmemBuffer Actually ShmemBuffer has pitch.
                    d3d11Context->UpdateSubresource(bgraTex, 0, &box, pSrc, pShmem->pitch, 0);
                }
                bgraTex->AddRef();     // For consistency with Release() below
                d3d11Fence = nullptr;  // No fence for shmem
            }
        }
    } else {
        // Check if layer told us to use our own encoder textures directly
        // (DXVK zero-copy path: layer imported our KMT handles into Vulkan)
        if (pSharedMem && pSharedMem->useEncoderTextures.load(std::memory_order_acquire) &&
            sharedCaptureTexturesCreated) {
            // Find which encoder texture matches by KMT handle
            int matchIdx = -1;
            for (int i = 0; i < ENCODER_TEXTURE_SLOT_COUNT; i++) {
                if (sharedCaptureKmtHandles[i] == sharedHandle) {
                    matchIdx = i;
                    break;
                }
            }
            if (matchIdx >= 0) {
                bgraTex = sharedCaptureTextures[matchIdx];
            }
            if (bgraTex) {
                bgraTex->AddRef();

                HANDLE directFenceHandle = fenceHandle;
                if ((!directFenceHandle || directFenceHandle == INVALID_HANDLE_VALUE) && pSharedMem) {
                    directFenceHandle = reinterpret_cast<HANDLE>(pSharedMem->encoderTextures.GetFenceHandle());
                }

                if (directFenceHandle && directFenceHandle != INVALID_HANDLE_VALUE && fenceValue > 0) {
                    HANDLE directFenceHandleAlt = NormalizeSourceHandleForWow64(directFenceHandle, sourcePid);
                    const bool hasDirectFenceAlt = (directFenceHandleAlt != directFenceHandle);

                    if (sourcePid > 0 && sourcePid == cachedSourcePid && cachedFenceHandle == directFenceHandle &&
                        cachedD3D11Fence) {
                        d3d11Fence = cachedD3D11Fence;
                        d3d11Fence->AddRef();
                    } else {
                        ce::HandleGuard hProcess(OpenProcess(PROCESS_DUP_HANDLE, FALSE, sourcePid));
                        HRESULT fenceHr = E_FAIL;
                        if (hProcess) {
                            ce::HandleGuard dupFence;
                            if (DuplicateHandle(hProcess.get(), directFenceHandle, GetCurrentProcess(),
                                                dupFence.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                                fenceHr = CallOpenSharedFence(d3d11Device, dupFence.get(), &d3d11Fence);
                            }
                            if (FAILED(fenceHr) && hasDirectFenceAlt) {
                                ce::HandleGuard dupFenceAlt;
                                if (DuplicateHandle(hProcess.get(), directFenceHandleAlt, GetCurrentProcess(),
                                                    dupFenceAlt.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                                    fenceHr = CallOpenSharedFence(d3d11Device, dupFenceAlt.get(), &d3d11Fence);
                                }
                            }
                        }
                        if (FAILED(fenceHr) && !g_HandleFailureCache.ShouldSkipFence(directFenceHandle)) {
                            fenceHr = CallOpenSharedFence(d3d11Device, directFenceHandle, &d3d11Fence);
                        }
                        if (FAILED(fenceHr) && hasDirectFenceAlt) {
                            fenceHr = CallOpenSharedFence(d3d11Device, directFenceHandleAlt, &d3d11Fence);
                        }

                        if (d3d11Fence) {
                            if (cachedD3D11Fence) {
                                cachedD3D11Fence->Release();
                            }
                            cachedD3D11Fence = d3d11Fence;
                            cachedD3D11Fence->AddRef();
                            cachedFenceHandle = directFenceHandle;
                            cachedSourcePid = sourcePid;
                        } else if (encodeFrameCounter < 20) {
                            DLL_Log(
                                "[VideoEncoder] Frame %d: Failed to open encoder-texture fence handle=%p value=%llu "
                                "pid=%u",
                                encodeFrameCounter, directFenceHandle, static_cast<unsigned long long>(fenceValue),
                                sourcePid);
                        }
                    }
                }
            }
            if (matchIdx >= 0 && encodeFrameCounter < 10) {
                DLL_Log(
                    "[VideoEncoder] Frame %d: Using encoder-owned texture[%d] directly (encoder fence=%p value=%llu)",
                    encodeFrameCounter, matchIdx, fenceHandle, static_cast<unsigned long long>(fenceValue));
            }
        }

        if (!bgraTex) {
            // Standard shared handle path
            HANDLE sharedHandleAlt = NormalizeSourceHandleForWow64(sharedHandle, sourcePid);
            HANDLE fenceHandleAlt = NormalizeSourceHandleForWow64(fenceHandle, sourcePid);
            const bool hasSharedAlt = (sharedHandleAlt != sharedHandle);
            const bool hasFenceAlt = (fenceHandleAlt != fenceHandle);

            // Check cache for valid fence and texture (Quad-Buffered Cache)
            // Texture caching works independently of fence (for D3D11 KMT path)
            cacheSlot = -1;
            bool skipFence = (fenceValue == 0 || fenceHandle == 0 || fenceHandle == INVALID_HANDLE_VALUE);
            bool fenceValid = !skipFence && (sourcePid > 0 && sourcePid == cachedSourcePid &&
                                             fenceHandle == cachedFenceHandle && cachedD3D11Fence);

            // For texture matching, we only need matching PID and handle
            // (fence-independent)
            bool pidMatches = (sourcePid > 0 && sourcePid == cachedSourcePid);

            // Search for cached texture by handle (works with or without fence)
            if (pidMatches) {
                for (int i = 0; i < SHARED_TEXTURE_SLOT_COUNT; i++) {
                    if (cachedTextureHandles[i] == sharedHandle && cachedSharedTextures[i]) {
                        cacheSlot = i;
                        break;
                    }
                }
            } else if (sourcePid > 0) {
                // New process -> Clear all cache
                for (int i = 0; i < SHARED_TEXTURE_SLOT_COUNT; i++) {
                    if (cachedSharedTextures[i]) {
                        cachedSharedTextures[i]->Release();
                        cachedSharedTextures[i] = nullptr;
                    }
                    cachedTextureHandles[i] = nullptr;
                }
                if (cachedD3D11Fence) {
                    cachedD3D11Fence->Release();
                    cachedD3D11Fence = nullptr;
                }
                cachedFenceHandle = nullptr;
                cachedSourcePid = sourcePid;  // Remember new PID
            }

            // ID3D11Texture2D *bgraTex = nullptr; // Moved up
            // ID3D11Fence *d3d11Fence = nullptr;   // Moved up

            if (cacheSlot >= 0) {
                // Full Cache Hit
                bgraTex = cachedSharedTextures[cacheSlot];
                d3d11Fence = cachedD3D11Fence;  // May be null for D3D11 KMT path (no fence)
                bgraTex->AddRef();
                if (d3d11Fence) {
                    d3d11Fence->AddRef();
                }

                if (encodeFrameCounter % kCacheLogIntervalFrames == 1) {
                    DLL_Log("[VideoEncoder] Using cached handles (pid=%u, slot=%d, frame=%d)", sourcePid, cacheSlot,
                            encodeFrameCounter);
                }
            } else {
                // Cache Miss (Partial or Full)
                // Use RAII to ensure handle is closed if we return early
                ce::HandleGuard hProcess(OpenProcess(PROCESS_DUP_HANDLE, FALSE, sourcePid));

                if (!hProcess) {
                    DLL_Log("[VideoEncoder] Frame %d: Failed to Open Process %u", encodeFrameCounter, sourcePid);
                    return false;
                }

                // 1. Handle Fence (Reuse if valid, Open if not)
                if (skipFence) {
                    d3d11Fence = nullptr;
                    if (encodeFrameCounter % 60 == 0)
                        DLL_Log("[VideoEncoder] Frame %d: SkipFence is true (Val=%llu Hnd=%p)", encodeFrameCounter,
                                fenceValue, fenceHandle);
                } else if (fenceValid) {
                    d3d11Fence = cachedD3D11Fence;
                    d3d11Fence->AddRef();
                } else {
                    ce::HandleGuard dupFence;
                    HRESULT hr = E_FAIL;

                    // CRITICAL: Always DuplicateHandle first to validate handles.
                    if (DuplicateHandle(hProcess.get(), fenceHandle, GetCurrentProcess(), dupFence.addressof(), 0,
                                        FALSE, DUPLICATE_SAME_ACCESS)) {
                        // Handle duplicated successfully - safe to call OpenSharedFence
                        hr = CallOpenSharedFence(d3d11Device, dupFence.get(), &d3d11Fence);
                        if (FAILED(hr) && encodeFrameCounter < 10) {
                            DLL_Log("[VideoEncoder] OpenSharedFence(dup) failed: HR=%x (Hnd=%p)", hr, dupFence.get());
                        }
                    } else {
                        DWORD err = GetLastError();
                        if (encodeFrameCounter < 10) {
                            DLL_Log("[VideoEncoder] DuplicateHandle failed: Err=%d (SrcPid=%u Hnd=%p)", err, sourcePid,
                                    fenceHandle);
                        }
                        // Last resort: try direct handle (may work for same-process or KMT handles)
                        if (!g_HandleFailureCache.ShouldSkipFence(fenceHandle)) {
                            hr = CallOpenSharedFence(d3d11Device, fenceHandle, &d3d11Fence);
                        }
                    }

                    // Alternate handle representation for WOW64 sources
                    if (FAILED(hr) && hasFenceAlt) {
                        ce::HandleGuard dupFenceAlt;
                        // Try direct first
                        hr = CallOpenSharedFence(d3d11Device, fenceHandleAlt, &d3d11Fence);
                        if (FAILED(hr)) {
                            if (DuplicateHandle(hProcess.get(), fenceHandleAlt, GetCurrentProcess(),
                                                dupFenceAlt.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                                hr = CallOpenSharedFence(d3d11Device, dupFenceAlt.get(), &d3d11Fence);
                            }
                        }
                    }

                    // Final fallback - try as generic shared resource
                    if (FAILED(hr)) {
                        hr = CallOpenSharedResource(d3d11Device, fenceHandle, IID_PPV_ARGS(&d3d11Fence));
                    }
                    if (FAILED(hr) && hasFenceAlt) {
                        hr = CallOpenSharedResource(d3d11Device, fenceHandleAlt, IID_PPV_ARGS(&d3d11Fence));
                    }
                    if (FAILED(hr) && encodeFrameCounter < 10) {
                        DLL_Log(
                            "[VideoEncoder] OpenSharedFence(direct) failed: HR=%x (Hnd=%p), trying "
                            "DuplicateHandle...",
                            hr, fenceHandle);
                    }

                    // Fallback to DuplicateHandle path (for handles that support it)
                    if (FAILED(hr)) {
                        if (DuplicateHandle(hProcess.get(), fenceHandle, GetCurrentProcess(), dupFence.addressof(), 0,
                                            FALSE, DUPLICATE_SAME_ACCESS)) {
                            hr = CallOpenSharedFence(d3d11Device, dupFence.get(), &d3d11Fence);
                            if (FAILED(hr)) {
                                DLL_Log("[VideoEncoder] OpenSharedFence(dup) failed: HR=%x (Hnd=%p)", hr,
                                        dupFence.get());
                            }
                        } else {
                            DWORD err = GetLastError();
                            DLL_Log(
                                "[VideoEncoder] DuplicateHandle failed: Err=%d (SrcPid=%u "
                                "Hnd=%p)",
                                err, sourcePid, fenceHandle);
                        }
                    }

                    // Alternate handle representation for WOW64 sources
                    if (FAILED(hr) && hasFenceAlt) {
                        ce::HandleGuard dupFenceAlt;
                        // Use DuplicateHandle first for safety
                        if (DuplicateHandle(hProcess.get(), fenceHandleAlt, GetCurrentProcess(),
                                            dupFenceAlt.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                            hr = CallOpenSharedFence(d3d11Device, dupFenceAlt.get(), &d3d11Fence);
                        }
                    }

                    // Final fallback - try as generic shared resource
                    if (FAILED(hr)) {
                        hr = CallOpenSharedResource(d3d11Device, fenceHandle, IID_PPV_ARGS(&d3d11Fence));
                    }
                    if (FAILED(hr) && hasFenceAlt) {
                        hr = CallOpenSharedResource(d3d11Device, fenceHandleAlt, IID_PPV_ARGS(&d3d11Fence));
                    }

                    if (d3d11Fence && encodeFrameCounter < 10) {
                        DLL_Log("[VideoEncoder] Successfully opened shared fence for PID %u", sourcePid);
                    }
                    // Cache Fence if successfully opened
                    if (d3d11Fence) {
                        if (cachedD3D11Fence)
                            cachedD3D11Fence->Release();
                        cachedD3D11Fence = d3d11Fence;
                        cachedD3D11Fence->AddRef();
                        cachedFenceHandle = fenceHandle;
                        cachedSourcePid = sourcePid;
                    }
                }

                // 2. Open Texture (We know it's missing if we are here)
                ce::HandleGuard dupTex;
                HRESULT hr = E_FAIL;
                HRESULT hrNtDirect = E_FAIL;
                HRESULT hrNtDup = E_FAIL;
                HRESULT hrKmtDup = E_FAIL;
                HRESULT hrNtAltDirect = E_FAIL;
                HRESULT hrNtAltDup = E_FAIL;
                HRESULT hrKmtAltDup = E_FAIL;
                HRESULT hrKmtDirect = E_FAIL;
                HRESULT hrKmtAltDirect = E_FAIL;

                if (encodeFrameCounter < 10) {
                    DLL_Log(
                        "[VideoEncoder] Frame %d: Opening shared texture: handle=%p, "
                        "sourcePid=%u (cached=%u, match=%s), format=%d",
                        encodeFrameCounter, sharedHandle, sourcePid, cachedSourcePid,
                        (sourcePid == cachedSourcePid) ? "yes" : "no", format);
                }

                if (sharedHandle == NULL) {
                    DLL_Log("[VideoEncoder] Frame %d: Error: sharedHandle is NULL", encodeFrameCounter);
                } else {
                    // D3D11 OpenSharedResource can throw SEH exceptions for invalid handles or
                    // incompatible formats. DuplicateHandle first to validate handle accessibility.
                    // Even duplicated handles can fail if D3D12/D3D11 devices are incompatible.
                    ce::HandleGuard dupTexDirect;
                    bool handleValid = DuplicateHandle(hProcess.get(), sharedHandle, GetCurrentProcess(),
                                                       dupTexDirect.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS);
                    if (handleValid) {
                        // Handle duplicated - try OpenSharedResource1 with the valid handle
                        hr = CallOpenSharedResource1(d3d11Device, dupTexDirect.get(), IID_PPV_ARGS(&bgraTex));
                        hrNtDup = hr;
                    } else {
                        hrNtDup = HRESULT_FROM_WIN32(GetLastError());
                        if (encodeFrameCounter < 10)
                            DLL_Log("[VideoEncoder] Frame %d: DuplicateHandle for texture failed: %p",
                                    encodeFrameCounter, sharedHandle);
                    }

                    if (FAILED(hr) && encodeFrameCounter < 10) {
                        DLL_Log(
                            "[VideoEncoder] Frame %d: OpenSharedResource1(direct=%p) "
                            "failed HR=%x. Trying KMT path...",
                            encodeFrameCounter, sharedHandle, hr);
                    }

                    // Fallback to KMT path with duplicated handle
                    if (FAILED(hr) && handleValid) {
                        hr = CallOpenSharedResource(d3d11Device, dupTexDirect.get(), IID_PPV_ARGS(&bgraTex));
                        hrKmtDup = hr;
                        if (SUCCEEDED(hr) && encodeFrameCounter < 10) {
                            DLL_Log("[VideoEncoder] Frame %d: Opened duplicated handle %p via KMT path",
                                    encodeFrameCounter, dupTexDirect.get());
                        }
                    }

                    // Try original handle as last resort (may work for same-process)
                    if (FAILED(hr) && !g_HandleFailureCache.ShouldSkipTexture(sharedHandle)) {
                        hr = CallOpenSharedResource(d3d11Device, sharedHandle, IID_PPV_ARGS(&bgraTex));
                        hrKmtDirect = hr;
                        if (SUCCEEDED(hr) && encodeFrameCounter < 10) {
                            DLL_Log("[VideoEncoder] Frame %d: Opened handle %p via KMT direct path", encodeFrameCounter,
                                    sharedHandle);
                        }
                    }

                    // WOW64 producers publish a 32-bit handle value in the
                    // shared ABI. Try its normalized representation once, but
                    // do not retry either representation after a sleep: ring
                    // publication already supplies the required ordering.
                    if (FAILED(hr) && hasSharedAlt) {
                        ce::HandleGuard dupTexAlt;
                        if (DuplicateHandle(hProcess.get(), sharedHandleAlt, GetCurrentProcess(), dupTexAlt.addressof(),
                                            0, FALSE, DUPLICATE_SAME_ACCESS)) {
                            hr = CallOpenSharedResource1(d3d11Device, dupTexAlt.get(), IID_PPV_ARGS(&bgraTex));
                            hrNtAltDup = hr;
                            if (FAILED(hr)) {
                                hr = CallOpenSharedResource(d3d11Device, dupTexAlt.get(), IID_PPV_ARGS(&bgraTex));
                                hrKmtAltDup = hr;
                            }
                        } else {
                            hrNtAltDup = HRESULT_FROM_WIN32(GetLastError());
                        }
                        if (FAILED(hr) && !g_HandleFailureCache.ShouldSkipTexture(sharedHandleAlt)) {
                            hr = CallOpenSharedResource(d3d11Device, sharedHandleAlt, IID_PPV_ARGS(&bgraTex));
                            hrKmtAltDirect = hr;
                        }
                    }

                    // Frame-ring publication uses release/acquire ordering, so
                    // a published handle cannot become more valid after an
                    // arbitrary sleep. Immediate retries only stalled the CFR
                    // encoder by up to 6 ms and repeated the same failing driver
                    // calls. Defer the frame to the existing bounded lineage
                    // retry path instead; a later publication/device state can
                    // then be observed without blocking the real-time thread.
                    if (FAILED(hr)) {
                        lastFrameDeferred.store(true, std::memory_order_relaxed);
                    }
                }  // end of else (sharedHandle != NULL)

                if (FAILED(hr)) {
                    static std::atomic<int> s_openDetailLogCount{0};
                    if (s_openDetailLogCount.fetch_add(1, std::memory_order_relaxed) < 16) {
                        DLL_Log(
                            "[VideoEncoder] Frame %d: Open detail h=%p alt=%p ntDir=%x ntDup=%x ntAltDir=%x "
                            "ntAltDup=%x "
                            "kmtDup=%x kmtAltDup=%x kmtDir=%x kmtAltDir=%x",
                            encodeFrameCounter, sharedHandle, sharedHandleAlt, hrNtDirect, hrNtDup, hrNtAltDirect,
                            hrNtAltDup, hrKmtDup, hrKmtAltDup, hrKmtDirect, hrKmtAltDirect);
                    }
                    DLL_Log(
                        "[VideoEncoder] Frame %d: Failed to OpenSharedResource (NT/KMT) "
                        "HR=%x, handle=%p, sourcePid=%u, format=%d",
                        encodeFrameCounter, hr, sharedHandle, sourcePid, format);
                    // Clean up fence if we opened it but failed texture
                    if (d3d11Fence) {
                        d3d11Fence->Release();
                    }
                    return false;
                }

                // Cache Texture
                // Find empty cache slot.
                int targetSlot = 0;
                for (int i = 0; i < SHARED_TEXTURE_SLOT_COUNT; i++) {
                    if (cachedTextureHandles[i] == nullptr) {
                        targetSlot = i;
                        break;
                    }
                    if (i == SHARED_TEXTURE_SLOT_COUNT - 1)
                        targetSlot = 0;  // Fallback to 0 if all full
                }

                if (cachedSharedTextures[targetSlot]) {
                    cachedSharedTextures[targetSlot]->Release();
                }

                cachedSharedTextures[targetSlot] = bgraTex;
                cachedSharedTextures[targetSlot]->AddRef();
                cachedTextureHandles[targetSlot] = sharedHandle;

                // hProcess, dupTex, dupFence are auto-closed by RAII here
                cacheSlot = targetSlot;
            }
        }  // End of if (!bgraTex) - standard shared handle path
    }  // End of isShmem else block

    // 2. Wait on Synchronization using D3D11 Fence
    // PROTECTED: Immediate Context access
    D3D11ScopedLock lock;

    HRESULT hr = S_OK;

    // D3D11 FENCE PATH (Async GPU sync)
    auto beforeFence = PerfTimer::now();
    if (d3d11Fence) {
        // CPU-side timeout to prevent GPU hangs (Resilience improvement)
        if (!fenceEvent) {
            fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }

        // Check if fence is already satisfied (avoid SetEvent overhead if possible)
        if (d3d11Fence->GetCompletedValue() < fenceValue) {
            d3d11Fence->SetEventOnCompletion(fenceValue, fenceEvent);
            // Non-blocking fence check: poll with 0ms timeout instead of
            // blocking the encoder thread. At 100% GPU, the fence may take
            // 1-5ms to signal — blocking for 200ms collapses the cadence.
            // If the fence isn't ready, we skip this frame (return false)
            // and the Bresenham produces a duplicate. Stutter > corruption.
            DWORD waitRes = WaitForSingleObject(fenceEvent, 0);
            if (waitRes == WAIT_TIMEOUT) {
                // Fence not ready — skip this frame, encoder thread stays responsive
                bgraTex->Release();
                d3d11Fence->Release();
                d3d11Fence = nullptr;
                lastFrameDeferred.store(true, std::memory_order_relaxed);
                return false;
            }
        }

        // Async GPU Wait (plus CPU timeout check above)
        d3d11Context->Wait(d3d11Fence, fenceValue);
    }
    auto afterFence = PerfTimer::now();
    stats.fenceWaitMs = PerfTimer::elapsed_ms(beforeFence, afterFence);

    if (d3d11Fence) {
        d3d11Fence->Release();
        d3d11Fence = nullptr;
    }

    if (FAILED(hr)) {
        DLL_Log("[VideoEncoder] Frame %d: Failed to Wait on Fence. HR=%x", encodeFrameCounter, hr);
        bgraTex->Release();
        return false;
    }

    auto afterOpen = PerfTimer::now();
    const AVPixelFormat activeSwFormat = GetActiveD3D11SwFormat();
    const bool useDirectRgbPath = IsDirectRgbD3D11SwFormat(activeSwFormat);

    // 4. Ensure Video Processor is initialized before RGB -> YUV conversion.
    if (!useDirectRgbPath && !videoProcessorInit) {
        if (!InitVideoProcessor()) {
            DLL_Log("[VideoEncoder] Frame %d: VP init failed", encodeFrameCounter);
            bgraTex->Release();
            return false;
        }
    }

    auto beforeConvert = PerfTimer::now();
    AVFrame* d3d11Frame = av_frame_alloc();
    if (!d3d11Frame) {
        bgraTex->Release();
        return false;
    }
    d3d11Frame->format = AV_PIX_FMT_D3D11;
    d3d11Frame->width = codecCtx->width;
    d3d11Frame->height = codecCtx->height;
    d3d11Frame->hw_frames_ctx = av_buffer_ref(d3d11FramesCtx);

    if (useDirectRgbPath) {
        const int frameRet = av_hwframe_get_buffer(d3d11FramesCtx, d3d11Frame, 0);
        if (frameRet < 0 || !d3d11Frame->data[0]) {
            DLL_Log("[VideoEncoder] Frame %d: Failed to allocate D3D11 HW frame for direct RGB path: %d",
                    encodeFrameCounter, frameRet);
            bgraTex->Release();
            av_frame_free(&d3d11Frame);
            return false;
        }

        if (!PrepareD3D11TextureForEncode(bgraTex, (ID3D11Texture2D*)d3d11Frame->data[0], CursorCompositionActive(), 0,
                                          0)) {
            DLL_Log("[VideoEncoder] Frame %d: Direct D3D11 RGB preparation failed", encodeFrameCounter);
            bgraTex->Release();
            av_frame_free(&d3d11Frame);
            return false;
        }
    } else {
        // 6. Point-composite a separate cursor in RGB, then convert the one
        // deterministic RGB stream to NV12/P010 on the GPU.
        if (!ConvertBGRAtoNV12(bgraTex, d3d11Frame, CursorCompositionActive(), true)) {
            DLL_Log("[VideoEncoder] Frame %d: GPU color conversion failed", encodeFrameCounter);
            bgraTex->Release();
            av_frame_free(&d3d11Frame);
            return false;
        }

        d3d11Frame->width = scalingEnabled ? outputWidth : width;
        d3d11Frame->height = scalingEnabled ? outputHeight : height;
    }

    auto afterConvert = PerfTimer::now();
    stats.colorConvertMs = PerfTimer::elapsed_ms(beforeConvert, afterConvert);
    bgraTex->Release();
    ApplyFrameColorMetadata(d3d11Frame, codecCtx);

    // Calculate relative PTS (start from 0) — timestamp is in microseconds
    const bool commitsStartPts = startPts.load(std::memory_order_relaxed) < 0;
    const int64_t effectiveStartPts = commitsStartPts ? timestamp : startPts.load(std::memory_order_relaxed);
    const int64_t targetPts = ComputeTargetVideoPts(timestamp, savedConfig.useVFR, savedConfig.fps, effectiveStartPts,
                                                    lastAssignedVideoPts, false);

    // 5. Encode (Direct D3D11 Frame) - with proper packet draining
    AVPacket* pkt = av_packet_alloc();
    int packetCount = 0;

    // Pure encoding time measurement (excluding color conversion/wait)
    auto encodeStart = PerfTimer::now();

    // Helper lambda to drain all available packets
    auto drainPackets = [&]() {
        while (true) {
            const auto receiveStart = PerfTimer::now();
            int ret = avcodec_receive_packet(codecCtx, pkt);
            const auto receiveEnd = PerfTimer::now();
            encoderReceiveAccumUs +=
                static_cast<uint64_t>(std::max(0.0, PerfTimer::elapsed_ms(receiveStart, receiveEnd) * 1000.0));
            ++encoderReceiveCalls;
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
                DLL_Log("[VideoEncoder] avcodec_receive_packet failed: %d (%s)", ret, errbuf);
                break;
            }

            packetCount++;
            const int64_t packetKey = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
            const auto submitIt = encoderSubmitQpcByPts.find(packetKey);
            if (submitIt != encoderSubmitQpcByPts.end()) {
                LARGE_INTEGER nowQpc = {};
                LARGE_INTEGER frequency = {};
                QueryPerformanceCounter(&nowQpc);
                QueryPerformanceFrequency(&frequency);
                if (frequency.QuadPart > 0 && nowQpc.QuadPart >= submitIt->second) {
                    const uint64_t latencyUs = static_cast<uint64_t>(nowQpc.QuadPart - submitIt->second) * 1000000ull /
                                               static_cast<uint64_t>(frequency.QuadPart);
                    encoderPacketLatencyAccumUs += latencyUs;
                    ++encoderPacketLatencySamples;
                    encoderPacketLatencyMaxUs = std::max(encoderPacketLatencyMaxUs, SaturatingToUint32(latencyUs));
                }
                encoderSubmitQpcByPts.erase(submitIt);
            }
            pkt->stream_index = stream->index;  // Ensure video stream index

            // Duration Logic
            if (savedConfig.useVFR) {
                // For VFR, duration is variable. Best guess is target frame duration.
                // Since time_base is 1us, duration is in us.
                pkt->duration = 1000000 / savedConfig.fps;
            } else {
                // For CFR, duration is 1 unit (1/FPS)
                pkt->duration = 1;
            }

            if (onPacket)
                onPacket(pkt);
            av_packet_unref(pkt);
        }
    };

    auto sendFrame = [&](AVFrame* frame) -> bool {
        drainPackets();
        auto timedSend = [&](AVFrame* sendTarget) {
            const auto sendStart = PerfTimer::now();
            const int result = avcodec_send_frame(codecCtx, sendTarget);
            const auto sendEnd = PerfTimer::now();
            encoderSendAccumUs +=
                static_cast<uint64_t>(std::max(0.0, PerfTimer::elapsed_ms(sendStart, sendEnd) * 1000.0));
            ++encoderSendCalls;
            return result;
        };
        int ret = timedSend(frame);
        int retries = 0;
        if (ret == AVERROR(EAGAIN))
            ++encoderEagainDrainCount;
        while (ret == AVERROR(EAGAIN) && retries < 10) {
            if (retries == 0) {
                lastEncoderOverloadTickMs.store(GetTickCount64(), std::memory_order_relaxed);
                PublishRuntimeState();
            }
            drainPackets();
            ret = timedSend(frame);
            retries++;
        }
        if (ret == AVERROR(EAGAIN)) {
            DLL_Log("[VideoEncoder] avcodec_send_frame remained EAGAIN after %d drain attempts", retries);
            return false;
        }
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
            DLL_Log("[VideoEncoder] avcodec_send_frame failed: %d (%s)", ret, errbuf);
            return false;
        }
        if (frame && frame->pts != AV_NOPTS_VALUE) {
            LARGE_INTEGER submitted = {};
            QueryPerformanceCounter(&submitted);
            encoderSubmitQpcByPts[frame->pts] = submitted.QuadPart;
            while (encoderSubmitQpcByPts.size() > 256)
                encoderSubmitQpcByPts.erase(encoderSubmitQpcByPts.begin());
        }
        drainPackets();
        return true;
    };

    bool success = true;

    d3d11Frame->pts = targetPts;

    if (success) {
        success = sendFrame(d3d11Frame);
    }

    const uint64_t timingNow = GetTickCount64();
    if (encoderTimingLastLogTick == 0 || timingNow - encoderTimingLastLogTick >= 1000) {
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

    auto afterEncode = PerfTimer::now();
    stats.ptsMs = RoundUsToMs(timestamp);
    stats.textureOpenMs = PerfTimer::elapsed_ms(frameStart, afterOpen);
    stats.colorConvertMs = PerfTimer::elapsed_ms(afterOpen, afterConvert);
    stats.encodeMs = PerfTimer::elapsed_ms(encodeStart, afterEncode);
    stats.totalMs = PerfTimer::elapsed_ms(frameStart, afterEncode);

    // Update last frame encode time (in microseconds)
    // This is robust against timer noise/underflow compared to (Total - Wait).
    lastEncodeTimeUs = (int64_t)(PerfTimer::elapsed_ms(afterFence, afterEncode) * 1000.0);
    lastFenceWaitUs = (int64_t)(stats.fenceWaitMs * 1000.0);
    stats.packetsProduced = packetCount;

    av_packet_free(&pkt);

    if (!success) {
        av_frame_free(&d3d11Frame);
        return false;
    }

    if (commitsStartPts) {
        startPts.store(effectiveStartPts, std::memory_order_relaxed);
        DLL_Log("[VideoEncoder] Recording started at PTS %lld us", static_cast<long long>(effectiveStartPts));
    }
    g_lastFramePts = timestamp;
    lastAssignedVideoPts = d3d11Frame->pts;

    // Update global stats
    g_framesEncoded++;
    outputFrameCount++;
    CacheRepeatFrameTexture(reinterpret_cast<ID3D11Texture2D*>(d3d11Frame->data[0]));
    g_totalFenceWait += stats.fenceWaitMs;
    g_totalColorConvert += stats.colorConvertMs;
    g_totalEncode += stats.encodeMs;
    if (stats.totalMs > g_maxFrameTime)
        g_maxFrameTime = stats.totalMs;
    if (stats.totalMs > expectedFrameMs * 2)
        g_slowFrameCount++;

    // Log individual slow frames for debugging
    // Log more frequently for performance tuning (every 30 frames)
    if (stats.totalMs > expectedFrameMs * 2 || encodeFrameCounter <= 5 || encodeFrameCounter % 30 == 0) {
        std::string features = "";
        if (IsConfiguredNvencLookaheadActive(savedConfig.lookahead))
            features += "Lookahead ";
        if (savedConfig.spatialAq || savedConfig.temporalAq)
            features += "AQ ";
        if (savedConfig.bFrames > 0)
            features += "B-Frames ";
        if (IsConfiguredNvencMultipassActive(savedConfig))
            features += "Multipass ";

        const char* slowLabel = (stats.totalMs > expectedFrameMs * 2) ? "(SLOW!)" : "";

        DLL_Log(
            "[PERF] Frame %d: TOTAL=%.2fms %s fence=%.2f convert=%.2f "
            "encode=%.2f pts=%lldms packets=%d [Features: %s] timing=cpu-wall-or-submit",
            encodeFrameCounter, stats.totalMs, slowLabel, stats.fenceWaitMs, stats.colorConvertMs, stats.encodeMs,
            stats.ptsMs, stats.packetsProduced, features.c_str());
    }

    // Periodic performance summary (about once per second at the configured FPS)
    if (encodeFrameCounter % fpsLogIntervalFrames == 0) {
        double avgFence = g_totalFenceWait / g_framesEncoded;
        double avgConvert = g_totalColorConvert / g_framesEncoded;
        double avgEncode = g_totalEncode / g_framesEncoded;
        double avgTotal = avgFence + avgConvert + avgEncode;

        // Identify bottleneck
        const char* bottleneck = "ENCODE";
        double maxTime = avgEncode;
        if (avgFence > maxTime) {
            bottleneck = "FENCE_WAIT";
            maxTime = avgFence;
        }
        if (avgConvert > maxTime) {
            bottleneck = "COLOR_CONV";
            maxTime = avgConvert;
        }

        DLL_Log(
            "[PERF SUMMARY] Frames=%lld Avg: total=%.2fms fence=%.2f "
            "convert=%.2f "
            "encode=%.2f | Max=%.2fms SlowFrames=%d | Bottleneck=%s | timing=cpu-wall-or-submit",
            g_framesEncoded, avgTotal, avgFence, avgConvert, avgEncode, g_maxFrameTime, g_slowFrameCount, bottleneck);

        // Frame timing analysis for smoothness
        if (stats.actualPtsDiff > 0) {
            const double jitter = static_cast<double>(stats.actualPtsDiff - stats.expectedPtsDiff);
            DLL_Log("[SMOOTHNESS] Expected=%0.2fms Actual=%0.2fms Jitter=%0.2fms",
                    static_cast<double>(stats.expectedPtsDiff), static_cast<double>(stats.actualPtsDiff), jitter);
        }
    }

    av_frame_free(&d3d11Frame);  // Releases D3D11 Tex

    return true;
}

// EncodeFrameD3D11: Direct D3D11 texture encoding for framegrab
// mode Zero-copy path - texture is converted RGB/BGRA -> NV12/P010 directly on GPU
bool VideoEncoder::PrepareFrameD3D11(ID3D11Texture2D* bgraTexture, uint32_t frameWidth, uint32_t frameHeight,
                                     bool isHDR) {
    if (!recordingRequested)
        return false;

    ReleaseInjectDeviceStateForScreenGrab();

    // Use captured frame dimensions if not yet set
    if (width == 0 || height == 0) {
        width = (int)frameWidth;
        height = (int)frameHeight;
        DLL_Log("[VideoEncoder] Framegrab using dimensions: %dx%d", width, height);
    }

    // Ensure D3D11 device is available (we need it for Video
    // Processor)
    if (!d3d11Device || !d3d11Context) {
        if (!AdoptTextureDevice(bgraTexture)) {
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    bgraTexture->GetDesc(&texDesc);
    const bool wants10BitInput = ce::video_format::IsHighPrecisionRgbInputFormat(texDesc.Format);
    if (!initDone || isHDR != currentIsHDR || wants10BitInput != currentUse10BitInput) {
        const bool reinitializingActiveRecording = initDone;
        const std::string preservedOutputFilename = outputFilename;
        auto preservedOutputReservation = std::move(outputReservation);
        if (reinitializingActiveRecording) {
            DLL_Log("[VideoEncoder] WGC mode changed (fmt=%d hdr=%d->%d use10bit=%d->%d). Re-initializing...",
                    texDesc.Format, currentIsHDR, isHDR, currentUse10BitInput, wants10BitInput);
            Stop();
            initDone = false;
            codecOpenFailed = false;
        }

        currentIsHDR = isHDR;
        currentUse10BitInput = wants10BitInput;
        use10BitPipeline = ShouldUse10BitOutput();
        if (!Init(savedConfig, width ? width : (int)frameWidth, height ? height : (int)frameHeight,
                  savedConfig.fps ? savedConfig.fps : 60, onPacket)) {
            DLL_Log("[VideoEncoder] Failed to Re-Init for WGC format mode change");
            return false;
        }
        if (!AdoptTextureDevice(bgraTexture)) {
            DLL_Log("[VideoEncoder] Failed to adopt WGC texture device after format mode change");
            return false;
        }
        if (reinitializingActiveRecording) {
            if (!preservedOutputFilename.empty()) {
                outputReservation = std::move(preservedOutputReservation);
                outputFilename = preservedOutputFilename;
                DLL_Log("[VideoEncoder] Preserving output filename across WGC mode re-init: %s",
                        outputFilename.c_str());
            }
            BeginDeferredRecording();
        } else if (!preservedOutputFilename.empty()) {
            outputReservation = std::move(preservedOutputReservation);
            outputFilename = preservedOutputFilename;
            DLL_Log("[VideoEncoder] Restored deferred staging output for first WGC frame: %s",
                    outputFilename.c_str());
            BeginDeferredRecording();
        }
    }

    // Ensure encoder is initialized with hardware context
    if (!EnsureDevice())
        return false;

    if (!fileOpened) {
        DLL_Log("[VideoEncoder] Opening Output File: %s", outputFilename.c_str());
        if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
            if (!outputReservation.ReleaseToWriter()) {
                DLL_Log("[VideoEncoder] ERROR: Reserved output identity changed before WGC mux open: %s",
                        outputFilename.c_str());
                return false;
            }
            int ret = avio_open(&fmtCtx->pb, outputFilename.c_str(), AVIO_FLAG_WRITE);
            if (ret < 0) {
                DLL_Log("Failed to open output file: %d", ret);
                return false;
            }
        }
        if (fmtCtx->priv_data) {
            av_opt_set(fmtCtx->priv_data, "reserve_index_space", "2000000", 0);
        }
        if (!ce::media::RequireMicrosecondMatroskaTimestampPrecision(fmtCtx)) {
            DLL_Log("[VideoEncoder] ERROR: Matroska timestamp_precision=1000 is required but unavailable");
            if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
                const int closeResult = avio_closep(&fmtCtx->pb);
                if (closeResult < 0)
                    DLL_Log("[VideoEncoder] ERROR: Failed to close rejected WGC output: %d", closeResult);
            }
            return false;
        }
        if (!ValidateFormatContextForHeader(fmtCtx)) {
            if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
                const int closeResult = avio_closep(&fmtCtx->pb);
                if (closeResult < 0)
                    DLL_Log("[VideoEncoder] ERROR: Failed to close invalid WGC output context: %d", closeResult);
            }
            return false;
        }

        const int headerResult = avformat_write_header(fmtCtx, nullptr);
        if (headerResult < 0) {
            DLL_Log("Failed to write header: %d", headerResult);
            if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
                const int closeResult = avio_closep(&fmtCtx->pb);
                if (closeResult < 0)
                    DLL_Log("[VideoEncoder] ERROR: Failed to close WGC output after header failure: %d", closeResult);
            }
            return false;
        }
        fileOpened = true;
    }

    return true;
}

bool VideoEncoder::EncodeFrameD3D11(ID3D11Texture2D* bgraTexture, int64_t pts, uint32_t frameWidth,
                                    uint32_t frameHeight, bool isHDR, int32_t captureLeft, int32_t captureTop,
                                    bool useExplicitCfrTimeline) {
    if (!recordingRequested)
        return false;

    inputFrameCount++;

    if (!PrepareFrameD3D11(bgraTexture, frameWidth, frameHeight, isHDR)) {
        return false;
    }

    // Detect new recording start and reset counters (using class members)
    if (startPts < 0) {
        needsCounterReset = true;
    }

    // Reset counters on new recording
    if (needsCounterReset) {
        encodeFrameCounter = 0;
        lastLogFrameCount = 0;
        needsCounterReset = false;
        DLL_Log(
            "[VideoEncoder] ScreenGrab: Reset encodeFrameCounter for new "
            "recording");
    }

    encodeFrameCounter++;

    FrameStats stats;
    stats.frameNumber = encodeFrameCounter;
    stats.ptsMs = RoundUsToMs(pts);

    double expectedFrameMs = 1000.0 / codecCtx->framerate.num;
    if (g_lastFramePts >= 0) {
        stats.actualPtsDiff = RoundUsToMs(pts - g_lastFramePts);
        stats.expectedPtsDiff = RoundUsToMs(static_cast<int64_t>(expectedFrameMs * 1000.0));
    }

    const int fpsLogIntervalFrames = (savedConfig.fps > 0) ? savedConfig.fps : 60;

    // Log frame stats periodically (about once per second at the configured FPS)
    if (encodeFrameCounter - lastLogFrameCount >= fpsLogIntervalFrames) {
        if (startPts >= 0 && pts > startPts) {
            double elapsedSec = static_cast<double>(pts - startPts) / 1000000.0;
            double outputFps = (elapsedSec > 0.001) ? ((double)encodeFrameCounter / elapsedSec) : 0.0;
            DLL_Log("[FPS] Framegrab: %.1f frames, %.1f fps over %.1fs", (double)encodeFrameCounter, outputFps,
                    elapsedSec);
        }
        lastLogFrameCount = encodeFrameCounter;
    }

    // Performance timing
    auto frameStart = PerfTimer::now();
    const AVPixelFormat activeSwFormat = GetActiveD3D11SwFormat();
    const bool useDirectRgbPath = IsDirectRgbD3D11SwFormat(activeSwFormat);

    if (!useDirectRgbPath && captureCursor && !videoProcessorInit) {
        if (!InitVideoProcessor()) {
            DLL_Log("[VideoEncoder] Frame %d: VP init failed", encodeFrameCounter);
            return false;
        }
    }

    const bool recomposeCursorForRepeats = CursorCompositionActive() && cursorRenderer;
    if (!recomposeCursorForRepeats && repeatSourceNeedsCursorRecompose) {
        InvalidateRepeatSourceFrameTexture();
    }

    auto beforeConvert = PerfTimer::now();
    AVFrame* d3d11Frame = av_frame_alloc();
    if (!d3d11Frame) {
        return false;
    }
    d3d11Frame->format = AV_PIX_FMT_D3D11;
    d3d11Frame->width = codecCtx->width;
    d3d11Frame->height = codecCtx->height;
    d3d11Frame->hw_frames_ctx = av_buffer_ref(d3d11FramesCtx);

    if (useDirectRgbPath) {
        const int frameRet = av_hwframe_get_buffer(d3d11FramesCtx, d3d11Frame, 0);
        if (frameRet < 0 || !d3d11Frame->data[0]) {
            DLL_Log("[VideoEncoder] Frame %d: Failed to allocate D3D11 HW frame for direct RGB path: %d",
                    encodeFrameCounter, frameRet);
            av_frame_free(&d3d11Frame);
            return false;
        }

        if (!PrepareD3D11TextureForEncode(bgraTexture, (ID3D11Texture2D*)d3d11Frame->data[0], CursorCompositionActive(),
                                          captureLeft, captureTop, true, 1)) {
            DLL_Log("[VideoEncoder] Frame %d: Direct D3D11 RGB preparation failed", encodeFrameCounter);
            av_frame_free(&d3d11Frame);
            return false;
        }
    } else {
        // WGC/DXGI/inject keep a hardware cursor separate whenever Windows
        // permits it. Point-composite that cursor into RGB before the single
        // VP conversion so its filtering matches a Windows-embedded cursor.
        // Scoped Lock for D3D11 Immediate Context (protects Blt/CopyResource)
        bool convertSuccess =
            ConvertBGRAtoNV12(bgraTexture, d3d11Frame, CursorCompositionActive(), true, captureLeft, captureTop, 1);

        if (!convertSuccess) {
            DLL_Log("[VideoEncoder] Frame %d: GPU color conversion failed", encodeFrameCounter);
            av_frame_free(&d3d11Frame);
            return false;
        }

        d3d11Frame->width = scalingEnabled ? outputWidth : width;
        d3d11Frame->height = scalingEnabled ? outputHeight : height;
    }

    auto afterConvert = PerfTimer::now();
    double convertMs = PerfTimer::elapsed_ms(beforeConvert, afterConvert);
    ApplyFrameColorMetadata(d3d11Frame, codecCtx);

    // Calculate PTS — pts is in microseconds
    const bool commitsStartPts = startPts.load(std::memory_order_relaxed) < 0;
    const int64_t effectiveStartPts = commitsStartPts ? pts : startPts.load(std::memory_order_relaxed);
    const int64_t targetPts = ComputeTargetVideoPts(pts, savedConfig.useVFR, savedConfig.fps, effectiveStartPts,
                                                    lastAssignedVideoPts, useExplicitCfrTimeline);

    // Encode
    AVPacket* pkt = av_packet_alloc();
    int packetCount = 0;

    auto drainPackets = [&]() {
        while (true) {
            int ret = avcodec_receive_packet(codecCtx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                break;
            packetCount++;
            pkt->stream_index = stream->index;

            // Debug: Log output packet PTS
            if (encodeFrameCounter < 30 || encodeFrameCounter % 1000 == 0) {
                DLL_Log(
                    "[Framegrab DEBUG] Received pkt: pts=%lld dts=%lld "
                    "size=%d "
                    "flags=%d",
                    pkt->pts, pkt->dts, pkt->size, pkt->flags);
            }

            // Set packet duration based on VFR/CFR mode (matches inject-mode logic)
            if (savedConfig.useVFR) {
                // VFR: time_base is 1/1000000, so duration is 1 frame in microseconds
                pkt->duration = 1000000 / (savedConfig.fps > 0 ? savedConfig.fps : 60);
            } else {
                // CFR: time_base is 1/fps, duration is 1 unit
                pkt->duration = 1;
            }

            if (onPacket)
                onPacket(pkt);
            av_packet_unref(pkt);
        }
    };

    auto sendFrame = [&](AVFrame* frame) -> bool {
        drainPackets();
        int ret = avcodec_send_frame(codecCtx, frame);
        int retries = 0;
        while (ret == AVERROR(EAGAIN) && retries < 10) {
            if (retries == 0) {
                lastEncoderOverloadTickMs.store(GetTickCount64(), std::memory_order_relaxed);
                PublishRuntimeState();
            }
            drainPackets();
            ret = avcodec_send_frame(codecCtx, frame);
            retries++;
        }
        if (ret == AVERROR(EAGAIN)) {
            DLL_Log("[VideoEncoder] ScreenGrab send_frame remained EAGAIN after %d drain attempts", retries);
            return false;
        }
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
            DLL_Log("[VideoEncoder] ScreenGrab send_frame failed: %d (%s)", ret, errbuf);
            return false;
        }
        drainPackets();
        return true;
    };

    bool success = true;

    d3d11Frame->pts = targetPts;

    // Debug: Log input frame PTS
    if (encodeFrameCounter < 20 || encodeFrameCounter % 1000 == 0) {
        DLL_Log("[Framegrab DEBUG] Sending frame %d with input PTS=%lld", encodeFrameCounter, d3d11Frame->pts);
    }

    if (success) {
        success = sendFrame(d3d11Frame);
        if (success) {
            lastAssignedVideoPts = d3d11Frame->pts;
            outputFrameCount++;
            CacheRepeatFrameTexture(reinterpret_cast<ID3D11Texture2D*>(d3d11Frame->data[0]));
            if (recomposeCursorForRepeats &&
                !CacheRepeatSourceFrameTexture(bgraTexture, frameWidth, frameHeight, isHDR, captureLeft, captureTop)) {
                repeatSourceNeedsCursorRecompose = false;
            }
        }
    }

    if (!success) {
        av_packet_free(&pkt);
        av_frame_free(&d3d11Frame);
        return false;
    }

    if (commitsStartPts) {
        startPts.store(effectiveStartPts, std::memory_order_relaxed);
        DLL_Log("[VideoEncoder] Framegrab recording started at PTS %lld us", static_cast<long long>(effectiveStartPts));
    }
    g_lastFramePts = pts;
    auto afterEncode = PerfTimer::now();
    double encodeMs = PerfTimer::elapsed_ms(afterConvert, afterEncode);
    double totalMs = PerfTimer::elapsed_ms(frameStart, afterEncode);

    lastEncodeTimeUs = static_cast<int64_t>(PerfTimer::elapsed_ms(beforeConvert, afterEncode) * 1000.0);
    lastFenceWaitUs = 0;

    av_packet_free(&pkt);

    // Log only severe slow frames individually. The per-second summary below captures
    // steady-state encode timing without flooding the log with routine single-frame spikes.
    if (totalMs > expectedFrameMs * 2.0) {
        std::string features = "";
        if (IsConfiguredNvencLookaheadActive(savedConfig.lookahead))
            features += "Lookahead ";
        if (savedConfig.spatialAq || savedConfig.temporalAq)
            features += "AQ ";
        if (savedConfig.bFrames > 0)
            features += "B-Frames ";
        if (IsConfiguredNvencMultipassActive(savedConfig))
            features += "Multipass ";

        DLL_Log(
            "[Framegrab PERF] Frame %d: total=%.2fms (%s) convert=%.2f "
            "encode=%.2f packets=%d [Features: %s] timing=cpu-wall-or-submit",
            encodeFrameCounter, totalMs, "SLOW!", convertMs, encodeMs, packetCount, features.c_str());
    }

    // Log periodic stats (about once per second at the configured FPS)
    if (encodeFrameCounter % fpsLogIntervalFrames == 0) {
        DLL_Log(
            "[Framegrab PERF] Frame %d: total=%.2fms convert=%.2f "
            "encode=%.2f packets=%d skipped=%lld duplicated=%lld timing=cpu-wall-or-submit",
            encodeFrameCounter, totalMs, convertMs, encodeMs, packetCount, skippedFrameCount, duplicatedFrameCount);
        if (stats.actualPtsDiff > 0) {
            const double jitter = static_cast<double>(stats.actualPtsDiff - stats.expectedPtsDiff);
            DLL_Log("[Framegrab SMOOTHNESS] Expected=%0.2fms Actual=%0.2fms Jitter=%0.2fms",
                    static_cast<double>(stats.expectedPtsDiff), static_cast<double>(stats.actualPtsDiff), jitter);
        }
    }

    av_frame_free(&d3d11Frame);
    return true;
}

bool VideoEncoder::RepeatLastFrame(int64_t timestamp, bool useExplicitCfrTimeline) {
    if (!recordingRequested) {
        return false;
    }

    lastFrameDeferred.store(false, std::memory_order_relaxed);

    const bool recomposeCursorForRepeat = repeatSourceNeedsCursorRecompose && repeatSourceFrameTexture;
    if (!repeatFrameTexture && !recomposeCursorForRepeat) {
        DLL_Log("[VideoEncoder] RepeatLastFrame requested without cached frame");
        return false;
    }

    if (!d3d11Device || !d3d11Context || !EnsureDevice()) {
        return false;
    }

    if (!fileOpened) {
        DLL_Log("[VideoEncoder] RepeatLastFrame requested before output file was opened");
        return false;
    }

    inputFrameCount++;

    const int fpsLogIntervalFrames = (savedConfig.fps > 0) ? savedConfig.fps : 60;
    if (startPts < 0) {
        startPts = timestamp;
        needsCounterReset = true;
    }
    if (outputFrameCount - lastLogFrameCount >= fpsLogIntervalFrames) {
        if (startPts >= 0 && timestamp > startPts) {
            double elapsedSec = static_cast<double>(timestamp - startPts) / 1000000.0;
            double outputFps = (elapsedSec > 0.001) ? (static_cast<double>(outputFrameCount) / elapsedSec) : 0.0;
            DLL_Log("[FPS] Output: %.1f frames, %.1f fps over %.1fs", static_cast<double>(outputFrameCount), outputFps,
                    elapsedSec);
        }
        lastLogFrameCount = outputFrameCount;
    }
    if (needsCounterReset) {
        encodeFrameCounter = 0;
        lastLogFrameCount = 0;
        needsCounterReset = false;
        DLL_Log("[VideoEncoder] Reset frameCounter for repeated-frame path");
    }

    encodeFrameCounter++;
    duplicatedFrameCount++;

    FrameStats stats;
    stats.frameNumber = encodeFrameCounter;
    stats.ptsMs = RoundUsToMs(timestamp);
    double expectedFrameMs = 1000.0 / codecCtx->framerate.num;
    if (g_lastFramePts >= 0) {
        stats.actualPtsDiff = RoundUsToMs(timestamp - g_lastFramePts);
        stats.expectedPtsDiff = RoundUsToMs(static_cast<int64_t>(expectedFrameMs * 1000.0));
    }

    // Re-encode the cached texture. Encoded packets are reference-dependent
    // bitstream units and cannot be replayed safely with rewritten timestamps.
    auto frameStart = PerfTimer::now();

    auto allocateD3D11RepeatFrame = [&]() -> AVFrame* {
        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            return nullptr;
        }
        frame->format = AV_PIX_FMT_D3D11;
        frame->width = codecCtx->width;
        frame->height = codecCtx->height;
        frame->hw_frames_ctx = av_buffer_ref(d3d11FramesCtx);
        return frame;
    };

    AVFrame* d3d11Frame = allocateD3D11RepeatFrame();
    if (!d3d11Frame) {
        return false;
    }

    auto beforeConvert = PerfTimer::now();
    bool populatedFromRepeatSource = false;
    if (recomposeCursorForRepeat) {
        populatedFromRepeatSource = PopulateD3D11FrameFromRepeatSource(d3d11Frame);
        if (!populatedFromRepeatSource) {
            if (!repeatCursorRecomposeFallbackLogged) {
                DLL_Log("[VideoEncoder] Cursor-aware repeat recompose failed; falling back to cached duplicate frame");
                repeatCursorRecomposeFallbackLogged = true;
            }
            av_frame_free(&d3d11Frame);
            if (!repeatFrameTexture) {
                return false;
            }
            d3d11Frame = allocateD3D11RepeatFrame();
            if (!d3d11Frame) {
                return false;
            }
        }
    }

    if (!populatedFromRepeatSource) {
        const int frameRet = av_hwframe_get_buffer(d3d11FramesCtx, d3d11Frame, 0);
        if (frameRet < 0 || !d3d11Frame->data[0]) {
            DLL_Log("[VideoEncoder] RepeatLastFrame failed to allocate D3D11 HW frame: %d", frameRet);
            av_frame_free(&d3d11Frame);
            return false;
        }

        {
            D3D11ScopedLock lock;
            d3d11Context->CopyResource(reinterpret_cast<ID3D11Texture2D*>(d3d11Frame->data[0]), repeatFrameTexture);
        }
    }

    auto afterConvert = PerfTimer::now();
    ApplyFrameColorMetadata(d3d11Frame, codecCtx);

    const int64_t targetPts = ComputeTargetVideoPts(timestamp, savedConfig.useVFR, savedConfig.fps, startPts,
                                                    lastAssignedVideoPts, useExplicitCfrTimeline);

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        av_frame_free(&d3d11Frame);
        return false;
    }

    int packetCount = 0;
    auto drainPackets = [&]() {
        while (true) {
            int ret = avcodec_receive_packet(codecCtx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
                DLL_Log("[VideoEncoder] RepeatLastFrame receive_packet failed: %d (%s)", ret, errbuf);
                break;
            }

            packetCount++;
            pkt->stream_index = stream->index;
            pkt->duration = savedConfig.useVFR ? (1000000 / std::max(savedConfig.fps, 1)) : 1;
            if (onPacket) {
                onPacket(pkt);
            }
            av_packet_unref(pkt);
        }
    };

    auto sendFrame = [&](AVFrame* frame) -> bool {
        drainPackets();
        int ret = avcodec_send_frame(codecCtx, frame);
        int retries = 0;
        while (ret == AVERROR(EAGAIN) && retries < 10) {
            if (retries == 0) {
                lastEncoderOverloadTickMs.store(GetTickCount64(), std::memory_order_relaxed);
                PublishRuntimeState();
            }
            drainPackets();
            ret = avcodec_send_frame(codecCtx, frame);
            retries++;
        }
        if (ret == AVERROR(EAGAIN)) {
            DLL_Log("[VideoEncoder] RepeatLastFrame send_frame remained EAGAIN after %d drain attempts", retries);
            return false;
        }
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
            DLL_Log("[VideoEncoder] RepeatLastFrame send_frame failed: %d (%s)", ret, errbuf);
            return false;
        }
        drainPackets();
        return true;
    };

    d3d11Frame->pts = targetPts;

    auto encodeStart = PerfTimer::now();
    const bool success = sendFrame(d3d11Frame);
    auto afterEncode = PerfTimer::now();

    if (!success) {
        av_packet_free(&pkt);
        av_frame_free(&d3d11Frame);
        return false;
    }

    stats.textureOpenMs = 0.0;
    stats.colorConvertMs = PerfTimer::elapsed_ms(beforeConvert, afterConvert);
    stats.encodeMs = PerfTimer::elapsed_ms(encodeStart, afterEncode);
    stats.totalMs = PerfTimer::elapsed_ms(frameStart, afterEncode);
    stats.packetsProduced = packetCount;

    lastEncodeTimeUs = static_cast<int64_t>(PerfTimer::elapsed_ms(beforeConvert, afterEncode) * 1000.0);
    lastFenceWaitUs = 0;

    g_lastFramePts = timestamp;
    lastAssignedVideoPts = d3d11Frame->pts;

    g_framesEncoded++;
    outputFrameCount++;
    if (populatedFromRepeatSource) {
        CacheRepeatFrameTexture(reinterpret_cast<ID3D11Texture2D*>(d3d11Frame->data[0]));
    }
    g_totalFenceWait += stats.fenceWaitMs;
    g_totalColorConvert += stats.colorConvertMs;
    g_totalEncode += stats.encodeMs;
    if (stats.totalMs > g_maxFrameTime) {
        g_maxFrameTime = stats.totalMs;
    }
    if (stats.totalMs > expectedFrameMs * 2.0) {
        g_slowFrameCount++;
    }

    av_packet_free(&pkt);
    av_frame_free(&d3d11Frame);
    return true;
}

void VideoEncoder::CleanupResources() {
    // Check if we should preserve encoder-owned textures (DXVK zero-copy path).
    // The Vulkan layer imported our KMT handles — destroying them invalidates the pipeline.
    bool preserveEncoderTextures =
        pSharedMem && pSharedMem->useEncoderTextures.load(std::memory_order_acquire) && sharedCaptureTexturesCreated;

    // Free any queued packets (defensive)
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        while (!packetQueue.empty()) {
            AVPacket* pkt = packetQueue.front();
            packetQueue.pop();
            av_packet_free(&pkt);
        }
    }

    currentQueueBytes = 0;

    if (stream)
        stream = nullptr;
    if (fmtCtx) {
        avformat_free_context(fmtCtx);
        fmtCtx = nullptr;
    }
    if (codecCtx) {
        avcodec_free_context(&codecCtx);
        codecCtx = nullptr;
    }

    if (preserveEncoderTextures) {
        // Keep D3D11 device/context alive — textures are bound to it.
        // Only free FFmpeg HW contexts; they'll be recreated in EnsureDevice().
        if (d3d11DeviceCtx)
            av_buffer_unref(&d3d11DeviceCtx);
        if (d3d11FramesCtx)
            av_buffer_unref(&d3d11FramesCtx);
        // Reset initDone so EnsureDevice() rebuilds FFmpeg contexts but reuses the device
        initDone = false;
    }

    if (hwDeviceCtx)
        av_buffer_unref(&hwDeviceCtx);
    if (hwFramesCtx)
        av_buffer_unref(&hwFramesCtx);

    for (int i = 0; i < SHARED_TEXTURE_SLOT_COUNT; i++) {
        if (cachedSharedTextures[i]) {
            cachedSharedTextures[i]->Release();
            cachedSharedTextures[i] = nullptr;
        }
        cachedTextureHandles[i] = nullptr;
    }

    if (cachedD3D11Fence) {
        cachedD3D11Fence->Release();
        cachedD3D11Fence = nullptr;
    }
    if (repeatFrameTexture) {
        repeatFrameTexture->Release();
        repeatFrameTexture = nullptr;
    }
    InvalidateRepeatSourceFrameTexture();
    cachedFenceHandle = nullptr;
    cachedSourcePid = 0;

    if (!preserveEncoderTextures) {
        for (int i = 0; i < SHARED_TEXTURE_SLOT_COUNT; i++) {
            if (sharedCaptureTextures[i]) {
                sharedCaptureTextures[i]->Release();
                sharedCaptureTextures[i] = nullptr;
            }
            if (sharedCaptureHandles[i]) {
                CloseHandle(sharedCaptureHandles[i]);
                sharedCaptureHandles[i] = nullptr;
            }
        }
        if (sharedCaptureFence) {
            sharedCaptureFence->Release();
            sharedCaptureFence = nullptr;
        }
        if (sharedCaptureFenceHandle) {
            CloseHandle(sharedCaptureFenceHandle);
            sharedCaptureFenceHandle = nullptr;
        }
        sharedCaptureTexturesCreated = false;
        sharedCaptureTextureFormat = 0;
    }

    if (bgraStagingTexture) {
        bgraStagingTexture->Release();
        bgraStagingTexture = nullptr;
    }

    CleanupVideoProcessor();
    if (cursorRenderer) {
        cursorRenderer->Cleanup();
    }

    if (!preserveEncoderTextures) {
        TrimD3D11Residency(d3d11Device, d3d11Context, "encoder");
        if (d3d11Context) {
            d3d11Context->Release();
            d3d11Context = nullptr;
        }
        if (d3d11Device) {
            d3d11Device->Release();
            d3d11Device = nullptr;
        }

        if (d3d11DeviceCtx)
            av_buffer_unref(&d3d11DeviceCtx);
        if (d3d11FramesCtx)
            av_buffer_unref(&d3d11FramesCtx);
    }

    initDone = false;
    fileOpened = false;
    startPts = -1;
    inputFrameCount = 0;
    outputFrameCount = 0;
    skippedFrameCount = 0;
    duplicatedFrameCount = 0;
    encodeFrameCounter = 0;
    lastLogFrameCount = 0;
    nextOutputTime_ms = -1;
    lastEncodeTimeUs = 0;
    lastFenceWaitUs = 0;
    lastFrameDeferred.store(false, std::memory_order_relaxed);
    encodedDurationUs.store(0, std::memory_order_relaxed);
    currentQueuePackets.store(0, std::memory_order_relaxed);
    peakQueueBytes.store(0, std::memory_order_relaxed);
    peakQueuePackets.store(0, std::memory_order_relaxed);
    muxBackpressureCount.store(0, std::memory_order_relaxed);
    muxBackpressureWaitUs.store(0, std::memory_order_relaxed);
    muxBackpressureMaxWaitUs.store(0, std::memory_order_relaxed);
    packetDurationClampCount.store(0, std::memory_order_relaxed);
    negativePtsCount.store(0, std::memory_order_relaxed);
    nonMonotonicPtsCount.store(0, std::memory_order_relaxed);
    lastQueuedVideoPts = AV_NOPTS_VALUE;
    lastAssignedVideoPts = -1;
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
    asyncWriteErrorCount = 0;
    if (outputReservation.CleanupOwnedFile()) {
        DLL_Log("[VideoEncoder] Removed unpublished staging output during cleanup: %s", outputFilename.c_str());
    }
}

void VideoEncoder::ReleasePreservedEncoderTextures() {
    if (!sharedCaptureTexturesCreated)
        return;

    DLL_Log("[VideoEncoder] Releasing preserved encoder textures (game exited)");

    // Clear shared memory flags so a new game won't try to import stale handles
    if (pSharedMem) {
        pSharedMem->encoderTextures.kmtReady.store(false, std::memory_order_release);
        pSharedMem->encoderTextures.ready.store(false, std::memory_order_release);
    }

    // Release encoder-owned KMT textures (mirrors !preserveEncoderTextures path in CleanupResources)
    for (int i = 0; i < SHARED_TEXTURE_SLOT_COUNT; i++) {
        if (sharedCaptureTextures[i]) {
            sharedCaptureTextures[i]->Release();
            sharedCaptureTextures[i] = nullptr;
        }
        if (sharedCaptureHandles[i]) {
            CloseHandle(sharedCaptureHandles[i]);
            sharedCaptureHandles[i] = nullptr;
        }
        sharedCaptureKmtHandles[i] = nullptr;
    }
    if (sharedCaptureFence) {
        sharedCaptureFence->Release();
        sharedCaptureFence = nullptr;
    }
    if (sharedCaptureFenceHandle) {
        CloseHandle(sharedCaptureFenceHandle);
        sharedCaptureFenceHandle = nullptr;
    }
    sharedCaptureTexturesCreated = false;
    sharedCaptureTextureFormat = 0;

    // Release D3D11 device and all resources that depend on it
    CleanupVideoProcessor();
    if (bgraStagingTexture) {
        bgraStagingTexture->Release();
        bgraStagingTexture = nullptr;
    }
    if (cursorRenderer) {
        cursorRenderer->Cleanup();
    }
    TrimD3D11Residency(d3d11Device, d3d11Context, "preserved-encoder");

    if (d3d11Context) {
        d3d11Context->Release();
        d3d11Context = nullptr;
    }
    if (d3d11Device) {
        d3d11Device->Release();
        d3d11Device = nullptr;
    }
    if (d3d11DeviceCtx)
        av_buffer_unref(&d3d11DeviceCtx);
    if (d3d11FramesCtx)
        av_buffer_unref(&d3d11FramesCtx);

    initDone = false;
    DLL_Log("[VideoEncoder] Preserved encoder textures released");
}

void VideoEncoder::Stop() {
    bool wasRecording = recordingRequested;
    recordingRequested = false;
    bool writerStillOwnsEncoderResources = false;

    if (wasRecording) {
        const uint32_t phase = pSharedMem ? pSharedMem->runtimeState.capturePhase.load(std::memory_order_relaxed)
                                          : static_cast<uint32_t>(CapturePipelinePhase::kIdle);
        const uint32_t totalFrames =
            pSharedMem ? pSharedMem->runtimeState.framesEncoded.load(std::memory_order_relaxed) : 0;
        const uint32_t liveFrames =
            pSharedMem ? pSharedMem->runtimeState.liveFramesEncoded.load(std::memory_order_relaxed) : 0;
        const uint32_t drainFrames =
            pSharedMem ? pSharedMem->runtimeState.drainFramesEncoded.load(std::memory_order_relaxed) : 0;
        DLL_Log(
            "[VideoEncoder] Recording stats: input=%lld output=%lld runtime=%u skipped=%lld duplicated=%lld phase=%s "
            "live=%u drain=%u backpressure=%u peakMux=%uKB peakPkts=%u",
            inputFrameCount, outputFrameCount, totalFrames, skippedFrameCount, duplicatedFrameCount,
            CapturePipelinePhaseToString(phase), liveFrames, drainFrames,
            muxBackpressureCount.load(std::memory_order_relaxed),
            peakQueueBytes.load(std::memory_order_relaxed) / 1024u, peakQueuePackets.load(std::memory_order_relaxed));

        // Final packet type distribution summary
        if (packetStats.totalPackets > 0) {
            int total = packetStats.totalPackets;
            int64_t avgKey = packetStats.keyframeCount > 0 ? packetStats.keyframeBytes / packetStats.keyframeCount : 0;
            int64_t avgRef = packetStats.refCount > 0 ? packetStats.refBytes / packetStats.refCount : 0;
            int64_t avgB = packetStats.bframeCount > 0 ? packetStats.bframeBytes / packetStats.bframeCount : 0;
            DLL_Log(
                "[VideoEncoder] FINAL PACKET STATS (%d pkts): "
                "Key=%d(avg %lldKB) Ref=%d(avg %lldKB) "
                "SEF=%d(%d%%) B-small=%d(avg %lldB)",
                total, packetStats.keyframeCount, avgKey / 1024, packetStats.refCount, avgRef / 1024,
                packetStats.sefCount, packetStats.sefCount * 100 / total, packetStats.bframeCount, avgB);
        }
    }

    if (wasRecording && writerRunning) {
        DLL_Log("[VideoEncoder] Stop: Signaling finalize (queueBytes=%zu)...", currentQueueBytes.load());
        isStopping = true;
        queueCV.notify_all();
        // Fall through to join — ensures file is fully closed before returning
    }

    // Always wait for writer thread to finish (writes trailer + closes file).
    // Use phase-aware bounded waits: trailer/probe finalization can be slower
    // than packet drain on busy disks, but the async writer must remain the
    // only owner of the muxer until it either completes or definitively times out.
    if (writerThread.joinable()) {
        HANDLE hThread = writerThread.native_handle();
        const uint64_t waitStartMs = GetTickCount64();
        constexpr uint64_t kSlowFinalizeWarnMs = 5000;
        constexpr uint64_t kWriterFinalizeTimeoutMs = 30000;
        DLL_Log("[VideoEncoder] Stop: Waiting for writer thread to finish (phase=%s timeout=%llums)...",
                WriterFinalizePhaseName(writerFinalizePhase.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(kWriterFinalizeTimeoutMs));

        DWORD waitResult = WAIT_TIMEOUT;
        while (true) {
            waitResult = WaitForSingleObject(hThread, 250);
            if (waitResult == WAIT_OBJECT_0) {
                break;
            }
            const uint64_t elapsedMs = GetTickCount64() - waitStartMs;
            const uint32_t phase = writerFinalizePhase.load(std::memory_order_relaxed);
            if (elapsedMs >= kSlowFinalizeWarnMs &&
                !writerFinalizeSlowWarningLogged.exchange(true, std::memory_order_acq_rel)) {
                DLL_Log(
                    "[VideoEncoder] Stop: WARNING writer_finalize_slow phase=%s elapsed=%llums queueBytes=%zu "
                    "queuePackets=%u; async writer still owns FFmpeg context",
                    WriterFinalizePhaseName(phase), static_cast<unsigned long long>(elapsedMs),
                    currentQueueBytes.load(std::memory_order_relaxed),
                    currentQueuePackets.load(std::memory_order_relaxed));
            }
            if (elapsedMs >= kWriterFinalizeTimeoutMs || waitResult == WAIT_FAILED) {
                break;
            }
        }

        if (waitResult == WAIT_OBJECT_0) {
            writerThread.join();
            if (writerFinalizeTimedOut.exchange(false, std::memory_order_acq_rel)) {
                DLL_Log("[VideoEncoder] Stop: Timed-out writer completed on a later stop; muxer ownership recovered.");
            }
            const uint64_t elapsedMs = GetTickCount64() - waitStartMs;
            DLL_Log("[VideoEncoder] Stop: Writer thread joined phase=%s elapsed=%llums.",
                    WriterFinalizePhaseName(writerFinalizePhase.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(elapsedMs));
        } else {
            writerFinalizeTimedOut.store(true, std::memory_order_release);
            const uint32_t timedOutPhase = writerFinalizePhase.load(std::memory_order_relaxed);
            // A live writer owns more than fmtCtx: the post-mux probe still
            // reads outputFilename and will perform CleanupResources on exit.
            // Never race it with synchronous cleanup merely because the muxer
            // file was already closed.
            writerStillOwnsEncoderResources = true;
            DLL_Log(
                "[VideoEncoder] Stop: ERROR writer_finalize_timeout result=%lu phase=%s elapsed=%llums "
                "queueBytes=%zu queuePackets=%u writerRetainsEncoderResources=%d; "
                "skipping synchronous finalize",
                waitResult, WriterFinalizePhaseName(timedOutPhase),
                static_cast<unsigned long long>(GetTickCount64() - waitStartMs),
                currentQueueBytes.load(std::memory_order_relaxed), currentQueuePackets.load(std::memory_order_relaxed),
                writerStillOwnsEncoderResources ? 1 : 0);
        }
    }

    if (writerStillOwnsEncoderResources) {
        return;
    }

    // Fallback: if thread wasn't running and file is still open, close it now
    if (fileOpened && !writerRunning.load(std::memory_order_acquire)) {
        DLL_Log("[VideoEncoder] Sync Stop: Finalizing file...");
        if (fmtCtx) {
            int64_t finalDurationUs = encodedDurationUs.load(std::memory_order_relaxed);
            if (finalDurationUs > 0) {
                LogPacketTimelineSummary(finalDurationUs);
            }
            const int trailerResult = av_write_trailer(fmtCtx);
            if (trailerResult < 0) {
                DLL_Log("[VideoEncoder] Sync Stop: ERROR av_write_trailer failed: %d", trailerResult);
            }
            if (finalDurationUs > 0) {
                LogFinalDurationSummary(fmtCtx, finalDurationUs, muxBackpressureCount.load(std::memory_order_relaxed),
                                        peakQueueBytes.load(std::memory_order_relaxed),
                                        peakQueuePackets.load(std::memory_order_relaxed),
                                        lastEncoderOverloadTickMs.load(std::memory_order_relaxed) > 0,
                                        lastMuxOverloadTickMs.load(std::memory_order_relaxed) > 0);
            }
            int closeResult = 0;
            if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
                closeResult = avio_closep(&fmtCtx->pb);
                if (closeResult < 0) {
                    DLL_Log("[VideoEncoder] Sync Stop: ERROR avio_closep failed: %d", closeResult);
                }
            }
            fileOpened = false;
            const bool published = FinalizeOutputPublication(trailerResult, closeResult, finalDurationUs);
            DLL_Log("[VideoEncoder] mux_closed file='%s' finalDurationUs=%lld", outputFilename.c_str(),
                    (long long)finalDurationUs);
            if (published && finalDurationUs > 0) {
                RunPostMuxDurationProbeBounded(outputFilename, finalDurationUs);
            }
        }
    }

    CleanupResources();
}

void VideoEncoder::Cancel() {
    discardOutputRequested.store(true, std::memory_order_release);
    DLL_Log("[VideoEncoder] Cancellation requested; any unpublished output will be discarded");
    Stop();
}

// ============================================================================
// D3D11 Video Processor for GPU-accelerated BGRA → NV12 conversion
// ============================================================================

bool VideoEncoder::InitVideoProcessor() {
    if (videoProcessorInit)
        return true;

    if (!d3d11Device) {
        DLL_Log("[VideoProcessor] D3D11 device not available");
        return false;
    }

    HRESULT hr;
    const bool outputIsHDR = ShouldEncodeHdrOutput();

    // Get video device interface
    hr = d3d11Device->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&videoDevice);
    if (FAILED(hr)) {
        DLL_Log("[VideoProcessor] Failed to get ID3D11VideoDevice. HR=%x", hr);
        return false;
    }

    // Get video context
    hr = d3d11Context->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&videoContext);
    if (FAILED(hr)) {
        DLL_Log("[VideoProcessor] Failed to get ID3D11VideoContext. HR=%x", hr);
        return false;
    }
    hr = videoContext->QueryInterface(__uuidof(ID3D11VideoContext1), (void**)&videoContext1);
    if (FAILED(hr)) {
        videoContext1 = nullptr;
        DLL_Log(
            "[VideoProcessor] ID3D11VideoContext1 unavailable (HR=%x); SDR uses the legacy VP contract and HDR "
            "output uses the direct P010 shader without VideoProcessor color conversion",
            hr);
    }

    // Store input dimensions (captured frame size)
    inputWidth = width;
    inputHeight = height;

    // Determine output dimensions based on scaling config
    if (savedConfig.scaling.enabled && savedConfig.scaling.outputWidth > 0 && savedConfig.scaling.outputHeight > 0) {
        outputWidth = savedConfig.scaling.outputWidth;
        outputHeight = savedConfig.scaling.outputHeight;
    } else {
        // No scaling or native resolution
        outputWidth = width;
        outputHeight = height;
    }

    // NV12 output textures require even-aligned dimensions
    outputWidth = outputWidth & ~1u;
    outputHeight = outputHeight & ~1u;
    if (outputWidth == 0 || outputHeight == 0) {
        DLL_Log("[VideoProcessor] Dimensions too small after NV12 alignment");
        return false;
    }

    // Check if scaling is actually needed (input != output)
    scalingEnabled = (inputWidth != outputWidth || inputHeight != outputHeight);

    if (scalingEnabled && !outputIsHDR) {
        DLL_Log("[VideoProcessor] GPU SCALING ENABLED: %dx%d -> %dx%d", inputWidth, inputHeight, outputWidth,
                outputHeight);
    } else if (!scalingEnabled) {
        DLL_Log("[VideoProcessor] Scaling disabled (input matches output: %dx%d)", inputWidth, inputHeight);
    }

    // Create video processor enumerator with potentially different input/output
    // dims
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputWidth = inputWidth;
    contentDesc.InputHeight = inputHeight;
    contentDesc.OutputWidth = outputWidth;
    contentDesc.OutputHeight = outputHeight;
    contentDesc.Usage =
        (savedConfig.scaling.quality == "best") ? D3D11_VIDEO_USAGE_OPTIMAL_QUALITY : D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = E_FAIL;
    try {
        hr = videoDevice->CreateVideoProcessorEnumerator(&contentDesc, &videoProcessorEnum);
    } catch (...) {
        hr = E_FAIL;
    }
    if (FAILED(hr)) {
        DLL_Log("[VideoProcessor] Failed to create enumerator. HR=%x", hr);
        return false;
    }

    // Create video processor
    try {
        hr = videoDevice->CreateVideoProcessor(videoProcessorEnum, 0, &videoProcessor);
    } catch (...) {
        hr = E_FAIL;
    }
    if (FAILED(hr)) {
        DLL_Log("[VideoProcessor] Failed to create processor. HR=%x", hr);
        return false;
    }

    // Capture textures are progressive desktop/game frames. Driver automatic
    // processing may apply temporal video heuristics that are inappropriate
    // for pixel-sharp UI. A separate cursor is already point-composited into
    // this RGB stream before the VP sees it.
    videoContext->VideoProcessorSetStreamFrameFormat(videoProcessor, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext->VideoProcessorSetStreamAutoProcessingMode(videoProcessor, 0, FALSE);
    D3D11_VIDEO_FRAME_FORMAT mainFrameFormat = D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST;
    BOOL mainAutoProcessing = TRUE;
    videoContext->VideoProcessorGetStreamFrameFormat(videoProcessor, 0, &mainFrameFormat);
    videoContext->VideoProcessorGetStreamAutoProcessingMode(videoProcessor, 0, &mainAutoProcessing);
    DLL_Log("[VideoProcessor] Deterministic single-stream processing: progressive=%d auto=%d",
            mainFrameFormat == D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE ? 1 : 0, mainAutoProcessing ? 1 : 0);

    // Configure scaling filter if scaling is enabled
    if (scalingEnabled && !outputIsHDR) {
        // Map sharpness (0-100) directly to D3D11 VP edge enhancement
        bool enableEdgeEnhancement = (savedConfig.scaling.sharpness > 0);
        int edgeEnhancementLevel = savedConfig.scaling.sharpness;

        if (enableEdgeEnhancement) {
            // Check if edge enhancement is supported
            D3D11_VIDEO_PROCESSOR_FILTER_RANGE filterRange = {};
            hr = videoProcessorEnum->GetVideoProcessorFilterRange(D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT,
                                                                  &filterRange);

            if (SUCCEEDED(hr)) {
                // Map our 0-100 level to the actual VP filter range
                int filterValue = filterRange.Default;
                if (filterRange.Maximum > filterRange.Minimum) {
                    filterValue = filterRange.Minimum +
                                  (edgeEnhancementLevel * (filterRange.Maximum - filterRange.Minimum) / 100);
                }

                videoContext->VideoProcessorSetStreamFilter(
                    videoProcessor, 0, D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT, TRUE, filterValue);

                DLL_Log(
                    "[VideoProcessor] Scaling: quality=%s, sharpness=%d "
                    "(filterValue=%d, range=%d-%d)",
                    savedConfig.scaling.quality.c_str(), edgeEnhancementLevel, filterValue, filterRange.Minimum,
                    filterRange.Maximum);
            } else {
                DLL_Log(
                    "[VideoProcessor] Edge enhancement (sharpness) not supported "
                    "by hardware");
            }
        } else {
            DLL_Log("[VideoProcessor] Scaling: quality=%s, sharpness=0 (disabled)",
                    savedConfig.scaling.quality.c_str());
        }

        // CRITICAL: Set source and destination rectangles for scaling
        // Without these, VideoProcessorBlt fails with E_INVALIDARG
        RECT sourceRect = {0, 0, (LONG)inputWidth, (LONG)inputHeight};
        RECT destRect = {0, 0, (LONG)outputWidth, (LONG)outputHeight};

        // Stream 0: Source rect = full input frame
        videoContext->VideoProcessorSetStreamSourceRect(videoProcessor, 0, TRUE, &sourceRect);
        // Stream 0: Dest rect = full output frame (scaled)
        videoContext->VideoProcessorSetStreamDestRect(videoProcessor, 0, TRUE, &destRect);
        // Output target = full output surface
        videoContext->VideoProcessorSetOutputTargetRect(videoProcessor, TRUE, &destRect);

        DLL_Log("[VideoProcessor] Scaling rects: source=%dx%d dest=%dx%d", inputWidth, inputHeight, outputWidth,
                outputHeight);
    } else if (scalingEnabled) {
        DLL_Log(
            "[HDR P010] Shader scaling configured: source=%dx%d dest=%dx%d filter=bilinear lumaSharpness=%d",
            inputWidth, inputHeight, outputWidth, outputHeight, savedConfig.scaling.sharpness);
    }

    const OutputRangeMode outputRange = GetEffectiveOutputRange(savedConfig.colorRange, outputIsHDR);

    // Configure color space: Full-range RGB input from capture -> requested YCbCr output range.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColorSpace = {};
    inputColorSpace.Usage = 0;          // 0 = Playback, 1 = Video processing
    inputColorSpace.RGB_Range = 0;      // 0 = Full range (0-255), 1 = Studio (16-235)
    inputColorSpace.YCbCr_Matrix = 1;   // 0 = BT.601, 1 = BT.709
    inputColorSpace.YCbCr_xvYCC = 0;    // 0 = Conventional, 1 = Extended
    inputColorSpace.Nominal_Range = 2;  // 2 = Full (0-255) for input

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColorSpace = {};
    outputColorSpace.Usage = 0;
    outputColorSpace.RGB_Range = outputRange == OutputRangeMode::kFull ? 0 : 1;
    outputColorSpace.YCbCr_Matrix = 1;  // BT.709
    outputColorSpace.YCbCr_xvYCC = 0;
    outputColorSpace.Nominal_Range = outputRange == OutputRangeMode::kFull ? 2 : 1;

    videoContext->VideoProcessorSetStreamColorSpace(videoProcessor, 0, &inputColorSpace);
    videoContext->VideoProcessorSetOutputColorSpace(videoProcessor, &outputColorSpace);
    const char* colorConversionSuffix =
        outputIsHDR ? "; HDR output bypasses VideoProcessor color conversion via direct P010 plane shaders"
                    : (videoContext1 ? "; ColorSpace1 overrides this per frame" : "");
    DLL_Log("[VideoProcessor] Legacy color-space baseline: Full RGB (0-255) -> %s YCbCr (%s, BT.709)%s",
            outputRange == OutputRangeMode::kFull ? "Full" : "Limited",
            outputRange == OutputRangeMode::kFull ? "0-255" : "16-235", colorConversionSuffix);

    // AVHWFrame textures are the VP output surfaces. They are allocated on
    // demand by libavutil and retained by NVENC for exactly as long as each
    // submitted frame remains in flight.
    const bool use10BitOutput = ShouldUse10BitOutput();
    const DXGI_FORMAT outputFormat = use10BitOutput ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;

    UINT formatSupport = 0;
    hr = d3d11Device->CheckFormatSupport(outputFormat, &formatSupport);
    if (SUCCEEDED(hr)) {
        DLL_Log("[VideoProcessor] Output fmt=%d formatSupport=0x%x", outputFormat, formatSupport);
    } else {
        DLL_Log("[VideoProcessor] CheckFormatSupport(fmt=%d) failed. HR=%x", outputFormat, hr);
    }
    DLL_Log("[VideoProcessor] Using AVHWFrame-owned %s output textures at %dx%d", use10BitOutput ? "P010" : "NV12",
            outputWidth, outputHeight);

    if (!outputIsHDR) {
        // Desktop Duplication textures may not support direct VP input views.
        // SDR output retains the compatibility staging copy; HDR output writes P010 planes
        // directly and deliberately does not allocate this legacy surface.
        D3D11_TEXTURE2D_DESC bgraDesc = {};
        bgraDesc.Width = inputWidth;
        bgraDesc.Height = inputHeight;
        bgraDesc.MipLevels = 1;
        bgraDesc.ArraySize = 1;
        bgraDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        bgraDesc.SampleDesc.Count = 1;
        bgraDesc.Usage = D3D11_USAGE_DEFAULT;
        bgraDesc.BindFlags = 0;

        hr = d3d11Device->CreateTexture2D(&bgraDesc, nullptr, &bgraStagingTexture);

        if (FAILED(hr)) {
            DLL_Log("[VideoProcessor] Failed to create BGRA staging texture. HR=%x", hr);
            return false;
        }
        DLL_Log("[VideoProcessor] Created BGRA staging texture at %dx%d for DD compatibility", inputWidth,
                inputHeight);
    } else {
        DLL_Log("[HDR P010] Legacy BGRA/VideoProcessor staging allocation skipped");
    }

    videoProcessorInit = true;

    if (outputIsHDR) {
        DLL_Log("[HDR P010] Initialized direct shader target for %dx%d -> %dx%d RGB10/PQ -> limited P010",
                inputWidth, inputHeight, outputWidth, outputHeight);
    } else if (scalingEnabled) {
        DLL_Log(
            "[VideoProcessor] Initialized with SCALING: %dx%d -> %dx%d "
            "RGB→%s",
            inputWidth, inputHeight, outputWidth, outputHeight, use10BitOutput ? "P010" : "NV12");
    } else {
        DLL_Log("[VideoProcessor] Initialized for %dx%d RGB→%s (no scaling)", outputWidth, outputHeight,
                use10BitOutput ? "P010" : "NV12");
    }
    return true;
}

VideoEncoder::CursorSourceRestore::~CursorSourceRestore() {
    if (!active || !context || !target || !backup || width == 0 || height == 0) {
        return;
    }
    const D3D11_BOX backupBox = {0, 0, 0, width, height, 1};
    context->CopySubresourceRegion(target, 0, destinationX, destinationY, 0, backup, 0, &backupBox);
}

void VideoEncoder::CleanupCursorCompositionResources() {
    if (cursorRestoreTexture) {
        cursorRestoreTexture->Release();
        cursorRestoreTexture = nullptr;
    }
    if (cursorCompositeTexture) {
        cursorCompositeTexture->Release();
        cursorCompositeTexture = nullptr;
    }
}

bool VideoEncoder::PrepareVideoProcessorCursorInput(ID3D11Texture2D* source, bool overlayCursor,
                                                    CursorSourceRestore* restore, ID3D11Texture2D** preparedSource) {
    if (!source || !restore || !preparedSource || !d3d11Device || !d3d11Context) {
        return false;
    }
    *preparedSource = source;
    if (!overlayCursor || !cursorRenderer || !cursorCaptureState.IsVisible()) {
        return true;
    }

    if (!cursorRenderer->Init(d3d11Device, d3d11Context)) {
        if (cursorPrecompositionFailureLogs++ < 5) {
            DLL_Log(
                "[Cursor] Failed to initialize RGB cursor renderer; video conversion continues without this "
                "separate cursor draw");
        }
        return true;
    }

    D3D11_TEXTURE2D_DESC sourceDesc = {};
    source->GetDesc(&sourceDesc);
    RECT cursorRect = {};
    if (!cursorRenderer->GetCursorFrameRect(static_cast<int>(sourceDesc.Width), static_cast<int>(sourceDesc.Height),
                                            cursorCaptureState, &cursorRect)) {
        if (cursorPrecompositionFailureLogs++ < 5) {
            DLL_Log(
                "[Cursor] Failed to resolve cursor bitmap/rectangle; video conversion continues without this "
                "separate cursor draw");
        }
        return true;
    }

    const LONG clippedLeft = std::clamp<LONG>(cursorRect.left, 0, static_cast<LONG>(sourceDesc.Width));
    const LONG clippedTop = std::clamp<LONG>(cursorRect.top, 0, static_cast<LONG>(sourceDesc.Height));
    const LONG clippedRight = std::clamp<LONG>(cursorRect.right, 0, static_cast<LONG>(sourceDesc.Width));
    const LONG clippedBottom = std::clamp<LONG>(cursorRect.bottom, 0, static_cast<LONG>(sourceDesc.Height));
    if (clippedLeft >= clippedRight || clippedTop >= clippedBottom) {
        return true;
    }

    const UINT regionWidth = static_cast<UINT>(clippedRight - clippedLeft);
    const UINT regionHeight = static_cast<UINT>(clippedBottom - clippedTop);
    ID3D11Texture2D* compositionTarget = source;
    bool useSmallRestore = (sourceDesc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0 && sourceDesc.SampleDesc.Count == 1;

    if (useSmallRestore) {
        bool recreateRestore = cursorRestoreTexture == nullptr;
        if (cursorRestoreTexture) {
            D3D11_TEXTURE2D_DESC existing = {};
            cursorRestoreTexture->GetDesc(&existing);
            recreateRestore =
                existing.Format != sourceDesc.Format || existing.Width < regionWidth || existing.Height < regionHeight;
        }
        if (recreateRestore) {
            if (cursorRestoreTexture) {
                cursorRestoreTexture->Release();
                cursorRestoreTexture = nullptr;
            }
            D3D11_TEXTURE2D_DESC restoreDesc = {};
            restoreDesc.Width = regionWidth;
            restoreDesc.Height = regionHeight;
            restoreDesc.MipLevels = 1;
            restoreDesc.ArraySize = 1;
            restoreDesc.Format = sourceDesc.Format;
            restoreDesc.SampleDesc.Count = 1;
            restoreDesc.Usage = D3D11_USAGE_DEFAULT;
            const HRESULT restoreHr = d3d11Device->CreateTexture2D(&restoreDesc, nullptr, &cursorRestoreTexture);
            if (FAILED(restoreHr)) {
                useSmallRestore = false;
                if (cursorPrecompositionFailureLogs++ < 5) {
                    DLL_Log("[Cursor] Small RGB restore texture creation failed: fmt=%d %ux%u HR=%x", sourceDesc.Format,
                            regionWidth, regionHeight, restoreHr);
                }
            }
        }
    }

    if (useSmallRestore) {
        const D3D11_BOX sourceBox = {
            static_cast<UINT>(clippedLeft),  static_cast<UINT>(clippedTop),    0,
            static_cast<UINT>(clippedRight), static_cast<UINT>(clippedBottom), 1,
        };
        d3d11Context->CopySubresourceRegion(cursorRestoreTexture, 0, 0, 0, 0, source, 0, &sourceBox);
        restore->context = d3d11Context;
        restore->target = source;
        restore->backup = cursorRestoreTexture;
        restore->destinationX = static_cast<UINT>(clippedLeft);
        restore->destinationY = static_cast<UINT>(clippedTop);
        restore->width = regionWidth;
        restore->height = regionHeight;
        restore->active = true;
    } else {
        bool recreateComposite = cursorCompositeTexture == nullptr;
        if (cursorCompositeTexture) {
            D3D11_TEXTURE2D_DESC existing = {};
            cursorCompositeTexture->GetDesc(&existing);
            recreateComposite = existing.Width != sourceDesc.Width || existing.Height != sourceDesc.Height ||
                                existing.MipLevels != sourceDesc.MipLevels ||
                                existing.ArraySize != sourceDesc.ArraySize || existing.Format != sourceDesc.Format ||
                                existing.SampleDesc.Count != sourceDesc.SampleDesc.Count ||
                                existing.SampleDesc.Quality != sourceDesc.SampleDesc.Quality;
        }
        if (recreateComposite) {
            if (cursorCompositeTexture) {
                cursorCompositeTexture->Release();
                cursorCompositeTexture = nullptr;
            }
            D3D11_TEXTURE2D_DESC compositeDesc = sourceDesc;
            compositeDesc.Usage = D3D11_USAGE_DEFAULT;
            compositeDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
            compositeDesc.CPUAccessFlags = 0;
            compositeDesc.MiscFlags = 0;
            const HRESULT compositeHr = d3d11Device->CreateTexture2D(&compositeDesc, nullptr, &cursorCompositeTexture);
            if (FAILED(compositeHr)) {
                if (cursorPrecompositionFailureLogs++ < 5) {
                    DLL_Log(
                        "[Cursor] RGB cursor fallback texture creation failed; video conversion continues without "
                        "this separate cursor draw: fmt=%d bind=%x HR=%x",
                        sourceDesc.Format, sourceDesc.BindFlags, compositeHr);
                }
                return true;
            }
        }
        d3d11Context->CopyResource(cursorCompositeTexture, source);
        compositionTarget = cursorCompositeTexture;
        *preparedSource = cursorCompositeTexture;
        if (!cursorFullCopyFallbackLogged) {
            DLL_Log("[Cursor] RGB precomposition fallback uses a full-frame GPU copy: fmt=%d bind=%x samples=%u",
                    sourceDesc.Format, sourceDesc.BindFlags, sourceDesc.SampleDesc.Count);
            cursorFullCopyFallbackLogged = true;
        }
    }

    const CursorColorMode colorMode = ce::video_format::IsFp16RgbInputFormat(sourceDesc.Format)
                                          ? CursorColorMode::ScRgb
                                          : (currentIsHDR ? CursorColorMode::Hdr10Pq : CursorColorMode::Sdr);
    const float cursorPaperWhiteNits = currentIsHDR ? sdrWhiteNits : 80.0f;
    if (!cursorRenderer->CompositeOntoFrame(compositionTarget, static_cast<int>(sourceDesc.Width),
                                            static_cast<int>(sourceDesc.Height), cursorCaptureState, colorMode,
                                            cursorPaperWhiteNits)) {
        if (cursorPrecompositionFailureLogs++ < 5) {
            DLL_Log(
                "[Cursor] Point-sampled RGB cursor draw failed; video conversion continues without this separate "
                "cursor draw: fmt=%d bind=%x",
                sourceDesc.Format, sourceDesc.BindFlags);
        }
        return true;
    }

    // The video processor must never observe its input simultaneously bound as
    // a graphics render target. All work remains ordered on this one immediate
    // context; no flush or CPU/GPU wait is required.
    d3d11Context->OMSetRenderTargets(0, nullptr, nullptr);
    if (!cursorPrecompositionLogged) {
        DLL_Log(
            "[Cursor] Point RGB precomposition before VP active: fmt=%d bind=%x region=%ux%u smallRestore=%d "
            "(separate and Windows-embedded cursors now share the main conversion stream)",
            sourceDesc.Format, sourceDesc.BindFlags, regionWidth, regionHeight, useSmallRestore ? 1 : 0);
        cursorPrecompositionLogged = true;
    }
    return true;
}

bool VideoEncoder::ConvertBGRAtoNV12(ID3D11Texture2D* bgraTexture, AVFrame* outputFrame, bool overlayCursor,
                                     bool allowDirectInputView, int captureOriginX, int captureOriginY,
                                     uint64_t keyedMutexAcquireKey) {
    if (!bgraTexture || !outputFrame || !d3d11FramesCtx) {
        DLL_Log("[VideoProcessor] Invalid RGB conversion input or output frame");
        return false;
    }
    D3D11_TEXTURE2D_DESC captureDesc = {};
    bgraTexture->GetDesc(&captureDesc);
    UpdateSdrWhiteLevelForCaptureArea(captureOriginX, captureOriginY, captureDesc.Width, captureDesc.Height);
    const bool outputIsHDR = ShouldEncodeHdrOutput();
    if (!videoProcessorInit) {
        if (!InitVideoProcessor())
            return false;
    }

    if (!outputFrame->buf[0]) {
        const int frameRet = av_hwframe_get_buffer(d3d11FramesCtx, outputFrame, 0);
        if (frameRet < 0 || !outputFrame->data[0]) {
            DLL_Log("[VideoProcessor] Failed to allocate AVHWFrame-owned output: %d", frameRet);
            return false;
        }
    }
    if (!outputFrame->data[0]) {
        DLL_Log("[VideoProcessor] AVHWFrame output is missing its D3D11 texture");
        return false;
    }

    auto* outputTexture = reinterpret_cast<ID3D11Texture2D*>(outputFrame->data[0]);
    const UINT outputArraySlice = static_cast<UINT>(reinterpret_cast<uintptr_t>(outputFrame->data[1]));
    ID3D11VideoProcessorOutputView* outputView = nullptr;
    if (!outputIsHDR) {
        for (const auto& cached : outputViewCache) {
            if (cached.texture == outputTexture && cached.arraySlice == outputArraySlice) {
                outputView = cached.view;
                break;
            }
        }

        if (!outputView) {
            D3D11_TEXTURE2D_DESC outputTextureDesc = {};
            outputTexture->GetDesc(&outputTextureDesc);
            if (outputArraySlice >= outputTextureDesc.ArraySize) {
                DLL_Log("[VideoProcessor] Invalid AVHWFrame array slice %u for array size %u", outputArraySlice,
                        outputTextureDesc.ArraySize);
                return false;
            }

            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = {};
            if (outputTextureDesc.ArraySize > 1) {
                outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
                outputViewDesc.Texture2DArray.MipSlice = 0;
                outputViewDesc.Texture2DArray.FirstArraySlice = outputArraySlice;
                outputViewDesc.Texture2DArray.ArraySize = 1;
            } else {
                outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
                outputViewDesc.Texture2D.MipSlice = 0;
            }

            HRESULT outputViewHr = E_FAIL;
            try {
                outputViewHr = videoDevice->CreateVideoProcessorOutputView(outputTexture, videoProcessorEnum,
                                                                           &outputViewDesc, &outputView);
            } catch (...) {
                outputViewHr = E_FAIL;
            }
            if (FAILED(outputViewHr) || !outputView) {
                DLL_Log(
                    "[VideoProcessor] Failed to bind AVHWFrame output view: HR=%x fmt=%d bind=%x array=%u slice=%u",
                    outputViewHr, outputTextureDesc.Format, outputTextureDesc.BindFlags, outputTextureDesc.ArraySize,
                    outputArraySlice);
                return false;
            }
            outputViewCache.push_back({outputTexture, outputArraySlice, outputView});
            if (outputViewCache.size() == 1) {
                DLL_Log("[VideoProcessor] AVHWFrame output-view cache active (fmt=%d bind=%x)",
                        outputTextureDesc.Format, outputTextureDesc.BindFlags);
            }
        }
    }

    // Debug: Log texture descriptions on first call per recording
    if (!vpFirstCallLogged) {
        D3D11_TEXTURE2D_DESC srcDesc;
        bgraTexture->GetDesc(&srcDesc);
        DLL_Log("[VP DEBUG] Source tex: %dx%d fmt=%d bind=%x misc=%x", srcDesc.Width, srcDesc.Height, srcDesc.Format,
                srcDesc.BindFlags, srcDesc.MiscFlags);
    }

    // Track whether this recording is using the direct VP P010 path.
    {
        D3D11_TEXTURE2D_DESC srcDesc;
        bgraTexture->GetDesc(&srcDesc);
        const bool shouldUse10BitPipeline =
            ce::video_format::IsHighPrecisionRgbInputFormat(srcDesc.Format) && ShouldUse10BitOutput();
        if (shouldUse10BitPipeline != use10BitPipeline) {
            use10BitPipeline = shouldUse10BitPipeline;
            DLL_Log("[VP] Input fmt=%d, VP output pipeline=%s", srcDesc.Format, use10BitPipeline ? "P010" : "NV12");
        }
    }

    struct KeyedMutexGuard {
        IDXGIKeyedMutex* mutex = nullptr;
        bool acquired = false;

        ~KeyedMutexGuard() {
            if (!mutex) {
                return;
            }
            if (acquired) {
                mutex->ReleaseSync(0);
            }
            mutex->Release();
        }
    } keyedMutexGuard;

    bgraTexture->QueryInterface(IID_PPV_ARGS(&keyedMutexGuard.mutex));
    if (keyedMutexGuard.mutex) {
        HRESULT kmHr = keyedMutexGuard.mutex->AcquireSync(keyedMutexAcquireKey, 1000);
        if (kmHr != S_OK) {
            static int kmFailCount = 0;
            if (kmFailCount++ < 5) {
                DLL_Log("[VideoProcessor] KeyedMutex AcquireSync failed: HR=%x", kmHr);
            }
            keyedMutexGuard.mutex->Release();
            keyedMutexGuard.mutex = nullptr;
            return false;
        }
        keyedMutexGuard.acquired = true;
    }

    CursorSourceRestore cursorSourceRestore;
    ID3D11Texture2D* preparedCursorSource = bgraTexture;
    if (!PrepareVideoProcessorCursorInput(bgraTexture, overlayCursor, &cursorSourceRestore, &preparedCursorSource)) {
        return false;
    }

    // Try to create the VP input view directly from the source texture only for
    // inject/shared-handle frames. WGC/direct-texture frames are valid capture
    // inputs, but probing them with CreateVideoProcessorInputView can raise a
    // handled D3D11 C++ exception before we fall back to the already-working
    // staging path.
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = {};
    inputViewDesc.FourCC = 0;
    inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputViewDesc.Texture2D.MipSlice = 0;

    // If input is RGBA, swap R/B channels to produce BGRA before VP processing.
    // D3D11 Video Processor expects BGRA input; DXVK KMT textures may be RGBA.
    // A fullscreen shader pass with a BGRA render target handles the byte reorder.
    ID3D11Texture2D* vpInputTexture = preparedCursorSource;
    bool allowVpInputView = allowDirectInputView;
    bool needReleaseConverted = false;
    bool vpInputIsLinear = false;
    bool wantsFp16VpStagingPath = false;
    D3D11_TEXTURE2D_DESC vpInputDesc = {};
    auto releaseConvertedInput = [&]() {
        if (needReleaseConverted && vpInputTexture && vpInputTexture != preparedCursorSource) {
            vpInputTexture->Release();
        }
        needReleaseConverted = false;
    };
    auto prepareHighPrecisionRgb10CompatInput = [&](HRESULT priorHr) -> bool {
        const DXGI_FORMAT sourceFormat = vpInputDesc.Format;
        const DXGI_FORMAT inputSrvFormat = ce::video_format::GetRgbShaderResourceViewFormat(sourceFormat);
        if (inputSrvFormat == DXGI_FORMAT_UNKNOWN) {
            DLL_Log("[VP] High-precision RGB10 fallback unsupported source format: fmt=%d", sourceFormat);
            return false;
        }
        const ce::video_format::RgbColorTransform colorTransform =
            ce::video_format::GetRgbColorTransform(sourceFormat, currentIsHDR, outputIsHDR);
        ID3D11Texture2D* converted =
            RenderFullscreenCopy(vpInputTexture, vpInputDesc.Width, vpInputDesc.Height, inputSrvFormat,
                                 DXGI_FORMAT_R10G10B10A2_UNORM, rgb10IntermediateTexture, rgb10IntermediateRTV,
                                 rgb10IntermediateWidth, rgb10IntermediateHeight, "RGB10", colorTransform,
                                 sdrWhiteNits);
        if (!converted) {
            DLL_Log("[VP] Failed to convert high-precision input to RGB10A2 before VP (srcFmt=%d srvFmt=%d)",
                    sourceFormat, inputSrvFormat);
            return false;
        }
        if (needReleaseConverted && vpInputTexture != preparedCursorSource) {
            vpInputTexture->Release();
        }
        vpInputTexture = converted;
        needReleaseConverted = true;
        allowVpInputView = true;
        vpInputIsLinear = ce::video_format::IsFp16RgbInputFormat(sourceFormat) &&
                          colorTransform == ce::video_format::RgbColorTransform::kNone;
        vpInputTexture->GetDesc(&vpInputDesc);
        if (!vpFp16CompatLogged) {
            DLL_Log(
                "[VP] High-precision input normalization: priorHR=%x srcFmt=%d srvFmt=%d final=RGB10A2 transform=%s "
                "output=%s",
                priorHr, sourceFormat, inputSrvFormat, ce::video_format::DescribeRgbColorTransform(colorTransform),
                ShouldUse10BitOutput() ? "P010" : "NV12");
            vpFp16CompatLogged = true;
        }
        if (!outputIsHDR && currentIsHDR && !hdrToSdrLogged) {
            DLL_Log(
                "[HDR->SDR] Whole-frame shader tone map active: transform=%s sourceWhite=%.1f-nit "
                "output=BT709-G22 headroom=80%% passes=1 intermediate=RGB10 gamut=luminance-preserving "
                "driverVPTransfer=0 cpuWait=0 frameAlpha=opaque",
                ce::video_format::DescribeRgbColorTransform(colorTransform), sdrWhiteNits);
            hdrToSdrLogged = true;
        }
        return true;
    };
    {
        D3D11_TEXTURE2D_DESC srcDesc;
        preparedCursorSource->GetDesc(&srcDesc);
        if (srcDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
            ID3D11Texture2D* converted = SwapRBChannels(preparedCursorSource, srcDesc.Width, srcDesc.Height);
            if (converted) {
                vpInputTexture = converted;
                needReleaseConverted = true;
                if (!vpFirstCallLogged)
                    DLL_Log("[VP] RGBA input detected - R/B swap applied before VP");
            } else {
                DLL_Log("[VP] R/B swap failed, using original texture");
            }
        }

        // CRITICAL: Validate texture before passing to D3D11 VideoProcessor.
        // D3D11's CreateVideoProcessorInputView throws SEH for incompatible formats,
        // and MinGW catch(...) CANNOT catch SEH exceptions (__fastfail).
        // We must prevent the call by checking format compatibility first.
        vpInputTexture->GetDesc(&vpInputDesc);
        const ce::video_format::RgbColorTransform requiredHdrTransform =
            ce::video_format::GetRgbColorTransform(vpInputDesc.Format, currentIsHDR, outputIsHDR);
        if (currentIsHDR && !ce::video_format::IsFp16RgbInputFormat(vpInputDesc.Format) &&
            !ce::video_format::IsHdr10RgbInputFormat(vpInputDesc.Format)) {
            DLL_Log("[HDR Color] Unsupported HDR source format %d; refusing an unconverted output",
                    vpInputDesc.Format);
            releaseConvertedInput();
            return false;
        }
        const bool normalizeHdrForOutput = currentIsHDR &&
                                           requiredHdrTransform != ce::video_format::RgbColorTransform::kNone;
        if (normalizeHdrForOutput && !prepareHighPrecisionRgb10CompatInput(S_OK)) {
            releaseConvertedInput();
            return false;
        }
        vpInputTexture->GetDesc(&vpInputDesc);
        wantsFp16VpStagingPath = !allowDirectInputView && ce::video_format::IsFp16RgbInputFormat(vpInputDesc.Format);
        if (wantsFp16VpStagingPath && fp16VpInputStrategy == Fp16VpInputStrategy::kUseRgb10Compat) {
            if (!prepareHighPrecisionRgb10CompatInput(S_OK)) {
                releaseConvertedInput();
                return false;
            }
        }
        if (outputIsHDR) {
            const bool converted = ConvertHdrRgb10ToP010(vpInputTexture, outputTexture, outputArraySlice);
            releaseConvertedInput();
            if (!converted) {
                DLL_Log(
                    "[HDR P010] Direct shader conversion failed; refusing the driver VideoProcessor PQ fallback "
                    "that produced corrupt colors");
            }
            vpFirstCallLogged = true;
            return converted;
        }
        bool vpCompatible =
            (vpInputDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || vpInputDesc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
             vpInputDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
             vpInputDesc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS ||  // WGC can provide TYPELESS with 10-bit display
             vpInputDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
             vpInputDesc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS || vpInputDesc.Format == DXGI_FORMAT_NV12 ||
             vpInputDesc.Format == DXGI_FORMAT_P010);

        if (!vpCompatible) {
            DLL_Log("[VP] Texture format %d not VP-compatible, frame dropped", vpInputDesc.Format);
            releaseConvertedInput();
            return false;  // Cannot convert - format not supported by VP
        }
    }

    vpFirstCallLogged = true;

    // CRITICAL: CreateVideoProcessorInputView can throw SEH for incompatible formats,
    // and MinGW catch(...) CANNOT catch SEH exceptions (__fastfail).
    // The texture format was pre-validated above, but the try/catch is a safety net.
    ID3D11VideoProcessorInputView* localInputView = nullptr;
    HRESULT hr = E_FAIL;
    if (allowVpInputView) {
        try {
            hr = videoDevice->CreateVideoProcessorInputView(vpInputTexture, videoProcessorEnum, &inputViewDesc,
                                                            &localInputView);
        } catch (...) {
            hr = E_FAIL;
            DLL_Log("[VP] CreateVideoProcessorInputView threw exception (fmt=%d)", vpInputDesc.Format);
        }
    }

    // Log CreateVideoProcessorInputView result on first call per recording
    if (allowVpInputView && !vpInputViewLogged) {
        vpInputViewLogged = true;
        DLL_Log("[VP] CreateVideoProcessorInputView(fmt=%d, bind=%x): HR=%x%s", vpInputDesc.Format,
                vpInputDesc.BindFlags, hr, SUCCEEDED(hr) ? " (direct OK)" : "");
    }

    if (!allowVpInputView || FAILED(hr)) {
        if (allowVpInputView) {
            static bool stagingLogged = false;
            if (!stagingLogged) {
                DLL_Log(
                    "[VP] Direct input view failed (HR=%x), using "
                    "staging copy",
                    hr);
                stagingLogged = true;
            }
        } else {
            static bool stagingBypassLogged = false;
            if (!stagingBypassLogged) {
                DLL_Log("[VP] D3D11 direct-texture path uses staging input by design");
                stagingBypassLogged = true;
            }
        }

        if (bgraStagingTexture) {
            D3D11_TEXTURE2D_DESC stageDesc;
            bgraStagingTexture->GetDesc(&stageDesc);

            // Check if staging texture needs to be recreated with correct
            // format
            if (stageDesc.Format != vpInputDesc.Format || stageDesc.Width != vpInputDesc.Width ||
                stageDesc.Height != vpInputDesc.Height) {
                DLL_Log("[VP] Recreating staging texture: %ux%u fmt %d -> %ux%u fmt %d", stageDesc.Width,
                        stageDesc.Height, stageDesc.Format, vpInputDesc.Width, vpInputDesc.Height, vpInputDesc.Format);
                bgraStagingTexture->Release();
                bgraStagingTexture = nullptr;
            }
        }

        // Create staging texture if needed
        if (!bgraStagingTexture) {
            D3D11_TEXTURE2D_DESC stageDesc = {};
            stageDesc.Width = vpInputDesc.Width;
            stageDesc.Height = vpInputDesc.Height;
            stageDesc.MipLevels = 1;
            stageDesc.ArraySize = 1;
            stageDesc.Format = vpInputDesc.Format;  // Match the texture fed into the VP path.
            stageDesc.SampleDesc.Count = 1;
            stageDesc.Usage = D3D11_USAGE_DEFAULT;
            stageDesc.BindFlags = 0;  // Compatible with VP

            ID3D11Device* baseDevice = nullptr;
            d3d11Device->QueryInterface(__uuidof(ID3D11Device), (void**)&baseDevice);
            hr = baseDevice->CreateTexture2D(&stageDesc, nullptr, &bgraStagingTexture);
            baseDevice->Release();

            if (FAILED(hr)) {
                DLL_Log("[VP] Failed to create staging texture: HR=%x", hr);
                releaseConvertedInput();
                return false;
            }
            DLL_Log("[VP] Created staging texture: %ux%u fmt=%d", vpInputDesc.Width, vpInputDesc.Height,
                    vpInputDesc.Format);
        }

        // Copy to staging
        ID3D11DeviceContext* ctx = nullptr;
        d3d11Device->GetImmediateContext(&ctx);
        if (ctx) {
            ctx->CopyResource(bgraStagingTexture, vpInputTexture);

            // Debug: Log copy on first few frames
            static int copyCount = 0;
            if (copyCount++ < 5) {
                DLL_Log("[VP] CopyResource to staging - frame %d", copyCount);
            }
            ctx->Release();
        }

        // Create input view from staging texture
        try {
            hr = videoDevice->CreateVideoProcessorInputView(bgraStagingTexture, videoProcessorEnum, &inputViewDesc,
                                                            &localInputView);
        } catch (...) {
            hr = E_FAIL;
        }
        if (FAILED(hr)) {
            const DXGI_FORMAT failedVpInputFormat = vpInputDesc.Format;
            if (ce::video_format::IsHighPrecisionRgbInputFormat(failedVpInputFormat)) {
                if (ce::video_format::IsFp16RgbInputFormat(failedVpInputFormat)) {
                    fp16VpInputStrategy = Fp16VpInputStrategy::kUseRgb10Compat;
                }
                if (!prepareHighPrecisionRgb10CompatInput(hr)) {
                    DLL_Log("[VP] Failed high-precision RGB10A2 compatibility blit after staging input failure");
                    releaseConvertedInput();
                    return false;
                }
                try {
                    hr = videoDevice->CreateVideoProcessorInputView(vpInputTexture, videoProcessorEnum, &inputViewDesc,
                                                                    &localInputView);
                } catch (...) {
                    hr = E_FAIL;
                }
                if (FAILED(hr)) {
                    DLL_Log("[VP] RGB10A2 VP input view failed after high-precision fallback: srcFmt=%d HR=%x",
                            failedVpInputFormat, hr);
                    releaseConvertedInput();
                    return false;
                }
                DLL_Log("[VP] Using RGB10A2 VP input for high-precision source fmt=%d", failedVpInputFormat);
            } else {
                DLL_Log("[VP] Failed to create input view from staging: HR=%x", hr);
                releaseConvertedInput();
                return false;
            }
        } else if (wantsFp16VpStagingPath && fp16VpInputStrategy == Fp16VpInputStrategy::kUnknown) {
            fp16VpInputStrategy = Fp16VpInputStrategy::kUseStaging;
            DLL_Log("[VP] Using native FP16 staging input for %s VP path", ShouldUse10BitOutput() ? "10-bit" : "8-bit");
        }
        // inputTexture = bgraStagingTexture;
    }

    if (videoContext1) {
        std::string configuredColorSpace = savedConfig.colorSpace;
        if (_stricmp(configuredColorSpace.c_str(), "auto") == 0 || configuredColorSpace.empty()) {
            configuredColorSpace = outputIsHDR ? "bt2020" : "bt709";
        } else if (_stricmp(configuredColorSpace.c_str(), "bt2020") == 0) {
            configuredColorSpace = "bt2020";
        } else {
            configuredColorSpace = "bt709";
        }
        const OutputRangeMode outputRange = GetEffectiveOutputRange(savedConfig.colorRange, outputIsHDR);
        const DXGI_COLOR_SPACE_TYPE inputColorSpace =
            GetVideoProcessorInputColorSpace(vpInputDesc.Format, outputIsHDR, vpInputIsLinear);
        const DXGI_COLOR_SPACE_TYPE outputColorSpace =
            GetVideoProcessorOutputColorSpace(ShouldUse10BitOutput(), outputIsHDR, configuredColorSpace, outputRange);
        videoContext1->VideoProcessorSetStreamColorSpace1(videoProcessor, 0, inputColorSpace);
        videoContext1->VideoProcessorSetOutputColorSpace1(videoProcessor, outputColorSpace);
        if (!vpColorContractLogged) {
            DLL_Log(
                "[VideoProcessor] ColorSpace1 contract: input=%d output=%d inputFmt=%d sourceHdr=%d outputHdr=%d "
                "bitDepth=%s range=%s",
                static_cast<int>(inputColorSpace), static_cast<int>(outputColorSpace), vpInputDesc.Format,
                currentIsHDR ? 1 : 0, outputIsHDR ? 1 : 0, ShouldUse10BitOutput() ? "10" : "8",
                DescribeOutputRange(outputRange));
            vpColorContractLogged = true;
        }
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = localInputView;

    hr = videoContext->VideoProcessorBlt(videoProcessor, outputView, 0, 1, &stream);

    localInputView->Release();

    if (FAILED(hr)) {
        static int bltFailCount = 0;
        if (bltFailCount++ < 5) {
            D3D11_TEXTURE2D_DESC srcDesc = {};
            bgraTexture->GetDesc(&srcDesc);
            const HRESULT deviceReason = d3d11Device->GetDeviceRemovedReason();
            DLL_Log(
                "[VideoProcessor] Blt failed. HR=%x streams=1 outputSlice=%u "
                "srcFmt=%d srcW=%u srcH=%u srcBind=%x srcMisc=%x "
                "inputW=%d inputH=%d outputW=%d outputH=%d deviceReason=%x",
                hr, outputArraySlice, srcDesc.Format, srcDesc.Width, srcDesc.Height, srcDesc.BindFlags,
                srcDesc.MiscFlags, inputWidth, inputHeight, outputWidth, outputHeight, deviceReason);
        }
        if (needReleaseConverted)
            vpInputTexture->Release();
        return false;
    }

    if (needReleaseConverted)
        vpInputTexture->Release();

    return true;
}

bool VideoEncoder::EnsureSwapRBShader() {
    if (swapRBShaderCreated)
        return true;

    // ID3DBlob's implementation and vtable live in d3dcompiler_47.dll. Keep the
    // module loaded until every compiler-owned blob below has been consumed and
    // released (ModuleGuard is declared first, so it is destroyed last).
    ce::ModuleGuard d3dCompiler(ce::security::LoadSystemLibrary(L"d3dcompiler_47.dll"));
    if (!d3dCompiler) {
        DLL_Log("[SwapRB] Failed to load d3dcompiler_47.dll");
        return false;
    }

    typedef HRESULT(WINAPI * pD3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                          LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    pD3DCompile d3dCompile = (pD3DCompile)GetProcAddress(d3dCompiler.get(), "D3DCompile");
    if (!d3dCompile) {
        DLL_Log("[SwapRB] Failed to get D3DCompile");
        return false;
    }

    auto compileShader = [&](const char* entry, const char* target, ce::ComGuard<ID3DBlob>& output) -> HRESULT {
        ce::ComGuard<ID3DBlob> errors;
        const HRESULT compileHr = d3dCompile(
            ce::video_color::kRgbColorConversionShaderSource,
            strlen(ce::video_color::kRgbColorConversionShaderSource), nullptr, nullptr, nullptr, entry, target, 0, 0,
            output.addressof(), errors.addressof());
        if (errors) {
            DLL_Log("[RGBConvert] %s/%s compiler output: %s", entry, target,
                    static_cast<const char*>(errors->GetBufferPointer()));
        }
        return compileHr;
    };

    ce::ComGuard<ID3DBlob> vsBlob;
    ce::ComGuard<ID3DBlob> copyPsBlob;
    ce::ComGuard<ID3DBlob> p010YBlob;
    ce::ComGuard<ID3DBlob> p010UvBlob;
    HRESULT hr = compileShader("VS_Main", "vs_4_0", vsBlob);
    if (SUCCEEDED(hr))
        hr = compileShader("PS_Main", "ps_4_0", copyPsBlob);
    if (SUCCEEDED(hr))
        hr = compileShader("PS_P010Y", "ps_4_0", p010YBlob);
    if (SUCCEEDED(hr))
        hr = compileShader("PS_P010UV", "ps_4_0", p010UvBlob);
    if (FAILED(hr)) {
        DLL_Log("[RGBConvert] Runtime shader compilation failed: HR=%x", hr);
        return false;
    }

    ce::ComGuard<ID3D11VertexShader> copyVs;
    ce::ComGuard<ID3D11PixelShader> copyPs;
    ce::ComGuard<ID3D11PixelShader> p010Y;
    ce::ComGuard<ID3D11PixelShader> p010Uv;
    hr = d3d11Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                         copyVs.addressof());
    if (SUCCEEDED(hr))
        hr = d3d11Device->CreatePixelShader(copyPsBlob->GetBufferPointer(), copyPsBlob->GetBufferSize(), nullptr,
                                            copyPs.addressof());
    if (SUCCEEDED(hr))
        hr = d3d11Device->CreatePixelShader(p010YBlob->GetBufferPointer(), p010YBlob->GetBufferSize(), nullptr,
                                            p010Y.addressof());
    if (SUCCEEDED(hr))
        hr = d3d11Device->CreatePixelShader(p010UvBlob->GetBufferPointer(), p010UvBlob->GetBufferSize(), nullptr,
                                            p010Uv.addressof());
    if (FAILED(hr)) {
        DLL_Log("[RGBConvert] Runtime shader creation failed: HR=%x", hr);
        return false;
    }

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ce::ComGuard<ID3D11SamplerState> copySampler;
    hr = d3d11Device->CreateSamplerState(&sampDesc, copySampler.addressof());
    if (FAILED(hr)) {
        DLL_Log("[SwapRB] CreateSamplerState failed: HR=%x", hr);
        return false;
    }
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    ce::ComGuard<ID3D11SamplerState> p010Sampler;
    hr = d3d11Device->CreateSamplerState(&sampDesc, p010Sampler.addressof());
    if (FAILED(hr)) {
        DLL_Log("[HDR P010] CreateSamplerState failed: HR=%x", hr);
        return false;
    }

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = 32;
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ce::ComGuard<ID3D11Buffer> constants;
    hr = d3d11Device->CreateBuffer(&cbDesc, nullptr, constants.addressof());
    if (FAILED(hr)) {
        DLL_Log("[SwapRB] Create constant buffer failed: HR=%x", hr);
        return false;
    }

    swapRBShaderVS = copyVs.release();
    swapRBShaderPS = copyPs.release();
    hdrP010LumaPS = p010Y.release();
    hdrP010ChromaPS = p010Uv.release();
    swapRBSampler = copySampler.release();
    hdrP010Sampler = p010Sampler.release();
    swapRBShaderCB = constants.release();
    swapRBShaderCreated = true;
    DLL_Log("[RGBConvert] Copy/scRGB/P010 shaders created successfully (compiler lifetime blob-scoped)");
    return true;
}

ID3D11Texture2D* VideoEncoder::RenderFullscreenCopy(ID3D11Texture2D* input, uint32_t w, uint32_t h,
                                                    DXGI_FORMAT inputSrvFormat, DXGI_FORMAT outputFormat,
                                                    ID3D11Texture2D*& cachedTexture, ID3D11RenderTargetView*& cachedRTV,
                                                    uint32_t& cachedWidth, uint32_t& cachedHeight,
                                                    const char* logPrefix,
                                                    ce::video_format::RgbColorTransform colorTransform,
                                                    float toneMapSdrWhiteNits) {
    if (!EnsureSwapRBShader())
        return nullptr;

    if (!cachedTexture || cachedWidth != w || cachedHeight != h) {
        if (cachedRTV) {
            cachedRTV->Release();
            cachedRTV = nullptr;
        }
        if (cachedTexture) {
            cachedTexture->Release();
            cachedTexture = nullptr;
        }

        D3D11_TEXTURE2D_DESC outDesc = {};
        outDesc.Width = w;
        outDesc.Height = h;
        outDesc.MipLevels = 1;
        outDesc.ArraySize = 1;
        outDesc.Format = outputFormat;
        outDesc.SampleDesc.Count = 1;
        outDesc.Usage = D3D11_USAGE_DEFAULT;
        outDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = d3d11Device->CreateTexture2D(&outDesc, nullptr, &cachedTexture);
        if (FAILED(hr)) {
            DLL_Log("[%s] Failed to create output texture fmt=%d: HR=%x", logPrefix, outputFormat, hr);
            return nullptr;
        }

        hr = d3d11Device->CreateRenderTargetView(cachedTexture, nullptr, &cachedRTV);
        if (FAILED(hr)) {
            DLL_Log("[%s] Failed to create RTV: HR=%x", logPrefix, hr);
            cachedTexture->Release();
            cachedTexture = nullptr;
            return nullptr;
        }

        cachedWidth = w;
        cachedHeight = h;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = inputSrvFormat;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    D3D11_TEXTURE2D_DESC inputDesc = {};
    input->GetDesc(&inputDesc);
    HRESULT hr = d3d11Device->CreateShaderResourceView(input, &srvDesc, &srv);
    if (FAILED(hr)) {
        static std::atomic<int> srvFailLogCount{0};
        if (srvFailLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            DLL_Log("[%s] Failed to create SRV: texFmt=%d srvFmt=%d bind=%x misc=%x HR=%x", logPrefix, inputDesc.Format,
                    inputSrvFormat, inputDesc.BindFlags, inputDesc.MiscFlags, hr);
        }
        return nullptr;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = d3d11Context->Map(swapRBShaderCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr) || !mapped.pData) {
        DLL_Log("[%s] Failed to map shader constant buffer: HR=%x", logPrefix, hr);
        srv->Release();
        return nullptr;
    }
    memset(mapped.pData, 0, 32);
    uint32_t* cbData = static_cast<uint32_t*>(mapped.pData);
    cbData[0] = static_cast<uint32_t>(colorTransform);
    float* cbFloats = static_cast<float*>(mapped.pData);
    cbFloats[5] = std::clamp(toneMapSdrWhiteNits, 80.0f, 1000.0f);
    d3d11Context->Unmap(swapRBShaderCB, 0);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)w;
    vp.Height = (float)h;
    vp.MaxDepth = 1.0f;
    d3d11Context->RSSetViewports(1, &vp);
    d3d11Context->OMSetRenderTargets(1, &cachedRTV, nullptr);
    d3d11Context->VSSetShader(swapRBShaderVS, nullptr, 0);
    d3d11Context->PSSetShader(swapRBShaderPS, nullptr, 0);
    d3d11Context->PSSetShaderResources(0, 1, &srv);
    d3d11Context->PSSetSamplers(0, 1, &swapRBSampler);
    d3d11Context->PSSetConstantBuffers(0, 1, &swapRBShaderCB);
    d3d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3d11Context->IASetInputLayout(nullptr);
    d3d11Context->Draw(3, 0);

    // Unbind render target and SRV
    ID3D11RenderTargetView* nullRTV = nullptr;
    d3d11Context->OMSetRenderTargets(1, &nullRTV, nullptr);
    ID3D11ShaderResourceView* nullSRV = nullptr;
    d3d11Context->PSSetShaderResources(0, 1, &nullSRV);
    srv->Release();

    cachedTexture->AddRef();  // Caller releases
    return cachedTexture;
}

bool VideoEncoder::ConvertHdrRgb10ToP010(ID3D11Texture2D* input, ID3D11Texture2D* output, UINT outputArraySlice) {
    if (!input || !output || !EnsureSwapRBShader()) {
        return false;
    }

    D3D11_TEXTURE2D_DESC inputDesc = {};
    D3D11_TEXTURE2D_DESC outputDesc = {};
    input->GetDesc(&inputDesc);
    output->GetDesc(&outputDesc);
    if (inputDesc.Format != DXGI_FORMAT_R10G10B10A2_UNORM || outputDesc.Format != DXGI_FORMAT_P010 ||
        outputArraySlice >= outputDesc.ArraySize || (outputDesc.Width & 1) != 0 || (outputDesc.Height & 1) != 0) {
        DLL_Log(
            "[HDR P010] Invalid direct conversion surfaces: inputFmt=%d outputFmt=%d output=%ux%u array=%u slice=%u",
            inputDesc.Format, outputDesc.Format, outputDesc.Width, outputDesc.Height, outputDesc.ArraySize,
            outputArraySlice);
        return false;
    }

    CachedHdrP010OutputViews* outputViews = nullptr;
    for (auto& cached : hdrP010OutputViewCache) {
        if (cached.texture == output && cached.arraySlice == outputArraySlice) {
            outputViews = &cached;
            break;
        }
    }
    if (!outputViews) {
        CachedHdrP010OutputViews cached = {};
        cached.texture = output;
        cached.arraySlice = outputArraySlice;

        auto createPlaneView = [&](DXGI_FORMAT format, UINT plane,
                                   ID3D11RenderTargetView1** view) -> HRESULT {
            D3D11_RENDER_TARGET_VIEW_DESC1 desc = {};
            desc.Format = format;
            if (outputDesc.ArraySize > 1) {
                desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray.MipSlice = 0;
                desc.Texture2DArray.FirstArraySlice = outputArraySlice;
                desc.Texture2DArray.ArraySize = 1;
                desc.Texture2DArray.PlaneSlice = plane;
            } else {
                desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                desc.Texture2D.MipSlice = 0;
                desc.Texture2D.PlaneSlice = plane;
            }
            return d3d11Device->CreateRenderTargetView1(output, &desc, view);
        };

        HRESULT hr = createPlaneView(DXGI_FORMAT_R16_UNORM, 0, &cached.lumaView);
        if (SUCCEEDED(hr)) {
            hr = createPlaneView(DXGI_FORMAT_R16G16_UNORM, 1, &cached.chromaView);
        }
        if (FAILED(hr) || !cached.lumaView || !cached.chromaView) {
            DLL_Log(
                "[HDR P010] Failed to create plane RTVs: HR=%x bind=%x array=%u slice=%u; refusing corrupt VP "
                "fallback",
                hr, outputDesc.BindFlags, outputDesc.ArraySize, outputArraySlice);
            if (cached.lumaView)
                cached.lumaView->Release();
            if (cached.chromaView)
                cached.chromaView->Release();
            return false;
        }
        hdrP010OutputViewCache.push_back(cached);
        outputViews = &hdrP010OutputViewCache.back();
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* inputView = nullptr;
    HRESULT hr = d3d11Device->CreateShaderResourceView(input, &srvDesc, &inputView);
    if (FAILED(hr) || !inputView) {
        DLL_Log("[HDR P010] Failed to create RGB10 input SRV: HR=%x bind=%x", hr, inputDesc.BindFlags);
        return false;
    }

    struct CopyConstants {
        uint32_t colorTransform;
        uint32_t padding;
        float outputInvWidth;
        float outputInvHeight;
        float lumaSharpenStrength;
        float padding2[3];
    };
    static_assert(sizeof(CopyConstants) == 32);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = d3d11Context->Map(swapRBShaderCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr) || !mapped.pData) {
        DLL_Log("[HDR P010] Failed to map shader constants: HR=%x", hr);
        inputView->Release();
        return false;
    }
    const float lumaSharpenStrength =
        scalingEnabled ? std::clamp(savedConfig.scaling.sharpness / 400.0f, 0.0f, 0.25f) : 0.0f;
    *static_cast<CopyConstants*>(mapped.pData) = {
        0, 0, 1.0f / static_cast<float>(outputDesc.Width), 1.0f / static_cast<float>(outputDesc.Height),
        lumaSharpenStrength, {0.0f, 0.0f, 0.0f}};
    d3d11Context->Unmap(swapRBShaderCB, 0);

    d3d11Context->VSSetShader(swapRBShaderVS, nullptr, 0);
    d3d11Context->PSSetShaderResources(0, 1, &inputView);
    d3d11Context->PSSetSamplers(0, 1, &hdrP010Sampler);
    d3d11Context->PSSetConstantBuffers(0, 1, &swapRBShaderCB);
    d3d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3d11Context->IASetInputLayout(nullptr);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(outputDesc.Width);
    viewport.Height = static_cast<float>(outputDesc.Height);
    viewport.MaxDepth = 1.0f;
    d3d11Context->RSSetViewports(1, &viewport);
    ID3D11RenderTargetView* lumaView = outputViews->lumaView;
    d3d11Context->OMSetRenderTargets(1, &lumaView, nullptr);
    d3d11Context->PSSetShader(hdrP010LumaPS, nullptr, 0);
    d3d11Context->Draw(3, 0);

    viewport.Width = static_cast<float>(outputDesc.Width / 2);
    viewport.Height = static_cast<float>(outputDesc.Height / 2);
    d3d11Context->RSSetViewports(1, &viewport);
    ID3D11RenderTargetView* chromaView = outputViews->chromaView;
    d3d11Context->OMSetRenderTargets(1, &chromaView, nullptr);
    d3d11Context->PSSetShader(hdrP010ChromaPS, nullptr, 0);
    d3d11Context->Draw(3, 0);

    ID3D11RenderTargetView* nullRtv = nullptr;
    d3d11Context->OMSetRenderTargets(1, &nullRtv, nullptr);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    d3d11Context->PSSetShaderResources(0, 1, &nullSrv);
    inputView->Release();

    if (!hdrP010DirectLogged) {
        DLL_Log(
            "[HDR P010] Direct shader conversion active: input=RGB10_PQ_P2020 output=P010_BT2020NCL_LIMITED "
            "matrix=shader planes=R16/R16G16 scaling=%ux%u->%ux%u lumaSharpen=%.3f driverVP=0 cpuWait=0",
            inputDesc.Width, inputDesc.Height, outputDesc.Width, outputDesc.Height, lumaSharpenStrength);
        hdrP010DirectLogged = true;
    }
    return true;
}

ID3D11Texture2D* VideoEncoder::SwapRBChannels(ID3D11Texture2D* input, uint32_t w, uint32_t h) {
    return RenderFullscreenCopy(input, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, swapRBTexture,
                                swapRBTextureRTV, swapRBTexWidth, swapRBTexHeight, "SwapRB");
}

void VideoEncoder::CleanupVideoProcessor() {
    for (auto& cached : outputViewCache) {
        if (cached.view) {
            cached.view->Release();
            cached.view = nullptr;
        }
    }
    outputViewCache.clear();
    for (auto& cached : hdrP010OutputViewCache) {
        if (cached.lumaView) {
            cached.lumaView->Release();
            cached.lumaView = nullptr;
        }
        if (cached.chromaView) {
            cached.chromaView->Release();
            cached.chromaView = nullptr;
        }
    }
    hdrP010OutputViewCache.clear();

    CleanupCursorCompositionResources();

    if (inputView) {
        inputView->Release();
        inputView = nullptr;
    }
    if (videoProcessor) {
        videoProcessor->Release();
        videoProcessor = nullptr;
    }
    if (videoProcessorEnum) {
        videoProcessorEnum->Release();
        videoProcessorEnum = nullptr;
    }
    if (videoContext) {
        videoContext->Release();
        videoContext = nullptr;
    }
    if (videoContext1) {
        videoContext1->Release();
        videoContext1 = nullptr;
    }
    if (videoDevice) {
        videoDevice->Release();
        videoDevice = nullptr;
    }
    videoProcessorInit = false;
    use10BitPipeline = false;

    if (vpInputFp16StagingRTV) {
        vpInputFp16StagingRTV->Release();
        vpInputFp16StagingRTV = nullptr;
    }
    if (vpInputFp16Staging) {
        vpInputFp16Staging->Release();
        vpInputFp16Staging = nullptr;
    }
    vpInputFp16StagingW = 0;
    vpInputFp16StagingH = 0;

    // Cleanup SwapRB shader resources
    if (swapRBTextureRTV) {
        swapRBTextureRTV->Release();
        swapRBTextureRTV = nullptr;
    }
    if (swapRBTexture) {
        swapRBTexture->Release();
        swapRBTexture = nullptr;
    }
    if (rgb10IntermediateRTV) {
        rgb10IntermediateRTV->Release();
        rgb10IntermediateRTV = nullptr;
    }
    if (rgb10IntermediateTexture) {
        rgb10IntermediateTexture->Release();
        rgb10IntermediateTexture = nullptr;
    }
    if (swapRBSampler) {
        swapRBSampler->Release();
        swapRBSampler = nullptr;
    }
    if (hdrP010Sampler) {
        hdrP010Sampler->Release();
        hdrP010Sampler = nullptr;
    }
    if (swapRBShaderCB) {
        swapRBShaderCB->Release();
        swapRBShaderCB = nullptr;
    }
    if (swapRBShaderPS) {
        swapRBShaderPS->Release();
        swapRBShaderPS = nullptr;
    }
    if (hdrP010LumaPS) {
        hdrP010LumaPS->Release();
        hdrP010LumaPS = nullptr;
    }
    if (hdrP010ChromaPS) {
        hdrP010ChromaPS->Release();
        hdrP010ChromaPS = nullptr;
    }
    if (swapRBShaderVS) {
        swapRBShaderVS->Release();
        swapRBShaderVS = nullptr;
    }
    swapRBShaderCreated = false;
    swapRBTexWidth = 0;
    swapRBTexHeight = 0;
    rgb10IntermediateWidth = 0;
    rgb10IntermediateHeight = 0;

    // Reset per-recording log flags
    vpFirstCallLogged = false;
    vpDeviceCompareLogged = false;
    vpInputViewLogged = false;
    vpFp16CompatLogged = false;
    vpColorContractLogged = false;
    hdrP010DirectLogged = false;
    hdrToSdrLogged = false;
    sdrWhiteMonitor = nullptr;
    sdrWhiteNits = 203.0f;
    fp16VpInputStrategy = Fp16VpInputStrategy::kUnknown;
    cursorPrecompositionLogged = false;
    cursorFullCopyFallbackLogged = false;
    cursorPrecompositionFailureLogs = 0;
}

int64_t VideoEncoder::GetExpectedFinalDurationUs() const {
    if (lastAssignedVideoPts < 0)
        return 0;

    if (savedConfig.useVFR) {
        return lastAssignedVideoPts + (1000000 / (savedConfig.fps > 0 ? savedConfig.fps : 60));
    } else {
        int fps = (codecCtx && codecCtx->framerate.num > 0) ? codecCtx->framerate.num : savedConfig.fps;
        if (fps <= 0)
            fps = 60;
        return av_rescale(lastAssignedVideoPts + 1, 1000000, fps);
    }
}

int64_t VideoEncoder::GetAssignedCfrFrameCount() const {
    return !savedConfig.useVFR && lastAssignedVideoPts >= 0 ? lastAssignedVideoPts + 1 : 0;
}

int VideoEncoder::GetConfiguredFps() const {
    return savedConfig.fps > 0 ? savedConfig.fps : 0;
}

int64_t VideoEncoder::GetEncodedDurationUs() const {
    int64_t encodedUs = encodedDurationUs.load(std::memory_order_relaxed);
    if (encodedUs > 0) {
        return encodedUs;
    }

    if (!codecCtx || codecCtx->framerate.num == 0)
        return 0;

    // Fallback for early startup before first packet is emitted.
    if (lastAssignedVideoPts >= 0) {
        return GetExpectedFinalDurationUs();
    }
    return av_rescale(encodeFrameCounter, 1000000 * (int64_t)codecCtx->framerate.den, codecCtx->framerate.num);
}

int64_t VideoEncoder::GetLastFrameEncodeTimeUs() const {
    return lastEncodeTimeUs;
}

int64_t VideoEncoder::GetLastFrameFenceWaitUs() const {
    return lastFenceWaitUs;
}

bool VideoEncoder::CanRepeatLastFrame() const {
    return recordingRequested &&
           (repeatFrameTexture != nullptr || (repeatSourceNeedsCursorRecompose && repeatSourceFrameTexture != nullptr));
}

void VideoEncoder::ResetRepeatFrameCache() {
    const bool hadCachedContent = repeatFrameTexture != nullptr || repeatSourceFrameTexture != nullptr;
    if (repeatFrameTexture) {
        repeatFrameTexture->Release();
        repeatFrameTexture = nullptr;
    }
    InvalidateRepeatSourceFrameTexture();
    repeatSourceCacheFailureLogged = false;
    repeatCursorRecomposeFallbackLogged = false;
    repeatSourceCacheKeyedMutexLogged = false;
    repeatSourceCacheKeyedAcquireFailCount = 0;
    if (hadCachedContent) {
        DLL_Log("[VideoEncoder] Repeat-frame cache invalidated for capture-source transition");
    }
}

bool VideoEncoder::WasLastFrameDeferred() const {
    return lastFrameDeferred.load(std::memory_order_relaxed);
}
// Async Packet Writer Loop
void VideoEncoder::AsyncWriteLoop() {
    DLL_Log("[VideoEncoder] Async Writer Thread Started");

    while (writerRunning || isStopping) {
        std::unique_lock<std::mutex> lock(queueMutex);

        // Wait for packets or stop signal
        queueCV.wait(lock, [this] { return !packetQueue.empty() || isStopping || !writerRunning; });

        // Drain queue
        while (!packetQueue.empty()) {
            AVPacket* pkt = packetQueue.front();
            packetQueue.pop();
            currentQueuePackets.store(SaturatingToUint32(packetQueue.size()), std::memory_order_relaxed);

            size_t pktSize = pkt->size + sizeof(AVPacket);
            currentQueueBytes -= pktSize;

            lock.unlock();  // Release lock while doing I/O

            if (fileOpened && fmtCtx) {
                // Log last few audio/video packets to verify PTS alignment
                if (pkt->stream_index != stream->index) {
                    if (audioWriteLogCount++ < 5 || audioWriteLogCount % 500 == 0) {
                        AVStream* ast = fmtCtx->streams[pkt->stream_index];
                        int64_t aPtsUs = av_rescale_q(pkt->pts, ast->time_base, AVRational{1, 1000000});
                        AVStream* vst = fmtCtx->streams[stream->index];
                        int64_t lastVUs = lastMuxerVideoPtsUs.load(std::memory_order_relaxed);
                        DLL_Log("[MuxAudio] pkt#%d pts=%lld tb=%d/%d ptsUs=%lld lastVideoPtsUs=%lld diffMs=%lld",
                                audioWriteLogCount, (long long)pkt->pts, ast->time_base.num, ast->time_base.den, aPtsUs,
                                lastVUs, (aPtsUs - lastVUs) / 1000);
                    }
                } else {
                    lastMuxerVideoPtsUs.store(av_rescale_q(pkt->pts, stream->time_base, AVRational{1, 1000000}),
                                              std::memory_order_relaxed);
                }
                const int writtenStreamIndex = pkt->stream_index;
                const int64_t writtenPts = pkt->pts;
                const int64_t writtenDts = pkt->dts;
                const int64_t writtenDuration = pkt->duration;
                const AVRational writtenTimeBase = fmtCtx->streams[pkt->stream_index]->time_base;
                const uint32_t writtenTerminalDiscardSamples = GetPacketTerminalDiscardSamples(pkt);
                const int writtenSampleRate = fmtCtx->streams[pkt->stream_index]->codecpar
                                                  ? fmtCtx->streams[pkt->stream_index]->codecpar->sample_rate
                                                  : 0;
                int ret = av_interleaved_write_frame(fmtCtx, pkt);
                if (ret >= 0) {
                    RecordWrittenPacketTimeline(writtenStreamIndex, writtenPts, writtenDts, writtenDuration,
                                                writtenTimeBase, writtenTerminalDiscardSamples, writtenSampleRate);
                }
                if (ret < 0) {
                    if (asyncWriteErrorCount++ < 10) {
                        char errbuf[AV_ERROR_MAX_STRING_SIZE];
                        av_strerror(ret, errbuf, sizeof(errbuf));
                        DLL_Log(
                            "[VideoEncoder] ERROR: av_interleaved_write_frame failed: "
                            "%d (%s) pts=%lld",
                            ret, errbuf, pkt->pts);
                    }
                }
            }

            av_packet_free(&pkt);
            PublishRuntimeState();
            lock.lock();  // Re-acquire lock
        }

        // Handle Stop/Flush signal
        if (isStopping) {
            writerFinalizePhase.store(kWriterPhaseFinalizeStarting, std::memory_order_release);
            DLL_Log("[VideoEncoder] Async Finalize: Starting...");

            // 1. Flush Encoder if valid
            if (initDone && codecCtx && fileOpened) {
                writerFinalizePhase.store(kWriterPhaseFlushingEncoder, std::memory_order_release);
                DLL_Log("[VideoEncoder] Async Finalize: Flushing encoder...");
                avcodec_send_frame(codecCtx, nullptr);

                AVPacket* pkt = av_packet_alloc();
                int flushedCount = 0;
                while (avcodec_receive_packet(codecCtx, pkt) == 0) {
                    // We need to set stream index and rescale PTS here
                    // Note: We use the same write logic as WriteFrame but simplified
                    pkt->stream_index = stream->index;

                    av_packet_rescale_ts(pkt, codecCtx->time_base, stream->time_base);
                    if (pkt->dts == AV_NOPTS_VALUE) {
                        pkt->dts = pkt->pts;
                    }
                    if (pkt->duration <= 0) {
                        // Use same Bresenham-aware duration as WriteFrame for
                        // consistent frame timing in the last flushed packets.
                        if (!savedConfig.useVFR && codecCtx->time_base.den > 0) {
                            int64_t frameNum =
                                av_rescale_q_rnd(pkt->pts, stream->time_base, codecCtx->time_base, AV_ROUND_NEAR_INF);
                            int64_t nextPts = av_rescale_q(frameNum + 1, codecCtx->time_base, stream->time_base);
                            pkt->duration = nextPts - pkt->pts;
                        }
                        if (pkt->duration <= 0) {
                            int fps = codecCtx->framerate.num;
                            if (fps > 0) {
                                pkt->duration = av_rescale(1, stream->time_base.den, fps);
                            }
                        }
                        if (pkt->duration <= 0) {
                            pkt->duration = av_rescale_q(1, codecCtx->time_base, stream->time_base);
                        }
                        if (pkt->duration <= 0) {
                            pkt->duration = 1;
                        }
                    }
                    // Clamp flushed packet duration to sane range
                    {
                        int64_t maxDuration = av_rescale_q(2, codecCtx->time_base, stream->time_base);
                        if (maxDuration < 2)
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
