#include "../captureengine/display_timing_nvidia.h"
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kThread = 0x1234;
constexpr uint32_t kOtherThread = 0x5678;
constexpr int64_t kFrequency = 10'000'000;  // QPC ticks per second on this hardware

// Payload shaped like the captured NVIDIA FlipRequest: 44 bytes, monotone
// counters at 0 and 8, the announced QPC at 16, zeros beyond.
std::vector<unsigned char> MakePayload(uint64_t counterA, uint64_t counterB, int64_t announcedQpc) {
    std::vector<unsigned char> payload(44, 0);
    std::memcpy(payload.data() + 0, &counterA, sizeof(counterA));
    std::memcpy(payload.data() + 8, &counterB, sizeof(counterB));
    std::memcpy(payload.data() + 16, &announcedQpc, sizeof(announcedQpc));
    return payload;
}

// Runs one discovery window of realistic events so the decoder locates the field.
void Locate(NvidiaFlipAnnouncementDecoder& decoder, int64_t baseQpc) {
    for (std::size_t i = 0; i < NvidiaFlipAnnouncementDecoder::kDiscoverySamples; ++i) {
        const int64_t eventQpc = baseQpc + static_cast<int64_t>(i) * 70'000;
        const auto payload = MakePayload(2'764'720 + i, 1249 + i, eventQpc + 70'000);
        decoder.Decode(payload.data(), payload.size(), eventQpc);
    }
}

}  // namespace

