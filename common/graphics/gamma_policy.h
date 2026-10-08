#pragma once

#include <cmath>
#include <cstdint>

// Display-curve compensation operates on encoded SDR values, independently of
// texture/view encoding. The source is the display curve the game expects;
// the destination is the monitor's calibrated curve.
namespace ce::gamma {

enum class Curve : uint8_t { Default = 0, Power22 = 1, Power24 = 2, Srgb = 3 };

inline bool EqualText(const char* left, const char* right) {
    if (!left || !right)
        return false;
    for (; *left && *right; ++left, ++right) {
        char value = *left;
        if (value >= 'A' && value <= 'Z')
            value = static_cast<char>(value - 'A' + 'a');
        if (value != *right)
            return false;
    }
    return *left == *right;
}

inline bool TryParse(const char* text, Curve& curve) {
    if (EqualText(text, "default") || EqualText(text, "off"))
        curve = Curve::Default;
    else if (EqualText(text, "2.2") || EqualText(text, "2,2"))
        curve = Curve::Power22;
    else if (EqualText(text, "2.4") || EqualText(text, "2,4"))
        curve = Curve::Power24;
    else if (EqualText(text, "srgb") || EqualText(text, "piecewise"))
        curve = Curve::Srgb;
    else
        return false;
    return true;
}

inline const char* Name(Curve curve) {
    switch (curve) {
        case Curve::Power22: return "2.2";
        case Curve::Power24: return "2.4";
        case Curve::Srgb: return "srgb";
        default: return "default";
    }
}

// Matches UE's convention: negative disables, zero is piecewise sRGB.
inline constexpr float Exponent(Curve curve) {
    switch (curve) {
        case Curve::Power22: return 2.2f;
        case Curve::Power24: return 2.4f;
        case Curve::Srgb: return 0.0f;
        default: return -1.0f;
    }
}

inline constexpr bool ValidExponent(float value) {
    return value == 0.0f || (value >= 1.0f && value <= 3.0f);
}

struct Request {
    float source = 2.2f;
    float destination = -1.0f;
};

inline constexpr bool Requested(const Request& request) {
    return ValidExponent(request.source) && ValidExponent(request.destination) &&
           request.source != request.destination;
}

inline double Decode(double value, double exponent) {
    const double magnitude = std::abs(value);
    const double linear = exponent == 0.0
                              ? (magnitude <= 0.04045 ? magnitude / 12.92
                                                      : std::pow((magnitude + 0.055) / 1.055, 2.4))
                              : std::pow(magnitude, exponent);
    return std::copysign(linear, value);
}

inline double Encode(double value, double exponent) {
    const double magnitude = std::abs(value);
    const double encoded = exponent == 0.0
                               ? (magnitude <= 0.0031308 ? magnitude * 12.92
                                                        : 1.055 * std::pow(magnitude, 1.0 / 2.4) - 0.055)
                               : std::pow(magnitude, 1.0 / exponent);
    return std::copysign(encoded, value);
}

inline double Convert(double value, const Request& request) {
    return Requested(request) ? Encode(Decode(value, request.source), request.destination) : value;
}

}  // namespace ce::gamma
