#include "recording_recovery.h"

#include "common/capture/reserved_capture_output.h"
#include "common/logging/log_privacy.h"
#include "mediaengine/engine/mediaengine.h"
#include "mediaengine/mux/matroska_timing.h"

#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <cwctype>
#include <limits>
#include <memory>
#include <vector>

namespace ce::media {
namespace {

constexpr int64_t kNoEnd = std::numeric_limits<int64_t>::max();

struct InputCloser {
    void operator()(AVFormatContext* context) const {
        if (context)
            avformat_close_input(&context);
    }
};
using InputContext = std::unique_ptr<AVFormatContext, InputCloser>;

struct PacketDeleter {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
using Packet = std::unique_ptr<AVPacket, PacketDeleter>;

std::string Utf8(const std::filesystem::path& path) {
    const std::wstring wide = path.wstring();
    if (wide.empty())
        return {};
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(std::max(bytes, 0)), '\0');
    if (bytes > 0)
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), bytes, nullptr,
                            nullptr);
    return utf8;
}

std::string Leaf(const std::filesystem::path& path) {
    return ce::privacy::CollapsePathForLog(Utf8(path.filename()));
}

// The muxer for the staging file's extension; the recovered file keeps the container the recording was made in.
const char* MuxerFor(const std::filesystem::path& staging) {
    std::wstring extension = staging.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (extension == L".mp4")
        return "mp4";
    if (extension == L".mov")
        return "mov";
    return "matroska";
}

bool Canceled(const volatile long* cancel) {
    return cancel && *cancel != 0;
}

// FFmpeg polls this during blocking reads and writes.
int InterruptRequested(void* opaque) {
    return Canceled(static_cast<const volatile long*>(opaque)) ? 1 : 0;
}

InputContext OpenInput(const std::string& path, const volatile long* cancel) {
    AVFormatContext* raw = avformat_alloc_context();
    if (!raw)
        return InputContext();
    raw->interrupt_callback.callback = &InterruptRequested;
    raw->interrupt_callback.opaque = const_cast<long*>(cancel);
    if (avformat_open_input(&raw, path.c_str(), nullptr, nullptr) < 0)  // frees `raw` on failure
        return InputContext();
    InputContext input(raw);
    if (avformat_find_stream_info(input.get(), nullptr) < 0)
        return InputContext();
    return input;
}

int64_t PacketStart(const AVPacket* packet) {
    return packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
}

// A packet's end in its stream's time base; video packets without a duration last one frame.
int64_t PacketEnd(const AVPacket* packet, const AVStream* stream) {
    const int64_t start = PacketStart(packet);
    if (start == AV_NOPTS_VALUE)
        return AV_NOPTS_VALUE;
    int64_t duration = packet->duration;
    if (duration <= 0 && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && stream->avg_frame_rate.num > 0)
        duration = av_rescale_q(1, av_inv_q(stream->avg_frame_rate), stream->time_base);
    return start + std::max<int64_t>(duration, 0);
}

int64_t ToUs(int64_t value, AVRational timeBase) {
    return av_rescale_q(value, timeBase, AVRational{1, AV_TIME_BASE});
}

bool IsPcm(const AVCodecParameters* parameters) {
    switch (parameters->codec_id) {
        case AV_CODEC_ID_PCM_S16LE:
        case AV_CODEC_ID_PCM_S24LE:
        case AV_CODEC_ID_PCM_S32LE:
        case AV_CODEC_ID_PCM_F32LE:
        case AV_CODEC_ID_PCM_F64LE:
            return parameters->block_align > 0;
        default:
            return false;
    }
}

struct StreamPlan {
    bool video = false;
    uint64_t packets = 0;
    int64_t endUs = AV_NOPTS_VALUE;        // where the track's data ends
    int64_t videoPrefixEndUs = AV_NOPTS_VALUE;  // video: end of the decode-order prefix kept for the common end
    bool videoPrefixClosed = false;
};

enum class PlanResult { kPlanned, kNoMedia, kCanceled };

