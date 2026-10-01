#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

// What the title did around a present the bridge absorbed, with timestamps.
//
// Session 20261001_145325: absorbing W3's re-presents ended the dark flashes, but each absorbed
// present was followed by the title's next frame arriving ~20 ms early and a ~45 ms gap after it
// (a ~30 ms hold on screen). Whether that comes from the absorbed call returning at once (a
// forwarded present would block in DLSS-G's hook) or from the title's own Reflex sleep schedule
// is only visible in the order and timing of the title's calls. The timeline keeps the recent
// calls and is printed once two presents after an absorbed one have returned from DLSS-G.
namespace ce::streamline_bridge {

enum class TimelineKind : uint8_t {
    kMarker,        // a 1.x Reflex marker or sleep reached the bridge (`id` = 1.x marker)
    kSleepReturn,   // the title's Reflex sleep returned
    kConstants,     // slSetConstants (`frame`)
    kTag,           // slSetTag
    kEvaluate,      // a non-Reflex slEvaluateFeature (`frame`)
    kPresent,       // a title present reached sl.common's hook (`id` = PresentAction)
    kDlssgReturn,   // sl.dlss_g's present hook returned (or was skipped for an absorbed present)
};

struct TimelineEvent {
    int64_t us = 0;
    TimelineKind kind = TimelineKind::kMarker;
    uint32_t id = 0;
    uint32_t frame = 0;
};

// Not thread-safe; the caller serialises it.
class PresentTimeline {
public:
    static constexpr size_t kCapacity = 128;
    static constexpr uint32_t kPresentsAfterAbsorb = 2;

    void Record(const TimelineEvent& event) {
        events_[next_ % kCapacity] = event;
        ++next_;
    }

    // An absorbed present at `us`: print once two more presents returned from DLSS-G.
    void ArmDump(int64_t absorbedUs) {
        if (dumpArmed_) {
            return;  // the dump already in flight covers this one too
        }
        dumpArmed_ = true;
        absorbedUs_ = absorbedUs;
        presentsToWait_ = kPresentsAfterAbsorb;
    }

    // After a kDlssgReturn of a present that was not absorbed: true when the armed dump is due.
    bool PresentReturned() {
        if (!dumpArmed_ || presentsToWait_ == 0) {
            return false;
        }
        if (--presentsToWait_ != 0) {
            return false;
        }
        dumpArmed_ = false;
        return true;
    }

    int64_t AbsorbedUs() const { return absorbedUs_; }

    // Events from the present before the absorbed one onwards, in ms relative to the absorbed present.
    std::string Format(int64_t absorbedUs) const {
        const size_t count = next_ < kCapacity ? next_ : kCapacity;
        const size_t first = next_ - count;
        size_t start = first;
        for (size_t i = first; i < next_; ++i) {
            const TimelineEvent& e = events_[i % kCapacity];
            if (e.kind == TimelineKind::kPresent && e.us < absorbedUs) {
                start = i;  // the last present before the absorbed one
            }
        }
        std::string out;
        uint32_t tagRun = 0;
        auto flushTags = [&] {
            if (tagRun != 0) {
                out += " tag x" + std::to_string(tagRun);
                tagRun = 0;
            }
        };
        for (size_t i = start; i < next_; ++i) {
            const TimelineEvent& e = events_[i % kCapacity];
            if (e.kind == TimelineKind::kTag) {
                ++tagRun;
                continue;
            }
            flushTags();
            char token[64];
            const double ms = static_cast<double>(e.us - absorbedUs) / 1000.0;
            switch (e.kind) {
            case TimelineKind::kMarker:
                std::snprintf(token, sizeof(token), " %+.1f:%s(%u)", ms, MarkerName(e.id), e.frame);
                break;
            case TimelineKind::kSleepReturn:
                std::snprintf(token, sizeof(token), " %+.1f:sleep-ret", ms);
                break;
            case TimelineKind::kConstants:
                std::snprintf(token, sizeof(token), " %+.1f:consts(%u)", ms, e.frame);
                break;
            case TimelineKind::kEvaluate:
                std::snprintf(token, sizeof(token), " %+.1f:eval(%u)", ms, e.frame);
                break;
            case TimelineKind::kPresent:
                std::snprintf(token, sizeof(token), " %+.1f:PRESENT[%s]", ms, ActionName(e.id));
                break;
            case TimelineKind::kDlssgReturn:
                std::snprintf(token, sizeof(token), " %+.1f:dlssg-ret", ms);
                break;
            case TimelineKind::kTag:
                break;
            }
            out += token;
        }
        flushTags();
        return out;
    }

    static const char* MarkerName(uint32_t id) {
        switch (id) {
        case 0: return "simS";
        case 1: return "simE";
        case 2: return "subS";
        case 3: return "subE";
        case 4: return "prsS";
        case 5: return "prsE";
        case 6: return "input";
        case 7: return "flash";
        case 8: return "ping";
        case 0x1000: return "sleep";
        default: return "marker?";
        }
    }

    // Matches PresentAction's order (kForward, kReMark, kAbsorb).
    static const char* ActionName(uint32_t action) {
        switch (action) {
        case 0: return "fwd";
        case 1: return "re-mark";
        case 2: return "ABSORB";
        default: return "?";
        }
    }

private:
    std::array<TimelineEvent, kCapacity> events_{};
    size_t next_ = 0;
    bool dumpArmed_ = false;
    int64_t absorbedUs_ = 0;
    uint32_t presentsToWait_ = 0;
};

}  // namespace ce::streamline_bridge
