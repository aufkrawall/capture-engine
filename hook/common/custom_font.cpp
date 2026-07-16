/**
 * Custom Overlay Font Atlas Implementation
 *
 * Uses Windows GDI to rasterize a font into a texture atlas.
 */

#include "custom_font.h"
#include <algorithm>
#include <cstring>

namespace CustomOverlay {

FontAtlas::FontAtlas() {
    memset(glyphs, 0, sizeof(glyphs));
}

FontAtlas::~FontAtlas() {
    Shutdown();
}

bool FontAtlas::Initialize(const char* fontName, int fontSize, float scale) {
    if (initialized)
        return true;

    memset(glyphs, 0, sizeof(glyphs));
    for (auto& spans : glyphSpans) {
        spans.clear();
    }

    dpiScale = scale;
    int scaledSize = (int)(fontSize * scale);

    // Create DC for font metrics
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc)
        return false;

    // Create font
    HFONT hFont = CreateFontA(-scaledSize,          // Height (negative = character height)
                              0,                    // Width
                              0, 0,                 // Escapement, Orientation
                              FW_BOLD,              // Weight - Bold for better visibility
                              FALSE, FALSE, FALSE,  // Italic, Underline, Strikeout
                              ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, fontName);

    if (!hFont) {
        DeleteDC(hdc);
        return false;
    }

    HFONT oldFont = (HFONT)SelectObject(hdc, hFont);

    // Get font metrics
    TEXTMETRICA tm;
    GetTextMetricsA(hdc, &tm);
    lineHeight = tm.tmHeight + tm.tmExternalLeading;

    // Calculate atlas size needed
    // For ASCII printable chars (32-126), estimate ~16 chars per row
    int atlasWidth = 512;
    int atlasHeight = 256;

    // Scale atlas for high DPI
    if (scale > 1.5f) {
        atlasWidth = 1024;
        atlasHeight = 512;
    }

    textureWidth = atlasWidth;
    textureHeight = atlasHeight;

    // Create DIB section for rendering
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = atlasWidth;
    bmi.bmiHeader.biHeight = -atlasHeight;  // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hBitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hBitmap) {
        SelectObject(hdc, oldFont);
        DeleteObject(hFont);
        DeleteDC(hdc);
        return false;
    }

    HBITMAP oldBitmap = (HBITMAP)SelectObject(hdc, hBitmap);

    // Clear to black
    memset(bits, 0, atlasWidth * atlasHeight * 4);

    // Set text rendering
    SetTextColor(hdc, RGB(255, 255, 255));
    SetBkMode(hdc, TRANSPARENT);

    // Keep a transparent gutter around every glyph. Linear filtering can then
    // never sample coverage from an adjacent cell when a glyph lands between
    // pixels at fractional DPI scales.
    constexpr int kGlyphPadding = 2;
    int cursorX = kGlyphPadding;
    int cursorY = kGlyphPadding;
    int maxRowHeight = 0;
    MAT2 identityTransform = {};
    identityTransform.eM11.value = 1;
    identityTransform.eM22.value = 1;

    for (int c = 32; c < 127; c++) {
        char ch = (char)c;

        SIZE charSize;
        GetTextExtentPoint32A(hdc, &ch, 1, &charSize);

        // GDI's advance rectangle is not the glyph's ink rectangle: characters
        // such as 'j', '/', '4', and 'R' can paint one or more pixels outside
        // it. Pack the union so overhangs remain part of this glyph while the
        // original advance and baseline placement stay unchanged.
        int cellLeft = 0;
        int cellTop = 0;
        int cellRight = charSize.cx;
        int cellBottom = charSize.cy;
        GLYPHMETRICS glyphMetrics = {};
        if (GetGlyphOutlineA(hdc, (UINT)(unsigned char)ch, GGO_METRICS, &glyphMetrics, 0, nullptr,
                             &identityTransform) != GDI_ERROR &&
            glyphMetrics.gmBlackBoxX > 0 && glyphMetrics.gmBlackBoxY > 0) {
            const int inkLeft = glyphMetrics.gmptGlyphOrigin.x;
            const int inkTop = tm.tmAscent - glyphMetrics.gmptGlyphOrigin.y;
            const int inkRight = inkLeft + (int)glyphMetrics.gmBlackBoxX;
            const int inkBottom = inkTop + (int)glyphMetrics.gmBlackBoxY;
            cellLeft = (std::min)(cellLeft, inkLeft);
            cellTop = (std::min)(cellTop, inkTop);
            cellRight = (std::max)(cellRight, inkRight);
            cellBottom = (std::max)(cellBottom, inkBottom);
        }
        const int cellWidth = (std::max)(1, cellRight - cellLeft);
        const int cellHeight = (std::max)(1, cellBottom - cellTop);

        // Check if we need to wrap to next row
        if (cursorX + cellWidth + kGlyphPadding >= atlasWidth) {
            cursorX = kGlyphPadding;
            cursorY += maxRowHeight + kGlyphPadding * 2;
            maxRowHeight = 0;
        }

        // Skip glyph if it would overflow the atlas vertically
        if (cursorY + cellHeight + kGlyphPadding >= atlasHeight)
            continue;

        // Clip to the measured cell as a final isolation boundary. This guards
        // against driver/font rasterizer overdraw without touching the two
        // transparent texels between adjacent cells.
        RECT cellRect = {cursorX, cursorY, cursorX + cellWidth, cursorY + cellHeight};
        ExtTextOutA(hdc, cursorX - cellLeft, cursorY - cellTop, ETO_CLIPPED, &cellRect, &ch, 1, nullptr);

        // Store glyph info
        glyphs[c].x = (uint16_t)cursorX;
        glyphs[c].y = (uint16_t)cursorY;
        glyphs[c].width = (uint16_t)cellWidth;
        glyphs[c].height = (uint16_t)cellHeight;
        glyphs[c].xOffset = (int16_t)cellLeft;
        glyphs[c].yOffset = (int16_t)cellTop;
        glyphs[c].xAdvance = (uint16_t)charSize.cx;

        cursorX += cellWidth + kGlyphPadding * 2;
        maxRowHeight = (std::max)(maxRowHeight, cellHeight);
    }

    // TextOut can be queued by GDI. Flush before consuming the DIB bits so an
    // atlas upload can never observe a partially rasterized glyph.
    GdiFlush();

    // Convert bitmap to RGBA texture data
    textureData.resize(atlasWidth * atlasHeight * 4);
    uint8_t* src = (uint8_t*)bits;
    uint8_t* dst = textureData.data();

    for (int y = 0; y < atlasHeight; y++) {
        for (int x = 0; x < atlasWidth; x++) {
            int idx = (y * atlasWidth + x) * 4;
            // Source is BGRA, we want RGBA
            uint8_t b = src[idx + 0];
            uint8_t g = src[idx + 1];
            uint8_t r = src[idx + 2];
            // Use brightness as alpha (white text on black background)
            uint8_t alpha = (uint8_t)((r + g + b) / 3);

            dst[idx + 0] = 255;    // R (white)
            dst[idx + 1] = 255;    // G
            dst[idx + 2] = 255;    // B
            dst[idx + 3] = alpha;  // A from brightness
        }
    }

    BuildGlyphSpans();

    // Cleanup
    SelectObject(hdc, oldBitmap);
    SelectObject(hdc, oldFont);
    DeleteObject(hBitmap);
    DeleteObject(hFont);
    DeleteDC(hdc);

    initialized = true;
    return true;
}

