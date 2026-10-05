#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "hook/d3d12/overlay_draw_transaction.h"

namespace {
using ce::dx12::DrawFailure;
using Flow = ProcessFrameFlow;
enum Event { Select, Allocator, ListReset, Prepare, Acquire, Refresh, Record, Close, Submit,
             DeviceBefore, Dispatch, DeviceAfter, RetireTargets, Release, Capture };
struct Buffer {
    std::vector<Event>& events;
    int references = 1, releases = 0;
    void Release() { --references; ++releases; events.push_back(::Release); }
};
struct State { bool overlayInit = true, syncInit = true; };
struct Operations {
    std::vector<Event> events;
    Buffer buffer{events};
    bool commands = true, swapchain = true, nullBuffer = false, failureReturnsBuffer = false;
    bool deviceLostBefore = false, deviceLostAfter = false, observedDeviceLost = false;
    int32_t allocatorResult = 0, listResult = 0, acquireResult = 0, closeResult = 0;
    Flow selectFlow = Flow::kContinue, allocatorFlow = Flow::kContinue;
    Flow prepareFlow = Flow::kContinue, recordFlow = Flow::kContinue, submitFlow = Flow::kContinue;
    std::vector<DrawFailure> failures;
    Flow SelectCommands() { events.push_back(Select); return selectFlow; }
    bool HasCommands() const { return commands; }
    Flow ResetAllocator() { events.push_back(Allocator); return allocatorFlow; }
    int32_t AllocatorResetResult() const { return allocatorResult; }
    void ResetCommandList() { events.push_back(ListReset); }
    int32_t CommandListResetResult() const { return listResult; }
    Flow PrepareRecording() { events.push_back(Prepare); return prepareFlow; }
    bool HasSwapchain() const { return swapchain; }
    ce::dx12::BackBufferAcquisition<Buffer> AcquireBackBuffer() {
        events.push_back(Acquire);
        Buffer* result = !nullBuffer && (acquireResult >= 0 || failureReturnsBuffer) ? &buffer : nullptr;
        if (result) ++result->references;
        return {acquireResult, result};
    }
    void RetireRenderTargets() { events.push_back(RetireTargets); }
    void RefreshRenderTarget(Buffer* acquired) { EXPECT_EQ(acquired, &buffer); events.push_back(Refresh); }
    Flow RecordCommands() {
        EXPECT_EQ(buffer.references, 2); events.push_back(Record); return recordFlow;
    }
    int32_t CloseCommands() { EXPECT_EQ(buffer.references, 2); events.push_back(Close); return closeResult; }
    Flow SubmitCommands() {
        EXPECT_EQ(buffer.references, 2);
        events.push_back(Submit);
        events.push_back(DeviceBefore);
        if (deviceLostBefore) { observedDeviceLost = true; return Flow::kOverlayDone; }
        events.push_back(Dispatch);
        events.push_back(DeviceAfter);
        observedDeviceLost = deviceLostAfter;
        return submitFlow;
    }
    void ReportFailure(DrawFailure failure, int32_t) { failures.push_back(failure); }
    bool Saw(Event event) const { for (auto value : events) if (value == event) return true; return false; }
};
}  // namespace

TEST(DX12DrawTransactionTest, SuccessfulDrawNeverEntersRecoveryOrChangesInitializationAndReleasesBeforeCapture) {
    Operations operations;
    State state;
    {
        ce::dx12::DrawBackBuffer<Buffer> backBuffer;
        EXPECT_EQ(ce::dx12::ExecuteOverlayDraw(operations, state, backBuffer), Flow::kContinue);
        EXPECT_TRUE(state.overlayInit);
        EXPECT_TRUE(state.syncInit);
        EXPECT_TRUE(operations.failures.empty());
        EXPECT_FALSE(operations.Saw(RetireTargets));
        EXPECT_EQ(backBuffer.Borrow(), nullptr);
        EXPECT_EQ(operations.buffer.references, 1);
        EXPECT_EQ(operations.buffer.releases, 1);
        operations.events.push_back(Capture);
    }
    EXPECT_EQ(operations.buffer.releases, 1);
    EXPECT_EQ(operations.events, (std::vector<Event>{Select, Allocator, ListReset, Prepare, Acquire,
        Refresh, Record, Close, Submit, DeviceBefore, Dispatch, DeviceAfter, Release, Capture}));
}

