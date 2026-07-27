// Screenshot raw-file handling, the SDR conversion path, the PQ/ST-2084
// transfer maths and the public ce::screenshot entry points. Parallel HDR
// conversion and AVIF encoding live in screenshot_hdr_encoding.cpp.

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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "screenshot_encoding_internal.h"

namespace ce::screenshot {

AvifEncodingPlan SelectAvifEncodingPlan(uint32_t width, uint32_t height, uint32_t hardwareThreads) {
    AvifEncodingPlan plan;
    plan.threadCount = std::min<uint32_t>({16u, std::max(1u, hardwareThreads), std::max(1u, height)});
    if (width >= 3840 && plan.threadCount >= 8) {
        plan.tileColumnsLog2 = 2;
    } else if (width >= 1920 && plan.threadCount >= 4) {
        plan.tileColumnsLog2 = 1;
    }
    if (height >= 2160 && plan.threadCount >= 8)
        plan.tileRowsLog2 = 1;
    return plan;
}

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

bool FlushPath(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const bool flushed = FlushFileBuffers(file) != FALSE;
    const bool closed = CloseHandle(file) != FALSE;
    return flushed && closed;
}

ReservedCaptureOutput ReserveScreenshotStaging(const std::filesystem::path& outputDirectory) {
    return ReservedCaptureOutput::Reserve(outputDirectory, L"screenshot_stage", L".part");
}

namespace {

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

double LinearToSrgb(double value);
uint8_t Quantize8(double value);
float HalfToFloat(uint16_t half);

bool ConvertSdrToTightBgra(const RawScreenshot& screenshot, std::vector<uint8_t>& bgra) {
    const auto format = static_cast<ScreenshotPixelFormat>(screenshot.header.pixelFormat);
    const auto encoding = static_cast<ScreenshotColorEncoding>(screenshot.header.colorEncoding);
    const bool isEightBitSrgb =
        (format == ScreenshotPixelFormat::BGRA8 || format == ScreenshotPixelFormat::RGBA8) &&
        encoding == ScreenshotColorEncoding::SRGB;
    const bool isTenBitGamma22 =
        format == ScreenshotPixelFormat::R10G10B10A2 && encoding == ScreenshotColorEncoding::BT709_G22;
    const bool isTenBitSrgb =
        format == ScreenshotPixelFormat::R10G10B10A2 && encoding == ScreenshotColorEncoding::SRGB;
    const bool isLinearFp16Sdr =
        format == ScreenshotPixelFormat::RGBA16F && encoding == ScreenshotColorEncoding::LinearScRGBSdr;
    if (!isEightBitSrgb && !isTenBitGamma22 && !isTenBitSrgb && !isLinearFp16Sdr) {
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
        } else if (format == ScreenshotPixelFormat::RGBA8) {
            for (uint32_t x = 0; x < screenshot.header.width; ++x) {
                destination[x * 4 + 0] = source[x * 4 + 2];
                destination[x * 4 + 1] = source[x * 4 + 1];
                destination[x * 4 + 2] = source[x * 4 + 0];
                destination[x * 4 + 3] = source[x * 4 + 3];
            }
        } else if (format == ScreenshotPixelFormat::R10G10B10A2) {
            for (uint32_t x = 0; x < screenshot.header.width; ++x) {
                uint32_t packed = 0;
                memcpy(&packed, source + static_cast<size_t>(x) * 4, sizeof(packed));
                const double red = static_cast<double>(packed & 0x3FFu) / 1023.0;
                const double green = static_cast<double>((packed >> 10) & 0x3FFu) / 1023.0;
                const double blue = static_cast<double>((packed >> 20) & 0x3FFu) / 1023.0;
                destination[x * 4 + 0] =
                    Quantize8(isTenBitGamma22 ? LinearToSrgb(std::pow(blue, 2.2)) : blue);
                destination[x * 4 + 1] =
                    Quantize8(isTenBitGamma22 ? LinearToSrgb(std::pow(green, 2.2)) : green);
                destination[x * 4 + 2] =
                    Quantize8(isTenBitGamma22 ? LinearToSrgb(std::pow(red, 2.2)) : red);
                destination[x * 4 + 3] = 255;
            }
        } else {
            for (uint32_t x = 0; x < screenshot.header.width; ++x) {
                uint16_t components[4]{};
                memcpy(components, source + static_cast<size_t>(x) * 8, sizeof(components));
                destination[x * 4 + 0] = Quantize8(LinearToSrgb(HalfToFloat(components[2])));
                destination[x * 4 + 1] = Quantize8(LinearToSrgb(HalfToFloat(components[1])));
                destination[x * 4 + 2] = Quantize8(LinearToSrgb(HalfToFloat(components[0])));
                destination[x * 4 + 3] = 255;
            }
        }
    }
    return true;
}

bool SavePixelsAsPng(const std::filesystem::path& outputDirectory, const RawScreenshot& screenshot,
                     std::filesystem::path& publishedPath) {
    std::vector<uint8_t> bgra;
    if (!ConvertSdrToTightBgra(screenshot, bgra))
        return false;

    ReservedCaptureOutput staging = ReserveScreenshotStaging(outputDirectory);
    if (!staging || !staging.ReleaseToWriter()) {
        LogError("[Screenshot] PNG staging reservation failed: win32=%lu", static_cast<unsigned long>(GetLastError()));
        return false;
    }

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* options = nullptr;
    bool encoded = false;
    const char* failureStage = "initialize WIC";
    HRESULT failureResult = E_FAIL;

    do {
        failureStage = "create WIC factory";
        failureResult =
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(failureResult))
            break;
        failureStage = "create WIC stream";
        failureResult = factory->CreateStream(&stream);
        if (FAILED(failureResult))
            break;
        failureStage = "open PNG staging stream";
        failureResult = stream->InitializeFromFilename(staging.Path().c_str(), GENERIC_WRITE);
        if (FAILED(failureResult))
            break;
        failureStage = "create PNG encoder";
        failureResult = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (FAILED(failureResult))
            break;
        failureStage = "initialize PNG encoder";
        failureResult = encoder->Initialize(stream, WICBitmapEncoderNoCache);
        if (FAILED(failureResult))
            break;
        failureStage = "create PNG frame";
        failureResult = encoder->CreateNewFrame(&frame, &options);
        if (FAILED(failureResult))
            break;
        failureStage = "initialize PNG frame";
        failureResult = frame->Initialize(options);
        if (FAILED(failureResult))
            break;
        failureStage = "set PNG dimensions";
        failureResult = frame->SetSize(screenshot.header.width, screenshot.header.height);
        if (FAILED(failureResult))
            break;

        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
        failureStage = "set PNG pixel format";
        failureResult = frame->SetPixelFormat(&pixelFormat);
        if (FAILED(failureResult) || pixelFormat != GUID_WICPixelFormat32bppBGRA) {
            if (SUCCEEDED(failureResult))
                failureResult = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
            break;
        }
        const UINT rowPitch = screenshot.header.width * 4;
        failureStage = "write PNG pixels";
        failureResult =
            frame->WritePixels(screenshot.header.height, rowPitch, static_cast<UINT>(bgra.size()), bgra.data());
        if (FAILED(failureResult))
            break;
        failureStage = "commit PNG frame";
        failureResult = frame->Commit();
        if (FAILED(failureResult))
            break;
        failureStage = "commit PNG encoder";
        failureResult = encoder->Commit();
        if (FAILED(failureResult))
            break;
        failureStage = "commit PNG stream";
        failureResult = stream->Commit(STGC_DEFAULT);
        if (FAILED(failureResult))
            break;
        encoded = true;
    } while (false);

