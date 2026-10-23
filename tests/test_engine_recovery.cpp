// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// The engine's recording recovery run (libcengine 11, part 3): started at READY on a worker thread, its repaired
// files published as CE_EVENT_RECORDING_RECOVERED, canceled at shutdown.
#include <gtest/gtest.h>

#include "engine_runtime_harness.h"

#include <atomic>
#include <string>
#include <thread>

namespace {
using namespace engine_runtime_test;

TEST(EngineRecoveryTest, ARepairedRecordingIsAnnouncedAfterReady) {
    Harness harness;
    std::atomic<int> calls{0};
    std::atomic<bool> sawCancelFlag{false};
    harness.recover = [&](const volatile long* cancel) {
        ++calls;
        sawCancelFlag = cancel != nullptr && *cancel == 0;
        return std::vector<RecoveredRecordingInfo>{{"C:\\rec\\capture_recovered_1.mkv", 1500000}};
    };
    std::unique_ptr<Runtime> runtime = harness.Start();
    ASSERT_TRUE(runtime);
    std::string path;
    std::string message;
    const std::optional<SeenEvent> recovered = WaitFor(*runtime, [&](const ce_event_t& e) {
        if (e.type != CE_EVENT_RECORDING_RECOVERED)
            return false;
        path = e.data.finalized.output_path ? e.data.finalized.output_path : "";
        message = e.message ? e.message : "";
        return true;
    });
    ASSERT_TRUE(recovered.has_value());
    if (!recovered)
        return;
    EXPECT_EQ(recovered->status, CE_OK);
    EXPECT_EQ(recovered->request, 0u);
    EXPECT_EQ(path, "C:\\rec\\capture_recovered_1.mkv");
    EXPECT_NE(message.find("1500 ms"), std::string::npos);
    EXPECT_EQ(calls.load(), 1);
    EXPECT_TRUE(sawCancelFlag.load());
    runtime->RequestShutdown();
    EXPECT_TRUE(runtime->WaitStopped(kWaitMs));
}

TEST(EngineRecoveryTest, ShutdownCancelsARunningPass) {
    Harness harness;
    HANDLE started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> sawCancel{false};
    harness.recover = [&](const volatile long* cancel) {
        SetEvent(started);
        // A long copy: it only returns once the engine asks it to stop.
        while (*cancel == 0)
            std::this_thread::yield();
        sawCancel = true;
        return std::vector<RecoveredRecordingInfo>{};
    };
    std::unique_ptr<Runtime> runtime = harness.Start();
    ASSERT_TRUE(runtime);
    ASSERT_EQ(WaitForSingleObject(started, kWaitMs), WAIT_OBJECT_0);
    runtime->RequestShutdown();
    EXPECT_TRUE(runtime->WaitStopped(kWaitMs));
    EXPECT_TRUE(sawCancel.load());
    CloseHandle(started);
}

TEST(EngineRecoveryTest, NoRecoveryMeansNoEventAndNoThread) {
    Harness harness;  // recover stays null, as in every engine test that does not exercise it
    std::unique_ptr<Runtime> runtime = harness.Start();
    ASSERT_TRUE(runtime);
    std::vector<SeenEvent> seen;
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) {
        return e.type == CE_EVENT_RUNTIME_STATE && e.data.runtime.state == CE_RUNTIME_READY;
    }, &seen));
    runtime->RequestShutdown();
    EXPECT_TRUE(runtime->WaitStopped(kWaitMs));
    for (const SeenEvent& event : seen)
        EXPECT_NE(event.type, CE_EVENT_RECORDING_RECOVERED);
}

}  // namespace
