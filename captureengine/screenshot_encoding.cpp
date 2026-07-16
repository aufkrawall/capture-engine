#include "screenshot_encoding.h"

#include "../common/logging.h"
#include "../common/reserved_capture_output.h"

// clang-format off
#include <windows.h>
#include <wincodec.h>
#include <wincodecsdk.h>
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
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace ce::screenshot {
namespace {

using ce::capture_output::ReservedCaptureOutput;

template <typename T>
void SafeRelease(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

class HandleGuard {
public:
    explicit HandleGuard(HANDLE handle) : handle_(handle) {}
    ~HandleGuard() {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
    }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    HANDLE Get() const {
        return handle_;
    }

private:
    HANDLE handle_ = nullptr;
};

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty())
        return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(),
                            length, nullptr, nullptr) != length) {
        return {};
    }
    return result;
}

bool ReadExact(HANDLE file, void* destination, uint64_t size) {
    auto* cursor = static_cast<uint8_t*>(destination);
    while (size != 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(size, 16ULL * 1024ULL * 1024ULL));
        DWORD bytesRead = 0;
        if (!ReadFile(file, cursor, chunk, &bytesRead, nullptr) || bytesRead != chunk)
            return false;
        cursor += bytesRead;
        size -= bytesRead;
    }
    return true;
}

bool FlushPath(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const bool flushed = FlushFileBuffers(file) != FALSE;
    const bool closed = CloseHandle(file) != FALSE;
    return flushed && closed;
}

ReservedCaptureOutput ReserveStagingOutput(const ReservedCaptureOutput& finalOutput) {
    return ReservedCaptureOutput::Reserve(finalOutput.Path().parent_path(),
                                          finalOutput.Path().stem().wstring() + L"_stage", L".part");
}

bool ConvertSdrToTightBgra(const RawScreenshot& screenshot, std::vector<uint8_t>& bgra) {
    const auto format = static_cast<ScreenshotPixelFormat>(screenshot.header.pixelFormat);
    if ((format != ScreenshotPixelFormat::BGRA8 && format != ScreenshotPixelFormat::RGBA8) ||
        screenshot.header.colorEncoding != static_cast<uint32_t>(ScreenshotColorEncoding::SRGB)) {
        return false;
    }
    const uint64_t tightRowPitch = static_cast<uint64_t>(screenshot.header.width) * 4;
    const uint64_t totalSize = tightRowPitch * screenshot.header.height;
    if (totalSize > std::numeric_limits<UINT>::max() || totalSize > std::numeric_limits<size_t>::max())
        return false;

    bgra.resize(static_cast<size_t>(totalSize));
    for (uint32_t y = 0; y < screenshot.header.height; ++y) {
        const uint8_t* source = screenshot.pixels.data() + static_cast<size_t>(y) * screenshot.header.rowPitch;
        uint8_t* destination = bgra.data() + static_cast<size_t>(y) * tightRowPitch;
        if (format == ScreenshotPixelFormat::BGRA8) {
            memcpy(destination, source, static_cast<size_t>(tightRowPitch));
        } else {
            for (uint32_t x = 0; x < screenshot.header.width; ++x) {
                destination[x * 4 + 0] = source[x * 4 + 2];
                destination[x * 4 + 1] = source[x * 4 + 1];
                destination[x * 4 + 2] = source[x * 4 + 0];
                destination[x * 4 + 3] = source[x * 4 + 3];
            }
        }
    }
    return true;
}

bool SavePixelsAsPng(ReservedCaptureOutput& finalOutput, const RawScreenshot& screenshot) {
    std::vector<uint8_t> bgra;
    if (!finalOutput || !ConvertSdrToTightBgra(screenshot, bgra))
        return false;

    ReservedCaptureOutput staging = ReserveStagingOutput(finalOutput);
    if (!staging || !staging.ReleaseToWriter())
        return false;

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* options = nullptr;
    bool encoded = false;

    do {
        const HRESULT result =
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(result) || FAILED(factory->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromFilename(staging.Path().c_str(), GENERIC_WRITE)) ||
            FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
            FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) ||
            FAILED(encoder->CreateNewFrame(&frame, &options)) || FAILED(frame->Initialize(options)) ||
            FAILED(frame->SetSize(screenshot.header.width, screenshot.header.height))) {
            break;
        }

        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&pixelFormat)) || pixelFormat != GUID_WICPixelFormat32bppBGRA)
            break;
        const UINT rowPitch = screenshot.header.width * 4;
        if (FAILED(
                frame->WritePixels(screenshot.header.height, rowPitch, static_cast<UINT>(bgra.size()), bgra.data())) ||
            FAILED(frame->Commit()) || FAILED(encoder->Commit()) || FAILED(stream->Commit(STGC_DEFAULT))) {
            break;
        }
        encoded = true;
    } while (false);

    SafeRelease(options);
    SafeRelease(frame);
    SafeRelease(encoder);
    SafeRelease(stream);
    SafeRelease(factory);

    if (!encoded || !FlushPath(staging.Path()) || !finalOutput.CommitStagingFile(staging)) {
        LogError("[Screenshot] PNG encoding or atomic publication failed");
        return false;
    }
    return true;
}

