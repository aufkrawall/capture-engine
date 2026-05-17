#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <vector>
#include "../common/config.h"
#include "audio_resampler.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
}

class AudioEncoder {
public:
    AudioEncoder();
    ~AudioEncoder();

    bool Init(const AudioConfig& config, std::function<void(AVPacket*)> packetCallback);

    AVCodecContext* GetCodecContext() const {
        return codecCtx;
    }
    bool IsReady() const {
        return initDone && codecCtx != nullptr;
    }

    // Reinitialize with saved config (used after Stop() failed to reopen)
    bool Reinit() {
        if (!savedConfig.codec.empty() && onPacket) {
            return Init(savedConfig, onPacket);
        }
        return false;
    }

    // Provide raw PCM data (supports S16, S24, S32, Float from WASAPI)
    // validBitsPerSample: 0 means same as bitsPerSample (from
    // WAVEFORMATEXTENSIBLE) blockAlign: bytes per frame
    void EncodeSamples(const uint8_t* data, int sizeBytes, int channels, int sampleRate, int bitsPerSample,
                       int validBitsPerSample, int blockAlign, bool isFloat, int64_t timestamp);

    void SetStreamIndex(int index);  // Now flushes buffered packets

    // SetRecordingStart - NOTE: logging happens in EncodeSamples to avoid header
    // deps
    void SetRecordingStart(int64_t startUs) {
        pendingStartUs = startUs;  // Will be applied and logged in EncodeSamples
        needsReset = true;
    }
    void SetRecordingEndUs(int64_t endUs) {
        recordingEndUs = endUs;
    }

    // Set callback to get current video elapsed time for clock drift compensation
    using VideoTimeGetter = std::function<int64_t()>;
    void SetVideoTimeGetter(VideoTimeGetter getter) {
        videoTimeGetter = std::move(getter);
    }

    int64_t GetSamplesCount() const { return samplesCount; }
    int GetStreamIndex() const { return streamIndex; }
    void Stop();

private:
    std::function<void(AVPacket*)> onPacket;
    AVCodecContext* codecCtx;
    std::unique_ptr<AudioResampler> resampler;

    // Track current input format to detect changes
    AudioResampler::InputFormat currentInputFormat;

    // FIFO buffer for audio samples
    AVAudioFifo* audioFifo;

    // Frame for encoding
    AVFrame* frame;
    int64_t samplesCount;
    int streamIndex;  // Stream index in the muxer

    bool initDone;
    AVCodecID savedCodecId;                  // Store codec ID for recreation between recordings
    std::string savedCodecName;              // Store codec name to ensure same encoder is found
    AudioConfig savedConfig;                 // Store full config for reinit
    int64_t firstTimestamp;                  // Timestamp of first audio packet (ms) - for sync
    int64_t recordingStartUs;                // When recording started (us video pts)
    int64_t recordingEndUs;                  // When video ended (us video pts)
    int64_t lastPacketTimestampMs;           // Last packet timestamp (ms) for PTS sync with
                                             // video
    std::atomic<int64_t> pendingStartUs{0};  // Deferred recording start (set by SetRecordingStart)
    std::atomic<bool> needsReset{false};     // Flag for deferred reset
    int fifoCapacity = 32768;                // Capacity set during Init()
    int64_t resampledSamplesTotal = 0;       // Total samples output from resampler (for drift calculation)

    // Callback to get video elapsed time for clock drift compensation
    VideoTimeGetter videoTimeGetter;

    // Continuity tracking for Backlog backlog detection
    int64_t lastInputTimestamp = -1;

    // Buffer for packets before streamIndex is set.
    // Uses deque for O(1) removal from the front when the buffer overflows.
    std::deque<AVPacket*> pendingPackets;
    std::deque<int64_t> pendingFrameDurations;

    // Per-recording warning flags (reset in Stop() for multi-recording support)
    bool warnedOnce = false;  // "stream not yet assigned" warning gate
    bool warnedMax = false;   // "pending buffer full" warning gate

    int fifoLogCounter = 0;
    int frameLogCounter = 0;
    int noPacketCount = 0;

    // FIFO overflow tracking - drop NEWEST samples to maintain timeline continuity
    bool wasDroppingSamples = false;
    int64_t totalDroppedSamples = 0;

    void ApplyPacketDuration(AVPacket* pkt);
    void Flush();
};
