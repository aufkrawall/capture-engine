#pragma once

#include <atomic>
#include "common/graphics/sharpen_policy.h"
#include "hook/sharpen/gamma_native_curve.h"

class VulkanPostProcessSettings {
public:
    void Store(uint8_t mode, uint8_t space, float strength, float intensity,
               uint8_t destination, uint8_t source, bool nativeRequested) {
        mode_.store(mode, std::memory_order_release);
        space_.store(space, std::memory_order_release);
        strength_.store(strength, std::memory_order_release);
        intensity_.store(intensity, std::memory_order_release);
        gamma_.store(uint32_t{destination} | (uint32_t{source} << 8) | (uint32_t{nativeRequested} << 16),
                     std::memory_order_release);
    }
    ce::sharpen::Request Request() const {
        ce::sharpen::Request request;
        request.mode = static_cast<ce::sharpen::Mode>(mode_.load(std::memory_order_acquire));
        request.space = static_cast<ce::sharpen::ConfiguredSpace>(space_.load(std::memory_order_acquire));
        request.strength = ce::sharpen::ClampStrength(strength_.load(std::memory_order_acquire));
        request.intensity = ce::sharpen::ClampIntensity(intensity_.load(std::memory_order_acquire));
        const auto curves = gamma_.load(std::memory_order_acquire);
        request.gamma.destination = ce::gamma::Exponent(static_cast<ce::gamma::Curve>(curves & 255));
        request.gamma.source = ce::gamma::Exponent(static_cast<ce::gamma::Curve>((curves >> 8) & 255));
        const float native = ce::gamma::VerifiedNativeCurve((curves & (1u << 16)) != 0);
        if (request.gamma.destination >= 0.0f && ce::gamma::ValidExponent(native)) request.gamma.source = native;
        return request;
    }
private:
    std::atomic<uint8_t> mode_{0}, space_{0};
    std::atomic<float> strength_{ce::sharpen::kDefaultStrength}, intensity_{ce::sharpen::kDefaultIntensity};
    std::atomic<uint32_t> gamma_{uint32_t{static_cast<uint8_t>(ce::gamma::Curve::Power22)} << 8};
};
