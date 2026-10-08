#pragma once

#include <cstdint>

#include "common/graphics/gamma_policy.h"

// Post-processing sharpen policy.
//
// CaptureEngine applies AMD FidelityFX CAS or RCAS to the frame the game is
// about to present, immediately before the overlay draws into the same target.
// That ordering is the whole design: the sharpen pass inherits every route
// decision the overlay already made, and CE's own overlay pixels are written
// after the filter has run, so they are never sharpened.
//
// Everything here is pure decision logic so it can be unit tested without a
// device. The renderers own the resources; this header owns the rules.
namespace ce::sharpen {

enum class Mode : uint8_t {
    Off = 0,
    // Contrast Adaptive Sharpening, sharpen-only (no scaling). 3x3 kernel.
    Cas = 1,
    // Robust Contrast Adaptive Sharpening, the FSR1 second pass. 5-tap cross.
    Rcas = 2,
};

// How the surface the pass reads and writes encodes its values. This is
// presentation meaning, never storage format: R10G10B10A2 is SDR or HDR10
// depending on the swapchain's color space, and FP16 is scRGB only under the
// matching contract.
enum class TargetEncoding : uint8_t {
    Unknown = 0,
    // sRGB-encoded 8-bit, either through an _SRGB view or by convention.
    Srgb,
    // Plain UNORM treated as already gamma-encoded Rec.709.
    Unorm,
    // HDR10 / ST 2084. PQ is itself perceptual, so the filter runs directly.
    Pq,
    // scRGB FP16: linear light. Sharpening linear values rings badly around
    // highlights, so the filter runs in a gamma space and converts back.
    ScrgbLinear,
    // Linear SDR transport; unlike HDR scRGB, gamma compensation is applicable.
    SdrLinear,
};

// The user's requested working space. `Auto` resolves from the encoding.
enum class ConfiguredSpace : uint8_t {
    Auto = 0,
    // Filter the stored values as they are.
    Direct,
    // Treat stored values as linear, encode to gamma for the filter, decode after.
    Gamma,
};

// What the shader is told to do with the values it loads.
enum class FilterSpace : uint8_t {
    Direct = 0,
    LinearToGamma = 1,
    ScrgbToPq = 2,
};

// Where the overlay route is about to draw. The sharpen pass rides exactly that
// target, so these are the overlay's own routes rather than a second taxonomy.
enum class Route : uint8_t {
    Unknown = 0,
    // The application's real backbuffer, the ordinary case.
    NormalBackbuffer,
    // CE renders into its own copy and blits: still a whole frame.
    OffscreenCopy,
    // DX12's post-Streamline route, on the frame Streamline is about to present.
    PostStreamline,
    // A present interposer's private output chain (NvPresent64). Every displayed
    // frame arrives here, real and generated alike.
    InterposerOutputChain,
    // The frame-generation runtime's UI resource. This is a transparent overlay
    // texture, NOT a frame, so there is nothing here to sharpen.
    RuntimeUiResource,
    // FSR/other runtime final output, never its interpolation input or UI texture.
    NativeFgOutput,
};

// AMD's parameters have different native conventions - CAS sharpness rises with
// the value, RCAS attenuation falls with it - so CE exposes one 0..1 contrast/strength
// and maps it per effect. Neither effect is "off" at 0: 0 is the mildest
// setting each one supports. `Mode::Off` is the only way to disable the pass.
// `contrast` (or `strength`) controls the kernel reaction to local contrast.
inline constexpr float kMinContrast = 0.0f;
inline constexpr float kMaxContrast = 1.0f;
inline constexpr float kDefaultContrast = 0.5f;

inline constexpr float kMinStrength = kMinContrast;
inline constexpr float kMaxStrength = kMaxContrast;
inline constexpr float kDefaultStrength = kDefaultContrast;

// How much of the filtered result is mixed back over the original pixels. This
// is the knob that behaves the way "sharpening amount" is normally expected to:
// 0 is genuinely no sharpening and 1 is the effect at full weight. It is
// independent of `contrast`, which selects how the kernel itself reacts to
// contrast, and it is the same control ReShade's CAS port calls "Sharpening
// Intensity" next to its "Contrast Adaptation".
inline constexpr float kMinAmount = 0.0f;
inline constexpr float kMaxAmount = 1.0f;
inline constexpr float kDefaultAmount = 1.0f;

inline constexpr float kMinIntensity = kMinAmount;
inline constexpr float kMaxIntensity = kMaxAmount;
inline constexpr float kDefaultIntensity = kDefaultAmount;

// Below this a target is a thumbnail, a probe chain, or a driver scratch
// surface rather than a frame worth filtering.
inline constexpr uint32_t kMinTargetExtent = 32;

inline constexpr float ClampContrast(float contrast) {
    if (!(contrast >= kMinContrast))  // Also rejects NaN.
        return kMinContrast;
    if (contrast > kMaxContrast)
        return kMaxContrast;
    return contrast;
}

inline constexpr float ClampStrength(float strength) {
    return ClampContrast(strength);
}

inline constexpr float ClampAmount(float amount) {
    if (!(amount >= kMinAmount))  // Also rejects NaN.
        return kMinAmount;
    if (amount > kMaxAmount)
        return kMaxAmount;
    return amount;
}

inline constexpr float ClampIntensity(float intensity) {
    return ClampAmount(intensity);
}

// CAS takes sharpness directly: 0 is its default (lowest ringing), 1 its maximum.
inline constexpr float CasSharpnessFromStrength(float strength) {
    return ClampStrength(strength);
}

// RCAS takes an attenuation in stops, where 0 is sharpest and larger is softer.
// AMD's own guidance is to stay at or below 2 stops.
inline constexpr float kRcasMaxAttenuation = 2.0f;
inline constexpr float RcasAttenuationFromStrength(float strength) {
    return (kMaxStrength - ClampStrength(strength)) * kRcasMaxAttenuation;
}

inline bool StringsEqualNoCase(const char* a, const char* b) {
    if (!a || !b)
        return false;
    for (; *a && *b; ++a, ++b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb)
            return false;
    }
    return *a == '\0' && *b == '\0';
}

