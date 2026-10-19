#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>

#include <windows.h>

#include "captureengine/media/status_overlay_sync.h"
#include "common/ipc/shared_defs.h"

// The media half of the status-dark protocol decides, per recording start, whether to wait for the
// controller-side overlay. These drive the real event handshake through the module interface with an
// isolated controller PID, and decide the path taken from the returned outcome, never from elapsed
// time: a "no consumer" start that waited would be reported as kTimedOut or hang the test.

namespace {

using ce::status_overlay::DarkHandshake;

// A PID no live process has, unique per test, so the events never collide with a real overlay or with
// another test (the names are keyed by the controller PID only).
uint32_t IsolatedControllerPid(uint32_t salt) {
    return 0xFFE00000u | salt;
}

// The controller-side status consumer's endpoints, as PseudoOverlay::StartStatusSyncWatcher creates them.
class ConsumerEndpoints {
public:
    explicit ConsumerEndpoints(uint32_t pid, bool createSync = true, bool createAck = true) {
        wchar_t name[64]{};
        if (createSync) {
            GenerateStatusOverlaySyncEventName(name, 64, pid);
            sync_ = CreateEventW(nullptr, FALSE, FALSE, name);
        }
        if (createAck) {
            GenerateStatusOverlayDarkAckEventName(name, 64, pid);
            ack_ = CreateEventW(nullptr, FALSE, FALSE, name);
        }
    }
    ~ConsumerEndpoints() {
        if (sync_)
            CloseHandle(sync_);
        if (ack_)
            CloseHandle(ack_);
    }
    ConsumerEndpoints(const ConsumerEndpoints&) = delete;
    ConsumerEndpoints& operator=(const ConsumerEndpoints&) = delete;
    HANDLE Sync() const {
        return sync_;
    }
    HANDLE Ack() const {
        return ack_;
    }

private:
    HANDLE sync_ = nullptr;
    HANDLE ack_ = nullptr;
};

// A bound far above any real wait: if the code under test waits where it must not, the outcome is not
// the expected one (or the test hangs), so the assertion cannot pass by luck of timing.
constexpr uint32_t kNeverReachedTimeoutMs = 600000;

}  // namespace

TEST(StatusOverlayDarkTest, WithoutAnAuthenticatedControllerThereIsNoConsumerAndNoWait) {
    ce::status_overlay::SetControllerPid(0);
    CaptureState state;
    EXPECT_EQ(ce::status_overlay::RequestDarkForCapture(state, "test", kNeverReachedTimeoutMs),
              DarkHandshake::kNoConsumer);
    EXPECT_TRUE(state.HasRuntimeFlag(kCaptureRuntimeFlagStatusOverlayDarkForCapture))
        << "the request still reaches the best-effort inject overlay through the shared flag";
}

TEST(StatusOverlayDarkTest, ControllerWithoutAnOverlayPublishesNoEndpointsSoNobodyWaits) {
    // The desktop overlay feature is off or the overlay was never created: no event pair exists for
    // this controller PID. This is the library case without CE_FEATURE_DESKTOP_OVERLAY.
    ce::status_overlay::SetControllerPid(IsolatedControllerPid(1));
    CaptureState state;
    EXPECT_EQ(ce::status_overlay::RequestDarkForCapture(state, "test", kNeverReachedTimeoutMs),
              DarkHandshake::kNoConsumer);
    ce::status_overlay::SetControllerPid(0);
}

TEST(StatusOverlayDarkTest, AHalfPublishedEndpointPairIsNotAConsumer) {
    const uint32_t pid = IsolatedControllerPid(2);
    ce::status_overlay::SetControllerPid(pid);
    CaptureState state;
    {
        const ConsumerEndpoints syncOnly(pid, /*createSync=*/true, /*createAck=*/false);
        ASSERT_NE(syncOnly.Sync(), nullptr);
        EXPECT_EQ(ce::status_overlay::RequestDarkForCapture(state, "test", kNeverReachedTimeoutMs),
                  DarkHandshake::kNoConsumer);
    }
    {
        const ConsumerEndpoints ackOnly(pid, /*createSync=*/false, /*createAck=*/true);
        ASSERT_NE(ackOnly.Ack(), nullptr);
        EXPECT_EQ(ce::status_overlay::RequestDarkForCapture(state, "test", kNeverReachedTimeoutMs),
                  DarkHandshake::kNoConsumer);
    }
    ce::status_overlay::SetControllerPid(0);
}