    SafeRelease(options);
    SafeRelease(frame);
    SafeRelease(encoder);
    SafeRelease(stream);
    SafeRelease(factory);

    if (!encoded) {
        LogError("[Screenshot] PNG encode failed at '%s': 0x%08X", failureStage,
                 static_cast<unsigned>(failureResult));
        return false;
    }
    if (!FlushPath(staging.Path())) {
        LogError("[Screenshot] PNG staging flush failed: win32=%lu", static_cast<unsigned long>(GetLastError()));
        return false;
    }
    if (!staging.PublishToNewPath(outputDirectory, L"screenshot", L".png")) {
        LogError("[Screenshot] PNG atomic publication failed: win32=%lu", static_cast<unsigned long>(GetLastError()));
        return false;
    }
    publishedPath = staging.Path();
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

double DecodeSt2084(double pq) {
    constexpr double m1 = 2610.0 / 16384.0;
    constexpr double m2 = 2523.0 / 32.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = 2413.0 / 128.0;
    constexpr double c3 = 2392.0 / 128.0;
    const double powered = std::pow(std::clamp(pq, 0.0, 1.0), 1.0 / m2);
    const double denominator = std::max(c2 - c3 * powered, 0.000001);
    return std::pow(std::max(powered - c1, 0.0) / denominator, 1.0 / m1) * 10000.0;
}

double LinearToSrgb(double value) {
    value = std::max(value, 0.0);
    return value < 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

void CompressRec709Gamut(double& red, double& green, double& blue, double luminance) {
    const double minimum = std::min(red, std::min(green, blue));
    if (minimum < 0.0) {
        const double scale = std::clamp(luminance / std::max(luminance - minimum, 0.000001), 0.0, 1.0);
        red = luminance + (red - luminance) * scale;
        green = luminance + (green - luminance) * scale;
        blue = luminance + (blue - luminance) * scale;
    }
    const double maximum = std::max(red, std::max(green, blue));
    if (maximum > 1.0) {
        const double scale =
            std::clamp((1.0 - luminance) / std::max(maximum - luminance, 0.000001), 0.0, 1.0);
        red = luminance + (red - luminance) * scale;
        green = luminance + (green - luminance) * scale;
        blue = luminance + (blue - luminance) * scale;
    }
    red = std::clamp(red, 0.0, 1.0);
    green = std::clamp(green, 0.0, 1.0);
    blue = std::clamp(blue, 0.0, 1.0);
}

uint8_t Quantize8(double value) {
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
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
            return encoding == ScreenshotColorEncoding::BT2020_PQ ||
                   encoding == ScreenshotColorEncoding::BT709_G22 || encoding == ScreenshotColorEncoding::SRGB;
        case ScreenshotPixelFormat::RGBA16F:
            return encoding == ScreenshotColorEncoding::LinearScRGB ||
                   encoding == ScreenshotColorEncoding::LinearScRGBSdr;
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

bool ConvertHdrPixelToSdrBgra(ScreenshotPixelFormat format, const uint8_t* pixel, float sdrWhiteNits,
                              Bgra8Pixel& converted) {
    if (!pixel)
        return false;

    double redNits = 0.0;
    double greenNits = 0.0;
    double blueNits = 0.0;
    if (format == ScreenshotPixelFormat::R10G10B10A2) {
        uint32_t packed = 0;
        memcpy(&packed, pixel, sizeof(packed));
        const double red2020 = DecodeSt2084(static_cast<double>(packed & 0x3FFu) / 1023.0);
        const double green2020 = DecodeSt2084(static_cast<double>((packed >> 10) & 0x3FFu) / 1023.0);
        const double blue2020 = DecodeSt2084(static_cast<double>((packed >> 20) & 0x3FFu) / 1023.0);
        redNits = 1.6604910 * red2020 - 0.5876411 * green2020 - 0.0728499 * blue2020;
        greenNits = -0.1245505 * red2020 + 1.1328999 * green2020 - 0.0083494 * blue2020;
        blueNits = -0.0181508 * red2020 - 0.1005789 * green2020 + 1.1187297 * blue2020;
    } else if (format == ScreenshotPixelFormat::RGBA16F) {
        uint16_t components[4]{};
        memcpy(components, pixel, sizeof(components));
        redNits = static_cast<double>(HalfToFloat(components[0])) * 80.0;
        greenNits = static_cast<double>(HalfToFloat(components[1])) * 80.0;
        blueNits = static_cast<double>(HalfToFloat(components[2])) * 80.0;
    } else {
        return false;
    }
    if (!std::isfinite(redNits))
        redNits = 0.0;
    if (!std::isfinite(greenNits))
        greenNits = 0.0;
    if (!std::isfinite(blueNits))
        blueNits = 0.0;

    const double whiteNits = std::clamp(std::isfinite(sdrWhiteNits) ? static_cast<double>(sdrWhiteNits) : 203.0,
                                        80.0, 1000.0);
    double red = redNits / whiteNits;
    double green = greenNits / whiteNits;
    double blue = blueNits / whiteNits;
    const double luminance = std::max(0.2126 * red + 0.7152 * green + 0.0722 * blue, 0.0);
    double mappedLuminance = luminance * 0.8;
    if (luminance > 1.0) {
        const double highlight = luminance - 1.0;
        mappedLuminance = 0.8 + 0.2 * highlight / (highlight + 1.0);
    }
    const double scale = luminance > 0.000001 ? mappedLuminance / luminance : 0.0;
    red *= scale;
    green *= scale;
    blue *= scale;
    CompressRec709Gamut(red, green, blue, mappedLuminance);

    converted.r = Quantize8(LinearToSrgb(red));
    converted.g = Quantize8(LinearToSrgb(green));
    converted.b = Quantize8(LinearToSrgb(blue));
    converted.a = 255;
    return true;
}

bool SaveRawScreenshot(const std::filesystem::path& outputDirectory, const RawScreenshot& screenshot,
                       std::filesystem::path& publishedPath, ScreenshotOutputColorSpace outputColorSpace,
                       float sdrWhiteNits) {
    publishedPath.clear();
    if (screenshot.pixels.size() != screenshot.header.payloadSize ||
        !ValidateRawHeader(screenshot.header, screenshot.header.totalSize, screenshot.header.requestId)) {
        return false;
    }
    const auto format = static_cast<ScreenshotPixelFormat>(screenshot.header.pixelFormat);
    const auto encoding = static_cast<ScreenshotColorEncoding>(screenshot.header.colorEncoding);
    if (format == ScreenshotPixelFormat::BGRA8 || format == ScreenshotPixelFormat::RGBA8 ||
        (format == ScreenshotPixelFormat::R10G10B10A2 &&
         (encoding == ScreenshotColorEncoding::BT709_G22 || encoding == ScreenshotColorEncoding::SRGB)) ||
        (format == ScreenshotPixelFormat::RGBA16F && encoding == ScreenshotColorEncoding::LinearScRGBSdr)) {
        return SavePixelsAsPng(outputDirectory, screenshot, publishedPath);
    }
    if (format != ScreenshotPixelFormat::R10G10B10A2 && format != ScreenshotPixelFormat::RGBA16F)
        return false;
    if (outputColorSpace == ScreenshotOutputColorSpace::Bt709) {
        RawScreenshot converted;
        if (!ConvertHdrToSdrScreenshot(screenshot, sdrWhiteNits, converted))
            return false;
        return SavePixelsAsPng(outputDirectory, converted, publishedPath);
    }
    return SaveHdrAvif(outputDirectory, screenshot, publishedPath);
}

}  // namespace ce::screenshot