// Unrecognized values are `Off`, never a silently different effect.
inline Mode ParseMode(const char* value) {
    if (!value || !*value)
        return Mode::Off;
    if (StringsEqualNoCase(value, "cas"))
        return Mode::Cas;
    if (StringsEqualNoCase(value, "rcas"))
        return Mode::Rcas;
    return Mode::Off;
}

inline const char* ModeName(Mode mode) {
    switch (mode) {
        case Mode::Cas:
            return "cas";
        case Mode::Rcas:
            return "rcas";
        case Mode::Off:
        default:
            return "off";
    }
}

inline ConfiguredSpace ParseConfiguredSpace(const char* value) {
    if (!value || !*value)
        return ConfiguredSpace::Auto;
    if (StringsEqualNoCase(value, "direct"))
        return ConfiguredSpace::Direct;
    if (StringsEqualNoCase(value, "gamma"))
        return ConfiguredSpace::Gamma;
    return ConfiguredSpace::Auto;
}

inline const char* ConfiguredSpaceName(ConfiguredSpace space) {
    switch (space) {
        case ConfiguredSpace::Direct:
            return "direct";
        case ConfiguredSpace::Gamma:
            return "gamma";
        case ConfiguredSpace::Auto:
        default:
            return "auto";
    }
}

// What the shader actually loads, which is not the same question as how the
// frame is presented. An _SRGB view decodes on read and re-encodes on write, so
// an 8-bit sRGB backbuffer reaches the shader as linear light exactly like
// scRGB does. Filtering linear values rings around highlights either way.
inline bool ValuesReachShaderAsLinear(TargetEncoding encoding, bool viewAppliesSrgbConversion) {
    return encoding == TargetEncoding::ScrgbLinear || encoding == TargetEncoding::SdrLinear ||
           viewAppliesSrgbConversion;
}

