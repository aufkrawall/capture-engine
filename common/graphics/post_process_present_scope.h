#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

namespace ce::post_process_present {

// Each nested physical Present owns a separate frame. Normal/PostSL visits
// within that Present share one successful-processing marker.
class Scope {
public:
    void Begin(uintptr_t swapchain) {
        if (depth_ < frames_.size())
            frames_[depth_] = {swapchain, false};
        ++depth_;
    }
    void End() {
        if (depth_ > 0)
            --depth_;
    }
    bool Processed(uintptr_t swapchain) const {
        return depth_ > 0 && depth_ <= frames_.size() && frames_[depth_ - 1].swapchain == swapchain &&
               frames_[depth_ - 1].processed;
    }
    void Mark(uintptr_t swapchain) {
        if (depth_ > 0 && depth_ <= frames_.size() && frames_[depth_ - 1].swapchain == swapchain)
            frames_[depth_ - 1].processed = true;
    }
private:
    struct Frame {
        uintptr_t swapchain = 0;
        bool processed = false;
    };
    std::array<Frame, 64> frames_{};
    std::size_t depth_ = 0;
};

}  // namespace ce::post_process_present