void FontAtlas::Shutdown() {
    textureData.clear();
    textureWidth = 0;
    textureHeight = 0;
    lineHeight = 0;
    memset(glyphs, 0, sizeof(glyphs));
    for (auto& spans : glyphSpans) {
        spans.clear();
    }
    initialized = false;
}

const Glyph* FontAtlas::GetGlyph(char c) const {
    const unsigned char code = static_cast<unsigned char>(c);
    const unsigned char index = code >= 32 && code <= 126 ? code : static_cast<unsigned char>('?');
    return &glyphs[index];
}

const std::vector<GlyphSpan>& FontAtlas::GetGlyphSpans(char c) const {
    const unsigned char code = static_cast<unsigned char>(c);
    const unsigned char index = code >= 32 && code <= 126 ? code : static_cast<unsigned char>('?');
    return glyphSpans[index];
}

void FontAtlas::BuildGlyphSpans() {
    if (textureData.empty() || textureWidth <= 0 || textureHeight <= 0)
        return;

    for (int c = 32; c < 127; ++c) {
        const Glyph& g = glyphs[c];
        auto& spans = glyphSpans[c];
        spans.clear();

        if (g.width == 0 || g.height == 0)
            continue;

        for (uint16_t row = 0; row < g.height; ++row) {
            uint16_t runX = 0;
            uint16_t runWidth = 0;
            uint8_t runAlpha = 0;

            auto flushRun = [&]() {
                if (runWidth > 0) {
                    spans.push_back({runX, row, runWidth, runAlpha});
                    runWidth = 0;
                }
            };

            for (uint16_t col = 0; col < g.width; ++col) {
                const int tx = g.x + col;
                const int ty = g.y + row;
                if (tx < 0 || tx >= textureWidth || ty < 0 || ty >= textureHeight) {
                    flushRun();
                    continue;
                }

                const size_t idx = ((size_t)ty * (size_t)textureWidth + (size_t)tx) * 4 + 3;
                const uint8_t alpha = textureData[idx];
                if (alpha == 0) {
                    flushRun();
                    continue;
                }

                const int bucket = (std::min)(255, (std::max)(16, ((int)alpha + 15) & ~15));
                const uint8_t quantizedAlpha = (uint8_t)bucket;
                if (runWidth > 0 && runAlpha == quantizedAlpha) {
                    ++runWidth;
                } else {
                    flushRun();
                    runX = col;
                    runWidth = 1;
                    runAlpha = quantizedAlpha;
                }
            }

            flushRun();
        }
    }
}

void FontAtlas::CalcTextSize(const char* text, float* outWidth, float* outHeight) const {
    if (!text)
        return;

    float width = 0;
    float height = (float)lineHeight;
    float lineWidth = 0;

    while (*text) {
        char c = *text++;
        if (c == '\n') {
            width = (width > lineWidth) ? width : lineWidth;
            lineWidth = 0;
            height += lineHeight;
            continue;
        }

        const Glyph* g = GetGlyph(c);
        if (g) {
            lineWidth += g->xAdvance;
        }
    }

    if (outWidth)
        *outWidth = (width > lineWidth) ? width : lineWidth;
    if (outHeight)
        *outHeight = height;
}

}  // namespace CustomOverlay