float HalfToFloat(uint16_t half) {
    const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16;
    uint32_t exponent = (half >> 10) & 0x1Fu;
    uint32_t mantissa = half & 0x3FFu;
    uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            int shift = 0;
            while ((mantissa & 0x400u) == 0) {
                mantissa <<= 1;
                ++shift;
            }
            mantissa &= 0x3FFu;
            bits = sign | (static_cast<uint32_t>(127 - 14 - shift) << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1Fu) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
    }
    float result = 0.0f;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

double EncodeSt2084(double nits) {
    constexpr double m1 = 2610.0 / 16384.0;
    constexpr double m2 = 2523.0 / 32.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = 2413.0 / 128.0;
    constexpr double c3 = 2392.0 / 128.0;
    const double normalized = std::clamp(nits, 0.0, 10000.0) / 10000.0;
    const double powered = std::pow(normalized, m1);
    return std::pow((c1 + c2 * powered) / (1.0 + c3 * powered), m2);
}

uint16_t Quantize10(double value) {
    return static_cast<uint16_t>(std::lround(std::clamp(value, 0.0, 1.0) * 1023.0));
}

void RgbPqToYuv(double red, double green, double blue, Yuv10Pixel& converted) {
    constexpr double kr = 0.2627;
    constexpr double kb = 0.0593;
    constexpr double kg = 1.0 - kr - kb;
    const double luma = kr * red + kg * green + kb * blue;
    const double cb = (blue - luma) / (2.0 * (1.0 - kb)) + 0.5;
    const double cr = (red - luma) / (2.0 * (1.0 - kr)) + 0.5;
    converted.y = Quantize10(luma);
    converted.u = Quantize10(cb);
    converted.v = Quantize10(cr);
}

bool FillHdrYuvFrame(const RawScreenshot& screenshot, AVFrame* frame) {
    if (!frame)
        return false;
    const auto format = static_cast<ScreenshotPixelFormat>(screenshot.header.pixelFormat);
    if (format != ScreenshotPixelFormat::R10G10B10A2 && format != ScreenshotPixelFormat::RGBA16F)
        return false;

    for (uint32_t row = 0; row < screenshot.header.height; ++row) {
        const uint8_t* source = screenshot.pixels.data() + static_cast<size_t>(row) * screenshot.header.rowPitch;
        auto* yPlane = reinterpret_cast<uint16_t*>(frame->data[0] + static_cast<size_t>(row) * frame->linesize[0]);
        auto* uPlane = reinterpret_cast<uint16_t*>(frame->data[1] + static_cast<size_t>(row) * frame->linesize[1]);
        auto* vPlane = reinterpret_cast<uint16_t*>(frame->data[2] + static_cast<size_t>(row) * frame->linesize[2]);
        const size_t pixelSize = BytesPerPixel(format);
        for (uint32_t column = 0; column < screenshot.header.width; ++column) {
            Yuv10Pixel converted{};
            if (!ConvertHdrPixelToYuv10(format, source + static_cast<size_t>(column) * pixelSize, converted))
                return false;
            yPlane[column] = converted.y;
            uPlane[column] = converted.u;
            vPlane[column] = converted.v;
        }
    }
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

bool SaveHdrAvif(ReservedCaptureOutput& finalOutput, const RawScreenshot& screenshot) {
    if (!finalOutput)
        return false;
    const AVCodec* codec = avcodec_find_encoder_by_name("libaom-av1");
    if (!EncoderSupportsYuv444p10(codec)) {
        LogError("[Screenshot] Source-built libaom-av1 10-bit 4:4:4 encoder is unavailable");
        return false;
    }

    ReservedCaptureOutput staging = ReserveStagingOutput(finalOutput);
    if (!staging || !staging.ReleaseToWriter())
        return false;
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
        failureStage = "set libaom cpu-used option";
        failureCode = av_dict_set(&options, "cpu-used", "6", 0);
        if (failureCode < 0)
            break;
        failureStage = "set libaom CRF option";
        failureCode = av_dict_set(&options, "crf", "12", 0);
        if (failureCode < 0)
            break;
        failureStage = "set libaom row-mt option";
        failureCode = av_dict_set(&options, "row-mt", "1", 0);
        if (failureCode < 0)
            break;
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
        if (!FillHdrYuvFrame(screenshot, frame))
            break;
        failureStage = "open AVIF staging file";
        failureCode = avio_open(&formatContext->pb, stagingPath.c_str(), AVIO_FLAG_WRITE);
        if (failureCode < 0)
            break;
        failureStage = "write AVIF header";
        failureCode = avformat_write_header(formatContext, nullptr);
        if (failureCode < 0)
            break;
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
    if (!finalOutput.CommitStagingFile(staging)) {
        LogError("[Screenshot] AVIF atomic publication failed: win32=%lu", static_cast<unsigned long>(GetLastError()));
        return false;
    }
    return true;
}

}  // namespace

