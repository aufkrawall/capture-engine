#include "mediaengine/engine/av_log_open_capture.h"

namespace ce::media {
namespace {

thread_local ScopedAvOpenDebugCapture* t_activeCapture = nullptr;

}  // namespace

ScopedAvOpenDebugCapture::ScopedAvOpenDebugCapture(size_t maxLines)
    : previous_(t_activeCapture), maxLines_(maxLines > 0 ? maxLines : 1) {
    lines_.reserve(maxLines_);
    t_activeCapture = this;
}

ScopedAvOpenDebugCapture::~ScopedAvOpenDebugCapture() {
    t_activeCapture = previous_;
}

std::string ScopedAvOpenDebugCapture::LastLineTruncated(size_t maxChars) const {
    if (lines_.empty() || maxChars == 0) {
        return {};
    }
    const std::string& last = lines_.back();
    if (last.size() <= maxChars) {
        return last;
    }
    if (maxChars <= 3) {
        return last.substr(0, maxChars);
    }
    return last.substr(0, maxChars - 3) + "...";
}

bool ScopedAvOpenDebugCapture::HasActiveCapture() {
    return t_activeCapture != nullptr;
}

bool ScopedAvOpenDebugCapture::TryRecordForCurrentThread(const std::string& redactedLine) {
    ScopedAvOpenDebugCapture* active = t_activeCapture;
    if (!active) {
        return false;
    }
    if (active->lines_.size() >= active->maxLines_) {
        // Keep the newest lines so the final error right before encoder->Init() fails is never lost.
        active->lines_.erase(active->lines_.begin());
        ++active->droppedCount_;
    }
    active->lines_.push_back(redactedLine);
    return true;
}

}  // namespace ce::media