// Pass 1: where each track ends, then the common end every track is cut to.
PlanResult PlanCut(AVFormatContext* input, std::vector<StreamPlan>& plans, int64_t* commonEndUs, std::string* detail,
                   const volatile long* cancel) {
    plans.assign(input->nb_streams, StreamPlan{});
    for (unsigned i = 0; i < input->nb_streams; ++i)
        plans[i].video = input->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
    Packet packet(av_packet_alloc());
    if (!packet) {
        *detail = "out of memory";
        return PlanResult::kCanceled;
    }
    // Every packet's end, video in decode order, so the video prefix is found without a second read.
    std::vector<std::vector<int64_t>> videoEnds(input->nb_streams);
    while (av_read_frame(input, packet.get()) >= 0) {
        if (Canceled(cancel)) {
            av_packet_unref(packet.get());
            *detail = "canceled";
            return PlanResult::kCanceled;
        }
        const unsigned index = static_cast<unsigned>(packet->stream_index);
        if (index < input->nb_streams) {
            const AVStream* stream = input->streams[index];
            const int64_t end = PacketEnd(packet.get(), stream);
            if (end != AV_NOPTS_VALUE) {
                const int64_t endUs = ToUs(end, stream->time_base);
                StreamPlan& plan = plans[index];
                ++plan.packets;
                plan.endUs = plan.endUs == AV_NOPTS_VALUE ? endUs : std::max(plan.endUs, endUs);
                if (plan.video)
                    videoEnds[index].push_back(endUs);
            }
        }
        av_packet_unref(packet.get());
    }
    int64_t commonEnd = kNoEnd;
    for (const StreamPlan& plan : plans) {
        if (plan.packets > 0)
            commonEnd = std::min(commonEnd, plan.endUs);
    }
    if (Canceled(cancel)) {
        *detail = "canceled";
        return PlanResult::kCanceled;
    }
    if (commonEnd == kNoEnd) {
        *detail = "no media packets";
        return PlanResult::kNoMedia;
    }
    // Video ends on a complete frame: the decode-order prefix whose frames all end by the common end. The audio is
    // then cut to that frame boundary, so every track ends at the same instant.
    for (unsigned i = 0; i < input->nb_streams; ++i) {
        StreamPlan& plan = plans[i];
        if (!plan.video || plan.packets == 0)
            continue;
        int64_t prefixEnd = AV_NOPTS_VALUE;
        for (const int64_t endUs : videoEnds[i]) {
            if (endUs > commonEnd)
                break;
            prefixEnd = prefixEnd == AV_NOPTS_VALUE ? endUs : std::max(prefixEnd, endUs);
        }
        if (prefixEnd == AV_NOPTS_VALUE) {
            *detail = "no complete video frame before the common end";
            return PlanResult::kNoMedia;
        }
        plan.videoPrefixEndUs = prefixEnd;
        commonEnd = std::min(commonEnd, prefixEnd);
    }
    *commonEndUs = commonEnd;
    return PlanResult::kPlanned;
}

// Cuts `packet` (an audio packet that runs past `endUs`) to end exactly there.
void TrimAudioTail(AVPacket* packet, const AVStream* stream, int64_t endUs) {
    const AVCodecParameters* parameters = stream->codecpar;
    const int sampleRate = parameters->sample_rate;
    if (sampleRate <= 0)
        return;
    const int64_t startUs = ToUs(PacketStart(packet), stream->time_base);
    const int64_t packetEndUs = ToUs(PacketEnd(packet, stream), stream->time_base);
    const int64_t keepSamples = av_rescale(endUs - startUs, sampleRate, AV_TIME_BASE);
    const int64_t totalSamples = std::max(av_rescale(packetEndUs - startUs, sampleRate, AV_TIME_BASE), keepSamples);
    const int64_t excess = totalSamples - keepSamples;
    if (excess <= 0)
        return;
    if (IsPcm(parameters)) {
        const int64_t keepBytes = keepSamples * parameters->block_align;
        if (keepBytes >= 0 && keepBytes < packet->size)
            packet->size = static_cast<int>(keepBytes);
        packet->duration = av_rescale_q(keepSamples, AVRational{1, sampleRate}, stream->time_base);
        return;
    }
    size_t size = 0;
    uint8_t* side = av_packet_get_side_data(packet, AV_PKT_DATA_SKIP_SAMPLES, &size);
    if (!side || size < 10)
        side = av_packet_new_side_data(packet, AV_PKT_DATA_SKIP_SAMPLES, 10);
    if (!side)
        return;
    AV_WL32(side + 4, static_cast<uint32_t>(std::min<int64_t>(excess, std::numeric_limits<uint32_t>::max())));
}

