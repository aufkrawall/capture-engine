#include "mediaengine/audio/capture_glitch_monitor.h"

#include <gtest/gtest.h>

#include <vector>

using ce::audio::CaptureGlitchEvent;
using ce::audio::CaptureGlitchMonitor;
using ce::audio::CaptureGlitchReport;

namespace {

constexpr uint32_t kRate = 48'000;
constexpr uint32_t kPacket = 480;  // 10 ms

// Feeds `seconds` of 10 ms packets from `position`, flagging every `flagEvery`th
// (0: none); returns the reports that carried an event and advances `position`.
std::vector<CaptureGlitchReport> Feed(CaptureGlitchMonitor& monitor, uint64_t& position, double seconds,
                                      uint32_t flagEvery) {
    std::vector<CaptureGlitchReport> events;
    const uint64_t packets = static_cast<uint64_t>(seconds * kRate / kPacket);
    for (uint64_t i = 0; i < packets; ++i) {
        const bool flagged = flagEvery != 0 && i % flagEvery == flagEvery - 1;
        const CaptureGlitchReport report = monitor.Observe(position, kPacket, flagged);
        if (report.event != CaptureGlitchEvent::kNone) {
            events.push_back(report);
        }
        position += kPacket;
    }
    return events;
}

}  // namespace

TEST(CaptureGlitchMonitorTest, OneDiscontinuityAtStreamStartIsNotAnEpisode) {
    CaptureGlitchMonitor monitor;
    monitor.Begin(kRate);
    uint64_t position = 0;
    EXPECT_EQ(monitor.Observe(position, kPacket, true).event, CaptureGlitchEvent::kNone);
    position += kPacket;
    EXPECT_TRUE(Feed(monitor, position, 30.0, 0).empty());
    EXPECT_EQ(monitor.Finish().event, CaptureGlitchEvent::kNone);
    EXPECT_EQ(monitor.totalDiscontinuities(), 1u);
    EXPECT_EQ(monitor.episodes(), 0u);
    EXPECT_EQ(monitor.glitchingSeconds(), 0.0);
}

// HotS 2026-10-28: the face camera's microphone flagged every fourth 10 ms packet.
TEST(CaptureGlitchMonitorTest, AMicrophoneGlitchingEvery40MsIsReportedThroughItsEpisode) {
    CaptureGlitchMonitor monitor;
    monitor.Begin(kRate);
    uint64_t position = 1'440;  // the mic's first flagged packet sat at devPos=1440

    const std::vector<CaptureGlitchReport> glitching = Feed(monitor, position, 40.0, 4);
    ASSERT_EQ(glitching.size(), 2u);  // started, then once after kReportEveryWindows more windows
    EXPECT_EQ(glitching[0].event, CaptureGlitchEvent::kStarted);
    EXPECT_EQ(glitching[0].windowDiscontinuities, 125u);
    EXPECT_DOUBLE_EQ(glitching[0].windowSeconds, CaptureGlitchMonitor::kWindowSeconds);
    EXPECT_DOUBLE_EQ(glitching[0].typicalSpacingMs, 40.0);
    EXPECT_EQ(glitching[1].event, CaptureGlitchEvent::kContinuing);
    EXPECT_DOUBLE_EQ(glitching[1].episodeSeconds, 35.0);
    EXPECT_EQ(glitching[1].episodeDiscontinuities, 875u);

    const std::vector<CaptureGlitchReport> healthy = Feed(monitor, position, 10.0, 0);
    ASSERT_EQ(healthy.size(), 1u);
    EXPECT_EQ(healthy[0].event, CaptureGlitchEvent::kEnded);
    EXPECT_DOUBLE_EQ(healthy[0].episodeSeconds, 40.0);
    EXPECT_EQ(healthy[0].episodeDiscontinuities, 1'000u);
    EXPECT_EQ(monitor.episodes(), 1u);
    EXPECT_DOUBLE_EQ(monitor.glitchingSeconds(), 40.0);
}

TEST(CaptureGlitchMonitorTest, StoppingInsideAnEpisodeEndsIt) {
    CaptureGlitchMonitor monitor;
    monitor.Begin(kRate);
    uint64_t position = 0;
    ASSERT_EQ(Feed(monitor, position, 12.0, 4).size(), 1u);
    const CaptureGlitchReport ended = monitor.Finish();
    EXPECT_EQ(ended.event, CaptureGlitchEvent::kEnded);
    EXPECT_DOUBLE_EQ(ended.episodeSeconds, 10.0);  // through the last full window
    EXPECT_EQ(ended.episodeDiscontinuities, 300u);
    EXPECT_EQ(monitor.Finish().event, CaptureGlitchEvent::kNone);
}

TEST(CaptureGlitchMonitorTest, AReactivatedStreamEndsTheEpisodeAndRestartsItsPositions) {
    CaptureGlitchMonitor monitor;
    monitor.Begin(kRate);
    uint64_t position = 0;
    ASSERT_EQ(Feed(monitor, position, 5.0, 4).size(), 1u);
    EXPECT_EQ(monitor.Begin(kRate).event, CaptureGlitchEvent::kEnded);
    position = 0;
    EXPECT_TRUE(Feed(monitor, position, 20.0, 0).empty());
    EXPECT_EQ(monitor.episodes(), 1u);
}

TEST(CaptureGlitchMonitorTest, APositionThatWentBackRestartsTheStream) {
    CaptureGlitchMonitor monitor;
    monitor.Begin(kRate);
    uint64_t position = 100'000;
    ASSERT_EQ(Feed(monitor, position, 6.0, 4).size(), 1u);
    position = 0;
    const std::vector<CaptureGlitchReport> events = Feed(monitor, position, 1.0, 0);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].event, CaptureGlitchEvent::kEnded);
}

TEST(CaptureGlitchMonitorTest, WithoutASampleRateTheMonitorStaysOut) {
    CaptureGlitchMonitor monitor;
    monitor.Begin(0);
    uint64_t position = 0;
    EXPECT_TRUE(Feed(monitor, position, 10.0, 1).empty());
    EXPECT_EQ(monitor.totalDiscontinuities(), 0u);
}
