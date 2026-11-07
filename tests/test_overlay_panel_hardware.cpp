#include <gtest/gtest.h>

#include <cstring>

#include "hook/overlay/custom_overlay_cpu_raster_backend.h"
#include "hook/overlay/overlay_layout_policy.h"
#include "hook/overlay/overlay_panel_draw.h"

namespace {

using namespace ce::overlay_panel;
using namespace ce::overlay_layout;

Inputs HardwareInputs() {
    Inputs in;
    in.rowMask = kRowGPU | kRowCPU | kRowGPUClocks | kRowCPUClocks | kRowRAM | kRowFPS;
    in.api = "DX12";
    in.fps = 144;
    in.gpu.name = "RTX 5070";
    in.gpu.load = {true, 60};
    in.gpu.temperatureC = {true, 61};
    in.gpu.powerW = {true, 135};
    in.gpu.fanRpm = {true, 769};
    in.gpu.coreMhz = {true, 2910};
    in.gpu.memoryMhz = {true, 17001};
    in.gpu.voltageV = {true, 0.92f};
    in.cpu.name = "5700X";
    in.cpu.load = 14;
    in.cpu.peakCoreLoad = 52;
    in.cpu.temperatureC = {true, 73};
    in.cpu.powerW = {true, 72};
    in.cpu.coreMhz = {true, 4191};
    in.cpu.peakCoreMhz = {true, 4569};
    in.ram = {true, true, 12.76f, 31.93f};
    in.graph.shown = true;
    in.graph.scaleLabel = "33 ms";
    in.graph.peakLabel = "13.8 ms";
    return in;
}

Inputs WithoutReadings(Inputs in) {
    in.rowMask &= ~(kRowGPUClocks | kRowCPUClocks);
    in.gpu.temperatureC = {};
    in.gpu.powerW = {};
    in.gpu.fanRpm = {};
    in.gpu.coreMhz = {};
    in.gpu.memoryMhz = {};
    in.gpu.voltageV = {};
    in.cpu.temperatureC = {};
    in.cpu.powerW = {};
    in.cpu.coreMhz = {};
    in.cpu.peakCoreMhz = {};
    return in;
}

const TextOp* FindText(const Scene& scene, const char* fragment) {
    for (const TextOp& text : scene.texts) {
        if (std::strstr(text.text, fragment))
            return &text;
    }
    return nullptr;
}

TEST(OverlayHardwareHistoryTest, MissingReadingsAndRecoveryKeepEveryLayoutInPlace) {
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        CustomOverlay::CpuRasterBackend backend;
        CustomOverlay::Renderer renderer;
        ASSERT_TRUE(renderer.Initialize(&backend, scale));
        RendererMeasure measure(renderer);
        Inputs live = HardwareInputs();
        live.unit = static_cast<float>(renderer.GetTextMetrics(CustomOverlay::TextSize::Base).pixelSize) / 14.0f;
        HardwareHistory history;
        history.Apply(live);
        Inputs dropped = WithoutReadings(live);
        history.Apply(dropped);
        EXPECT_EQ(dropped.rowMask, live.rowMask);
        EXPECT_EQ(dropped.sensorColumns, live.sensorColumns);
        Inputs recovered = HardwareInputs();
        recovered.unit = live.unit;
        recovered.gpu.fanRpm = {true, 0};  // A stopped fan is still a real reading.
        history.Apply(recovered);
        for (Layout layout : {Layout::Hero, Layout::List, Layout::Meters, Layout::Strip}) {
            SCOPED_TRACE(static_cast<int>(layout));
            SCOPED_TRACE(scale);
            Scene a, b, c;
            Build(layout, live, measure, a);
            Build(layout, dropped, measure, b);
            Build(layout, recovered, measure, c);
            EXPECT_EQ(a.width, b.width);
            EXPECT_EQ(a.height, b.height);
            EXPECT_EQ(a.width, c.width);
            EXPECT_EQ(a.height, c.height);
            EXPECT_EQ(a.graph.x, b.graph.x);
            EXPECT_EQ(a.graph.y, b.graph.y);
            EXPECT_EQ(a.graph.width, b.graph.width);
            EXPECT_NE(FindText(b, "--"), nullptr);
            EXPECT_EQ(FindText(b, "17001"), nullptr) << "No stale sensor value survives a missing sample";
            for (const char* anchor : {"5700X", "RAM", "31.93"}) {
                const TextOp* before = FindText(a, anchor);
                const TextOp* after = FindText(b, anchor);
                ASSERT_TRUE(before && after) << anchor;
                EXPECT_EQ(before->x, after->x) << anchor;
                EXPECT_EQ(before->y, after->y) << anchor;
            }
        }
        renderer.Shutdown();
    }
}

TEST(OverlayHardwareHistoryTest, HiddenRowsAndANewRendererDoNotReserveUnreadableHardware) {
    HardwareHistory history;
    Inputs hidden = HardwareInputs();
    hidden.rowMask = kRowFPS;
    history.Apply(hidden);
    Inputs empty = WithoutReadings(HardwareInputs());
    history.Apply(empty);
    EXPECT_EQ(empty.sensorColumns, 0u);
    EXPECT_EQ(empty.rowMask & (kRowGPUClocks | kRowCPUClocks), 0u);
    EXPECT_FALSE(empty.gpu.fanRpm.Shown());

    Inputs live = HardwareInputs();
    history.Apply(live);
    Inputs dropped = WithoutReadings(live);
    history.Apply(dropped);
    EXPECT_TRUE(dropped.gpu.memoryMhz.Shown());
    history = {};
    Inputs fresh = WithoutReadings(HardwareInputs());
    history.Apply(fresh);
    EXPECT_EQ(fresh.sensorColumns, 0u);
    EXPECT_EQ(fresh.rowMask & (kRowGPUClocks | kRowCPUClocks), 0u);
    EXPECT_FALSE(fresh.gpu.memoryMhz.Shown());
}

}  // namespace