bool Remux(const std::filesystem::path& staging, const std::string& outputPath, const std::vector<StreamPlan>& plans,
           int64_t commonEndUs, std::string* detail, const volatile long* cancel) {
    InputContext input = OpenInput(Utf8(staging), cancel);
    if (!input) {
        *detail = "reopen failed";
        return false;
    }
    AVFormatContext* output = nullptr;
    if (avformat_alloc_output_context2(&output, nullptr, MuxerFor(staging), outputPath.c_str()) < 0 || !output) {
        *detail = "output context";
        return false;
    }
    struct OutputCloser {
        AVFormatContext* context;
        ~OutputCloser() {
            if (context) {
                if (context->pb)
                    avio_closep(&context->pb);
                avformat_free_context(context);
            }
        }
    } closer{output};
    for (unsigned i = 0; i < input->nb_streams; ++i) {
        const AVStream* in = input->streams[i];
        AVStream* out = avformat_new_stream(output, nullptr);
        if (!out || avcodec_parameters_copy(out->codecpar, in->codecpar) < 0) {
            *detail = "stream copy";
            return false;
        }
        out->codecpar->codec_tag = 0;
        out->time_base = in->time_base;
        out->disposition = in->disposition;
        av_dict_copy(&out->metadata, in->metadata, 0);
    }
    av_dict_copy(&output->metadata, input->metadata, 0);
    if (!RequireMicrosecondMatroskaTimestampPrecision(output)) {
        *detail = "matroska timestamp precision";
        return false;
    }
    output->interrupt_callback.callback = &InterruptRequested;
    output->interrupt_callback.opaque = const_cast<long*>(cancel);
    if (avio_open2(&output->pb, outputPath.c_str(), AVIO_FLAG_WRITE, &output->interrupt_callback, nullptr) < 0) {
        *detail = "open output";
        return false;
    }
    if (avformat_write_header(output, nullptr) < 0) {
        *detail = "header";
        return false;
    }
    std::vector<bool> videoClosed(input->nb_streams, false);
    Packet packet(av_packet_alloc());
    if (!packet)
        return false;
    int writeErrors = 0;
    while (av_read_frame(input.get(), packet.get()) >= 0) {
        if (Canceled(cancel)) {
            av_packet_unref(packet.get());
            *detail = "canceled";
            return false;
        }
        const unsigned index = static_cast<unsigned>(packet->stream_index);
        if (index >= input->nb_streams || PacketStart(packet.get()) == AV_NOPTS_VALUE) {
            av_packet_unref(packet.get());
            continue;
        }
        const AVStream* stream = input->streams[index];
        const int64_t startUs = ToUs(PacketStart(packet.get()), stream->time_base);
        const int64_t endUs = ToUs(PacketEnd(packet.get(), stream), stream->time_base);
        bool keep = true;
        if (plans[index].video) {
            // The decode-order prefix: everything after the first frame past the common end is dropped, so no kept
            // frame references a dropped one.
            if (videoClosed[index] || endUs > commonEndUs) {
                videoClosed[index] = true;
                keep = false;
            }
        } else if (startUs >= commonEndUs) {
            keep = false;
        } else if (endUs > commonEndUs && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            TrimAudioTail(packet.get(), stream, commonEndUs);
        }
        if (!keep) {
            av_packet_unref(packet.get());
            continue;
        }
        packet->pos = -1;
        av_packet_rescale_ts(packet.get(), stream->time_base, output->streams[index]->time_base);
        if (av_interleaved_write_frame(output, packet.get()) < 0)
            ++writeErrors;
        av_packet_unref(packet.get());
    }
    if (av_write_trailer(output) < 0 || writeErrors > 0) {
        *detail = writeErrors > 0 ? "packet write errors" : "trailer";
        return false;
    }
    if (avio_closep(&output->pb) < 0) {
        *detail = "close output";
        return false;
    }
    return true;
}

}  // namespace

const char* RecoveryOutcomeName(RecoveryOutcome outcome) {
    switch (outcome) {
        case RecoveryOutcome::kRecovered:
            return "recovered";
        case RecoveryOutcome::kDiscardedEmpty:
            return "discarded_empty";
        case RecoveryOutcome::kKeptIncomplete:
            return "kept_incomplete";
        case RecoveryOutcome::kWriterAlive:
            return "writer_alive";
        case RecoveryOutcome::kMissing:
            return "missing";
        case RecoveryOutcome::kFailed:
            break;
    }
    return "failed";
}

