#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace ce::face_camera {

inline constexpr uint32_t kInitialRepeatSourceGraceMs = 5000;

enum class Shape : uint8_t {
    kRectangle = 0,
    kRoundedRectangle,
    kCircle,
};

enum class Position : uint8_t {
    kTopLeft = 0,
    kTopCenter,
    kTopRight,
    kCenterLeft,
    kCenter,
    kCenterRight,
    kBottomLeft,
    kBottomCenter,
    kBottomRight,
    kCustom,
};

enum class Crop : uint8_t {
    kFill = 0,
    kStretch,
};

struct Config {
    bool enabled = false;
    std::string device = "default";
    uint32_t requestedWidth = 1280;
    uint32_t requestedHeight = 720;
    uint32_t requestedFps = 30;

    Position position = Position::kBottomRight;
    Shape shape = Shape::kRoundedRectangle;
    Crop crop = Crop::kFill;
    float widthPercent = 24.0f;
    float marginPercent = 2.0f;
    float customXPercent = 100.0f;
    float customYPercent = 100.0f;
    float cornerRadiusPercent = 12.0f;
    float opacity = 1.0f;
    float borderWidthPercent = 0.75f;
    uint32_t borderColorRgb = 0xFFFFFFu;
    bool mirror = true;
    uint32_t staleTimeoutMs = 2000;
};

inline bool operator==(const Config& lhs, const Config& rhs) {
    return lhs.enabled == rhs.enabled && lhs.device == rhs.device &&
           lhs.requestedWidth == rhs.requestedWidth && lhs.requestedHeight == rhs.requestedHeight &&
           lhs.requestedFps == rhs.requestedFps && lhs.position == rhs.position && lhs.shape == rhs.shape &&
           lhs.crop == rhs.crop && lhs.widthPercent == rhs.widthPercent && lhs.marginPercent == rhs.marginPercent &&
           lhs.customXPercent == rhs.customXPercent && lhs.customYPercent == rhs.customYPercent &&
           lhs.cornerRadiusPercent == rhs.cornerRadiusPercent && lhs.opacity == rhs.opacity &&
           lhs.borderWidthPercent == rhs.borderWidthPercent && lhs.borderColorRgb == rhs.borderColorRgb &&
           lhs.mirror == rhs.mirror && lhs.staleTimeoutMs == rhs.staleTimeoutMs;
}

inline bool operator!=(const Config& lhs, const Config& rhs) {
    return !(lhs == rhs);
}

inline bool IsFrameStale(uint64_t receivedTickMs, uint64_t nowTickMs, uint32_t timeoutMs) {
    return timeoutMs > 0 && receivedTickMs > 0 && nowTickMs >= receivedTickMs &&
           nowTickMs - receivedTickMs > timeoutMs;
}

inline bool IsWithinInitialRepeatSourceGrace(uint64_t startTickMs, uint64_t nowTickMs, uint32_t staleTimeoutMs) {
    if (startTickMs == 0 || nowTickMs < startTickMs)
        return false;
    const uint64_t graceMs = std::max<uint64_t>(kInitialRepeatSourceGraceMs, staleTimeoutMs);
    return nowTickMs - startTickMs <= graceMs;
}

struct Layout {
    bool valid = false;
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    float normalizedLeft = 0.0f;
    float normalizedTop = 0.0f;
    float normalizedWidth = 0.0f;
    float normalizedHeight = 0.0f;
    float sourceU0 = 0.0f;
    float sourceV0 = 0.0f;
    float sourceU1 = 1.0f;
    float sourceV1 = 1.0f;
    float displayedWidthPixels = 0.0f;
    float displayedHeightPixels = 0.0f;
};

inline bool TryParseShape(const std::string& value, Shape* result) {
    if (!result)
        return false;
    if (value == "rectangle" || value == "rect") {
        *result = Shape::kRectangle;
        return true;
    }
    if (value == "rounded" || value == "rounded_rectangle" || value == "rounded-rectangle") {
        *result = Shape::kRoundedRectangle;
        return true;
    }
    if (value == "circle" || value == "circular") {
        *result = Shape::kCircle;
        return true;
    }
    return false;
}

