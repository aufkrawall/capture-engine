#include <gtest/gtest.h>

#include "../captureengine/screenshot_encoding.h"
#include "../common/logging.h"

#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
}

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace {

ScreenshotRawHeaderV2 ValidHeader() {
    ScreenshotRawHeaderV2 header;
    header.pixelFormat = static_cast<uint32_t>(ScreenshotPixelFormat::BGRA8);
    header.colorEncoding = static_cast<uint32_t>(ScreenshotColorEncoding::SRGB);
    header.width = 2;
    header.height = 2;
    header.rowPitch = 8;
    header.payloadSize = 16;
    header.totalSize = sizeof(header) + header.payloadSize;
    header.requestId = 42;
    return header;
}

std::filesystem::path UniqueRawPath() {
    static std::atomic<uint32_t> sequence{1};
    return std::filesystem::temp_directory_path() /
           (L"ce_screenshot_raw_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
            std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)) + L".ready");
}

class TemporaryRawFile {
  public:
    TemporaryRawFile() : path_(UniqueRawPath()) {}
    ~TemporaryRawFile() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }
    const std::filesystem::path& Path() const {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty())
        return {};
    const int required =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0,
                            nullptr, nullptr);
    if (required <= 0)
        return {};
    std::string result(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                            result.data(), required, nullptr, nullptr) != required) {
        return {};
    }
    return result;
}

struct DecodedAvifInfo {
    bool decoded = false;
    int width = 0;
    int height = 0;
    AVPixelFormat format = AV_PIX_FMT_NONE;
    AVColorPrimaries colorPrimaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic colorTransfer = AVCOL_TRC_UNSPECIFIED;
    AVColorSpace colorSpace = AVCOL_SPC_UNSPECIFIED;
    AVColorRange colorRange = AVCOL_RANGE_UNSPECIFIED;
    std::array<ce::screenshot::Yuv10Pixel, 4> firstRow{};
};

DecodedAvifInfo DecodeAvif(const std::filesystem::path& path) {
    DecodedAvifInfo info;
    AVFormatContext* formatContext = nullptr;
    AVCodecContext* codecContext = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    const std::string utf8Path = WideToUtf8(path.wstring());

    do {
        if (utf8Path.empty() || avformat_open_input(&formatContext, utf8Path.c_str(), nullptr, nullptr) < 0 ||
            avformat_find_stream_info(formatContext, nullptr) < 0) {
            break;
        }
        const int streamIndex = av_find_best_stream(formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (streamIndex < 0)
            break;
        AVStream* stream = formatContext->streams[streamIndex];
        const AVCodec* decoder = avcodec_find_decoder_by_name("libaom-av1");
        codecContext = avcodec_alloc_context3(decoder);
        if (!decoder || !codecContext || avcodec_parameters_to_context(codecContext, stream->codecpar) < 0 ||
            avcodec_open2(codecContext, decoder, nullptr) < 0) {
            break;
        }
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (!packet || !frame)
            break;

        auto receiveFrame = [&]() {
            for (;;) {
                const int receiveResult = avcodec_receive_frame(codecContext, frame);
                if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF)
                    return false;
                if (receiveResult < 0)
                    return false;
                info.decoded = true;
                info.width = frame->width;
                info.height = frame->height;
                info.format = static_cast<AVPixelFormat>(frame->format);
                info.colorPrimaries = frame->color_primaries;
                info.colorTransfer = frame->color_trc;
                info.colorSpace = frame->colorspace;
                info.colorRange = frame->color_range;
                if (frame->format == AV_PIX_FMT_YUV444P10LE && frame->width >= 4) {
                    const auto* yPlane = reinterpret_cast<const uint16_t*>(frame->data[0]);
                    const auto* uPlane = reinterpret_cast<const uint16_t*>(frame->data[1]);
                    const auto* vPlane = reinterpret_cast<const uint16_t*>(frame->data[2]);
                    for (size_t index = 0; index < info.firstRow.size(); ++index)
                        info.firstRow[index] = {yPlane[index], uPlane[index], vPlane[index]};
                }
                return true;
            }
        };

        while (!info.decoded && av_read_frame(formatContext, packet) >= 0) {
            if (packet->stream_index == streamIndex && avcodec_send_packet(codecContext, packet) >= 0)
                receiveFrame();
            av_packet_unref(packet);
        }
        if (!info.decoded && avcodec_send_packet(codecContext, nullptr) >= 0)
            receiveFrame();
    } while (false);

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codecContext);
    avformat_close_input(&formatContext);
    return info;
}

}  // namespace

