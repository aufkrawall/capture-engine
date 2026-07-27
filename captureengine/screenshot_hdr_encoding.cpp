// Parallel HDR screenshot conversion and AVIF encoding: row-parallel PQ and
// HDR-to-SDR conversion plus the libavcodec AVIF writer. The maths these call
// through and the public entry points live in screenshot_encoding.cpp.

#include "screenshot_encoding_internal.h"

#include "../common/logging.h"

// clang-format off
#include <windows.h>
// clang-format on

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace ce::screenshot {

namespace {

template <typename RowFunction>
bool RunParallelRows(uint32_t height, uint32_t workerCount, const char* operation, RowFunction& processRow) {
    std::atomic<bool> success{true};
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    try {
        for (uint32_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
            workers.emplace_back([&, workerIndex]() {
                const uint32_t firstRow = static_cast<uint32_t>((static_cast<uint64_t>(height) * workerIndex) /
                                                                workerCount);
                const uint32_t endRow = static_cast<uint32_t>((static_cast<uint64_t>(height) * (workerIndex + 1)) /
                                                              workerCount);
                for (uint32_t row = firstRow; row < endRow && success.load(std::memory_order_relaxed); ++row) {
                    if (!processRow(row)) {
                        success.store(false, std::memory_order_relaxed);
                        return;
                    }
                }
            });
        }
    } catch (const std::system_error& error) {
        success.store(false, std::memory_order_relaxed);
        LogError("[Screenshot] %s worker creation failed: %s", operation, error.what());
    }
    for (std::thread& worker : workers) {
        if (worker.joinable())
            worker.join();
    }
    return success.load(std::memory_order_relaxed);
}

bool FillHdrYuvFrame(const RawScreenshot& screenshot, AVFrame* frame, uint32_t workerCount) {
    if (!frame)
        return false;
    const auto format = static_cast<ScreenshotPixelFormat>(screenshot.header.pixelFormat);
    if (format != ScreenshotPixelFormat::R10G10B10A2 && format != ScreenshotPixelFormat::RGBA16F)
        return false;

    const size_t pixelSize = BytesPerPixel(format);
    auto convertRow = [&](uint32_t row) {
        const uint8_t* source = screenshot.pixels.data() + static_cast<size_t>(row) * screenshot.header.rowPitch;
        auto* yPlane = reinterpret_cast<uint16_t*>(frame->data[0] + static_cast<size_t>(row) * frame->linesize[0]);
        auto* uPlane = reinterpret_cast<uint16_t*>(frame->data[1] + static_cast<size_t>(row) * frame->linesize[1]);
        auto* vPlane = reinterpret_cast<uint16_t*>(frame->data[2] + static_cast<size_t>(row) * frame->linesize[2]);
        for (uint32_t column = 0; column < screenshot.header.width; ++column) {
            Yuv10Pixel converted{};
            if (!ConvertHdrPixelToYuv10(format, source + static_cast<size_t>(column) * pixelSize, converted))
                return false;
            yPlane[column] = converted.y;
            uPlane[column] = converted.u;
            vPlane[column] = converted.v;
        }
        return true;
    };
    const auto started = std::chrono::steady_clock::now();
    if (!RunParallelRows(screenshot.header.height, workerCount, "HDR-to-PQ conversion", convertRow))
        return false;
    const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                                 started)
                               .count();
    LogInfo("[Screenshot] HDR-to-PQ conversion: %ux%u format=%u workers=%u duration=%.3fms",
            screenshot.header.width, screenshot.header.height, static_cast<unsigned>(format), workerCount,
            elapsedUs / 1000.0);
    return true;
}

bool EncoderSupportsYuv444p10(const AVCodec* codec) {
    if (!codec)
        return false;
    const void* configurations = nullptr;
    int configurationCount = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configurations,
                                     &configurationCount) < 0) {
        return false;
    }
    if (!configurations)
        return true;
    const auto* formats = static_cast<const AVPixelFormat*>(configurations);
    for (int index = 0; index < configurationCount; ++index) {
        if (formats[index] == AV_PIX_FMT_YUV444P10LE)
            return true;
    }
    return false;
}

}  // namespace

