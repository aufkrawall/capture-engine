#include <gtest/gtest.h>

#include <vector>

#include "av1_test_bitstream.h"
#include "mediaengine/video/av1_obu.h"

// The reader must land exactly on refresh_frame_flags for every sequence
// option that moves it, and the writer must emit the one-byte show-existing
// header CE relies on. Real-encoder cross-checks live in
// test_av1_show_existing_roundtrip.cpp.

namespace {

using namespace ce::media::av1;

TemporalUnitInfo ParseUnit(const av1_test::Sequence& s, const av1_test::Frame& f, ParseResult* result = nullptr) {
    SequenceHeader sequence;
    TemporalUnitInfo info;
    const auto unit = av1_test::TemporalUnit(s, f, true);
    const ParseResult parsed = ParseTemporalUnit(unit.data(), unit.size(), &sequence, &info);
    if (result)
        *result = parsed;
    EXPECT_EQ(parsed.status, ParseStatus::Ok) << parsed.reason;
    return info;
}

TEST(Av1ObuTest, ShowExistingUnitIsTemporalDelimiterPlusOneByteFrameHeader) {
    SequenceHeader sequence;
    const auto payload = av1_test::SequencePayload({});
    ASSERT_EQ(ParseSequenceHeaderObu(payload.data(), payload.size(), &sequence).status, ParseStatus::Ok);
    for (uint8_t slot = 0; slot < kNumRefFrames; ++slot) {
        uint8_t out[kMaxShowExistingUnitBytes] = {};
        ASSERT_EQ(WriteShowExistingTemporalUnit(sequence, slot, 0, out, sizeof(out)), 5u);
        EXPECT_EQ(out[0], 0x12);  // OBU_TEMPORAL_DELIMITER, has_size
        EXPECT_EQ(out[1], 0x00);
        EXPECT_EQ(out[2], 0x1a);  // OBU_FRAME_HEADER, has_size
        EXPECT_EQ(out[3], 0x01);
        EXPECT_EQ(out[4], 0x88 | slot << 4);  // show_existing, idx, trailing one bit
    }
}

TEST(Av1ObuTest, WrittenShowExistingParsesBackWithFrameIds) {
    av1_test::Sequence s;
    s.frameIds = true;
    SequenceHeader sequence;
    const auto payload = av1_test::SequencePayload(s);
    ASSERT_EQ(ParseSequenceHeaderObu(payload.data(), payload.size(), &sequence).status, ParseStatus::Ok);
    EXPECT_EQ(sequence.frameIdLength, av1_test::FrameIdLength(s));
    uint8_t out[kMaxShowExistingUnitBytes] = {};
    const size_t size = WriteShowExistingTemporalUnit(sequence, 6, 0x1234, out, sizeof(out));
    ASSERT_GT(size, 5u);
    TemporalUnitInfo info;
    ASSERT_EQ(ParseTemporalUnit(out, size, &sequence, &info).status, ParseStatus::Ok);
    EXPECT_EQ(info.frameHeaders, 1);
    EXPECT_TRUE(info.frame.showExistingFrame);
    EXPECT_EQ(info.frame.frameToShowMapIdx, 6);
    EXPECT_EQ(info.frame.currentFrameId, 0x1234u);
}

TEST(Av1ObuTest, RefreshFlagsSurviveEverySequenceOptionThatShiftsThem) {
    struct Case {
        const char* name;
        av1_test::Sequence s;
    };
    std::vector<Case> cases;
    cases.push_back({"default", {}});
    av1_test::Sequence timing;
    timing.timing = timing.equalInterval = true;
    cases.push_back({"timing_equal_interval", timing});
    av1_test::Sequence model = timing;
    model.decoderModel = true;
    cases.push_back({"decoder_model_equal_interval", model});
    av1_test::Sequence modelVariable = model;
    modelVariable.equalInterval = false;
    cases.push_back({"decoder_model_variable_interval", modelVariable});
    av1_test::Sequence ops = model;
    ops.operatingPointIdc = {0x103, 0x101, 0};
    ops.decoderModelForOp = {true, false, true};
    ops.initialDisplayDelay = true;
    cases.push_back({"operating_points", ops});
    av1_test::Sequence ids;
    ids.frameIds = true;
    cases.push_back({"frame_ids", ids});
    av1_test::Sequence screen;
    screen.screenContent = 1;
    screen.integerMv = 2;
    cases.push_back({"screen_content_forced_integer_select", screen});
    av1_test::Sequence noScreen;
    noScreen.screenContent = 0;
    cases.push_back({"no_screen_content", noScreen});
    av1_test::Sequence noOrderHint;
    noOrderHint.orderHint = false;
    cases.push_back({"no_order_hint", noOrderHint});

    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        av1_test::Frame inter;
        inter.refresh = 0xa4;
        inter.frameId = 77;
        inter.orderHint = 9;
        inter.allowScreenContent = true;
        inter.bufferRemoval = true;
        const auto info = ParseUnit(c.s, inter);
        EXPECT_TRUE(info.sequenceHeaderSeen);
        EXPECT_EQ(info.frame.frameType, FrameType::Inter);
        EXPECT_TRUE(info.frame.showFrame);
        EXPECT_TRUE(info.frame.showableFrame);
        EXPECT_EQ(info.frame.refreshFrameFlags, 0xa4);
        if (c.s.frameIds)
            EXPECT_EQ(info.frame.currentFrameId, 77u);

        av1_test::Frame resilient = inter;
        resilient.errorResilient = true;  // no primary_ref_frame
        resilient.refresh = 0x10;
        EXPECT_EQ(ParseUnit(c.s, resilient).frame.refreshFrameFlags, 0x10);
    }
}

