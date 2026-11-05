#pragma once

#include "overlay_layout_policy.h"
#include "overlay_panel.h"

#include <algorithm>
#include <cmath>

namespace ce::overlay_panel::detail {

using namespace ce::overlay_layout;

// Spacing in pixels at 100% scale with the default 14 px font.
inline constexpr float kPadX = 10.0f;
inline constexpr float kPadTop = 7.0f;
inline constexpr float kPadBottom = 8.0f;
inline constexpr float kGroupGap = 7.0f;
inline constexpr float kColumnGap = 13.0f;
inline constexpr float kLabelGap = 18.0f;
inline constexpr float kInlineGap = 8.0f;
inline constexpr float kChipPadX = 6.0f;
inline constexpr float kGraphHeight = 30.0f;
inline constexpr float kBarHeight = 7.0f;
inline constexpr float kMinContentWidth = 210.0f;

// 85% is the threshold the load colors always used. It is amber, never red: 99% GPU load is normal in a
// GPU-limited game. Memory near its total is what really hurts, so that one reaches red.
inline constexpr float kLoadWarnPercent = 85.0f;
inline constexpr float kMemoryWarnRatio = 0.90f;
inline constexpr float kMemoryCritRatio = 0.97f;

struct Short {
    char s[80] = "";
    const char* c() const {
        return s;
    }
};

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
Short Fmt(const char* format, ...);

Short Percent(const Reading& reading);
Short Degrees(const Reading& reading);
Short Whole(const Reading& reading);

inline Tone ReadingTone(const Reading& reading) {
    return reading.valid ? Tone::Value : Tone::Unit;
}
inline Tone LoadTone(float percent) {
    return percent >= kLoadWarnPercent ? Tone::Warn : Tone::Value;
}
inline Tone LoadReadingTone(const Reading& reading) {
    return reading.valid ? LoadTone(reading.value) : Tone::Unit;
}
Tone MemoryTone(const MemoryValue& memory);

class Composer {
public:
    Composer(const Inputs& inputs, const TextMeasure& measure, Scene& scene);

    const Inputs& in;
    const float left;

    float Px(float layoutPixels) const {
        return std::round(layoutPixels * in.unit);
    }
    bool Has(uint32_t row) const {
        return (in.rowMask & row) != 0;
    }
    bool Column(uint32_t column) const {
        return (in.sensorColumns & column) != 0;
    }
    const FontMetrics& Font(Size size) const {
        return fonts_[static_cast<int>(size)];
    }
    float W(const char* text, Size size = Size::Base) const {
        return measure_.Width(text, size);
    }
    float W(const Short& text, Size size = Size::Base) const {
        return measure_.Width(text.c(), size);
    }
    float Pitch(Size size) const;
    float FitWidth(float desired) const;
    // Wraps at word boundaries, then glyph boundaries for a long word. Returns the next free y.
    float WrappedText(float x, float top, float width, const char* text, Size size, Tone tone);
    float WrappedHeight(float width, const char* text, Size size) const;
    // Where text sits in a row: the row centers on digit ink, because line height varies a lot by font.
    float Baseline(float top, float pitch, Size size = Size::Base) const {
        return top + std::round((pitch + Font(size).digitHeight) * 0.5f);
    }

    void Text(float x, float baseline, const char* text, Size size, Tone tone);
    void TextRight(float right, float baseline, const char* text, Size size, Tone tone);
    void Rect(float x, float y, float width, float height, Fill fill);
    // A number and its dimmer unit, right-aligned as one: "135 W".
    void ValueUnitRight(float right, float baseline, const char* value, const char* unit, Size size, Tone valueTone);
    float ValueUnitWidth(const char* value, const char* unit, Size size = Size::Base) const;

    void Finish(float contentWidth, float bottom);
    Scene& MutableScene() {
        return scene_;
    }

private:
    const TextMeasure& measure_;
    Scene& scene_;
    FontMetrics fonts_[3];
};

// A row of quiet readings with even gaps: "mem 17001 MHz   0.920 V   769 RPM".
class Line {
public:
    void Add(const Short& text, Tone tone = Tone::Unit, const Short& reserved = {}) {
        if (count_ < kCapacity) {
            items_[count_].text = text;
            items_[count_].reserved = reserved.s[0] ? reserved : text;
            items_[count_].tone = tone;
            ++count_;
        }
    }
    bool Empty() const {
        return count_ == 0;
    }
    float Width(const Composer& c, Size size) const;
    void Draw(Composer& c, float x, float baseline, Size size) const;
    void DrawRight(Composer& c, float right, float baseline, Size size) const;
    float DrawWrapped(Composer& c, float x, float top, float width, Size size) const;

private:
    static constexpr int kCapacity = 5;
    struct Item {
        Short text;
        Short reserved;
        Tone tone = Tone::Unit;
    };
    Item items_[kCapacity];
    int count_ = 0;
};

// Lines carry their reserved field widths alongside the current text, including unavailable readings.
Line GpuDetailLine(const Inputs& in);  // mem clock, voltage, fan: the core clock has a column
Line CpuDetailLine(const Inputs& in);  // peak core load and clock
Line GpuClocksLine(const Inputs& in, bool withFan);  // "2910 / 17001 MHz", "0.920 V"
Line CpuClocksLine(const Inputs& in, bool withPeak);  // "4191 / 4569 MHz"

// Sections more than one layout shares. Each Draw takes the row's top and returns the next free y.
float NotificationWidth(const Composer& c);
float DrawNotification(Composer& c, float y, float width);

// The slot for recording state: a filled chip when live, amber text while starting.
float RecordingSlotWidth(const Composer& c);
float RecordingWarningWidth(const Composer& c);
bool HasRecordingWarning(const Composer& c);
void DrawRecordingSlotRight(Composer& c, float right, float top);
float DrawRecordingFullRow(Composer& c, float y, float width);  // The chip across the panel, with any warning.
Short RecordingChipText(const Composer& c);
float DrawChip(Composer& c, float x, float top, float width, const char* text);

float FgRowWidth(const Composer& c);
float DrawFgRow(Composer& c, float y, float width);
float FgStatusWidth(const Composer& c, Size size);
Short FgStatusText(const Composer& c);

float GraphSectionWidth(const Composer& c);
float DrawGraphSection(Composer& c, float y, float width);

float BenchmarkWidth(const Composer& c);
float DrawBenchmark(Composer& c, float y, float width);

// "4.31 / 11.66 GB": the used amount right-aligned in a slot wide enough for the total's digits.
struct MemoryParts {
    Short used;
    Short rest;
    Tone usedTone = Tone::Value;
    float usedSlot = 0.0f;
    float spaceWidth = 0.0f;
};
MemoryParts MakeMemoryParts(const Composer& c, const MemoryValue& memory, Size size);
float MemoryPartsWidth(const Composer& c, const MemoryParts& parts, Size size);
void DrawMemoryParts(Composer& c, float x, float baseline, const MemoryParts& parts, Size size);
float DrawWrappedMemory(Composer& c, float y, float width, const char* label, const MemoryValue& memory);
float DrawWrappedHardware(Composer& c, float y, float width, bool gpu, Layout layout);
float DrawWrappedFrame(Composer& c, float y, float width, bool list);
void DrawBar(Composer& c, float x, float baseline, float width, float percent, Tone tone, float mark = -1.0f);

void BuildHero(Composer& c);
void BuildList(Composer& c);
void BuildMeters(Composer& c);
void BuildStrip(Composer& c);

}  // namespace ce::overlay_panel::detail
