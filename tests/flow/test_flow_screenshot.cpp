// Screenshot requests as the screenshot code in CaptureEngine sends them, served by the hook on a running game, with
// and without frame generation, with and without the overlay in the picture. W3 with its overlay stack (session
// 20261106_195839) failed every in-game request at the device identity check and fell back to desktop capture: the
// back buffer and the queue gave two interfaces of one device. The device-view scenarios reproduce that shape.

#include "tests/flow/flow_test_support.h"
#include "tests/flow/device_view_answers.h"

#include "common/ipc/shared_defs.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

using namespace ce::flow;

constexpr int kFramesToServeARequest = 240;

struct ScreenshotResult {
    ScreenshotRequestStatus status = ScreenshotRequestStatus::Idle;
    ScreenshotPayloadKind payload = ScreenshotPayloadKind::None;
    uint32_t error = 0;
    ScreenshotRawHeaderV2 header{};
    std::vector<uint8_t> pixels;
};

// Publishes a request in the protocol's order, renders until the hook completes it and reads the published payload.
ScreenshotResult TakeScreenshot(FlowGame& game, uint64_t requestId) {
    ScreenshotResult result;
    SharedMemoryLayout* memory = game.CaptureMemory();
    auto& state = memory->runtimeState;
    const std::filesystem::path directory = std::filesystem::path(game.LogDirectory()) / "screenshots";
    std::filesystem::create_directories(directory);
    const std::filesystem::path partPath = directory / ("request" + std::to_string(requestId) + ".raw.part");
    const std::string partPathText = partPath.string();
    char eventName[128]{};
    snprintf(eventName, sizeof(eventName), "Local\\CEFlow_Screenshot_%08lX_%llu", GetCurrentProcessId(),
             static_cast<unsigned long long>(requestId));
    const HANDLE completion = CreateEventA(nullptr, FALSE, FALSE, eventName);
    if (!completion || partPathText.size() >= sizeof(state.screenshotPath)) {
        ADD_FAILURE() << "could not prepare the request";
        return result;
    }
    state.screenshotRequestId.store(0, std::memory_order_release);
    std::memset(state.screenshotPath, 0, sizeof(state.screenshotPath));
    std::memcpy(state.screenshotPath, partPathText.data(), partPathText.size());
    std::memset(state.screenshotCompletionEventName, 0, sizeof(state.screenshotCompletionEventName));
    std::memcpy(state.screenshotCompletionEventName, eventName, std::strlen(eventName));
    state.screenshotError.store(ERROR_SUCCESS, std::memory_order_relaxed);
    state.screenshotPayloadKind.store(static_cast<uint32_t>(ScreenshotPayloadKind::None), std::memory_order_relaxed);
    state.screenshotStatus.store(static_cast<uint32_t>(ScreenshotRequestStatus::Pending), std::memory_order_release);
    state.screenshotRequestId.store(requestId, std::memory_order_release);

    bool completed = false;
    for (int frame = 0; frame < kFramesToServeARequest && !completed; ++frame) {
        if (!game.RenderFrame()) {
            ADD_FAILURE() << game.Error();
            break;
        }
        completed = WaitForSingleObject(completion, 0) == WAIT_OBJECT_0;
    }
    // The readback may still be on its way from the GPU once the frames stopped.
    if (!completed)
        completed = WaitForSingleObject(completion, 20000) == WAIT_OBJECT_0;
    CloseHandle(completion);
    EXPECT_TRUE(completed) << "the hook never completed the request; logs: " << game.LogDirectory();

    result.status = static_cast<ScreenshotRequestStatus>(state.screenshotStatus.load(std::memory_order_acquire));
    result.payload = static_cast<ScreenshotPayloadKind>(state.screenshotPayloadKind.load(std::memory_order_acquire));
    result.error = state.screenshotError.load(std::memory_order_acquire);
    state.screenshotRequestId.store(0, std::memory_order_release);
    std::filesystem::path readyPath = partPath;
    readyPath.replace_extension(".ready");
    std::ifstream file(readyPath, std::ios::binary);
    if (file.read(reinterpret_cast<char*>(&result.header), sizeof(result.header))) {
        result.pixels.resize(static_cast<size_t>(result.header.payloadSize));
        file.read(reinterpret_cast<char*>(result.pixels.data()), static_cast<std::streamsize>(result.pixels.size()));
    }
    return result;
}

