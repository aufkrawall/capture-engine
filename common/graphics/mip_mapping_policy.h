#pragma once

#include <string_view>

namespace ce::mip_mapping {

enum class Mode {
    Default,
    Nearest,
    Bilinear,
    Trilinear,
};

inline bool TryParseMode(std::string_view value, Mode& mode) {
    if (value.empty() || value == "default") {
        mode = Mode::Default;
        return true;
    }
    if (value == "nearest") {
        mode = Mode::Nearest;
        return true;
    }
    if (value == "bilinear") {
        mode = Mode::Bilinear;
        return true;
    }
    if (value == "trilinear") {
        mode = Mode::Trilinear;
        return true;
    }
    return false;
}

inline Mode ParseMode(std::string_view value) {
    Mode mode = Mode::Default;
    TryParseMode(value, mode);
    return mode;
}

inline bool IsExplicit(Mode mode) {
    return mode != Mode::Default;
}

template <typename T>
inline void ApplyDiscreteFilters(Mode mode, T pointMag, T linearMag, T pointMin, T linearMin, T pointMip,
                                 T linearMip, T& magFilter, T& minFilter, T& mipFilter) {
    switch (mode) {
        case Mode::Nearest:
            magFilter = pointMag;
            minFilter = pointMin;
            mipFilter = pointMip;
            break;
        case Mode::Bilinear:
            magFilter = linearMag;
            minFilter = linearMin;
            mipFilter = pointMip;
            break;
        case Mode::Trilinear:
            magFilter = linearMag;
            minFilter = linearMin;
            mipFilter = linearMip;
            break;
        case Mode::Default:
            break;
    }
}

constexpr int kGLNearest = 0x2600;
constexpr int kGLLinear = 0x2601;
constexpr int kGLNearestMipmapNearest = 0x2700;
constexpr int kGLLinearMipmapNearest = 0x2701;
constexpr int kGLNearestMipmapLinear = 0x2702;
constexpr int kGLLinearMipmapLinear = 0x2703;

inline bool IsOpenGLMipFilter(int filter) {
    return filter >= kGLNearestMipmapNearest && filter <= kGLLinearMipmapLinear;
}

inline int ApplyOpenGLMinFilter(Mode mode, int filter) {
    if (!IsOpenGLMipFilter(filter))
        return filter;
    switch (mode) {
        case Mode::Nearest:
            return kGLNearestMipmapNearest;
        case Mode::Bilinear:
            return kGLLinearMipmapNearest;
        case Mode::Trilinear:
            return kGLLinearMipmapLinear;
        case Mode::Default:
            return filter;
    }
    return filter;
}

inline int ApplyOpenGLMagFilter(Mode mode, int filter, bool mipmappingEnabled) {
    if (!mipmappingEnabled)
        return filter;
    if (mode == Mode::Nearest)
        return kGLNearest;
    if (mode == Mode::Bilinear || mode == Mode::Trilinear)
        return kGLLinear;
    return filter;
}

}  // namespace ce::mip_mapping
