/**
 * Overlay system unit tests.
 *
 * FontAtlas tests run entirely on CPU/GDI — no graphics API context required.
 * Renderer tests use a mock backend to verify geometry/command building.
 */

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "../hook/common/custom_font.h"
#include "../hook/common/custom_overlay.h"
#include "../hook/common/legacy_overlay_cache.h"
#include "../hook/common/overlay_layout_policy.h"
#include "source_fragment_reader.h"

using namespace CustomOverlay;

namespace {

std::string ReadOverlaySource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

size_t CountOccurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t pos = 0; (pos = text.find(needle, pos)) != std::string::npos; pos += needle.size())
        ++count;
    return count;
}

}  // namespace

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

TEST_F(FontAtlasTest, GlyphSpansReconstructQuantizedCoverageForPrintableAscii) {
    const uint8_t* textureData = atlas.GetTextureData();
    ASSERT_NE(textureData, nullptr);
    const int textureWidth = atlas.GetTextureWidth();
    const int textureHeight = atlas.GetTextureHeight();

    for (int c = 32; c <= 126; ++c) {
        SCOPED_TRACE(::testing::Message() << "ASCII " << c);
        const Glyph* glyph = atlas.GetGlyph((char)c);
        ASSERT_NE(glyph, nullptr);
        ASSERT_LE((int)glyph->x + (int)glyph->width, textureWidth);
        ASSERT_LE((int)glyph->y + (int)glyph->height, textureHeight);

        std::vector<uint8_t> reconstructed((size_t)glyph->width * (size_t)glyph->height, 0);
        const auto& spans = atlas.GetGlyphSpans((char)c);
        for (const auto& span : spans) {
            ASSERT_LT(span.x, glyph->width);
            ASSERT_LT(span.y, glyph->height);
            ASSERT_LE((uint32_t)span.x + (uint32_t)span.width, (uint32_t)glyph->width);
            ASSERT_GT(span.width, 0u);
            EXPECT_TRUE(span.alpha == 255 || (span.alpha >= 16 && span.alpha <= 240 && span.alpha % 16 == 0));

            for (uint16_t x = 0; x < span.width; ++x) {
                uint8_t& coverage = reconstructed[(size_t)span.y * glyph->width + span.x + x];
                EXPECT_EQ((unsigned)coverage, 0u) << "glyph spans overlap";
                coverage = span.alpha;
            }
        }

        for (uint16_t y = 0; y < glyph->height; ++y) {
            for (uint16_t x = 0; x < glyph->width; ++x) {
                const size_t atlasIndex =
                    ((size_t)(glyph->y + y) * (size_t)textureWidth + (size_t)(glyph->x + x)) * 4 + 3;
                const uint8_t sourceAlpha = textureData[atlasIndex];
                const int roundedAlpha = sourceAlpha == 0 ? 0 : (((int)sourceAlpha + 15) & ~15);
                const uint8_t expectedAlpha = (uint8_t)((std::min)(255, roundedAlpha));
                EXPECT_EQ((unsigned)reconstructed[(size_t)y * glyph->width + x], (unsigned)expectedAlpha);
            }
        }
    }
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
    const char highBit = static_cast<char>(0xFF);
    EXPECT_EQ(atlas.GetGlyph(highBit), question);
    EXPECT_EQ(&atlas.GetGlyphSpans(highBit), &atlas.GetGlyphSpans('?'));
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

TEST(FontAtlasPaddingTest, PrintableGlyphsHaveTwoTransparentTexelsAroundEveryCellAtSupportedDpiScales) {
    for (float dpiScale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        FontAtlas scaledAtlas;
        ASSERT_TRUE(scaledAtlas.Initialize("Segoe UI", 14, dpiScale));
        const uint8_t* pixels = scaledAtlas.GetTextureData();
        ASSERT_NE(pixels, nullptr);
        const int width = scaledAtlas.GetTextureWidth();

        auto alphaAt = [&](int x, int y) -> uint8_t {
            return pixels[((size_t)y * (size_t)width + (size_t)x) * 4u + 3u];
        };

        for (int c = 32; c <= 126; ++c) {
            SCOPED_TRACE(::testing::Message() << "dpi=" << dpiScale << " ascii=" << c);
            const Glyph* glyph = scaledAtlas.GetGlyph((char)c);
            ASSERT_NE(glyph, nullptr);
            ASSERT_GE(glyph->x, 2u);
            ASSERT_GE(glyph->y, 2u);
            ASSERT_LE((int)glyph->x + (int)glyph->width + 2, scaledAtlas.GetTextureWidth());
            ASSERT_LE((int)glyph->y + (int)glyph->height + 2, scaledAtlas.GetTextureHeight());

            for (int y = glyph->y; y < glyph->y + glyph->height; ++y) {
                EXPECT_EQ(alphaAt(glyph->x - 1, y), 0u);
                EXPECT_EQ(alphaAt(glyph->x - 2, y), 0u);
                EXPECT_EQ(alphaAt(glyph->x + glyph->width, y), 0u);
                EXPECT_EQ(alphaAt(glyph->x + glyph->width + 1, y), 0u);
            }
            for (int x = glyph->x; x < glyph->x + glyph->width; ++x) {
                EXPECT_EQ(alphaAt(x, glyph->y - 1), 0u);
                EXPECT_EQ(alphaAt(x, glyph->y - 2), 0u);
                EXPECT_EQ(alphaAt(x, glyph->y + glyph->height), 0u);
                EXPECT_EQ(alphaAt(x, glyph->y + glyph->height + 1), 0u);
            }
        }
    }
}

// ============================================================================
// Renderer tests with mock backend
// ============================================================================

class MockBackend : public RendererBackend {
public:
    bool initCalled = false;
    bool shutdownCalled = false;
    int renderCallCount = 0;
    int drawDataChangedCount = 0;
    int lastVertexCount = 0;
    int lastCommandCount = 0;

    bool Initialize(int, int, const uint8_t*) override {
        initCalled = true;
        return true;
    }

    void Shutdown() override {
        shutdownCalled = true;
    }

    void OnDrawDataChanged() override {
        drawDataChangedCount++;
    }

    void Render(const std::vector<DrawVertex>& verts, const std::vector<uint16_t>&,
                const std::vector<DrawCommand>& cmds, int, int) override {
        renderCallCount++;
        lastVertexCount = (int)verts.size();
        lastCommandCount = (int)cmds.size();
    }
};

class SolidTextMockBackend : public MockBackend {
public:
    bool PreferSolidTextGeometry() const override {
        return true;
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
    EXPECT_EQ(backend.drawDataChangedCount, 1) << "cached draws must not dirty legacy uploads";
}

TEST_F(RendererTest, NewFramesNotifyBackendExactlyOnceAndCachedFramesDoNot) {
    renderer.BeginFrame(1920, 1080);
    renderer.DrawRectFilled(5, 5, 20, 10, Colors::White);
    renderer.EndFrame();
    EXPECT_EQ(backend.drawDataChangedCount, 1);

    ASSERT_TRUE(renderer.RenderCachedFrame(1920, 1080));
    EXPECT_EQ(backend.drawDataChangedCount, 1);

    renderer.BeginFrame(1920, 1080);
    renderer.DrawRectFilled(5, 5, 21, 10, Colors::White);
    renderer.EndFrame();
    EXPECT_EQ(backend.drawDataChangedCount, 2);
}

TEST_F(RendererTest, EmptyFrameProducesNoRender) {
    renderer.BeginFrame(1920, 1080);
    renderer.EndFrame();  // No draw calls

    EXPECT_EQ(backend.renderCallCount, 0) << "Empty frame should not call Render";
    EXPECT_EQ(backend.drawDataChangedCount, 1);
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

TEST(RendererSolidTextTest, PreferredSolidTextEmitsOnlySolidCommands) {
    SolidTextMockBackend backend;
    Renderer renderer;
    ASSERT_TRUE(renderer.Initialize(&backend, 1.0f));

    renderer.BeginFrame(1920, 1080);
    renderer.DrawTextWithShadow(10, 10, "FPS: 60", Colors::White, Colors::Black);
    renderer.EndFrame();

    ASSERT_GT(backend.lastVertexCount, 0);
    ASSERT_FALSE(renderer.GetCommands().empty());
    for (const auto& cmd : renderer.GetCommands()) {
        EXPECT_FALSE(cmd.useTexture);
    }
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

TEST(RendererGeometryTest, FrameGraphUsesAllSamplesWithExactMonotonicEndpointInterpolation) {
    MockBackend backend;
    Renderer renderer;
    ASSERT_TRUE(renderer.Initialize(&backend, 1.5f));

    float samples[180];
    for (int i = 0; i < 180; ++i)
        samples[i] = 5.0f + (float)i * 0.05f;

    renderer.BeginFrame(3840, 2160);
    renderer.DrawFrameTimeGraph(10.0f, 20.0f, 330.0f, 50.0f, samples, 180, 0.0f, 33.0f, Colors::Green);
    renderer.EndFrame();

    const auto& vertices = renderer.GetVertices();
    ASSERT_EQ(vertices.size(), 180u * 4u);
    float previousX = -1.0f;
    for (int i = 0; i < 180; ++i) {
        const float centerX = (vertices[(size_t)i * 4u + 1u].x + vertices[(size_t)i * 4u + 2u].x) * 0.5f;
        if (i > 0)
            EXPECT_GT(centerX, previousX);
        previousX = centerX;
    }
    const float firstX = (vertices[1].x + vertices[2].x) * 0.5f;
    const float lastX = (vertices[717].x + vertices[718].x) * 0.5f;
    EXPECT_NEAR(firstX, 12.0f, 0.001f);
    EXPECT_NEAR(lastX, 338.0f, 0.001f);
}

TEST(RendererGeometryTest, SharpAlternatingGraphUsesBoundedBevelGeometry) {
    MockBackend backend;
    Renderer renderer;
    ASSERT_TRUE(renderer.Initialize(&backend, 2.0f));

    float samples[180];
    for (int i = 0; i < 180; ++i)
        samples[i] = (i & 1) ? 32.0f : 1.0f;

    renderer.BeginFrame(3840, 2160);
    renderer.DrawFrameTimeGraph(10.0f, 20.0f, 330.0f, 50.0f, samples, 180, 0.0f, 33.0f, Colors::Green);
    renderer.EndFrame();

    ASSERT_GT(renderer.GetVertices().size(), 180u * 4u) << "sharp joins should expand to bevel cross-sections";
    for (const DrawVertex& vertex : renderer.GetVertices()) {
        EXPECT_TRUE(std::isfinite(vertex.x));
        EXPECT_TRUE(std::isfinite(vertex.y));
        EXPECT_GE(vertex.x, 8.0f);
        EXPECT_LE(vertex.x, 342.0f);
        EXPECT_GE(vertex.y, 18.0f);
        EXPECT_LE(vertex.y, 72.0f);
    }
    ASSERT_EQ(renderer.GetCommands().size(), 1u) << "bevels must not add draw calls";
}

TEST(RendererGeometryTest, FractionalDpiShadowUsesOneSnappedOrigin) {
    MockBackend backend;
    Renderer renderer;
    ASSERT_TRUE(renderer.Initialize(&backend, 1.5f));

    renderer.BeginFrame(1920, 1080);
    renderer.DrawTextWithShadow(10.6f, 20.6f, "A", Colors::White, Colors::Black);
    renderer.EndFrame();

    const auto& vertices = renderer.GetVertices();
    ASSERT_GE(vertices.size(), 8u);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(vertices[i].x - vertices[i + 4].x, 2.0f);
        EXPECT_FLOAT_EQ(vertices[i].y - vertices[i + 4].y, 2.0f);
    }
}

TEST(LegacyOverlayCacheTest, FailedUploadsStayDirtyAndSuccessfulUploadsCacheGeometry) {
    LegacyGeometryUploadState state;
    EXPECT_TRUE(state.NeedsUpload());
    state.MarkUploadFailed();
    EXPECT_TRUE(state.NeedsUpload());
    state.MarkUploadSucceeded();
    EXPECT_FALSE(state.NeedsUpload());
    state.MarkDrawDataChanged();
    EXPECT_TRUE(state.NeedsUpload());
    state.MarkUploadSucceeded();
    state.MarkBufferRecreated();
    EXPECT_TRUE(state.NeedsUpload());
}

TEST(LegacyOverlayCacheTest, DX10ConstantsOnlyChangeForViewportHdrOrPaperWhite) {
    DX10ConstantBufferState state;
    EXPECT_TRUE(state.NeedsUpdate(1920, 1080, 0, 200.0f));
    state.MarkUpdated(1920, 1080, 0, 200.0f);
    EXPECT_FALSE(state.NeedsUpdate(1920, 1080, 0, 200.0f));
    EXPECT_TRUE(state.NeedsUpdate(2560, 1440, 0, 200.0f));
    EXPECT_TRUE(state.NeedsUpdate(1920, 1080, 1, 200.0f));
    EXPECT_TRUE(state.NeedsUpdate(1920, 1080, 0, 203.0f));
    state.Invalidate();
    EXPECT_TRUE(state.NeedsUpdate(1920, 1080, 0, 200.0f));
}

TEST(LegacyOverlayCacheTest, OpenGL21PrefersArraysAndRetainsImmediateFallback) {
    EXPECT_EQ(SelectLegacyGLDrawPath(true, true, true), LegacyGLDrawPath::Arrays);
    EXPECT_EQ(SelectLegacyGLDrawPath(false, true, true), LegacyGLDrawPath::Immediate);
    EXPECT_EQ(SelectLegacyGLDrawPath(true, false, true), LegacyGLDrawPath::Immediate);
    EXPECT_EQ(SelectLegacyGLDrawPath(true, true, false), LegacyGLDrawPath::Immediate);
}

TEST(OverlayHdrSourceTest, DirectXAndVulkanApplyTheSameRec709ToRec2020Transform) {
    const std::string hlsl = ReadOverlaySource("tools/compile_shaders.py");
    const std::string textured = ReadOverlaySource("hook/vulkan_layer/shaders/overlay_textured.frag");
    const std::string solid = ReadOverlaySource("hook/vulkan_layer/shaders/overlay_solid.frag");
    const std::string matrixRow = "0.6274038959, 0.3292830384, 0.0433130657";
    for (const auto* source : {&hlsl, &textured, &solid}) {
        ASSERT_FALSE(source->empty());
        EXPECT_NE(source->find(matrixRow), std::string::npos);
        EXPECT_NE(source->find("Rec2020"), std::string::npos);
        EXPECT_NE(source->find("10000.0"), std::string::npos);
    }
    EXPECT_EQ(CountOccurrences(hlsl, "Rec709ToRec2020(lin) * paperWhiteNits"), 3u);
    EXPECT_NE(textured.find("rec709ToRec2020(lin) * pc.paperWhiteNits"), std::string::npos);
    EXPECT_NE(solid.find("rec709ToRec2020(lin) * pc.paperWhiteNits"), std::string::npos);
}

TEST(OverlayHdrSourceTest, AutoPaperWhiteUsesWindowsPerMonitorCalibration) {
    const std::string source = ReadOverlaySource("hook/common/overlay_adapter.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL"), std::string::npos);
    EXPECT_NE(source.find("80.0f / 1000.0f"), std::string::npos);
    EXPECT_NE(source.find("resolvedHdrPaperWhiteNits = 203.0f"), std::string::npos);
    EXPECT_NE(source.find("MonitorFromWindow(referenceHwnd, MONITOR_DEFAULTTONEAREST)"), std::string::npos);
}

TEST(OverlayDpiSourceTest, InjectOverlayScaleUsesNearestMonitorEffectiveDpi) {
    // RoboCop 20260821_224340 initialized on a 150% display while its DPI-virtualized
    // window reported 96 DPI. The first swapchain was 2560x1440 and later resized to
    // 3840x2160; warm backend reuse preserved the already-wrong font atlas. The inject
    // overlay must therefore resolve display DPI at initialization exactly like the
    // pseudo-overlay instead of consulting the game window's awareness-dependent value.
    const std::string adapter = ReadOverlaySource("hook/common/overlay_adapter.cpp");
    const std::string internalHeader = ReadOverlaySource("hook/common/overlay_adapter_internal.h");
    ASSERT_FALSE(adapter.empty());
    ASSERT_FALSE(internalHeader.empty());

    EXPECT_NE(adapter.find("GetDpiForMonitor"), std::string::npos);
    EXPECT_NE(adapter.find("MDT_EFFECTIVE_DPI"), std::string::npos);
    EXPECT_NE(adapter.find("ResolveOverlayDpi"), std::string::npos);
    EXPECT_EQ(adapter.find("GetDpiForWindow"), std::string::npos);
    EXPECT_NE(internalHeader.find("pseudo_overlay_dpi_policy.h"), std::string::npos);
}

TEST(LegacyOverlayBackendSourceTest, DX8AndDX9ReuseStateBlocksButCaptureAndApplyEveryDraw) {
    const std::string dx8 = ReadOverlaySource("hook/common/custom_overlay_dx8.cpp");
    const std::string dx9 = ReadOverlaySource("hook/common/custom_overlay_dx9.cpp");
    ASSERT_FALSE(dx8.empty());
    ASSERT_FALSE(dx9.empty());

    EXPECT_EQ(CountOccurrences(dx8, "CreateStateBlock(D3DSBT_ALL"), 1u);
    EXPECT_EQ(CountOccurrences(dx8, "CaptureStateBlock(stateBlock)"), 1u);
    EXPECT_EQ(CountOccurrences(dx8, "ApplyStateBlock(stateBlock)"), 1u);
    EXPECT_NE(dx8.find("if (stateBlock == 0)"), std::string::npos);

    EXPECT_EQ(CountOccurrences(dx9, "CreateStateBlock(D3DSBT_ALL"), 1u);
    EXPECT_EQ(CountOccurrences(dx9, "stateBlock->Capture()"), 1u);
    EXPECT_EQ(CountOccurrences(dx9, "stateBlock->Apply()"), 1u);
    EXPECT_NE(dx9.find("if (!stateBlock)"), std::string::npos);
}

TEST(LegacyOverlayBackendSourceTest, FailedUploadsReturnBeforeLegacyDrawSubmission) {
    for (const char* path : {"hook/common/custom_overlay_dx8.cpp", "hook/common/custom_overlay_dx9.cpp",
                             "hook/common/custom_overlay_dx10.cpp"}) {
        const std::string source = ReadOverlaySource(path);
        SCOPED_TRACE(path);
        ASSERT_FALSE(source.empty());
        const size_t failedUpload = source.find("geometryUpload.MarkUploadFailed();");
        const size_t successfulUpload = source.find("geometryUpload.MarkUploadSucceeded();");
        const size_t drawSubmission = source.find("DrawIndexed");
        ASSERT_NE(failedUpload, std::string::npos);
        ASSERT_NE(successfulUpload, std::string::npos);
        ASSERT_NE(drawSubmission, std::string::npos);
        EXPECT_LT(failedUpload, drawSubmission);
        EXPECT_LT(successfulUpload, drawSubmission);
        EXPECT_NE(source.find("geometryUpload.NeedsUpload()"), std::string::npos);
    }
}

TEST(LegacyOverlayBackendSourceTest, OpenGLLegacyPathPreservesSentinelsWithoutPerFrameErrorDrain) {
    const std::string source = ReadOverlaySource("hook/common/custom_overlay_gl.cpp");
    const std::string hookSource = ReadOverlaySource("hook/apis/opengl_hook.cpp");
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(hookSource.empty());

    const size_t renderStart = source.find("void OpenGLBackend::Render(");
    const size_t modernStart = source.find("void OpenGLBackend::RenderModern(", renderStart);
    ASSERT_NE(renderStart, std::string::npos);
    ASSERT_NE(modernStart, std::string::npos);
    EXPECT_EQ(source.substr(renderStart, modernStart - renderStart).find("ClearGLErrors();"), std::string::npos);

    const size_t saveVbo = source.find("pglGetIntegerv(GL_ARRAY_BUFFER_BINDING, &lastVBO)");
    const size_t zeroVbo = source.find("pglBindBuffer(GL_ARRAY_BUFFER, 0)", saveVbo);
    const size_t restoreVao = source.find("pglBindVertexArray((GLuint)lastVAO)", zeroVbo);
    const size_t restoreArrays = source.find("if (lastVertexArray)", restoreVao);
    ASSERT_NE(saveVbo, std::string::npos);
    ASSERT_NE(zeroVbo, std::string::npos);
    ASSERT_NE(restoreVao, std::string::npos);
    ASSERT_NE(restoreArrays, std::string::npos);
    EXPECT_LT(saveVbo, zeroVbo);
    EXPECT_LT(restoreVao, restoreArrays);

    EXPECT_NE(source.find("pglGetIntegerv(GL_ACTIVE_TEXTURE, &lastActiveTexture)"), std::string::npos);
    EXPECT_NE(source.find("pglGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &lastClientActiveTexture)"), std::string::npos);
    EXPECT_NE(source.find("pglBlendFuncSeparate((GLenum)lastBlendSrcRGB"), std::string::npos);
    EXPECT_NE(source.find("pglMatrixMode((GLenum)lastMatrixMode)"), std::string::npos);

    EXPECT_EQ(hookSource.find("DetourSwapBuffers(0x%p) entering"), std::string::npos);
    EXPECT_EQ(hookSource.find("DetourWglSwapBuffers(0x%p) entering"), std::string::npos);
    EXPECT_EQ(hookSource.find("DetourWglSwapLayerBuffers(0x%p) entering"), std::string::npos);
    EXPECT_EQ(hookSource.find("OpenGL: wglMakeCurrent(HDC=0x%p"), std::string::npos);
    EXPECT_NE(hookSource.find("perfMetrics.overlayUs = opengl_hook_g_LastOverlayUs;"), std::string::npos);
}

TEST(OverlayLayoutPolicyTest, FrameGenerationRowsAppearAndDisappearAtomicallyAcrossTransitions) {
    ce::overlay_layout::RowInputs input = {};
    input.showGPU = input.showCPU = input.showVRAM = input.showRAM = true;
    input.showFPS = input.showFPSAverages = input.showFG = true;

    const uint32_t offMask = ce::overlay_layout::BuildOverlayRowMask(input);
    EXPECT_EQ(ce::overlay_layout::CountOverlayRows(offMask), 6u);
    EXPECT_EQ(offMask & (ce::overlay_layout::kRowFGRates | ce::overlay_layout::kRowFGStatus), 0u);

    input.fgActive = true;  // OFF -> DLSS 2x; type/multiplier do not alter row presence.
    const uint32_t dlss2Mask = ce::overlay_layout::BuildOverlayRowMask(input);
    EXPECT_EQ(ce::overlay_layout::CountOverlayRows(dlss2Mask), 8u);
    EXPECT_NE(dlss2Mask & ce::overlay_layout::kRowFGRates, 0u);
    EXPECT_NE(dlss2Mask & ce::overlay_layout::kRowFGStatus, 0u);
    EXPECT_EQ(ce::overlay_layout::BuildOverlayRowMask(input), dlss2Mask);  // DLSS 4x / FSR 2x

    input.fgActive = false;
    EXPECT_EQ(ce::overlay_layout::BuildOverlayRowMask(input), offMask);
    input.reserveFGSpace = true;
    EXPECT_EQ(ce::overlay_layout::BuildOverlayRowMask(input), dlss2Mask);

    input.recordingActive = true;
    input.showRecording = true;
    input.notificationVisible = true;
    const uint32_t expandedMask = ce::overlay_layout::BuildOverlayRowMask(input);
    EXPECT_EQ(ce::overlay_layout::CountOverlayRows(expandedMask), 10u);

    input.recordingActive = false;
    input.recordingStarting = true;
    EXPECT_EQ(ce::overlay_layout::BuildOverlayRowMask(input), expandedMask);

    input.showRecording = false;
    EXPECT_EQ(ce::overlay_layout::BuildOverlayRowMask(input) & ce::overlay_layout::kRowRecording, 0u);
}

TEST(OverlayLayoutPolicyTest, MemoryValuesNeverRequireFabricatedCapacity) {
    using ce::overlay_layout::MemoryValueMode;
    using ce::overlay_layout::SelectMemoryValueMode;

    EXPECT_EQ(SelectMemoryValueMode(false, 0, 0), MemoryValueMode::Unavailable);
    EXPECT_EQ(SelectMemoryValueMode(true, 0, 0), MemoryValueMode::UsedOnly);
    EXPECT_EQ(SelectMemoryValueMode(false, 8ull << 30, 0), MemoryValueMode::UsedOnly);
    EXPECT_EQ(SelectMemoryValueMode(true, 8ull << 30, 32ull << 30), MemoryValueMode::UsedAndTotal);
}
