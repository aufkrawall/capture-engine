#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ce::audio {
// One timeline stage owns each instance. Progress survives short packets/pulls;
// ingest corrections must never consume the output stage's recovery fade.
class PacketBoundaryFade {
public:
    void Arm(int samples) { total_ = remaining_ = std::max(0, samples); }
    int Remaining() const { return remaining_; }
    size_t Apply(float* data, size_t frames, size_t channels) {
        if (!data || !channels || !remaining_) return 0;
        const size_t count = std::min(frames, static_cast<size_t>(remaining_));
        const int done = total_ - remaining_;
        for (size_t frame = 0; frame < count; ++frame) {
            const float phase = static_cast<float>(done + frame + 1) / static_cast<float>(total_);
            const float gain = 0.5f * (1.0f - std::cos(3.14159265358979323846f * phase));
            for (size_t channel = 0; channel < channels; ++channel)
                data[frame * channels + channel] *= gain;
        }
        remaining_ -= static_cast<int>(count);
        return count;
    }
private:
    int total_ = 0, remaining_ = 0;
};
}  // namespace ce::audio
