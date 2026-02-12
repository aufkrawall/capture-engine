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

#include "custom_font.h"
#include <cstdint>
#include <vector>
#include <windows.h>

namespace CustomOverlay {

// Forward declarations
class RendererBackend;

// Text alignment
enum class TextAlign { Left, Center, Right };

// Overlay position (matches ImGui positions)
enum class Position { TopLeft, TopRight, BottomLeft, BottomRight };

// Draw vertex for rendering
struct DrawVertex {
  float x, y;     // Position
  float u, v;     // Texture coords
  uint32_t color; // ABGR color
};

// Draw command - batch of vertices to render
struct DrawCommand {
  uint32_t vertexOffset;
  uint32_t vertexCount;
  uint32_t indexOffset;
  uint32_t indexCount;
  bool useTexture; // True for text, false for solid shapes
};

// Overlay renderer - main API
class Renderer {
public:
  Renderer();
  ~Renderer();

  // Initialize with a graphics backend
  bool Initialize(RendererBackend *backend, float dpiScale = 1.0f);
  void Shutdown();
  bool IsInitialized() const { return initialized; }

  // Frame lifecycle
  void BeginFrame(int viewportWidth, int viewportHeight);
  void EndFrame();

  // Text rendering
  void DrawText(float x, float y, const char *text, uint32_t color);
  void DrawTextWithShadow(float x, float y, const char *text, uint32_t color,
                          uint32_t shadowColor, float shadowOffset = 1.0f);

  // Calculate text size
  void CalcTextSize(const char *text, float *outWidth, float *outHeight) const;

  // Basic shapes
  void DrawRect(float x, float y, float w, float h, uint32_t color);
  void DrawRectFilled(float x, float y, float w, float h, uint32_t color);
  void DrawLine(float x0, float y0, float x1, float y1, uint32_t color, float thickness = 1.0f);

  // Frame time graph
  void DrawFrameTimeGraph(float x, float y, float width, float height,
                          const float *frameTimes, int count,
                          float minVal, float maxVal, uint32_t color);

  // Debug
  void ValidateCommands();

  // Window-like containers (simplified)
  void BeginWindow(float x, float y, uint32_t bgColor, float alpha = 0.8f);
  void EndWindow();

  // Table layout (simplified 2-column layout)
  void BeginTable();
  void TableRow(const char *label, const char *value, uint32_t labelColor,
                uint32_t valueColor);
  void EndTable();

  // Font access
  FontAtlas *GetFont() { return &fontAtlas; }
  float GetDpiScale() const { return dpiScale; }
  int GetLineHeight() const { return fontAtlas.GetLineHeight(); }

  // Get draw data for backend to render
  const std::vector<DrawVertex> &GetVertices() const { return vertices; }
  const std::vector<uint16_t> &GetIndices() const { return indices; }
  const std::vector<DrawCommand> &GetCommands() const { return commands; }

private:
  void AddTextQuads(float x, float y, const char *text, uint32_t color);
  void AddQuad(float x, float y, float w, float h, float u0, float v0, float u1,
               float v1, uint32_t color);
  void FlushBatch(bool useTexture);

  FontAtlas fontAtlas;
  RendererBackend *backend = nullptr;

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

  // Track current batch starting offsets for correct command creation
  uint32_t currentBatchVertexOffset = 0;
  uint32_t currentBatchIndexOffset = 0;
};

// Abstract backend interface - implement for each graphics API
class RendererBackend {
public:
  virtual ~RendererBackend() = default;

  // Initialize backend resources (create shaders, buffers, etc.)
  virtual bool Initialize(int fontTextureWidth, int fontTextureHeight,
                          const uint8_t *fontTextureData) = 0;
  virtual void Shutdown() = 0;

  // Render the accumulated draw commands
  virtual void Render(const std::vector<DrawVertex> &vertices,
                      const std::vector<uint16_t> &indices,
                      const std::vector<DrawCommand> &commands,
                      int viewportWidth, int viewportHeight) = 0;
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
constexpr uint32_t Background = 0xE0282828; // Dark semi-transparent
constexpr uint32_t FPS = 0xFF00FF00;        // Bright green
constexpr uint32_t CPU = 0xFF00BFFF;        // DeepSkyBlue
constexpr uint32_t GPU = 0xFF32CD32;        // LimeGreen
constexpr uint32_t RAM = 0xFFB8860B;        // DarkGoldenrod
constexpr uint32_t VRAM = 0xFFFF8C00;       // DarkOrange
constexpr uint32_t LoadLow = 0xFF00FF00;    // Green
constexpr uint32_t LoadMed = 0xFF00FFFF;    // Yellow
constexpr uint32_t LoadHigh = 0xFF0000FF;   // Red
} // namespace Colors

} // namespace CustomOverlay
