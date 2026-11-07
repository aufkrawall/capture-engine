#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "mediaengine/engine/mediaengine.h"
#include "mediaengine/engine/mediaengine_handles.h"
#include "mediaengine/engine/mediaengine_instance.h"
#include "common/ipc/shared_defs.h"

extern "C" {
#include <libavutil/log.h>
}

namespace {

// Collects the log lines a pipeline's route delivers (DR-40).
struct LineSink {
    std::vector<std::string> lines;
    static void Append(void* context, const char* message) {
        static_cast<LineSink*>(context)->lines.emplace_back(message);
    }
};

MediaEngineHandle CreatePipeline(MediaEngineLogFn log = nullptr, void* context = nullptr,
                                 const wchar_t* roleHostPath = nullptr) {
    MediaEngineCreateDesc desc{};
    desc.structSize = sizeof(desc);
    desc.logCallback = log;
    desc.logContext = context;
    desc.roleHostPath = roleHostPath;
    MediaEngineHandle handle = 0;
    EXPECT_TRUE(MediaEngine_Create(&desc, &handle));
    EXPECT_NE(handle, 0u);
    return handle;
}

}  // namespace

TEST(MediaEngineHandleTest, TwoPipelinesInOneProcessAreIndependent) {
    const MediaEngineHandle first = CreatePipeline();
    const MediaEngineHandle second = CreatePipeline();
    ASSERT_NE(first, second);

    MediaEngine_SetAudioOnly(first, true);
    {
        MediaEngineLease firstLease(first);
        MediaEngineLease secondLease(second);
        ASSERT_NE(firstLease.get(), nullptr);
        ASSERT_NE(secondLease.get(), nullptr);
        EXPECT_NE(&*firstLease, &*secondLease);
        EXPECT_TRUE(firstLease->pendingAudioOnly);
        EXPECT_FALSE(secondLease->pendingAudioOnly);
    }
    MediaEngine_Destroy(first);
    MediaEngine_Destroy(second);
}

TEST(MediaEngineHandleTest, SharedStateRetargetDoesNotWaitForTheCodecLock) {
    const MediaEngineHandle handle = CreatePipeline();
    auto live = std::make_unique<SharedMemoryLayout>();
    auto detached = std::make_unique<SharedMemoryLayout>();
    MediaEngine_SetSharedMem(handle, live.get(), nullptr);
    live->runtimeState.liveFramesEncoded.store(12);
    {
        MediaEngineLease lease(handle);
        ASSERT_TRUE(lease);
        // Simulate the serialized codec call without a codec thread or timing
        // assumption. The binder must return while this lock is still held.
        std::lock_guard codecCall(lease->apiMutex);
        bool detachedOk = false;
        std::thread binder([&] { detachedOk = MediaEngine_DetachSharedMem(handle, live.get(), detached.get()); });
        binder.join();
        EXPECT_TRUE(detachedOk);
        EXPECT_EQ(detached->runtimeState.liveFramesEncoded.load(), 12u);
        live->runtimeState.liveFramesEncoded.store(1000);
        {
            auto publication = lease->recordingMemory.Publish();
            ASSERT_EQ(lease->recordingMemory.get(), detached.get());
            publication->runtimeState.liveFramesEncoded.store(42);
        }
        EXPECT_EQ(live->runtimeState.liveFramesEncoded.load(), 1000u);
    }
    MediaEngine_Destroy(handle);
    EXPECT_FALSE(MediaEngine_DetachSharedMem(handle, live.get(), detached.get()));
}

