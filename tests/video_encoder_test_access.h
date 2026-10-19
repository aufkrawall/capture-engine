#pragma once

// The one definition of VideoEncoder's test friend: every test unit that needs
// it includes this header, so the friend struct stays ODR-consistent.

#include <atomic>
#include <string>

#include "mediaengine/video/video_encoder.h"

struct VideoEncoderTestAccess {
    static void SetPreviousDeferral(VideoEncoder& encoder) {
        encoder.lastFrameDeferred.store(true, std::memory_order_relaxed);
    }
    // The inject first-frame path: initDone is still false, so Init runs again.
    static bool InitializeFirstInjectFrame(VideoEncoder& encoder, int width, int height) {
        return encoder.ReinitForFormatModeChange(encoder.currentIsHDR, encoder.currentUse10BitInput, width, height);
    }
    static ce::media::AutoEncoderBinding& AutoEncoder(VideoEncoder& encoder) { return encoder.autoEncoder; }
    static const std::string& EncoderName(const VideoEncoder& encoder) { return encoder.savedConfig.encoder; }
    static AVFormatContext* FormatContext(const VideoEncoder& encoder) { return encoder.fmtCtx; }
    static AVCodecContext* CodecContext(const VideoEncoder& encoder) { return encoder.codecCtx; }
    static AVStream* Stream(const VideoEncoder& encoder) { return encoder.stream; }
    static void SetStream(VideoEncoder& encoder, AVStream* stream) { encoder.stream = stream; }
    static void SetInitDone(VideoEncoder& encoder, bool done) { encoder.initDone = done; }
};
