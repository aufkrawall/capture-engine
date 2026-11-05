#include "overlay_panel_internal.h"

#include <array>
#include <cfloat>

// The strip: cells side by side, each with a main line and a quieter one beneath. It covers the least
// area, and it is the one layout that can outgrow a screen, so cells wrap into another band.
namespace ce::overlay_panel::detail {

namespace {

inline constexpr float kCellPadX = 11.0f;
inline constexpr float kStripPadTop = 5.0f;
inline constexpr float kStripPadBottom = 6.0f;
inline constexpr float kGraphMinWidth = 120.0f;

struct CellRows {
    float top = 0.0f;
    float main = 0.0f;  // Baselines.
    float detail = 0.0f;
    float height = 0.0f;
};

struct ChipColumns {
    float load = 0.0f;
    float temperature = 0.0f;
    float power = 0.0f;
    float total = 0.0f;
};

ChipColumns MeasureChip(const Composer& c, const char* name) {
    const float gap = c.Px(kInlineGap);
    ChipColumns columns;
    columns.load = c.W("100%");
    columns.temperature = c.Column(kColumnTemperature) ? c.W("100\xB0") : 0.0f;
    columns.power = c.Column(kColumnPower) ? c.ValueUnitWidth("999", "W") : 0.0f;
    columns.total = c.W(name) + gap + columns.load + (columns.temperature > 0.0f ? gap + columns.temperature : 0.0f) +
                    (columns.power > 0.0f ? gap + columns.power : 0.0f);
    return columns;
}

void DrawChipMain(Composer& c, float x, float baseline, const char* name, const Reading& load,
                  const Reading& temperature, const Reading& power) {
    const ChipColumns columns = MeasureChip(c, name);
    const float gap = c.Px(kInlineGap);
    c.Text(x, baseline, name, Size::Base, Tone::Value);
    float cursor = x + c.W(name) + gap + columns.load;
    c.TextRight(cursor, baseline, Percent(load).c(), Size::Base, LoadReadingTone(load));
    if (columns.temperature > 0.0f) {
        cursor += gap + columns.temperature;
        c.TextRight(cursor, baseline, Degrees(temperature).c(), Size::Base, ReadingTone(temperature));
    }
    if (columns.power > 0.0f) {
        cursor += gap + columns.power;
        c.ValueUnitRight(cursor, baseline, Whole(power).c(), power.valid ? "W" : "", Size::Base, ReadingTone(power));
    }
}

Reading CpuLoadReading(const Composer& c) {
    return Reading{true, c.in.cpu.load};
}

Short LatencyDetail(const Composer& c, bool reserve, const char* label) {
    if (reserve)
        return Fmt("500.0 ms %s", label);
    return c.in.latencyValid ? Fmt("%.1f ms %s", c.in.latencyMs, label) : Fmt("-- %s", label);
}

// ---- Cells: a width and a draw for each. Reserved widths keep them steady as values change. --------------------

bool NotificationShown(const Composer& c) {
    return c.Has(kRowNotification);
}
float NotificationCellWidth(const Composer& c) {
    return NotificationWidth(c);
}
void DrawNotificationCell(Composer& c, float x, const CellRows& rows, float) {
    c.Text(x, rows.main, c.in.notification, Size::Base, c.in.notificationTone);
}

bool FpsShown(const Composer& c) {
    return c.Has(kRowFPS);
}
float FpsCellWidth(const Composer& c) {
    return (std::max)(c.W("9999 FPS"), c.W(c.in.api, Size::Small));
}
void DrawFpsCell(Composer& c, float x, const CellRows& rows, float) {
    c.Text(x, rows.main, Fmt("%.0f FPS", c.in.fps).c(), Size::Base, Tone::Fps);
    c.Text(x, rows.detail, c.in.api, Size::Small, Tone::Unit);
}

bool FgShown(const Composer& c) {
    return c.Has(kRowFGStatus);
}
float FgCellWidth(const Composer& c) {
    return (std::max)(FgStatusWidth(c, Size::Base), c.Has(kRowFGRates) ? c.W("9999 / 9999 FPS", Size::Small) : 0.0f);
}
void DrawFgCell(Composer& c, float x, const CellRows& rows, float) {
    c.Text(x, rows.main, FgStatusText(c).c(), Size::Base, Tone::FgStatus);
    if (c.Has(kRowFGRates))
        c.Text(x, rows.detail, Fmt("%.0f / %.0f FPS", c.in.fgBaseFps, c.in.fgOutputFps).c(), Size::Small, Tone::Unit);
}

bool LowsShown(const Composer& c) {
    return c.Has(kRowFPSAverages) || c.Has(kRowSystemLatency);
}
float LowsCellWidth(const Composer& c) {
    float width = c.Has(kRowFPSAverages) ? (std::max)(c.W("9999 / 9999 / 9999"), c.W("AVG / 1% / 0.1%", Size::Small)) : 0.0f;
    if (c.Has(kRowSystemLatency)) {
        const Size size = c.Has(kRowFPSAverages) ? Size::Small : Size::Base;
        float label = c.W(LatencyDetail(c, true, c.in.latencyLabel), size);
        for (size_t i = 0; i < c.in.latencyLabelChoiceCount; ++i)
            label = (std::max)(label, c.W(LatencyDetail(c, true, c.in.latencyLabelChoices[i]), size));
        width = (std::max)(width, label);
    }
    return width;
}
void DrawLowsCell(Composer& c, float x, const CellRows& rows, float) {
    if (c.Has(kRowFPSAverages)) {
        c.Text(x, rows.main, Fmt("%.0f / %.0f / %.0f", c.in.averageFps, c.in.low1Fps, c.in.low01Fps).c(), Size::Base,
               Tone::Value);
        if (c.Has(kRowSystemLatency)) {
            c.Text(x, rows.detail, LatencyDetail(c, false, c.in.latencyLabel).c(), Size::Small, Tone::Unit);
        } else {
            c.Text(x, rows.detail, "AVG / 1% / 0.1%", Size::Small, Tone::Unit);
        }
    } else {
        c.Text(x, rows.main, LatencyDetail(c, false, c.in.latencyLabel).c(), Size::Base, Tone::Value);
    }
}

bool GpuShown(const Composer& c) {
    return c.Has(kRowGPU);
}
float GpuCellWidth(const Composer& c) {
    return (std::max)(MeasureChip(c, c.in.gpu.name).total, GpuClocksLine(c.in, true, true).Width(c, Size::Small));
}
void DrawGpuCell(Composer& c, float x, const CellRows& rows, float) {
    DrawChipMain(c, x, rows.main, c.in.gpu.name, c.in.gpu.load, c.in.gpu.temperatureC, c.in.gpu.powerW);
    GpuClocksLine(c.in, false, true).Draw(c, x, rows.detail, Size::Small);
}

bool CpuShown(const Composer& c) {
    return c.Has(kRowCPU);
}
float CpuCellWidth(const Composer& c) {
    return (std::max)(MeasureChip(c, c.in.cpu.name).total, CpuClocksLine(c.in, true, true).Width(c, Size::Small));
}
void DrawCpuCell(Composer& c, float x, const CellRows& rows, float) {
    DrawChipMain(c, x, rows.main, c.in.cpu.name, CpuLoadReading(c), c.in.cpu.temperatureC, c.in.cpu.powerW);
    CpuClocksLine(c.in, false, true).Draw(c, x, rows.detail, Size::Small);
}

bool MemoryShown(const Composer& c) {
    return c.Has(kRowVRAM) || c.Has(kRowRAM);
}
float MemoryLineWidth(const Composer& c, const char* label, const MemoryValue& memory, Size size) {
    return c.W(label, size) + c.W(" ", size) + MemoryPartsWidth(c, MakeMemoryParts(c, memory, size), size);
}
void DrawMemoryLine(Composer& c, float x, float baseline, const char* label, const MemoryValue& memory, Size size) {
    c.Text(x, baseline, label, size, Tone::Label);
    DrawMemoryParts(c, x + c.W(label, size) + c.W(" ", size), baseline, MakeMemoryParts(c, memory, size), size);
}
float MemoryCellWidth(const Composer& c) {
    float width = 0.0f;
    if (c.Has(kRowVRAM))
        width = MemoryLineWidth(c, "VRAM", c.in.vram, Size::Base);
    if (c.Has(kRowRAM))
        width = (std::max)(width, MemoryLineWidth(c, "RAM", c.in.ram, c.Has(kRowVRAM) ? Size::Small : Size::Base));
    return width;
}
void DrawMemoryCell(Composer& c, float x, const CellRows& rows, float) {
    if (c.Has(kRowVRAM))
        DrawMemoryLine(c, x, rows.main, "VRAM", c.in.vram, Size::Base);
    if (c.Has(kRowRAM)) {
        const bool second = c.Has(kRowVRAM);
        DrawMemoryLine(c, x, second ? rows.detail : rows.main, "RAM", c.in.ram, second ? Size::Small : Size::Base);
    }
}

bool GraphShown(const Composer& c) {
    return c.in.graph.shown;
}
float GraphCellWidth(const Composer& c) {
    return (std::max)(c.Px(kGraphMinWidth), GraphSectionWidth(c));
}
void DrawGraphCell(Composer& c, float x, const CellRows& rows, float width) {
    const float pitch = c.Pitch(Size::Base);
    const float rule = (std::max)(1.0f, std::floor(c.in.unit));
    GraphBox& box = c.MutableScene().graph;
    box.valid = true;
    box.x = x;
    box.y = rows.top + c.Px(2.0f);
    box.width = width;
    box.height = pitch - c.Px(4.0f);
    c.Rect(x, box.y, width, rule, Fill::Rule);
    c.Text(x, rows.detail, c.in.graph.scaleLabel, Size::Small, Tone::Unit);
    c.TextRight(x + width, rows.detail, c.in.graph.peakLabel, Size::Small, c.in.graph.peakTone);
}

bool RecordingShown(const Composer& c) {
    return c.Has(kRowRecording);
}
float RecordingCellWidth(const Composer& c) {
    float width = RecordingSlotWidth(c);
    if (HasRecordingWarning(c))
        width = (std::max)(width, c.W(RecordingChipText(c), Size::Small) + c.Px(kChipPadX) * 2.0f);
    return width;
}
void DrawRecordingCell(Composer& c, float x, const CellRows& rows, float width) {
    const RecordingValue& recording = c.in.recording;
    if (!recording.visible)
        return;
    const float top = rows.top + std::round((rows.height - c.Pitch(Size::Small)) * 0.5f);
    if (recording.kind == RecordingKind::Starting) {
        c.Text(x, c.Baseline(top, c.Pitch(Size::Small), Size::Small), recording.text, Size::Small, Tone::Warn);
    } else if (recording.kind != RecordingKind::None) {
        DrawChip(c, x, top, width, RecordingChipText(c).c());
    }
}

bool BenchmarkShown(const Composer& c) {
    return c.Has(kRowBenchmarkTimer) || c.Has(kRowBenchmarkFPS);
}
float BenchmarkCellWidth(const Composer& c) {
    float width = 0.0f;
    if (c.Has(kRowBenchmarkTimer)) {
        width = c.W("BENCHMARK") + c.Px(kInlineGap) + (std::max)(c.W("00:00 / 00:00"), c.W(c.in.benchmarkTimer));
    }
    if (c.Has(kRowBenchmarkFPS))
        width = (std::max)(width, c.W("999.9 / 999.9 / 999.9", Size::Small));
    return width;
}
void DrawBenchmarkCell(Composer& c, float x, const CellRows& rows, float width) {
    if (c.Has(kRowBenchmarkTimer)) {
        c.Text(x, rows.main, "BENCHMARK", Size::Base, c.in.benchmarkTone);
        c.TextRight(x + width, rows.main, c.in.benchmarkTimer, Size::Base, c.in.benchmarkTone);
    }
    if (c.Has(kRowBenchmarkFPS))
        c.Text(x, rows.detail, c.in.benchmarkFps, Size::Small, Tone::Unit);
}

struct CellKind {
    bool (*shown)(const Composer&);
    float (*width)(const Composer&);
    void (*draw)(Composer&, float x, const CellRows&, float width);
};

constexpr std::array<CellKind, 10> kCells = {{
    {NotificationShown, NotificationCellWidth, DrawNotificationCell},
    {FpsShown, FpsCellWidth, DrawFpsCell},
    {FgShown, FgCellWidth, DrawFgCell},
    {LowsShown, LowsCellWidth, DrawLowsCell},
    {GpuShown, GpuCellWidth, DrawGpuCell},
    {CpuShown, CpuCellWidth, DrawCpuCell},
    {MemoryShown, MemoryCellWidth, DrawMemoryCell},
    {GraphShown, GraphCellWidth, DrawGraphCell},
    {RecordingShown, RecordingCellWidth, DrawRecordingCell},
    {BenchmarkShown, BenchmarkCellWidth, DrawBenchmarkCell},
}};

}  // namespace

void BuildStrip(Composer& c) {
    const float padX = c.Px(kCellPadX);
    const float bandHeight = c.Pitch(Size::Base) + c.Pitch(Size::Small);
    const float padTop = c.Px(kStripPadTop);
    const float padBottom = c.Px(kStripPadBottom);
    const float rule = (std::max)(1.0f, std::floor(c.in.unit));
    const float limit = c.in.maxWidth > 0.0f ? c.in.maxWidth : FLT_MAX;

    float panelWidth = 0.0f;
    float x = 0.0f;
    float top = padTop;
    bool cellInBand = false;
    for (const CellKind& kind : kCells) {
        if (!kind.shown(c))
            continue;
        const float inner = kind.width(c);
        const float outer = inner + padX * 2.0f;
        if (cellInBand && x + outer > limit) {
            panelWidth = (std::max)(panelWidth, x);
            x = 0.0f;
            top += bandHeight + padBottom;
            cellInBand = false;
        }
        if (cellInBand)
            c.Rect(x, top, rule, bandHeight, Fill::Rule);

        CellRows rows;
        rows.top = top;
        rows.height = bandHeight;
        rows.main = c.Baseline(top, c.Pitch(Size::Base));
        rows.detail = c.Baseline(top + c.Pitch(Size::Base), c.Pitch(Size::Small), Size::Small);
        kind.draw(c, x + padX, rows, inner);

        x += outer;
        cellInBand = true;
    }
    panelWidth = (std::max)(panelWidth, x);
    if (panelWidth <= 0.0f)
        return;

    Scene& scene = c.MutableScene();
    scene.width = panelWidth;
    scene.height = top + bandHeight + padBottom;
}

}  // namespace ce::overlay_panel::detail
