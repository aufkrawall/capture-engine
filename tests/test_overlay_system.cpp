/**
 * Overlay system unit tests.
 *
 * FontAtlas tests run entirely on CPU/GDI — no graphics API context required.
 * Renderer tests use a mock backend to verify geometry/command building.
 */

#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include "../hook/common/custom_font.h"
#include "../hook/common/custom_overlay.h"

using namespace CustomOverlay;

// ============================================================================
// FontAtlas tests (pure CPU / Windows GDI — no GPU required)
// ============================================================================

class FontAtlasTest : public ::testing::Test {
protected:
    FontAtlas atlas;

    void SetUp() override {
        // Initialize with a common system font at standard size
        ASSERT_TRUE(atlas.Initialize("Arial", 14, 1.0f));
    }
};

TEST_F(FontAtlasTest, InitializeSucceeds) {
    EXPECT_TRUE(atlas.IsInitialized());
    EXPECT_GT(atlas.GetTextureWidth(), 0);
    EXPECT_GT(atlas.GetTextureHeight(), 0);
    EXPECT_GT(atlas.GetLineHeight(), 0);
}

TEST_F(FontAtlasTest, TextureDataNonEmpty) {
    const uint8_t* data = atlas.GetTextureData();
    ASSERT_NE(data, nullptr);

    // At least one non-zero alpha pixel should exist (the glyphs)
    int w = atlas.GetTextureWidth();
    int h = atlas.GetTextureHeight();
    bool hasPixel = false;
    for (int i = 3; i < w * h * 4; i += 4) {  // Check alpha channel
        if (data[i] > 0) {
            hasPixel = true;
            break;
        }
    }
    EXPECT_TRUE(hasPixel) << "Font atlas texture has no visible pixels";
}

TEST_F(FontAtlasTest, GetGlyphPrintableAscii) {
    // All printable ASCII characters (32-126) must have valid glyphs
    for (int c = 32; c <= 126; c++) {
        const Glyph* g = atlas.GetGlyph((char)c);
        ASSERT_NE(g, nullptr) << "Null glyph for char " << c;
        // Glyph must be within atlas bounds
        EXPECT_LT(g->x + g->width, (uint16_t)(atlas.GetTextureWidth() + 1))
            << "Glyph for char " << c << " extends past texture width";
        EXPECT_LT(g->y + g->height, (uint16_t)(atlas.GetTextureHeight() + 1))
            << "Glyph for char " << c << " extends past texture height";
    }
}

TEST_F(FontAtlasTest, GetGlyphNonPrintableFallsBackToQuestion) {
    // Non-printable chars should map to '?'
    const Glyph* nonPrintable = atlas.GetGlyph('\x01');
    const Glyph* question = atlas.GetGlyph('?');
    ASSERT_NE(nonPrintable, nullptr);
    ASSERT_NE(question, nullptr);
    EXPECT_EQ(nonPrintable->x, question->x);
    EXPECT_EQ(nonPrintable->y, question->y);
}

TEST_F(FontAtlasTest, CalcTextSizeSingleChar) {
    float w = 0, h = 0;
    atlas.CalcTextSize("A", &w, &h);
    EXPECT_GT(w, 0.0f);
    EXPECT_GT(h, 0.0f);
}

TEST_F(FontAtlasTest, CalcTextSizeWidthGrowsWithText) {
    float w1 = 0, h1 = 0, w2 = 0, h2 = 0;
    atlas.CalcTextSize("A", &w1, &h1);
    atlas.CalcTextSize("AAA", &w2, &h2);
    EXPECT_GT(w2, w1) << "Wider text should have larger width";
    EXPECT_FLOAT_EQ(h1, h2) << "Single-line height should be the same";
}

TEST_F(FontAtlasTest, CalcTextSizeMultiLine) {
    float w = 0, h1 = 0, h2 = 0;
    atlas.CalcTextSize("A", &w, &h1);
    atlas.CalcTextSize("A\nB", &w, &h2);
    EXPECT_GT(h2, h1) << "Two-line text should be taller than one-line";
}

TEST_F(FontAtlasTest, CalcTextSizeNullOutput) {
    // Should not crash with null output pointers
    atlas.CalcTextSize("Test", nullptr, nullptr);
}

TEST_F(FontAtlasTest, DigitsHaveNonZeroWidth) {
    for (char c = '0'; c <= '9'; c++) {
        const Glyph* g = atlas.GetGlyph(c);
        ASSERT_NE(g, nullptr);
        EXPECT_GT(g->width, 0u) << "Digit '" << c << "' has zero width";
        EXPECT_GT(g->xAdvance, 0u) << "Digit '" << c << "' has zero advance";
    }
}

TEST_F(FontAtlasTest, ShutdownAndReinitialize) {
    atlas.Shutdown();
    EXPECT_FALSE(atlas.IsInitialized());
    EXPECT_TRUE(atlas.Initialize("Arial", 14, 1.0f));
    EXPECT_TRUE(atlas.IsInitialized());
}

