#pragma once

#include <stddef.h>
#include <stdint.h>
#include <array>

// Encoded inject-frame lineage checks: the producer's frame counter must only
// grow, and a shared texture slot must only carry newer frames, within one
// shared transport generation.

namespace ce::capture_policy {

struct InjectLineageObservation {
    // The frame opened a new transport generation; all earlier lineage was
    // discarded before the checks below ran.
    bool generationReset = false;
    uint32_t previousGeneration = 0;
    // Last frame index of the discarded generation (0 when none was encoded).
    uint32_t previousGenerationLastFrame = 0;
    // Generation resets seen by this tracker, including this one.
    uint32_t generationResetCount = 0;

    bool lineageRegression = false;
    uint32_t previousFrameIndex = 0;

    bool textureReuse = false;
    uint32_t previousTextureFrame = 0;
};

inline bool ShouldLogInjectLineageGenerationReset(uint32_t generationResetCount) {
    // Swapchain re-creations are rare; a producer flapping between generations
    // must not flood the log, so only the first resets and then a sparse sample.
    return generationResetCount != 0 && (generationResetCount <= 16 || generationResetCount % 256 == 0);
}

// A producer that re-creates its swapchain republishes a new texture set and
// fence under a new transport generation and restarts its frame counter at 1.
// Comparing the new generation's frames against the old one's counters reports
// a lineage regression and one texture-slot "reuse" per slot, although nothing
// was reused: the old textures were already dropped. Lineage is therefore
// scoped to the generation that produced it.
template <size_t kTextureSlotCount>
class InjectLineageTracker {
public:
    InjectLineageObservation Observe(uint32_t transportGeneration, uint32_t frameIndex, int32_t textureIndex) {
        InjectLineageObservation observation;
        const bool textureValid = textureIndex >= 0 && static_cast<size_t>(textureIndex) < kTextureSlotCount;
        if (frameIndex == 0 && !textureValid) {
            return observation;  // not an inject-lineage frame
        }

        if (hasGeneration_ && transportGeneration != generation_) {
            observation.generationReset = true;
            observation.previousGeneration = generation_;
            observation.previousGenerationLastFrame = lastFrameIndex_;
            observation.generationResetCount = ++generationResetCount_;
            lastFrameIndex_ = 0;
            lastFrameByTexture_ = {};
        }
        hasGeneration_ = true;
        generation_ = transportGeneration;

        if (frameIndex != 0) {
            if (lastFrameIndex_ != 0 && frameIndex < lastFrameIndex_) {
                observation.lineageRegression = true;
                observation.previousFrameIndex = lastFrameIndex_;
            }
            lastFrameIndex_ = frameIndex;
        }
        if (textureValid) {
            uint32_t& lastTextureFrame = lastFrameByTexture_[static_cast<size_t>(textureIndex)];
            if (lastTextureFrame != 0 && frameIndex != 0 && frameIndex <= lastTextureFrame) {
                observation.textureReuse = true;
                observation.previousTextureFrame = lastTextureFrame;
            }
            lastTextureFrame = frameIndex;
        }
        return observation;
    }

    void Reset() {
        *this = InjectLineageTracker{};
    }

    uint32_t LastFrameIndex() const {
        return lastFrameIndex_;
    }

private:
    bool hasGeneration_ = false;
    uint32_t generation_ = 0;
    uint32_t lastFrameIndex_ = 0;
    uint32_t generationResetCount_ = 0;
    std::array<uint32_t, kTextureSlotCount> lastFrameByTexture_{};
};

}  // namespace ce::capture_policy
