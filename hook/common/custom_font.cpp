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

    // Render each ASCII character
    int cursorX = 1;
    int cursorY = 1;
    int maxRowHeight = 0;

    for (int c = 32; c < 127; c++) {
        char ch = (char)c;

        SIZE charSize;
        GetTextExtentPoint32A(hdc, &ch, 1, &charSize);

        // Check if we need to wrap to next row
        if (cursorX + charSize.cx + 1 >= atlasWidth) {
            cursorX = 1;
            cursorY += maxRowHeight + 1;
            maxRowHeight = 0;
        }

        // Skip glyph if it would overflow the atlas vertically
        if (cursorY + charSize.cy >= atlasHeight)
            continue;

        // Draw character
        TextOutA(hdc, cursorX, cursorY, &ch, 1);

        // Store glyph info
        glyphs[c].x = (uint16_t)cursorX;
        glyphs[c].y = (uint16_t)cursorY;
        glyphs[c].width = (uint16_t)charSize.cx;
        glyphs[c].height = (uint16_t)charSize.cy;
        glyphs[c].xOffset = 0;
        glyphs[c].yOffset = 0;
        glyphs[c].xAdvance = (uint16_t)charSize.cx;

        cursorX += charSize.cx + 1;
        maxRowHeight = (maxRowHeight > (int)charSize.cy) ? maxRowHeight : (int)charSize.cy;
    }

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
    initialized = false;
}

const Glyph* FontAtlas::GetGlyph(char c) const {
    if (c < 32 || c > 126)
        c = '?';
    return &glyphs[(int)c];
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