uint32_t BytesPerPixel(ScreenshotPixelFormat format) {
    switch (format) {
        case ScreenshotPixelFormat::BGRA8:
        case ScreenshotPixelFormat::RGBA8:
        case ScreenshotPixelFormat::R10G10B10A2:
            return 4;
        case ScreenshotPixelFormat::RGBA16F:
            return 8;
        default:
            return 0;
    }
}

bool IsValidFormatEncoding(ScreenshotPixelFormat format, ScreenshotColorEncoding encoding) {
    switch (format) {
        case ScreenshotPixelFormat::BGRA8:
        case ScreenshotPixelFormat::RGBA8:
            return encoding == ScreenshotColorEncoding::SRGB;
        case ScreenshotPixelFormat::R10G10B10A2:
            return encoding == ScreenshotColorEncoding::BT2020_PQ;
        case ScreenshotPixelFormat::RGBA16F:
            return encoding == ScreenshotColorEncoding::LinearScRGB;
        default:
            return false;
    }
}

bool ValidateRawHeader(const ScreenshotRawHeaderV2& header, uint64_t fileSize, uint64_t expectedRequestId) {
    if (header.magic != ScreenshotRawHeaderV2::kMagic || header.version != ScreenshotRawHeaderV2::kVersion ||
        header.headerSize != sizeof(ScreenshotRawHeaderV2) || header.width == 0 || header.height == 0 ||
        header.width > kMaximumScreenshotDimension || header.height > kMaximumScreenshotDimension ||
        header.reserved32 != 0 || header.reserved64 != 0 ||
        (expectedRequestId != 0 && header.requestId != expectedRequestId)) {
        return false;
    }

    const auto format = static_cast<ScreenshotPixelFormat>(header.pixelFormat);
    const auto encoding = static_cast<ScreenshotColorEncoding>(header.colorEncoding);
    const uint32_t bytesPerPixel = BytesPerPixel(format);
    if (bytesPerPixel == 0 || !IsValidFormatEncoding(format, encoding))
        return false;

    const uint64_t minimumRowPitch = static_cast<uint64_t>(header.width) * bytesPerPixel;
    if (header.rowPitch < minimumRowPitch || header.rowPitch % bytesPerPixel != 0 ||
        static_cast<uint64_t>(header.rowPitch) - minimumRowPitch > 65535ULL ||
        header.rowPitch > std::numeric_limits<uint64_t>::max() / header.height) {
        return false;
    }
    const uint64_t expectedPayload = static_cast<uint64_t>(header.rowPitch) * header.height;
    return header.payloadSize == expectedPayload &&
           header.payloadSize <= std::numeric_limits<uint64_t>::max() - sizeof(ScreenshotRawHeaderV2) &&
           header.totalSize == sizeof(ScreenshotRawHeaderV2) + header.payloadSize && header.totalSize == fileSize &&
           header.payloadSize <= static_cast<uint64_t>(std::numeric_limits<size_t>::max());
}

bool ReadRawScreenshot(const std::filesystem::path& path, uint64_t expectedRequestId, RawScreenshot& screenshot) {
    screenshot = {};
    HandleGuard file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file.Get() == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.Get(), &size) || size.QuadPart < 0 ||
        !ReadExact(file.Get(), &screenshot.header, sizeof(screenshot.header)) ||
        !ValidateRawHeader(screenshot.header, static_cast<uint64_t>(size.QuadPart), expectedRequestId)) {
        screenshot = {};
        return false;
    }

    screenshot.pixels.resize(static_cast<size_t>(screenshot.header.payloadSize));
    if (!ReadExact(file.Get(), screenshot.pixels.data(), screenshot.header.payloadSize)) {
        screenshot = {};
        return false;
    }
    return true;
}

