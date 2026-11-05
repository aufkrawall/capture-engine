#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "hook/overlay/overlay_layout_policy.h"
#include "hook/overlay/overlay_panel.h"

// The overlay's layouts, driven through their interface with a font whose metrics are known. What they
// must do for a reader: show every value, keep it in one place while it changes, and never collide.

namespace {

using namespace ce::overlay_panel;
using namespace ce::overlay_layout;

// Every character the width of a tabular digit, which is what the atlas gives digits in any font.
class FakeMeasure final : public TextMeasure {
public:
    explicit FakeMeasure(float scale = 1.0f) : scale_(scale) {}
    float Width(const char* text, Size size) const override {
        return static_cast<float>(std::strlen(text)) * CharWidth(size);
    }
    FontMetrics Font(Size size) const override {
        switch (size) {
            case Size::Small:
                return {12.0f * scale_, 11.0f * scale_, 8.0f * scale_};
            case Size::Hero:
                return {32.0f * scale_, 28.0f * scale_, 23.0f * scale_};
            case Size::Base:
                break;
        }
        return {14.0f * scale_, 13.0f * scale_, 10.0f * scale_};
    }
    float CharWidth(Size size) const {
        switch (size) {
            case Size::Small:
                return 6.0f * scale_;
            case Size::Hero:
                return 18.0f * scale_;
            case Size::Base:
                break;
        }
        return 7.0f * scale_;
    }

private:
    float scale_;
};

const char* const kLatencyLabels[] = {"PC Latency~", "Latency est.", "PC Latency"};

// The values of the screenshot that started this.
Inputs FullInputs() {
    Inputs in;
    in.rowMask = kRowGPU | kRowGPUClocks | kRowCPU | kRowCPUClocks | kRowVRAM | kRowRAM | kRowFPS | kRowFPSAverages |
                 kRowSystemLatency | kRowRecording;
    in.api = "DX11 -> DX12 (DXUP)";
    in.fps = 140.0f;
    in.averageFps = 100.0f;
    in.low1Fps = 20.0f;
    in.low01Fps = 21.0f;
    in.latencyLabel = "Latency est.";
    in.latencyLabelChoices = kLatencyLabels;
    in.latencyLabelChoiceCount = 3;
    in.latencyValid = true;
    in.latencyMs = 14.9f;
    in.gpu.name = "RTX 5070";
    in.gpu.load = {true, 60.0f};
    in.gpu.temperatureC = {true, 61.0f};
    in.gpu.powerW = {true, 135.0f};
    in.gpu.fanRpm = {true, 769.0f};
    in.gpu.coreMhz = {true, 2910.0f};
    in.gpu.memoryMhz = {true, 17001.0f};
    in.gpu.voltageV = {true, 0.92f};
    in.cpu.name = "5700X";
    in.cpu.load = 14.0f;
    in.cpu.peakCoreLoad = 52.0f;
    in.cpu.temperatureC = {true, 73.0f};
    in.cpu.powerW = {true, 72.0f};
    in.cpu.coreMhz = {true, 4191.0f};
    in.cpu.peakCoreMhz = {true, 4569.0f};
    in.sensorColumns = kColumnTemperature | kColumnPower | kColumnClock | kColumnFan;
    in.vram = {true, true, 4.31f, 11.66f};
    in.ram = {true, true, 12.76f, 31.93f};
    in.recording.kind = RecordingKind::Live;
    in.recording.text = "REC 00:00:01";
    in.graph.shown = true;
    in.graph.scaleLabel = "33 ms | Display";
    in.graph.peakLabel = "13.8 ms";
    in.graph.peakTone = Tone::Warn;
    return in;
}

Inputs WithFrameGeneration(Inputs in) {
    in.rowMask |= kRowFGRates | kRowFGStatus;
    in.fgLabel = "DLSS FG";
    in.fgMultiplier = 3;
    in.fgBaseFps = 46.0f;
    in.fgOutputFps = 139.0f;
    return in;
}

constexpr Layout kLayouts[] = {Layout::Hero, Layout::List, Layout::Meters, Layout::Strip};

const char* Name(Layout layout) {
    switch (layout) {
        case Layout::Hero:
            return "hero";
        case Layout::List:
            return "list";
        case Layout::Meters:
            return "meters";
        case Layout::Strip:
            return "strip";
    }
    return "?";
}

Scene BuildScene(Layout layout, const Inputs& in, const FakeMeasure& measure = FakeMeasure()) {
    Scene scene;
    Build(layout, in, measure, scene);
    return scene;
}

const TextOp* FindExact(const Scene& scene, const std::string& text) {
    for (const TextOp& op : scene.texts) {
        if (text == op.text)
            return &op;
    }
    return nullptr;
}

bool Shows(const Scene& scene, const std::string& fragment) {
    for (const TextOp& op : scene.texts) {
        if (std::string(op.text).find(fragment) != std::string::npos)
            return true;
    }
    return false;
}

float RightEdge(const TextOp& op, const FakeMeasure& measure = FakeMeasure()) {
    return op.x + measure.Width(op.text, op.size);
}

// All text sits inside the panel and no two pieces of it touch.
void ExpectTidy(const Scene& scene, const FakeMeasure& measure, const std::string& context) {
    ASSERT_GT(scene.width, 0.0f) << context;
    ASSERT_GT(scene.height, 0.0f) << context;
    struct Ink {
        float left, right, top, bottom;
        const char* text;
    };
    std::vector<Ink> inks;
    for (const TextOp& op : scene.texts) {
        const FontMetrics font = measure.Font(op.size);
        const float baseline = op.y + font.ascent;
        Ink ink{op.x, RightEdge(op, measure), baseline - font.digitHeight, baseline, op.text};
        EXPECT_GE(ink.left, 0.0f) << context << ": " << op.text;
        EXPECT_LE(ink.right, scene.width + 1.0f) << context << ": " << op.text;
        EXPECT_GE(ink.top, 0.0f) << context << ": " << op.text;
        EXPECT_LE(ink.bottom, scene.height) << context << ": " << op.text;
        inks.push_back(ink);
    }
    for (size_t a = 0; a < inks.size(); ++a) {
        for (size_t b = a + 1; b < inks.size(); ++b) {
            const bool apart = inks[a].right <= inks[b].left || inks[b].right <= inks[a].left ||
                               inks[a].bottom <= inks[b].top || inks[b].bottom <= inks[a].top;
            EXPECT_TRUE(apart) << context << ": '" << inks[a].text << "' touches '" << inks[b].text << "'";
        }
    }
    for (const RectOp& rect : scene.rects) {
        EXPECT_GE(rect.x, 0.0f) << context;
        EXPECT_LE(rect.x + rect.width, scene.width + 1.0f) << context;
        EXPECT_LE(rect.y + rect.height, scene.height + 1.0f) << context;
    }
    if (scene.graph.valid) {
        EXPECT_GE(scene.graph.x, 0.0f) << context;
        EXPECT_LE(scene.graph.x + scene.graph.width, scene.width) << context;
        EXPECT_LE(scene.graph.y + scene.graph.height, scene.height) << context;
    }
}

TEST(OverlayPanelTest, EveryLayoutShowsEveryValue) {
    for (const Layout layout : kLayouts) {
        SCOPED_TRACE(Name(layout));
        const Scene scene = BuildScene(layout, FullInputs());
        for (const char* value :
             {"DX11 -> DX12 (DXUP)", "140", "100", "20", "21", "14.9", "Latency est.", "RTX 5070", "60%", "61\xB0", "135",
              "769", "2910", "17001", "0.920", "5700X", "14%", "52%", "73\xB0", "72", "4191", "4569", "4.31", "11.66",
              "12.76", "31.93", "VRAM", "RAM", "REC 00:00:01", "33 ms", "Display", "13.8 ms"}) {
            EXPECT_TRUE(Shows(scene, value)) << value;
        }
        EXPECT_TRUE(scene.graph.valid);
    }
}

TEST(OverlayPanelTest, NothingCollidesInAnyLayoutOrState) {
    Inputs stressed = FullInputs();
    stressed.recording.warning = "!ENC SEVERE 9999.9/9999!";
    stressed.gpu.load = {true, 99.0f};
    Inputs bare = FullInputs();
    bare.rowMask = kRowFPS;
    bare.graph.shown = false;
    Inputs notifying = WithFrameGeneration(FullInputs());
    notifying.rowMask |= kRowNotification | kRowBenchmarkTimer | kRowBenchmarkFPS;
    notifying.notification = "Screenshot saved!";
    notifying.benchmarkTimer = "00:10 / 01:00";
    notifying.benchmarkFps = "99.9 / 88.8 / 77.7";
    Inputs longNames = FullInputs();
    longNames.gpu.name = "Radeon RX 7900 XTX Limited";
    longNames.cpu.name = "Ryzen 9 9950X3D";
    longNames.api = "DX9 -> DX11 -> DX12 (DXUP)";
    longNames.graph.scaleLabel = "100 ms | Present (no display data)";
    Inputs starting = FullInputs();
    starting.recording.kind = RecordingKind::Starting;
    starting.recording.text = "STARTING RECORDING...";

    const Inputs cases[] = {FullInputs(), WithFrameGeneration(FullInputs()), stressed, bare, notifying, longNames, starting};
    for (const Layout layout : kLayouts) {
        int index = 0;
        for (const Inputs& in : cases) {
            for (const float scale : {1.0f, 1.5f, 2.0f}) {
                Inputs scaled = in;
                scaled.unit = scale;
                const FakeMeasure measure(scale);
                ExpectTidy(BuildScene(layout, scaled, measure), measure,
                           std::string(Name(layout)) + " case " + std::to_string(index) + " scale " + std::to_string(scale));
            }
            ++index;
        }
    }
}

// The panel must not breathe with its values, which is what made the overlay hard to read at a glance.
TEST(OverlayPanelTest, ChangingDigitCountsMoveNothing) {
    Inputs small = FullInputs();
    small.fps = 9.0f;
    small.averageFps = 9.0f;
    small.low1Fps = 1.0f;
    small.low01Fps = 1.0f;
    small.latencyMs = 1.0f;
    small.gpu.load = {true, 1.0f};
    small.gpu.temperatureC = {true, 9.0f};
    small.gpu.powerW = {true, 5.0f};
    small.gpu.fanRpm = {true, 0.0f};
    small.gpu.coreMhz = {true, 300.0f};
    small.gpu.memoryMhz = {true, 400.0f};
    small.cpu.load = 2.0f;
    small.cpu.peakCoreLoad = 9.0f;
    small.cpu.temperatureC = {true, 9.0f};
    small.cpu.powerW = {true, 9.0f};
    small.cpu.coreMhz = {true, 800.0f};
    small.cpu.peakCoreMhz = {true, 900.0f};
    small.vram.usedGb = 0.5f;
    small.ram.usedGb = 1.5f;
    Inputs large = FullInputs();
    large.fps = 1234.0f;
    large.averageFps = 1234.0f;
    large.low1Fps = 1111.0f;
    large.low01Fps = 1000.0f;
    large.latencyMs = 123.4f;
    large.gpu.load = {true, 100.0f};
    large.gpu.temperatureC = {true, 100.0f};
    large.gpu.powerW = {true, 450.0f};
    large.gpu.fanRpm = {true, 3200.0f};
    large.cpu.load = 100.0f;
    large.cpu.peakCoreLoad = 100.0f;
    large.cpu.temperatureC = {true, 100.0f};
    large.cpu.powerW = {true, 230.0f};
    large.vram.usedGb = 11.0f;
    large.ram.usedGb = 31.0f;

    for (const Layout layout : kLayouts) {
        SCOPED_TRACE(Name(layout));
        const Scene a = BuildScene(layout, small);
        const Scene b = BuildScene(layout, large);
        EXPECT_EQ(a.width, b.width);
        EXPECT_EQ(a.height, b.height);
        EXPECT_EQ(a.graph.x, b.graph.x);
        EXPECT_EQ(a.graph.y, b.graph.y);
        EXPECT_EQ(a.graph.width, b.graph.width);
    }
}

// The same reading stays in the same column for GPU and CPU: what the old right-aligned runs could not do.
TEST(OverlayPanelTest, GpuAndCpuReadingsShareTheirColumns) {
    Inputs in = FullInputs();
    in.gpu.powerW = {true, 135.0f};
    in.cpu.powerW = {true, 72.0f};
    for (const Layout layout : {Layout::Hero, Layout::List, Layout::Meters}) {
        SCOPED_TRACE(Name(layout));
        const Scene scene = BuildScene(layout, in);
        const TextOp* gpuLoad = FindExact(scene, "60%");
        const TextOp* cpuLoad = FindExact(scene, "14%");
        const TextOp* gpuTemp = FindExact(scene, "61\xB0");
        const TextOp* cpuTemp = FindExact(scene, "73\xB0");
        const TextOp* gpuPower = FindExact(scene, "135");
        const TextOp* cpuPower = FindExact(scene, "72");
        ASSERT_TRUE(gpuLoad && cpuLoad && gpuTemp && cpuTemp && gpuPower && cpuPower);
        EXPECT_EQ(RightEdge(*gpuLoad), RightEdge(*cpuLoad));
        EXPECT_EQ(RightEdge(*gpuTemp), RightEdge(*cpuTemp));
        EXPECT_EQ(RightEdge(*gpuPower), RightEdge(*cpuPower));
    }
    const Scene hero = BuildScene(Layout::Hero, in);
    const TextOp* head = FindExact(hero, "TEMP");
    const TextOp* value = FindExact(hero, "61\xB0");
    ASSERT_TRUE(head && value);
    EXPECT_EQ(RightEdge(*head), RightEdge(*value)) << "a heading sits over its column";
}

// Colors used to say what a row was, so a problem had nothing left to stand out with. Load's color must
// also stay on the load: at 99% the whole row once turned red, as if temperature and fan were critical.
TEST(OverlayPanelTest, OnlyWhatNeedsAttentionIsColored) {
    Inputs in = FullInputs();
    for (const Layout layout : kLayouts) {
        SCOPED_TRACE(Name(layout));
        const Scene calm = BuildScene(layout, in);
        for (const TextOp& op : calm.texts) {
            const bool peakLabel = std::strcmp(op.text, "13.8 ms") == 0;
            EXPECT_TRUE(peakLabel || op.tone != Tone::Warn) << op.text;
            EXPECT_NE(op.tone, Tone::Crit) << op.text;
        }
    }
    in.gpu.load = {true, 99.0f};
    in.cpu.load = 90.0f;
    in.vram = {true, true, 11.40f, 11.66f};
    in.ram = {true, true, 29.00f, 31.93f};
    for (const Layout layout : kLayouts) {
        SCOPED_TRACE(Name(layout));
        const Scene busy = BuildScene(layout, in);
        const TextOp* load = FindExact(busy, "99%");
        const TextOp* temperature = FindExact(busy, "61\xB0");
        const TextOp* vram = FindExact(busy, "11.40");
        const TextOp* ram = FindExact(busy, "29.00");
        ASSERT_TRUE(load && temperature && vram && ram);
        EXPECT_EQ(load->tone, Tone::Warn) << "99% GPU load is normal in a GPU-limited game, so never red";
        EXPECT_EQ(temperature->tone, Tone::Value) << "a load color must not reach the sensors";
        EXPECT_EQ(vram->tone, Tone::Crit) << "97% of the VRAM in use";
        EXPECT_EQ(ram->tone, Tone::Warn) << "90% of the RAM in use";
    }
}

TEST(OverlayPanelTest, HeroMakesFpsTheLargestThingAndPutsItsGraphUnderIt) {
    const Scene scene = BuildScene(Layout::Hero, FullInputs());
    const TextOp* fps = FindExact(scene, "140");
    const TextOp* gpu = FindExact(scene, "RTX 5070");
    ASSERT_TRUE(fps && gpu);
    EXPECT_EQ(fps->size, Size::Hero);
    EXPECT_EQ(fps->tone, Tone::Fps);
    EXPECT_LT(fps->y, scene.graph.y);
    EXPECT_LT(scene.graph.y, gpu->y);
    for (const Layout layout : {Layout::List, Layout::Meters, Layout::Strip}) {
        for (const TextOp& op : BuildScene(layout, FullInputs()).texts)
            EXPECT_NE(op.size, Size::Hero) << Name(layout) << ": only hero needs the large digits";
    }
}

TEST(OverlayPanelTest, ARowOutsideTheMaskIsNotDrawnAndLeavesNoGap) {
    struct RowCase {
        uint32_t row;
        const char* fragment;
    };
    const Inputs full = WithFrameGeneration(FullInputs());
    for (const Layout layout : {Layout::Hero, Layout::List, Layout::Meters}) {
        SCOPED_TRACE(Name(layout));
        const float fullHeight = BuildScene(layout, full).height;
        for (const RowCase& rowCase :
             {RowCase{kRowGPU, "RTX 5070"}, RowCase{kRowCPU, "5700X"}, RowCase{kRowVRAM, "VRAM"}, RowCase{kRowRAM, "12.76"},
              RowCase{kRowFGStatus, "DLSS FG"}, RowCase{kRowRecording, "REC 00"}, RowCase{kRowSystemLatency, "14.9"},
              RowCase{kRowFPSAverages, "100"}}) {
            Inputs in = full;
            in.rowMask &= ~rowCase.row;
            const Scene scene = BuildScene(layout, in);
            EXPECT_FALSE(Shows(scene, rowCase.fragment)) << rowCase.fragment;
            EXPECT_LE(scene.height, fullHeight) << rowCase.fragment;
        }
    }
    Inputs nothing;
    for (const Layout layout : kLayouts) {
        const Scene scene = BuildScene(layout, nothing);
        EXPECT_EQ(scene.width, 0.0f) << "an empty panel is not drawn at all";
        EXPECT_TRUE(scene.texts.empty());
        EXPECT_TRUE(scene.rects.empty());
    }
}

TEST(OverlayPanelTest, AHardwareColumnAppearsWithItsFirstReadingAndThenStays) {
    Inputs unelevated = FullInputs();
    unelevated.sensorColumns = 0;
    unelevated.rowMask &= ~(kRowGPUClocks | kRowCPUClocks);
    unelevated.gpu.temperatureC = {};
    unelevated.gpu.powerW = {};
    unelevated.gpu.fanRpm = {};
    unelevated.gpu.coreMhz = {};
    unelevated.gpu.memoryMhz = {};
    unelevated.gpu.voltageV = {};
    unelevated.cpu.temperatureC = {};
    unelevated.cpu.powerW = {};
    unelevated.cpu.coreMhz = {};
    unelevated.cpu.peakCoreMhz = {};
    const Scene hero = BuildScene(Layout::Hero, unelevated);
    EXPECT_EQ(FindExact(hero, "TEMP"), nullptr);
    EXPECT_EQ(FindExact(hero, "POWER"), nullptr);
    EXPECT_EQ(FindExact(hero, "CLOCK"), nullptr);
    EXPECT_NE(FindExact(hero, "LOAD"), nullptr);
    EXPECT_FALSE(Shows(hero, "--")) << "nothing to apologize for in a column that was never there";
    for (const Layout layout : kLayouts) {
        const FakeMeasure measure;
        ExpectTidy(BuildScene(layout, unelevated, measure), measure, Name(layout));
    }

    // A sensor that drops out for a moment: "--" in place, nothing moves.
    Inputs dropped = FullInputs();
    dropped.gpu.temperatureC = {};
    dropped.gpu.powerW = {};
    for (const Layout layout : kLayouts) {
        SCOPED_TRACE(Name(layout));
        const Scene before = BuildScene(layout, FullInputs());
        const Scene after = BuildScene(layout, dropped);
        EXPECT_TRUE(Shows(after, "--"));
        EXPECT_EQ(before.width, after.width);
        EXPECT_EQ(before.height, after.height);
    }
}

TEST(OverlayPanelTest, TheRecordingChipReadsOnAnyBackground) {
    for (const Layout layout : kLayouts) {
        SCOPED_TRACE(Name(layout));
        const Scene scene = BuildScene(layout, FullInputs());
        const TextOp* text = FindExact(scene, "REC 00:00:01");
        ASSERT_NE(text, nullptr);
        EXPECT_EQ(text->tone, Tone::OnChip);
        const auto chip = std::find_if(scene.rects.begin(), scene.rects.end(),
                                       [](const RectOp& rect) { return rect.fill == Fill::Chip; });
        ASSERT_NE(chip, scene.rects.end());
        EXPECT_LE(chip->x, text->x);
        EXPECT_GE(chip->x + chip->width, RightEdge(*text));
        const FontMetrics font = FakeMeasure().Font(text->size);
        const float baseline = text->y + font.ascent;
        EXPECT_LE(chip->y, baseline - font.digitHeight);
        EXPECT_GE(chip->y + chip->height, baseline);
    }
}

TEST(OverlayPanelTest, RecordingStatesKeepTheirPlaceAndTheirMeaning) {
    Inputs warning = FullInputs();
    warning.recording.warning = "!ENCODER OVERLOAD!";
    Inputs starting = FullInputs();
    starting.recording.kind = RecordingKind::Starting;
    starting.recording.text = "STARTING RECORDING...";
    Inputs blinkOn = FullInputs();
    blinkOn.recording.kind = RecordingKind::NotRecording;
    blinkOn.recording.text = "NOT RECORDING";
    Inputs blinkOff = blinkOn;
    blinkOff.recording.visible = false;

    for (const Layout layout : kLayouts) {
        SCOPED_TRACE(Name(layout));
        const Scene live = BuildScene(layout, FullInputs());
        const Scene warned = BuildScene(layout, warning);
        EXPECT_TRUE(Shows(warned, "REC 00:00:01 !ENCODER OVERLOAD!"));
        if (layout != Layout::Strip)
            EXPECT_EQ(warned.width, live.width) << "the warning's width is reserved while recording";

        const Scene startingScene = BuildScene(layout, starting);
        const TextOp* startingText = FindExact(startingScene, "STARTING RECORDING...");
        ASSERT_NE(startingText, nullptr);
        EXPECT_EQ(startingText->tone, Tone::Warn);
        EXPECT_EQ(startingScene.width, live.width);
        EXPECT_EQ(startingScene.height, live.height);

        // The dark phase of the blink must not make the panel jump.
        const Scene on = BuildScene(layout, blinkOn);
        const Scene off = BuildScene(layout, blinkOff);
        EXPECT_TRUE(Shows(on, "NOT RECORDING"));
        EXPECT_FALSE(Shows(off, "NOT RECORDING"));
        EXPECT_EQ(on.width, off.width);
        EXPECT_EQ(on.height, off.height);
    }
}

TEST(OverlayPanelTest, FrameGenerationShowsItsStatusAndBothRates) {
    const Inputs in = WithFrameGeneration(FullInputs());
    for (const Layout layout : kLayouts) {
        SCOPED_TRACE(Name(layout));
        const Scene scene = BuildScene(layout, in);
        const TextOp* status = FindExact(scene, "DLSS FG 3x");
        ASSERT_NE(status, nullptr) << "the label once appeared twice in one row";
        EXPECT_EQ(status->tone, Tone::FgStatus);
        EXPECT_TRUE(Shows(scene, "46"));
        EXPECT_TRUE(Shows(scene, "139"));
    }
    // A switch between generators must not resize the panel.
    Inputs other = in;
    other.fgLabel = "XeFG";
    other.fgMultiplier = 2;
    for (const Layout layout : kLayouts)
        EXPECT_EQ(BuildScene(layout, in).width, BuildScene(layout, other).width) << Name(layout);
}

TEST(OverlayPanelTest, MetersDrawBarsToScaleAndMarkTheBusiestCore) {
    Inputs in = FullInputs();
    in.gpu.load = {true, 50.0f};
    in.cpu.load = 25.0f;
    in.cpu.peakCoreLoad = 75.0f;
    in.vram = {true, true, 3.0f, 12.0f};
    const Scene scene = BuildScene(Layout::Meters, in);
    std::vector<RectOp> tracks;
    std::vector<RectOp> fills;
    std::vector<RectOp> marks;
    for (const RectOp& rect : scene.rects) {
        if (rect.fill == Fill::BarTrack)
            tracks.push_back(rect);
        else if (rect.fill == Fill::BarValue)
            fills.push_back(rect);
        else if (rect.fill == Fill::BarMark)
            marks.push_back(rect);
    }
    ASSERT_EQ(tracks.size(), 4u) << "GPU, CPU, VRAM, RAM";
    ASSERT_EQ(fills.size(), 4u);
    ASSERT_EQ(marks.size(), 1u);
    for (const RectOp& track : tracks) {
        EXPECT_EQ(track.x, tracks[0].x) << "one shared axis";
        EXPECT_EQ(track.width, tracks[0].width);
    }
    const float full = tracks[0].width;
    EXPECT_NEAR(fills[0].width, full * 0.50f, 1.0f);
    EXPECT_NEAR(fills[1].width, full * 0.25f, 1.0f);
    EXPECT_NEAR(fills[2].width, full * 0.25f, 1.0f);
    EXPECT_NEAR(marks[0].x, tracks[1].x + (full - marks[0].width) * 0.75f, 1.0f);
    EXPECT_LT(marks[0].y, tracks[1].y) << "the tick stands out of the bar";
}

TEST(OverlayPanelTest, TheStripIsNarrowAndWrapsBeforeLeavingTheScreen) {
    Inputs in = WithFrameGeneration(FullInputs());
    const Scene oneBand = BuildScene(Layout::Strip, in);
    EXPECT_LT(oneBand.height, BuildScene(Layout::Hero, in).height / 2.0f);
    EXPECT_GT(oneBand.width, BuildScene(Layout::Hero, in).width);
    EXPECT_TRUE(std::any_of(oneBand.rects.begin(), oneBand.rects.end(),
                            [](const RectOp& rect) { return rect.fill == Fill::Rule; }));

    in.maxWidth = oneBand.width * 0.6f;
    const Scene wrapped = BuildScene(Layout::Strip, in);
    EXPECT_LE(wrapped.width, in.maxWidth);
    EXPECT_GT(wrapped.height, oneBand.height);
    const FakeMeasure measure;
    ExpectTidy(wrapped, measure, "wrapped strip");
    EXPECT_TRUE(Shows(wrapped, "REC 00:00:01")) << "wrapping loses nothing";
    EXPECT_TRUE(Shows(wrapped, "31.93"));

    // A window narrower than any cell still draws everything rather than looping or dropping it.
    in.maxWidth = 10.0f;
    const Scene narrow = BuildScene(Layout::Strip, in);
    EXPECT_TRUE(Shows(narrow, "140"));
    EXPECT_TRUE(Shows(narrow, "REC 00:00:01"));
}

TEST(OverlayPanelTest, AGraphOnlyPanelStillHasItsLabelsInside) {
    Inputs in;
    in.graph.shown = true;
    in.graph.scaleLabel = "Present (no display data)";
    in.graph.peakLabel = "-- ms";
    for (const Layout layout : kLayouts) {
        SCOPED_TRACE(Name(layout));
        const Scene scene = BuildScene(layout, in);
        const FakeMeasure measure;
        ExpectTidy(scene, measure, Name(layout));
        EXPECT_TRUE(scene.graph.valid);
        EXPECT_TRUE(Shows(scene, "no display data"));
    }
    Inputs off = FullInputs();
    off.graph.shown = false;
    for (const Layout layout : kLayouts)
        EXPECT_FALSE(BuildScene(layout, off).graph.valid) << Name(layout);
}

TEST(OverlayPanelTest, MemoryWithoutATotalOrWithoutTelemetry) {
    Inputs in = FullInputs();
    in.vram = {};
    in.ram = {true, false, 12.76f, 0.0f};
    for (const Layout layout : kLayouts) {
        SCOPED_TRACE(Name(layout));
        const Scene scene = BuildScene(layout, in);
        EXPECT_NE(FindExact(scene, "--"), nullptr);
        EXPECT_NE(FindExact(scene, "12.76"), nullptr);
        EXPECT_NE(FindExact(scene, "GB"), nullptr);
        EXPECT_FALSE(Shows(scene, "/ 0.00"));
    }
}

TEST(OverlayPanelTest, UnavailableLatencyKeepsItsPlace) {
    Inputs in = FullInputs();
    in.latencyValid = false;
    in.latencyLabel = "PC Latency";
    for (const Layout layout : kLayouts) {
        SCOPED_TRACE(Name(layout));
        const Scene scene = BuildScene(layout, in);
        EXPECT_TRUE(Shows(scene, "PC Latency"));
        EXPECT_TRUE(Shows(scene, "--"));
        EXPECT_FALSE(Shows(scene, "14.9"));
        EXPECT_EQ(scene.width, BuildScene(layout, FullInputs()).width) << "every label's width is reserved";
    }
}

TEST(OverlayPanelTest, SpacingScalesWithTheDisplay) {
    Inputs doubled = FullInputs();
    doubled.unit = 2.0f;
    for (const Layout layout : kLayouts) {
        SCOPED_TRACE(Name(layout));
        const Scene base = BuildScene(layout, FullInputs());
        const Scene scaled = BuildScene(layout, doubled, FakeMeasure(2.0f));
        // Row pitches round to whole pixels, which adds up over a tall panel.
        EXPECT_NEAR(scaled.width, base.width * 2.0f, base.width * 0.03f);
        EXPECT_NEAR(scaled.height, base.height * 2.0f, base.height * 0.03f);
    }
}

TEST(OverlayPanelTest, RebuildingKeepsNoOldContent) {
    Scene scene;
    const FakeMeasure measure;
    Build(Layout::Meters, FullInputs(), measure, scene);
    ASSERT_FALSE(scene.rects.empty());
    Inputs small;
    small.rowMask = kRowFPS;
    small.fps = 60.0f;
    Build(Layout::List, small, measure, scene);
    EXPECT_FALSE(Shows(scene, "RTX 5070"));
    EXPECT_TRUE(scene.rects.empty());
    EXPECT_FALSE(scene.graph.valid);
}

TEST(OverlayPanelTest, PeakFrameTimeToneComparesWithTheAverage) {
    EXPECT_EQ(PeakFrameTimeTone(10.0f, 8.0f), Tone::Label);
    EXPECT_EQ(PeakFrameTimeTone(13.0f, 8.0f), Tone::Warn);
    EXPECT_EQ(PeakFrameTimeTone(17.0f, 8.0f), Tone::Crit);
    EXPECT_EQ(PeakFrameTimeTone(17.0f, 0.0f), Tone::Label) << "no average yet";
}

}  // namespace
