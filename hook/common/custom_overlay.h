/**
 * Custom Overlay Renderer
 *
 * Lightweight overlay rendering system to replace ImGui.
 * Provides a simple API for drawing text and basic shapes.
 *
 * Each graphics API (DX11, DX12, Vulkan, etc.) implements a backend
 * that converts draw commands to native rendering calls.
 */

#pragma once

#include <windows.h>
#include <cstdint>
#include <vector>
#include "custom_font.h"

namespace CustomOverlay {

// Forward declarations
class RendererBackend;

// Text alignment
enum class TextAlign { Left, Center, Right };

// Overlay position (matches ImGui positions)
enum class Position { TopLeft, TopRight, BottomLeft, BottomRight };

// Draw vertex for rendering
struct DrawVertex {
    float x, y;      // Position
    float u, v;      // Texture coords
    uint32_t color;  // ABGR color
};

// Draw command - batch of vertices to render
struct DrawCommand {
    uint32_t vertexOffset;
    uint32_t vertexCount;
    uint32_t indexOffset;
    uint32_t indexCount;
    bool useTexture;  // True for text, false for solid shapes
};

// Overlay renderer - main API
class Renderer {
public:
    Renderer();
    ~Renderer();

    // Initialize with a graphics backend
    bool Initialize(RendererBackend* backend, float dpiScale = 1.0f);
    void Shutdown();
    void SetSkipDeviceRelease(bool skip) {
        skipDeviceRelease = skip;
    }
    bool IsInitialized() const {
        return initialized;
    }

    // Frame lifecycle
    void BeginFrame(int viewportWidth, int viewportHeight);
    void EndFrame();
    bool RenderCachedFrame(int viewportWidth, int viewportHeight);

    // Text rendering
    void DrawText(float x, float y, const char* text, uint32_t color);
    void DrawTextWithShadow(float x, float y, const char* text, uint32_t color, uint32_t shadowColor,
                            float shadowOffset = 1.0f);
    void DrawTextScaled(float x, float y, const char* text, uint32_t color, float scale);
    void DrawTextScaledWithShadow(float x, float y, const char* text, uint32_t color, uint32_t shadowColor, float scale,
                                  float shadowOffset = 1.0f);

    // Right-aligned text (prevents flicker when digit count changes)
    void DrawTextRightAligned(float rightX, float y, const char* text, uint32_t color, uint32_t shadowColor,
                              float shadowOffset = 1.0f);
    void DrawTextScaledRightAligned(float rightX, float y, const char* text, uint32_t color, uint32_t shadowColor,
                                    float scale, float shadowOffset = 1.0f);

    // Calculate text size
    void CalcTextSize(const char* text, float* outWidth, float* outHeight) const;
    void CalcTextSizeScaled(const char* text, float* outWidth, float* outHeight, float scale) const;

    // Basic shapes
    void DrawRect(float x, float y, float w, float h, uint32_t color);
    void DrawRectFilled(float x, float y, float w, float h, uint32_t color);
    void DrawLine(float x0, float y0, float x1, float y1, uint32_t color, float thickness = 1.0f);

    // Frame time graph
    void DrawFrameTimeGraph(float x, float y, float width, float height, const float* frameTimes, int count,
                            float minVal, float maxVal, uint32_t color);

    // Debug
    void ValidateCommands();

    // Window-like containers (simplified)
    void BeginWindow(float x, float y, uint32_t bgColor, float alpha = 0.8f);
    void EndWindow();

    // Table layout (simplified 2-column layout)
    void BeginTable();
    void TableRow(const char* label, const char* value, uint32_t labelColor, uint32_t valueColor);
    void EndTable();

    // Font access
    FontAtlas* GetFont() {
        return &fontAtlas;
    }
    float GetDpiScale() const {
        return dpiScale;
    }
    int GetLineHeight() const {
        return fontAtlas.GetLineHeight();
    }

    // Get draw data for backend to render
    const std::vector<DrawVertex>& GetVertices() const {
        return vertices;
    }
    const std::vector<uint16_t>& GetIndices() const {
        return indices;
    }
    const std::vector<DrawCommand>& GetCommands() const {
        return commands;
    }

private:
    void AddTextQuads(float x, float y, const char* text, uint32_t color);
    void AddTextQuadsScaled(float x, float y, const char* text, uint32_t color, float scale);
    void AddTextSolidQuads(float x, float y, const char* text, uint32_t color);
    void AddTextSolidQuadsScaled(float x, float y, const char* text, uint32_t color, float scale);
    void AddQuad(float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t color);
    void FlushBatch(bool useTexture);
    // Renders a connected polyline with miter joins and AA fringe
    void DrawGraphPolyline(const float* xs, const float* ys, int count, uint32_t color, float thickness);