TEST(DX12DrawTransactionTest, ResetFailuresRecoverOnlySynchronizationWithoutAcquiringOrSubmitting) {
    for (bool allocatorFailure : {true, false}) {
        Operations operations;
        State state;
        operations.allocatorResult = allocatorFailure ? -1 : 0;
        operations.listResult = allocatorFailure ? 0 : -1;
        ce::dx12::DrawBackBuffer<Buffer> backBuffer;
        EXPECT_EQ(ce::dx12::ExecuteOverlayDraw(operations, state, backBuffer), Flow::kContinue);
        EXPECT_TRUE(state.overlayInit);
        EXPECT_FALSE(state.syncInit);
        EXPECT_EQ(operations.failures, (std::vector<DrawFailure>{allocatorFailure ?
            DrawFailure::AllocatorReset : DrawFailure::CommandListReset}));
        EXPECT_EQ(operations.Saw(ListReset), !allocatorFailure);
        EXPECT_FALSE(operations.Saw(Acquire));
        EXPECT_FALSE(operations.Saw(Submit));
        EXPECT_EQ(operations.buffer.releases, 0);
    }
}

TEST(DX12DrawTransactionTest, MissingCommandsBusyAllocatorAndPrimingSkipCannotEnterRecoveryOrTouchBackbuffers) {
    for (int scenario = 0; scenario < 4; ++scenario) {
        Operations operations;
        State state;
        if (scenario == 0) operations.commands = false;
        if (scenario == 1) operations.selectFlow = Flow::kOverlayDone;
        if (scenario == 2) operations.allocatorFlow = Flow::kOverlayDone;
        if (scenario == 3) operations.prepareFlow = Flow::kOverlayDone;
        ce::dx12::DrawBackBuffer<Buffer> backBuffer;
        const auto result = ce::dx12::ExecuteOverlayDraw(operations, state, backBuffer);
        EXPECT_EQ(result, scenario == 0 ? Flow::kContinue : Flow::kOverlayDone);
        EXPECT_TRUE(state.overlayInit);
        EXPECT_TRUE(state.syncInit);
        EXPECT_FALSE(operations.Saw(Acquire));
        EXPECT_FALSE(operations.Saw(Submit));
        EXPECT_FALSE(operations.Saw(RetireTargets));
        EXPECT_EQ(operations.buffer.releases, 0);
    }
}

TEST(DX12DrawTransactionTest, MissingInterfacePreservesStateAndGetBufferFailureRetiresOnlyRenderTargets) {
    for (int scenario = 0; scenario < 4; ++scenario) {
        Operations operations;
        State state;
        operations.swapchain = scenario != 0;
        operations.acquireResult = scenario == 1 || scenario == 3 ? -1 : 0;
        operations.nullBuffer = scenario == 2;
        operations.failureReturnsBuffer = scenario == 3;
        {
            ce::dx12::DrawBackBuffer<Buffer> backBuffer;
            EXPECT_EQ(ce::dx12::ExecuteOverlayDraw(operations, state, backBuffer), Flow::kContinue);
            EXPECT_EQ(state.overlayInit, scenario == 0);
            EXPECT_TRUE(state.syncInit);
            EXPECT_EQ(operations.Saw(RetireTargets), scenario != 0);
            EXPECT_FALSE(operations.Saw(Record));
            EXPECT_FALSE(operations.Saw(Submit));
            EXPECT_EQ(backBuffer.Borrow(), nullptr);
            EXPECT_EQ(operations.buffer.references, 1);
        }
        EXPECT_EQ(operations.failures, (std::vector<DrawFailure>{scenario == 0 ?
            DrawFailure::MissingSwapchain : DrawFailure::GetBuffer}));
        EXPECT_EQ(operations.buffer.releases, scenario == 3 ? 1 : 0);
    }
}