bool ConvertHdrToSdrScreenshot(const RawScreenshot& screenshot, float sdrWhiteNits, RawScreenshot& converted) {
    const auto format = static_cast<ScreenshotPixelFormat>(screenshot.header.pixelFormat);
    const size_t pixelSize = BytesPerPixel(format);
    if ((format != ScreenshotPixelFormat::R10G10B10A2 && format != ScreenshotPixelFormat::RGBA16F) ||
        pixelSize == 0) {
        return false;
    }

    const uint32_t outputRowPitch = screenshot.header.width * 4;
    std::vector<uint8_t> pixels(static_cast<size_t>(outputRowPitch) * screenshot.header.height);
    const unsigned hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
    const unsigned workerCount =
        std::min<unsigned>({16u, hardwareThreads, std::max(1u, screenshot.header.height)});
    auto convertRow = [&](uint32_t row) {
        const uint8_t* source = screenshot.pixels.data() + static_cast<size_t>(row) * screenshot.header.rowPitch;
        auto* destination =
            reinterpret_cast<Bgra8Pixel*>(pixels.data() + static_cast<size_t>(row) * outputRowPitch);
        for (uint32_t column = 0; column < screenshot.header.width; ++column) {
            if (!ConvertHdrPixelToSdrBgra(format, source + static_cast<size_t>(column) * pixelSize, sdrWhiteNits,
                                          destination[column])) {
                return false;
            }
        }
        return true;
    };
    const auto started = std::chrono::steady_clock::now();
    if (!RunParallelRows(screenshot.header.height, workerCount, "HDR-to-SDR conversion", convertRow))
        return false;

    const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                                 started)
                               .count();
    LogInfo("[Screenshot] HDR->SDR conversion: %ux%u white=%.1f-nit workers=%u duration=%.3fms",
            screenshot.header.width, screenshot.header.height, sdrWhiteNits, workerCount, elapsedUs / 1000.0);
    return MakeRawScreenshot(pixels.data(), screenshot.header.width, screenshot.header.height, outputRowPitch,
                             ScreenshotPixelFormat::BGRA8, ScreenshotColorEncoding::SRGB, converted);
}

