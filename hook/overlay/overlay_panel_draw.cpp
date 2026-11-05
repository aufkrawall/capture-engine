#include "overlay_panel_draw.h"

#include <algorithm>
#include <cmath>

namespace ce::overlay_panel {

namespace {

using CustomOverlay::ColorRGBA;
using CustomOverlay::TextSize;

TextSize ToTextSize(Size size) {
    switch (size) {
        case Size::Small:
            return TextSize::Small;
        case Size::Hero:
            return TextSize::Hero;
        case Size::Base:
            break;
    }
    return TextSize::Base;
}

// The colors were chosen for contrast on the darkest and brightest scenes under the default panel: the
// labels keep 5:1 and the warning colors at least 3:1 over a white scene at 72%. Pure red would not.
uint32_t ToneColor(Tone tone, const SceneStyle& style) {
    switch (tone) {
        case Tone::Value:
            return style.value;
        case Tone::Label:
            return ColorRGBA(0xB9, 0xBE, 0xC6);
        case Tone::Unit:
            return ColorRGBA(0xA6, 0xAC, 0xB5);
        case Tone::Fps:
            return style.fps;
        case Tone::FgStatus:
            return ColorRGBA(0x5F, 0xE6, 0xFF);
        case Tone::Good:
            return ColorRGBA(0x7C, 0xE3, 0x8B);
        case Tone::Warn:
            return ColorRGBA(0xFF, 0xC1, 0x42);
        case Tone::Crit:
            return ColorRGBA(0xFF, 0x5F, 0x52);
        case Tone::OnChip:
            return ColorRGBA(0xFF, 0xFF, 0xFF);
    }
    return style.value;
}

uint32_t FillColor(Fill fill, const SceneStyle& style) {
    switch (fill) {
        case Fill::Chip:
            return ColorRGBA(0xC9, 0x26, 0x1B);
        case Fill::BarTrack:
            return ColorRGBA(0xFF, 0xFF, 0xFF, 0x33);
        case Fill::BarValue:
            return style.value;
        case Fill::BarWarn:
            return ToneColor(Tone::Warn, style);
        case Fill::BarCrit:
            return ToneColor(Tone::Crit, style);
        case Fill::BarMark:
            return style.fps;
        case Fill::Rule:
            return ColorRGBA(0xFF, 0xFF, 0xFF, 0x38);
    }
    return style.value;
}

}  // namespace

float RendererMeasure::Width(const char* text, Size size) const {
    return renderer_.TextWidth(text, ToTextSize(size));
}

FontMetrics RendererMeasure::Font(Size size) const {
    const CustomOverlay::TextSizeMetrics& metrics = renderer_.GetTextMetrics(ToTextSize(size));
    FontMetrics font;
    font.pixelSize = static_cast<float>(metrics.pixelSize);
    font.ascent = static_cast<float>(metrics.ascent);
    font.digitHeight = static_cast<float>(metrics.digitHeight);
    return font;
}

int ShadowPixelsForScale(float dpiScale) {
    return (std::max)(1, static_cast<int>(std::floor(dpiScale)));
}

void DrawScene(CustomOverlay::Renderer& renderer, const Scene& scene, float x, float y, const SceneStyle& style) {
    renderer.DrawRectFilled(x, y, scene.width, scene.height, style.background);
    for (const RectOp& rect : scene.rects)
        renderer.DrawRectFilled(x + rect.x, y + rect.y, rect.width, rect.height, FillColor(rect.fill, style));
    for (const TextOp& text : scene.texts) {
        // A chip's own red is the contrast; a shadow on it only blurs the white.
        const int shadow = text.tone == Tone::OnChip ? 0 : style.shadowPixels;
        renderer.DrawSizedText(x + text.x, y + text.y, text.text, ToneColor(text.tone, style), ToTextSize(text.size),
                               style.shadow, shadow);
    }
}

}  // namespace ce::overlay_panel