TEST(DX12DrawTransactionTest, CloseFailureRecoversSynchronizationAndUsesTheNormalReleasePointWithoutSubmission) {
    Operations operations;
    State state;
    operations.closeResult = -1;
    {
        ce::dx12::DrawBackBuffer<Buffer> backBuffer;
        EXPECT_EQ(ce::dx12::ExecuteOverlayDraw(operations, state, backBuffer), Flow::kContinue);
        EXPECT_TRUE(state.overlayInit);
        EXPECT_FALSE(state.syncInit);
        EXPECT_EQ(operations.failures, (std::vector<DrawFailure>{DrawFailure::Close}));
        EXPECT_FALSE(operations.Saw(Submit));
        EXPECT_EQ(operations.buffer.releases, 1);
        EXPECT_EQ(backBuffer.Borrow(), nullptr);
    }
    EXPECT_EQ(operations.buffer.releases, 1);
}

TEST(DX12DrawTransactionTest, RecordingOrSubmissionEarlyExitKeepsTheDestructorBackstopAndCannotDoubleRelease) {
    for (int scenario = 0; scenario < 3; ++scenario) {
        Operations operations;
        State state;
        if (scenario == 0) operations.recordFlow = Flow::kReturn;
        if (scenario == 1) operations.submitFlow = Flow::kOverlayDone;
        if (scenario == 2) operations.deviceLostBefore = true;
        {
            ce::dx12::DrawBackBuffer<Buffer> backBuffer;
            EXPECT_EQ(ce::dx12::ExecuteOverlayDraw(operations, state, backBuffer),
                scenario == 0 ? Flow::kReturn : Flow::kOverlayDone);
            EXPECT_EQ(operations.buffer.references, 2);
            EXPECT_EQ(operations.buffer.releases, 0);
            EXPECT_TRUE(state.overlayInit);
            EXPECT_TRUE(state.syncInit);
            EXPECT_TRUE(operations.failures.empty());
            EXPECT_EQ(operations.Saw(Close), scenario != 0);
            EXPECT_EQ(operations.Saw(Dispatch), scenario == 1);
            EXPECT_EQ(operations.observedDeviceLost, scenario == 2);
        }
        EXPECT_EQ(operations.buffer.references, 1);
        EXPECT_EQ(operations.buffer.releases, 1);
    }
}

TEST(DX12DrawTransactionTest, DeviceLossObservedAfterDispatchStillRetiresTheAcquiredBufferExactlyOnce) {
    Operations operations;
    State state;
    operations.deviceLostAfter = true;
    {
        ce::dx12::DrawBackBuffer<Buffer> backBuffer;
        EXPECT_EQ(ce::dx12::ExecuteOverlayDraw(operations, state, backBuffer), Flow::kContinue);
        EXPECT_TRUE(operations.observedDeviceLost);
        EXPECT_TRUE(operations.Saw(Dispatch));
        EXPECT_EQ(operations.buffer.references, 1);
        EXPECT_EQ(operations.buffer.releases, 1);
    }
    EXPECT_EQ(operations.buffer.releases, 1);
}

TEST(DX12DrawTransactionTest, EmptyOrExplicitlyRetiredLeaseHasNoDestructorSideEffects) {
    Operations operations;
    {
        ce::dx12::DrawBackBuffer<Buffer> backBuffer;
        backBuffer.Release();
        EXPECT_EQ(operations.buffer.releases, 0);
        ++operations.buffer.references;
        backBuffer.Adopt(&operations.buffer);
        backBuffer.Release();
        backBuffer.Release();
    }
    EXPECT_EQ(operations.buffer.references, 1);
    EXPECT_EQ(operations.buffer.releases, 1);
}
