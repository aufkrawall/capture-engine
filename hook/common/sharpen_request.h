#pragma once

#include "../../common/config.h"
#include "../../common/sharpen_policy.h"

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
    return request;
}

}  // namespace ce::sharpen