TEST(ScreenshotRawHeaderTest, AcceptsExactVersionedPayload) {
    const ScreenshotRawHeaderV2 header = ValidHeader();
    EXPECT_TRUE(ce::screenshot::ValidateRawHeader(header, header.totalSize, header.requestId));
}

TEST(ScreenshotRawHeaderTest, RejectsStaleMalformedAndTruncatedMetadata) {
    ScreenshotRawHeaderV2 header = ValidHeader();
    EXPECT_FALSE(ce::screenshot::ValidateRawHeader(header, header.totalSize, header.requestId + 1));
    EXPECT_FALSE(ce::screenshot::ValidateRawHeader(header, header.totalSize - 1, header.requestId));

    header = ValidHeader();
    header.magic ^= 1;
    EXPECT_FALSE(ce::screenshot::ValidateRawHeader(header, header.totalSize, header.requestId));
    header = ValidHeader();
    header.version++;
    EXPECT_FALSE(ce::screenshot::ValidateRawHeader(header, header.totalSize, header.requestId));
    header = ValidHeader();
    header.headerSize--;
    EXPECT_FALSE(ce::screenshot::ValidateRawHeader(header, header.totalSize, header.requestId));
    header = ValidHeader();
    header.reserved64 = 1;
    EXPECT_FALSE(ce::screenshot::ValidateRawHeader(header, header.totalSize, header.requestId));
}

TEST(ScreenshotRawHeaderTest, RejectsInvalidDimensionsFormatsAndRowPitch) {
    ScreenshotRawHeaderV2 header = ValidHeader();
    header.width = ce::screenshot::kMaximumScreenshotDimension + 1;
    EXPECT_FALSE(ce::screenshot::ValidateRawHeader(header, header.totalSize, header.requestId));

    header = ValidHeader();
    header.colorEncoding = static_cast<uint32_t>(ScreenshotColorEncoding::BT2020_PQ);
    EXPECT_FALSE(ce::screenshot::ValidateRawHeader(header, header.totalSize, header.requestId));

    header = ValidHeader();
    header.rowPitch = 7;
    EXPECT_FALSE(ce::screenshot::ValidateRawHeader(header, header.totalSize, header.requestId));

    header = ValidHeader();
    header.width = 1;
    header.rowPitch = 65540;
    header.payloadSize = static_cast<uint64_t>(header.rowPitch) * header.height;
    header.totalSize = sizeof(header) + header.payloadSize;
    EXPECT_FALSE(ce::screenshot::ValidateRawHeader(header, header.totalSize, header.requestId));

    header = ValidHeader();
    header.payloadSize++;
    EXPECT_FALSE(ce::screenshot::ValidateRawHeader(header, header.totalSize, header.requestId));
}

TEST(ScreenshotRawHeaderTest, ReadsExactPayloadAndRejectsTruncation) {
    TemporaryRawFile file;
    const ScreenshotRawHeaderV2 header = ValidHeader();
    const std::array<uint8_t, 16> pixels{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.good());
        output.write(reinterpret_cast<const char*>(&header), sizeof(header));
        output.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
        ASSERT_TRUE(output.good());
    }

    ce::screenshot::RawScreenshot screenshot;
    ASSERT_TRUE(ce::screenshot::ReadRawScreenshot(file.Path(), header.requestId, screenshot));
    EXPECT_EQ(screenshot.pixels, std::vector<uint8_t>(pixels.begin(), pixels.end()));

    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.good());
        output.write(reinterpret_cast<const char*>(&header), sizeof(header));
        output.write(reinterpret_cast<const char*>(pixels.data()), pixels.size() - 1);
    }
    EXPECT_FALSE(ce::screenshot::ReadRawScreenshot(file.Path(), header.requestId, screenshot));
}

