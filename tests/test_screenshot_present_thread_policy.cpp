#include <gtest/gtest.h>

#include "../hook/common/screenshot_hook.h"
#include "../hook/common/screenshot_worker.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// A screenshot must never make a render or present thread wait on the GPU. The
// D3D12 backbuffer copy is submitted on the game's own swapchain queue, and a
// frame-generation runtime only drains that queue once its presenter thread
// makes progress - which it cannot do while the present call it drives is
// blocked inside our hook. These tests pin the pieces that keep the wait on the
// worker: the reservation that makes a submitted copy always collectable, and
// the ownership rules that keep it from being released under in-flight work.

namespace {

std::atomic<uint32_t> g_sequence{1};

std::string ReadSource(const std::filesystem::path& relativePath) {
    std::ifstream file(std::filesystem::current_path() / relativePath, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::filesystem::path UniqueDirectory() {
    return std::filesystem::temp_directory_path() /
           (L"ce_screenshot_present_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
            std::to_wstring(g_sequence.fetch_add(1, std::memory_order_relaxed)));
}

struct HandleCloser {
    void operator()(void* handle) const {
        if (handle)
            CloseHandle(static_cast<HANDLE>(handle));
    }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

bool IsDisowned(const ScreenshotDx12Readback& readback) {
    return readback.device == nullptr && readback.fence == nullptr && readback.buffer == nullptr &&
           readback.allocator == nullptr && readback.commandList == nullptr && !readback.submitted;
}

class ScreenshotReservationTest : public testing::Test {
protected:
    void TearDown() override {
        ShutdownScreenshotWorker();
        if (!directory_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(directory_, error);
        }
    }

    // Builds the task the D3D12 producer hands over once its copy is submitted.
    ScreenshotTask MakeTask(const std::filesystem::path& partPath, uint64_t requestId,
                            const std::vector<uint8_t>& pixels) {
        const std::string eventName = "Local\\CaptureEngineScreenshotPresentTest_" +
                                      std::to_string(GetCurrentProcessId()) + "_" +
                                      std::to_string(g_sequence.fetch_add(1, std::memory_order_relaxed));
        event_.reset(CreateEventA(nullptr, FALSE, FALSE, eventName.c_str()));
        EXPECT_TRUE(event_ != nullptr);
        shared_.runtimeState.screenshotRequestId.store(requestId, std::memory_order_release);
        shared_.runtimeState.screenshotCompletedRequestId.store(0, std::memory_order_release);
        shared_.runtimeState.screenshotStatus.store(static_cast<uint32_t>(ScreenshotRequestStatus::Writing),
                                                    std::memory_order_release);

        ScreenshotTask task;
        task.sharedMemory = &shared_;
        task.requestId = requestId;
        task.partPath = partPath.wstring();
        task.readyPath = (partPath.parent_path() / L"capture.ready").wstring();
        task.header.requestId = requestId;
        task.header.pixelFormat = static_cast<uint32_t>(ScreenshotPixelFormat::BGRA8);
        task.header.colorEncoding = static_cast<uint32_t>(ScreenshotColorEncoding::SRGB);
        task.header.width = 1;
        task.header.height = 1;
        task.header.rowPitch = 4;
        task.header.payloadSize = pixels.size();
        task.header.totalSize = sizeof(ScreenshotRawHeaderV2) + pixels.size();
        task.pixels = pixels;
        EXPECT_LT(eventName.size(), sizeof(task.completionEventName));
        std::copy_n(eventName.c_str(), eventName.size(), task.completionEventName);
        return task;
    }

    SharedMemoryLayout shared_{};
    UniqueHandle event_;
    std::filesystem::path directory_;
};

}  // namespace

TEST_F(ScreenshotReservationTest, ReservationIsExclusiveSoASubmittedCopyIsAlwaysCollectable) {
    ASSERT_TRUE(ReserveScreenshotWorkerSlot());
    EXPECT_FALSE(ReserveScreenshotWorkerSlot())
        << "a second producer must be told the worker is busy before it records any GPU work";
    ReleaseScreenshotWorkerSlot();
    EXPECT_TRUE(ReserveScreenshotWorkerSlot());
    ReleaseScreenshotWorkerSlot();
}

TEST_F(ScreenshotReservationTest, ReservedTaskCompletesAndFreesTheSlotForTheNextRequest) {
    directory_ = UniqueDirectory();
    ASSERT_TRUE(std::filesystem::create_directories(directory_));
    const std::filesystem::path partPath = directory_ / L"capture.part";
    const std::filesystem::path readyPath = directory_ / L"capture.ready";
    constexpr uint64_t requestId = 41;

    ASSERT_TRUE(ReserveScreenshotWorkerSlot());
    const std::vector<uint8_t> pixels{9, 8, 7, 6};
    SubmitReservedScreenshotTask(MakeTask(partPath, requestId, pixels));
    ASSERT_EQ(WaitForSingleObject(event_.get(), 5000), WAIT_OBJECT_0);

    EXPECT_EQ(shared_.runtimeState.screenshotStatus.load(std::memory_order_acquire),
              static_cast<uint32_t>(ScreenshotRequestStatus::Succeeded));
    ASSERT_TRUE(std::filesystem::exists(readyPath));

    EXPECT_TRUE(ReserveScreenshotWorkerSlot()) << "the worker must free its slot once the task is finished";
    ReleaseScreenshotWorkerSlot();
}

TEST(ScreenshotDx12ReadbackTest, MovingHandsOverEveryReferenceExactlyOnce) {
    // Dummy addresses only: the test never releases them, it checks that exactly
    // one owner is left so a real readback can never be double-released.
    auto* device = reinterpret_cast<ID3D12Device*>(0x1000);
    auto* fence = reinterpret_cast<ID3D12Fence*>(0x2000);
    auto* buffer = reinterpret_cast<ID3D12Resource*>(0x3000);

    ScreenshotDx12Readback source;
    source.device = device;
    source.fence = fence;
    source.buffer = buffer;
    source.fenceValue = 7;
    source.bufferSize = 4096;
    source.submitted = true;

    ScreenshotDx12Readback moved = std::move(source);
    EXPECT_EQ(moved.device, device);
    EXPECT_EQ(moved.fence, fence);
    EXPECT_EQ(moved.buffer, buffer);
    EXPECT_EQ(moved.fenceValue, 7u);
    EXPECT_EQ(moved.bufferSize, 4096u);
    EXPECT_TRUE(moved.submitted);

    // Reading the moved-from object is the contract under test: exactly one
    // owner may be left, or a real readback would be released twice.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    EXPECT_TRUE(IsDisowned(source));

    moved.Disown();
    EXPECT_TRUE(IsDisowned(moved));
}

TEST(ScreenshotPresentThreadPolicyTest, TheD3D12ProducerSubmitsTheCopyWithoutWaitingForIt) {
    const std::string source = ReadSource(std::filesystem::path("hook") / "common" / "screenshot_hook.cpp");
    ASSERT_FALSE(source.empty());

    const size_t function = source.find("bool SaveDX12TextureAsScreenshotRaw(");
    ASSERT_NE(function, std::string::npos);
    const std::string body = source.substr(function);

    // The copy is recorded, submitted and signalled here...
    EXPECT_NE(body.find("queue->ExecuteCommandLists(1, lists)"), std::string::npos);
    EXPECT_NE(body.find("queue->Signal(readback.fence, readback.fenceValue)"), std::string::npos);
    EXPECT_NE(body.find("SubmitReservedScreenshotTask(std::move(task))"), std::string::npos);

    // ...and never collected here. A blocking wait on this thread froze the game
    // outright under DLSS frame generation.
    EXPECT_EQ(body.find("WaitForSingleObject"), std::string::npos)
        << "the D3D12 producer runs on a present thread and must not wait on the GPU";
    EXPECT_EQ(body.find("INFINITE"), std::string::npos);
    EXPECT_EQ(body.find("SetEventOnCompletion"), std::string::npos);
    EXPECT_EQ(body.find("->Map("), std::string::npos)
        << "mapping the readback implies the copy already completed, which only the worker may assume";
}

TEST(ScreenshotPresentThreadPolicyTest, OnlyTheWorkerWaitsAndItNeverWaitsForever) {
    const std::string worker = ReadSource(std::filesystem::path("hook") / "common" / "screenshot_worker.cpp");
    ASSERT_FALSE(worker.empty());

    EXPECT_NE(worker.find("WaitForSingleObject(fenceEvent, kReadbackWaitSliceMs)"), std::string::npos);
    EXPECT_EQ(worker.find("WaitForSingleObject(fenceEvent, INFINITE)"), std::string::npos)
        << "slicing the wait is what keeps device-removal detection and hook shutdown responsive";
    EXPECT_NE(worker.find("GetDeviceRemovedReason()"), std::string::npos);
}

namespace {

// The queue each overlay-list submit in the PostSL chunk targets.
std::string OverlaySubmitQueue(const std::string& line) {
    static const char* const kVirtualCalls[] = {"slQueue->ExecuteCommandLists(1, lists);",
                                                "scQueue->ExecuteCommandLists(1, lists);",
                                                "queue->ExecuteCommandLists(1, lists);"};
    for (const char* call : kVirtualCalls) {
        const size_t at = line.find(call);
        if (at != std::string::npos) {
            const std::string queue(call, strchr(call, '-') - call);
            // "queue->" also matches the tail of "slQueue->"/"scQueue->".
            if (at == 0 || !isalnum(static_cast<unsigned char>(line[at - 1])))
                return queue;
        }
    }
    static const char* const kFunctionCalls[] = {"selectedQueueOrigECL(", "realECL(", "origECL("};
    for (const char* call : kFunctionCalls) {
        const size_t at = line.find(call);
        if (at == std::string::npos || line.find(", 1, lists);", at) == std::string::npos)
            continue;
        if (at != 0 && isalnum(static_cast<unsigned char>(line[at - 1])))
            continue;
        const size_t open = at + strlen(call);
        return line.substr(open, line.find(',', open) - open);
    }
    return {};
}

std::string TrimAscii(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos)
        return {};
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line))
        lines.push_back(line);
    return lines;
}

}  // namespace

