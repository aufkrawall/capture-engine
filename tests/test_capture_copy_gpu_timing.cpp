#include <gtest/gtest.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "hook/capture/capture_copy_gpu_timing.h"

namespace {

using Microsoft::WRL::ComPtr;

TEST(CaptureCopyGpuTimingTest, GpuTicksMapOntoTheCounterAtEachClocksOwnRate) {
    GpuClockCalibration calibration;
    calibration.gpuTicks = 5'000'000;
    calibration.qpcTicks = 70'000;
    calibration.gpuFrequency = 1'000'000'000;  // 1 ns ticks
    calibration.qpcFrequency = 10'000'000;     // 100 ns ticks
    EXPECT_EQ(MapGpuTicksToQpc(calibration, 5'000'000), 70'000);
    EXPECT_EQ(MapGpuTicksToQpc(calibration, 6'000'000), 80'000) << "1 ms of GPU ticks is 1 ms of counter";
    EXPECT_EQ(MapGpuTicksToQpc(calibration, 4'500'000), 65'000) << "a timestamp before the calibration point";
    calibration.gpuFrequency = 0;
    EXPECT_EQ(MapGpuTicksToQpc(calibration, 6'000'000), 0) << "no calibration, no time";
}

class CaptureCopyGpuTimerTest : public ::testing::Test {
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
        ASSERT_EQ(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), S_OK);
        ASSERT_EQ(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_)), S_OK);
        ASSERT_EQ(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
                                             IID_PPV_ARGS(&list_)),
                  S_OK);
        ASSERT_EQ(list_->Close(), S_OK);
        ASSERT_TRUE(timer_.Initialize(device_.Get(), kSlots));
    }

    static int64_t Qpc() {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }

    // The capture's shape: the timer brackets the copy inside its own list,
    // the list runs on the queue, the timer learns the fence value. The fence
    // is signalled only when `signal` is set (a held Signal is not yet queued).
    void SubmitTimedCopy(uint32_t slot, uint64_t fenceValue, bool signal) {
        ASSERT_EQ(allocator_->Reset(), S_OK);
        ASSERT_EQ(list_->Reset(allocator_.Get(), nullptr), S_OK);
        ASSERT_TRUE(timer_.BeginCopy(list_.Get(), slot));
        timer_.EndCopy(list_.Get(), slot);
        ASSERT_EQ(list_->Close(), S_OK);
        ID3D12CommandList* lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        timer_.Submitted(queue_.Get(), slot, fenceValue, Qpc());
        if (signal) {
            ASSERT_EQ(queue_->Signal(fence_.Get(), fenceValue), S_OK);
        }
    }

    // Bounded so a wedged queue fails the test.
    bool WaitForFence(uint64_t value) {
        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!done)
            return false;
        const bool reached = SUCCEEDED(fence_->SetEventOnCompletion(value, done)) &&
                             WaitForSingleObject(done, 10000) == WAIT_OBJECT_0;
        CloseHandle(done);
        return reached;
    }

    static constexpr uint32_t kSlots = 3;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    CaptureCopyGpuTimer timer_;
    CaptureCopyGpuSample samples_[4];
};

// The GPU's timestamps come back on the counter the Present probe uses, inside
// the window in which the copy can have run.
TEST_F(CaptureCopyGpuTimerTest, ACopyIsTimedOnTheCounterOnceItsFenceCompleted) {
    const int64_t beforeSubmit = Qpc();
    SubmitTimedCopy(1, 1, false);
    CapturePresentDownCall present;
    present.entryQpc = Qpc();
    present.exitQpc = present.entryQpc + 10;
    present.slow = true;
    present.slowOrdinal = 7;
    timer_.NotePresent(&present);
    EXPECT_EQ(timer_.Harvest(fence_->GetCompletedValue(), samples_, 4), 0u)
        << "nothing is read before the copy's completion Signal ran";

    ASSERT_EQ(queue_->Signal(fence_.Get(), 1), S_OK);  // the held Signal, after the Present
    ASSERT_TRUE(WaitForFence(1));
    const int64_t afterCompletion = Qpc();
    ASSERT_EQ(timer_.Harvest(fence_->GetCompletedValue(), samples_, 4), 1u);
    const CaptureCopyGpuSample& sample = samples_[0];
    EXPECT_TRUE(sample.presentNoted);
    ASSERT_TRUE(sample.timestampsValid);
    EXPECT_LE(sample.copyStartQpc, sample.copyEndQpc);
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const int64_t slack = frequency.QuadPart / 500;  // 2 ms of calibration and timer granularity
    EXPECT_GE(sample.copyStartQpc, beforeSubmit - slack);
    EXPECT_LE(sample.copyEndQpc, afterCompletion + slack);
    EXPECT_EQ(sample.present.slowOrdinal, 7u);
    EXPECT_TRUE(sample.present.slow);
    EXPECT_EQ(timer_.Harvest(fence_->GetCompletedValue(), samples_, 4), 0u) << "a copy is handed over once";
}

