#pragma once

#include "custom_overlay.h"
#include "overlay_panel.h"

// Puts a panel scene on the renderer: the only place tones and fills become colors.
namespace ce::overlay_panel {

class RendererMeasure final : public TextMeasure {
public:
    explicit RendererMeasure(const CustomOverlay::Renderer& renderer) : renderer_(renderer) {}
    float Width(const char* text, Size size) const override;
    FontMetrics Font(Size size) const override;

private:
    const CustomOverlay::Renderer& renderer_;
};

struct SceneStyle {
    uint32_t background = 0;
    uint32_t value = 0;
    uint32_t fps = 0;
    uint32_t shadow = 0;
    int shadowPixels = 0;
};

// One physical pixel up to 150%: with the panel behind the text, a heavier shadow only thickens the glyphs.
int ShadowPixelsForScale(float dpiScale);

// The background first, then the rectangles and the text, so a backend that cannot merge shapes with text
// spends one draw call on each. The caller draws the graph last.
void DrawScene(CustomOverlay::Renderer& renderer, const Scene& scene, float x, float y, const SceneStyle& style);

}  // namespace ce::overlay_panel