TEST(MediaEngineHandleTest, MeteredAacRepairDiagnosticsSurviveTheGlobalLibavSampler) {
    LineSink sink;
    const MediaEngineHandle handle = CreatePipeline(LineSink::Append, &sink);
    MediaEngine_SetLogCallback(nullptr);
    const int previousLevel = av_log_get_level();
    av_log_set_level(AV_LOG_WARNING);
    {
        MediaEngineLease lease(handle);
        for (int message = 0; message < 250; ++message)
            av_log(nullptr, AV_LOG_WARNING, "ordinary diagnostic %d\n", message);
        av_log(nullptr, AV_LOG_WARNING, "AAC NMR budget repair: frame=17 events=1\n");
        EXPECT_FALSE(sink.lines.empty());
        if (!sink.lines.empty())
            EXPECT_NE(sink.lines.back().find("AAC NMR budget repair: frame=17"), std::string::npos);
    }
    av_log_set_level(previousLevel);
    MediaEngine_Destroy(handle);
}

TEST(MediaEngineHandleTest, DestroyingOnePipelineLeavesTheOtherWorking) {
    const MediaEngineHandle first = CreatePipeline();
    const MediaEngineHandle second = CreatePipeline();
    MediaEngine_Destroy(first);

    {
        MediaEngineLease gone(first);
        EXPECT_EQ(gone.get(), nullptr);
    }
    MediaEngine_SetAudioOnly(second, true);
    {
        MediaEngineLease alive(second);
        ASSERT_NE(alive.get(), nullptr);
        EXPECT_TRUE(alive->pendingAudioOnly);
    }
    MediaEngine_Destroy(second);
}

TEST(MediaEngineHandleTest, InvalidAndDestroyedHandlesFailClosed) {
    EXPECT_EQ(MediaEngineLease(0).get(), nullptr);
    EXPECT_EQ(MediaEngineLease(0xFFFFFFFFFFFFFFFFull).get(), nullptr);
    EXPECT_EQ(MediaEngine_GetD3D11Device(0), nullptr);
    EXPECT_FALSE(MediaEngine_Init(0, nullptr));

    const MediaEngineHandle handle = CreatePipeline();
    MediaEngine_Destroy(handle);
    MediaEngine_Destroy(handle);  // a second destroy is refused, not a crash

    uint64_t sequence = 99;
    EXPECT_FALSE(MediaEngine_TakeInjectSourceReadToken(handle, &sequence));
    EXPECT_EQ(sequence, 0u);
    EXPECT_FALSE(MediaEngine_IsInjectSourceReadComplete(handle, 1));
    EXPECT_EQ(MediaEngine_GetD3D11Device(handle), nullptr);
    EXPECT_FALSE(MediaEngine_StopRecording(handle, true));
    EXPECT_FALSE(MediaEngine_CanRepeatLastFrame(handle));
}

TEST(MediaEngineHandleTest, CreateRefusesADescriptorOfAnotherLayout) {
    MediaEngineCreateDesc desc{};
    desc.structSize = sizeof(desc) - 1;
    MediaEngineHandle handle = 77;
    EXPECT_FALSE(MediaEngine_Create(&desc, &handle));
    EXPECT_EQ(handle, 0u);
    EXPECT_FALSE(MediaEngine_Create(nullptr, &handle));
}

TEST(MediaEngineHandleTest, AHandleStaysDeadAfterItsSlotIsReused) {
    const MediaEngineHandle stale = CreatePipeline();
    MediaEngine_Destroy(stale);
    const MediaEngineHandle fresh = CreatePipeline();
    EXPECT_NE(stale, fresh);

    EXPECT_EQ(MediaEngineLease(stale).get(), nullptr);
    MediaEngine_SetAudioOnly(stale, true);
    {
        MediaEngineLease live(fresh);
        ASSERT_NE(live.get(), nullptr);
        EXPECT_FALSE(live->pendingAudioOnly) << "a refused call on a stale handle must not reach the new pipeline";
    }
    MediaEngine_Destroy(fresh);
}

TEST(MediaEngineHandleTest, CreatePassesTheRoleHostPathToThePipeline) {
    const std::wstring host = L"C:\\Program Files\\Client\\captureengine.exe";
    const MediaEngineHandle handle = CreatePipeline(nullptr, nullptr, host.c_str());
    {
        MediaEngineLease lease(handle);
        ASSERT_NE(lease.get(), nullptr);
        EXPECT_EQ(lease->roleHostPath, host);
    }
    MediaEngine_Destroy(handle);

    const MediaEngineHandle defaulted = CreatePipeline();
    {
        MediaEngineLease lease(defaulted);
        ASSERT_NE(lease.get(), nullptr);
        EXPECT_TRUE(lease->roleHostPath.empty()) << "an empty path keeps the executable of this process";
    }
    MediaEngine_Destroy(defaulted);
}

