#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "hook/overlay/custom_overlay.h"

// Text and solid shapes used to alternate commands, so an overlay frame took four draw calls and a
// texture and stage setup for each switch, all on the game's thread (Borderlands 2, session
// 20261103_194318: 9.2 us of draw per frame). A backend whose textured path reproduces a vertex color
// exactly on opaque white has shapes sample the atlas' white block instead, and one command is left.
namespace {

using CustomOverlay::DrawCommand;
using CustomOverlay::DrawVertex;
using CustomOverlay::Renderer;
using CustomOverlay::RendererBackend;

class RecordingBackend : public RendererBackend {
public:
    bool Initialize(int, int, const uint8_t*) override {
        return true;
    }
    void Shutdown() override {}
    void Render(const std::vector<DrawVertex>&, const std::vector<uint16_t>&, const std::vector<DrawCommand>&, int,
                int) override {
        ++renders;
    }
    int renders = 0;
};

class FontTextureShapesBackend final : public RecordingBackend {
public:
    bool DrawsShapesWithFontTexture() const override {
        return true;
    }
};

constexpr uint32_t kColor = 0xFF102030;

void DrawMixedFrame(Renderer& renderer) {
    constexpr int kSamples = 8;
    const float samples[kSamples] = {4, 5, 6, 5, 7, 6, 5, 6};
    renderer.BeginFrame(640, 480);
    renderer.DrawRectFilled(0, 0, 100, 100, kColor);
    renderer.DrawText(4, 4, "FPS", kColor);
    renderer.DrawLine(0, 50, 100, 50, kColor);
    renderer.DrawText(4, 60, "CPU", kColor);
    renderer.DrawFrameTimeGraph(0, 70, 100, 20, samples, kSamples, 0.0f, 10.0f, kColor);
    renderer.EndFrame();
}

// A vertex is a shape's when it does not sample a glyph; the text in these frames has none at this UV.
size_t CountAtUV(const std::vector<DrawVertex>& vertices, float u, float v) {
    size_t count = 0;
    for (const DrawVertex& vertex : vertices) {
        if (vertex.u == u && vertex.v == v)
            ++count;
    }
    return count;
}

}  // namespace

TEST(OverlayShapeBatchingTest, ShapesAndTextShareOneCommandThatSamplesTheWhiteBlock) {
    FontTextureShapesBackend backend;
    Renderer renderer;
    ASSERT_TRUE(renderer.Initialize(&backend, 1.0f));
    DrawMixedFrame(renderer);

    ASSERT_EQ(renderer.GetCommands().size(), 1u);
    const DrawCommand& command = renderer.GetCommands().front();
    EXPECT_TRUE(command.useTexture);
    EXPECT_EQ(command.indexCount, renderer.GetIndices().size());

    const float u = renderer.GetFont()->GetWhiteU();
    const float v = renderer.GetFont()->GetWhiteV();
    const size_t shapeVertices = CountAtUV(renderer.GetVertices(), u, v);
    EXPECT_GE(shapeVertices, 8u) << "the rect and the line, at least";
    EXPECT_EQ(CountAtUV(renderer.GetVertices(), 0.0f, 0.0f), 0u) << "no shape may sample the atlas' empty corner";
    EXPECT_LT(shapeVertices, renderer.GetVertices().size()) << "the glyphs keep their own coordinates";

    // Where that UV lands: opaque white, whichever neighbor a linear filter picks.
    const CustomOverlay::FontAtlas& atlas = *renderer.GetFont();
    const int centerX = static_cast<int>(u * static_cast<float>(atlas.GetTextureWidth()) + 0.5f);
    const int centerY = static_cast<int>(v * static_cast<float>(atlas.GetTextureHeight()) + 0.5f);
    for (int y = centerY - 1; y <= centerY; ++y) {
        for (int x = centerX - 1; x <= centerX; ++x) {
            const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(atlas.GetTextureWidth()) +
                                  static_cast<size_t>(x)) *
                                 4u;
            const uint8_t* texel = atlas.GetTextureData() + index;
            EXPECT_EQ(texel[0], 255);
            EXPECT_EQ(texel[1], 255);
            EXPECT_EQ(texel[2], 255);
            EXPECT_EQ(texel[3], 255) << x << "," << y;
        }
    }
}

TEST(OverlayShapeBatchingTest, ABackendThatDidNotOptInKeepsItsSolidCommands) {
    RecordingBackend backend;
    Renderer renderer;
    ASSERT_TRUE(renderer.Initialize(&backend, 1.0f));
    DrawMixedFrame(renderer);

    EXPECT_GT(renderer.GetCommands().size(), 1u);
    bool sawSolid = false;
    for (const DrawCommand& command : renderer.GetCommands())
        sawSolid = sawSolid || !command.useTexture;
    EXPECT_TRUE(sawSolid);
    EXPECT_GE(CountAtUV(renderer.GetVertices(), 0.0f, 0.0f), 8u) << "solid geometry samples nothing";
}

// The tail merges into the command before it, so rewinding must give that command its old size back.
TEST(OverlayShapeBatchingTest, RewindingARowsAndGraphFrameShrinksTheMergedCommand) {
    FontTextureShapesBackend backend;
    Renderer renderer;
    ASSERT_TRUE(renderer.Initialize(&backend, 1.0f));
    renderer.BeginFrame(640, 480);
    renderer.DrawText(4, 4, "FPS", kColor);
    renderer.MarkTail();
    const uint32_t rowIndices = static_cast<uint32_t>(renderer.GetIndices().size());
    renderer.DrawRectFilled(0, 0, 10, 10, kColor);
    renderer.EndFrame();
    ASSERT_EQ(renderer.GetCommands().size(), 1u);
    ASSERT_EQ(renderer.GetCommands().front().indexCount, rowIndices + 6u);

    ASSERT_TRUE(renderer.RewindToTail());
    ASSERT_EQ(renderer.GetCommands().size(), 1u);
    EXPECT_EQ(renderer.GetCommands().front().indexCount, rowIndices);
    renderer.DrawRectFilled(5, 5, 10, 10, kColor);
    renderer.EndFrame();
    ASSERT_EQ(renderer.GetCommands().size(), 1u);
    EXPECT_EQ(renderer.GetCommands().front().indexCount, rowIndices + 6u);
    EXPECT_EQ(backend.renders, 2);
}
