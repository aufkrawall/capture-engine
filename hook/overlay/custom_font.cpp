/**
 * Custom Overlay Font Atlas Implementation
 *
 * Uses Windows GDI to rasterize a font into a texture atlas.
 */

#include "custom_font.h"
#include "overlay_font_policy.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>

namespace CustomOverlay {

namespace {

// A rasterized atlas depends only on (font, size, DPI scale, weight). Rendering it is a few ms of GDI work,
// and CE builds a renderer per route: GTA's FSR FG start built two on AMD's presenter thread inside one
// ExecuteCommandLists (session 20260925_225006). Later renderers copy the finished atlas instead.
struct AtlasSnapshot {
    std::string fontName;
    std::string faceName;
    int fontSize = 0;
    float dpiScale = 1.0f;
    int weight = 0;
    std::vector<uint8_t> textureData;
    int textureWidth = 0;
    int textureHeight = 0;
    FontAtlas::GlyphSet sets[kTextSizeCount];
};

std::atomic<uint32_t> g_RasterizedAtlasCount{0};

std::mutex& AtlasCacheMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::shared_ptr<const AtlasSnapshot>>& AtlasCache() {
    static std::vector<std::shared_ptr<const AtlasSnapshot>> cache;
    return cache;
}

std::shared_ptr<const AtlasSnapshot> FindAtlasSnapshot(const char* fontName, int fontSize, float scale, int weight) {
    std::lock_guard<std::mutex> lock(AtlasCacheMutex());
    for (const auto& snapshot : AtlasCache()) {
        if (snapshot->fontSize == fontSize && snapshot->dpiScale == scale && snapshot->weight == weight &&
            snapshot->fontName == fontName) {
            return snapshot;
        }
    }
    return nullptr;
}

constexpr int kDegreeSlot = 127;
// Keep a transparent gutter around every glyph. Linear filtering can then never sample coverage from an
// adjacent cell when a glyph lands between pixels at fractional DPI scales.
constexpr int kGlyphPadding = 2;
constexpr float kSmallTextRatio = 0.82f;
constexpr float kHeroTextRatio = 2.3f;
constexpr int kMinSmallPixelSize = 8;

int GlyphSlot(char c) {
    if (c == kDegreeChar)
        return kDegreeSlot;
    const unsigned char code = static_cast<unsigned char>(c);
    return code >= 32 && code <= 126 ? code : static_cast<int>('?');
}

struct PendingGlyph {
    int sizeIndex = 0;
    int slot = 0;
    wchar_t character = 0;
    int advance = 0;
    int cellLeft = 0;
    int cellTop = 0;
    int cellWidth = 0;
    int cellHeight = 0;
    int atlasX = 0;
    int atlasY = 0;
    bool placed = false;
};

// Rows of glyphs from the top left. Returns how many fit; the bottom edge stays clear of the white block.
size_t PackGlyphs(std::vector<PendingGlyph>& pending, int atlasWidth, int atlasHeight, int whiteBlockSize) {
    int cursorX = kGlyphPadding;
    int cursorY = kGlyphPadding;
    int rowHeight = 0;
    size_t placed = 0;
    for (PendingGlyph& glyph : pending) {
        glyph.placed = false;
        if (cursorX + glyph.cellWidth + kGlyphPadding >= atlasWidth) {
            cursorX = kGlyphPadding;
            cursorY += rowHeight + kGlyphPadding * 2;
            rowHeight = 0;
        }
        if (cursorY + glyph.cellHeight + kGlyphPadding >= atlasHeight - whiteBlockSize)
            continue;
        glyph.atlasX = cursorX;
        glyph.atlasY = cursorY;
        glyph.placed = true;
        ++placed;
        cursorX += glyph.cellWidth + kGlyphPadding * 2;
        rowHeight = (std::max)(rowHeight, glyph.cellHeight);
    }
    return placed;
}

// GDI's advance rectangle is not the glyph's ink rectangle: characters such as 'j', '/', '4', and 'R' can
// paint one or more pixels outside it. The cell is the union, so overhangs remain part of this glyph while
// the original advance and baseline placement stay unchanged.
void MeasureGlyph(HDC hdc, const TEXTMETRICW& tm, PendingGlyph& glyph) {
    SIZE charSize = {};
    GetTextExtentPoint32W(hdc, &glyph.character, 1, &charSize);
    MAT2 identityTransform = {};
    identityTransform.eM11.value = 1;
    identityTransform.eM22.value = 1;
    int cellLeft = 0;
    int cellTop = 0;
    int cellRight = charSize.cx;
    int cellBottom = charSize.cy;
    GLYPHMETRICS glyphMetrics = {};
    if (GetGlyphOutlineW(hdc, glyph.character, GGO_METRICS, &glyphMetrics, 0, nullptr, &identityTransform) !=
            GDI_ERROR &&
        glyphMetrics.gmBlackBoxX > 0 && glyphMetrics.gmBlackBoxY > 0) {
        const int inkLeft = glyphMetrics.gmptGlyphOrigin.x;
        const int inkTop = tm.tmAscent - glyphMetrics.gmptGlyphOrigin.y;
        cellLeft = (std::min)(cellLeft, inkLeft);
        cellTop = (std::min)(cellTop, inkTop);
        cellRight = (std::max)(cellRight, inkLeft + static_cast<int>(glyphMetrics.gmBlackBoxX));
        cellBottom = (std::max)(cellBottom, inkTop + static_cast<int>(glyphMetrics.gmBlackBoxY));
    }
    glyph.advance = charSize.cx;
    glyph.cellLeft = cellLeft;
    glyph.cellTop = cellTop;
    glyph.cellWidth = (std::max)(1, cellRight - cellLeft);
    glyph.cellHeight = (std::max)(1, cellBottom - cellTop);
}

int DigitInkHeight(HDC hdc) {
    MAT2 identityTransform = {};
    identityTransform.eM11.value = 1;
    identityTransform.eM22.value = 1;
    GLYPHMETRICS glyphMetrics = {};
    if (GetGlyphOutlineW(hdc, L'0', GGO_METRICS, &glyphMetrics, 0, nullptr, &identityTransform) == GDI_ERROR)
        return 0;
    return static_cast<int>(glyphMetrics.gmBlackBoxY);
}

std::wstring WideFromUtf8(const char* text) {
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (length <= 1)
        return {};
    std::wstring wide(static_cast<size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide.data(), length);
    return wide;
}

std::string Utf8FromWide(const wchar_t* text) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
        return {};
    std::string narrow(static_cast<size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow.data(), length, nullptr, nullptr);
    return narrow;
}

}  // namespace

