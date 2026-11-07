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

Fill BarFill(Tone tone) {
    switch (tone) {
        case Tone::Warn:
            return Fill::BarWarn;
        case Tone::Crit:
            return Fill::BarCrit;
        default:
            return Fill::BarValue;
    }
}

// mark is the tick for the busiest core, as a percentage; negative draws none.
void DrawBar(Composer& c, float x, float baseline, float width, float percent, Tone tone, float mark) {
    const float height = c.Px(kBarHeight);
    const float y = std::round(baseline - c.Font(Size::Base).digitHeight * 0.5f - height * 0.5f);
    c.Rect(x, y, width, height, Fill::BarTrack);
    const float fill = std::round(width * std::clamp(percent, 0.0f, 100.0f) / 100.0f);
    if (fill > 0.0f)
        c.Rect(x, y, fill, height, BarFill(tone));
    if (mark >= 0.0f) {
        const float tick = (std::max)(2.0f, c.Px(2.0f));
        const float overhang = c.Px(2.0f);
        c.Rect(x + std::round((width - tick) * std::clamp(mark, 0.0f, 100.0f) / 100.0f), y - overhang, tick,
               height + overhang * 2.0f, Fill::BarMark);
    }
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
    : in(inputs),
      left(inputs.maxWidth > 0.0f ? (std::min)(std::round(kPadX * inputs.unit), std::floor(inputs.maxWidth / 4.0f))
                                  : std::round(kPadX * inputs.unit)),
      measure_(measure), scene_(scene) {
    for (const Size size : {Size::Base, Size::Small, Size::Hero})
        fonts_[static_cast<int>(size)] = measure.Font(size);
}

float Composer::FitWidth(float desired) const {
    return in.maxWidth > 0.0f ? (std::min)(desired, (std::max)(1.0f, in.maxWidth - left * 2.0f)) : desired;
}

namespace {

// Measuring and drawing take the same breaks. No clipping, scaled glyphs, or new atlas is involved.
template <typename Width, typename Emit>
int WrapLines(const char* text, float width, Width measure, Emit emit) {
    int lines = 0;
    while (text && *text) {
        char part[80];
        size_t count = 0;
        size_t space = 0;
        while (text[count] && count < sizeof(part) - 1) {
            part[count] = text[count];
            part[count + 1] = '\0';
            if (count > 0 && measure(part) > width)
                break;
            if (text[count] == ' ')
                space = count;
            ++count;
        }
        if (text[count] && space > 0)
            count = space;
        part[count] = '\0';
        emit(part, lines++);
        text += count;
        while (*text == ' ')
            ++text;
    }
    return lines;
}

}  // namespace

float Composer::WrappedHeight(float width, const char* text, Size size) const {
    if (!text || !text[0])
        return 0.0f;
    if (W(text, size) <= width)
        return Pitch(size);
    const int lines = WrapLines(text, width, [&](const char* part) { return W(part, size); },
                                [](const char*, int) {});
    return Pitch(size) * static_cast<float>(lines);
}

float Composer::WrappedText(float x, float top, float width, const char* text, Size size, Tone tone) {
    if (!text || !text[0])
        return top;
    const float pitch = Pitch(size);
    if (W(text, size) <= width) {
        Text(x, Baseline(top, pitch, size), text, size, tone);
        return top + pitch;
    }
    const int lines = WrapLines(text, width, [&](const char* part) { return W(part, size); },
                                [&](const char* part, int line) {
                                    Text(x, Baseline(top + static_cast<float>(line) * pitch, pitch, size), part, size, tone);
                                });
    return top + static_cast<float>(lines) * pitch;
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
        width += c.W(items_[i].reserved, size) + (i > 0 ? c.Px(kInlineGap) : 0.0f);
    return width;
}

void Line::Draw(Composer& c, float x, float baseline, Size size) const {
    for (int i = 0; i < count_; ++i) {
        c.Text(x, baseline, items_[i].text.c(), size, items_[i].tone);
        x += c.W(items_[i].reserved, size) + c.Px(kInlineGap);
    }
}

void Line::DrawRight(Composer& c, float right, float baseline, Size size) const {
    for (int i = count_ - 1; i >= 0; --i) {
        c.TextRight(right, baseline, items_[i].text.c(), size, items_[i].tone);
        right -= c.W(items_[i].reserved, size) + c.Px(kInlineGap);
    }
}

float Line::DrawWrapped(Composer& c, float x, float top, float width, Size size) const {
    float cursor = x;
    float height = 0.0f;
    const float gap = c.Px(kInlineGap);
    for (int i = 0; i < count_; ++i) {
        const Item& item = items_[i];
        const float slot = (std::min)(width, c.W(item.reserved, size));
        if (cursor > x && cursor + slot > x + width) {
            top += height;
            cursor = x;
            height = 0.0f;
        }
        const float bottom = c.WrappedText(cursor, top, slot, item.text.c(), size, item.tone);
        height = (std::max)({height, bottom - top, c.WrappedHeight(slot, item.reserved.c(), size)});
        cursor += slot + gap;
    }
    return top + height;
}

Line GpuDetailLine(const Inputs& in) {
    Line line;
    if (in.gpu.memoryMhz.Shown())
        line.Add(in.gpu.memoryMhz.valid ? Fmt("mem %.0f MHz", in.gpu.memoryMhz.value) : Fmt("mem -- MHz"),
                 Tone::Unit, Fmt("mem 99999 MHz"));
    if (in.gpu.voltageV.Shown())
        line.Add(in.gpu.voltageV.valid ? Fmt("%.3f V", in.gpu.voltageV.value) : Fmt("-- V"),
                 Tone::Unit, Fmt("9.999 V"));
    if (in.gpu.fanRpm.Shown())
        line.Add(Fmt("%s RPM", Whole(in.gpu.fanRpm).c()), Tone::Unit, Fmt("9999 RPM"));
    return line;
}

Line CpuDetailLine(const Inputs& in) {
    Line line;
    line.Add(Fmt("peak core %.0f%%", in.cpu.peakCoreLoad),
             LoadTone(in.cpu.peakCoreLoad) == Tone::Warn ? Tone::Warn : Tone::Unit, Fmt("peak core 100%%"));
    if (in.cpu.peakCoreMhz.Shown())
        line.Add(Fmt("%s MHz", Whole(in.cpu.peakCoreMhz).c()), Tone::Unit, Fmt("9999 MHz"));
    return line;
}

namespace {

Short TwoClocks(const Reading& first, const Reading& second, float firstReserve, float secondReserve, bool reserve) {
    const Short a = reserve ? Fmt("%.0f", firstReserve) : Whole(first);
    const Short b = reserve ? Fmt("%.0f", secondReserve) : Whole(second);
    if (first.Shown() && second.Shown())
        return Fmt("%s / %s MHz", a.c(), b.c());
    return Fmt("%s MHz", first.Shown() ? a.c() : b.c());
}

}  // namespace

Line GpuClocksLine(const Inputs& in, bool withFan) {
    Line line;
    if (in.gpu.coreMhz.Shown() || in.gpu.memoryMhz.Shown())
        line.Add(TwoClocks(in.gpu.coreMhz, in.gpu.memoryMhz, 9999.0f, 99999.0f, false), Tone::Unit,
                 TwoClocks(in.gpu.coreMhz, in.gpu.memoryMhz, 9999.0f, 99999.0f, true));
    if (in.gpu.voltageV.Shown())
        line.Add(in.gpu.voltageV.valid ? Fmt("%.3f V", in.gpu.voltageV.value) : Fmt("-- V"),
                 Tone::Unit, Fmt("9.999 V"));
    if (withFan && in.gpu.fanRpm.Shown())
        line.Add(Fmt("%s RPM", Whole(in.gpu.fanRpm).c()), Tone::Unit, Fmt("9999 RPM"));
    return line;
}

Line CpuClocksLine(const Inputs& in, bool withPeak) {
    Line line;
    if (in.cpu.coreMhz.Shown() || in.cpu.peakCoreMhz.Shown())
        line.Add(TwoClocks(in.cpu.coreMhz, in.cpu.peakCoreMhz, 9999.0f, 9999.0f, false), Tone::Unit,
                 TwoClocks(in.cpu.coreMhz, in.cpu.peakCoreMhz, 9999.0f, 9999.0f, true));
    if (withPeak) {
        line.Add(Fmt("peak %.0f%%", in.cpu.peakCoreLoad),
                 LoadTone(in.cpu.peakCoreLoad) == Tone::Warn ? Tone::Warn : Tone::Unit, Fmt("peak 100%%"));
    }
    return line;
}

float NotificationWidth(const Composer& c) {
    if (!c.Has(kRowNotification))
        return 0.0f;
    return (std::max)(c.in.notificationMinWidth, c.W(c.in.notification));
}

float DrawNotification(Composer& c, float y, float width) {
    if (!c.Has(kRowNotification))
        return y;
    return c.WrappedText(c.left, y, width, c.in.notification, Size::Base, c.in.notificationTone);
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

float DrawChip(Composer& c, float x, float top, float width, const char* text) {
    const float pad = (std::min)(c.Px(kChipPadX), width / 4.0f);
    const float bottom = c.WrappedText(x + pad, top, width - pad * 2.0f, text, Size::Small, Tone::OnChip);
    c.Rect(x, top, width, bottom - top, Fill::Chip);
    return bottom;
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
    if (!c.in.recording.visible) {
        const float pad = (std::min)(c.Px(kChipPadX), width / 4.0f);
        return y + (std::max)(pitch, c.WrappedHeight(width - pad * 2.0f, RecordingChipText(c).c(), Size::Small));
    }
    if (c.in.recording.visible) {
        if (c.in.recording.kind == RecordingKind::Starting) {
            return c.WrappedText(c.left, y, width, c.in.recording.text, Size::Small, Tone::Warn);
        } else if (c.in.recording.kind != RecordingKind::None) {
            // Only a warning earns the whole width; a plain timer stays the size of its text.
            const Short text = RecordingChipText(c);
            return DrawChip(c, c.left, y,
                            (std::min)(width, HasRecordingWarning(c) ? width : c.W(text, Size::Small) +
                                                                                 c.Px(kChipPadX) * 2.0f), text.c());
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
    if (width < FgRowWidth(c)) {
        Line line;
        line.Add(FgStatusText(c), Tone::FgStatus, Fmt("NVIDIA SM 4x"));
        if (c.Has(kRowFGRates))
            line.Add(Fmt("base %.0f / display %.0f FPS", c.in.fgBaseFps, c.in.fgOutputFps), Tone::Label,
                     Fmt("base 9999 / display 9999 FPS"));
        return line.DrawWrapped(c, c.left, y, width, Size::Base);
    }
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
    if (width < GraphSectionWidth(c)) {
        y = c.WrappedText(c.left, y, width, c.in.graph.scaleLabel, Size::Small, Tone::Label);
        y = c.WrappedText(c.left, y, width, c.in.graph.peakLabel, Size::Small, c.in.graph.peakTone);
    } else {
        const float baseline = c.Baseline(y, pitch, Size::Small);
        c.Text(c.left, baseline, c.in.graph.scaleLabel, Size::Small, Tone::Label);
        c.TextRight(c.left + width, baseline, c.in.graph.peakLabel, Size::Small, c.in.graph.peakTone);
        y += pitch;
    }
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
    if (width < BenchmarkWidth(c)) {
        if (c.Has(kRowBenchmarkTimer)) {
            Line line;
            line.Add(Fmt("BENCHMARK"), c.in.benchmarkTone);
            line.Add(Fmt("%s", c.in.benchmarkTimer), c.in.benchmarkTone, Fmt("00:00 / 00:00"));
            y = line.DrawWrapped(c, c.left, y, width, Size::Base);
        }
        if (c.Has(kRowBenchmarkFPS)) {
            Line line;
            line.Add(Fmt("BENCH FPS"));
            line.Add(Fmt("%s", c.in.benchmarkFps), Tone::Value, Fmt("9999.9 / 9999.9 / 9999.9"));
            y = line.DrawWrapped(c, c.left, y, width, Size::Base);
        }
        return y;
    }
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

float DrawWrappedMemory(Composer& c, float y, float width, const char* label, const MemoryValue& memory) {
    const MemoryParts parts = MakeMemoryParts(c, memory, Size::Base);
    Line line;
    line.Add(Fmt("%s", label));
    line.Add(parts.used, parts.usedTone, memory.hasTotal ? Fmt("%.2f", memory.totalGb) : Fmt("999.99"));
    if (parts.rest.s[0])
        line.Add(parts.rest);
    return line.DrawWrapped(c, c.left, y, width, Size::Base);
}

float DrawWrappedHardware(Composer& c, float y, float width, bool gpu, Layout layout) {
    y = c.WrappedText(c.left, y, width, gpu ? c.in.gpu.name : c.in.cpu.name, Size::Base, Tone::Value);
    const Reading load = gpu ? c.in.gpu.load : Reading{true, c.in.cpu.load};
    if (layout == Layout::Meters) {
        const float pitch = c.Pitch(Size::Base);
        DrawBar(c, c.left, c.Baseline(y, pitch), width, load.valid ? load.value : 0.0f,
                LoadReadingTone(load), gpu ? -1.0f : c.in.cpu.peakCoreLoad);
        y += pitch;
    }
    Line line;
    line.Add(Fmt("LOAD %s", Percent(load).c()), LoadReadingTone(load), Fmt("LOAD 100%%"));
    const Reading clock = gpu ? c.in.gpu.coreMhz : c.in.cpu.coreMhz;
    if (layout == Layout::Hero && c.Column(kColumnClock))
        line.Add(Fmt("%s MHz", Whole(clock).c()), ReadingTone(clock), Fmt("9999 MHz"));
    if (c.Column(kColumnTemperature))
        line.Add(Degrees(gpu ? c.in.gpu.temperatureC : c.in.cpu.temperatureC),
                 ReadingTone(gpu ? c.in.gpu.temperatureC : c.in.cpu.temperatureC), Fmt("100\xB0"));
    if (c.Column(kColumnPower))
        line.Add(Fmt("%s W", Whole(gpu ? c.in.gpu.powerW : c.in.cpu.powerW).c()),
                 ReadingTone(gpu ? c.in.gpu.powerW : c.in.cpu.powerW), Fmt("999 W"));
    y = line.DrawWrapped(c, c.left, y, width, Size::Base);
    const Line details = layout == Layout::Hero ? (gpu ? GpuDetailLine(c.in) : CpuDetailLine(c.in))
                                               : (gpu ? GpuClocksLine(c.in, true) : CpuClocksLine(c.in, true));
    return details.DrawWrapped(c, c.left, y, width, Size::Small);
}

float DrawWrappedFrame(Composer& c, float y, float width, bool list) {
    const auto row = [&](const char* label, const Short& value, const Short& reserved, Tone tone) {
        Line line;
        line.Add(Fmt("%s", label));
        line.Add(value, tone, reserved);
        y = line.DrawWrapped(c, c.left, y, width, Size::Base);
    };
    if (c.Has(kRowFPS))
        row(list && c.in.api[0] ? c.in.api : "FPS", Fmt("%.0f FPS", c.in.fps), Fmt("9999 FPS"), Tone::Fps);
    if (list && c.Has(kRowFGRates))
        row("Base / Display", Fmt("%.0f / %.0f FPS", c.in.fgBaseFps, c.in.fgOutputFps),
            Fmt("9999 / 9999 FPS"), Tone::Value);
    if (c.Has(kRowFPSAverages))
        row("Avg / 1% / 0.1%", Fmt("%.0f / %.0f / %.0f", c.in.averageFps, c.in.low1Fps, c.in.low01Fps),
            Fmt("9999 / 9999 / 9999"), Tone::Value);
    if (c.Has(kRowSystemLatency))
        row(c.in.latencyLabel, c.in.latencyValid ? Fmt("%.1f ms", c.in.latencyMs) : Fmt("--"),
            Fmt("500.0 ms"), c.in.latencyValid ? Tone::Value : Tone::Unit);
    if (list && c.Has(kRowFGStatus))
        row("Frame generation", FgStatusText(c), Fmt("NVIDIA SM 4x"), Tone::FgStatus);
    return y;
}

}  // namespace detail

void HardwareHistory::Apply(Inputs& in) {
    using namespace ce::overlay_layout;
    const auto remember = [](Reading& reading, uint32_t& fields, uint32_t bit) {
        if (reading.valid)
            fields |= bit;
        reading.present = (fields & bit) != 0;
    };
    if ((in.rowMask & kRowGPU) != 0) {
        remember(in.gpu.temperatureC, gpu_, 1u << 0);
        remember(in.gpu.powerW, gpu_, 1u << 1);
        remember(in.gpu.coreMhz, gpu_, 1u << 2);
        remember(in.gpu.memoryMhz, gpu_, 1u << 3);
        remember(in.gpu.voltageV, gpu_, 1u << 4);
        remember(in.gpu.fanRpm, gpu_, 1u << 5);
        if (in.gpu.temperatureC.Shown())
            columns_ |= kColumnTemperature;
        if (in.gpu.powerW.Shown())
            columns_ |= kColumnPower;
        if (in.gpu.coreMhz.Shown())
            columns_ |= kColumnClock;
        if (in.gpu.fanRpm.Shown())
            columns_ |= kColumnFan;
        if (in.gpu.coreMhz.Shown() || in.gpu.memoryMhz.Shown() || in.gpu.voltageV.Shown())
            in.rowMask |= kRowGPUClocks;
    }
    if ((in.rowMask & kRowCPU) != 0) {
        remember(in.cpu.temperatureC, cpu_, 1u << 0);
        remember(in.cpu.powerW, cpu_, 1u << 1);
        remember(in.cpu.coreMhz, cpu_, 1u << 2);
        remember(in.cpu.peakCoreMhz, cpu_, 1u << 3);
        if (in.cpu.temperatureC.Shown())
            columns_ |= kColumnTemperature;
        if (in.cpu.powerW.Shown())
            columns_ |= kColumnPower;
        if (in.cpu.coreMhz.Shown())
            columns_ |= kColumnClock;
        if (in.cpu.coreMhz.Shown() || in.cpu.peakCoreMhz.Shown())
            in.rowMask |= kRowCPUClocks;
    }
    in.sensorColumns |= columns_;
}

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