TEST_F(FontAtlasTest, DoubleInitializeIsIdempotent) {
    int w1 = atlas.GetTextureWidth();
    // Second Initialize call on an already-initialized atlas should return true
    // without re-initializing (guarded by if (initialized) return true)
    EXPECT_TRUE(atlas.Initialize("Arial", 14, 1.0f));
    EXPECT_EQ(atlas.GetTextureWidth(), w1);
}

// ============================================================================
// Renderer tests with mock backend
// ============================================================================

class MockBackend : public RendererBackend {
public:
    bool initCalled = false;
    bool shutdownCalled = false;
    int renderCallCount = 0;
    int lastVertexCount = 0;
    int lastCommandCount = 0;

    bool Initialize(int, int, const uint8_t*) override {
        initCalled = true;
        return true;
    }

    void Shutdown() override {
        shutdownCalled = true;
    }

    void Render(const std::vector<DrawVertex>& verts, const std::vector<uint16_t>&,
                const std::vector<DrawCommand>& cmds, int, int) override {
        renderCallCount++;
        lastVertexCount = (int)verts.size();
        lastCommandCount = (int)cmds.size();
    }
};

class RendererTest : public ::testing::Test {
protected:
    MockBackend backend;
    Renderer renderer;

    void SetUp() override {
        ASSERT_TRUE(renderer.Initialize(&backend, 1.0f));
    }
};

TEST_F(RendererTest, InitializeCallsBackend) {
    EXPECT_TRUE(backend.initCalled);
    EXPECT_TRUE(renderer.IsInitialized());
}

TEST_F(RendererTest, BeginEndFrameCallsRender) {
    renderer.BeginFrame(1920, 1080);
    renderer.DrawText(10, 10, "FPS: 60", Colors::White);
    renderer.EndFrame();

    EXPECT_EQ(backend.renderCallCount, 1);
    EXPECT_GT(backend.lastVertexCount, 0) << "Text should produce vertices";
    EXPECT_GT(backend.lastCommandCount, 0) << "Text should produce draw commands";
}

TEST_F(RendererTest, RenderCachedFrameReusesPreviousDrawData) {
    renderer.BeginFrame(1920, 1080);
    renderer.DrawText(10, 10, "FPS: 60", Colors::White);
    renderer.EndFrame();

    const int initialVertexCount = backend.lastVertexCount;
    const int initialCommandCount = backend.lastCommandCount;

    EXPECT_TRUE(renderer.RenderCachedFrame(1920, 1080));
    EXPECT_EQ(backend.renderCallCount, 2);
    EXPECT_EQ(backend.lastVertexCount, initialVertexCount);
    EXPECT_EQ(backend.lastCommandCount, initialCommandCount);
}

TEST_F(RendererTest, EmptyFrameProducesNoRender) {
    renderer.BeginFrame(1920, 1080);
    renderer.EndFrame();  // No draw calls

    EXPECT_EQ(backend.renderCallCount, 0) << "Empty frame should not call Render";
}

TEST_F(RendererTest, RenderCachedFrameReturnsFalseWithoutDrawData) {
    EXPECT_FALSE(renderer.RenderCachedFrame(1920, 1080));
    EXPECT_EQ(backend.renderCallCount, 0);
}

TEST_F(RendererTest, DrawRectProducesVertices) {
    renderer.BeginFrame(1920, 1080);
    renderer.DrawRectFilled(0, 0, 100, 20, Colors::Background);
    renderer.EndFrame();

    EXPECT_GT(backend.lastVertexCount, 0);
}

TEST_F(RendererTest, DrawTextCalculatesNonZeroSize) {
    float w = 0, h = 0;
    renderer.CalcTextSize("Hello", &w, &h);
    EXPECT_GT(w, 0.0f);
    EXPECT_GT(h, 0.0f);
}

TEST_F(RendererTest, DrawTextScaledSizeProportional) {
    float w1 = 0, h1 = 0, w2 = 0, h2 = 0;
    renderer.CalcTextSizeScaled("A", &w1, &h1, 1.0f);
    renderer.CalcTextSizeScaled("A", &w2, &h2, 2.0f);
    EXPECT_NEAR(w2, w1 * 2.0f, 0.5f);
    EXPECT_NEAR(h2, h1 * 2.0f, 0.5f);
}

TEST_F(RendererTest, MultipleFramesAccumulate) {
    for (int i = 0; i < 5; i++) {
        renderer.BeginFrame(1920, 1080);
        renderer.DrawText(10, 10, "Test", Colors::White);
        renderer.EndFrame();
    }
    EXPECT_EQ(backend.renderCallCount, 5);
}

TEST_F(RendererTest, ShutdownCallsBackend) {
    renderer.Shutdown();
    EXPECT_TRUE(backend.shutdownCalled);
    EXPECT_FALSE(renderer.IsInitialized());
}
