#include <gtest/gtest.h>

#include "hook/present/device_loss_latch.h"
#include "hook/present/dxgi_shared.h"

namespace {

namespace device_loss = ce::device_loss;
using device_loss::PresentResultTransition;

// Restores the process-wide latch so tests stay independent of each other.
class DeviceLossLatchTest : public ::testing::Test {
protected:
    void SetUp() override { saved_ = DXGIShared::g_SharedState.deviceRemovedFatal.load(); }
    void TearDown() override { DXGIShared::g_SharedState.deviceRemovedFatal.store(saved_); }

private:
    bool saved_ = false;
};

}  // namespace

TEST(DeviceLossLatchPolicyTest, ADeviceLostPresentLatchesOnce) {
    for (const HRESULT lost : {DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET, DXGI_ERROR_DEVICE_HUNG}) {
        EXPECT_TRUE(device_loss::IsDeviceLostPresentResult(lost));
        EXPECT_EQ(device_loss::DecidePresentResultTransition(false, lost), PresentResultTransition::kLatch);
        EXPECT_EQ(device_loss::DecidePresentResultTransition(true, lost), PresentResultTransition::kNone);
    }
}

// HotS on DXUP, logs/20261028_092712: the D3D12 device was lost, the game went on presenting a
// D3D11 swapchain at 60 fps, and the latch suppressed every heartbeat until a false freeze dump.
TEST(DeviceLossLatchPolicyTest, ASuccessfulPresentAfterTheLossProvesItsDeviceAlive) {
    EXPECT_EQ(device_loss::DecidePresentResultTransition(true, S_OK), PresentResultTransition::kRecover);
    EXPECT_EQ(device_loss::DecidePresentResultTransition(true, DXGI_STATUS_OCCLUDED),
              PresentResultTransition::kRecover)
        << "an occluded Present still reached a live device";
    EXPECT_EQ(device_loss::DecidePresentResultTransition(false, S_OK), PresentResultTransition::kNone);
}

TEST(DeviceLossLatchPolicyTest, AFailureThatIsNotADeviceLossChangesNothing) {
    for (const HRESULT failure : {DXGI_ERROR_WAS_STILL_DRAWING, DXGI_ERROR_INVALID_CALL, E_FAIL}) {
        EXPECT_FALSE(device_loss::IsDeviceLostPresentResult(failure));
        EXPECT_EQ(device_loss::DecidePresentResultTransition(true, failure), PresentResultTransition::kNone);
        EXPECT_EQ(device_loss::DecidePresentResultTransition(false, failure), PresentResultTransition::kNone);
    }
}

TEST(DeviceLossLatchPolicyTest, TransitionLogsAreBoundedForAnAlternatingProcess) {
    EXPECT_TRUE(device_loss::ShouldLogTransition(1));
    EXPECT_TRUE(device_loss::ShouldLogTransition(8));
    EXPECT_FALSE(device_loss::ShouldLogTransition(9));
    EXPECT_FALSE(device_loss::ShouldLogTransition(63));
    EXPECT_TRUE(device_loss::ShouldLogTransition(64));
}

TEST_F(DeviceLossLatchTest, PresentResultsLatchAndClearTheSharedLatch) {
    DXGIShared::g_SharedState.deviceRemovedFatal.store(false);
    DXGIShared::NotePresentDeviceResult(DXGI_ERROR_DEVICE_REMOVED, "test Present");
    EXPECT_TRUE(DXGIShared::g_SharedState.deviceRemovedFatal.load());

    DXGIShared::NotePresentDeviceResult(DXGI_ERROR_WAS_STILL_DRAWING, "test Present");
    EXPECT_TRUE(DXGIShared::g_SharedState.deviceRemovedFatal.load()) << "no proof either way";

    DXGIShared::NotePresentDeviceResult(S_OK, "test Present");
    EXPECT_FALSE(DXGIShared::g_SharedState.deviceRemovedFatal.load());
}

// CE's own D3D12 device check latches without a Present result; the recovered game's first
// successful Present must still clear it, whichever API it presents through.
TEST_F(DeviceLossLatchTest, ALatchFromCesDeviceCheckIsClearedByASuccessfulPresent) {
    DXGIShared::g_SharedState.deviceRemovedFatal.store(true);
    DXGIShared::NotePresentDeviceResult(S_OK, "test wrapped Present");
    EXPECT_FALSE(DXGIShared::g_SharedState.deviceRemovedFatal.load());
}
