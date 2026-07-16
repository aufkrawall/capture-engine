#pragma once

#include <algorithm>
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
    enum class FinalFramePolicy {
        ExactShortFrame,
        PadAndSignalDiscard,
        BlockAlignedPcm,
    };

    struct AudioCodecRuntimeContract {
        std::string encoderName;
        AVCodecID codecId = AV_CODEC_ID_NONE;
        AVSampleFormat sampleFormat = AV_SAMPLE_FMT_NONE;
        int sampleRate = 0;
        int channels = 0;
        uint32_t channelMask = 0;
        int rawBitDepth = 0;
        int frameSize = 0;
        int capabilities = 0;
        int initialPadding = 0;
        FinalFramePolicy finalFramePolicy = FinalFramePolicy::ExactShortFrame;
        bool requiresMatroskaCodecDelay = false;
        bool requiresMatroskaDiscardPadding = false;
        bool nativeAacNmr = false;
        int nativeAacNmrSpeed = -1;
        bool valid = false;
    };

    struct AudioFinalizationReport {
        int64_t timelineTargetSamples = 0;
        int64_t inputTimelineSamples = 0;
        int64_t expectedSourceSilenceSamples = 0;
        int64_t codecSubmittedSamples = 0;
        int64_t primingSamples = 0;
        int64_t terminalPaddingSamples = 0;
        int64_t packetEndpointSamples = 0;
        int64_t expectedDecodedSamples = 0;
        uint64_t packetCount = 0;
        uint64_t packetBytes = 0;
        uint64_t controlPacketCount = 0;
        uint64_t durationlessPacketCount = 0;
        bool drainReachedEof = false;
        bool protocolError = false;
    };

    struct EncodeResult {
        int64_t acceptedSamples = 0;
        int64_t submittedSamples = 0;
        bool failed = false;
    };

    AudioEncoder();
    ~AudioEncoder();

    bool Init(const AudioConfig& config, std::function<void(AVPacket*)> packetCallback);

    AVCodecContext* GetCodecContext() const {
        return codecCtx;
    }
    bool IsReady() const {
        return initDone && codecCtx != nullptr;
    }
    const AudioCodecRuntimeContract& GetRuntimeContract() const {
        return runtimeContract;
    }
    const AudioFinalizationReport& GetFinalizationReport() const {
        return finalizationReport;
    }
    void SetExpectedSourceSilenceSamples(int64_t samples) {
        finalizationReport.expectedSourceSilenceSamples = std::max<int64_t>(0, samples);
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
    EncodeResult EncodeSamples(const uint8_t* data, int sizeBytes, int channels, int sampleRate, int bitsPerSample,
                               int validBitsPerSample, int blockAlign, bool isFloat, uint32_t channelMask,
                               int64_t timestamp);
    EncodeResult EncodeSamples(const uint8_t* data, int sizeBytes, int channels, int sampleRate, int bitsPerSample,
                               int validBitsPerSample, int blockAlign, bool isFloat, int64_t timestamp);

    void SetStreamIndex(int index);  // Now flushes buffered packets

    // Called by the audio-timeline reset owner while packet routing is gated.
    // The generation makes repeated delivery idempotent and prevents a route
    // reset from becoming visible before its encoder FIFO/cursors are reset.
    bool ResetForRecordingStart(int64_t startUs, uint64_t generation);
    void SetRecordingEndUs(int64_t endUs) {
        recordingEndUs = endUs;
    }

    // Set callback to get current video elapsed time for clock drift compensation
    using VideoTimeGetter = std::function<int64_t()>;
    void SetVideoTimeGetter(VideoTimeGetter getter) {
        videoTimeGetter = std::move(getter);
    }

    int64_t GetSamplesCount() const {
        return samplesCount;
    }
    int GetStreamIndex() const {
        return streamIndex;
    }
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
    AVCodecID savedCodecId;         // Store codec ID for recreation between recordings
    std::string savedCodecName;     // Store codec name to ensure same encoder is found
    AudioConfig savedConfig;        // Store full config for reinit
    int64_t firstTimestamp;         // Timestamp of first audio packet (ms) - for sync
    int64_t recordingStartUs;       // When recording started (us video pts)
    int64_t recordingEndUs;         // When video ended (us video pts)
    int64_t lastPacketTimestampMs;  // Last packet timestamp (ms) for PTS sync with
                                    // video
    uint64_t recordingResetGeneration = 0;
    int fifoCapacity = 32768;           // Capacity set during Init()
    int64_t resampledSamplesTotal = 0;  // Total samples output from resampler (for drift calculation)

    // Callback to get video elapsed time for clock drift compensation
    VideoTimeGetter videoTimeGetter;

    // Continuity tracking for Backlog backlog detection
    int64_t lastInputTimestamp = -1;

    // Buffer for packets before streamIndex is set.
    // Uses deque for O(1) removal from the front when the buffer overflows.
    std::deque<AVPacket*> pendingPackets;

    // Per-recording warning flags (reset in Stop() for multi-recording support)
    bool warnedOnce = false;  // "stream not yet assigned" warning gate
    bool warnedMax = false;   // "pending buffer full" warning gate

    int fifoLogCounter = 0;
    int frameLogCounter = 0;
    int noPacketCount = 0;
    bool allowShortFinalFrame = true;
    int outputChannels = 2;
    uint32_t outputChannelMask = 0;

    // FIFO overflow tracking - drop NEWEST samples to maintain timeline continuity
    bool wasDroppingSamples = false;
    int64_t totalDroppedSamples = 0;
    int64_t totalAcceptedSamples = 0;
    AudioCodecRuntimeContract runtimeContract;
    AudioFinalizationReport finalizationReport;

    void ApplyPacketDuration(AVPacket* pkt);
    void Flush();
    void ReleaseCodecResources();
};
