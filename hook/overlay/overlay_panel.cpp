#include "overlay_panel_internal.h"

#include <cstdarg>
#include <cstdio>

namespace ce::overlay_panel {

namespace detail {

Short Fmt(const char* format, ...) {
    Short out;
    va_list args;
    va_start(args, format);
    std::vsnprintf(out.s, sizeof(out.s), format, args);
    va_end(args);
    return out;
}

Short Percent(const Reading& reading) {
    return reading.valid ? Fmt("%.0f%%", reading.value) : Fmt("--");
}

Short Degrees(const Reading& reading) {
    return reading.valid ? Fmt("%.0f\xB0", reading.value) : Fmt("--");
}

Short Whole(const Reading& reading) {
    return reading.valid ? Fmt("%.0f", reading.value) : Fmt("--");
}

Tone MemoryTone(const MemoryValue& memory) {
    if (!memory.available)
        return Tone::Unit;
    if (!memory.hasTotal || memory.totalGb <= 0.0f)
        return Tone::Value;
    const float ratio = memory.usedGb / memory.totalGb;
    if (ratio >= kMemoryCritRatio)
        return Tone::Crit;
    return ratio >= kMemoryWarnRatio ? Tone::Warn : Tone::Value;
}

Composer::Composer(const Inputs& inputs, const TextMeasure& measure, Scene& scene)
    : in(inputs), left(std::round(kPadX * inputs.unit)), measure_(measure), scene_(scene) {
    for (const Size size : {Size::Base, Size::Small, Size::Hero})
        fonts_[static_cast<int>(size)] = measure.Font(size);
}

float Composer::Pitch(Size size) const {
    switch (size) {
        case Size::Small:
            return std::round(Font(size).pixelSize * 1.36f);
        case Size::Hero:
            return std::round(Font(size).digitHeight + Px(4.0f));
        case Size::Base:
            break;
    }
    return std::round(Font(Size::Base).pixelSize * 1.4f);
}

void Composer::Text(float x, float baseline, const char* text, Size size, Tone tone) {
    if (!text || !text[0])
        return;
    TextOp& op = scene_.texts.emplace_back();
    op.x = std::round(x);
    op.y = std::round(baseline - Font(size).ascent);
    op.size = size;
    op.tone = tone;
    std::snprintf(op.text, sizeof(op.text), "%s", text);
}

void Composer::TextRight(float right, float baseline, const char* text, Size size, Tone tone) {
    Text(std::floor(right - W(text, size)), baseline, text, size, tone);
}

void Composer::Rect(float x, float y, float width, float height, Fill fill) {
    RectOp& op = scene_.rects.emplace_back();
    op.x = std::round(x);
    op.y = std::round(y);
    op.width = std::round(width);
    op.height = std::round(height);
    op.fill = fill;
}

float Composer::ValueUnitWidth(const char* value, const char* unit, Size size) const {
    if (!unit || !unit[0])
        return W(value, size);
    return W(value, size) + W(" ", size) + W(unit, size);
}

void Composer::ValueUnitRight(float right, float baseline, const char* value, const char* unit, Size size,
                              Tone valueTone) {
    if (unit && unit[0]) {
        TextRight(right, baseline, unit, size, Tone::Unit);
        right -= W(unit, size) + W(" ", size);
    }
    TextRight(right, baseline, value, size, valueTone);
}

void Composer::Finish(float contentWidth, float bottom) {
    scene_.width = contentWidth + left * 2.0f;
    scene_.height = bottom + Px(kPadBottom);
}

float Line::Width(const Composer& c, Size size) const {
    float width = 0.0f;
    for (int i = 0; i < count_; ++i)
        width += c.W(items_[i].text, size) + (i > 0 ? c.Px(kInlineGap) : 0.0f);
    return width;
}

void Line::Draw(Composer& c, float x, float baseline, Size size) const {
    for (int i = 0; i < count_; ++i) {
        c.Text(x, baseline, items_[i].text.c(), size, items_[i].tone);
        x += c.W(items_[i].text, size) + c.Px(kInlineGap);
    }
}

void Line::DrawRight(Composer& c, float right, float baseline, Size size) const {
    for (int i = count_ - 1; i >= 0; --i) {
        c.TextRight(right, baseline, items_[i].text.c(), size, items_[i].tone);
        right -= c.W(items_[i].text, size) + c.Px(kInlineGap);
    }
}

Line GpuDetailLine(const Inputs& in, bool reserve) {
    Line line;
    if (in.gpu.memoryMhz.valid)
        line.Add(Fmt("mem %.0f MHz", reserve ? 99999.0f : in.gpu.memoryMhz.value));
    if (in.gpu.voltageV.valid)
        line.Add(Fmt("%.3f V", reserve ? 9.999f : in.gpu.voltageV.value));
    if (in.gpu.fanRpm.valid)
        line.Add(Fmt("%.0f RPM", reserve ? 9999.0f : in.gpu.fanRpm.value));
    return line;
}

Line CpuDetailLine(const Inputs& in, bool reserve) {
    Line line;
    const float peak = reserve ? 100.0f : in.cpu.peakCoreLoad;
    line.Add(Fmt("peak core %.0f%%", peak), LoadTone(in.cpu.peakCoreLoad) == Tone::Warn ? Tone::Warn : Tone::Unit);
    if (in.cpu.peakCoreMhz.valid)
        line.Add(Fmt("%.0f MHz", reserve ? 9999.0f : in.cpu.peakCoreMhz.value));
    return line;
}

namespace {

Short TwoClocks(const Reading& first, const Reading& second, float firstReserve, float secondReserve, bool reserve) {
    const float a = reserve ? firstReserve : first.value;
    const float b = reserve ? secondReserve : second.value;
    if (first.valid && second.valid)
        return Fmt("%.0f / %.0f MHz", a, b);
    if (first.valid)
        return Fmt("%.0f MHz", a);
    return Fmt("%.0f MHz", b);
}

}  // namespace

Line GpuClocksLine(const Inputs& in, bool reserve, bool withFan) {
    Line line;
    if (in.gpu.coreMhz.valid || in.gpu.memoryMhz.valid)
        line.Add(TwoClocks(in.gpu.coreMhz, in.gpu.memoryMhz, 9999.0f, 99999.0f, reserve));
    if (in.gpu.voltageV.valid)
        line.Add(Fmt("%.3f V", reserve ? 9.999f : in.gpu.voltageV.value));
    if (withFan && in.gpu.fanRpm.valid)
        line.Add(Fmt("%.0f RPM", reserve ? 9999.0f : in.gpu.fanRpm.value));
    return line;
}

Line CpuClocksLine(const Inputs& in, bool reserve, bool withPeak) {
    Line line;
    if (in.cpu.coreMhz.valid || in.cpu.peakCoreMhz.valid)
        line.Add(TwoClocks(in.cpu.coreMhz, in.cpu.peakCoreMhz, 9999.0f, 9999.0f, reserve));
    if (withPeak) {
        const float peak = reserve ? 100.0f : in.cpu.peakCoreLoad;
        line.Add(Fmt("peak %.0f%%", peak), LoadTone(in.cpu.peakCoreLoad) == Tone::Warn ? Tone::Warn : Tone::Unit);
    }
    return line;
}

float NotificationWidth(const Composer& c) {
    if (!c.Has(kRowNotification))
        return 0.0f;
    return (std::max)(c.in.notificationMinWidth, c.W(c.in.notification));
}

float DrawNotification(Composer& c, float y) {
    if (!c.Has(kRowNotification))
        return y;
    const float pitch = c.Pitch(Size::Base);
    c.Text(c.left, c.Baseline(y, pitch), c.in.notification, Size::Base, c.in.notificationTone);
    return y + pitch;
}

float RecordingSlotWidth(const Composer& c) {
    if (!c.Has(kRowRecording))
        return 0.0f;
    float width = 0.0f;
    for (const char* text : {"STARTING RECORDING...", "STARTING AUDIO..."})
        width = (std::max)(width, c.W(text, Size::Small));
    for (const char* text : {"REC 00:00:00", "AUDIO 00:00:00", "NOT RECORDING"})
        width = (std::max)(width, c.W(text, Size::Small) + c.Px(kChipPadX) * 2.0f);
    return width;
}

float RecordingWarningWidth(const Composer& c) {
    if (!c.Has(kRowRecording))
        return 0.0f;
    float width = 0.0f;
    for (const char* text : {"AUDIO 00:00:00 !ENCODER OVERLOAD!", "AUDIO 00:00:00 !ENC SEVERE 9999.9/9999!",
                             "AUDIO 00:00:00 !VIDEO DEGRADED!", "AUDIO 00:00:00 !RECOVERING!"}) {
        width = (std::max)(width, c.W(text, Size::Small) + c.Px(kChipPadX) * 2.0f);
    }
    return width;
}

bool HasRecordingWarning(const Composer& c) {
    return c.Has(kRowRecording) && c.in.recording.kind == RecordingKind::Live && c.in.recording.warning[0] != '\0';
}

Short RecordingChipText(const Composer& c) {
    return HasRecordingWarning(c) ? Fmt("%s %s", c.in.recording.text, c.in.recording.warning)
                                  : Fmt("%s", c.in.recording.text);
}

void DrawChip(Composer& c, float x, float top, float width, const char* text) {
    const float pitch = c.Pitch(Size::Small);
    c.Rect(x, top, width, pitch, Fill::Chip);
    c.Text(x + c.Px(kChipPadX), c.Baseline(top, pitch, Size::Small), text, Size::Small, Tone::OnChip);
}

void DrawRecordingSlotRight(Composer& c, float right, float top) {
    const RecordingValue& recording = c.in.recording;
    if (!c.Has(kRowRecording) || !recording.visible)
        return;
    if (recording.kind == RecordingKind::Starting) {
        c.TextRight(right, c.Baseline(top, c.Pitch(Size::Small), Size::Small), recording.text, Size::Small, Tone::Warn);
    } else if (recording.kind != RecordingKind::None) {
        const float width = c.W(recording.text, Size::Small) + c.Px(kChipPadX) * 2.0f;
        DrawChip(c, right - width, top, width, recording.text);
    }
}

float DrawRecordingFullRow(Composer& c, float y, float width) {
    const float pitch = c.Pitch(Size::Small);
    if (c.in.recording.visible) {
        if (c.in.recording.kind == RecordingKind::Starting) {
            c.Text(c.left, c.Baseline(y, pitch, Size::Small), c.in.recording.text, Size::Small, Tone::Warn);
        } else if (c.in.recording.kind != RecordingKind::None) {
            // Only a warning earns the whole width; a plain timer stays the size of its text.
            const Short text = RecordingChipText(c);
            DrawChip(c, c.left, y, HasRecordingWarning(c) ? width : c.W(text, Size::Small) + c.Px(kChipPadX) * 2.0f,
                     text.c());
        }
    }
    return y + pitch;
}

Short FgStatusText(const Composer& c) {
    return Fmt("%s %dx", c.in.fgLabel, c.in.fgMultiplier);
}

float FgStatusWidth(const Composer& c, Size size) {
    float width = c.W(FgStatusText(c), size);
    for (const char* label : {"DLSS FG 4x", "FSR FG 4x", "NVIDIA SM 4x", "XeFG 4x", "FG 4x"})
        width = (std::max)(width, c.W(label, size));
    return width;
}

float FgRowWidth(const Composer& c) {
    if (!c.Has(kRowFGStatus))
        return 0.0f;
    return FgStatusWidth(c, Size::Base) + c.Px(kLabelGap) +
           (c.Has(kRowFGRates) ? c.W("base 9999 / display 9999 FPS", Size::Small) : 0.0f);
}

float DrawFgRow(Composer& c, float y, float width) {
    if (!c.Has(kRowFGStatus))
        return y;
    const float pitch = c.Pitch(Size::Base);
    const float baseline = c.Baseline(y, pitch);
    c.Text(c.left, baseline, FgStatusText(c).c(), Size::Base, Tone::FgStatus);
    if (c.Has(kRowFGRates)) {
        const Short rates = Fmt("base %.0f / display %.0f FPS", c.in.fgBaseFps, c.in.fgOutputFps);
        c.TextRight(c.left + width, baseline, rates.c(), Size::Small, Tone::Label);
    }
    return y + pitch;
}

float GraphSectionWidth(const Composer& c) {
    if (!c.in.graph.shown)
        return 0.0f;
    return c.W(c.in.graph.scaleLabel, Size::Small) + c.W(c.in.graph.peakLabel, Size::Small) + c.Px(16.0f);
}

float DrawGraphSection(Composer& c, float y, float width) {
    if (!c.in.graph.shown)
        return y;
    const float pitch = c.Pitch(Size::Small);
    const float baseline = c.Baseline(y, pitch, Size::Small);
    c.Text(c.left, baseline, c.in.graph.scaleLabel, Size::Small, Tone::Label);
    c.TextRight(c.left + width, baseline, c.in.graph.peakLabel, Size::Small, c.in.graph.peakTone);
    y += pitch;
    // The rule is the height the scale label names.
    c.Rect(c.left, y, width, (std::max)(1.0f, std::floor(c.in.unit)), Fill::Rule);
    GraphBox& box = c.MutableScene().graph;
    box.valid = true;
    box.x = c.left;
    box.y = y;
    box.width = width;
    box.height = c.Px(kGraphHeight);
    return y + box.height;
}

float BenchmarkWidth(const Composer& c) {
    float width = 0.0f;
    if (c.Has(kRowBenchmarkTimer)) {
        float value = c.W(c.in.benchmarkTimer);
        for (const char* text : {"00:00 / 00:00", "Starts in 10s", "00:00 (Done)"})
            value = (std::max)(value, c.W(text));
        width = (std::max)(width, c.W("BENCHMARK") + c.Px(kLabelGap) + value);
    }
    if (c.Has(kRowBenchmarkFPS))
        width = (std::max)(width, c.W("BENCH FPS") + c.Px(kLabelGap) + c.W("999.9 / 999.9 / 999.9"));
    return width;
}

float DrawBenchmark(Composer& c, float y, float width) {
    const float pitch = c.Pitch(Size::Base);
    const float right = c.left + width;
    if (c.Has(kRowBenchmarkTimer)) {
        const float baseline = c.Baseline(y, pitch);
        c.Text(c.left, baseline, "BENCHMARK", Size::Base, c.in.benchmarkTone);
        c.TextRight(right, baseline, c.in.benchmarkTimer, Size::Base, c.in.benchmarkTone);
        y += pitch;
    }
    if (c.Has(kRowBenchmarkFPS)) {
        const float baseline = c.Baseline(y, pitch);
        c.Text(c.left, baseline, "BENCH FPS", Size::Base, Tone::Label);
        c.TextRight(right, baseline, c.in.benchmarkFps, Size::Base, Tone::Value);
        y += pitch;
    }
    return y;
}

MemoryParts MakeMemoryParts(const Composer& c, const MemoryValue& memory, Size size) {
    MemoryParts parts;
    parts.usedTone = MemoryTone(memory);
    parts.spaceWidth = c.W(" ", size);
    if (!memory.available) {
        parts.used = Fmt("--");
        parts.usedSlot = c.W(parts.used, size);
        return parts;
    }
    parts.used = Fmt("%.2f", memory.usedGb);
    if (memory.hasTotal) {
        parts.rest = Fmt("/ %.2f GB", memory.totalGb);
        parts.usedSlot = (std::max)(c.W(parts.used, size), c.W(Fmt("%.2f", memory.totalGb), size));
    } else {
        parts.rest = Fmt("GB");
        parts.usedSlot = (std::max)(c.W(parts.used, size), c.W("999.99", size));
    }
    return parts;
}

float MemoryPartsWidth(const Composer& c, const MemoryParts& parts, Size size) {
    return parts.usedSlot + (parts.rest.s[0] ? parts.spaceWidth + c.W(parts.rest, size) : 0.0f);
}

void DrawMemoryParts(Composer& c, float x, float baseline, const MemoryParts& parts, Size size) {
    c.TextRight(x + parts.usedSlot, baseline, parts.used.c(), size, parts.usedTone);
    if (parts.rest.s[0])
        c.Text(x + parts.usedSlot + parts.spaceWidth, baseline, parts.rest.c(), size, Tone::Unit);
}

}  // namespace detail

void Build(Layout layout, const Inputs& inputs, const TextMeasure& measure, Scene& scene) {
    scene.width = 0.0f;
    scene.height = 0.0f;
    scene.rects.clear();
    scene.texts.clear();
    scene.graph = GraphBox{};
    if (inputs.rowMask == 0 && !inputs.graph.shown)
        return;
    detail::Composer composer(inputs, measure, scene);
    switch (layout) {
        case Layout::Hero:
            detail::BuildHero(composer);
            break;
        case Layout::List:
            detail::BuildList(composer);
            break;
        case Layout::Meters:
            detail::BuildMeters(composer);
            break;
        case Layout::Strip:
            detail::BuildStrip(composer);
            break;
    }
}

Tone PeakFrameTimeTone(float peakMs, float averageMs) {
    if (averageMs <= 0.001f)
        return Tone::Label;
    const float ratio = peakMs / averageMs;
    if (ratio > 2.0f)
        return Tone::Crit;
    return ratio > 1.5f ? Tone::Warn : Tone::Label;
}

}  // namespace ce::overlay_panel