TEST(Av1ObuTest, ShownKeyRefreshesEverySlotAndIsNotShowable) {
    av1_test::Frame key;
    key.type = 0;
    const auto info = ParseUnit({}, key);
    EXPECT_EQ(info.frame.frameType, FrameType::Key);
    EXPECT_FALSE(info.frame.showableFrame);
    EXPECT_EQ(info.frame.refreshFrameFlags, 0xff);
}

TEST(Av1ObuTest, HiddenFramesCarryTheirOwnShowableBit) {
    av1_test::Frame hidden;
    hidden.show = false;
    hidden.showable = true;
    hidden.refresh = 0x40;
    auto info = ParseUnit({}, hidden);
    EXPECT_FALSE(info.frame.showFrame);
    EXPECT_TRUE(info.frame.showableFrame);
    EXPECT_EQ(info.frame.refreshFrameFlags, 0x40);
    hidden.showable = false;
    EXPECT_FALSE(ParseUnit({}, hidden).frame.showableFrame);
}

TEST(Av1ObuTest, IntraOnlyAndSwitchFramesFollowTheSpecialCases) {
    av1_test::Frame intraOnly;
    intraOnly.type = 2;
    intraOnly.refresh = 0x03;
    auto info = ParseUnit({}, intraOnly);
    EXPECT_TRUE(info.frame.showableFrame);
    EXPECT_EQ(info.frame.refreshFrameFlags, 0x03);
    av1_test::Frame switchFrame;
    switchFrame.type = 3;
    info = ParseUnit({}, switchFrame);
    EXPECT_EQ(info.frame.refreshFrameFlags, 0xff);
}

TEST(Av1ObuTest, UnsupportedAndMalformedInputFailsClosed) {
    av1_test::Sequence reduced;
    reduced.reducedStill = true;
    SequenceHeader sequence;
    auto payload = av1_test::SequencePayload(reduced);
    ASSERT_EQ(ParseSequenceHeaderObu(payload.data(), payload.size(), &sequence).status, ParseStatus::Ok);
    EXPECT_EQ(CanWriteShowExisting(sequence).status, ParseStatus::Unsupported);
    uint8_t out[kMaxShowExistingUnitBytes] = {};
    EXPECT_EQ(WriteShowExistingTemporalUnit(sequence, 0, 0, out, sizeof(out)), 0u);

    av1_test::Sequence model;
    model.timing = model.decoderModel = true;
    payload = av1_test::SequencePayload(model);
    ASSERT_EQ(ParseSequenceHeaderObu(payload.data(), payload.size(), &sequence).status, ParseStatus::Ok);
    EXPECT_STREQ(CanWriteShowExisting(sequence).reason, "decoder_model_without_equal_picture_interval");

    // A frame before any sequence header, and a truncated unit.
    SequenceHeader empty;
    TemporalUnitInfo info;
    auto unit = av1_test::TemporalUnit({}, {}, false);
    EXPECT_EQ(ParseTemporalUnit(unit.data(), unit.size(), &empty, &info).status, ParseStatus::NoSequence);
    unit = av1_test::TemporalUnit({}, {}, true);
    unit.resize(unit.size() - 9);
    EXPECT_EQ(ParseTemporalUnit(unit.data(), unit.size(), &empty, &info).status, ParseStatus::Truncated);

    // OBU extensions are reported, so the ledger can refuse scalable streams.
    SequenceHeader valid;
    auto withExtension = av1_test::Obu(1, av1_test::SequencePayload({}));
    const auto frame = av1_test::Obu(6, av1_test::FramePayload({}, {}), true);
    withExtension.insert(withExtension.end(), frame.begin(), frame.end());
    ASSERT_EQ(ParseTemporalUnit(withExtension.data(), withExtension.size(), &valid, &info).status, ParseStatus::Ok);
    EXPECT_TRUE(info.extensionSeen);
}

TEST(Av1ObuTest, Av1cExtradataOrRawSequenceHeaderBothParse) {
    const auto raw = av1_test::Obu(1, av1_test::SequencePayload({}));
    SequenceHeader sequence;
    TemporalUnitInfo info;
    ASSERT_EQ(ParseTemporalUnit(raw.data(), raw.size(), &sequence, &info).status, ParseStatus::Ok);
    EXPECT_TRUE(info.sequenceHeaderSeen);
    EXPECT_EQ(info.frameHeaders, 0);
    EXPECT_EQ(sequence.orderHintBits, 7);
}

}  // namespace
