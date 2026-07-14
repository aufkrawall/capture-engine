#pragma once

#include <algorithm>
#include <cstdint>

namespace ce::cursor_geometry {

inline int ResolveHotspotForPosition(int hotspot, bool positionIsShapeTopLeft) {
    return positionIsShapeTopLeft ? 0 : hotspot;
}

struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

struct ClippedRects {
    Rect source;
    Rect destination;
};

inline int ScaleLeadingEdge(int value, int numerator, int denominator) {
    const int64_t scaled = static_cast<int64_t>(value) * numerator;
    int64_t quotient = scaled / denominator;
    if (scaled % denominator < 0) {
        --quotient;
    }
    return static_cast<int>(quotient);
}

inline int ScaleTrailingEdge(int value, int numerator, int denominator) {
    const int64_t scaled = static_cast<int64_t>(value) * numerator;
    int64_t quotient = scaled / denominator;
    if (scaled % denominator > 0) {
        ++quotient;
    }
    return static_cast<int>(quotient);
}

inline bool ScaleDestinationRect(const Rect& input, int outputWidth, int outputHeight, int inputWidth, int inputHeight,
                                 Rect* output) {
    if (!output || outputWidth <= 0 || outputHeight <= 0 || inputWidth <= 0 || inputHeight <= 0) {
        return false;
    }
    *output = {
        ScaleLeadingEdge(input.left, outputWidth, inputWidth),
        ScaleLeadingEdge(input.top, outputHeight, inputHeight),
        ScaleTrailingEdge(input.right, outputWidth, inputWidth),
        ScaleTrailingEdge(input.bottom, outputHeight, inputHeight),
    };
    return output->left < output->right && output->top < output->bottom;
}

// Maps a screen-space cursor rectangle into the encoded frame. The captured
// content can be a monitor, a window client area, or a swap-chain texture whose
// dimensions differ from the client area (render scaling / DPI transitions).
inline bool MapScreenCursorToFrame(int screenX, int screenY, int hotspotX, int hotspotY, int cursorWidth,
                                   int cursorHeight, int captureLeft, int captureTop, int captureWidth,
                                   int captureHeight, int frameWidth, int frameHeight, Rect* output) {
    if (!output || cursorWidth <= 0 || cursorHeight <= 0 || captureWidth <= 0 || captureHeight <= 0 ||
        frameWidth <= 0 || frameHeight <= 0) {
        return false;
    }

    const Rect screenRelative = {
        screenX - hotspotX - captureLeft,
        screenY - hotspotY - captureTop,
        screenX - hotspotX - captureLeft + cursorWidth,
        screenY - hotspotY - captureTop + cursorHeight,
    };
    return ScaleDestinationRect(screenRelative, frameWidth, frameHeight, captureWidth, captureHeight, output);
}

// Clips a scaled cursor destination to the output frame and derives the
// corresponding source crop. Using floor for the leading edges and ceil for
// the trailing edges preserves every source texel that contributes to the
// visible destination without stretching the full cursor at frame edges.
inline bool ComputeClippedRects(const Rect& destination, int sourceWidth, int sourceHeight, int frameWidth,
                                int frameHeight, ClippedRects* result) {
    if (!result || sourceWidth <= 0 || sourceHeight <= 0 || frameWidth <= 0 || frameHeight <= 0) {
        return false;
    }

    const int destinationWidth = destination.right - destination.left;
    const int destinationHeight = destination.bottom - destination.top;
    if (destinationWidth <= 0 || destinationHeight <= 0) {
        return false;
    }

    Rect clipped = {
        std::max(destination.left, 0),
        std::max(destination.top, 0),
        std::min(destination.right, frameWidth),
        std::min(destination.bottom, frameHeight),
    };
    if (clipped.left >= clipped.right || clipped.top >= clipped.bottom) {
        return false;
    }

    Rect source = {
        ScaleLeadingEdge(clipped.left - destination.left, sourceWidth, destinationWidth),
        ScaleLeadingEdge(clipped.top - destination.top, sourceHeight, destinationHeight),
        ScaleTrailingEdge(clipped.right - destination.left, sourceWidth, destinationWidth),
        ScaleTrailingEdge(clipped.bottom - destination.top, sourceHeight, destinationHeight),
    };
    source.left = std::clamp(source.left, 0, sourceWidth);
    source.top = std::clamp(source.top, 0, sourceHeight);
    source.right = std::clamp(source.right, 0, sourceWidth);
    source.bottom = std::clamp(source.bottom, 0, sourceHeight);
    if (source.left >= source.right || source.top >= source.bottom) {
        return false;
    }

    result->source = source;
    result->destination = clipped;
    return true;
}

}  // namespace ce::cursor_geometry