// Only linear values need perceptual transformation; stored sRGB, plain UNORM and PQ
// are already perceptual enough for a contrast-adaptive kernel to behave.
// scRGB linear light is mapped via ST 2084 PQ to strictly fit in [0, 1] without
// crushing HDR highlights, while SDR linear values use the standard gamma round trip.
inline FilterSpace ResolveFilterSpace(TargetEncoding encoding, bool viewAppliesSrgbConversion,
                                      ConfiguredSpace configured) {
    switch (configured) {
        case ConfiguredSpace::Direct:
            return FilterSpace::Direct;
        case ConfiguredSpace::Gamma:
            return encoding == TargetEncoding::ScrgbLinear ? FilterSpace::ScrgbToPq
                                                           : FilterSpace::LinearToGamma;
        case ConfiguredSpace::Auto:
        default:
            break;
    }
    if (encoding == TargetEncoding::ScrgbLinear)
        return FilterSpace::ScrgbToPq;
    return ValuesReachShaderAsLinear(encoding, viewAppliesSrgbConversion)
               ? FilterSpace::LinearToGamma : FilterSpace::Direct;
}

inline FilterSpace ResolveFilterSpace(bool valuesAreLinear, ConfiguredSpace configured) {
    switch (configured) {
        case ConfiguredSpace::Direct:
            return FilterSpace::Direct;
        case ConfiguredSpace::Gamma:
            return FilterSpace::LinearToGamma;
        case ConfiguredSpace::Auto:
        default:
            break;
    }
    return valuesAreLinear ? FilterSpace::LinearToGamma : FilterSpace::Direct;
}

// A route that carries an actual frame. The UI-resource route does not, and an
// unclassified route is refused rather than guessed at.
inline bool RouteCarriesFrame(Route route) {
    switch (route) {
        case Route::NormalBackbuffer:
        case Route::OffscreenCopy:
        case Route::PostStreamline:
        case Route::InterposerOutputChain:
        case Route::NativeFgOutput:
            return true;
        case Route::RuntimeUiResource:
        case Route::Unknown:
        default:
            return false;
    }
}

struct Request {
    Mode mode = Mode::Off;
    // The effect's own contrast-adaptation parameter (sharpen_contrast / sharpen_strength).
    float strength = kDefaultStrength;
    // How much of the filtered result reaches the frame (sharpen_amount / sharpen_intensity).
    float intensity = kDefaultIntensity;
    ConfiguredSpace space = ConfiguredSpace::Auto;
    ce::gamma::Request gamma;
};

inline bool Requested(const Request& request) {
    return (request.mode != Mode::Off && ClampIntensity(request.intensity) > 0.0f) ||
           ce::gamma::Requested(request.gamma);
}

inline bool IsHdrEncoding(TargetEncoding encoding) {
    return encoding == TargetEncoding::Pq || encoding == TargetEncoding::ScrgbLinear;
}

// Sharpening configured at an intensity that changes pixels.
inline bool SharpenActive(const Request& request) {
    return request.mode != Mode::Off && ClampIntensity(request.intensity) > kMinIntensity;
}

// Whether the request leaves any work for a target of this encoding: sharpening, or a gamma curve on an SDR
// target. An HDR target keeps its transfer function, so a gamma-only request is a deliberate no-op there
// (Decide's `gamma_hdr_passthrough`), not a pass that failed; callers account such frames as not requested.
inline bool RequestHasWork(const Request& request, TargetEncoding encoding) {
    return SharpenActive(request) || (ce::gamma::Requested(request.gamma) && !IsHdrEncoding(encoding));
}

