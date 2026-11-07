#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "hook/overlay/custom_overlay_cpu_raster_backend.h"
#include "hook/overlay/overlay_layout_policy.h"
#include "hook/overlay/overlay_panel_draw.h"

namespace {

using namespace ce::overlay_panel;
using namespace ce::overlay_layout;

Inputs ViewportInputs() {
    Inputs in;
    in.rowMask = kRowGPU | kRowCPU | kRowGPUClocks | kRowCPUClocks | kRowVRAM | kRowRAM | kRowFPS |
                 kRowFPSAverages | kRowSystemLatency | kRowRecording | kRowFGStatus | kRowFGRates |
                 kRowNotification | kRowBenchmarkTimer | kRowBenchmarkFPS;
    in.api = "DX9 -> DX11 -> DX12 (DXUP)";
    in.fps = 144;
    in.averageFps = 125;
    in.low1Fps = 95;
    in.low01Fps = 73;
    in.latencyLabel = "Latency est.";
    in.latencyValid = true;
    in.latencyMs = 14.9f;
    in.fgLabel = "DLSS FG";
    in.fgMultiplier = 3;
    in.fgBaseFps = 48;
    in.fgOutputFps = 144;
    in.gpu.name = "Radeon RX 7900 XTX Limited";
    in.gpu.load = {true, 60};
    in.gpu.temperatureC = {true, 61};
    in.gpu.powerW = {true, 135};
    in.gpu.fanRpm = {true, 769};
    in.gpu.coreMhz = {true, 2910};
    in.gpu.memoryMhz = {true, 17001};
    in.gpu.voltageV = {true, 0.92f};
    in.cpu.name = "Ryzen 9 9950X3D";
    in.cpu.load = 14;
    in.cpu.peakCoreLoad = 52;
    in.cpu.temperatureC = {true, 72};
    in.cpu.powerW = {true, 75};
    in.cpu.coreMhz = {true, 4191};
    in.cpu.peakCoreMhz = {true, 4569};
    in.sensorColumns = kColumnTemperature | kColumnPower | kColumnClock | kColumnFan;
    in.vram = {true, true, 4.31f, 11.66f};
    in.ram = {true, true, 12.76f, 31.93f};
    in.recording.kind = RecordingKind::Live;
    in.recording.text = "REC 00:00:01";
    in.recording.warning = "!ENC SEVERE 9999.9/9999!";
    in.notification = "Finalizing recording...";
    in.benchmarkTimer = "00:10 / 01:00";
    in.benchmarkFps = "1234.5 / 1111.1 / 1000.1";
    in.graph.shown = true;
    in.graph.scaleLabel = "100 ms | Present (no display data)";
    in.graph.peakLabel = "13.8 ms";
    return in;
}

void ExpectInside(const Scene& scene, const RendererMeasure& measure, float limit) {
    EXPECT_LE(scene.width, limit);
    EXPECT_GT(scene.width, 0);
    for (const TextOp& op : scene.texts) {
        EXPECT_GE(op.x, 0) << op.text;
        EXPECT_LE(op.x + measure.Width(op.text, op.size), scene.width) << op.text;
        const FontMetrics font = measure.Font(op.size);
        EXPECT_GE(op.y + font.ascent - font.digitHeight, 0) << op.text;
        EXPECT_LE(op.y + font.ascent, scene.height) << op.text;
    }
    for (size_t i = 0; i < scene.texts.size(); ++i) {
        const TextOp& a = scene.texts[i];
        const FontMetrics af = measure.Font(a.size);
        const float ab = a.y + af.ascent;
        for (size_t j = i + 1; j < scene.texts.size(); ++j) {
            const TextOp& b = scene.texts[j];
            const FontMetrics bf = measure.Font(b.size);
            const float bb = b.y + bf.ascent;
            const bool apart = a.x + measure.Width(a.text, a.size) <= b.x ||
                               b.x + measure.Width(b.text, b.size) <= a.x ||
                               ab <= bb - bf.digitHeight || bb <= ab - af.digitHeight;
            EXPECT_TRUE(apart) << a.text << " overlaps " << b.text;
        }
    }
    for (const RectOp& op : scene.rects) {
        EXPECT_GE(op.x, 0);
        EXPECT_LE(op.x + op.width, scene.width);
        EXPECT_GE(op.y, 0);
        EXPECT_LE(op.y + op.height, scene.height);
    }
    ASSERT_TRUE(scene.graph.valid);
    EXPECT_LE(scene.graph.x + scene.graph.width, scene.width);
    EXPECT_LE(scene.graph.y + scene.graph.height, scene.height);
}

TEST(OverlayViewportTest, NarrowLayoutsKeepEveryValueAtItsRasterizedFontSize) {
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        CustomOverlay::CpuRasterBackend backend;
        CustomOverlay::Renderer renderer;
        ASSERT_TRUE(renderer.Initialize(&backend, scale));
        RendererMeasure measure(renderer);
        for (Layout layout : {Layout::Hero, Layout::List, Layout::Meters}) {
            for (float limit : {280.0f, 400.0f, 600.0f}) {
                SCOPED_TRACE(static_cast<int>(layout));
                SCOPED_TRACE(scale);
                SCOPED_TRACE(limit);
                Inputs in = ViewportInputs();
                in.unit = static_cast<float>(renderer.GetTextMetrics(CustomOverlay::TextSize::Base).pixelSize) / 14.0f;
                in.maxWidth = limit;
                Scene scene;
                Build(layout, in, measure, scene);
                ExpectInside(scene, measure, limit);
                std::string text;
                for (const TextOp& op : scene.texts) {
                    text += op.text;
                    text += ' ';
                }
                for (const char* value : {"144", "125", "95", "73", "14.9", "DLSS", "48", "60%", "61\xB0",
                                          "135", "769", "2910", "17001", "0.920", "14%", "52%", "72\xB0", "75",
                                          "4191", "4569", "4.31", "11.66", "12.76", "31.93", "REC", "SEVERE",
                                          "1234.5", "1111.1", "1000.1", "13.8"})
                    EXPECT_NE(text.find(value), std::string::npos) << value;
                if (layout == Layout::Meters)
                    EXPECT_GE(scene.rects.size(), 8u) << "Load and memory bars remain visible";
            }
        }
        renderer.Shutdown();
    }
}

