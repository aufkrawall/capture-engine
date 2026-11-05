#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// The overlay's look: turns the current values into positioned text and rectangles, in one of the
// layouts [Overlay] layout selects. It knows nothing about graphics APIs, the atlas or where the panel
// sits on screen, so every layout rule is tested with a fake font and no renderer. Text is measured
// through TextMeasure and comes back in panel coordinates with the origin at the panel's top left.
//
// Rules every layout follows:
// - Color means state. Tones say what a value is, and only Warn and Crit call for attention.
// - A value sits in a column reserved for its widest form, so a changing digit count never moves its
//   neighbours or resizes the panel.
// - rowMask (overlay_layout_policy.h) is the only source of what is shown.
namespace ce::overlay_panel {

enum class Layout : uint8_t { Hero, List, Meters, Strip };
enum class Size : uint8_t { Base, Small, Hero };
enum class Tone : uint8_t { Value, Label, Unit, Fps, FgStatus, Good, Warn, Crit, OnChip };
enum class Fill : uint8_t { Chip, BarTrack, BarValue, BarWarn, BarCrit, BarMark, Rule };

struct FontMetrics {
    float pixelSize = 0.0f;
    float ascent = 0.0f;
    float digitHeight = 0.0f;
};

class TextMeasure {
public:
    virtual ~TextMeasure() = default;
    virtual float Width(const char* text, Size size) const = 0;
    virtual FontMetrics Font(Size size) const = 0;
};

struct Reading {
    bool valid = false;
    float value = 0.0f;
};

struct GpuValues {
    const char* name = "";
    Reading load;
    Reading temperatureC;
    Reading powerW;
    Reading fanRpm;
    Reading coreMhz;
    Reading memoryMhz;
    Reading voltageV;
};

struct CpuValues {
    const char* name = "";
    float load = 0.0f;
    float peakCoreLoad = 0.0f;
    Reading temperatureC;
    Reading powerW;
    Reading coreMhz;
    Reading peakCoreMhz;
};

struct MemoryValue {
    bool available = false;
    bool hasTotal = false;
    float usedGb = 0.0f;
    float totalGb = 0.0f;
};

enum class RecordingKind : uint8_t { None, Starting, Live, NotRecording };

struct RecordingValue {
    RecordingKind kind = RecordingKind::None;
    const char* text = "";     // "REC 00:00:01"
    const char* warning = "";  // "!ENCODER OVERLOAD!" while a live recording is in trouble
    bool visible = true;       // Off for the dark phase of a blink: the slot stays reserved.
};

struct GraphValue {
    bool shown = false;
    const char* scaleLabel = "";
    const char* peakLabel = "";
    Tone peakTone = Tone::Label;
};

// A hardware column stays once it has shown a reading, so a sensor that drops out for a moment reads "--"
// instead of resizing the panel.
enum SensorColumn : uint32_t {
    kColumnTemperature = 1u << 0,
    kColumnPower = 1u << 1,
    kColumnClock = 1u << 2,
    kColumnFan = 1u << 3,
};

struct Inputs {
    uint32_t rowMask = 0;
    float unit = 1.0f;      // Physical pixels per layout pixel: DPI scale and font size together.
    float maxWidth = 0.0f;  // The strip wraps into another band beyond this. 0 is unlimited.

    const char* api = "";
    float fps = 0.0f;
    float averageFps = 0.0f;
    float low1Fps = 0.0f;
    float low01Fps = 0.0f;

    const char* latencyLabel = "";
    const char* const* latencyLabelChoices = nullptr;  // Every label latencyLabel can become, for the width.
    size_t latencyLabelChoiceCount = 0;
    bool latencyValid = false;
    float latencyMs = 0.0f;

    const char* fgLabel = "";
    int fgMultiplier = 1;
    float fgBaseFps = 0.0f;
    float fgOutputFps = 0.0f;

    GpuValues gpu;
    CpuValues cpu;
    uint32_t sensorColumns = 0;
    MemoryValue vram;
    MemoryValue ram;

    RecordingValue recording;

    const char* notification = "";
    Tone notificationTone = Tone::Good;
    float notificationMinWidth = 0.0f;

    const char* benchmarkTimer = "";
    Tone benchmarkTone = Tone::Value;
    const char* benchmarkFps = "";

    GraphValue graph;
};

struct TextOp {
    float x = 0.0f;
    float y = 0.0f;
    Size size = Size::Base;
    Tone tone = Tone::Value;
    char text[80] = "";
};

struct RectOp {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    Fill fill = Fill::Rule;
};

struct GraphBox {
    bool valid = false;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

// Draw the rectangles, then the text, then the graph: rectangles sit under the text, and keeping each kind
// together is what keeps the panel to as few draw calls as the backend allows. Empty when nothing is shown.
struct Scene {
    float width = 0.0f;
    float height = 0.0f;
    std::vector<RectOp> rects;
    std::vector<TextOp> texts;
    GraphBox graph;
};

// Rebuilds the scene in place, keeping its capacity.
void Build(Layout layout, const Inputs& inputs, const TextMeasure& measure, Scene& scene);

// How the peak frame time of the last two seconds compares with their average.
Tone PeakFrameTimeTone(float peakMs, float averageMs);

}  // namespace ce::overlay_panel