TEST(MediaEngineHandleTest, DllLogGoesToTheRouteOfThePipelineTheThreadWorksFor) {
    LineSink sink;
    const MediaEngineHandle handle = CreatePipeline(&LineSink::Append, &sink);
    {
        MediaEngineLease lease(handle);
        ASSERT_NE(lease.get(), nullptr);
        DLL_Log("inside %d", 1);
    }
    DLL_Log("outside, no route");
    MediaEngine_Destroy(handle);
    ASSERT_EQ(sink.lines.size(), 1u);
    EXPECT_EQ(sink.lines.front(), "inside 1");
}

TEST(MediaEngineHandleTest, ARouteEndsWithItsLease) {
    LineSink sink;
    const MediaEngineHandle handle = CreatePipeline(&LineSink::Append, &sink);
    {
        MediaEngineLease lease(handle);
        EXPECT_NE(CurrentMediaEngineLogRoute().callback, nullptr);
    }
    EXPECT_EQ(CurrentMediaEngineLogRoute().callback, nullptr);
    MediaEngine_Destroy(handle);
}

TEST(MediaEngineHandleTest, ThreadsAPipelineStartsLogThroughItsRoute) {
    LineSink sink;
    const MediaEngineHandle handle = CreatePipeline(&LineSink::Append, &sink);
    {
        MediaEngineLease lease(handle);
        ASSERT_NE(lease.get(), nullptr);
        std::thread worker(WithInheritedLogRoute([] { DLL_Log("from a pipeline thread"); }));
        worker.join();
    }
    // A thread started with no pipeline in effect has no route, so its lines do not reach this pipeline.
    std::thread stranger(WithInheritedLogRoute([] { DLL_Log("from a process thread"); }));
    stranger.join();
    MediaEngine_Destroy(handle);
    ASSERT_EQ(sink.lines.size(), 1u);
    EXPECT_EQ(sink.lines.front(), "from a pipeline thread");
}

TEST(MediaEngineHandleTest, HeldPipelineSpansCallsOnTheHoldingThread) {
    const MediaEngineHandle handle = CreatePipeline();
    MediaEngineInstance* held = HoldPipeline(handle);
    ASSERT_NE(held, nullptr);
    EXPECT_EQ(HeldPipeline(handle), held);
    EXPECT_EQ(HeldPipeline(handle + 1), nullptr);
    ReleasePipelineHold(handle);
    EXPECT_EQ(HeldPipeline(handle), nullptr);
    EXPECT_EQ(HoldPipeline(0), nullptr);

    // The device lock is taken and released in two calls on the same thread.
    MediaEngine_LockD3D11(handle);
    MediaEngine_UnlockD3D11(handle);
    MediaEngine_Destroy(handle);
}

TEST(MediaEngineHandleTest, RetireHandsThePipelineBackOnlyAfterTheHoldIsReleased) {
    const MediaEngineHandle handle = CreatePipeline();
    ASSERT_NE(HoldPipeline(handle), nullptr);

    std::atomic<bool> retired{false};
    std::unique_ptr<MediaEngineInstance> instance;
    std::thread destroyer([&] {
        instance = RetireMediaEngineInstance(handle);
        retired.store(true);
    });
    // The hold is released from this thread; Retire waits for it whether or not it has started yet.
    ReleasePipelineHold(handle);
    destroyer.join();
    EXPECT_TRUE(retired.load());
    EXPECT_NE(instance, nullptr);
    EXPECT_EQ(RetireMediaEngineInstance(handle), nullptr) << "a retired handle cannot be retired twice";
    EXPECT_EQ(MediaEngineLease(handle).get(), nullptr);
}