TEST(ScreenshotRawHeaderTest, CreatesOnlySupportedCheckedLayouts) {
    std::array<uint8_t, 32> pixels{};
    ce::screenshot::RawScreenshot screenshot;
    EXPECT_TRUE(ce::screenshot::MakeRawScreenshot(pixels.data(), 2, 2, 8, ScreenshotPixelFormat::BGRA8,
                                                  ScreenshotColorEncoding::SRGB, screenshot));
    EXPECT_EQ(screenshot.header.payloadSize, 16u);
    EXPECT_FALSE(ce::screenshot::MakeRawScreenshot(
        pixels.data(), ce::screenshot::kMaximumScreenshotDimension + 1, 1, 8, ScreenshotPixelFormat::BGRA8,
        ScreenshotColorEncoding::SRGB, screenshot));
    EXPECT_FALSE(ce::screenshot::MakeRawScreenshot(pixels.data(), 2, 2, 7, ScreenshotPixelFormat::BGRA8,
                                                   ScreenshotColorEncoding::SRGB, screenshot));
    EXPECT_FALSE(ce::screenshot::MakeRawScreenshot(pixels.data(), 2, 2, 8, ScreenshotPixelFormat::BGRA8,
                                                   ScreenshotColorEncoding::BT2020_PQ, screenshot));
}

