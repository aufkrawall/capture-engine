#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "hook/overlay/custom_overlay_cpu_raster_backend.h"
#include "hook/overlay/overlay_cpu_raster.h"
#include "hook/overlay/overlay_layout_policy.h"
#include "hook/overlay/overlay_panel_draw.h"

// Draws the panels through the real renderer, atlas and CPU rasterizer, so a layout change can be looked at
// and its pixels trusted without starting a game. Set CE_OVERLAY_PREVIEW_DIR to write BMP files there, one
// per layout and state, on a dark and a bright background. Unset, the same code still runs and checks that
// the panel really reaches the pixels.

namespace {

using namespace ce::overlay_panel;
using namespace ce::overlay_layout;

const char* const kLatencyLabels[] = {"PC Latency~", "Latency est.", "PC Latency"};

Inputs PreviewInputs(const char* state) {
    Inputs in;
    in.rowMask = kRowGPU | kRowGPUClocks | kRowCPU | kRowCPUClocks | kRowVRAM | kRowRAM | kRowFPS | kRowFPSAverages |
                 kRowSystemLatency | kRowRecording;
    in.api = "DX11 -> DX12 (DXUP)";
    in.fps = 140.0f;
    in.averageFps = 100.0f;
    in.low1Fps = 20.0f;
    in.low01Fps = 20.0f;
    in.latencyLabel = "Latency est.";
    in.latencyLabelChoices = kLatencyLabels;
    in.latencyLabelChoiceCount = 3;
    in.latencyValid = true;
    in.latencyMs = 14.9f;
    in.gpu.name = "RTX 5070";
    in.gpu.load = {true, 60.0f};
    in.gpu.temperatureC = {true, 60.0f};
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
    if (std::string(state) == "fg") {
        in.rowMask |= kRowFGRates | kRowFGStatus;
        in.rowMask &= ~static_cast<uint32_t>(kRowRecording);
        in.fgLabel = "DLSS FG";
        in.fgMultiplier = 3;
        in.fgBaseFps = 46.0f;
        in.fgOutputFps = 139.0f;
        in.fps = 139.0f;
        in.averageFps = 136.0f;
        in.low1Fps = 112.0f;
        in.low01Fps = 94.0f;
        in.latencyMs = 31.6f;
        in.graph.peakTone = Tone::Label;
        in.graph.peakLabel = "9.4 ms";
    } else if (std::string(state) == "stress") {
        in.recording.text = "REC 00:12:47";
        in.recording.warning = "!ENCODER OVERLOAD!";
        in.fps = 38.0f;
        in.averageFps = 61.0f;
        in.low1Fps = 19.0f;
        in.low01Fps = 11.0f;
        in.latencyMs = 62.4f;
        in.gpu.load = {true, 99.0f};
        in.gpu.temperatureC = {true, 84.0f};
        in.gpu.powerW = {true, 214.0f};
        in.gpu.fanRpm = {true, 2140.0f};
        in.cpu.load = 96.0f;
        in.cpu.peakCoreLoad = 100.0f;
        in.vram = {true, true, 11.40f, 11.66f};
        in.ram = {true, true, 29.00f, 31.93f};
        in.graph.scaleLabel = "100 ms | Display";
        in.graph.peakLabel = "84.1 ms";
        in.graph.peakTone = Tone::Crit;
    }
    return in;
}

void WriteBmp(const std::string& path, int width, int height, const std::vector<uint32_t>& pixels) {
    FILE* file = nullptr;
    ASSERT_EQ(fopen_s(&file, path.c_str(), "wb"), 0) << path;
    const uint32_t bytes = static_cast<uint32_t>(pixels.size() * 4u);
    const uint32_t fileSize = 54u + bytes;
    const uint32_t offset = 54u;
    const uint32_t headerSize = 40u;
    const int32_t topDownHeight = -height;
    const uint16_t planes = 1;
    const uint16_t bits = 32;
    const uint32_t zero = 0;
    std::fwrite("BM", 1, 2, file);
    std::fwrite(&fileSize, 4, 1, file);
    std::fwrite(&zero, 4, 1, file);
    std::fwrite(&offset, 4, 1, file);
    std::fwrite(&headerSize, 4, 1, file);
    std::fwrite(&width, 4, 1, file);
    std::fwrite(&topDownHeight, 4, 1, file);
    std::fwrite(&planes, 2, 1, file);
    std::fwrite(&bits, 2, 1, file);
    for (int i = 0; i < 6; ++i)
        std::fwrite(&zero, 4, 1, file);
    std::fwrite(pixels.data(), 4, pixels.size(), file);
    std::fclose(file);
}

// A premultiplied BGRA panel over a flat color, by source-over.
std::vector<uint32_t> Composite(const std::vector<uint32_t>& panel, uint32_t background) {
    std::vector<uint32_t> out(panel.size());
    for (size_t i = 0; i < panel.size(); ++i) {
        const uint32_t src = panel[i];
        const uint32_t inverse = 255u - (src >> 24);
        uint32_t pixel = 0xFF000000u;
        for (int shift = 0; shift < 24; shift += 8) {
            const uint32_t s = (src >> shift) & 0xFFu;
            const uint32_t b = (background >> shift) & 0xFFu;
            pixel |= (std::min)(255u, s + (b * inverse + 127u) / 255u) << shift;
        }
        out[i] = pixel;
    }
    return out;
}

TEST(OverlayPanelPreview, ThePanelsReachThePixels) {
    const char* directory = std::getenv("CE_OVERLAY_PREVIEW_DIR");
    const char* fontName = std::getenv("CE_OVERLAY_PREVIEW_FONT");
    const char* scaleText = std::getenv("CE_OVERLAY_PREVIEW_SCALE");
    const char* widthText = std::getenv("CE_OVERLAY_PREVIEW_MAX_WIDTH");
    const float dpiScale = scaleText ? static_cast<float>(std::atof(scaleText)) : 1.5f;

    const struct {
        Layout layout;
        const char* name;
    } layouts[] = {{Layout::Hero, "hero"}, {Layout::List, "list"}, {Layout::Meters, "meters"}, {Layout::Strip, "strip"}};

    for (const auto& layout : layouts) {
        for (const char* state : {"rec", "fg", "stress"}) {
            SCOPED_TRACE(std::string(layout.name) + " " + state);
            CustomOverlay::CpuRasterBackend backend;
            CustomOverlay::Renderer renderer;
            CustomOverlay::FontRequest font;
            font.face = fontName;
            ASSERT_TRUE(renderer.Initialize(&backend, dpiScale, font));

            Inputs in = PreviewInputs(state);
            in.unit = static_cast<float>(renderer.GetTextMetrics(CustomOverlay::TextSize::Base).pixelSize) / 14.0f;
            in.maxWidth = widthText ? static_cast<float>(std::atof(widthText)) : 0.0f;
            Scene scene;
            RendererMeasure measure(renderer);
            Build(layout.layout, in, measure, scene);
            ASSERT_GT(scene.width, 0.0f);

            const int padding = 16;
            const int width = static_cast<int>(scene.width) + padding * 2;
            const int height = static_cast<int>(scene.height) + padding * 2;
            renderer.BeginFrame(width, height);
            SceneStyle style;
            style.background = 0xB8000000u;  // bg_alpha=0.72
            style.value = 0xFFFFFFFFu;
            style.fps = 0xFF05FAB8u;
            style.shadow = 0xFF000000u;
            style.shadowPixels = ShadowPixelsForScale(dpiScale);
            DrawScene(renderer, scene, static_cast<float>(padding), static_cast<float>(padding), style);
            if (scene.graph.valid) {
                float samples[182];
                for (int i = 0; i < 182; ++i)
                    samples[i] = 7.0f + static_cast<float>((i * 7919) % 13) * 0.25f + (i % 61 == 0 ? 6.0f : 0.0f);
                renderer.DrawFrameTimeGraph(padding + scene.graph.x, padding + scene.graph.y, scene.graph.width,
                                            scene.graph.height, samples, 182, 0.0f, 33.0f, style.fps, 0.0f);
            }
            renderer.EndFrame();

            ce::overlay_cpu_raster::FontAtlasView atlas;
            atlas.pixels = renderer.GetFont()->GetTextureData();
            atlas.width = renderer.GetFont()->GetTextureWidth();
            atlas.height = renderer.GetFont()->GetTextureHeight();
            ce::overlay_cpu_raster::Target target;
            target.width = width;
            target.height = height;
            std::vector<uint32_t> panel;
            ASSERT_TRUE(ce::overlay_cpu_raster::Rasterize(renderer.GetVertices(), renderer.GetIndices(),
                                                          renderer.GetCommands(), atlas, target, panel));

            // Text really came out: inside the panel, some pixels are near-opaque and bright.
            size_t brightPixels = 0;
            for (const uint32_t pixel : panel) {
                if ((pixel >> 24) > 200u && (pixel & 0xFFu) > 180u)
                    ++brightPixels;
            }
            EXPECT_GT(brightPixels, 200u);

            if (directory) {
                const std::string base = std::string(directory) + "\\" + layout.name + "_" + state;
                WriteBmp(base + "_dark.bmp", width, height, Composite(panel, 0x00241640u));
                WriteBmp(base + "_bright.bmp", width, height, Composite(panel, 0x00FFFFFFu));
            }
            renderer.Shutdown();
        }
    }
}

}  // namespace
