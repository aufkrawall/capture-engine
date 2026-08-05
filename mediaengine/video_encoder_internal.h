#pragma once

namespace {
struct PostMuxProbeControl;
}

struct HandleFailureCache;

namespace {
struct ResolvedVideoFormat;
}

class D3D11ScopedLock;

class PerfTimer;

struct FrameStats;

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
}

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

namespace fs = std::filesystem;

#ifndef D3D11_FORMAT_SUPPORT_SHAREABLE
#define D3D11_FORMAT_SUPPORT_SHAREABLE 0x2000
#endif

namespace {
enum class OutputRangeMode { kLimited, kFull };
}

extern "C" __declspec(dllexport) HRESULT __cdecl CallOpenSharedFence(ID3D11Device5* dev, HANDLE h, ID3D11Fence** out);

extern "C" __declspec(dllexport) HRESULT __cdecl CallOpenSharedResource(ID3D11Device5* dev, HANDLE h, REFIID riid, void** out);

extern "C" __declspec(dllexport) HRESULT __cdecl CallOpenSharedResource1(ID3D11Device5* dev, HANDLE h, REFIID riid, void** out);

extern "C" {
#include <libavutil/intreadwrite.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

inline void TrimD3D11Residency(ID3D11Device* device, ID3D11DeviceContext* context, const char* label) {
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

inline const char* WriterFinalizePhaseName(uint32_t phase) {
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
inline bool WriterFinishedWithin(std::future<void>& finished, uint64_t timeoutMs) {
    // A joinable writer is always paired with a valid future. Should that ever
    // not hold, report "still running" so muxer ownership is never assumed free.
    if (!finished.valid()) {
        return false;
    }
    return finished.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::ready;
}

inline bool HasValidStreamTimeBase(const AVStream* stream) {
    return stream && stream->time_base.num > 0 && stream->time_base.den > 0;
}

inline constexpr uint64_t video_encoder_kPostMuxProbeTimeoutMs = 5000;

inline constexpr int video_encoder_kPostMuxProbeMaxPackets = 512;

inline constexpr int video_encoder_kPostMuxProbeMaxTailPackets = 16384;

namespace {
struct PostMuxProbeControl {
    std::atomic<bool> cancel{false};
    uint64_t deadlineTickMs = 0;
};
}

inline bool ShouldCancelPostMuxProbe(const PostMuxProbeControl* control) {
    if (!control) {
        return false;
    }
    if (control->cancel.load(std::memory_order_acquire)) {
        return true;
    }
    return control->deadlineTickMs > 0 && GetTickCount64() >= control->deadlineTickMs;
}

inline int PostMuxProbeInterrupt(void* opaque) {
    return ShouldCancelPostMuxProbe(static_cast<PostMuxProbeControl*>(opaque)) ? 1 : 0;
}

inline int64_t ParseDurationTagUs(const char* value) {
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

inline int64_t GetStreamStartUs(const AVStream* stream) {
    if (!HasValidStreamTimeBase(stream) || stream->start_time == AV_NOPTS_VALUE) {
        return 0;
    }

    return av_rescale_q(stream->start_time, stream->time_base, AVRational{1, 1000000});
}

inline int64_t GetStreamDurationUs(const AVStream* stream) {
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

inline uint32_t GetPacketTerminalDiscardSamples(const AVPacket* packet) {
    if (!packet) {
        return 0;
    }
    size_t sideDataSize = 0;
    const uint8_t* sideData = av_packet_get_side_data(packet, AV_PKT_DATA_SKIP_SAMPLES, &sideDataSize);
    return sideData && sideDataSize >= 8 ? AV_RL32(sideData + 4) : 0;
}

inline bool LogPostMuxDurationProbe(const std::string& filename, int64_t finalDurationUs,
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
    while (pkt && packetsRead < video_encoder_kPostMuxProbeMaxPackets && !ShouldCancelPostMuxProbe(control)) {
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
    if (packetsRead >= video_encoder_kPostMuxProbeMaxPackets) {
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
        while (pkt && tailPacketsRead < video_encoder_kPostMuxProbeMaxTailPackets && !ShouldCancelPostMuxProbe(control)) {
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
                filename.c_str(), ret, tailPacketsRead, video_encoder_kPostMuxProbeMaxTailPackets);
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

inline void RunPostMuxDurationProbeBounded(const std::string& filename, int64_t finalDurationUs,
                                    uint64_t timeoutMs = video_encoder_kPostMuxProbeTimeoutMs) {
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

inline bool ValidateFormatContextForHeader(const AVFormatContext* fmtCtx) {
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

inline int64_t ComputeTargetVideoPts(int64_t timestampUs, bool useVfr, int fps, int64_t startPts, int64_t lastAssignedVideoPts,
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

inline bool IsConfiguredNvencLookaheadActive(const std::string& value) {
    return !value.empty() && _stricmp(value.c_str(), "off") != 0 && _stricmp(value.c_str(), "false") != 0 &&
           _stricmp(value.c_str(), "disabled") != 0 && value != "0";
}

inline bool IsConfiguredNvencMultipassActive(const VideoConfig& config) {
    if (_stricmp(config.multipass.c_str(), "qres") == 0 || _stricmp(config.multipass.c_str(), "fullres") == 0) {
        return true;
    }
    if (!config.multipass.empty() && _stricmp(config.multipass.c_str(), "auto") != 0) {
        return false;
    }
    return config.bFrames > 0 || _stricmp(config.rateControl.c_str(), "cbr") == 0;
}

inline void LogFinalDurationSummary(AVFormatContext* fmtCtx, int64_t finalDurationUs, uint32_t muxBackpressureEvents,
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

inline HandleFailureCache video_encoder_g_HandleFailureCache;

template <typename AtomicT>
void UpdateAtomicPeak(AtomicT& peak, uint32_t value) {
    uint32_t current = peak.load(std::memory_order_relaxed);
    while (value > current &&
           !peak.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

inline uint32_t SaturatingToUint32(uint64_t value) {
    return value > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(value);
}

namespace {
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
}

inline const char* GetPixFmtNameSafe(AVPixelFormat pixFmt) {
    const char* name = av_get_pix_fmt_name(pixFmt);
    return name ? name : "unknown";
}

inline bool SupportsCodecPixelFormat(const AVCodec* codec, AVPixelFormat pixFmt) {
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

inline bool SupportsD3D11HwInputFormat(const AVCodec* codec, AVPixelFormat swFormat) {
    return SupportsCodecPixelFormat(codec, AV_PIX_FMT_D3D11) && SupportsCodecPixelFormat(codec, swFormat);
}

inline bool DeviceSupportsHwFrameSwFormat(AVBufferRef* deviceCtx, AVPixelFormat swFormat) {
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

inline bool IsDirectRgbD3D11SwFormat(AVPixelFormat swFormat) {
    return swFormat == AV_PIX_FMT_BGRA || swFormat == AV_PIX_FMT_X2BGR10;
}

inline bool UsesQsvHardwareFrames(const std::string& encoderName) {
    return encoderName.size() >= 4 &&
           _stricmp(encoderName.c_str() + encoderName.size() - 4, "_qsv") == 0;
}

inline std::string ResolveRequestedBitDepth(const VideoConfig& config, bool prefer10Bit) {
    if (config.bitDepth.empty() || _stricmp(config.bitDepth.c_str(), "auto") == 0) {
        return prefer10Bit ? "10" : "8";
    }
    return config.bitDepth;
}

inline std::string ResolveRequestedChroma(const VideoConfig& config) {
    if (config.chromaSubsampling.empty() || _stricmp(config.chromaSubsampling.c_str(), "auto") == 0) {
        return "420";
    }
    return config.chromaSubsampling;
}

inline bool ResolveVideoFormat(const VideoConfig& config, bool isHDR, bool prefer10Bit, const AVCodec* codec,
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
        resolved.codecPixFmt = UsesQsvHardwareFrames(config.encoder) ? AV_PIX_FMT_QSV : AV_PIX_FMT_D3D11;
        resolved.d3d11SwFormat = resolved.use10Bit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
        resolved.directDxgiFormat = DXGI_FORMAT_UNKNOWN;
        resolved.usesVideoProcessor = true;
        resolved.requiresEvenDimensions = true;
        if (resolved.codecPixFmt == AV_PIX_FMT_QSV) {
            if (!SupportsCodecPixelFormat(codec, AV_PIX_FMT_QSV) ||
                !SupportsCodecPixelFormat(codec, resolved.d3d11SwFormat)) {
                if (error) {
                    *error = "[VideoEncoder] The selected Quick Sync codec does not support the requested bit depth";
                }
                return false;
            }
        } else if (!SupportsD3D11HwInputFormat(codec, resolved.d3d11SwFormat)) {
            if (error) {
                *error = "[VideoEncoder] The selected encoder does not support the requested D3D11 hardware input format";
            }
            return false;
        }
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

inline bool WantsFullOutputRange(const std::string& colorRange) {
    return !colorRange.empty() && _stricmp(colorRange.c_str(), "full") == 0;
}

inline OutputRangeMode GetEffectiveOutputRange(const std::string& colorRange, bool /*isHDR*/) {
    if (WantsFullOutputRange(colorRange)) {
        return OutputRangeMode::kFull;
    }
    return OutputRangeMode::kLimited;
}

inline AVColorRange GetAVColorRange(OutputRangeMode range) {
    return range == OutputRangeMode::kFull ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
}

inline const char* DescribeOutputRange(OutputRangeMode range) {
    return range == OutputRangeMode::kFull ? "full" : "limited";
}

inline bool ApplyFrameColorMetadata(AVFrame* frame, const AVCodecContext* codec, int hdrNominalPeakNits) {
    if (!frame || !codec) {
        return false;
    }

    frame->color_range = codec->color_range;
    frame->color_primaries = codec->color_primaries;
    frame->color_trc = codec->color_trc;
    frame->colorspace = codec->colorspace;
    frame->chroma_location = codec->chroma_sample_location;

    if (codec->color_trc != AVCOL_TRC_SMPTE2084 || codec->color_primaries != AVCOL_PRI_BT2020) {
        return true;
    }

    const int metadataResult =
        ce::video_metadata::AddNominalHdrMetadataToFrame(frame, hdrNominalPeakNits);
    if (metadataResult < 0) {
        DLL_Log("[HDR Metadata] Failed to attach frame metadata: %d", metadataResult);
        return false;
    }
    return true;
}

inline DXGI_COLOR_SPACE_TYPE GetVideoProcessorInputColorSpace(DXGI_FORMAT format, bool isHDR, bool forceLinear = false) {
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

inline DXGI_COLOR_SPACE_TYPE GetVideoProcessorOutputColorSpace(bool use10Bit, bool isHDR, const std::string& colorSpace,
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

inline bool QuerySdrWhiteLevelNits(HMONITOR monitor, float* nits, ULONG* rawLevel) {
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
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
            sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sourceName.header.size = sizeof(sourceName);
            sourceName.header.adapterId = path.sourceInfo.adapterId;
            sourceName.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS ||
                lstrcmpiW(sourceName.viewGdiDeviceName, monitorInfo.szDevice) != 0) {
                continue;
            }

            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
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

inline HANDLE NormalizeSourceHandleForWow64(HANDLE handle, uint32_t sourcePid) {
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

inline ce::capture_output::ReservedCaptureOutput ReserveOutputStagingFile(const VideoConfig& config) {
    const fs::path exeDir = ce::capture_output::GetExecutableDirectory();
    const fs::path outDir = ce::capture_output::ResolveCaptureDirectory(config.outputDir, exeDir);
    const std::wstring ext(config.container.begin(), config.container.end());
    auto reservation = ce::capture_output::ReservedCaptureOutput::Reserve(outDir, L"capture_stage", ext);
    if (reservation) {
        DLL_Log("[VideoEncoder] Reserved unpublished staging output: %s", reservation.Utf8Path().c_str());
    } else {
        DLL_Log("[VideoEncoder] ERROR: Could not reserve a collision-safe staging output in: %s",
                outDir.string().c_str());
    }
    return reservation;
}

inline int AllocateOutputContextForContainer(AVFormatContext** formatContext, const VideoConfig& config) {
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

inline int64_t RoundUsToMs(int64_t valueUs) {
    if (valueUs >= 0) {
        return (valueUs + 500) / 1000;
    }
    return (valueUs - 500) / 1000;
}

// Global stats for frame analysis
inline int64_t video_encoder_g_lastFramePts = -1;

inline int64_t video_encoder_g_framesEncoded = 0;

// static int64_t g_framesDropped = 0;
inline double video_encoder_g_totalFenceWait = 0;

inline double video_encoder_g_totalColorConvert = 0;

inline double video_encoder_g_totalEncode = 0;

inline double video_encoder_g_maxFrameTime = 0;

inline int video_encoder_g_slowFrameCount = 0;  // Frames taking > 2x expected time