bool MakeRawScreenshot(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t rowPitch,
                       ScreenshotPixelFormat format, ScreenshotColorEncoding encoding, RawScreenshot& screenshot) {
    const uint32_t bytesPerPixel = BytesPerPixel(format);
    if (!pixels || bytesPerPixel == 0 || !IsValidFormatEncoding(format, encoding) || width == 0 || height == 0 ||
        width > kMaximumScreenshotDimension || height > kMaximumScreenshotDimension ||
        rowPitch < static_cast<uint64_t>(width) * bytesPerPixel || rowPitch % bytesPerPixel != 0 ||
        static_cast<uint64_t>(rowPitch) - static_cast<uint64_t>(width) * bytesPerPixel > 65535ULL ||
        rowPitch > std::numeric_limits<uint64_t>::max() / height) {
        return false;
    }
    const uint64_t payloadSize = static_cast<uint64_t>(rowPitch) * height;
    if (payloadSize > std::numeric_limits<size_t>::max() ||
        payloadSize > std::numeric_limits<uint64_t>::max() - sizeof(ScreenshotRawHeaderV2)) {
        return false;
    }
    screenshot = {};
    screenshot.header.pixelFormat = static_cast<uint32_t>(format);
    screenshot.header.colorEncoding = static_cast<uint32_t>(encoding);
    screenshot.header.width = width;
    screenshot.header.height = height;
    screenshot.header.rowPitch = rowPitch;
    screenshot.header.payloadSize = payloadSize;
    screenshot.header.totalSize = sizeof(ScreenshotRawHeaderV2) + payloadSize;
    screenshot.pixels.assign(pixels, pixels + static_cast<size_t>(payloadSize));
    return true;
}

bool ConvertHdrPixelToYuv10(ScreenshotPixelFormat format, const uint8_t* pixel, Yuv10Pixel& converted) {
    if (!pixel)
        return false;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    if (format == ScreenshotPixelFormat::R10G10B10A2) {
        uint32_t packed = 0;
        memcpy(&packed, pixel, sizeof(packed));
        red = static_cast<double>(packed & 0x3FFu) / 1023.0;
        green = static_cast<double>((packed >> 10) & 0x3FFu) / 1023.0;
        blue = static_cast<double>((packed >> 20) & 0x3FFu) / 1023.0;
    } else if (format == ScreenshotPixelFormat::RGBA16F) {
        uint16_t components[4]{};
        memcpy(components, pixel, sizeof(components));
        double red709 = HalfToFloat(components[0]);
        double green709 = HalfToFloat(components[1]);
        double blue709 = HalfToFloat(components[2]);
        if (!std::isfinite(red709))
            red709 = 0.0;
        if (!std::isfinite(green709))
            green709 = 0.0;
        if (!std::isfinite(blue709))
            blue709 = 0.0;
        constexpr double redFromRed = 0.6274038959;
        constexpr double redFromGreen = 0.3292830384;
        constexpr double greenFromRed = 0.0690972894;
        constexpr double greenFromGreen = 0.9195403951;
        constexpr double blueFromRed = 0.0163914389;
        constexpr double blueFromGreen = 0.0880133079;
        const double red2020 =
            redFromRed * red709 + redFromGreen * green709 + (1.0 - redFromRed - redFromGreen) * blue709;
        const double green2020 =
            greenFromRed * red709 + greenFromGreen * green709 + (1.0 - greenFromRed - greenFromGreen) * blue709;
        const double blue2020 =
            blueFromRed * red709 + blueFromGreen * green709 + (1.0 - blueFromRed - blueFromGreen) * blue709;
        red = EncodeSt2084(red2020 * 80.0);
        green = EncodeSt2084(green2020 * 80.0);
        blue = EncodeSt2084(blue2020 * 80.0);
    } else {
        return false;
    }
    RgbPqToYuv(red, green, blue, converted);
    return true;
}

bool SaveRawScreenshot(const std::filesystem::path& outputDirectory, const RawScreenshot& screenshot,
                       std::filesystem::path& publishedPath) {
    publishedPath.clear();
    if (screenshot.pixels.size() != screenshot.header.payloadSize ||
        !ValidateRawHeader(screenshot.header, screenshot.header.totalSize, screenshot.header.requestId)) {
        return false;
    }
    const auto format = static_cast<ScreenshotPixelFormat>(screenshot.header.pixelFormat);
    if (format == ScreenshotPixelFormat::BGRA8 || format == ScreenshotPixelFormat::RGBA8) {
        ReservedCaptureOutput output = ReservedCaptureOutput::Reserve(outputDirectory, L"screenshot", L".png");
        if (output && SavePixelsAsPng(output, screenshot)) {
            publishedPath = output.Path();
            return true;
        }
        return false;
    }
    if (format != ScreenshotPixelFormat::R10G10B10A2 && format != ScreenshotPixelFormat::RGBA16F)
        return false;
    ReservedCaptureOutput output = ReservedCaptureOutput::Reserve(outputDirectory, L"screenshot", L".avif");
    if (output && SaveHdrAvif(output, screenshot)) {
        publishedPath = output.Path();
        return true;
    }
    return false;
}

}  // namespace ce::screenshot
