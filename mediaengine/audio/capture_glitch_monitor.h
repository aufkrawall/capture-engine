#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// One WASAPI capture stream's data discontinuities, judged per window of device
// time.
//
// A capture endpoint flags AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY when its data
// is not continuous with the previous packet. Once at stream start is normal; a
// device that keeps doing it is losing capture data. Heroes of the Storm,
// 2026-10-28: the face camera's built-in USB microphone flagged one every 40 ms
// for whole recordings, and the game's Present stalled on that same 40 ms clock
// while CE held the microphone open (llm-wiki/log/recent.md). The per-packet
// log stops after 8 lines, so such a device looked healthy after its first
// 300 ms. The monitor reports episodes instead: the first window holding
// kEpisodeMinPerWindow or more, every kReportEveryWindows windows while the
// episode lasts, and the window that falls below again.
namespace ce::audio {

enum class CaptureGlitchEvent : uint8_t {
    kNone,
    kStarted,     // a window crossed the threshold
    kContinuing,  // still glitching, kReportEveryWindows windows since the last report
    kEnded,       // a window fell below, or the stream ended inside an episode
};

struct CaptureGlitchReport {
    CaptureGlitchEvent event = CaptureGlitchEvent::kNone;
    bool streamEnded = false;  // kEnded by a stop or re-activation, not by the device recovering
    uint32_t windowDiscontinuities = 0;
    double windowSeconds = 0;
    double typicalSpacingMs = 0;  // median device time between flagged packets in the window, 0 below two
    uint64_t episodeDiscontinuities = 0;
    double episodeSeconds = 0;
};

class CaptureGlitchMonitor {
public:
    static constexpr double kWindowSeconds = 5.0;
    static constexpr uint32_t kEpisodeMinPerWindow = 10;  // two per second
    static constexpr uint32_t kReportEveryWindows = 6;    // 30 s
    static constexpr size_t kMaxSpacings = 128;

    // A fresh stream (start or re-activation): device positions restart. An
    // episode still open is ended first; the caller reports it.
    CaptureGlitchReport Begin(uint32_t sampleRate) {
        CaptureGlitchReport report = Finish();
        sampleRate_ = sampleRate;
        windowFrames_ = static_cast<uint64_t>(kWindowSeconds * static_cast<double>(sampleRate));
        started_ = false;
        return report;
    }

    CaptureGlitchReport Observe(uint64_t devicePosition, uint32_t frames, bool discontinuity) {
        if (sampleRate_ == 0) {
            return {};
        }
        // A position that went back is a stream the caller did not Begin.
        const CaptureGlitchReport restart =
            started_ && devicePosition < windowStart_ ? Begin(sampleRate_) : CaptureGlitchReport{};
        if (!started_) {
            started_ = true;
            OpenWindow(devicePosition);
        }
        if (discontinuity) {
            ++totalDiscontinuities_;
            ++windowDiscontinuities_;
            if (hasLastFlag_ && devicePosition > lastFlagPosition_ && spacings_.size() < kMaxSpacings) {
                spacings_.push_back(devicePosition - lastFlagPosition_);
            }
            hasLastFlag_ = true;
            lastFlagPosition_ = devicePosition;
        }
        const uint64_t end = devicePosition + frames;
        if (end - windowStart_ < windowFrames_) {
            return restart;
        }
        const CaptureGlitchReport closed = CloseWindow(end);
        return closed.event != CaptureGlitchEvent::kNone ? closed : restart;
    }

    // The stream ended: an open episode ends with it, through the last full window.
    CaptureGlitchReport Finish() {
        if (!inEpisode_) {
            return {};
        }
        inEpisode_ = false;
        CaptureGlitchReport report;
        report.event = CaptureGlitchEvent::kEnded;
        report.streamEnded = true;
        report.episodeDiscontinuities = episodeDiscontinuities_ + windowDiscontinuities_;
        report.episodeSeconds = Seconds(windowStart_ - episodeStart_);
        glitchingFrames_ += windowStart_ - episodeStart_;
        return report;
    }

    uint64_t totalDiscontinuities() const { return totalDiscontinuities_; }
    uint32_t episodes() const { return episodes_; }
    double glitchingSeconds() const {
        return Seconds(glitchingFrames_ + (inEpisode_ ? windowStart_ - episodeStart_ : 0));
    }

private:
    void OpenWindow(uint64_t position) {
        windowStart_ = position;
        windowDiscontinuities_ = 0;
        spacings_.clear();
        hasLastFlag_ = false;
    }

    CaptureGlitchReport CloseWindow(uint64_t end) {
        CaptureGlitchReport report;
        report.windowDiscontinuities = windowDiscontinuities_;
        report.windowSeconds = Seconds(end - windowStart_);
        if (!spacings_.empty()) {
            auto middle = spacings_.begin() + static_cast<std::ptrdiff_t>(spacings_.size() / 2);
            std::nth_element(spacings_.begin(), middle, spacings_.end());
            report.typicalSpacingMs = Seconds(*middle) * 1000.0;
        }
        const bool glitching = windowDiscontinuities_ >= kEpisodeMinPerWindow;
        if (glitching && !inEpisode_) {
            inEpisode_ = true;
            ++episodes_;
            episodeStart_ = windowStart_;
            episodeDiscontinuities_ = 0;
            windowsSinceReport_ = 0;
            report.event = CaptureGlitchEvent::kStarted;
        } else if (glitching) {
            if (++windowsSinceReport_ >= kReportEveryWindows) {
                windowsSinceReport_ = 0;
                report.event = CaptureGlitchEvent::kContinuing;
            }
        } else if (inEpisode_) {
            inEpisode_ = false;
            glitchingFrames_ += windowStart_ - episodeStart_;
            report.event = CaptureGlitchEvent::kEnded;
            report.episodeDiscontinuities = episodeDiscontinuities_ + windowDiscontinuities_;
            report.episodeSeconds = Seconds(windowStart_ - episodeStart_);
        }
        if (inEpisode_) {
            episodeDiscontinuities_ += windowDiscontinuities_;
            report.episodeDiscontinuities = episodeDiscontinuities_;
            report.episodeSeconds = Seconds(end - episodeStart_);
        }
        OpenWindow(end);
        return report;
    }

    double Seconds(uint64_t frames) const {
        return sampleRate_ != 0 ? static_cast<double>(frames) / static_cast<double>(sampleRate_) : 0.0;
    }

    uint32_t sampleRate_ = 0;
    uint64_t windowFrames_ = 0;
    bool started_ = false;
    uint64_t windowStart_ = 0;  // also the end of the last full window: an episode's extent so far
    uint32_t windowDiscontinuities_ = 0;
    std::vector<uint64_t> spacings_;
    bool hasLastFlag_ = false;
    uint64_t lastFlagPosition_ = 0;
    bool inEpisode_ = false;
    uint64_t episodeStart_ = 0;
    uint64_t episodeDiscontinuities_ = 0;
    uint32_t windowsSinceReport_ = 0;
    uint64_t totalDiscontinuities_ = 0;
    uint32_t episodes_ = 0;
    uint64_t glitchingFrames_ = 0;
};

}  // namespace ce::audio
