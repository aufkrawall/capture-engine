#include <gtest/gtest.h>

#include "captureengine/media/screenshot_encoding.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::filesystem::path UniqueRawPath() {
    static std::atomic<uint32_t> sequence{1};
    return std::filesystem::temp_directory_path() /
           (L"ce_screenshot_paths_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
            std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)) + L".ready");
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty())
        return {};
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                             static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0)
        return {};
    std::string result(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                            required, nullptr, nullptr) != required) {
        return {};
    }
    return result;
}

}  // namespace

// TakeScreenshot reports the files a capture published (M6): the SCREENSHOT event carries them. The
// capture itself needs a desktop, so these drive the same publication structure it fills.
namespace {

std::filesystem::path PathFromUtf8(const std::string& utf8) {
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                                           nullptr, 0);
    std::wstring wide(static_cast<size_t>(std::max(length, 0)), L'\0');
    if (length > 0)
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                            length);
    return std::filesystem::path(wide);
}

std::filesystem::path FreshScreenshotDirectory() {
    std::filesystem::path directory = UniqueRawPath();
    directory.replace_extension();
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);
    return directory;
}

}  // namespace

TEST(ScreenshotPathsTest, CombinedHdrCaptureReportsBothFilesHdrFirst) {
    const std::filesystem::path directory = FreshScreenshotDirectory();
    ASSERT_TRUE(std::filesystem::is_directory(directory));
    std::vector<uint16_t> pixels(4 * 4 * 4, 0x3C00u);
    ce::screenshot::RawScreenshot screenshot;
    ASSERT_TRUE(ce::screenshot::MakeRawScreenshot(reinterpret_cast<const uint8_t*>(pixels.data()), 4, 4, 32,
                                                  ScreenshotPixelFormat::RGBA16F,
                                                  ScreenshotColorEncoding::LinearScRGB, screenshot));

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ASSERT_TRUE(SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE);
    ce::screenshot::ScreenshotPublication published;
    const bool saved = ce::screenshot::SaveRawScreenshot(
        directory, screenshot, published, ce::screenshot::ScreenshotOutputColorSpace::SourceAndBt709, 80.0f);
    if (SUCCEEDED(comResult))
        CoUninitialize();
    ASSERT_TRUE(saved);

    const std::vector<std::string> paths = ce::screenshot::PublishedScreenshotPaths(published);
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_EQ(paths[0], WideToUtf8(published.hdrPath.wstring()));
    EXPECT_EQ(paths[1], WideToUtf8(published.sdrPath.wstring()));
    EXPECT_EQ(PathFromUtf8(paths[0]).extension(), L".avif");
    EXPECT_EQ(PathFromUtf8(paths[1]).extension(), L".png");
    EXPECT_TRUE(std::filesystem::exists(PathFromUtf8(paths[0])));
    EXPECT_TRUE(std::filesystem::exists(PathFromUtf8(paths[1])));

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

TEST(ScreenshotPathsTest, SdrCaptureReportsExactlyOnePngEvenInCombinedMode) {
    const std::filesystem::path directory = FreshScreenshotDirectory();
    ASSERT_TRUE(std::filesystem::is_directory(directory));
    const std::array<uint8_t, 8> pixels{0, 0, 0, 255, 255, 255, 255, 255};
    ce::screenshot::RawScreenshot screenshot;
    ASSERT_TRUE(ce::screenshot::MakeRawScreenshot(pixels.data(), 2, 1, 8, ScreenshotPixelFormat::BGRA8,
                                                  ScreenshotColorEncoding::SRGB, screenshot));

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ASSERT_TRUE(SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE);
    ce::screenshot::ScreenshotPublication published;
    const bool saved = ce::screenshot::SaveRawScreenshot(
        directory, screenshot, published, ce::screenshot::ScreenshotOutputColorSpace::SourceAndBt709, 200.0f);
    if (SUCCEEDED(comResult))
        CoUninitialize();
    ASSERT_TRUE(saved);

    const std::vector<std::string> paths = ce::screenshot::PublishedScreenshotPaths(published);
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(PathFromUtf8(paths[0]).extension(), L".png");
    EXPECT_TRUE(std::filesystem::exists(PathFromUtf8(paths[0])));

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

TEST(ScreenshotPathsTest, FailedEncodePublishesAndReportsNoPaths) {
    const std::filesystem::path directory = FreshScreenshotDirectory();
    ASSERT_TRUE(std::filesystem::is_directory(directory));
    std::array<uint8_t, 16> pixels{};
    ce::screenshot::RawScreenshot screenshot;
    ASSERT_TRUE(ce::screenshot::MakeRawScreenshot(pixels.data(), 2, 2, 8, ScreenshotPixelFormat::BGRA8,
                                                  ScreenshotColorEncoding::SRGB, screenshot));
    screenshot.header.payloadSize++;  // inconsistent payload: nothing may be written
    ce::screenshot::ScreenshotPublication published;
    EXPECT_FALSE(ce::screenshot::SaveRawScreenshot(directory, screenshot, published));
    EXPECT_TRUE(ce::screenshot::PublishedScreenshotPaths(published).empty());
    EXPECT_TRUE(std::filesystem::is_empty(directory));

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

TEST(ScreenshotPathsTest, PartialCombinedPublicationReportsTheFileThatExists) {
    // SaveHdrAndSdrScreenshots keeps whichever variant did publish when the other failed: the request
    // fails, but the kept file is real and a caller must be able to find it.
    ce::screenshot::ScreenshotPublication published;
    published.hdrPath = L"D:\\Captures\\capture_1.avif";
    const std::vector<std::string> paths = ce::screenshot::PublishedScreenshotPaths(published);
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths[0], "D:\\Captures\\capture_1.avif");

    ce::screenshot::ScreenshotPublication sdrOnly;
    sdrOnly.sdrPath = L"D:\\Captures\\capture_1.png";
    ASSERT_EQ(ce::screenshot::PublishedScreenshotPaths(sdrOnly).size(), 1u);

    EXPECT_TRUE(ce::screenshot::PublishedScreenshotPaths(ce::screenshot::ScreenshotPublication{}).empty());
}

TEST(ScreenshotPathsTest, NonAsciiDirectoryRoundTripsAsUtf8) {
    // The runtime hands these paths to clients as UTF-8; an ACP-lossy conversion would name a file that
    // does not exist.
    ce::screenshot::ScreenshotPublication published;
    published.sdrPath = std::filesystem::path(L"D:\\Aufn\u00E4hmen\\\u65E5\u672C\\capture_2.png");
    const std::vector<std::string> paths = ce::screenshot::PublishedScreenshotPaths(published);
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths[0], "D:\\Aufn\xC3\xA4hmen\\\xE6\x97\xA5\xE6\x9C\xAC\\capture_2.png");
    EXPECT_EQ(PathFromUtf8(paths[0]), published.sdrPath);
}