TEST(ScreenshotPresentThreadPolicyTest, EveryPostSLOverlaySubmitCapturesTheOverlayFreeFrameFirst) {
    const std::string source =
        ReadSource(std::filesystem::path("hook") / "apis" / "dx12_hook_postsl_render_submit.cpp");
    ASSERT_FALSE(source.empty());

    const std::vector<std::string> lines = SplitLines(source);
    size_t submits = 0;
    for (size_t index = 0; index < lines.size(); ++index) {
        const std::string queue = OverlaySubmitQueue(lines[index]);
        if (queue.empty())
            continue;
        ++submits;
        ASSERT_GT(index, 0u);
        // Overlay-free screenshots and final-output recording copies have to be
        // submitted on the same queue, immediately ahead of the overlay list.
        // Anything else lets the overlay land in an output that asked to exclude it.
        const std::string capture = TrimAscii(lines[index - 1]);
        EXPECT_EQ(capture.find("captureBeforeOverlay(" + queue + ","), 0u)
            << "unguarded overlay submit at line " << (index + 1) << ": " << TrimAscii(lines[index]);
    }
    EXPECT_GT(submits, 10u) << "the submit-path scan found suspiciously few branches";
}

TEST(ScreenshotPresentThreadPolicyTest, ProcessFrameYieldsBothScreenshotVariantsToThePostSLRoute) {
    const std::string source = ReadSource(std::filesystem::path("hook") / "apis" / "dx12_hook_process.cpp");
    ASSERT_FALSE(source.empty());

    // PostSL runs earlier in the same Present. Taking the overlay-free shot here
    // captured a backbuffer the overlay had already been drawn into.
    size_t guarded = 0;
    for (size_t at = source.find("if (screenshotRequested && !screenshotWantsOverlay"); at != std::string::npos;
         at = source.find("if (screenshotRequested && !screenshotWantsOverlay", at + 1)) {
        EXPECT_NE(source.find("!screenshotUsePostSL", at), source.npos);
        EXPECT_LT(source.find("!screenshotUsePostSL", at), source.find('\n', at));
        ++guarded;
    }
    EXPECT_EQ(guarded, 2u) << "both ProcessFrameExternal overloads must yield to PostSL";
}

