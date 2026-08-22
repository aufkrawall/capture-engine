#include "config_load_internal.h"

#include "../hook/common/ue5_cvar_override_policy.h"

#include <bit>

static_assert(UE5_CVAR_OVERRIDE_CAPACITY == ce::ue5_cvar::kCustomCVarOverrideCapacity,
              "shared custom-CVar storage must match the hook policy capacity");

namespace {

std::string NormalizeUE5Value(ConfigReader& reader, const char* key) {
    std::string value = reader.GetStr("UE5", key, "default");
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::replace(value.begin(), value.end(), ',', '.');
    return value;
}

// The tri-state UE5 switches all share one shape: default leaves the engine's own
// value in place, on/off write the documented CVar value in that direction. An
// unparsable value is reported and left at default rather than guessing a
// direction, because both directions are visible changes to the rendered frame.
int ParseUE5Toggle(ConfigReader& reader, const char* key) {
    const std::string value = NormalizeUE5Value(reader, key);
    if (value == "default")
        return -1;
    if (value == "on" || value == "true" || value == "1" || value == "yes" || value == "enabled")
        return 1;
    if (value == "off" || value == "false" || value == "0" || value == "no" || value == "disabled")
        return 0;
    LogInvalidConfigBoundary("UE5", key, value, "default");
    return -1;
}

// Bounded numeric UE5 settings. `disabled` is the value that means "leave the
// engine alone", and it is also what an out-of-range or unparsable entry falls
// back to after being reported.
float ParseUE5Float(ConfigReader& reader, const char* key, float minimum, float maximum,
                    float disabled) {
    const std::string value = NormalizeUE5Value(reader, key);
    if (value == "default")
        return disabled;
    float parsed = 0.0f;
    if (!ce::TryParseFiniteFloat(value, parsed) || parsed < minimum || parsed > maximum) {
        LogInvalidConfigBoundary("UE5", key, value, "default");
        return disabled;
    }
    return parsed;
}

int ParseUE5Int(ConfigReader& reader, const char* key, int minimum, int maximum, int disabled) {
    const std::string value = NormalizeUE5Value(reader, key);
    if (value == "default")
        return disabled;
    int parsed = 0;
    if (!TryParseInt(value, parsed) || parsed < minimum || parsed > maximum) {
        LogInvalidConfigBoundary("UE5", key, value, "default");
        return disabled;
    }
    return parsed;
}

uint8_t ParseRayReconstructionSettingsPreset(ConfigReader& reader) {
    const std::string value = NormalizeUE5Value(reader, "ray_reconstruction_optimal_settings");
    if (value == "default" || value == "off" || value == "false" || value == "0" || value == "no" ||
        value == "disabled")
        return ce::ue5_cvar::kRayReconstructionPresetOff;
    if (value == "light")
        return ce::ue5_cvar::kRayReconstructionPresetLight;
    if (value == "medium")
        return ce::ue5_cvar::kRayReconstructionPresetMedium;
    if (value == "full" || value == "on" || value == "true" || value == "1" || value == "yes" ||
        value == "enabled")
        return ce::ue5_cvar::kRayReconstructionPresetFull;
    LogInvalidConfigBoundary("UE5", "ray_reconstruction_optimal_settings", value, "off");
    return ce::ue5_cvar::kRayReconstructionPresetOff;
}

void ParseCustomCVarOverrides(ConfigReader& reader, AppConfig& config) {
    config.graphics.ue5CustomCVarOverrideMask = 0;
    config.graphics.ue5CustomCVarOverrideValues.fill(0);
    const std::string list = reader.GetStr("UE5", "custom_cvar_overrides", "off");
    if (list.empty() || ce::ue5_cvar::EqualsIgnoreCase(list, "off") ||
        ce::ue5_cvar::EqualsIgnoreCase(list, "default"))
        return;

    std::size_t begin = 0;
    while (begin <= list.size()) {
        const std::size_t comma = list.find(',', begin);
        const std::string entry = Trim(list.substr(begin, comma == std::string::npos ? comma : comma - begin));
        const std::size_t equals = entry.find('=');
        const std::string name = equals == std::string::npos ? std::string{} : Trim(entry.substr(0, equals));
        const std::string value = equals == std::string::npos ? std::string{} : Trim(entry.substr(equals + 1));
        const std::size_t specIndex = ce::ue5_cvar::FindSpecIndex(name);
        bool valid = !name.empty() && !value.empty() && specIndex < ce::ue5_cvar::kSpecs.size();
        uint32_t bits = 0;
        if (valid) {
            const auto& spec = ce::ue5_cvar::kSpecs[specIndex];
            if (spec.type == ce::ue5_cvar::ValueType::Int32) {
                int parsed = 0;
                valid = TryParseInt(value, parsed);
                bits = static_cast<uint32_t>(static_cast<int32_t>(parsed));
            } else {
                float parsed = 0.0f;
                valid = ce::TryParseFiniteFloat(value, parsed);
                bits = std::bit_cast<uint32_t>(parsed);
            }
        }
        if (valid) {
            config.graphics.ue5CustomCVarOverrideMask |= uint64_t{1} << specIndex;
            config.graphics.ue5CustomCVarOverrideValues[specIndex] = bits;
        } else if (!entry.empty()) {
            LogInvalidConfigBoundary("UE5", "custom_cvar_overrides", entry, "entry ignored");
        }
        if (comma == std::string::npos)
            break;
        begin = comma + 1;
    }
}

}  // namespace