TEST(DisplayTimingNvidiaDecoderTest, LocatesTheAnnouncementFieldInTheCapturedLayout) {
    NvidiaFlipAnnouncementDecoder decoder;
    decoder.SetQpcFrequency(kFrequency);
    EXPECT_FALSE(decoder.located());
    Locate(decoder, 582'956'509'692);
    ASSERT_TRUE(decoder.located());
    EXPECT_EQ(decoder.offset(), 16u);
    EXPECT_FALSE(decoder.abandoned());
}

// An unambiguous payload is usable before the quorum is reached, otherwise the
// first window of frames would keep the uncorrected sawtooth on every start.
TEST(DisplayTimingNvidiaDecoderTest, UnambiguousPayloadDecodesBeforeTheFieldIsLocked) {
    NvidiaFlipAnnouncementDecoder decoder;
    decoder.SetQpcFrequency(kFrequency);
    const int64_t eventQpc = 582'956'509'692;
    const auto payload = MakePayload(2'764'720, 1249, eventQpc + 70'700);
    EXPECT_EQ(decoder.Decode(payload.data(), payload.size(), eventQpc), eventQpc + 70'700);
    EXPECT_FALSE(decoder.located());
}

// Two slots that both look like announcements are not unambiguous, so nothing
// is used until the quorum decides which one is the field.
TEST(DisplayTimingNvidiaDecoderTest, AmbiguousPayloadDecodesNothingBeforeLocking) {
    NvidiaFlipAnnouncementDecoder decoder;
    decoder.SetQpcFrequency(kFrequency);
    const int64_t eventQpc = 582'956'509'692;
    auto payload = MakePayload(2'764'720, 1249, eventQpc + 70'700);
    const int64_t second = eventQpc + 40'000;
    std::memcpy(payload.data() + 24, &second, sizeof(second));
    EXPECT_EQ(decoder.Decode(payload.data(), payload.size(), eventQpc), 0);
}

TEST(DisplayTimingNvidiaDecoderTest, ReturnsTheAnnouncedTimeOnceLocated) {
    NvidiaFlipAnnouncementDecoder decoder;
    decoder.SetQpcFrequency(kFrequency);
    Locate(decoder, 582'956'509'692);
    const int64_t eventQpc = 582'960'000'000;
    const auto payload = MakePayload(2'800'000, 2000, eventQpc + 70'700);
    EXPECT_EQ(decoder.Decode(payload.data(), payload.size(), eventQpc), eventQpc + 70'700);
}

// The real frame flips immediately and the generated frame is scheduled a frame
// ahead; both are announced within a fraction of a millisecond of each other.
TEST(DisplayTimingNvidiaDecoderTest, ResolvesBothImmediateAndScheduledAnnouncements) {
    NvidiaFlipAnnouncementDecoder decoder;
    decoder.SetQpcFrequency(kFrequency);
    Locate(decoder, 582'956'509'692);

    const int64_t immediateEvent = 582'970'000'000;
    const auto immediate = MakePayload(1, 1, immediateEvent + 23);
    EXPECT_EQ(decoder.Decode(immediate.data(), immediate.size(), immediateEvent), immediateEvent + 23);

    const int64_t scheduledEvent = immediateEvent + 1676;
    const auto scheduled = MakePayload(2, 2, scheduledEvent + 70'649);
    EXPECT_EQ(decoder.Decode(scheduled.data(), scheduled.size(), scheduledEvent), scheduledEvent + 70'649);
}

TEST(DisplayTimingNvidiaDecoderTest, CountersAndHandlesAreNeverMistakenForAnnouncements) {
    NvidiaFlipAnnouncementDecoder decoder;
    decoder.SetQpcFrequency(kFrequency);
    // No slot holds a plausible timestamp, so discovery must never lock on.
    for (std::size_t attempt = 0; attempt <= NvidiaFlipAnnouncementDecoder::kMaxDiscoveryAttempts; ++attempt) {
        for (std::size_t i = 0; i < NvidiaFlipAnnouncementDecoder::kDiscoverySamples; ++i) {
            const int64_t eventQpc = 582'956'509'692 + static_cast<int64_t>(i) * 70'000;
            const auto payload = MakePayload(2'764'720 + i, 1249 + i, 0);
            EXPECT_EQ(decoder.Decode(payload.data(), payload.size(), eventQpc), 0);
        }
    }
    EXPECT_FALSE(decoder.located());
    EXPECT_TRUE(decoder.abandoned());
}

TEST(DisplayTimingNvidiaDecoderTest, DiscoveryIsGivenUpRatherThanGuessed) {
    NvidiaFlipAnnouncementDecoder decoder;
    decoder.SetQpcFrequency(kFrequency);
    // A field that is plausible only half the time never reaches the quorum.
    for (std::size_t attempt = 0; attempt <= NvidiaFlipAnnouncementDecoder::kMaxDiscoveryAttempts; ++attempt) {
        for (std::size_t i = 0; i < NvidiaFlipAnnouncementDecoder::kDiscoverySamples; ++i) {
            const int64_t eventQpc = 582'956'509'692 + static_cast<int64_t>(i) * 70'000;
            const auto payload = MakePayload(1, 1, (i % 2) ? eventQpc + 70'000 : 0);
            decoder.Decode(payload.data(), payload.size(), eventQpc);
        }
    }
    EXPECT_FALSE(decoder.located());
}

TEST(DisplayTimingNvidiaDecoderTest, AnnouncementTooFarAheadIsRejected) {
    NvidiaFlipAnnouncementDecoder decoder;
    decoder.SetQpcFrequency(kFrequency);
    Locate(decoder, 582'956'509'692);
    const int64_t eventQpc = 582'980'000'000;
    const int64_t tooFar = eventQpc + (NvidiaFlipAnnouncementDecoder::kMaxAheadUs + 1'000) * (kFrequency / 1'000'000);
    const auto payload = MakePayload(1, 1, tooFar);
    EXPECT_EQ(decoder.Decode(payload.data(), payload.size(), eventQpc), 0);
    EXPECT_EQ(decoder.rejected(), 1u);
}

TEST(DisplayTimingNvidiaDecoderTest, ShortOrMissingPayloadYieldsNoAnnouncement) {
    NvidiaFlipAnnouncementDecoder decoder;
    decoder.SetQpcFrequency(kFrequency);
    unsigned char tiny[4] = {};
    EXPECT_EQ(decoder.Decode(tiny, sizeof(tiny), 582'956'509'692), 0);
    EXPECT_EQ(decoder.Decode(nullptr, 44, 582'956'509'692), 0);
}

TEST(DisplayTimingNvidiaDecoderTest, WithoutAQpcFrequencyNothingIsDecoded) {
    NvidiaFlipAnnouncementDecoder decoder;
    const auto payload = MakePayload(1, 1, 582'956'579'692);
    EXPECT_EQ(decoder.Decode(payload.data(), payload.size(), 582'956'509'692), 0);
    EXPECT_FALSE(decoder.located());
}

// A driver update that moves the field must be followed, not silently trusted.
TEST(DisplayTimingNvidiaDecoderTest, RelocatesWhenTheLocatedSlotStopsAnswering) {
    NvidiaFlipAnnouncementDecoder decoder;
    decoder.SetQpcFrequency(kFrequency);
    Locate(decoder, 582'956'509'692);
    ASSERT_EQ(decoder.offset(), 16u);

    // Same payload shape, announcement moved to offset 24.
    int64_t eventQpc = 583'000'000'000;
    for (std::size_t i = 0; i < NvidiaFlipAnnouncementDecoder::kMaxConsecutiveRejects +
                                    NvidiaFlipAnnouncementDecoder::kDiscoverySamples + 1;
         ++i) {
        std::vector<unsigned char> payload(44, 0);
        const uint64_t counter = 3'000'000 + i;
        std::memcpy(payload.data() + 0, &counter, sizeof(counter));
        const int64_t announced = eventQpc + 70'000;
        std::memcpy(payload.data() + 24, &announced, sizeof(announced));
        decoder.Decode(payload.data(), payload.size(), eventQpc);
        eventQpc += 70'000;
    }
    EXPECT_TRUE(decoder.located());
    EXPECT_EQ(decoder.offset(), 24u);
}

TEST(DisplayTimingNvidiaDecoderTest, ResetClearsLocationAndAbandonment) {
    NvidiaFlipAnnouncementDecoder decoder;
    decoder.SetQpcFrequency(kFrequency);
    Locate(decoder, 582'956'509'692);
    ASSERT_TRUE(decoder.located());
    decoder.Reset();
    EXPECT_FALSE(decoder.located());
    EXPECT_FALSE(decoder.abandoned());
    EXPECT_EQ(decoder.rejected(), 0u);
}

TEST(DisplayTimingNvidiaTrackerTest, AnnouncedTimeBecomesTheDelayOverTheFlipEvent) {
    NvidiaFlipDelayTracker tracker;
    tracker.ObserveAnnouncement(kThread, 1000, 1700);
    const NvidiaFlipDelay delay = tracker.TakeFlipDelay(kThread);
    EXPECT_TRUE(delay.matched);
    EXPECT_EQ(delay.delay, 700);
}

// Frame generation programs the paced flips in a burst and they scan out evenly;
// the announcements are what turn the one series into the other.
TEST(DisplayTimingNvidiaTrackerTest, BurstProgrammedFlipsResolveToAnEvenScreenSeries) {
    NvidiaFlipDelayTracker tracker;
    const int64_t programmedAt[] = {1000, 1100, 1150, 1200};
    const int64_t announced[] = {2000, 2700, 3400, 4100};
    std::vector<int64_t> screenTimes;
    for (int i = 0; i < 4; ++i) {
        tracker.ObserveAnnouncement(kThread, programmedAt[i], announced[i]);
        screenTimes.push_back(programmedAt[i] + tracker.TakeFlipDelay(kThread).delay);
    }
    ASSERT_EQ(screenTimes.size(), 4u);
    for (std::size_t i = 1; i < screenTimes.size(); ++i)
        EXPECT_EQ(screenTimes[i] - screenTimes[i - 1], 700) << "interval " << i;
}

TEST(DisplayTimingNvidiaTrackerTest, AnnouncementAlreadyInThePastYieldsNoDelay) {
    NvidiaFlipDelayTracker tracker;
    tracker.ObserveAnnouncement(kThread, 5000, 4000);
    EXPECT_EQ(tracker.TakeFlipDelay(kThread).delay, 0);
}

TEST(DisplayTimingNvidiaTrackerTest, AnnouncementIsNotOverwrittenWhileOutstanding) {
    NvidiaFlipDelayTracker tracker;
    tracker.ObserveAnnouncement(kThread, 1000, 1500);
    tracker.ObserveAnnouncement(kThread, 1000, 9999);
    EXPECT_EQ(tracker.TakeFlipDelay(kThread).delay, 500);
}

TEST(DisplayTimingNvidiaTrackerTest, ConsumingOneFlipDropsEveryOutstandingAnnouncement) {
    NvidiaFlipDelayTracker tracker;
    tracker.ObserveAnnouncement(kThread, 1000, 1600);
    tracker.ObserveAnnouncement(kOtherThread, 1000, 2200);
    EXPECT_EQ(tracker.TakeFlipDelay(kThread).delay, 600);
    EXPECT_EQ(tracker.pendingRequestCount(), 0u);
    EXPECT_FALSE(tracker.TakeFlipDelay(kOtherThread).matched);
}

TEST(DisplayTimingNvidiaTrackerTest, UnmatchedThreadKeepsAnnouncementsForTheirOwnFlip) {
    NvidiaFlipDelayTracker tracker;
    tracker.ObserveAnnouncement(kThread, 1000, 1600);
    const NvidiaFlipDelay miss = tracker.TakeFlipDelay(kOtherThread);
    EXPECT_FALSE(miss.matched);
    EXPECT_EQ(miss.delay, 0);
    EXPECT_EQ(tracker.TakeFlipDelay(kThread).delay, 600);
}

// Non-NVIDIA adapters never announce; the flip timestamp must pass through.
TEST(DisplayTimingNvidiaTrackerTest, NoAnnouncementLeavesTheFlipTimestampUntouched) {
    NvidiaFlipDelayTracker tracker;
    const NvidiaFlipDelay delay = tracker.TakeFlipDelay(kThread);
    EXPECT_FALSE(delay.matched);
    EXPECT_EQ(delay.delay, 0);
}

TEST(DisplayTimingNvidiaTrackerTest, NonPositiveAnnouncementIsIgnored) {
    NvidiaFlipDelayTracker tracker;
    tracker.ObserveAnnouncement(kThread, 1000, 0);
    EXPECT_EQ(tracker.pendingRequestCount(), 0u);
    EXPECT_FALSE(tracker.TakeFlipDelay(kThread).matched);
}

TEST(DisplayTimingNvidiaTrackerTest, PruneDropsAnnouncementsOlderThanTheCutoff) {
    NvidiaFlipDelayTracker tracker;
    tracker.ObserveAnnouncement(kThread, 1000, 1600);
    tracker.ObserveAnnouncement(kOtherThread, 9000, 9600);
    tracker.PruneBefore(5000);
    EXPECT_EQ(tracker.pendingRequestCount(), 1u);
    EXPECT_FALSE(tracker.TakeFlipDelay(kThread).matched);
    EXPECT_TRUE(tracker.TakeFlipDelay(kOtherThread).matched);
}