void ExpectACapturedFrame(const ScreenshotResult& result, uint64_t requestId, FlowGame& game) {
    EXPECT_EQ(static_cast<uint32_t>(result.status), static_cast<uint32_t>(ScreenshotRequestStatus::Succeeded))
        << "error " << result.error << "; logs: " << game.LogDirectory();
    EXPECT_EQ(static_cast<uint32_t>(result.payload), static_cast<uint32_t>(ScreenshotPayloadKind::RawV2));
    EXPECT_EQ(result.header.requestId, requestId);
    EXPECT_EQ(result.header.width, 640u);
    EXPECT_EQ(result.header.height, 360u);
    ASSERT_EQ(result.pixels.size(), result.header.payloadSize);
    ASSERT_FALSE(result.pixels.empty());
    bool anyPixel = false;
    for (uint8_t byte : result.pixels)
        anyPixel = anyPixel || byte != 0;
    EXPECT_TRUE(anyPixel) << "the frame's pixels never reached the file";
}

void SetOverlayInScreenshots(FlowGame& game, bool include) {
    SharedMemoryLayout* memory = game.CaptureMemory();
    memory->BeginWriteOverlayConfig();
    memory->overlayConfig.screenshotIncludeOverlay = include;
    memory->EndWriteOverlayConfig();
}

GameOptions StreamlineGameOptions() {
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kStreamline;
    return options;
}

void StartDLSSGame(FlowGame& game) {
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(StreamlineGameOptions())) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true, 3)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
}

void DLSSScreenshot(bool includeOverlay) {
    FlowGame game(CurrentTestName());
    StartDLSSGame(game);
    SetOverlayInScreenshots(game, includeOverlay);
    ExpectACapturedFrame(TakeScreenshot(game, 1), 1, game);
    ExpectACapturedFrame(TakeScreenshot(game, 2), 2, game);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowScreenshot, NativeSwapchainServesTheRequest) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ExpectACapturedFrame(TakeScreenshot(game, 1), 1, game);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowScreenshot, DLSSFrameGenerationServesAnOverlayRequest) { DLSSScreenshot(true); }
TEST(FlowScreenshot, DLSSFrameGenerationServesAnOverlayFreeRequest) { DLSSScreenshot(false); }

void DeviceViewOfBackBuffer(bool includeOverlay) {
    FlowGame game(CurrentTestName());
    StartDLSSGame(game);
    SetOverlayInScreenshots(game, includeOverlay);
    ComPtr<ID3D12Device> device;
    ASSERT_EQ(game.RetainGameQueue()->GetDevice(IID_PPV_ARGS(&device)), S_OK);
    ComPtr<ID3D12Resource> backBuffer;
    ASSERT_EQ(game.RetainUnderlyingGameSwapchain()->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), S_OK);
    ScreenshotResult result;
    {
        DeviceViewAnswers view(backBuffer.Get(), device.Get());
        result = TakeScreenshot(game, 1);
    }
    ExpectACapturedFrame(result, 1, game);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowScreenshot, ABackBufferReachedThroughADeviceViewStillServesAnOverlayRequest) { DeviceViewOfBackBuffer(true); }
TEST(FlowScreenshot, ABackBufferReachedThroughADeviceViewStillServesAnOverlayFreeRequest) {
    DeviceViewOfBackBuffer(false);
}

TEST(FlowScreenshot, AQueueReachedThroughADeviceViewBuildsTheReadbackFromThatView) {
    FlowGame game(CurrentTestName());
    StartDLSSGame(game);
    ComPtr<ID3D12Device> device;
    const auto queue = game.RetainGameQueue();
    ASSERT_EQ(queue->GetDevice(IID_PPV_ARGS(&device)), S_OK);
    ScreenshotResult result;
    {
        DeviceViewAnswers view(queue.Get(), device.Get());
        result = TakeScreenshot(game, 1);
    }
    ExpectACapturedFrame(result, 1, game);
    ExpectNoDebugLayerErrors();
}

void NoCallbackFSRScreenshot(bool queueDeviceView) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.swapchain = SwapchainKind::kFidelityFX;
    options.fsrUi = FSRUiResource::kPlaceholder;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(80)) << game.Error();
    SetOverlayInScreenshots(game, true);
    ScreenshotResult result;
    if (queueDeviceView) {
        const auto queue = game.RetainGameQueue();
        ComPtr<ID3D12Device> device;
        ASSERT_EQ(queue->GetDevice(IID_PPV_ARGS(&device)), S_OK);
        DeviceViewAnswers view(queue.Get(), device.Get());
        result = TakeScreenshot(game, 1);
    } else {
        result = TakeScreenshot(game, 1);
    }
    ExpectACapturedFrame(result, 1, game);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowScreenshot, NoCallbackFSRServesTheRequest) { NoCallbackFSRScreenshot(false); }
TEST(FlowScreenshot, NoCallbackFSRServesTheRequestThroughAQueueDeviceView) { NoCallbackFSRScreenshot(true); }

}  // namespace
