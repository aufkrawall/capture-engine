#include <gtest/gtest.h>

#include <windows.h>

#include <memory>
#include <thread>

#include "captureengine/app/mediaengine_loader.h"
#include "captureengine/media/media_main_internal.h"
#include "captureengine/media/media_session_state.h"
#include "common/capture/inject_frame_ring_lease.h"
#include "common/config/config.h"

// A stopping session detaches its view from the inject session's shared memory (common/capture/detached_live_state.h),
// but the frame ring is the hook's channel to its consumer, not state the private copy can carry. The ingest thread
// kept running across the retarget: it read the copy's empty ring, walked 2^32 cursor positions in a catch-up that
// stored its ingest cursor into the copy, while the drain's leases completed on the live ring. The live ring was left
// with readIndex one past ingestIndex, and every later recording of that game was rejected as a corrupt shared frame
// ring (HotS session 20261027_235200: write=14343 ingest=14342 read=14343).

namespace {

int64_t NoEngineTiming(MediaEngineHandle) {
    return 0;
}

void Publish(FrameRingBuffer& ring, uint32_t index) {
    auto& slot = ring.slots[index % FRAME_RING_SIZE];
    slot.textureIndex = static_cast<int32_t>(index % SHARED_TEXTURE_SLOT_COUNT);
    slot.valid.store(1, std::memory_order_release);
    ring.writeIndex.store(index + 1, std::memory_order_release);
}

bool PassesRecordingStartRingCheck(const FrameRingBuffer& ring) {
    const uint32_t write = ring.writeIndex.load(std::memory_order_acquire);
    const uint32_t ingest = ring.ingestIndex.load(std::memory_order_acquire);
    const uint32_t read = ring.readIndex.load(std::memory_order_acquire);
    return IsFrameRingWindowValid(write, read) &&
           static_cast<uint32_t>(ingest - read) <= static_cast<uint32_t>(write - read);
}

class DetachFrameRingHandoffTest : public testing::Test {
protected:
    void SetUp() override {
        savedEncodeTime_ = ce::mediaengine_api::MediaEngine_GetLastFrameEncodeTimeUs;
        savedFenceWait_ = ce::mediaengine_api::MediaEngine_GetLastFrameFenceWaitUs;
        // The ingest thread's once-per-second statistics line reads these.
        ce::mediaengine_api::MediaEngine_GetLastFrameEncodeTimeUs = NoEngineTiming;
        ce::mediaengine_api::MediaEngine_GetLastFrameFenceWaitUs = NoEngineTiming;
        session_.liveSharedMem = live_.get();
        session_.pSharedMem = live_.get();
    }

    void TearDown() override {
        // Without the hand-off the ingest thread outlives the detach; stop it before the session goes away.
        StopInjectCapturePipeline();
        session_.recording = false;
        if (session_.injectCaptureShutdownEvent) {
            CloseHandle(session_.injectCaptureShutdownEvent);
            session_.injectCaptureShutdownEvent = NULL;
        }
        ce::mediaengine_api::MediaEngine_GetLastFrameEncodeTimeUs = savedEncodeTime_;
        ce::mediaengine_api::MediaEngine_GetLastFrameFenceWaitUs = savedFenceWait_;
    }

    // What StartRecording does for an inject recording.
    void StartIngest() {
        session_.recording = true;
        session_.injectCaptureShutdown = false;
        ASSERT_TRUE(EnsureInjectCaptureEvents() || session_.injectCaptureShutdownEvent);
        MediaSessionState* session = &session_;
        const AppConfig* config = &config_;
        session_.injectCaptureThread = std::thread([session, config]() {
            MediaSessionBinding binding(*session);
            InjectCaptureThreadFunc(*config);
        });
    }

    std::unique_ptr<SharedMemoryLayout> live_ = std::make_unique<SharedMemoryLayout>();
    MediaSessionState session_;
    MediaSessionBinding binding_{session_};
    AppConfig config_;
    MediaEngine_GetLastFrameEncodeTimeUs_t savedEncodeTime_ = nullptr;
    MediaEngine_GetLastFrameFenceWaitUs_t savedFenceWait_ = nullptr;
};

}  // namespace

TEST_F(DetachFrameRingHandoffTest, AStoppingSessionHandsTheLiveRingOverBeforeItsViewMoves) {
    FrameRingBuffer& ring = live_->frameRing;
    // Frame 0 was ingested and its encoder read is still in flight: the drain owns that lease.
    Publish(ring, 0);
    ring.ingestIndex.store(1, std::memory_order_release);
    auto drainLeases = std::make_shared<ce::InjectFrameRingLeaseState>(&ring);
    ce::InjectFrameRingLease drainRead = drainLeases->Acquire(0);
    // Frames 1 and 2 are published; the ingest thread may or may not have observed them when the stop arrives.
    Publish(ring, 1);
    Publish(ring, 2);
    StartIngest();

    DetachFromSharedLiveState("test stop");

    EXPECT_FALSE(session_.injectCaptureThread.joinable()) << "the ingest thread kept consuming across the retarget";
    ASSERT_EQ(session_.pSharedMem.get(), session_.detachedSharedMem.get());
    const FrameRingBuffer& copy = session_.detachedSharedMem->frameRing;
    EXPECT_EQ(copy.ingestIndex.load(std::memory_order_acquire), 0u) << "an ingest cursor reached the private copy";
    EXPECT_EQ(copy.writeIndex.load(std::memory_order_acquire), 0u);

    // Handed over with nothing unread; only the drain's lease is still outstanding.
    EXPECT_EQ(ring.writeIndex.load(std::memory_order_acquire), 3u);
    EXPECT_EQ(ring.ingestIndex.load(std::memory_order_acquire), 3u);
    EXPECT_EQ(ring.readIndex.load(std::memory_order_acquire), 0u);
    EXPECT_TRUE(PassesRecordingStartRingCheck(ring));

    // The drain's read finishes after the detach: it acknowledges on the live ring.
    drainRead.Reset();
    EXPECT_EQ(ring.readIndex.load(std::memory_order_acquire), 3u);
    EXPECT_TRUE(PassesRecordingStartRingCheck(ring)) << "the next recording start would reject this ring";

    // The stop path's own ring reset runs on the detached view and leaves the live ring to the next recording.
    Publish(ring, 3);
    ResetInjectFrameRingToLatest("test recording stop");
    EXPECT_EQ(ring.ingestIndex.load(std::memory_order_acquire), 3u);
    EXPECT_EQ(ring.slots[3].valid.load(std::memory_order_acquire), 1u);
}

TEST_F(DetachFrameRingHandoffTest, ASessionThatNeverConsumedTheRingLeavesItAlone) {
    FrameRingBuffer& ring = live_->frameRing;
    Publish(ring, 0);

    DetachFromSharedLiveState("test stop");

    ASSERT_EQ(session_.pSharedMem.get(), session_.detachedSharedMem.get());
    EXPECT_EQ(ring.ingestIndex.load(std::memory_order_acquire), 0u);
    EXPECT_EQ(ring.readIndex.load(std::memory_order_acquire), 0u);
    EXPECT_EQ(ring.slots[0].valid.load(std::memory_order_acquire), 1u);
}