TEST(ScreenshotRawHeaderTest, RefusesToEncodeAnInconsistentInMemoryPayload) {
    std::filesystem::path directory = UniqueRawPath();
    directory.replace_extension();
    ASSERT_TRUE(std::filesystem::create_directories(directory));

    std::array<uint8_t, 16> pixels{};
    ce::screenshot::RawScreenshot screenshot;
    ASSERT_TRUE(ce::screenshot::MakeRawScreenshot(pixels.data(), 2, 2, 8, ScreenshotPixelFormat::BGRA8,
                                                  ScreenshotColorEncoding::SRGB, screenshot));
    screenshot.header.payloadSize++;
    std::filesystem::path publishedPath = directory / L"must-not-exist.png";
    EXPECT_FALSE(ce::screenshot::SaveRawScreenshot(directory, screenshot, publishedPath));
    EXPECT_TRUE(publishedPath.empty());
    EXPECT_TRUE(std::filesystem::is_empty(directory));

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

TEST(ScreenshotColorTest, ConvertsFullRangeBt2020PqR10Fixtures) {
    const std::array<uint32_t, 3> fixtures{
        0u,
        0x3FFFFFFFu,
        0x000003FFu,
    };
    const std::array<ce::screenshot::Yuv10Pixel, 3> expected{
        ce::screenshot::Yuv10Pixel{0, 512, 512},
        ce::screenshot::Yuv10Pixel{1023, 512, 512},
        ce::screenshot::Yuv10Pixel{269, 369, 1023},
    };
    for (size_t index = 0; index < fixtures.size(); ++index) {
        ce::screenshot::Yuv10Pixel converted{};
        ASSERT_TRUE(ce::screenshot::ConvertHdrPixelToYuv10(
            ScreenshotPixelFormat::R10G10B10A2, reinterpret_cast<const uint8_t*>(&fixtures[index]), converted));
        EXPECT_EQ(converted.y, expected[index].y);
        EXPECT_EQ(converted.u, expected[index].u);
        EXPECT_EQ(converted.v, expected[index].v);
    }
}

TEST(ScreenshotColorTest, ConvertsLinearScRgbAtEightyNitsPerUnit) {
    const std::array<uint16_t, 4> linearGray{0x3C00u, 0x3C00u, 0x3C00u, 0x3C00u};
    ce::screenshot::Yuv10Pixel converted{};
    ASSERT_TRUE(ce::screenshot::ConvertHdrPixelToYuv10(
        ScreenshotPixelFormat::RGBA16F, reinterpret_cast<const uint8_t*>(linearGray.data()), converted));
    EXPECT_EQ(converted.y, 497);
    EXPECT_EQ(converted.u, 512);
    EXPECT_EQ(converted.v, 512);
}

TEST(ScreenshotColorTest, SanitizesNonFiniteScRgbAndRejectsUnsupportedInput) {
    const std::array<uint16_t, 4> nanPixel{0x7E00u, 0x7E00u, 0x7E00u, 0x3C00u};
    ce::screenshot::Yuv10Pixel converted{};
    ASSERT_TRUE(ce::screenshot::ConvertHdrPixelToYuv10(
        ScreenshotPixelFormat::RGBA16F, reinterpret_cast<const uint8_t*>(nanPixel.data()), converted));
    EXPECT_EQ(converted.y, 0);
    EXPECT_EQ(converted.u, 512);
    EXPECT_EQ(converted.v, 512);
    EXPECT_FALSE(ce::screenshot::ConvertHdrPixelToYuv10(ScreenshotPixelFormat::BGRA8,
                                                        reinterpret_cast<const uint8_t*>(nanPixel.data()), converted));
    EXPECT_FALSE(ce::screenshot::ConvertHdrPixelToYuv10(ScreenshotPixelFormat::RGBA16F, nullptr, converted));
}

TEST(ScreenshotAvifTest, SourceBuiltLibaomEncodesTenBit444WithHdrMetadata) {
    std::filesystem::path directory = UniqueRawPath();
    directory.replace_extension();
    ASSERT_TRUE(std::filesystem::create_directories(directory));

    std::vector<uint32_t> pixels(16, 0x3FFFFFFFu);
    pixels[0] = 0;
    pixels[1] = 0x000003FFu;
    pixels[2] = 0x000FFC00u;
    pixels[3] = 0x3FF00000u;
    ce::screenshot::RawScreenshot screenshot;
    ASSERT_TRUE(ce::screenshot::MakeRawScreenshot(
        reinterpret_cast<const uint8_t*>(pixels.data()), 4, 4, 16, ScreenshotPixelFormat::R10G10B10A2,
        ScreenshotColorEncoding::BT2020_PQ, screenshot));

    std::filesystem::path publishedPath;
    const std::filesystem::path logPath = directory / L"encode.log";
    Log_Init(WideToUtf8(logPath.wstring()), LogLevel::Debug);
    const bool saved = ce::screenshot::SaveRawScreenshot(directory, screenshot, publishedPath);
    Log_Shutdown();
    std::ifstream logInput(logPath, std::ios::binary);
    const std::string encodeLog((std::istreambuf_iterator<char>(logInput)), std::istreambuf_iterator<char>());
    EXPECT_TRUE(saved) << encodeLog;
    EXPECT_EQ(publishedPath.extension(), L".avif");
    EXPECT_TRUE(std::filesystem::exists(publishedPath));

    const DecodedAvifInfo decoded = DecodeAvif(publishedPath);
    EXPECT_TRUE(decoded.decoded);
    EXPECT_EQ(decoded.width, 4);
    EXPECT_EQ(decoded.height, 4);
    EXPECT_EQ(decoded.format, AV_PIX_FMT_YUV444P10LE);
    EXPECT_EQ(decoded.colorPrimaries, AVCOL_PRI_BT2020);
    EXPECT_EQ(decoded.colorTransfer, AVCOL_TRC_SMPTE2084);
    EXPECT_EQ(decoded.colorSpace, AVCOL_SPC_BT2020_NCL);
    EXPECT_EQ(decoded.colorRange, AVCOL_RANGE_JPEG);
    for (size_t index = 0; index < decoded.firstRow.size(); ++index) {
        ce::screenshot::Yuv10Pixel expected{};
        ASSERT_TRUE(ce::screenshot::ConvertHdrPixelToYuv10(
            ScreenshotPixelFormat::R10G10B10A2, reinterpret_cast<const uint8_t*>(&pixels[index]), expected));
        const auto error = [](uint16_t actual, uint16_t reference) {
            return actual > reference ? actual - reference : reference - actual;
        };
        EXPECT_LE(error(decoded.firstRow[index].y, expected.y), 32u);
        EXPECT_LE(error(decoded.firstRow[index].u, expected.u), 32u);
        EXPECT_LE(error(decoded.firstRow[index].v, expected.v), 32u);
    }

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}