TEST(OverlayViewportTest, ReviewReproductionFitsSixHundredPixelsAtTwoHundredPercent) {
    CustomOverlay::CpuRasterBackend backend;
    CustomOverlay::Renderer renderer;
    ASSERT_TRUE(renderer.Initialize(&backend, 2.0f));
    RendererMeasure measure(renderer);
    Inputs in = ViewportInputs();
    in.rowMask &= ~(kRowNotification | kRowBenchmarkTimer | kRowBenchmarkFPS | kRowFGStatus | kRowFGRates);
    in.recording.kind = RecordingKind::NotRecording;
    in.recording.text = "NOT RECORDING";
    in.recording.warning = "";
    in.unit = static_cast<float>(renderer.GetTextMetrics(CustomOverlay::TextSize::Base).pixelSize) / 14.0f;
    in.maxWidth = 600;  // 640 px viewport, default 10 px padding at 200% on both sides.
    Scene scene;
    Build(Layout::Hero, in, measure, scene);
    ExpectInside(scene, measure, 600);
    for (const TextOp& op : scene.texts)
        EXPECT_LE(20 + op.x + measure.Width(op.text, op.size), 620) << op.text;
    renderer.Shutdown();
}

TEST(OverlayViewportTest, WrappedHardwareDropoutAndWarningBlinksKeepTheirSpace) {
    CustomOverlay::CpuRasterBackend backend;
    CustomOverlay::Renderer renderer;
    ASSERT_TRUE(renderer.Initialize(&backend, 2.0f));
    RendererMeasure measure(renderer);
    for (Layout layout : {Layout::Hero, Layout::List, Layout::Meters}) {
        HardwareHistory history;
        Inputs live = ViewportInputs();
        live.unit = 2;
        live.maxWidth = 280;
        live.recording.kind = RecordingKind::NotRecording;
        live.recording.text = "NOT RECORDING";
        live.recording.warning = "";
        history.Apply(live);
        Inputs missing = live;
        missing.gpu.memoryMhz = {};
        missing.gpu.voltageV = {};
        missing.gpu.fanRpm = {};
        missing.cpu.peakCoreMhz = {};
        missing.recording.visible = false;
        history.Apply(missing);
        Scene a, b;
        Build(layout, live, measure, a);
        Build(layout, missing, measure, b);
        EXPECT_EQ(a.width, b.width) << static_cast<int>(layout);
        EXPECT_EQ(a.height, b.height) << static_cast<int>(layout);
        EXPECT_EQ(a.graph.y, b.graph.y) << static_cast<int>(layout);
    }
    renderer.Shutdown();
}

TEST(OverlayViewportTest, LongUnbrokenNamesWrapWithoutLosingCharacters) {
    CustomOverlay::CpuRasterBackend backend;
    CustomOverlay::Renderer renderer;
    ASSERT_TRUE(renderer.Initialize(&backend, 2.0f));
    RendererMeasure measure(renderer);
    Inputs in;
    in.unit = 2;
    in.maxWidth = 200;
    in.rowMask = kRowGPU;
    in.gpu.name = "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (Layout layout : {Layout::Hero, Layout::List, Layout::Meters}) {
        Scene scene;
        Build(layout, in, measure, scene);
        std::string name;
        for (const TextOp& text : scene.texts) {
            if (text.text[0] >= 'A' && text.text[0] <= 'Z' && !std::strstr(text.text, "LOAD"))
                name += text.text;
            EXPECT_LE(text.x + measure.Width(text.text, text.size), scene.width);
        }
        EXPECT_EQ(name, in.gpu.name);
    }
    renderer.Shutdown();
}

}  // namespace
