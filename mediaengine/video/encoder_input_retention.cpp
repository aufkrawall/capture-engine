#include "encoder_input_retention.h"

#include <cstring>

extern "C" {
#include <libavutil/frame.h>
}

namespace ce::media {

bool EncoderReleasesInputOnSubmit(const char* encoderName) {
    if (!encoderName)
        return false;
    const size_t length = std::strlen(encoderName);
    return length >= 3 && std::strcmp(encoderName + length - 3, "_mf") == 0;
}

EncoderInputRetention::~EncoderInputRetention() {
    ReleaseAll();
}

size_t EncoderInputRetention::Retain(const AVFrame* frame) {
    if (!frame)
        return 0;
    AVFrame* reference = av_frame_clone(frame);
    if (!reference)
        return 0;
    frames_.push_back(reference);
    size_t released = 0;
    while (frames_.size() > kMaxRetainedFrames) {
        av_frame_free(&frames_.front());
        frames_.pop_front();
        ++released;
    }
    return released;
}

bool EncoderInputRetention::ReleaseForPacket(int64_t pts) {
    for (auto it = frames_.begin(); it != frames_.end(); ++it) {
        if ((*it)->pts == pts) {
            av_frame_free(&*it);
            frames_.erase(it);
            return true;
        }
    }
    return false;
}

size_t EncoderInputRetention::ReleaseAll() {
    const size_t released = frames_.size();
    for (AVFrame*& frame : frames_)
        av_frame_free(&frame);
    frames_.clear();
    return released;
}

}  // namespace ce::media
