#pragma once

// Taking a frame from the encoder's hardware frames pool.
//
// av_hwframe_get_buffer stores a new frames-context reference in frame->hw_frames_ctx and overwrites whatever is
// there without releasing it. A reference set before the call is therefore lost: the frames context never reaches a
// zero count, so its pool textures and the D3D11 device they belong to outlive the recording. A media process hid it
// by exiting; on a runtime thread every recording left its encoder device behind (about 22 driver threads and
// ~245 MiB VRAM each, SDK run 2026-10-21) and every encoded frame leaked one reference.

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

namespace ce::media {

// Fills `frame` from `framesCtx`'s pool; the frame then holds exactly the one frames-context reference FFmpeg adds.
// Returns av_hwframe_get_buffer's result.
inline int GetEncoderPoolFrame(AVBufferRef* framesCtx, AVFrame* frame) {
    av_buffer_unref(&frame->hw_frames_ctx);
    return av_hwframe_get_buffer(framesCtx, frame, 0);
}

}  // namespace ce::media
