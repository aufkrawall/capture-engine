#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace testapp::avsync {

constexpr int kDefaultWidth = 1920;
constexpr int kDefaultHeight = 1080;
constexpr int kDefaultFps = 120;
constexpr int kDefaultDurationSeconds = 40;
constexpr double kEventPeriodSeconds = 1.0;
constexpr double kPreStartSeconds = 1.0;
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr int kFrameMarkerBits = 16;

struct Rgb8 {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

struct StimulusState {
    int eventIndex;
    int paletteIndex;
    double eventStartSeconds;
    double eventEndSeconds;
    double frequencyHz;
    Rgb8 color;
};

inline constexpr std::array<double, 16> kFrequenciesHz = {
    440.0, 550.0, 660.0, 770.0, 880.0, 990.0, 1100.0, 1210.0,
    495.0, 605.0, 715.0, 825.0, 935.0, 1045.0, 1155.0, 1265.0,
};

inline constexpr std::array<Rgb8, 16> kPalette = {{
    {232, 48, 48},
    {48, 204, 88},
    {64, 112, 240},
    {232, 208, 48},
    {216, 72, 216},
    {48, 212, 220},
    {248, 136, 40},
    {152, 96, 240},
    {160, 48, 64},
    {56, 148, 64},
    {48, 80, 168},
    {160, 144, 48},
    {152, 56, 152},
    {48, 152, 160},
    {176, 96, 48},
    {96, 72, 176},
}};

inline int EventIndexAt(double stimulusSeconds) {
    if (stimulusSeconds < 0.0) {
        return -1;
    }
    return static_cast<int>(std::floor(stimulusSeconds / kEventPeriodSeconds));
}

inline int PaletteIndexForEvent(int eventIndex) {
    if (eventIndex < 0) {
        return -1;
    }
    return eventIndex % static_cast<int>(kPalette.size());
}

inline StimulusState StateAt(double stimulusSeconds) {
    const int eventIndex = EventIndexAt(stimulusSeconds);
    if (eventIndex < 0) {
        return {-1, -1, 0.0, 0.0, 0.0, {0, 0, 0}};
    }
    const int paletteIndex = PaletteIndexForEvent(eventIndex);
    const double eventStart = static_cast<double>(eventIndex) * kEventPeriodSeconds;
    return {eventIndex,
            paletteIndex,
            eventStart,
            eventStart + kEventPeriodSeconds,
            kFrequenciesHz[static_cast<size_t>(paletteIndex)],
            kPalette[static_cast<size_t>(paletteIndex)]};
}

inline uint16_t EncodeFrameMarker(uint64_t frameId) {
    return static_cast<uint16_t>(frameId & 0xffffu);
}

inline bool FrameMarkerBit(uint16_t marker, int bitIndex) {
    if (bitIndex < 0 || bitIndex >= kFrameMarkerBits) {
        return false;
    }
    return ((marker >> bitIndex) & 1u) != 0;
}

inline uint16_t DecodeFrameMarkerBits(const bool* bits, int bitCount) {
    uint16_t marker = 0;
    const int count = std::min(bitCount, kFrameMarkerBits);
    for (int i = 0; i < count; ++i) {
        if (bits[i]) {
            marker |= static_cast<uint16_t>(1u << i);
        }
    }
    return marker;
}

inline double SmoothLanePosition(double stimulusSeconds) {
    if (stimulusSeconds < 0.0) {
        return 0.0;
    }
    const double cycle = std::fmod(stimulusSeconds * 0.25, 1.0);
    return cycle < 0.0 ? cycle + 1.0 : cycle;
}

inline double SegmentCycleBase(int eventIndex) {
    if (eventIndex <= 0) {
        return 0.0;
    }
    double cycles = 0.0;
    for (int i = 0; i < eventIndex; ++i) {
        cycles += kFrequenciesHz[static_cast<size_t>(PaletteIndexForEvent(i))] * kEventPeriodSeconds;
    }
    return cycles;
}

inline float AudioSampleAt(double stimulusSeconds, int channelIndex, int channelCount) {
    if (stimulusSeconds < 0.0) {
        return 0.0f;
    }
    const StimulusState state = StateAt(stimulusSeconds);
    const double localTime = stimulusSeconds - state.eventStartSeconds;
    const double cycles = SegmentCycleBase(state.eventIndex) + state.frequencyHz * localTime;
    const double channelScale = channelCount <= 1 ? 1.0 : (channelIndex == 0 ? 1.0 : 0.82);
    const double sample = std::sin(kTwoPi * cycles) * 0.20 * channelScale;
    return static_cast<float>(sample);
}

inline int NearestPaletteIndex(uint8_t r, uint8_t g, uint8_t b) {
    int best = 0;
    int bestDist = 0x7fffffff;
    for (size_t i = 0; i < kPalette.size(); ++i) {
        const int dr = static_cast<int>(r) - static_cast<int>(kPalette[i].r);
        const int dg = static_cast<int>(g) - static_cast<int>(kPalette[i].g);
        const int db = static_cast<int>(b) - static_cast<int>(kPalette[i].b);
        const int dist = dr * dr + dg * dg + db * db;
        if (dist < bestDist) {
            bestDist = dist;
            best = static_cast<int>(i);
        }
    }
    return best;
}

}  // namespace testapp::avsync
