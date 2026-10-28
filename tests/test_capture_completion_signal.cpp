#include <gtest/gtest.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "hook/capture/capture_completion_signal.h"

namespace {

using Microsoft::WRL::ComPtr;

TEST(CaptureCompletionSignalTest, OnlyThePlainForegroundRouteHoldsTheSignalPastPresent) {
    EXPECT_EQ(ResolveCaptureCompletionSignal(true, true, false, false), CaptureCompletionSignal::kAfterPresent);
    EXPECT_EQ(ResolveCaptureCompletionSignal(false, true, false, false), CaptureCompletionSignal::kImmediate)
        << "a runtime presenter's output (XeFG with generation off) is not the game's own Present";
    EXPECT_EQ(ResolveCaptureCompletionSignal(true, false, false, false), CaptureCompletionSignal::kImmediate)
        << "unfocused D3D12 presentation keeps the Signal its focus-loss fence waits rely on";
    EXPECT_EQ(ResolveCaptureCompletionSignal(true, true, true, false), CaptureCompletionSignal::kImmediate)
        << "frame generation keeps the validated immediate Signal";
    EXPECT_EQ(ResolveCaptureCompletionSignal(true, true, false, true), CaptureCompletionSignal::kImmediate)
        << "a runtime-owned swapchain is not presented by the game's own Present";
}

class HeldCompletionSignalTest : public ::testing::Test {
protected:
    void SetUp() override {
        ComPtr<IDXGIFactory4> factory;
        ASSERT_EQ(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), S_OK);
        ComPtr<IDXGIAdapter> adapter;
        ASSERT_EQ(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), S_OK);
        ASSERT_EQ(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)), S_OK);
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ASSERT_EQ(device_->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue_)), S_OK);
        ASSERT_EQ(device_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&captureFence_)), S_OK);
        ASSERT_EQ(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&probeFence_)), S_OK);
    }

    // Queues a Signal on the probe fence and waits for it: everything queued
    // before it has executed. Bounded so a wedged queue fails the test.
    bool DrainQueue() {
        const uint64_t value = ++probeValue_;
        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!done)
            return false;
        const bool armed = SUCCEEDED(queue_->Signal(probeFence_.Get(), value)) &&
                           SUCCEEDED(probeFence_->SetEventOnCompletion(value, done));
        const bool drained = armed && WaitForSingleObject(done, 10000) == WAIT_OBJECT_0;
        CloseHandle(done);
        return drained;
    }

    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> captureFence_;
    ComPtr<ID3D12Fence> probeFence_;
    uint64_t probeValue_ = 0;
};

TEST_F(HeldCompletionSignalTest, AHeldSignalReachesTheFenceOnlyOnceReleased) {
    HeldCompletionSignal held;
    EXPECT_FALSE(held.Holding());
    held.Hold(queue_.Get(), 7);
    EXPECT_TRUE(held.Holding());
    EXPECT_EQ(held.Value(), 7u);

    ASSERT_TRUE(DrainQueue());
    EXPECT_EQ(captureFence_->GetCompletedValue(), 0u) << "holding queues nothing";

    ASSERT_EQ(held.Release(captureFence_.Get()), S_OK);
    EXPECT_FALSE(held.Holding());
    ASSERT_TRUE(DrainQueue());
    EXPECT_EQ(captureFence_->GetCompletedValue(), 7u);
}

// The Present the Signal is held for is queue work submitted after the copy:
// the released Signal is ordered behind it, exactly as a Signal queued after
// the Present would be.
TEST_F(HeldCompletionSignalTest, TheReleasedSignalFollowsWorkQueuedWhileItWasHeld) {
    ComPtr<ID3D12Fence> presentGate;
    ASSERT_EQ(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presentGate)), S_OK);

    HeldCompletionSignal held;
    held.Hold(queue_.Get(), 3);
    ASSERT_EQ(queue_->Wait(presentGate.Get(), 1), S_OK);  // the queue's next work: blocked until "presented"
    ASSERT_EQ(held.Release(captureFence_.Get()), S_OK);
    EXPECT_EQ(captureFence_->GetCompletedValue(), 0u) << "the Signal sits behind the work queued before it";

    ASSERT_EQ(presentGate->Signal(1), S_OK);
    ASSERT_TRUE(DrainQueue());
    EXPECT_EQ(captureFence_->GetCompletedValue(), 3u);
}

TEST_F(HeldCompletionSignalTest, ReleasingWithNothingHeldQueuesNoSignal) {
    HeldCompletionSignal held;
    EXPECT_EQ(held.Release(captureFence_.Get()), S_OK);
    ASSERT_TRUE(DrainQueue());
    EXPECT_EQ(captureFence_->GetCompletedValue(), 0u);

    held.Hold(queue_.Get(), 5);
    ASSERT_EQ(held.Release(captureFence_.Get()), S_OK);
    EXPECT_EQ(held.Release(captureFence_.Get()), S_OK) << "a hold is released once";
    ASSERT_TRUE(DrainQueue());
    EXPECT_EQ(captureFence_->GetCompletedValue(), 5u);
}

TEST(HeldCompletionSignalNoDeviceTest, AReleaseWithoutAFenceEndsTheHoldAndFails) {
    HeldCompletionSignal held;
    held.Hold(nullptr, 9);
    EXPECT_EQ(held.Release(nullptr), E_POINTER);
    EXPECT_FALSE(held.Holding()) << "a failed Signal means the queue or device is gone; nothing stays held";

    held.Hold(nullptr, 11);
    held.Abandon();
    EXPECT_FALSE(held.Holding());
}

}  // namespace