inline bool TryParsePosition(const std::string& value, Position* result) {
    if (!result)
        return false;
    struct Entry {
        const char* name;
        Position position;
    };
    constexpr Entry entries[] = {
        {"top_left", Position::kTopLeft},         {"top_center", Position::kTopCenter},
        {"top_right", Position::kTopRight},       {"center_left", Position::kCenterLeft},
        {"center", Position::kCenter},            {"center_right", Position::kCenterRight},
        {"bottom_left", Position::kBottomLeft},   {"bottom_center", Position::kBottomCenter},
        {"bottom_right", Position::kBottomRight}, {"custom", Position::kCustom},
    };
    for (const auto& entry : entries) {
        if (value == entry.name) {
            *result = entry.position;
            return true;
        }
    }
    return false;
}

inline bool TryParseCrop(const std::string& value, Crop* result) {
    if (!result)
        return false;
    if (value == "fill" || value == "crop") {
        *result = Crop::kFill;
        return true;
    }
    if (value == "stretch") {
        *result = Crop::kStretch;
        return true;
    }
    return false;
}

inline bool TryParseResolution(const std::string& value, uint32_t* width, uint32_t* height) {
    if (!width || !height)
        return false;
    if (value == "auto") {
        *width = 0;
        *height = 0;
        return true;
    }
    const size_t separator = value.find_first_of("xX");
    if (separator == std::string::npos || value.find_first_of("xX", separator + 1) != std::string::npos)
        return false;
    try {
        size_t widthEnd = 0;
        size_t heightEnd = 0;
        const unsigned long parsedWidth = std::stoul(value.substr(0, separator), &widthEnd, 10);
        const unsigned long parsedHeight = std::stoul(value.substr(separator + 1), &heightEnd, 10);
        if (widthEnd != separator || heightEnd != value.size() - separator - 1 || parsedWidth < 160 ||
            parsedWidth > 7680 || parsedHeight < 120 || parsedHeight > 4320) {
            return false;
        }
        *width = static_cast<uint32_t>(parsedWidth);
        *height = static_cast<uint32_t>(parsedHeight);
        return true;
    } catch (...) {
        return false;
    }
}