void LoadUE5Settings(ConfigReader& reader, AppConfig& config) {
    config.graphics.forceRayReconstruction = reader.GetBoolCompat2(
        "UE5", "force_ray_reconstruction", "DLSS", "force_ray_reconstruction", "Graphics",
        "force_ray_reconstruction", false);
    config.graphics.rayReconstructionOptimalSettings = ParseRayReconstructionSettingsPreset(reader);
    config.graphics.disablePostProcessingEffects =
        reader.GetBool("UE5", "disable_post_processing_effects", false);
    std::string tonemapperSharpen = reader.GetStr("UE5", "tonemapper_sharpen", "default");
    std::transform(tonemapperSharpen.begin(), tonemapperSharpen.end(), tonemapperSharpen.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::replace(tonemapperSharpen.begin(), tonemapperSharpen.end(), ',', '.');
    config.graphics.tonemapperSharpen = -1.0f;
    if (tonemapperSharpen != "default") {
        float parsedSharpen = 0.0f;
        if (!ce::TryParseFiniteFloat(tonemapperSharpen, parsedSharpen) || parsedSharpen < 0.0f ||
            parsedSharpen > 10.0f) {
            LogInvalidConfigBoundary("UE5", "tonemapper_sharpen", tonemapperSharpen, "default");
        } else {
            config.graphics.tonemapperSharpen = parsedSharpen;
        }
    }
    std::string internalFpsLimit = reader.GetStr("UE5", "internal_fps_limit", "default");
    std::transform(internalFpsLimit.begin(), internalFpsLimit.end(), internalFpsLimit.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::replace(internalFpsLimit.begin(), internalFpsLimit.end(), ',', '.');
    config.graphics.internalFpsLimit = -1.0f;
    if (internalFpsLimit != "default") {
        float parsedFpsLimit = 0.0f;
        if (internalFpsLimit == "off" || internalFpsLimit == "0") {
            config.graphics.internalFpsLimit = 0.0f;
        } else if (!ce::TryParseFiniteFloat(internalFpsLimit, parsedFpsLimit) || parsedFpsLimit <= 0.0f ||
                   parsedFpsLimit > 1000.0f) {
            LogInvalidConfigBoundary("UE5", "internal_fps_limit", internalFpsLimit, "default");
        } else {
            config.graphics.internalFpsLimit = parsedFpsLimit;
        }
    }
    std::string internalAnisotropicFiltering =
        reader.GetStr("UE5", "internal_anisotropic_filtering", "default");
    std::transform(internalAnisotropicFiltering.begin(), internalAnisotropicFiltering.end(),
                   internalAnisotropicFiltering.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    config.graphics.internalAnisotropicFiltering = 0;
    if (internalAnisotropicFiltering != "default") {
        int afLevel = 0;
        if (internalAnisotropicFiltering == "off" || internalAnisotropicFiltering == "1x") {
            afLevel = 1;
        } else if (internalAnisotropicFiltering == "2x") {
            afLevel = 2;
        } else if (internalAnisotropicFiltering == "4x") {
            afLevel = 4;
        } else if (internalAnisotropicFiltering == "8x") {
            afLevel = 8;
        } else if (internalAnisotropicFiltering == "16x") {
            afLevel = 16;
        } else {
            LogInvalidConfigBoundary("UE5", "internal_anisotropic_filtering",
                                     internalAnisotropicFiltering, "default");
        }
        config.graphics.internalAnisotropicFiltering = afLevel;
    }

    // UE's own texture mip bias. Unlike every other numeric UE5 knob here, 0 is a
    // real value ("no bias") rather than "off", so the untouched state is a
    // sentinel outside UE's accepted range instead of zero or a negative value.
    std::string internalTextureMipBias =
        reader.GetStr("UE5", "internal_texture_mip_bias", "default");
    std::transform(internalTextureMipBias.begin(), internalTextureMipBias.end(),
                   internalTextureMipBias.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::replace(internalTextureMipBias.begin(), internalTextureMipBias.end(), ',', '.');
    config.graphics.internalTextureMipBias = kUE5TextureMipBiasDisabled;
    if (internalTextureMipBias != "default") {
        float parsedMipBias = 0.0f;
        if (!ce::TryParseFiniteFloat(internalTextureMipBias, parsedMipBias) ||
            !IsUE5TextureMipBiasRequested(parsedMipBias)) {
            LogInvalidConfigBoundary("UE5", "internal_texture_mip_bias", internalTextureMipBias,
                                     "default");
        } else {
            config.graphics.internalTextureMipBias = parsedMipBias;
        }
    }

    // Display gamma transform. srgb selects UE's piecewise sRGB/Rec709 curve,
    // a number selects a pure power curve of that exponent. Carried as the
    // r.TonemapperGamma value itself, so srgb is literally 0 ("default
    // behavior") and negative means untouched.
    std::string displayGamma = reader.GetStr("UE5", "display_gamma", "default");
    std::transform(displayGamma.begin(), displayGamma.end(), displayGamma.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::replace(displayGamma.begin(), displayGamma.end(), ',', '.');
    config.graphics.displayGamma = -1.0f;
    if (displayGamma != "default") {
        float parsedGamma = 0.0f;
        if (displayGamma == "srgb" || displayGamma == "piecewise") {
            config.graphics.displayGamma = 0.0f;
        } else if (!ce::TryParseFiniteFloat(displayGamma, parsedGamma) || parsedGamma < 1.0f ||
                   parsedGamma > 3.0f) {
            LogInvalidConfigBoundary("UE5", "display_gamma", displayGamma, "default");
        } else {
            config.graphics.displayGamma = parsedGamma;
        }
    }

    // Depth of field. UE's own r.DepthOfFieldQuality documents 0 as "Off"; on
    // restores the engine default quality, which is all a CVar can do - the blur
    // itself still comes from the game's post-process settings.
    config.graphics.depthOfField = ParseUE5Toggle(reader, "depth_of_field");

    // DLSS Super Resolution. off is the NVIDIA plugin's own r.NGX.DLSS.Enable=0;
    // on additionally routes the renderer through the third-party temporal
    // upscaler, which is what a game that never exposed DLSS does not do by
    // itself. The quality mode is expressed as UE's screen percentage because
    // that is what the plugin resolves into its quality modes.
    config.graphics.dlssSuperResolution = ParseUE5Toggle(reader, "dlss_super_resolution");
    const std::string dlssQuality = NormalizeUE5Value(reader, "dlss_super_resolution_quality");
    config.graphics.dlssScreenPercentage = 0.0f;
    if (dlssQuality != "default") {
        float screenPercentage = 0.0f;
        if (dlssQuality == "dlaa" || dlssQuality == "native") {
            screenPercentage = 100.0f;
        } else if (dlssQuality == "ultra_quality" || dlssQuality == "ultraquality") {
            screenPercentage = 76.99f;
        } else if (dlssQuality == "quality") {
            screenPercentage = 66.67f;
        } else if (dlssQuality == "balanced") {
            screenPercentage = 58.0f;
        } else if (dlssQuality == "performance") {
            screenPercentage = 50.0f;
        } else if (dlssQuality == "ultra_performance" || dlssQuality == "ultraperformance") {
            screenPercentage = 33.33f;
        } else if (!ce::TryParseFiniteFloat(dlssQuality, screenPercentage) ||
                   screenPercentage < kUE5DlssScreenPercentageMin ||
                   screenPercentage > kUE5DlssScreenPercentageMax) {
            LogInvalidConfigBoundary("UE5", "dlss_super_resolution_quality", dlssQuality, "default");
            screenPercentage = 0.0f;
        }
        config.graphics.dlssScreenPercentage = screenPercentage;
    }

    // HDR. Every value here is in the unit the engine's own help text names, so
    // the black floor is configured in nits and converted to the log10 level the
    // CVar stores on the hook side.
    config.graphics.hdrOutput = ParseUE5Toggle(reader, "hdr_output");
    config.graphics.hdrPeakLuminance = ParseUE5Int(reader, "hdr_peak_luminance", kUE5HdrPeakLuminanceMin,
                                                   kUE5HdrPeakLuminanceMax, 0);
    config.graphics.hdrPaperWhite =
        ParseUE5Float(reader, "hdr_paper_white", kUE5HdrPaperWhiteMin, kUE5HdrPaperWhiteMax, 0.0f);
    config.graphics.hdrUiLuminance =
        ParseUE5Float(reader, "hdr_ui_luminance", kUE5HdrUiLuminanceMin, kUE5HdrUiLuminanceMax, 0.0f);
    config.graphics.hdrMinLuminance =
        ParseUE5Float(reader, "hdr_min_luminance", kUE5HdrMinLuminanceMin, kUE5HdrMinLuminanceMax, 0.0f);
    const std::string hdrColorGamut = NormalizeUE5Value(reader, "hdr_color_gamut");
    config.graphics.hdrColorGamut = -1;
    if (hdrColorGamut != "default") {
        if (hdrColorGamut == "rec709" || hdrColorGamut == "srgb") {
            config.graphics.hdrColorGamut = 0;
        } else if (hdrColorGamut == "dcip3" || hdrColorGamut == "dci-p3" || hdrColorGamut == "p3") {
            config.graphics.hdrColorGamut = 1;
        } else if (hdrColorGamut == "rec2020" || hdrColorGamut == "bt2020") {
            config.graphics.hdrColorGamut = 2;
        } else if (hdrColorGamut == "aces") {
            config.graphics.hdrColorGamut = 3;
        } else if (hdrColorGamut == "acescg") {
            config.graphics.hdrColorGamut = 4;
        } else {
            LogInvalidConfigBoundary("UE5", "hdr_color_gamut", hdrColorGamut, "default");
        }
    }
    ParseCustomCVarOverrides(reader, config);
}