FontAtlas::FontAtlas() = default;

FontAtlas::~FontAtlas() {
    Shutdown();
}

bool FontAtlas::Initialize(const char* fontName, int fontSize, float scale, int requestedWeight,
                           bool acceptSubstitute) {
    if (initialized)
        return true;
    if (!fontName)
        return false;

    const int resolvedWeight = ce::overlay_font::ResolveWeight(fontName, requestedWeight);
    if (const auto snapshot = FindAtlasSnapshot(fontName, fontSize, scale, resolvedWeight)) {
        textureData = snapshot->textureData;
        textureWidth = snapshot->textureWidth;
        textureHeight = snapshot->textureHeight;
        dpiScale = snapshot->dpiScale;
        weight = snapshot->weight;
        faceName = snapshot->faceName;
        for (int size = 0; size < kTextSizeCount; ++size)
            sets[size] = snapshot->sets[size];
        initialized = true;
        return true;
    }

    const std::wstring wideName = WideFromUtf8(fontName);
    if (wideName.empty() || !Rasterize(wideName.c_str(), fontSize, scale, resolvedWeight, acceptSubstitute))
        return false;

    initialized = true;
    g_RasterizedAtlasCount.fetch_add(1, std::memory_order_relaxed);
    PublishSnapshot(fontName, fontSize, scale);
    return true;
}