inline Layout ResolveLayout(const Config& config, uint32_t targetWidth, uint32_t targetHeight,
                            uint32_t displayedWidth, uint32_t displayedHeight, uint32_t sourceWidth,
                            uint32_t sourceHeight) {
    Layout result;
    if (targetWidth == 0 || targetHeight == 0 || displayedWidth == 0 || displayedHeight == 0 || sourceWidth == 0 ||
        sourceHeight == 0) {
        return result;
    }

    const float displayW = static_cast<float>(displayedWidth);
    const float displayH = static_cast<float>(displayedHeight);
    const float margin = std::min(displayW, displayH) * std::clamp(config.marginPercent, 0.0f, 25.0f) / 100.0f;
    const float availableW = std::max(1.0f, displayW - 2.0f * margin);
    const float availableH = std::max(1.0f, displayH - 2.0f * margin);
    const float sourceAspect = static_cast<float>(sourceWidth) / static_cast<float>(sourceHeight);
    const float destinationAspect = config.shape == Shape::kCircle ? 1.0f : sourceAspect;

    float overlayW = std::clamp(displayW * std::clamp(config.widthPercent, 5.0f, 100.0f) / 100.0f, 1.0f,
                                availableW);
    float overlayH = overlayW / std::max(destinationAspect, 0.001f);
    if (overlayH > availableH) {
        const float scale = availableH / overlayH;
        overlayW *= scale;
        overlayH = availableH;
    }

    enum class Horizontal { kLeft, kCenter, kRight, kCustom };
    enum class Vertical { kTop, kCenter, kBottom, kCustom };
    Horizontal horizontal = Horizontal::kRight;
    Vertical vertical = Vertical::kBottom;
    switch (config.position) {
        case Position::kTopLeft: horizontal = Horizontal::kLeft; vertical = Vertical::kTop; break;
        case Position::kTopCenter: horizontal = Horizontal::kCenter; vertical = Vertical::kTop; break;
        case Position::kTopRight: horizontal = Horizontal::kRight; vertical = Vertical::kTop; break;
        case Position::kCenterLeft: horizontal = Horizontal::kLeft; vertical = Vertical::kCenter; break;
        case Position::kCenter: horizontal = Horizontal::kCenter; vertical = Vertical::kCenter; break;
        case Position::kCenterRight: horizontal = Horizontal::kRight; vertical = Vertical::kCenter; break;
        case Position::kBottomLeft: horizontal = Horizontal::kLeft; vertical = Vertical::kBottom; break;
        case Position::kBottomCenter: horizontal = Horizontal::kCenter; vertical = Vertical::kBottom; break;
        case Position::kBottomRight: horizontal = Horizontal::kRight; vertical = Vertical::kBottom; break;
        case Position::kCustom: horizontal = Horizontal::kCustom; vertical = Vertical::kCustom; break;
    }

    const float travelX = std::max(0.0f, availableW - overlayW);
    const float travelY = std::max(0.0f, availableH - overlayH);
    const float left = margin + (horizontal == Horizontal::kLeft
                                     ? 0.0f
                                     : horizontal == Horizontal::kCenter
                                           ? travelX * 0.5f
                                           : horizontal == Horizontal::kRight
                                                 ? travelX
                                                 : travelX * std::clamp(config.customXPercent, 0.0f, 100.0f) /
                                                       100.0f);
    const float top = margin + (vertical == Vertical::kTop
                                    ? 0.0f
                                    : vertical == Vertical::kCenter
                                          ? travelY * 0.5f
                                          : vertical == Vertical::kBottom
                                                ? travelY
                                                : travelY * std::clamp(config.customYPercent, 0.0f, 100.0f) /
                                                      100.0f);

    const float targetScaleX = static_cast<float>(targetWidth) / displayW;
    const float targetScaleY = static_cast<float>(targetHeight) / displayH;
    result.left = std::clamp(static_cast<int>(std::floor(left * targetScaleX)), 0, static_cast<int>(targetWidth));
    result.top = std::clamp(static_cast<int>(std::floor(top * targetScaleY)), 0, static_cast<int>(targetHeight));
    result.right = std::clamp(static_cast<int>(std::ceil((left + overlayW) * targetScaleX)), result.left,
                              static_cast<int>(targetWidth));
    result.bottom = std::clamp(static_cast<int>(std::ceil((top + overlayH) * targetScaleY)), result.top,
                               static_cast<int>(targetHeight));
    if (result.left >= result.right || result.top >= result.bottom)
        return result;

    result.normalizedLeft = static_cast<float>(result.left) / static_cast<float>(targetWidth);
    result.normalizedTop = static_cast<float>(result.top) / static_cast<float>(targetHeight);
    result.normalizedWidth = static_cast<float>(result.right - result.left) / static_cast<float>(targetWidth);
    result.normalizedHeight = static_cast<float>(result.bottom - result.top) / static_cast<float>(targetHeight);
    result.displayedWidthPixels = overlayW;
    result.displayedHeightPixels = overlayH;

    if (config.crop == Crop::kFill) {
        const float displayedAspect = overlayW / std::max(overlayH, 0.001f);
        if (sourceAspect > displayedAspect) {
            const float visibleWidth = displayedAspect / sourceAspect;
            result.sourceU0 = (1.0f - visibleWidth) * 0.5f;
            result.sourceU1 = 1.0f - result.sourceU0;
        } else if (sourceAspect < displayedAspect) {
            const float visibleHeight = sourceAspect / displayedAspect;
            result.sourceV0 = (1.0f - visibleHeight) * 0.5f;
            result.sourceV1 = 1.0f - result.sourceV0;
        }
    }
    result.valid = true;
    return result;
}

}  // namespace ce::face_camera
