#include <gtest/gtest.h>

#include <deque>
#include <map>
#include <random>
#include <vector>

#include "av1_test_bitstream.h"
#include "mediaengine/video/av1_show_existing.h"

// Ledger contract: a show-existing repeat changes what a repeat costs, never
// which slots repeat or what they show. The simulation below runs the same
// slot pattern through a FIFO encoder model with a pipeline delay and a decoder
// model, and requires one packet per slot in PTS order, each showing exactly
// the frame a re-encoded repeat would have shown.

namespace {

using ce::media::Av1PacketPlan;
using ce::media::Av1RepeatReencodeReason;
using ce::media::Av1ShowExistingConfig;
using ce::media::Av1ShowExistingRepeats;

Av1ShowExistingConfig EnabledConfig(uint32_t arm = 2, uint32_t bound = 4) {
    Av1ShowExistingConfig config;
    config.enabled = true;
    config.disabledReason = "";
    config.armAfterInterFrames = arm;
    config.maxPendingShowExisting = bound;
    return config;
}

std::vector<uint8_t> Packet(uint8_t type, uint8_t refresh, bool withSequence) {
    av1_test::Frame frame;
    frame.type = type;
    frame.refresh = refresh;
    return av1_test::TemporalUnit({}, frame, withSequence);
}

Av1PacketPlan Feed(Av1ShowExistingRepeats& ledger, int64_t pts, uint8_t type, uint8_t refresh) {
    const auto packet = Packet(type, refresh, type == 0);
    return ledger.OnEncoderPacket(pts, packet.data(), packet.size());
}

// Key at 0, inter 1..3 written: armed with arm=2.
void Arm(Av1ShowExistingRepeats& ledger, uint8_t refresh = 0x01) {
    for (int64_t pts = 0; pts < 4; ++pts) {
        ledger.OnFrameSubmitted(pts, pts == 0);
        Feed(ledger, pts, pts == 0 ? 0 : 1, refresh);
    }
    ASSERT_TRUE(ledger.Armed());
}

TEST(Av1ShowExistingTest, RepeatsReencodeUntilTheHeaderContractIsVerified) {
    Av1ShowExistingRepeats ledger;
    ledger.Reset(EnabledConfig(3));
    Av1PacketPlan plan;
    Av1RepeatReencodeReason reason{};
    ledger.OnFrameSubmitted(0, true);
    Feed(ledger, 0, 0, 0xff);
    for (int64_t pts = 1; pts <= 3; ++pts) {
        EXPECT_FALSE(ledger.TryCommitShowExisting(pts + 100, false, false, &plan, &reason));
        EXPECT_EQ(reason, Av1RepeatReencodeReason::NotArmed);
        ledger.OnFrameSubmitted(pts, false);
        Feed(ledger, pts, 1, 0x02);
    }
    EXPECT_TRUE(ledger.Armed());
    EXPECT_TRUE(ledger.TryCommitShowExisting(4, false, false, &plan, &reason));
    ASSERT_EQ(plan.after.size(), 1u);  // nothing outstanding: written at once
    EXPECT_EQ(plan.after[0].pts, 4);
    EXPECT_EQ(plan.after[0].bytes[4], 0x88 | 1 << 4);  // slot 1 = lowest refresh bit
}

TEST(Av1ShowExistingTest, CommittedRepeatsFollowTheirFramesPacketWithItsSlot) {
    Av1ShowExistingRepeats ledger;
    ledger.Reset(EnabledConfig());
    Arm(ledger);
    Av1PacketPlan plan;
    Av1RepeatReencodeReason reason{};
    ledger.OnFrameSubmitted(4, false);
    ASSERT_TRUE(ledger.TryCommitShowExisting(5, false, false, &plan, &reason));
    ASSERT_TRUE(ledger.TryCommitShowExisting(6, false, false, &plan, &reason));
    EXPECT_TRUE(plan.after.empty());
    EXPECT_EQ(ledger.PendingShowExisting(), 2u);
    const Av1PacketPlan released = Feed(ledger, 4, 1, 0x60);
    ASSERT_EQ(released.after.size(), 2u);
    EXPECT_EQ(released.after[0].pts, 5);
    EXPECT_EQ(released.after[1].pts, 6);
    EXPECT_EQ(released.after[1].bytes[4], 0x88 | 5 << 4);
    EXPECT_EQ(released.holdSlots, 0);
    EXPECT_EQ(ledger.PendingShowExisting(), 0u);
}

TEST(Av1ShowExistingTest, KeyframesOverlaysAndTheLedgerBoundForceReencodes) {
    Av1ShowExistingRepeats ledger;
    ledger.Reset(EnabledConfig(2, 2));
    Arm(ledger);
    Av1PacketPlan plan;
    Av1RepeatReencodeReason reason{};
    EXPECT_FALSE(ledger.TryCommitShowExisting(10, false, true, &plan, &reason));
    EXPECT_EQ(reason, Av1RepeatReencodeReason::KeyframeDue);
    ledger.OnFrameSubmitted(10, true);  // the re-encoded keyframe
    EXPECT_FALSE(ledger.TryCommitShowExisting(11, false, false, &plan, &reason));
    EXPECT_EQ(reason, Av1RepeatReencodeReason::AfterKeyframe);
    ledger.OnFrameSubmitted(11, false);
    EXPECT_FALSE(ledger.TryCommitShowExisting(12, true, false, &plan, &reason));
    EXPECT_EQ(reason, Av1RepeatReencodeReason::Overlay);
    EXPECT_TRUE(ledger.TryCommitShowExisting(12, false, false, &plan, &reason));
    EXPECT_TRUE(ledger.TryCommitShowExisting(13, false, false, &plan, &reason));
    EXPECT_FALSE(ledger.TryCommitShowExisting(14, false, false, &plan, &reason));
    EXPECT_EQ(reason, Av1RepeatReencodeReason::LedgerBound);
    EXPECT_EQ(ledger.Stats().pendingPeak, 2u);
}

TEST(Av1ShowExistingTest, AnUnexpectedKeyframeHoldsCommittedSlotsAndDisarms) {
    Av1ShowExistingRepeats ledger;
    ledger.Reset(EnabledConfig());
    Arm(ledger);
    Av1PacketPlan plan;
    Av1RepeatReencodeReason reason{};
    ledger.OnFrameSubmitted(4, false);
    ASSERT_TRUE(ledger.TryCommitShowExisting(5, false, false, &plan, &reason));
    const Av1PacketPlan released = Feed(ledger, 4, 0, 0xff);  // NVENC chose a key
    EXPECT_TRUE(released.after.empty());
    EXPECT_EQ(released.holdSlots, 1);
    EXPECT_STREQ(released.violation, "unexpected_keyframe");
    EXPECT_FALSE(ledger.TryCommitShowExisting(6, false, false, &plan, &reason));
    EXPECT_EQ(reason, Av1RepeatReencodeReason::Disarmed);
    EXPECT_EQ(ledger.Stats().holdSlots, 1u);
}

TEST(Av1ShowExistingTest, ANonReferenceFrameCannotBeShownAgain) {
    Av1ShowExistingRepeats ledger;
    ledger.Reset(EnabledConfig());
    Arm(ledger);
    Av1PacketPlan plan;
    Av1RepeatReencodeReason reason{};
    ledger.OnFrameSubmitted(4, false);
    ASSERT_TRUE(ledger.TryCommitShowExisting(5, false, false, &plan, &reason));
    const Av1PacketPlan released = Feed(ledger, 4, 1, 0x00);
    EXPECT_EQ(released.holdSlots, 1);
    EXPECT_STREQ(released.violation, "non_reference_frame");
}

TEST(Av1ShowExistingTest, ADroppedSubmitMovesItsRepeatsBehindThePreviousFrame) {
    Av1ShowExistingRepeats ledger;
    ledger.Reset(EnabledConfig());
    Arm(ledger);
    Av1PacketPlan plan;
    Av1RepeatReencodeReason reason{};
    ledger.OnFrameSubmitted(4, false);
    ledger.OnFrameSubmitted(5, false);
    ASSERT_TRUE(ledger.TryCommitShowExisting(6, false, false, &plan, &reason));
    ledger.OnFrameDropped(5);
    EXPECT_FALSE(ledger.TryCommitShowExisting(7, false, false, &plan, &reason));
    EXPECT_EQ(reason, Av1RepeatReencodeReason::NoPriorFrame);
    const Av1PacketPlan released = Feed(ledger, 4, 1, 0x04);
    ASSERT_EQ(released.after.size(), 1u);
    EXPECT_EQ(released.after[0].pts, 6);
    EXPECT_EQ(released.after[0].bytes[4], 0x88 | 2 << 4);
}

TEST(Av1ShowExistingTest, APacketOutOfOrderDisarmsWithoutWritingTheGap) {
    Av1ShowExistingRepeats ledger;
    ledger.Reset(EnabledConfig());
    Arm(ledger);
    Av1PacketPlan plan;
    Av1RepeatReencodeReason reason{};
    ledger.OnFrameSubmitted(4, false);
    ASSERT_TRUE(ledger.TryCommitShowExisting(5, false, false, &plan, &reason));
    ledger.OnFrameSubmitted(6, false);
    const Av1PacketPlan released = Feed(ledger, 6, 1, 0x01);
    EXPECT_STREQ(released.violation, "packet_order_mismatch");
    EXPECT_TRUE(released.after.empty());
    EXPECT_EQ(ledger.PendingShowExisting(), 0u);
    EXPECT_EQ(ledger.Stats().holdSlots, 1u);
}

TEST(Av1ShowExistingTest, DisabledSessionsNeverCommit) {
    Av1ShowExistingRepeats ledger;
    Av1ShowExistingConfig config = EnabledConfig();
    config.enabled = false;
    config.disabledReason = "b_frames";
    ledger.Reset(config);
    Av1PacketPlan plan;
    Av1RepeatReencodeReason reason{};
    ledger.OnFrameSubmitted(0, true);
    EXPECT_FALSE(ledger.TryCommitShowExisting(1, false, false, &plan, &reason));
    EXPECT_EQ(reason, Av1RepeatReencodeReason::Disabled);
    EXPECT_STREQ(ledger.DisabledReason(), "b_frames");
}

TEST(Av1ShowExistingTest, AnUnwritableSequenceDisables) {
    Av1ShowExistingRepeats ledger;
    ledger.Reset(EnabledConfig());
    av1_test::Sequence model;
    model.timing = model.decoderModel = true;
    const auto extradata = av1_test::Obu(1, av1_test::SequencePayload(model));
    EXPECT_FALSE(ledger.ObserveExtradata(extradata.data(), extradata.size()));
    EXPECT_FALSE(ledger.Enabled());
}

// Same slots, same pictures: one packet per slot in PTS order, every slot
// showing what a re-encoded repeat would show.
TEST(Av1ShowExistingTest, EverySlotShowsTheSameFrameAsReencodingWould) {
    for (uint32_t seed = 1; seed <= 20; ++seed) {
        SCOPED_TRACE(seed);
        std::mt19937 random(seed);
        Av1ShowExistingRepeats ledger;
        ledger.Reset(EnabledConfig(3, 6));
        constexpr size_t kPipeline = 5;
        std::deque<std::pair<int64_t, uint8_t>> encoder;  // pts, refresh
        std::map<int64_t, int> contentOfPts;              // encoded pts -> content
        std::array<int, 8> decoderSlots = {};
        std::vector<std::pair<int64_t, int>> displayed;  // written pts -> content
        std::vector<int> expected;                       // content per slot
        int lastContent = -1;
        int nextContent = 0;
        auto write = [&](const Av1PacketPlan& plan) {
            for (const auto& unit : plan.after) {
                const uint8_t idx = (unit.bytes[4] >> 4) & 7;
                ASSERT_EQ(unit.bytes[4] & 0x8f, 0x88);
                displayed.emplace_back(unit.pts, decoderSlots[idx]);
            }
            ASSERT_EQ(plan.holdSlots, 0);
            ASSERT_EQ(plan.violation, nullptr);
        };
        auto popPacket = [&]() {
            const auto [pts, refresh] = encoder.front();
            encoder.pop_front();
            const bool key = refresh == 0xff;
            const auto packet = Packet(key ? 0 : 1, refresh, key);
            const int content = contentOfPts.at(pts);
            for (int slot = 0; slot < 8; ++slot) {
                if (refresh & (1 << slot))
                    decoderSlots[slot] = content;
            }
            displayed.emplace_back(pts, content);
            write(ledger.OnEncoderPacket(pts, packet.data(), packet.size()));
        };
        for (int64_t pts = 0; pts < 400; ++pts) {
            const bool fresh = pts == 0 || random() % 3 != 0;
            const bool keyed = pts % 37 == 0;
            if (fresh) {
                lastContent = nextContent++;
            } else {
                Av1PacketPlan plan;
                Av1RepeatReencodeReason reason{};
                if (ledger.TryCommitShowExisting(pts, random() % 11 == 0, keyed, &plan, &reason)) {
                    expected.push_back(lastContent);
                    write(plan);
                    continue;
                }
            }
            // Fresh frame or re-encoded repeat of the cached content.
            expected.push_back(lastContent);
            contentOfPts[pts] = lastContent;
            ledger.OnFrameSubmitted(pts, keyed);
            encoder.emplace_back(pts, keyed ? 0xff : static_cast<uint8_t>(1u << (pts % 3)));
            if (encoder.size() > kPipeline)
                popPacket();
        }
        while (!encoder.empty())
            popPacket();
        ASSERT_EQ(displayed.size(), expected.size());
        for (size_t slot = 0; slot < displayed.size(); ++slot) {
            ASSERT_EQ(displayed[slot].first, static_cast<int64_t>(slot));
            ASSERT_EQ(displayed[slot].second, expected[slot]) << "slot " << slot;
        }
        EXPECT_GT(ledger.Stats().written, 50u);
        EXPECT_EQ(ledger.PendingShowExisting(), 0u);
    }
}

}  // namespace