RecoveryReport RecoverInterruptedRecording(const std::filesystem::path& staging, const volatile long* cancel) {
    RecoveryReport report;
    // An exclusive open fails while the writer still holds its reservation (reserved_capture_output.h).
    HANDLE probe = CreateFileW(staging.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (probe == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            report.outcome = RecoveryOutcome::kMissing;
        else if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION)
            report.outcome = RecoveryOutcome::kWriterAlive;
        else
            report.detail = "open error " + std::to_string(error);
        return report;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(probe, &size);
    CloseHandle(probe);
    if (size.QuadPart == 0) {
        DeleteFileW(staging.c_str());
        report.outcome = RecoveryOutcome::kDiscardedEmpty;
        report.detail = "empty";
        DLL_Log("[RecordingRecovery] staging=%s outcome=%s", Leaf(staging).c_str(), RecoveryOutcomeName(report.outcome));
        return report;
    }

    const std::filesystem::path directory = staging.parent_path();
    const std::wstring extension = staging.extension().wstring().empty() ? L"mkv" : staging.extension().wstring().substr(1);
    if (Canceled(cancel)) {
        report.detail = "canceled";
        return report;
    }
    InputContext input = OpenInput(Utf8(staging), cancel);
    if (!input) {
        if (Canceled(cancel)) {
            report.detail = "canceled";
            return report;
        }
        // Bytes without a readable header: keep them under a name that says so, never delete user data.
        ce::capture_output::ReservedCaptureOutput name =
            ce::capture_output::ReservedCaptureOutput::Reserve(directory, L"capture_incomplete", extension);
        if (name) {
            const std::filesystem::path target = name.Path();
            name.CleanupOwnedFile();
            if (MoveFileExW(staging.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
                report.outcome = RecoveryOutcome::kKeptIncomplete;
                report.output = target;
            }
        }
        report.detail = "unreadable header";
        DLL_Log("[RecordingRecovery] staging=%s bytes=%lld outcome=%s", Leaf(staging).c_str(),
                static_cast<long long>(size.QuadPart), RecoveryOutcomeName(report.outcome));
        return report;
    }
    report.streams = input->nb_streams;
    std::vector<StreamPlan> plans;
    int64_t commonEndUs = 0;
    std::string planDetail;
    const PlanResult planned = PlanCut(input.get(), plans, &commonEndUs, &planDetail, cancel);
    input.reset();
    if (planned == PlanResult::kCanceled) {
        report.detail = planDetail;
        DLL_Log("[RecordingRecovery] staging=%s outcome=failed reason=%s", Leaf(staging).c_str(), planDetail.c_str());
        return report;
    }
    if (planned == PlanResult::kNoMedia) {
        DeleteFileW(staging.c_str());
        report.outcome = RecoveryOutcome::kDiscardedEmpty;
        report.detail = planDetail;
        DLL_Log("[RecordingRecovery] staging=%s bytes=%lld outcome=%s reason=%s", Leaf(staging).c_str(),
                static_cast<long long>(size.QuadPart), RecoveryOutcomeName(report.outcome), planDetail.c_str());
        return report;
    }
    report.endUs = commonEndUs;
    for (const StreamPlan& plan : plans) {
        if (plan.packets > 0)
            report.longestTailUs = std::max(report.longestTailUs, plan.endUs - commonEndUs);
    }

    ce::capture_output::ReservedCaptureOutput output =
        ce::capture_output::ReservedCaptureOutput::Reserve(directory, L"capture_recovered", extension);
    if (!output || !output.ReleaseToWriter()) {
        report.detail = "cannot reserve the recovered file";
        DLL_Log("[RecordingRecovery] staging=%s outcome=failed reason=%s", Leaf(staging).c_str(), report.detail.c_str());
        return report;
    }
    std::string remuxDetail;
    if (!Remux(staging, output.Utf8Path(), plans, commonEndUs, &remuxDetail, cancel)) {
        output.CleanupOwnedFile();
        report.detail = remuxDetail;
        DLL_Log("[RecordingRecovery] staging=%s outcome=failed reason=%s", Leaf(staging).c_str(), remuxDetail.c_str());
        return report;
    }
    output.Publish();
    report.output = output.Path();
    report.outcome = RecoveryOutcome::kRecovered;
    if (!DeleteFileW(staging.c_str()))
        DLL_Log("[RecordingRecovery] WARN: repaired, but the staging file could not be removed (error=%lu)",
                GetLastError());
    DLL_Log("[RecordingRecovery] staging=%s bytes=%lld outcome=recovered output=%s streams=%u end_ms=%lld "
            "longest_tail_ms=%lld",
            Leaf(staging).c_str(), static_cast<long long>(size.QuadPart), Leaf(report.output).c_str(), report.streams,
            static_cast<long long>(commonEndUs / 1000), static_cast<long long>(report.longestTailUs / 1000));
    return report;
}

}  // namespace ce::media