struct Target {
    Route route = Route::Unknown;
    TargetEncoding encoding = TargetEncoding::Unknown;
    uint32_t width = 0;
    uint32_t height = 0;
    // The source and destination views carry an _SRGB format, so the hardware
    // decodes on every load and re-encodes on every store.
    bool viewAppliesSrgbConversion = false;
    bool sourceRaw = false;
    // The renderer proved it can both read the frame and write the target this
    // present. Anything unproven fails closed instead of guessing.
    bool readable = false;
    bool writable = false;
    // Integer output quantization; zero denotes floating-point output.
    uint32_t colorBits = 0;
};

struct Decision {
    bool run = false;
    Mode mode = Mode::Off;
    ce::gamma::Request gamma;
    bool gammaValuesLinear = false;
    bool gammaOutputLinear = false;
    float gammaDitherScale = 0.0f;
    FilterSpace filterSpace = FilterSpace::Direct;
    // CAS sharpness or RCAS attenuation, already in the effect's own units.
    float effectParameter = 0.0f;
    // Weight of the filtered result against the original pixels.
    float intensity = kDefaultIntensity;
    // Stable identifier for logs; never null.
    const char* reason = "";
};

// The single gate every backend calls. Ordered so the most specific refusal
// wins, because the reason string is what a session log has to explain.
inline Decision Decide(const Request& request, const Target& target) {
    Decision decision;
    decision.filterSpace = ResolveFilterSpace(
        target.encoding, target.viewAppliesSrgbConversion && !target.sourceRaw, request.space);

    decision.intensity = ClampIntensity(request.intensity);

    const bool sharpen = SharpenActive(request);
    const bool gammaRequested = ce::gamma::Requested(request.gamma);
    const bool hdr = IsHdrEncoding(target.encoding);
    if (gammaRequested && !hdr) {
        decision.gamma = request.gamma;
        decision.gammaValuesLinear = (target.viewAppliesSrgbConversion && !target.sourceRaw) || target.encoding == TargetEncoding::SdrLinear;
        decision.gammaOutputLinear = target.viewAppliesSrgbConversion || target.encoding == TargetEncoding::SdrLinear;
        decision.gammaDitherScale = target.colorBits == 8 ? 1.0f / 255.0f
                                    : target.colorBits == 10 ? 1.0f / 1023.0f : 0.0f;
    }
    if (!sharpen && !ce::gamma::Requested(decision.gamma)) {
        decision.reason = gammaRequested && hdr ? "gamma_hdr_passthrough"
                          : request.mode != Mode::Off ? "zero_intensity" : "disabled";
        return decision;
    }
    decision.mode = sharpen ? request.mode : Mode::Off;
    if (target.route == Route::RuntimeUiResource) {
        decision.reason = "ui_resource_route_carries_no_frame";
        return decision;
    }
    if (!RouteCarriesFrame(target.route)) {
        decision.reason = "route_unclassified";
        return decision;
    }
    if (target.encoding == TargetEncoding::Unknown) {
        decision.reason = "unknown_presentation_encoding";
        return decision;
    }
    if (target.width < kMinTargetExtent || target.height < kMinTargetExtent) {
        decision.reason = "target_too_small";
        return decision;
    }
    if (!target.readable) {
        decision.reason = "frame_not_readable";
        return decision;
    }
    if (!target.writable) {
        decision.reason = "target_not_writable";
        return decision;
    }

    decision.run = true;
    decision.effectParameter = decision.mode == Mode::Cas ? CasSharpnessFromStrength(request.strength)
                                                         : RcasAttenuationFromStrength(request.strength);
    decision.reason = decision.mode == Mode::Off ? "gamma"
                      : ce::gamma::Requested(decision.gamma) ? (decision.mode == Mode::Cas ? "cas_gamma" : "rcas_gamma")
                      : decision.mode == Mode::Cas ? "cas" : "rcas";
    return decision;
}

}  // namespace ce::sharpen
