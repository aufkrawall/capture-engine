#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace testapp::avsync {

constexpr int kDefaultWidth = 1920;
constexpr int kDefaultHeight = 1080;
constexpr int kDefaultFps = 120;
constexpr int kDefaultDurationSeconds = 40;
constexpr double kEventPeriodSeconds = 1.0;
constexpr double kPreStartSeconds = 1.0;
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr int kFrameMarkerBits = 16;
constexpr int kVisualMarkerVersion = 2;
constexpr int kDefaultAudioBufferMs = 20;
constexpr int kMinAudioBufferMs = 5;
constexpr int kMaxAudioBufferMs = 500;
constexpr double kDefaultAudioLeadMs = 75.0;
constexpr double kMinAudioLeadMs = -500.0;
constexpr double kMaxAudioLeadMs = 500.0;
constexpr double kDefaultAnalysisStartSeconds = 2.0;
constexpr double kDefaultSourceStallToleranceSeconds = 0.050;

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

struct EncoderStressLayout {
    int tile = 0;
    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;
    int columns = 0;
    int rows = 0;
    int reserveLeft = 0;
    int reserveRight = 0;
    int reserveTop = 0;
    int reserveBottom = 0;
    bool valid = false;
};

inline EncoderStressLayout ComputeEncoderStressLayout(int width, int height, int markerMargin, int markerTile,
                                                      int markerGap) {
    EncoderStressLayout layout{};
    if (width <= 0 || height <= 0) {
        return layout;
    }

    layout.tile = std::clamp(width / 80, 24, 64);
    layout.left = markerMargin;
    const int maxRight = width - markerMargin;
    layout.top = markerMargin + 2 * (markerTile + markerGap) + 18;
    const int maxBottom = std::min(static_cast<int>(height * 0.68), height - markerMargin - 96);
    layout.reserveLeft = static_cast<int>(width * 0.40);
    layout.reserveRight = static_cast<int>(width * 0.60);
    layout.reserveTop = static_cast<int>(height * 0.38);
    layout.reserveBottom = static_cast<int>(height * 0.52);

    const int usableWidth = maxRight - layout.left;
    const int usableHeight = maxBottom - layout.top;
    if (usableWidth < layout.tile || usableHeight < layout.tile) {
        return layout;
    }

    layout.columns = usableWidth / layout.tile;
    layout.rows = usableHeight / layout.tile;
    if (layout.columns <= 0 || layout.rows <= 0) {
        return layout;
    }

    layout.right = layout.left + layout.columns * layout.tile;
    layout.bottom = layout.top + layout.rows * layout.tile;
    layout.valid = layout.right > layout.left && layout.bottom > layout.top;
    return layout;
}

struct SourceStallSpec {
    double startSeconds = 0.0;
    double durationSeconds = 0.0;
    bool valid = false;

    double EndSeconds() const {
        return startSeconds + durationSeconds;
    }
};

inline constexpr std::array<double, 16> kFrequenciesHz = {
    440.0, 550.0, 660.0, 770.0, 880.0, 990.0, 1100.0, 1210.0, 495.0, 605.0, 715.0, 825.0, 935.0, 1045.0, 1155.0, 1265.0,
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

inline uint8_t FrameMarkerChecksum(uint16_t marker, int paletteIndex) {
    return static_cast<uint8_t>((marker & 0xffu) ^ ((marker >> 8) & 0xffu) ^
                                static_cast<uint8_t>(paletteIndex < 0 ? 0 : paletteIndex));
}

inline bool FrameMarkerParity(uint16_t marker, int paletteIndex) {
    uint32_t value = static_cast<uint32_t>(marker) ^ static_cast<uint32_t>(FrameMarkerChecksum(marker, paletteIndex));
    bool parity = false;
    while (value) {
        parity = !parity;
        value &= value - 1u;
    }
    return parity;
}

inline bool FrameMarkerBit(uint16_t marker, int bitIndex) {
    if (bitIndex < 0 || bitIndex >= kFrameMarkerBits) {
        return false;
    }
    return ((marker >> bitIndex) & 1u) != 0;
}

inline uint32_t EncoderStressTileHash(uint32_t tileX, uint32_t tileY, uint64_t frameId, int paletteIndex) {
    uint32_t value = 0x9e3779b9u;
    value ^= tileX + 0x85ebca6bu + (value << 6) + (value >> 2);
    value ^= tileY + 0xc2b2ae35u + (value << 6) + (value >> 2);
    value ^= static_cast<uint32_t>(frameId) + 0x27d4eb2fu + (value << 6) + (value >> 2);
    value ^= static_cast<uint32_t>(frameId >> 32) + 0x165667b1u + (value << 6) + (value >> 2);
    value ^= static_cast<uint32_t>(paletteIndex < 0 ? 0 : paletteIndex) * 0x7feb352du;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
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

inline double FastLanePosition(double stimulusSeconds) {
    if (stimulusSeconds < 0.0) {
        return 0.0;
    }
    const double cycle = std::fmod(stimulusSeconds * 1.0, 1.0);
    return cycle < 0.0 ? cycle + 1.0 : cycle;
}

inline double ExpectedMotionPosition(double stimulusSeconds) {
    return SmoothLanePosition(stimulusSeconds);
}

inline int ClampAudioBufferMs(int value) {
    return std::clamp(value, kMinAudioBufferMs, kMaxAudioBufferMs);
}

inline double ClampAudioLeadMs(double value) {
    return std::clamp(value, kMinAudioLeadMs, kMaxAudioLeadMs);
}

inline bool IsFiniteDoubleBits(double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7ff0000000000000ull) != 0x7ff0000000000000ull;
}

inline double ClampAnalysisStartSeconds(double value, double durationSeconds) {
    if (!IsFiniteDoubleBits(value)) {
        return kDefaultAnalysisStartSeconds;
    }
    if (value < 0.0) {
        return 0.0;
    }
    const double maxDuration = IsFiniteDoubleBits(durationSeconds) ? std::max(0.0, durationSeconds) : 0.0;
    return value > maxDuration ? maxDuration : value;
}

inline bool ShouldUseDxgiTearing(bool requested, bool supported, bool vsyncEnabled) {
    return requested && supported && !vsyncEnabled;
}

inline bool ParseSourceStallSpec(const std::string& text, SourceStallSpec* out) {
    if (!out) {
        return false;
    }
    *out = {};
    const size_t colon = text.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
        return false;
    }

    char* end = nullptr;
    const std::string startText = text.substr(0, colon);
    const double start = std::strtod(startText.c_str(), &end);
    if (!end || *end != '\0' || start < 0.0 || !std::isfinite(start)) {
        return false;
    }
    end = nullptr;
    const std::string durationText = text.substr(colon + 1);
    const double durationMs = std::strtod(durationText.c_str(), &end);
    if (!end || *end != '\0' || durationMs <= 0.0 || !std::isfinite(durationMs)) {
        return false;
    }

    out->startSeconds = start;
    out->durationSeconds = durationMs / 1000.0;
    out->valid = true;
    return true;
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
