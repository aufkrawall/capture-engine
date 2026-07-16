#include <gtest/gtest.h>

#include "../hook/common/screenshot_hook.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

std::atomic<uint32_t> g_sequence{1};

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

std::filesystem::path UniqueDirectory() {
    return std::filesystem::temp_directory_path() /
           (L"ce_screenshot_worker_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
            std::to_wstring(g_sequence.fetch_add(1, std::memory_order_relaxed)));
}

std::string UniqueEventName() {
    return "Local\\CaptureEngineScreenshotWorkerTest_" + std::to_string(GetCurrentProcessId()) + "_" +
           std::to_string(g_sequence.fetch_add(1, std::memory_order_relaxed));
}

class ScreenshotWorkerTest : public testing::Test {
protected:
    void TearDown() override {
        ShutdownScreenshotWorker();
        if (!directory_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(directory_, error);
        }
    }

    bool PrepareRequest(const std::filesystem::path& partPath, uint64_t requestId) {
        const std::string utf8Path = WideToUtf8(partPath.wstring());
        eventName_ = UniqueEventName();
        if (utf8Path.empty() || utf8Path.size() >= sizeof(shared_.runtimeState.screenshotPath) ||
            eventName_.size() >= sizeof(shared_.runtimeState.screenshotCompletionEventName)) {
            return false;
        }
        event_.reset(CreateEventA(nullptr, FALSE, FALSE, eventName_.c_str()));
        if (!event_)
            return false;
        strcpy_s(shared_.runtimeState.screenshotPath, utf8Path.c_str());
        strcpy_s(shared_.runtimeState.screenshotCompletionEventName, eventName_.c_str());
        shared_.runtimeState.screenshotRequestId.store(requestId, std::memory_order_release);
        shared_.runtimeState.screenshotCompletedRequestId.store(0, std::memory_order_release);
        shared_.runtimeState.screenshotStatus.store(static_cast<uint32_t>(ScreenshotRequestStatus::Pending),
                                                    std::memory_order_release);
        return true;
    }

    struct HandleCloser {
        void operator()(void* handle) const {
            if (handle)
                CloseHandle(static_cast<HANDLE>(handle));
        }
    };

    using UniqueHandle = std::unique_ptr<void, HandleCloser>;

    SharedMemoryLayout shared_{};
    UniqueHandle event_;
    std::string eventName_;
    std::filesystem::path directory_;
};

}  // namespace

TEST_F(ScreenshotWorkerTest, PublishesExactRawPayloadBeforeSignalingSuccess) {
    directory_ = UniqueDirectory();
    ASSERT_TRUE(std::filesystem::create_directories(directory_));
    const std::filesystem::path partPath = directory_ / L"capture.part";
    const std::filesystem::path readyPath = directory_ / L"capture.ready";
    constexpr uint64_t requestId = 17;
    ASSERT_TRUE(PrepareRequest(partPath, requestId));

    const std::array<uint8_t, 16> pixels{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    ASSERT_TRUE(QueueScreenshotPixels(&shared_, requestId, pixels.data(), 2, 2, 8, ScreenshotPixelFormat::BGRA8,
                                      ScreenshotColorEncoding::SRGB));
    ASSERT_EQ(WaitForSingleObject(event_.get(), 5000), WAIT_OBJECT_0);

    EXPECT_EQ(shared_.runtimeState.screenshotCompletedRequestId.load(std::memory_order_acquire), requestId);
    EXPECT_EQ(shared_.runtimeState.screenshotStatus.load(std::memory_order_acquire),
              static_cast<uint32_t>(ScreenshotRequestStatus::Succeeded));
    EXPECT_EQ(shared_.runtimeState.screenshotPayloadKind.load(std::memory_order_acquire),
              static_cast<uint32_t>(ScreenshotPayloadKind::RawV2));
    EXPECT_FALSE(std::filesystem::exists(partPath));
    ASSERT_TRUE(std::filesystem::exists(readyPath));

    std::ifstream input(readyPath, std::ios::binary);
    ScreenshotRawHeaderV2 header{};
    std::array<uint8_t, 16> storedPixels{};
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    input.read(reinterpret_cast<char*>(storedPixels.data()), storedPixels.size());
    ASSERT_TRUE(input.good());
    EXPECT_EQ(header.requestId, requestId);
    EXPECT_EQ(header.totalSize, sizeof(header) + pixels.size());
    EXPECT_EQ(header.payloadSize, pixels.size());
    EXPECT_EQ(storedPixels, pixels);
}

TEST_F(ScreenshotWorkerTest, ReportsWorkerPublicationFailureWithoutLeavingPartFile) {
    directory_ = UniqueDirectory();
    const std::filesystem::path partPath = directory_ / L"missing" / L"capture.part";
    constexpr uint64_t requestId = 23;
    ASSERT_TRUE(PrepareRequest(partPath, requestId));

    const std::array<uint8_t, 4> pixel{1, 2, 3, 4};
    ASSERT_TRUE(QueueScreenshotPixels(&shared_, requestId, pixel.data(), 1, 1, 4, ScreenshotPixelFormat::BGRA8,
                                      ScreenshotColorEncoding::SRGB));
    ASSERT_EQ(WaitForSingleObject(event_.get(), 5000), WAIT_OBJECT_0);

    EXPECT_EQ(shared_.runtimeState.screenshotCompletedRequestId.load(std::memory_order_acquire), requestId);
    EXPECT_EQ(shared_.runtimeState.screenshotStatus.load(std::memory_order_acquire),
              static_cast<uint32_t>(ScreenshotRequestStatus::Failed));
    EXPECT_NE(shared_.runtimeState.screenshotError.load(std::memory_order_acquire), ERROR_SUCCESS);
    EXPECT_EQ(shared_.runtimeState.screenshotPayloadKind.load(std::memory_order_acquire),
              static_cast<uint32_t>(ScreenshotPayloadKind::None));
    EXPECT_FALSE(std::filesystem::exists(partPath));
}
