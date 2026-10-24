#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "captureengine/media/dxgi_dup_first_frame_gate.h"

// The duplication thread proves the first frame's format and holds the frame
// until the owner has adopted that format. Before the hold, the thread built
// the retained pool for the first frame while the owner recomputed the route
// for the same format: a BGRA8 pool got NV12 plane views (E_INVALIDARG) and the
// capture fell back to the RGB reservoir.

namespace {

constexpr std::chrono::milliseconds kNoDeadline{60000};  // never reached; the gate decides

}  // namespace

TEST(DuplicationFirstFrameGateTest, HeldFrameIsDeliveredOnlyAfterTheOwnerAdoptedTheFormat) {
    DuplicationFirstFrameGate gate;
    gate.Reset();
    int adoptedFormat = 0;  // owner state; the thread reads it only after delivery opens
    int formatSeenAtDelivery = -1;
    bool delivered = false;
    std::thread capture([&] {
        gate.Prove();
        delivered = gate.WaitForDelivery();
        if (delivered)
            formatSeenAtDelivery = adoptedFormat;
    });
    ASSERT_TRUE(gate.WaitForProof(kNoDeadline));
    adoptedFormat = 87;  // the owner adopts the proven format, then opens delivery
    gate.OpenDelivery();
    capture.join();
    EXPECT_TRUE(delivered);
    EXPECT_EQ(formatSeenAtDelivery, 87);
    EXPECT_TRUE(gate.FailureReason().empty());
}

TEST(DuplicationFirstFrameGateTest, StopBeforeDeliveryReleasesTheHeldFrameUndelivered) {
    DuplicationFirstFrameGate gate;
    gate.Reset();
    bool delivered = true;
    std::thread capture([&] {
        gate.Prove();
        delivered = gate.WaitForDelivery();
    });
    ASSERT_TRUE(gate.WaitForProof(kNoDeadline));
    gate.Close();  // the owner's pool prewarm failed: Stop
    capture.join();
    EXPECT_FALSE(delivered);
}

TEST(DuplicationFirstFrameGateTest, RejectedFirstFrameFailsTheStartWithItsReason) {
    DuplicationFirstFrameGate gate;
    gate.Reset();
    std::thread capture([&] { gate.Fail("first duplication frame format violates the contract"); });
    EXPECT_FALSE(gate.WaitForProof(kNoDeadline));
    capture.join();
    EXPECT_EQ(gate.FailureReason(), "first duplication frame format violates the contract");
    gate.Prove();  // a decided proof does not change
    EXPECT_FALSE(gate.WaitForProof(kNoDeadline));
}

TEST(DuplicationFirstFrameGateTest, ResetAfterProofDoesNotTurnIntoAFailure) {
    DuplicationFirstFrameGate gate;
    gate.Reset();
    gate.Prove();
    gate.Fail("access lost");  // a later duplication reset is not a start failure
    EXPECT_TRUE(gate.WaitForProof(kNoDeadline));
    EXPECT_TRUE(gate.FailureReason().empty());
}

TEST(DuplicationFirstFrameGateTest, UndecidedOrClosedStartsReportWhy) {
    DuplicationFirstFrameGate gate;
    gate.Reset();
    EXPECT_FALSE(gate.WaitForProof(std::chrono::milliseconds(0)));
    EXPECT_EQ(gate.FailureReason(), "timed out waiting for first duplication frame format proof");

    gate.Reset();
    gate.Close();
    EXPECT_FALSE(gate.WaitForProof(kNoDeadline));
    EXPECT_EQ(gate.FailureReason(), "capture thread stopped");
    EXPECT_FALSE(gate.WaitForDelivery());

    // A restart begins a new handover.
    gate.Reset();
    gate.Prove();
    EXPECT_TRUE(gate.WaitForProof(kNoDeadline));
    gate.OpenDelivery();
    EXPECT_TRUE(gate.WaitForDelivery());
}
