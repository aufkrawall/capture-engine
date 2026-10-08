#pragma once

#include "common/config/config.h"
#include "common/graphics/sharpen_policy.h"
#include "gamma_native_curve.h"

namespace ce::sharpen {

// One place where the resolved configuration becomes a sharpen request, so the
// D3D11, D3D12 and Vulkan renderers cannot drift into three readings of the
// same settings. Parsing here rather than caching a parsed struct keeps the
// merged GraphicsConfig the single source of truth, and the values are two
// short keyword comparisons.
inline Request ResolveRequest(const GraphicsConfig& graphics) {
    Request request;
    request.mode = ParseMode(graphics.sharpenMode.c_str());
    request.strength = ClampStrength(graphics.sharpenStrength);
    request.intensity = ClampIntensity(graphics.sharpenIntensity);
    request.space = ParseConfiguredSpace(graphics.sharpenColorSpace.c_str());
    ce::gamma::Curve destination = ce::gamma::Curve::Default;
    ce::gamma::Curve source = ce::gamma::Curve::Power22;
    ce::gamma::TryParse(graphics.postProcessDisplayGamma.c_str(), destination);
    ce::gamma::TryParse(graphics.postProcessGammaSource.c_str(), source);
    request.gamma.destination = ce::gamma::Exponent(destination);
    request.gamma.source = ce::gamma::Exponent(source);
    if (destination != ce::gamma::Curve::Default) {
        const float native = ce::gamma::VerifiedNativeCurve(ce::gamma::ValidExponent(graphics.displayGamma) || graphics.ue5CustomCVarOverrideMask != 0);
        if (ce::gamma::ValidExponent(native))
            request.gamma.source = native;
    }
    return request;
}

}  // namespace ce::sharpen