TEST(StatusOverlayDarkTest, ALiveConsumerThatAnswersEndsTheHandshakeAcknowledged) {
    const uint32_t pid = IsolatedControllerPid(3);
    ce::status_overlay::SetControllerPid(pid);
    const ConsumerEndpoints consumer(pid);
    ASSERT_NE(consumer.Sync(), nullptr);
    ASSERT_NE(consumer.Ack(), nullptr);

    // The consumer's watcher: it blocks on the sync event (no sleeps, no polling) and answers once.
    std::atomic<bool> woke{false};
    std::thread watcher([&]() {
        if (WaitForSingleObject(consumer.Sync(), INFINITE) == WAIT_OBJECT_0) {
            woke.store(true, std::memory_order_release);
            SetEvent(consumer.Ack());
        }
    });
    CaptureState state;
    EXPECT_EQ(ce::status_overlay::RequestDarkForCapture(state, "test", kNeverReachedTimeoutMs),
              DarkHandshake::kAcknowledged);
    watcher.join();
    EXPECT_TRUE(woke.load(std::memory_order_acquire)) << "the request must wake the consumer";
    EXPECT_TRUE(state.HasRuntimeFlag(kCaptureRuntimeFlagStatusOverlayDarkForCapture));
    ce::status_overlay::SetControllerPid(0);
}

TEST(StatusOverlayDarkTest, ALiveConsumerThatNeverAnswersTimesOutAtTheBound) {
    const uint32_t pid = IsolatedControllerPid(4);
    ce::status_overlay::SetControllerPid(pid);
    const ConsumerEndpoints consumer(pid);
    ASSERT_NE(consumer.Ack(), nullptr);

    CaptureState state;
    // A zero bound polls the acknowledgement once: deterministic, and still the "wait" path.
    EXPECT_EQ(ce::status_overlay::RequestDarkForCapture(state, "test", 0), DarkHandshake::kTimedOut);
    // The wake-up was still delivered, so a slow consumer can still catch up.
    EXPECT_EQ(WaitForSingleObject(consumer.Sync(), 0), WAIT_OBJECT_0);
    ce::status_overlay::SetControllerPid(0);
}

TEST(StatusOverlayDarkTest, AStaleAcknowledgementFromAnEarlierRequestDoesNotCount) {
    const uint32_t pid = IsolatedControllerPid(5);
    ce::status_overlay::SetControllerPid(pid);
    const ConsumerEndpoints consumer(pid);
    ASSERT_NE(consumer.Ack(), nullptr);
    SetEvent(consumer.Ack());  // left over from the previous recording start

    CaptureState state;
    EXPECT_EQ(ce::status_overlay::RequestDarkForCapture(state, "test", 0), DarkHandshake::kTimedOut);
    ce::status_overlay::SetControllerPid(0);
}

TEST(StatusOverlayDarkTest, ReleaseClearsTheRequestWhateverTheHandshakeDid) {
    ce::status_overlay::SetControllerPid(IsolatedControllerPid(6));
    CaptureState state;
    ce::status_overlay::RequestDarkForCapture(state, "test", 0);
    ASSERT_TRUE(state.HasRuntimeFlag(kCaptureRuntimeFlagStatusOverlayDarkForCapture));
    ce::status_overlay::ReleaseDarkForCapture(state, "test");
    EXPECT_FALSE(state.HasRuntimeFlag(kCaptureRuntimeFlagStatusOverlayDarkForCapture));
    ce::status_overlay::ReleaseDarkForCapture(state, "test");  // idempotent
    EXPECT_FALSE(state.HasRuntimeFlag(kCaptureRuntimeFlagStatusOverlayDarkForCapture));
    ce::status_overlay::SetControllerPid(0);
}

TEST(StatusOverlayDarkTest, SignalSyncWakesAConsumerAndIsHarmlessWithout) {
    ce::status_overlay::SetControllerPid(IsolatedControllerPid(7));
    ce::status_overlay::SignalSync();  // no consumer: nothing to open, nothing to throw

    const uint32_t pid = IsolatedControllerPid(8);
    ce::status_overlay::SetControllerPid(pid);
    const ConsumerEndpoints consumer(pid);
    ASSERT_NE(consumer.Sync(), nullptr);
    ce::status_overlay::SignalSync();
    EXPECT_EQ(WaitForSingleObject(consumer.Sync(), 0), WAIT_OBJECT_0);
    ce::status_overlay::SetControllerPid(0);
}

TEST(StatusOverlayDarkTest, HandshakeNamesAreStableForLogParsing) {
    EXPECT_STREQ(ce::status_overlay::DarkHandshakeName(DarkHandshake::kNoConsumer), "no_consumer");
    EXPECT_STREQ(ce::status_overlay::DarkHandshakeName(DarkHandshake::kAcknowledged), "acknowledged");
    EXPECT_STREQ(ce::status_overlay::DarkHandshakeName(DarkHandshake::kTimedOut), "timed_out");
}
