#include <gtest/gtest.h>

#include "../mediaengine/mux_invariants.h"

namespace {

using ce::mux::ComputeDurationDeltaUs;
using ce::mux::HeaderValidationIssue;
using ce::mux::HeaderValidationIssueToString;
using ce::mux::IsDurationWithinToleranceUs;
using ce::mux::ValidateStreamForHeader;

}  // namespace

TEST(MuxInvariantTest, HeaderValidationAcceptsStreamsWithCodecParamsAndTimeBase) {
    EXPECT_EQ(ValidateStreamForHeader(true, true, 1, 1000), HeaderValidationIssue::kNone);
}

TEST(MuxInvariantTest, HeaderValidationRejectsMissingStream) {
    EXPECT_EQ(ValidateStreamForHeader(false, false, 0, 0), HeaderValidationIssue::kMissingStream);
    EXPECT_STREQ(HeaderValidationIssueToString(HeaderValidationIssue::kMissingStream), "missing stream");
}

TEST(MuxInvariantTest, HeaderValidationRejectsMissingCodecParameters) {
    EXPECT_EQ(ValidateStreamForHeader(true, false, 1, 1000), HeaderValidationIssue::kMissingCodecParams);
    EXPECT_STREQ(HeaderValidationIssueToString(HeaderValidationIssue::kMissingCodecParams), "missing codec parameters");
}

TEST(MuxInvariantTest, HeaderValidationRejectsInvalidTimeBase) {
    EXPECT_EQ(ValidateStreamForHeader(true, true, 0, 1000), HeaderValidationIssue::kInvalidTimeBase);
    EXPECT_EQ(ValidateStreamForHeader(true, true, 1, 0), HeaderValidationIssue::kInvalidTimeBase);
    EXPECT_STREQ(HeaderValidationIssueToString(HeaderValidationIssue::kInvalidTimeBase), "invalid time base");
}

TEST(MuxInvariantTest, DurationDeltaAndToleranceAreAbsolute) {
    EXPECT_EQ(ComputeDurationDeltaUs(1000, 900), 100);
    EXPECT_EQ(ComputeDurationDeltaUs(900, 1000), 100);
    EXPECT_TRUE(IsDurationWithinToleranceUs(1000, 1001, 1));
    EXPECT_FALSE(IsDurationWithinToleranceUs(1000, 1002, 1));
    EXPECT_TRUE(IsDurationWithinToleranceUs(1000, 1000, -1));
    EXPECT_FALSE(IsDurationWithinToleranceUs(1000, 1002, -1));
}