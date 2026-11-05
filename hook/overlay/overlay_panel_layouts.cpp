#include "overlay_panel_internal.h"

namespace ce::overlay_panel::detail {

namespace {

// Stacks the sections with a gap between groups, and none before the first or after an empty one.
class Flow {
public:
    Flow(Composer& c, float y) : c_(c), y_(y) {}
    float Y() const {
        return y_;
    }
    void Set(float y) {
        y_ = y;
    }
    bool Group(bool shown) {
        if (shown && any_)
            y_ += c_.Px(kGroupGap);
        any_ = any_ || shown;
        return shown;
    }

private:
    Composer& c_;
    float y_;
    bool any_ = false;
};

float MaxNameWidth(const Composer& c, Size size = Size::Base) {
    float width = 0.0f;
    if (c.Has(kRowGPU))
        width = (std::max)(width, c.W(c.in.gpu.name, size));
    if (c.Has(kRowCPU))
        width = (std::max)(width, c.W(c.in.cpu.name, size));
    return width;
}

// The sensor columns GPU and CPU share. Each is reserved for its widest value, and its heading where it
// has one, and a column no sensor has ever filled takes no space.
struct SensorColumns {
    float load = 0.0f;
    float clock = 0.0f;
    float temperature = 0.0f;
    float power = 0.0f;
};

SensorColumns MeasureSensorColumns(const Composer& c, bool headings, bool clockColumn) {
    const auto column = [&](float value, const char* heading) {
        return headings ? (std::max)(value, c.W(heading, Size::Small)) : value;
    };
    SensorColumns columns;
    columns.load = column(c.W("100%"), "LOAD");
    if (clockColumn && c.Column(kColumnClock))
        columns.clock = column(c.W("9999"), "CLOCK");
    if (c.Column(kColumnTemperature))
        columns.temperature = column(c.W("100\xB0"), "TEMP");
    if (c.Column(kColumnPower))
        columns.power = column(c.ValueUnitWidth("999", "W"), "POWER");
    return columns;
}

float SensorColumnsWidth(const Composer& c, const SensorColumns& columns) {
    const float gap = c.Px(kColumnGap);
    float width = columns.load;
    for (const float column : {columns.clock, columns.temperature, columns.power}) {
        if (column > 0.0f)
            width += gap + column;
    }
    return width;
}

// Right edges of the columns, from `right` inward.
struct Edges {
    float power = 0.0f;
    float temperature = 0.0f;
    float clock = 0.0f;
    float load = 0.0f;
};

Edges ComputeEdges(const Composer& c, float right, const SensorColumns& columns) {
    const float gap = c.Px(kColumnGap);
    Edges edges;
    float x = right;
    edges.power = x;
    if (columns.power > 0.0f)
        x -= columns.power + gap;
    edges.temperature = x;
    if (columns.temperature > 0.0f)
        x -= columns.temperature + gap;
    edges.clock = x;
    if (columns.clock > 0.0f)
        x -= columns.clock + gap;
    edges.load = x;
    return edges;
}

void DrawSensorCells(Composer& c, const SensorColumns& columns, const Edges& edges, float baseline, const Reading& load,
                     const Reading& clock, const Reading& temperature, const Reading& power) {
    c.TextRight(edges.load, baseline, Percent(load).c(), Size::Base, LoadReadingTone(load));
    if (columns.clock > 0.0f)
        c.TextRight(edges.clock, baseline, Whole(clock).c(), Size::Base, ReadingTone(clock));
    if (columns.temperature > 0.0f)
        c.TextRight(edges.temperature, baseline, Degrees(temperature).c(), Size::Base, ReadingTone(temperature));
    if (columns.power > 0.0f) {
        c.ValueUnitRight(edges.power, baseline, Whole(power).c(), power.valid ? "W" : "", Size::Base,
                         ReadingTone(power));
    }
}

Reading CpuLoad(const Inputs& in) {
    return Reading{true, in.cpu.load};
}

// Hero and Meters both open with the API and recording state on one quiet line.
float HeaderWidth(const Composer& c, Size size) {
    const float api = c.Has(kRowFPS) ? c.W(c.in.api, size) : 0.0f;
    const float slot = RecordingSlotWidth(c);
    return api + (api > 0.0f && slot > 0.0f ? c.Px(kLabelGap) : 0.0f) + slot;
}

float DrawHeader(Composer& c, float y, float width, Size size) {
    const bool api = c.Has(kRowFPS) && c.in.api[0] != '\0';
    const bool slot = c.Has(kRowRecording) && !HasRecordingWarning(c);
    if (!api && !slot)
        return y;
    const float pitch = c.Pitch(size);
    if (api)
        c.Text(c.left, c.Baseline(y, pitch, size), c.in.api, size, Tone::Label);
    if (slot) {
        const float chipTop = y + std::round((pitch - c.Pitch(Size::Small)) * 0.5f);
        DrawRecordingSlotRight(c, c.left + width, chipTop);
    }
    return y + pitch;
}

float DrawWarningRow(Composer& c, float y, float width) {
    return HasRecordingWarning(c) ? DrawRecordingFullRow(c, y, width) : y;
}

// ---- Hero ---------------------------------------------------------------------------------------------------------

struct StatColumn {
    const char* head = "";
    Short value;
    const char* unit = "";
    Tone tone = Tone::Value;
    float width = 0.0f;
};

struct Stats {
    StatColumn columns[4];
    int count = 0;
    float width = 0.0f;
};

Stats MeasureStats(const Composer& c) {
    Stats stats;
    const auto add = [&](const char* head, const Short& value, const char* unit, Tone tone, float valueWidth,
                         float headWidth) {
        StatColumn& column = stats.columns[stats.count++];
        column.head = head;
        column.value = value;
        column.unit = unit;
        column.tone = tone;
        column.width = (std::max)(valueWidth, headWidth);
        stats.width += column.width + (stats.count > 1 ? c.Px(12.0f) : 0.0f);
    };
    if (c.Has(kRowFPSAverages)) {
        const float slot = c.W("9999");
        add("AVG", Fmt("%.0f", c.in.averageFps), "", Tone::Value, slot, c.W("AVG", Size::Small));
        add("1%", Fmt("%.0f", c.in.low1Fps), "", Tone::Value, slot, c.W("1%", Size::Small));
        add("0.1%", Fmt("%.0f", c.in.low01Fps), "", Tone::Value, slot, c.W("0.1%", Size::Small));
    }
    if (c.Has(kRowSystemLatency)) {
        float head = c.W(c.in.latencyLabel, Size::Small);
        for (size_t i = 0; i < c.in.latencyLabelChoiceCount; ++i)
            head = (std::max)(head, c.W(c.in.latencyLabelChoices[i], Size::Small));
        add(c.in.latencyLabel, c.in.latencyValid ? Fmt("%.1f", c.in.latencyMs) : Fmt("--"),
            c.in.latencyValid ? "ms" : "", c.in.latencyValid ? Tone::Value : Tone::Unit,
            c.ValueUnitWidth("500.0", "ms"), head);
    }
    return stats;
}

float HeroBandWidth(const Composer& c, const Stats& stats) {
    const float digits = (std::max)(c.W("9999", Size::Hero), c.W(Fmt("%.0f", c.in.fps), Size::Hero));
    const float hero = c.Has(kRowFPS) ? digits + c.Px(4.0f) + c.W("FPS", Size::Small) : 0.0f;
    return hero + (hero > 0.0f && stats.count > 0 ? c.Px(kLabelGap) : 0.0f) + stats.width;
}

float DrawHeroBand(Composer& c, float y, float width, const Stats& stats) {
    const bool fps = c.Has(kRowFPS);
    if (!fps && stats.count == 0)
        return y;
    const float statsHeight = stats.count > 0 ? c.Pitch(Size::Small) + c.Pitch(Size::Base) : 0.0f;
    const float height = (std::max)(fps ? c.Pitch(Size::Hero) : 0.0f, statsHeight);
    // Everything in the band stands on the value row's baseline.
    const float baseline = y + height - std::round((c.Pitch(Size::Base) - c.Font(Size::Base).digitHeight) * 0.5f);
    if (fps) {
        const Short number = Fmt("%.0f", c.in.fps);
        c.Text(c.left, baseline, number.c(), Size::Hero, Tone::Fps);
        c.Text(c.left + c.W(number, Size::Hero) + c.Px(4.0f), baseline, "FPS", Size::Small, Tone::Label);
    }
    const float headBaseline = y + height - c.Pitch(Size::Base) -
                               std::round((c.Pitch(Size::Small) - c.Font(Size::Small).digitHeight) * 0.5f);
    float right = c.left + width;
    for (int i = stats.count - 1; i >= 0; --i) {
        const StatColumn& column = stats.columns[i];
        c.TextRight(right, headBaseline, column.head, Size::Small, Tone::Label);
        c.ValueUnitRight(right, baseline, column.value.c(), column.unit, Size::Base, column.tone);
        right -= column.width + c.Px(12.0f);
    }
    return y + height;
}

float HwTableWidth(const Composer& c) {
    if (!c.Has(kRowGPU) && !c.Has(kRowCPU))
        return 0.0f;
    float width = MaxNameWidth(c) + c.Px(kColumnGap) + SensorColumnsWidth(c, MeasureSensorColumns(c, true, true));
    if (c.Has(kRowGPU))
        width = (std::max)(width, GpuDetailLine(c.in).Width(c, Size::Small));
    if (c.Has(kRowCPU))
        width = (std::max)(width, CpuDetailLine(c.in).Width(c, Size::Small));
    return width;
}

float DrawHwTable(Composer& c, float y, float width) {
    const SensorColumns columns = MeasureSensorColumns(c, true, true);
    const Edges edges = ComputeEdges(c, c.left + width, columns);
    const float small = c.Pitch(Size::Small);
    const float base = c.Pitch(Size::Base);
    const float headBaseline = c.Baseline(y, small, Size::Small);
    c.TextRight(edges.load, headBaseline, "LOAD", Size::Small, Tone::Label);
    if (columns.clock > 0.0f)
        c.TextRight(edges.clock, headBaseline, "CLOCK", Size::Small, Tone::Label);
    if (columns.temperature > 0.0f)
        c.TextRight(edges.temperature, headBaseline, "TEMP", Size::Small, Tone::Label);
    if (columns.power > 0.0f)
        c.TextRight(edges.power, headBaseline, "POWER", Size::Small, Tone::Label);
    y += small;

    const auto detail = [&](const Line& line) {
        if (!line.Empty()) {
            line.Draw(c, c.left, c.Baseline(y, small, Size::Small), Size::Small);
            y += small;
        }
    };
    if (c.Has(kRowGPU)) {
        const float baseline = c.Baseline(y, base);
        c.Text(c.left, baseline, c.in.gpu.name, Size::Base, Tone::Value);
        DrawSensorCells(c, columns, edges, baseline, c.in.gpu.load, c.in.gpu.coreMhz, c.in.gpu.temperatureC,
                        c.in.gpu.powerW);
        y += base;
        detail(GpuDetailLine(c.in));
    }
    if (c.Has(kRowCPU)) {
        const float baseline = c.Baseline(y, base);
        c.Text(c.left, baseline, c.in.cpu.name, Size::Base, Tone::Value);
        DrawSensorCells(c, columns, edges, baseline, CpuLoad(c.in), c.in.cpu.coreMhz, c.in.cpu.temperatureC,
                        c.in.cpu.powerW);
        y += base;
        detail(CpuDetailLine(c.in));
    }
    return y;
}

float MemoryCellWidth(const Composer& c, const char* label, const MemoryValue& memory) {
    return c.W(label) + c.W(" ") + MemoryPartsWidth(c, MakeMemoryParts(c, memory, Size::Base), Size::Base);
}

void DrawMemoryCell(Composer& c, float x, float baseline, const char* label, const MemoryValue& memory) {
    c.Text(x, baseline, label, Size::Base, Tone::Label);
    DrawMemoryParts(c, x + c.W(label) + c.W(" "), baseline, MakeMemoryParts(c, memory, Size::Base), Size::Base);
}

float MemoryRowWidth(const Composer& c) {
    const bool vram = c.Has(kRowVRAM);
    const bool ram = c.Has(kRowRAM);
    return (vram ? MemoryCellWidth(c, "VRAM", c.in.vram) : 0.0f) + (vram && ram ? c.Px(kLabelGap) : 0.0f) +
           (ram ? MemoryCellWidth(c, "RAM", c.in.ram) : 0.0f);
}

float DrawMemoryRow(Composer& c, float y, float width) {
    const float pitch = c.Pitch(Size::Base);
    const float baseline = c.Baseline(y, pitch);
    if (c.Has(kRowVRAM))
        DrawMemoryCell(c, c.left, baseline, "VRAM", c.in.vram);
    if (c.Has(kRowRAM)) {
        const float x = c.Has(kRowVRAM) ? c.left + width - MemoryCellWidth(c, "RAM", c.in.ram) : c.left;
        DrawMemoryCell(c, x, baseline, "RAM", c.in.ram);
    }
    return y + pitch;
}

// ---- List ---------------------------------------------------------------------------------------------------------

float FgRatesReserve(const Composer& c) {
    return c.ValueUnitWidth("9999 / 9999", "FPS");
}

float ListFrameWidth(const Composer& c) {
    float width = 0.0f;
    const float gap = c.Px(kLabelGap);
    if (c.Has(kRowFPS))
        width = (std::max)(width, c.W(c.in.api[0] ? c.in.api : "FPS") + gap + c.W("9999 FPS"));
    if (c.Has(kRowFGRates))
        width = (std::max)(width, c.W("Base / Display") + gap + FgRatesReserve(c));
    if (c.Has(kRowFPSAverages))
        width = (std::max)(width, c.W("Avg / 1% / 0.1%") + gap + c.W("9999 / 9999 / 9999"));
    if (c.Has(kRowSystemLatency)) {
        float label = c.W(c.in.latencyLabel);
        for (size_t i = 0; i < c.in.latencyLabelChoiceCount; ++i)
            label = (std::max)(label, c.W(c.in.latencyLabelChoices[i]));
        width = (std::max)(width, label + gap + c.ValueUnitWidth("500.0", "ms"));
    }
    if (c.Has(kRowFGStatus))
        width = (std::max)(width, c.W("Frame generation") + gap + FgStatusWidth(c, Size::Base));
    return width;
}

float DrawListFrame(Composer& c, float y, float width) {
    const float pitch = c.Pitch(Size::Base);
    const float right = c.left + width;
    const auto label = [&](const char* text) {
        const float baseline = c.Baseline(y, pitch);
        c.Text(c.left, baseline, text, Size::Base, Tone::Label);
        return baseline;
    };
    if (c.Has(kRowFPS)) {
        const float baseline = label(c.in.api[0] ? c.in.api : "FPS");
        c.TextRight(right, baseline, Fmt("%.0f FPS", c.in.fps).c(), Size::Base, Tone::Fps);
        y += pitch;
    }
    if (c.Has(kRowFGRates)) {
        const float baseline = label("Base / Display");
        c.ValueUnitRight(right, baseline, Fmt("%.0f / %.0f", c.in.fgBaseFps, c.in.fgOutputFps).c(), "FPS", Size::Base,
                         Tone::Value);
        y += pitch;
    }
    if (c.Has(kRowFPSAverages)) {
        const float baseline = label("Avg / 1% / 0.1%");
        c.TextRight(right, baseline, Fmt("%.0f / %.0f / %.0f", c.in.averageFps, c.in.low1Fps, c.in.low01Fps).c(),
                    Size::Base, Tone::Value);
        y += pitch;
    }
    if (c.Has(kRowSystemLatency)) {
        const float baseline = label(c.in.latencyLabel);
        c.ValueUnitRight(right, baseline, c.in.latencyValid ? Fmt("%.1f", c.in.latencyMs).c() : "--",
                         c.in.latencyValid ? "ms" : "", Size::Base, c.in.latencyValid ? Tone::Value : Tone::Unit);
        y += pitch;
    }
    if (c.Has(kRowFGStatus)) {
        const float baseline = label("Frame generation");
        c.TextRight(right, baseline, FgStatusText(c).c(), Size::Base, Tone::FgStatus);
        y += pitch;
    }
    return y;
}

// The last column holds what only one chip has: the GPU's fan, the CPU's busiest core.
float ListLastColumnWidth(const Composer& c) {
    float width = 0.0f;
    if (c.Has(kRowGPU) && c.Column(kColumnFan))
        width = (std::max)(width, c.ValueUnitWidth("9999", "RPM"));
    if (c.Has(kRowCPU))
        width = (std::max)(width, c.W("peak 100%"));
    return width;
}

float ListHwWidth(const Composer& c) {
    if (!c.Has(kRowGPU) && !c.Has(kRowCPU))
        return 0.0f;
    const float nameWidth = (std::max)(MaxNameWidth(c), c.W("Clocks"));
    const float last = ListLastColumnWidth(c);
    float width = nameWidth + c.Px(kColumnGap) + SensorColumnsWidth(c, MeasureSensorColumns(c, false, false)) +
                  (last > 0.0f ? c.Px(kColumnGap) + last : 0.0f);
    const float clocks = (std::max)(c.Has(kRowGPUClocks) ? GpuClocksLine(c.in, false).Width(c, Size::Base) : 0.0f,
                                    c.Has(kRowCPUClocks) ? CpuClocksLine(c.in, false).Width(c, Size::Base) : 0.0f);
    return (std::max)(width, c.W("Clocks") + c.Px(kLabelGap) + clocks);
}

float DrawListChip(Composer& c, float y, float width, const char* name, const Reading& load, const Reading& temperature,
                   const Reading& power, bool gpu) {
    const float pitch = c.Pitch(Size::Base);
    const float baseline = c.Baseline(y, pitch);
    const float right = c.left + width;
    const float last = ListLastColumnWidth(c);
    const SensorColumns columns = MeasureSensorColumns(c, false, false);
    const Edges edges = ComputeEdges(c, right - (last > 0.0f ? last + c.Px(kColumnGap) : 0.0f), columns);
    c.Text(c.left, baseline, name, Size::Base, Tone::Value);
    DrawSensorCells(c, columns, edges, baseline, load, Reading{}, temperature, power);
    if (gpu && c.Column(kColumnFan)) {
        c.ValueUnitRight(right, baseline, Whole(c.in.gpu.fanRpm).c(), c.in.gpu.fanRpm.valid ? "RPM" : "", Size::Base,
                         ReadingTone(c.in.gpu.fanRpm));
    } else if (!gpu) {
        const Short peak = Fmt("%.0f%%", c.in.cpu.peakCoreLoad);
        c.TextRight(right, baseline, peak.c(), Size::Base, LoadTone(c.in.cpu.peakCoreLoad));
        c.TextRight(right - c.W(peak) - c.W(" "), baseline, "peak", Size::Base, Tone::Unit);
    }
    return y + pitch;
}

float DrawListClocks(Composer& c, float y, float width, const Line& line) {
    const float pitch = c.Pitch(Size::Base);
    const float baseline = c.Baseline(y, pitch);
    c.Text(c.left, baseline, "Clocks", Size::Base, Tone::Label);
    line.DrawRight(c, c.left + width, baseline, Size::Base);
    return y + pitch;
}

float DrawListMemory(Composer& c, float y, float width, const char* label, const MemoryValue& memory) {
    const float pitch = c.Pitch(Size::Base);
    const float baseline = c.Baseline(y, pitch);
    c.Text(c.left, baseline, label, Size::Base, Tone::Label);
    const MemoryParts parts = MakeMemoryParts(c, memory, Size::Base);
    DrawMemoryParts(c, c.left + width - MemoryPartsWidth(c, parts, Size::Base), baseline, parts, Size::Base);
    return y + pitch;
}

// ---- Meters -------------------------------------------------------------------------------------------------------

inline constexpr float kBarWidth = 104.0f;
inline constexpr float kBarHeight = 7.0f;

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
void DrawBar(Composer& c, float x, float baseline, float width, float percent, Tone tone, float mark = -1.0f) {
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

float MetersFrameWidth(const Composer& c) {
    float width = 0.0f;
    const float gap = c.Px(kLabelGap);
    if (c.Has(kRowFPS)) {
        const float lows = c.Has(kRowFPSAverages)
                               ? gap + c.W("9999 / 9999 / 9999") + c.Px(5.0f) + c.W("AVG 1% 0.1%", Size::Small)
                               : 0.0f;
        width = (std::max)(width, c.W("9999 FPS") + lows);
    }
    if (c.Has(kRowSystemLatency)) {
        float label = c.W(c.in.latencyLabel);
        for (size_t i = 0; i < c.in.latencyLabelChoiceCount; ++i)
            label = (std::max)(label, c.W(c.in.latencyLabelChoices[i]));
        width = (std::max)(width, label + gap + c.ValueUnitWidth("500.0", "ms"));
    }
    return width;
}

float DrawMetersFrame(Composer& c, float y, float width) {
    const float pitch = c.Pitch(Size::Base);
    const float right = c.left + width;
    if (c.Has(kRowFPS)) {
        const float baseline = c.Baseline(y, pitch);
        c.Text(c.left, baseline, Fmt("%.0f FPS", c.in.fps).c(), Size::Base, Tone::Fps);
        if (c.Has(kRowFPSAverages)) {
            const char* legend = "AVG 1% 0.1%";
            c.TextRight(right, baseline, legend, Size::Small, Tone::Label);
            c.TextRight(right - c.W(legend, Size::Small) - c.Px(5.0f), baseline,
                        Fmt("%.0f / %.0f / %.0f", c.in.averageFps, c.in.low1Fps, c.in.low01Fps).c(), Size::Base,
                        Tone::Value);
        }
        y += pitch;
    }
    if (c.Has(kRowSystemLatency)) {
        const float baseline = c.Baseline(y, pitch);
        c.Text(c.left, baseline, c.in.latencyLabel, Size::Base, Tone::Label);
        c.ValueUnitRight(right, baseline, c.in.latencyValid ? Fmt("%.1f", c.in.latencyMs).c() : "--",
                         c.in.latencyValid ? "ms" : "", Size::Base, c.in.latencyValid ? Tone::Value : Tone::Unit);
        y += pitch;
    }
    return y;
}

float MetersNameWidth(const Composer& c) {
    float width = MaxNameWidth(c);
    if (c.Has(kRowVRAM))
        width = (std::max)(width, c.W("VRAM"));
    if (c.Has(kRowRAM))
        width = (std::max)(width, c.W("RAM"));
    return width;
}

float MetersValueAreaWidth(const Composer& c) {
    float width = 0.0f;
    if (c.Has(kRowGPU) || c.Has(kRowCPU))
        width = SensorColumnsWidth(c, MeasureSensorColumns(c, false, false));
    if (c.Has(kRowVRAM))
        width = (std::max)(width, MemoryPartsWidth(c, MakeMemoryParts(c, c.in.vram, Size::Base), Size::Base));
    if (c.Has(kRowRAM))
        width = (std::max)(width, MemoryPartsWidth(c, MakeMemoryParts(c, c.in.ram, Size::Base), Size::Base));
    return width;
}

float MetersHwWidth(const Composer& c) {
    if (!c.Has(kRowGPU) && !c.Has(kRowCPU) && !c.Has(kRowVRAM) && !c.Has(kRowRAM))
        return 0.0f;
    const float gap = c.Px(kColumnGap);
    float width = MetersNameWidth(c) + gap + c.Px(kBarWidth) + gap + MetersValueAreaWidth(c);
    if (c.Has(kRowGPU))
        width = (std::max)(width, GpuClocksLine(c.in, true).Width(c, Size::Small));
    if (c.Has(kRowCPU))
        width = (std::max)(width, CpuClocksLine(c.in, true).Width(c, Size::Small));
    return width;
}

float DrawMetersHw(Composer& c, float y, float width) {
    const float gap = c.Px(kColumnGap);
    const float pitch = c.Pitch(Size::Base);
    const float small = c.Pitch(Size::Small);
    const float right = c.left + width;
    const float barX = c.left + MetersNameWidth(c) + gap;
    const float barWidth = c.Px(kBarWidth);
    const SensorColumns columns = MeasureSensorColumns(c, false, false);
    const Edges edges = ComputeEdges(c, right, columns);

    const auto chip = [&](const char* name, const Reading& load, const Reading& temperature, const Reading& power,
                          float mark, const Line& detail) {
        const float baseline = c.Baseline(y, pitch);
        c.Text(c.left, baseline, name, Size::Base, Tone::Value);
        DrawBar(c, barX, baseline, barWidth, load.valid ? load.value : 0.0f, LoadReadingTone(load), mark);
        DrawSensorCells(c, columns, edges, baseline, load, Reading{}, temperature, power);
        y += pitch;
        if (!detail.Empty()) {
            detail.Draw(c, c.left, c.Baseline(y, small, Size::Small), Size::Small);
            y += small;
        }
    };
    if (c.Has(kRowGPU))
        chip(c.in.gpu.name, c.in.gpu.load, c.in.gpu.temperatureC, c.in.gpu.powerW, -1.0f,
             GpuClocksLine(c.in, true));
    if (c.Has(kRowCPU))
        chip(c.in.cpu.name, CpuLoad(c.in), c.in.cpu.temperatureC, c.in.cpu.powerW, c.in.cpu.peakCoreLoad,
             CpuClocksLine(c.in, true));

    const auto memory = [&](const char* label, const MemoryValue& value) {
        const float baseline = c.Baseline(y, pitch);
        c.Text(c.left, baseline, label, Size::Base, Tone::Label);
        const float percent =
            value.available && value.hasTotal && value.totalGb > 0.0f ? value.usedGb / value.totalGb * 100.0f : 0.0f;
        DrawBar(c, barX, baseline, barWidth, percent, MemoryTone(value));
        const MemoryParts parts = MakeMemoryParts(c, value, Size::Base);
        DrawMemoryParts(c, right - MemoryPartsWidth(c, parts, Size::Base), baseline, parts, Size::Base);
        y += pitch;
    };
    if (c.Has(kRowVRAM))
        memory("VRAM", c.in.vram);
    if (c.Has(kRowRAM))
        memory("RAM", c.in.ram);
    return y;
}

}  // namespace

void BuildHero(Composer& c) {
    const Stats stats = MeasureStats(c);
    float width = c.Px(kMinContentWidth);
    for (const float section : {NotificationWidth(c), HeaderWidth(c, Size::Small), RecordingWarningWidth(c),
                                FgRowWidth(c), HeroBandWidth(c, stats), GraphSectionWidth(c), HwTableWidth(c),
                                MemoryRowWidth(c), BenchmarkWidth(c)}) {
        width = (std::max)(width, section);
    }

    Flow flow(c, c.Px(kPadTop));
    flow.Set(DrawNotification(c, flow.Y()));
    flow.Set(DrawHeader(c, flow.Y(), width, Size::Small));
    flow.Set(DrawWarningRow(c, flow.Y(), width));
    flow.Set(DrawFgRow(c, flow.Y(), width));
    flow.Set(DrawHeroBand(c, flow.Y(), width, stats));
    flow.Group(flow.Y() > c.Px(kPadTop) || c.in.graph.shown);
    flow.Set(DrawGraphSection(c, flow.Y(), width));
    if (flow.Group(c.Has(kRowGPU) || c.Has(kRowCPU)))
        flow.Set(DrawHwTable(c, flow.Y(), width));
    if (c.Has(kRowVRAM) || c.Has(kRowRAM)) {
        flow.Set(flow.Y() + c.Px(4.0f));
        flow.Set(DrawMemoryRow(c, flow.Y(), width));
    }
    if (flow.Group(c.Has(kRowBenchmarkTimer) || c.Has(kRowBenchmarkFPS)))
        flow.Set(DrawBenchmark(c, flow.Y(), width));
    c.Finish(width, flow.Y());
}

void BuildList(Composer& c) {
    float memoryWidth = 0.0f;
    if (c.Has(kRowVRAM))
        memoryWidth = (std::max)(memoryWidth, c.W("VRAM") + c.Px(kLabelGap) +
                                                  MemoryPartsWidth(c, MakeMemoryParts(c, c.in.vram, Size::Base), Size::Base));
    if (c.Has(kRowRAM))
        memoryWidth = (std::max)(memoryWidth, c.W("VRAM") + c.Px(kLabelGap) +
                                                  MemoryPartsWidth(c, MakeMemoryParts(c, c.in.ram, Size::Base), Size::Base));
    float width = c.Px(kMinContentWidth);
    for (const float section : {NotificationWidth(c), ListFrameWidth(c), GraphSectionWidth(c), ListHwWidth(c),
                                memoryWidth, RecordingWarningWidth(c), BenchmarkWidth(c)}) {
        width = (std::max)(width, section);
    }

    Flow flow(c, c.Px(kPadTop));
    const bool frame = c.Has(kRowFPS) || c.Has(kRowSystemLatency) || c.Has(kRowFGStatus);
    flow.Set(DrawNotification(c, flow.Y()));
    flow.Group(c.Has(kRowNotification));
    if (flow.Group(frame || c.in.graph.shown)) {
        flow.Set(DrawListFrame(c, flow.Y(), width));
        flow.Set(DrawGraphSection(c, flow.Y(), width));
    }
    if (flow.Group(c.Has(kRowGPU))) {
        flow.Set(DrawListChip(c, flow.Y(), width, c.in.gpu.name, c.in.gpu.load, c.in.gpu.temperatureC, c.in.gpu.powerW,
                              true));
        if (c.Has(kRowGPUClocks))
            flow.Set(DrawListClocks(c, flow.Y(), width, GpuClocksLine(c.in, false)));
    }
    if (flow.Group(c.Has(kRowCPU))) {
        flow.Set(DrawListChip(c, flow.Y(), width, c.in.cpu.name, CpuLoad(c.in), c.in.cpu.temperatureC, c.in.cpu.powerW,
                              false));
        if (c.Has(kRowCPUClocks))
            flow.Set(DrawListClocks(c, flow.Y(), width, CpuClocksLine(c.in, false)));
    }
    if (flow.Group(c.Has(kRowVRAM) || c.Has(kRowRAM))) {
        if (c.Has(kRowVRAM))
            flow.Set(DrawListMemory(c, flow.Y(), width, "VRAM", c.in.vram));
        if (c.Has(kRowRAM))
            flow.Set(DrawListMemory(c, flow.Y(), width, "RAM", c.in.ram));
    }
    if (flow.Group(c.Has(kRowRecording)))
        flow.Set(DrawRecordingFullRow(c, flow.Y(), width));
    if (flow.Group(c.Has(kRowBenchmarkTimer) || c.Has(kRowBenchmarkFPS)))
        flow.Set(DrawBenchmark(c, flow.Y(), width));
    c.Finish(width, flow.Y());
}

void BuildMeters(Composer& c) {
    float width = c.Px(kMinContentWidth);
    for (const float section : {NotificationWidth(c), HeaderWidth(c, Size::Base), RecordingWarningWidth(c), FgRowWidth(c),
                                MetersFrameWidth(c), GraphSectionWidth(c), MetersHwWidth(c), BenchmarkWidth(c)}) {
        width = (std::max)(width, section);
    }

    Flow flow(c, c.Px(kPadTop));
    flow.Set(DrawNotification(c, flow.Y()));
    flow.Set(DrawHeader(c, flow.Y(), width, Size::Base));
    flow.Set(DrawWarningRow(c, flow.Y(), width));
    flow.Set(DrawFgRow(c, flow.Y(), width));
    flow.Set(DrawMetersFrame(c, flow.Y(), width));
    flow.Group(flow.Y() > c.Px(kPadTop) || c.in.graph.shown);
    flow.Set(DrawGraphSection(c, flow.Y(), width));
    if (flow.Group(c.Has(kRowGPU) || c.Has(kRowCPU) || c.Has(kRowVRAM) || c.Has(kRowRAM)))
        flow.Set(DrawMetersHw(c, flow.Y(), width));
    if (flow.Group(c.Has(kRowBenchmarkTimer) || c.Has(kRowBenchmarkFPS)))
        flow.Set(DrawBenchmark(c, flow.Y(), width));
    c.Finish(width, flow.Y());
}

}  // namespace ce::overlay_panel::detail
