#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "../common/output_completion_notification.h"
#include "../common/pseudo_overlay_completion.h"

namespace oc = ce::output_completion;
namespace pov = ce::pseudo_overlay;

namespace {

constexpr uint32_t kLastNotificationType = static_cast<uint32_t>(OverlayNotificationType::StreamingEndedAudioVideoDegraded);

}  // namespace

// Session 20260926_041008: only audio content was lost, yet both overlays said
// "Recording saved - video degraded". Each degraded completion now names its track.
TEST(OutputCompletionNotificationTest, DegradedTextNamesTheAffectedTrack) {
    EXPECT_STREQ(oc::DescribeOutputCompletion(OverlayNotificationType::RecordingSavedDegraded).text,
                 "Recording saved - video degraded");
    EXPECT_STREQ(oc::DescribeOutputCompletion(OverlayNotificationType::RecordingSavedAudioDegraded).text,
                 "Recording saved - audio degraded");
    EXPECT_STREQ(oc::DescribeOutputCompletion(OverlayNotificationType::RecordingSavedAudioVideoDegraded).text,
                 "Recording saved - audio and video degraded");
    EXPECT_STREQ(oc::DescribeOutputCompletion(OverlayNotificationType::StreamingEndedDegraded).text,
                 "Stream ended - video degraded");
    EXPECT_STREQ(oc::DescribeOutputCompletion(OverlayNotificationType::StreamingEndedAudioDegraded).text,
                 "Stream ended - audio degraded");
    EXPECT_STREQ(oc::DescribeOutputCompletion(OverlayNotificationType::StreamingEndedAudioVideoDegraded).text,
                 "Stream ended - audio and video degraded");
}

TEST(OutputCompletionNotificationTest, OnlyDegradedAndFailedCompletionsAreWarnings) {
    for (const OverlayNotificationType type : oc::kOutputCompletionNotificationTypes) {
        const auto completion = oc::DescribeOutputCompletion(type);
        ASSERT_NE(completion.text, nullptr) << static_cast<uint32_t>(type);
        const std::string text = completion.text;
        const bool degradedOrFailed =
            text.find("degraded") != std::string::npos || text.find("failed") != std::string::npos;
        EXPECT_EQ(completion.warning, degradedOrFailed) << text;
    }
}

TEST(OutputCompletionNotificationTest, WidthListCoversEveryDescribedCompletion) {
    size_t described = 0;
    for (uint32_t value = 0; value <= kLastNotificationType + 4; ++value) {
        const auto type = static_cast<OverlayNotificationType>(value);
        if (oc::DescribeOutputCompletion(type).text == nullptr) {
            continue;
        }
        ++described;
        bool listed = false;
        for (const OverlayNotificationType candidate : oc::kOutputCompletionNotificationTypes) {
            listed = listed || candidate == type;
        }
        EXPECT_TRUE(listed) << "completion " << value << " missing from the overlay width list";
    }
    EXPECT_EQ(described, std::size(oc::kOutputCompletionNotificationTypes));
}

TEST(OutputCompletionNotificationTest, ScreenshotAndNoneAreNotCompletions) {
    EXPECT_EQ(oc::DescribeOutputCompletion(OverlayNotificationType::None).text, nullptr);
    EXPECT_EQ(oc::DescribeOutputCompletion(OverlayNotificationType::ScreenshotSaved).text, nullptr);
    EXPECT_EQ(oc::DescribeOutputCompletion(OverlayNotificationType::ScreenshotFailed).text, nullptr);
    EXPECT_EQ(oc::DescribeOutputCompletion(OverlayNotificationType::RecordingFinalizing).text, nullptr);
}

// The in-game overlay hides finalization feedback while a newer recording runs. Its old
// numeric range (3..10) let the audio-degraded values (11..14) through during recording.
TEST(OutputCompletionNotificationTest, FinalizationNotificationsIncludeTheAudioDegradedValues) {
    EXPECT_FALSE(oc::IsRecordingFinalizationNotification(static_cast<uint32_t>(OverlayNotificationType::None)));
    EXPECT_FALSE(
        oc::IsRecordingFinalizationNotification(static_cast<uint32_t>(OverlayNotificationType::ScreenshotSaved)));
    EXPECT_FALSE(
        oc::IsRecordingFinalizationNotification(static_cast<uint32_t>(OverlayNotificationType::ScreenshotFailed)));
    for (uint32_t value = static_cast<uint32_t>(OverlayNotificationType::RecordingFinalizing);
         value <= kLastNotificationType; ++value) {
        EXPECT_TRUE(oc::IsRecordingFinalizationNotification(value)) << value;
    }
    EXPECT_FALSE(oc::IsRecordingFinalizationNotification(kLastNotificationType + 1));
}

// The controller pseudo overlay maps the shared notification through its own kinds; every
// completion must come back out as the same notification, so it shows the same text.
TEST(OutputCompletionNotificationTest, PseudoOverlayRoundTripKeepsEveryCompletion) {
    pov::OverlayVisibilityInputs in;
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::Idle;
    in.nowMs = 10'000;
    in.recordingNotifyUntilMs = in.nowMs + 7000;
    for (const OverlayNotificationType type : oc::kOutputCompletionNotificationTypes) {
        in.recordingNotification = ToPseudoRecordingNotification(static_cast<uint32_t>(type));
        ASSERT_NE(in.recordingNotification, pov::RecordingNotificationKind::None) << static_cast<uint32_t>(type);
        EXPECT_EQ(ToOutputCompletionNotification(pov::SelectPseudoOverlayText(in)), type)
            << static_cast<uint32_t>(type);
    }
}

TEST(OutputCompletionNotificationTest, PseudoOverlayAudioDegradedSaveIsNotVideoDegraded) {
    pov::OverlayVisibilityInputs in;
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::Idle;
    in.nowMs = 10'000;
    in.recordingNotifyUntilMs = in.nowMs + 7000;
    in.recordingNotification =
        ToPseudoRecordingNotification(static_cast<uint32_t>(OverlayNotificationType::RecordingSavedAudioDegraded));
    const auto kind = pov::SelectPseudoOverlayText(in);
    EXPECT_EQ(kind, pov::OverlayTextKind::RecordingSavedAudioDegraded);
    const auto completion = oc::DescribeOutputCompletion(ToOutputCompletionNotification(kind));
    ASSERT_NE(completion.text, nullptr);
    EXPECT_EQ(std::strstr(completion.text, "video"), nullptr);
    EXPECT_TRUE(completion.warning);
}
