#pragma once

// The widest sampler mip bias seen so far, most negative and most positive,
// tracked lock-free so a sampler-creation hook can feed it on every call.
// Observe() reports whether the value widened the range, which is what lets a
// caller log the range only when it changes rather than once per sampler.

#include <atomic>
#include <limits>

#include "common/platform/strict_float_parse.h"

namespace ce::mip_bias {

class BiasRange {
public:
    // A non-finite bias is no evidence of any range and is ignored.
    bool Observe(float bias) {
        if (!ce::IsFiniteFloatBits(bias)) {
            return false;
        }
        bool widened = false;
        float currentMin = min_.load(std::memory_order_relaxed);
        while (bias < currentMin) {
            if (min_.compare_exchange_weak(currentMin, bias, std::memory_order_relaxed)) {
                widened = true;
                break;
            }
        }
        float currentMax = max_.load(std::memory_order_relaxed);
        while (bias > currentMax) {
            if (max_.compare_exchange_weak(currentMax, bias, std::memory_order_relaxed)) {
                widened = true;
                break;
            }
        }
        return widened;
    }

    bool HasValue() const {
        return Min() <= Max();
    }
    float Min() const {
        return min_.load(std::memory_order_relaxed);
    }
    float Max() const {
        return max_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<float> min_{std::numeric_limits<float>::infinity()};
    std::atomic<float> max_{-std::numeric_limits<float>::infinity()};
};

}  // namespace ce::mip_bias