// A copy whose Present never reached the probe must not be paired with the
// next down-call: that would time a later frame against it.
TEST_F(CaptureCopyGpuTimerTest, ACopyWithoutItsPresentIsHandedOverUnpaired) {
    SubmitTimedCopy(0, 1, true);
    ASSERT_TRUE(WaitForFence(1));
    EXPECT_EQ(timer_.Harvest(1, samples_, 4), 0u) << "the pending copy waits for its Present";

    timer_.NotePresent(nullptr);  // a down-call that followed no copy
    CapturePresentDownCall later;
    later.entryQpc = Qpc();
    later.exitQpc = later.entryQpc;
    timer_.NotePresent(&later);  // nothing pending any more: ignored
    ASSERT_EQ(timer_.Harvest(1, samples_, 4), 1u);
    EXPECT_FALSE(samples_[0].presentNoted);
    EXPECT_TRUE(samples_[0].timestampsValid);
    EXPECT_EQ(samples_[0].present.entryQpc, 0) << "the later call's times were not attached";
}

TEST_F(CaptureCopyGpuTimerTest, HarvestStopsAtCapacityAndKeepsTheRest) {
    for (uint32_t slot = 0; slot < kSlots; ++slot) {
        SubmitTimedCopy(slot, slot + 1, true);
        CapturePresentDownCall present;
        present.entryQpc = Qpc();
        present.exitQpc = present.entryQpc;
        timer_.NotePresent(&present);
    }
    ASSERT_TRUE(WaitForFence(kSlots));
    EXPECT_EQ(timer_.Harvest(kSlots, samples_, 2), 2u);
    EXPECT_EQ(timer_.Harvest(kSlots, samples_, 4), 1u);
    EXPECT_EQ(timer_.Harvest(kSlots, samples_, 4), 0u);
}

// A new generation starts its fence at zero: records of the old one must not
// survive into it, and its device objects must outlive copies still in flight.
TEST_F(CaptureCopyGpuTimerTest, RetiringHandsTheDeviceObjectsOverAndDropsTheRecords) {
    SubmitTimedCopy(2, 1, true);
    ASSERT_TRUE(WaitForFence(1));
    CaptureCopyGpuTimer::Resources retired = timer_.Retire();
    EXPECT_NE(retired.queryHeap, nullptr);
    EXPECT_NE(retired.readback, nullptr);
    EXPECT_FALSE(timer_.Enabled());
    EXPECT_FALSE(timer_.BeginCopy(list_.Get(), 0)) << "a retired timer records nothing";
    EXPECT_EQ(timer_.Harvest(1, samples_, 4), 0u);

    ASSERT_TRUE(timer_.Initialize(device_.Get(), kSlots));
    EXPECT_EQ(timer_.Harvest(1, samples_, 4), 0u) << "the new generation starts empty";
}

TEST(CaptureCopyGpuTimerNoDeviceTest, WithoutADeviceTheTimerStaysOff) {
    CaptureCopyGpuTimer timer;
    EXPECT_FALSE(timer.Initialize(nullptr, 4));
    EXPECT_FALSE(timer.Enabled());
    EXPECT_FALSE(timer.BeginCopy(nullptr, 0));
    CaptureCopyGpuSample sample;
    EXPECT_EQ(timer.Harvest(10, &sample, 1), 0u);
    timer.Abandon();
}

}  // namespace
