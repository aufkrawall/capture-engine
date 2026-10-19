#include <gtest/gtest.h>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "hook/metrics/performance_metrics.h"
#include "hook/overlay/overlay_adapter.h"
#include "hook/runtime/ipc_client.h"

namespace {

struct BackendCounts {
    int renders = 0;
    int dataChanges = 0;
    size_t vertices = 0;
    size_t commands = 0;
    std::vector<CustomOverlay::DrawVertex> lastVertices;
    std::vector<CustomOverlay::DrawCommand> lastCommands;
};

class CountingBackend final : public CustomOverlay::RendererBackend {
public:
    explicit CountingBackend(BackendCounts& counts) : counts_(counts) {}
    bool Initialize(int, int, const uint8_t*) override {
        return true;
    }
    void Shutdown() override {}
    void OnDrawDataChanged() override {
        ++counts_.dataChanges;
    }
    void Render(const std::vector<CustomOverlay::DrawVertex>& vertices, const std::vector<uint16_t>&,
                const std::vector<CustomOverlay::DrawCommand>& commands, int, int) override {
        ++counts_.renders;
        counts_.vertices = vertices.size();
        counts_.commands = commands.size();
        counts_.lastVertices = vertices;
        counts_.lastCommands = commands;
    }

private:
    BackendCounts& counts_;
};

struct OverlayHarness {
    explicit OverlayHarness(bool frameTimeGraph) {
        OverlayConfig cfg{};
        cfg.showOverlay = true;
        cfg.showFPS = true;
        cfg.showFrameTime = frameTimeGraph;
        cfg.showCPU = true;
        cfg.showGPU = true;
        cfg.showRAM = true;
        cfg.showVRAM = true;
        cfg.showRecording = true;
        cfg.showFG = true;
        cfg.textUpdateInterval = 500;
        shm->BeginWriteOverlayConfig();
        shm->overlayConfig = cfg;
        shm->EndWriteOverlayConfig();
        ipc.AttachIsolatedHostMemory(shm.get());
        initialized = adapter.InitCustom(new CountingBackend(counts), OverlayBackendType::DX11);
        adapter.SetIPCClient(&ipc);
        adapter.SetMetrics(&metrics);
    }
    ~OverlayHarness() {
        adapter.Shutdown();
    }
    // Frame times without drawing, so the graph has a line from the first frame on.
    void Feed(int presents) {
        for (int i = 0; i < presents; ++i) {
            clockUs += 1600;
            metrics.Update(clockUs);
        }
    }
    // Present-to-present at 625 FPS, then one overlay frame.
    void Frame() {
        clockUs += 1600;
        metrics.Update(clockUs);
        adapter.RenderOverlay(3840, 2160);
    }

