#pragma once

#include <algorithm>
#include <cstdint>

namespace ce::cursor_bitmap {

struct Bgra8 {
    uint8_t b = 0;
    uint8_t g = 0;
    uint8_t r = 0;
    uint8_t a = 0;
};

inline int MedianOfThree(int a, int b, int c) {
    return a + b + c - std::min({a, b, c}) - std::max({a, b, c});
}

// Recover a straight-alpha cursor texel from DrawIconEx results over opaque
// black and white backgrounds. This avoids relying on whether a particular
// HCURSOR's color bitmap stores straight, premultiplied, or absent alpha.
inline Bgra8 ReconstructStraightAlpha(Bgra8 overBlack, Bgra8 overWhite) {
    const int backgroundContribution = std::clamp(
        MedianOfThree(static_cast<int>(overWhite.b) - overBlack.b,
                      static_cast<int>(overWhite.g) - overBlack.g,
                      static_cast<int>(overWhite.r) - overBlack.r),
        0, 255);
    const int alpha = 255 - backgroundContribution;
    if (alpha == 0) {
        return {};
    }

    auto unpremultiply = [alpha](uint8_t channel) {
        return static_cast<uint8_t>(std::clamp((static_cast<int>(channel) * 255 + alpha / 2) / alpha, 0, 255));
    };
    return {
        unpremultiply(overBlack.b),
        unpremultiply(overBlack.g),
        unpremultiply(overBlack.r),
        static_cast<uint8_t>(alpha),
    };
}

}  // namespace ce::cursor_bitmap