TEST(ScreenshotPresentThreadPolicyTest, FinalStreamlineRecordingCoversGeneratedOutputsAndBothOverlayModes) {
    const std::string submit =
        ReadSource(std::filesystem::path("hook") / "apis" / "dx12_hook_postsl_render_submit.cpp");
    const std::string process = ReadSource(std::filesystem::path("hook") / "apis" / "dx12_hook_process.cpp");
    const std::string finalRoute =
        ReadSource(std::filesystem::path("hook") / "apis" / "dx12_hook_final_output_capture.cpp");
    const std::string startup =
        ReadSource(std::filesystem::path("hook") / "apis" / "dx12_hook_fg_startup.cpp");
    ASSERT_FALSE(submit.empty());
    ASSERT_FALSE(process.empty());
    ASSERT_FALSE(finalRoute.empty());
    ASSERT_FALSE(startup.empty());

    EXPECT_NE(submit.find("if (!finalOutputCapture.includeOverlay &&"), std::string::npos);
    EXPECT_NE(submit.find("if (finalOutputCapture.includeOverlay &&"), std::string::npos);
    EXPECT_EQ(submit.find("rendered && finalOutputCapture.includeOverlay"), std::string::npos)
        << "an output with no drawable overlay content still has to be recorded";
    EXPECT_EQ(submit.find("IsCurrentFrameReal"), std::string::npos)
        << "generated Streamline outputs are the frames this route exists to retain";
    EXPECT_NE(finalRoute.find("IsPostSLFinalOutputPresentCallback"), std::string::npos);
    const size_t exactHandoff = startup.find("bool DX12_TryRenderExactPostSLBeforeStartupHandoffPresent");
    const size_t retainedService = startup.find("bool DX12_TryInvokePostSLStartupActivationCallback(");
    ASSERT_NE(exactHandoff, std::string::npos);
    ASSERT_NE(retainedService, std::string::npos);
    EXPECT_NE(startup.find("InvokePostSLCallbackForFinalOutputPresent", exactHandoff), std::string::npos)
        << "the explicit draw immediately before a real handoff Present is a genuine output";
    EXPECT_EQ(startup.find("InvokePostSLCallbackForFinalOutputPresent", retainedService), std::string::npos)
        << "retained startup/warmup service callbacks must not manufacture recording frames";

    size_t routedBaseCaptures = 0;
    for (size_t at = process.find("!DX12_ShouldUseStreamlineFinalOutputCapture()"); at != std::string::npos;
         at = process.find("!DX12_ShouldUseStreamlineFinalOutputCapture()", at + 1)) {
        ++routedBaseCaptures;
    }
    EXPECT_EQ(routedBaseCaptures, 2u) << "both ProcessFrameExternal overloads must yield base capture to PostSL";
}
