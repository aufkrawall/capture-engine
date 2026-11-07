// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "common/capture/recording_memory.h"
#include "common/ipc/shared_defs.h"

#include <gtest/gtest.h>
#include <future>
#include <memory>
#include <thread>
#include <type_traits>

TEST(RecordingMemoryTest, ReadersCannotPublishThroughAnEscapedPointer) {
    static_assert(std::is_same_v<decltype(std::declval<ce::RecordingMemory>().get()), const SharedMemoryLayout*>);
}

TEST(RecordingMemoryTest, LatePublicationsCannotOverwriteTheNextRecording) {
    auto live = std::make_unique<SharedMemoryLayout>();
    auto detached = std::make_unique<SharedMemoryLayout>();
    ce::RecordingMemory memory;
    EXPECT_EQ(memory.Retarget(live.get(), nullptr), 1u);
    {
        auto publication = memory.Publish();
        publication->runtimeState.liveFramesEncoded.store(12);
    }
    EXPECT_EQ(memory.Retarget(detached.get(), nullptr), 2u);
    live->runtimeState.liveFramesEncoded.store(1000);
    std::thread lateProducer([&] {
        for (uint32_t count = 0; count < 500; ++count) {
            auto publication = memory.Publish();
            publication->runtimeState.liveFramesEncoded.store(count);
        }
    });
    lateProducer.join();
    EXPECT_EQ(live->runtimeState.liveFramesEncoded.load(), 1000u);
    EXPECT_EQ(detached->runtimeState.liveFramesEncoded.load(), 499u);
    memory.InitializeIfEmpty(live.get());
    EXPECT_EQ(memory.get(), detached.get());
    EXPECT_EQ(memory.Retarget(detached.get(), nullptr), 0u);
}

TEST(RecordingMemoryTest, RetargetCompletesOnlyAfterTheOldPublicationEnds) {
    auto live = std::make_unique<SharedMemoryLayout>();
    auto detached = std::make_unique<SharedMemoryLayout>();
    ce::RecordingMemory memory;
    memory.Retarget(live.get(), nullptr);
    std::promise<void> started;
    std::atomic<bool> oldPublicationEnded{false};
    bool safeAtReturn = false;
    std::thread detacher;
    {
        auto oldPublication = memory.Publish();
        detacher = std::thread([&] {
            started.set_value();
            memory.Detach(*live, *detached);
            safeAtReturn = oldPublicationEnded.load();
        });
        started.get_future().wait();
        oldPublication->runtimeState.liveFramesEncoded.store(42);
        oldPublicationEnded.store(true);
    }
    detacher.join();
    EXPECT_TRUE(safeAtReturn);
    EXPECT_EQ(live->runtimeState.liveFramesEncoded.load(), 42u);
    EXPECT_EQ(detached->runtimeState.liveFramesEncoded.load(), 42u);
    auto nextPublication = memory.Publish();
    EXPECT_EQ(memory.get(), detached.get());
}

TEST(RecordingMemoryTest, DetachedSnapshotRefreshesStaleSeedsAndIsIdempotent) {
    auto live = std::make_unique<SharedMemoryLayout>();
    auto detached = std::make_unique<SharedMemoryLayout>();
    ce::RecordingMemory memory;
    memory.Retarget(live.get(), nullptr);
    detached->runtimeState.liveFramesEncoded.store(1);
    {
        auto publication = memory.Publish();
        publication->runtimeState.liveFramesEncoded.store(42);
    }
    EXPECT_EQ(memory.Detach(*live, *detached), 2u);
    EXPECT_EQ(detached->runtimeState.liveFramesEncoded.load(), 42u);
    live->runtimeState.liveFramesEncoded.store(1000);
    EXPECT_EQ(memory.Detach(*live, *detached), 0u);
    EXPECT_EQ(detached->runtimeState.liveFramesEncoded.load(), 42u);
}

TEST(RecordingMemoryTest, EarlyAbortCopiesItsFallbackBeforeAnyEngineBindingExists) {
    auto live = std::make_unique<SharedMemoryLayout>();
    auto detached = std::make_unique<SharedMemoryLayout>();
    live->runtimeState.liveFramesEncoded.store(7);
    ce::RecordingMemory memory;
    EXPECT_EQ(memory.Detach(*live, *detached), 1u);
    EXPECT_EQ(detached->runtimeState.liveFramesEncoded.load(), 7u);
    EXPECT_EQ(memory.get(), detached.get());
}