bool FontAtlas::Rasterize(const wchar_t* face, int fontSize, float scale, int resolvedWeight, bool acceptSubstitute) {
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    const int basePixels = (std::max)(1, (int)(fontSize * scale));
    const int pixelSizes[kTextSizeCount] = {
        basePixels,
        (std::max)(kMinSmallPixelSize, static_cast<int>(std::lround(static_cast<float>(basePixels) * kSmallTextRatio))),
        static_cast<int>(std::lround(static_cast<float>(basePixels) * kHeroTextRatio)),
    };

    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc)
        return false;
    const HGDIOBJ originalFont = GetCurrentObject(hdc, OBJ_FONT);

    HFONT fonts[kTextSizeCount] = {};
    const auto releaseFonts = [&]() {
        SelectObject(hdc, originalFont);
        for (HFONT font : fonts) {
            if (font)
                DeleteObject(font);
        }
        DeleteDC(hdc);
    };
    for (int size = 0; size < kTextSizeCount; ++size) {
        fonts[size] = CreateFontW(-pixelSizes[size], 0, 0, 0, resolvedWeight, FALSE, FALSE, FALSE, ANSI_CHARSET,
                                  OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE, face);
        if (!fonts[size]) {
            releaseFonts();
            return false;
        }
    }

    SelectObject(hdc, fonts[0]);
    wchar_t selectedFace[LF_FACESIZE * 2] = {};
    GetTextFaceW(hdc, static_cast<int>(std::size(selectedFace)), selectedFace);
    if (!acceptSubstitute && !ce::overlay_font::FaceMatches(face, selectedFace)) {
        releaseFonts();
        return false;
    }

    std::vector<PendingGlyph> pending;
    pending.reserve(220);
    for (int size = 0; size < kTextSizeCount; ++size) {
        SelectObject(hdc, fonts[size]);
        TEXTMETRICW tm = {};
        GetTextMetricsW(hdc, &tm);
        TextSizeMetrics& metrics = sets[size].metrics;
        metrics.pixelSize = pixelSizes[size];
        metrics.lineHeight = tm.tmHeight + tm.tmExternalLeading;
        metrics.ascent = tm.tmAscent;
        metrics.digitHeight = DigitInkHeight(hdc);

        const size_t firstOfSize = pending.size();
        const auto add = [&](int slot, wchar_t character) {
            PendingGlyph glyph;
            glyph.sizeIndex = size;
            glyph.slot = slot;
            glyph.character = character;
            MeasureGlyph(hdc, tm, glyph);
            pending.push_back(glyph);
        };
        if (static_cast<TextSize>(size) == TextSize::Hero) {
            for (const wchar_t character : {L' ', L'-', L'.'})
                add(static_cast<int>(character), character);
            for (wchar_t digit = L'0'; digit <= L'9'; ++digit)
                add(static_cast<int>(digit), digit);
        } else {
            for (int code = 32; code < 127; ++code)
                add(code, static_cast<wchar_t>(code));
            add(kDegreeSlot, L'\u00B0');
        }

        // One advance for all digits, whatever the font does: GDI cannot select OpenType tabular figures,
        // and proportional ones make a right-aligned column twitch with every value. Each digit stays
        // centered in the shared cell.
        int digitAdvance = 0;
        for (size_t i = firstOfSize; i < pending.size(); ++i) {
            if (pending[i].character >= L'0' && pending[i].character <= L'9')
                digitAdvance = (std::max)(digitAdvance, pending[i].advance);
        }
        for (size_t i = firstOfSize; i < pending.size(); ++i) {
            PendingGlyph& glyph = pending[i];
            if (glyph.character >= L'0' && glyph.character <= L'9') {
                glyph.cellLeft += (digitAdvance - glyph.advance) / 2;
                glyph.advance = digitAdvance;
            }
        }
    }

    // The smallest texture that holds everything; a size beyond the largest keeps what fits.
    constexpr int kAtlasSizes[][2] = {{512, 256},   {512, 512},   {1024, 512},  {1024, 1024},
                                      {2048, 1024}, {2048, 2048}, {4096, 2048}, {4096, 4096}};
    int atlasWidth = 0;
    int atlasHeight = 0;
    for (const auto& candidate : kAtlasSizes) {
        atlasWidth = candidate[0];
        atlasHeight = candidate[1];
        if (PackGlyphs(pending, atlasWidth, atlasHeight, kWhiteBlockSize) == pending.size())
            break;
    }

    const size_t atlasByteSize = static_cast<size_t>(atlasWidth) * static_cast<size_t>(atlasHeight) * 4u;
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
        releaseFonts();
        return false;
    }
    HBITMAP oldBitmap = (HBITMAP)SelectObject(hdc, hBitmap);
    memset(bits, 0, atlasByteSize);
    SetTextColor(hdc, RGB(255, 255, 255));
    SetBkMode(hdc, TRANSPARENT);

    int selectedSize = -1;
    for (const PendingGlyph& glyph : pending) {
        if (!glyph.placed)
            continue;
        if (glyph.sizeIndex != selectedSize) {
            selectedSize = glyph.sizeIndex;
            SelectObject(hdc, fonts[selectedSize]);
        }
        // Undo the digit cell's shift for the drawing origin: the atlas holds the ink at the cell's left.
        SIZE charSize = {};
        GetTextExtentPoint32W(hdc, &glyph.character, 1, &charSize);
        const int digitShift = glyph.character >= L'0' && glyph.character <= L'9'
                                   ? (glyph.advance - charSize.cx) / 2
                                   : 0;
        // Clip to the measured cell as a final isolation boundary. This guards against driver/font rasterizer
        // overdraw without touching the two transparent texels between adjacent cells.
        RECT cellRect = {glyph.atlasX, glyph.atlasY, glyph.atlasX + glyph.cellWidth, glyph.atlasY + glyph.cellHeight};
        ExtTextOutW(hdc, glyph.atlasX - (glyph.cellLeft - digitShift), glyph.atlasY - glyph.cellTop, ETO_CLIPPED,
                    &cellRect, &glyph.character, 1, nullptr);

        Glyph& out = sets[glyph.sizeIndex].glyphs[glyph.slot];
        out.x = static_cast<uint16_t>(glyph.atlasX);
        out.y = static_cast<uint16_t>(glyph.atlasY);
        out.width = static_cast<uint16_t>(glyph.cellWidth);
        out.height = static_cast<uint16_t>(glyph.cellHeight);
        out.xOffset = static_cast<int16_t>(glyph.cellLeft);
        out.yOffset = static_cast<int16_t>(glyph.cellTop);
        out.xAdvance = static_cast<uint16_t>(glyph.advance);
    }

    // TextOut can be queued by GDI. Flush before consuming the DIB bits so an
    // atlas upload can never observe a partially rasterized glyph.
    GdiFlush();

    // Convert bitmap to RGBA texture data
    textureWidth = atlasWidth;
    textureHeight = atlasHeight;
    textureData.resize(atlasByteSize);
    uint8_t* src = (uint8_t*)bits;
    uint8_t* dst = textureData.data();

    for (int y = 0; y < atlasHeight; y++) {
        for (int x = 0; x < atlasWidth; x++) {
            const size_t idx =
                (static_cast<size_t>(y) * static_cast<size_t>(atlasWidth) + static_cast<size_t>(x)) * 4u;
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
    for (int y = atlasHeight - kWhiteBlockSize; y < atlasHeight; y++) {
        memset(dst + static_cast<size_t>(y) * static_cast<size_t>(atlasWidth) * 4u, 255,
               static_cast<size_t>(kWhiteBlockSize) * 4u);
    }

    for (GlyphSet& set : sets)
        BuildGlyphSpans(set);

    SelectObject(hdc, oldBitmap);
    DeleteObject(hBitmap);
    releaseFonts();

    dpiScale = scale;
    weight = resolvedWeight;
    faceName = Utf8FromWide(selectedFace);
    return true;
}

uint32_t FontAtlas::RasterizedAtlasCount() {
    return g_RasterizedAtlasCount.load(std::memory_order_relaxed);
}

void FontAtlas::PublishSnapshot(const char* fontName, int fontSize, float scale) const {
    auto snapshot = std::make_shared<AtlasSnapshot>();
    snapshot->fontName = fontName;
    snapshot->faceName = faceName;
    snapshot->fontSize = fontSize;
    snapshot->dpiScale = scale;
    snapshot->weight = weight;
    snapshot->textureData = textureData;
    snapshot->textureWidth = textureWidth;
    snapshot->textureHeight = textureHeight;
    for (int size = 0; size < kTextSizeCount; ++size)
        snapshot->sets[size] = sets[size];
    std::lock_guard<std::mutex> lock(AtlasCacheMutex());
    for (const auto& existing : AtlasCache()) {
        if (existing->fontSize == fontSize && existing->dpiScale == scale && existing->weight == weight &&
            existing->fontName == fontName) {
            return;
        }
    }
    AtlasCache().push_back(std::move(snapshot));
}

void FontAtlas::Shutdown() {
    textureData.clear();
    textureWidth = 0;
    textureHeight = 0;
    weight = 0;
    faceName.clear();
    for (GlyphSet& set : sets)
        set = GlyphSet{};
    initialized = false;
}

const Glyph* FontAtlas::GetGlyph(char c, TextSize size) const {
    return &sets[static_cast<int>(size)].glyphs[GlyphSlot(c)];
}

const std::vector<GlyphSpan>& FontAtlas::GetGlyphSpans(char c, TextSize size) const {
    return sets[static_cast<int>(size)].spans[GlyphSlot(c)];
}

void FontAtlas::BuildGlyphSpans(GlyphSet& set) {
    if (textureData.empty() || textureWidth <= 0 || textureHeight <= 0)
        return;

    for (int c = 32; c < 128; ++c) {
        const Glyph& g = set.glyphs[c];
        auto& spans = set.spans[c];
        spans.clear();
        if (g.width == 0 || g.height == 0)
            continue;

        for (uint16_t row = 0; row < g.height; ++row) {
            uint16_t runX = 0;
            uint16_t runWidth = 0;
            uint8_t runAlpha = 0;
            const size_t rowStart = spans.size();

            auto flushRun = [&]() {
                if (runWidth == 0)
                    return;
                // Spans that reached the previous row are the only ones a run can continue.
                for (size_t i = 0; i < rowStart; ++i) {
                    GlyphSpan& span = spans[i];
                    if (span.y + span.height == row && span.x == runX && span.width == runWidth &&
                        span.alpha == runAlpha) {
                        ++span.height;
                        runWidth = 0;
                        return;
                    }
                }
                spans.push_back({runX, row, runWidth, 1, runAlpha});
                runWidth = 0;
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

void FontAtlas::CalcTextSize(const char* text, float* outWidth, float* outHeight, TextSize size) const {
    if (!text)
        return;

    const int lineHeight = GetLineHeight(size);
    float width = 0;
    float height = (float)lineHeight;
    float lineWidth = 0;

    while (*text) {
        char c = *text++;
        if (c == '\n') {
            width = (width > lineWidth) ? width : lineWidth;
            lineWidth = 0;
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            height += lineHeight;
            continue;
        }

        const Glyph* g = GetGlyph(c, size);
        if (g) {
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            lineWidth += g->xAdvance;
        }
    }

    if (outWidth)
        *outWidth = (width > lineWidth) ? width : lineWidth;
    if (outHeight)
        *outHeight = height;
}

}  // namespace CustomOverlay
