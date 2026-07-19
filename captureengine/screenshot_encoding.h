#pragma once

#include "../common/shared_defs.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace ce::screenshot {

constexpr uint32_t kMaximumScreenshotDimension = 16384;

struct RawScreenshot {
    ScreenshotRawHeaderV2 header{};
    std::vector<uint8_t> pixels;
};

struct Yuv10Pixel {
    uint16_t y = 0;
    uint16_t u = 0;
    uint16_t v = 0;
};

struct Bgra8Pixel {
    uint8_t b = 0;
    uint8_t g = 0;
    uint8_t r = 0;
    uint8_t a = 255;
};
static_assert(sizeof(Bgra8Pixel) == 4, "BGRA screenshot pixels must be tightly packed");

enum class ScreenshotOutputColorSpace : uint8_t {
    PreserveSource = 0,
    Bt709 = 1,
};

struct AvifEncodingPlan {
    uint32_t threadCount = 1;
    uint32_t tileColumnsLog2 = 0;
    uint32_t tileRowsLog2 = 0;
};

uint32_t BytesPerPixel(ScreenshotPixelFormat format);
AvifEncodingPlan SelectAvifEncodingPlan(uint32_t width, uint32_t height, uint32_t hardwareThreads);
bool IsValidFormatEncoding(ScreenshotPixelFormat format, ScreenshotColorEncoding encoding);
bool ValidateRawHeader(const ScreenshotRawHeaderV2& header, uint64_t fileSize, uint64_t expectedRequestId);
bool ReadRawScreenshot(const std::filesystem::path& path, uint64_t expectedRequestId, RawScreenshot& screenshot);
bool MakeRawScreenshot(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t rowPitch,
                       ScreenshotPixelFormat format, ScreenshotColorEncoding encoding, RawScreenshot& screenshot);
bool ConvertHdrPixelToYuv10(ScreenshotPixelFormat format, const uint8_t* pixel, Yuv10Pixel& converted);
bool ConvertHdrPixelToSdrBgra(ScreenshotPixelFormat format, const uint8_t* pixel, float sdrWhiteNits,
                              Bgra8Pixel& converted);
bool SaveRawScreenshot(const std::filesystem::path& outputDirectory, const RawScreenshot& screenshot,
                       std::filesystem::path& publishedPath,
                       ScreenshotOutputColorSpace outputColorSpace = ScreenshotOutputColorSpace::PreserveSource,
                       float sdrWhiteNits = 203.0f);

}  // namespace ce::screenshot