    FontAtlas fontAtlas;
    RendererBackend* backend = nullptr;

    std::vector<DrawVertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<DrawCommand> commands;

    int viewportWidth = 0;
    int viewportHeight = 0;
    float dpiScale = 1.0f;

    // Current drawing state
    float cursorX = 0;
    float cursorY = 0;
    bool inWindow = false;
    bool inTable = false;
    float windowX = 0, windowY = 0;
    float tableStartY = 0;

    bool initialized = false;
    bool frameStarted = false;
    bool skipDeviceRelease = false;

    // Track current batch starting offsets for correct command creation
    uint32_t currentBatchVertexOffset = 0;
    uint32_t currentBatchIndexOffset = 0;
};

// Abstract backend interface - implement for each graphics API
class RendererBackend {
public:
    virtual ~RendererBackend() = default;

    // Initialize backend resources (create shaders, buffers, etc.)
    virtual bool Initialize(int fontTextureWidth, int fontTextureHeight, const uint8_t* fontTextureData) = 0;
    virtual void Shutdown() = 0;

    // Set HDR rendering parameters (call before Render)
    // hdrMode: 0=SDR, 1=scRGB/FP16, 2=HDR10/PQ
    virtual void SetHDRParams(int hdrMode, float paperWhiteNits) {
        this->hdrMode = hdrMode;
        this->paperWhiteNits = paperWhiteNits;
    }

    // Render the accumulated draw commands
    virtual void OnDrawDataChanged() {}
    virtual void Render(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                        const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) = 0;

    // Some backends prefer glyph coverage as solid geometry instead of font SRV sampling.
    virtual bool PreferSolidTextGeometry() const {
        return false;
    }

protected:
    int hdrMode = 0;
    float paperWhiteNits = 200.0f;
};

// Predefined colors (matches common overlay colors)
namespace Colors {
constexpr uint32_t White = 0xFFFFFFFF;
constexpr uint32_t Black = 0xFF000000;
constexpr uint32_t Red = 0xFF0000FF;
constexpr uint32_t Green = 0xFF00FF00;
constexpr uint32_t Blue = 0xFFFF0000;
constexpr uint32_t Yellow = 0xFF00FFFF;
constexpr uint32_t Cyan = 0xFFFFFF00;
constexpr uint32_t Magenta = 0xFFFF00FF;
constexpr uint32_t Orange = 0xFF00A5FF;
constexpr uint32_t Gray = 0xFF808080;
constexpr uint32_t DarkGray = 0xFF404040;
constexpr uint32_t Transparent = 0x00000000;

// Semantic colors for overlay
constexpr uint32_t Background = 0xE0282828;  // Dark semi-transparent
constexpr uint32_t FPS = 0xFF00FF00;         // Bright green
constexpr uint32_t CPU = 0xFF00BFFF;         // DeepSkyBlue
constexpr uint32_t GPU = 0xFF32CD32;         // LimeGreen
constexpr uint32_t RAM = 0xFFB8860B;         // DarkGoldenrod
constexpr uint32_t VRAM = 0xFFFF8C00;        // DarkOrange
constexpr uint32_t LoadLow = 0xFF00FF00;     // Green
constexpr uint32_t LoadMed = 0xFF00FFFF;     // Yellow
constexpr uint32_t LoadHigh = 0xFF0000FF;    // Red

// Label colors (matching reference overlay style)
constexpr uint32_t LabelGreen = 0xFF00C850;   // Green for GPU/CPU names
constexpr uint32_t LabelOrange = 0xFF0080FF;  // Orange for VRAM
constexpr uint32_t LabelPink = 0xFF8080FF;    // Pink/Red for RAM
constexpr uint32_t LabelCyan = 0xFFFFFF00;    // Cyan for FG
constexpr uint32_t LabelYellow = 0xFF00FFFF;  // Yellow for FPS labels
constexpr uint32_t ValueYellow = 0xFF00FFFF;  // Yellow for FPS values
constexpr uint32_t ValueCyan = 0xFFFFFF00;    // Cyan for CPU %
}  // namespace Colors

}  // namespace CustomOverlay