    std::unique_ptr<SharedMemoryLayout> shm = std::make_unique<SharedMemoryLayout>();
    IPCClient ipc;
    PerformanceMetrics metrics;
    OverlayAdapter adapter;
    BackendCounts counts;
    int64_t clockUs = 1'000'000;
    bool initialized = false;
};

// The graph moves every frame; the rows change at the text update interval (500 ms). Every frame
// used to rebuild all of the overlay's geometry while the graph was shown (Mirror's Edge on DXUP at
// 630 FPS, 20261018_151515: 12.9 us per frame here against 0.3 us for a cached one).
TEST(OverlayFrameCostTest, TheGraphAloneMovingRedrawsOnlyTheGraph) {
    OverlayHarness harness(true);
    ASSERT_TRUE(harness.initialized);
    harness.Feed(300);
    harness.Frame();  // the first frame is a full build
    ASSERT_EQ(harness.adapter.FullFramesForTest(), 1u);
    const std::vector<CustomOverlay::DrawVertex> builtVertices = harness.counts.lastVertices;
    const std::vector<CustomOverlay::DrawCommand> builtCommands = harness.counts.lastCommands;
    ASSERT_GE(builtCommands.size(), 2u) << "rows, then the graph line last";
    const CustomOverlay::DrawCommand graph = builtCommands.back();
    ASSERT_FALSE(graph.useTexture);

    for (int i = 0; i < 200; ++i)
        harness.Frame();
    // The 2 s labels and the scale stay put within this run, so every later frame is graph-only.
    EXPECT_EQ(harness.adapter.FullFramesForTest(), 1u);
    EXPECT_EQ(harness.adapter.GraphOnlyFramesForTest(), 200u);
    // The rows' geometry is exactly the full build's; the graph line is redrawn in its place.
    ASSERT_EQ(harness.counts.lastCommands.size(), builtCommands.size());
    ASSERT_GE(harness.counts.lastVertices.size(), graph.vertexOffset);
    EXPECT_EQ(std::memcmp(harness.counts.lastVertices.data(), builtVertices.data(),
                          graph.vertexOffset * sizeof(CustomOverlay::DrawVertex)),
              0);
    for (size_t i = 0; i + 1 < builtCommands.size(); ++i) {
        EXPECT_EQ(harness.counts.lastCommands[i].indexOffset, builtCommands[i].indexOffset) << i;
        EXPECT_EQ(harness.counts.lastCommands[i].indexCount, builtCommands[i].indexCount) << i;
    }
    EXPECT_EQ(harness.counts.lastCommands.back().vertexOffset, graph.vertexOffset);
    EXPECT_FALSE(harness.counts.lastCommands.back().useTexture);
    EXPECT_EQ(harness.counts.dataChanges, 201) << "each redraw still uploads its new geometry";
}

}  // namespace

namespace {
// FNV-1a over the bytes of the graph's geometry, so a faster polyline must emit exactly the same.
uint64_t GraphGeometryHash(const CustomOverlay::Renderer& renderer) {
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&](const void* data, size_t bytes) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i)
            hash = (hash ^ p[i]) * 1099511628211ull;
    };
    mix(renderer.GetVertices().data(), renderer.GetVertices().size() * sizeof(CustomOverlay::DrawVertex));
    mix(renderer.GetIndices().data(), renderer.GetIndices().size() * sizeof(uint16_t));
    return hash;
}

// The polyline was rewritten for speed (one normal per segment, no per-element push_back: 6.4 us ->
// 1.7 us per graph here). Its geometry must stay byte for byte what it was; these are the hashes of
// the previous implementation. A deliberate visual change re-records them.
TEST(OverlayFrameCostTest, GraphGeometryIsByteIdenticalToThePreviousPolyline) {
    BackendCounts counts;
    CountingBackend backend(counts);
    CustomOverlay::Renderer renderer;
    ASSERT_TRUE(renderer.Initialize(&backend, 1.25f));
    // Leading empty samples, a spread of frame times and spikes past the scale (bevel joins).
    float data[182];
    for (int i = 0; i < 182; ++i)
        data[i] =
            i < 5 ? 0.0f : 1.6f + 9.0f * static_cast<float>((i * 7919) % 13) / 13.0f + (i % 37 == 0 ? 30.0f : 0.0f);
    const struct {
        float offset;
        uint64_t hash;
        size_t vertices;
        size_t indices;
    } expected[] = {{0.0f, 0x974bfb62d5da89fbull, 928, 4158},
                    {0.37f, 0x258458b2c8dece70ull, 932, 4176},
                    {0.99f, 0xa29e813bb6bc5fe5ull, 932, 4176}};
    for (const auto& frame : expected) {
        SCOPED_TRACE(frame.offset);
        renderer.BeginFrame(3840, 2160);
        renderer.DrawFrameTimeGraph(10.5f, 10.25f, 300.0f, 36.0f, data, 182, 0.4f, 33.0f, 0xFF00FFFF, frame.offset);
        EXPECT_EQ(renderer.GetVertices().size(), frame.vertices);
        EXPECT_EQ(renderer.GetIndices().size(), frame.indices);
        EXPECT_EQ(GraphGeometryHash(renderer), frame.hash);
    }
    renderer.Shutdown();
}
}  // namespace
