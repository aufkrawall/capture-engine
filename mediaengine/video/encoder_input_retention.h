#pragma once

// Keeps an encoder's input frame referenced until the encoder returned its
// packet, for encoder wrappers that release the frame on submit.
//
// FFmpeg's Media Foundation wrapper (mfenc.c) wraps the D3D11 pool texture in
// an IMFSample, hands it to the (asynchronous) hardware MFT and unreferences
// the AVFrame right away. The texture then returns to the FFmpeg pool while the
// MFT may not have read it yet, and CE's next conversion can overwrite it, so
// the encoded frame shows later content. An encoder cannot emit a frame's
// packet before reading that frame, so the packet with the frame's pts is the
// earliest point the pool may recycle its texture. NVENC, AMF and Quick Sync
// keep their own reference until then and do not need this.

#include <cstddef>
#include <cstdint>
#include <deque>

struct AVFrame;

namespace ce::media {

// Media Foundation only (see above).
bool EncoderReleasesInputOnSubmit(const char* encoderName);

class EncoderInputRetention {
public:
    // Bound for a pts that never comes back (dropped frame): the oldest entry
    // is released beyond it, which only returns that texture to the pool.
    static constexpr size_t kMaxRetainedFrames = 64;

    EncoderInputRetention() = default;
    ~EncoderInputRetention();
    EncoderInputRetention(const EncoderInputRetention&) = delete;
    EncoderInputRetention& operator=(const EncoderInputRetention&) = delete;

    // Takes a new reference to `frame`. Returns how many older entries were
    // released to stay within kMaxRetainedFrames.
    size_t Retain(const AVFrame* frame);
    // Releases the entry submitted with `pts`; false when none is retained.
    bool ReleaseForPacket(int64_t pts);
    size_t ReleaseAll();
    size_t Size() const { return frames_.size(); }

private:
    std::deque<AVFrame*> frames_;
};

}  // namespace ce::media