bool SaveHdrAvif(const std::filesystem::path& outputDirectory, const RawScreenshot& screenshot,
                 std::filesystem::path& publishedPath) {
    const auto overallStarted = std::chrono::steady_clock::now();
    const AVCodec* codec = avcodec_find_encoder_by_name("libaom-av1");
    if (!EncoderSupportsYuv444p10(codec)) {
        LogError("[Screenshot] Source-built libaom-av1 10-bit 4:4:4 encoder is unavailable");
        return false;
    }

    ReservedCaptureOutput staging = ReserveScreenshotStaging(outputDirectory);
    if (!staging || !staging.ReleaseToWriter()) {
        LogError("[Screenshot] AVIF staging reservation failed: win32=%lu", static_cast<unsigned long>(GetLastError()));
        return false;
    }
    const std::string stagingPath = WideToUtf8(staging.Path().wstring());
    if (stagingPath.empty())
        return false;

    AVFormatContext* formatContext = nullptr;
    AVCodecContext* codecContext = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    AVDictionary* options = nullptr;
    bool success = false;
    const char* failureStage = "initialize";
    int failureCode = 0;
    int64_t encodeElapsedUs = 0;
    const AvifEncodingPlan encodingPlan = SelectAvifEncodingPlan(
        screenshot.header.width, screenshot.header.height, std::max(1u, std::thread::hardware_concurrency()));

    do {
        failureStage = "allocate AVIF muxer";
        failureCode = avformat_alloc_output_context2(&formatContext, nullptr, "avif", stagingPath.c_str());
        if (failureCode < 0 || !formatContext)
            break;
        failureStage = "allocate libaom context";
        codecContext = avcodec_alloc_context3(codec);
        if (!codecContext)
            break;

        codecContext->width = static_cast<int>(screenshot.header.width);
        codecContext->height = static_cast<int>(screenshot.header.height);
        codecContext->time_base = AVRational{1, 1};
        codecContext->framerate = AVRational{1, 1};
        codecContext->pix_fmt = AV_PIX_FMT_YUV444P10LE;
        codecContext->thread_count = static_cast<int>(encodingPlan.threadCount);
        codecContext->gop_size = 1;
        codecContext->max_b_frames = 0;
        codecContext->color_primaries = AVCOL_PRI_BT2020;
        codecContext->color_trc = AVCOL_TRC_SMPTE2084;
        codecContext->colorspace = AVCOL_SPC_BT2020_NCL;
        codecContext->color_range = AVCOL_RANGE_JPEG;
        if (formatContext->oformat->flags & AVFMT_GLOBALHEADER)
            codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        failureStage = "set libaom still-picture option";
        failureCode = av_dict_set(&options, "still-picture", "1", 0);
        if (failureCode < 0)
            break;
        failureStage = "set libaom realtime usage option";
        failureCode = av_dict_set(&options, "usage", "realtime", 0);
        if (failureCode < 0)
            break;
        failureStage = "set libaom cpu-used option";
        failureCode = av_dict_set(&options, "cpu-used", "8", 0);
        if (failureCode < 0)
            break;
        failureStage = "set libaom CRF option";
        failureCode = av_dict_set(&options, "crf", "8", 0);
        if (failureCode < 0)
            break;
        failureStage = "set libaom row-mt option";
        failureCode = av_dict_set(&options, "row-mt", "1", 0);
        if (failureCode < 0)
            break;
        if (encodingPlan.tileColumnsLog2 != 0) {
            failureStage = "set libaom tile-columns option";
            failureCode = av_dict_set_int(&options, "tile-columns", encodingPlan.tileColumnsLog2, 0);
            if (failureCode < 0)
                break;
        }
        if (encodingPlan.tileRowsLog2 != 0) {
            failureStage = "set libaom tile-rows option";
            failureCode = av_dict_set_int(&options, "tile-rows", encodingPlan.tileRowsLog2, 0);
            if (failureCode < 0)
                break;
        }
        failureStage = "open libaom encoder";
        failureCode = avcodec_open2(codecContext, codec, &options);
        if (failureCode < 0)
            break;
        if (av_dict_count(options) != 0) {
            failureStage = "consume all libaom options";
            failureCode = AVERROR_OPTION_NOT_FOUND;
            break;
        }

        failureStage = "allocate AVIF stream";
        AVStream* stream = avformat_new_stream(formatContext, nullptr);
        if (!stream)
            break;
        failureStage = "copy libaom stream parameters";
        failureCode = avcodec_parameters_from_context(stream->codecpar, codecContext);
        if (failureCode < 0)
            break;
        stream->time_base = codecContext->time_base;
        stream->avg_frame_rate = codecContext->framerate;

        failureStage = "allocate AVIF frame and packet";
        frame = av_frame_alloc();
        packet = av_packet_alloc();
        if (!frame || !packet)
            break;
        frame->format = codecContext->pix_fmt;
        frame->width = codecContext->width;
        frame->height = codecContext->height;
        frame->pts = 0;
        frame->color_primaries = codecContext->color_primaries;
        frame->color_trc = codecContext->color_trc;
        frame->colorspace = codecContext->colorspace;
        frame->color_range = codecContext->color_range;
        failureStage = "allocate AVIF frame planes";
        failureCode = av_frame_get_buffer(frame, 32);
        if (failureCode < 0)
            break;
        failureStage = "make AVIF frame writable";
        failureCode = av_frame_make_writable(frame);
        if (failureCode < 0)
            break;
        failureStage = "convert HDR pixels";
        failureCode = 0;
        if (!FillHdrYuvFrame(screenshot, frame, encodingPlan.threadCount))
            break;
        failureStage = "open AVIF staging file";
        failureCode = avio_open(&formatContext->pb, stagingPath.c_str(), AVIO_FLAG_WRITE);
        if (failureCode < 0)
            break;
        failureStage = "write AVIF header";
        failureCode = avformat_write_header(formatContext, nullptr);
        if (failureCode < 0)
            break;
        const auto encodeStarted = std::chrono::steady_clock::now();
        failureStage = "submit AVIF frame";
        failureCode = avcodec_send_frame(codecContext, frame);
        if (failureCode < 0) {
            break;
        }

        auto drainPackets = [&](bool flushing) {
            for (;;) {
                const int receiveResult = avcodec_receive_packet(codecContext, packet);
                if (receiveResult == AVERROR(EAGAIN)) {
                    if (!flushing)
                        return true;
                    failureStage = "drain flushed libaom packets";
                    failureCode = receiveResult;
                    return false;
                }
                if (receiveResult == AVERROR_EOF) {
                    if (flushing)
                        return true;
                    failureStage = "drain libaom packet";
                    failureCode = receiveResult;
                    return false;
                }
                if (receiveResult < 0) {
                    failureStage = flushing ? "drain flushed libaom packets" : "drain libaom packet";
                    failureCode = receiveResult;
                    return false;
                }
                packet->stream_index = stream->index;
                if (packet->duration <= 0)
                    packet->duration = 1;
                av_packet_rescale_ts(packet, codecContext->time_base, stream->time_base);
                const int writeResult = av_interleaved_write_frame(formatContext, packet);
                av_packet_unref(packet);
                if (writeResult < 0) {
                    failureStage = "write AVIF packet";
                    failureCode = writeResult;
                    return false;
                }
            }
        };

        if (!drainPackets(false))
            break;
        failureStage = "flush libaom encoder";
        failureCode = avcodec_send_frame(codecContext, nullptr);
        if (failureCode < 0 || !drainPackets(true))
            break;
        failureStage = "write AVIF trailer";
        failureCode = av_write_trailer(formatContext);
        if (failureCode < 0)
            break;
        avio_flush(formatContext->pb);
        if (formatContext->pb->error < 0) {
            failureStage = "flush AVIF staging file";
            failureCode = formatContext->pb->error;
            break;
        }
        failureStage = "close AVIF staging file";
        failureCode = avio_closep(&formatContext->pb);
        if (failureCode < 0)
            break;
        encodeElapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - encodeStarted)
                              .count();
        success = true;
    } while (false);

    av_dict_free(&options);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codecContext);
    if (formatContext) {
        if (formatContext->pb && avio_closep(&formatContext->pb) < 0)
            success = false;
        avformat_free_context(formatContext);
    }

    if (!success) {
        char errorText[AV_ERROR_MAX_STRING_SIZE] = "no FFmpeg error code";
        if (failureCode < 0)
            av_strerror(failureCode, errorText, sizeof(errorText));
        LogError("[Screenshot] AVIF encode failed at '%s': %d (%s)", failureStage, failureCode, errorText);
        return false;
    }
    if (!FlushPath(staging.Path())) {
        LogError("[Screenshot] AVIF staging flush failed: win32=%lu", static_cast<unsigned long>(GetLastError()));
        return false;
    }
    if (!staging.PublishToNewPath(outputDirectory, L"screenshot", L".avif")) {
        LogError("[Screenshot] AVIF atomic publication failed: win32=%lu", static_cast<unsigned long>(GetLastError()));
        return false;
    }
    publishedPath = staging.Path();
    const auto totalElapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - overallStarted)
                                    .count();
    LogInfo("[Screenshot] AVIF pipeline: %ux%u mode=realtime cpu=8 crf=8 threads=%u tiles=%ux%u "
            "encode=%.3fms total=%.3fms",
            screenshot.header.width, screenshot.header.height, encodingPlan.threadCount,
            1u << encodingPlan.tileColumnsLog2, 1u << encodingPlan.tileRowsLog2, encodeElapsedUs / 1000.0,
            totalElapsedUs / 1000.0);
    return true;
}

}  // namespace ce::screenshot
