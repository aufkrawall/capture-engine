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
    // Publish both variants of one capture: the preserved HDR AVIF and the
    // tone-mapped SDR PNG. An SDR source has no second variant to publish and
    // still yields exactly one PNG.
    SourceAndBt709 = 2,
};

// What a single capture actually published. An unused variant stays empty.
struct ScreenshotPublication {
    std::filesystem::path hdrPath;  // 10-bit 4:4:4 BT.2020/PQ AVIF
    std::filesystem::path sdrPath;  // BT.709/sRGB PNG

    bool Any() const {
        return !hdrPath.empty() || !sdrPath.empty();
    }
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
// True when the payload carries genuine HDR content, which is the only case in
// which a second, HDR-preserving variant exists to publish.
bool IsHdrScreenshotSource(const RawScreenshot& screenshot);
bool SaveRawScreenshot(const std::filesystem::path& outputDirectory, const RawScreenshot& screenshot,
                       ScreenshotPublication& published,
                       ScreenshotOutputColorSpace outputColorSpace = ScreenshotOutputColorSpace::PreserveSource,
                       float sdrWhiteNits = 203.0f);

}  // namespace ce::screenshot
