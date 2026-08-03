#include "mediaengine.h"
#include "../common/capture_pipeline_policy.h"
#include "../common/logging.h"
#include "../common/path_utils.h"
#include "../common/reserved_capture_output.h"
#include "../common/shared_defs.h"
#include "audio_capture.h"
#include "audio_encoder.h"
#include "audio_latency_probe.h"
#include "matroska_timing.h"
#include "process_loopback_capture.h"

#include <dxgi1_5.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include "../common/frame_timing_utils.h"
#include "audio_resampler.h"
#include "audio_ring_buffer.h"  // Pull Model Buffer
#include "audio_sync_utils.h"
#include "audio_time_utils.h"
#include "video_encoder.h"

extern "C" {
extern ID3D11Device* g_SharedD3D11Device;
extern ID3D11DeviceContext* g_SharedD3D11Context;
}

// Global or Singleton state preferred for DLL functions
// Or we map config to instance.
// For simplicity, SINGLETON pattern (one active engine).

class MediaEngine {
public:
    MediaEngine()
        : recording(false),
          audioRunning(false),
          recordingStartTime(),
          firstVideoFrameMs(0),
          firstVideoFrameCommitted(false),
          lastVideoFrameMs(0),
          videoElapsedMs(0) {}
    ~MediaEngine() {
        try {
        StopRecording();
        } catch (...) {
            DLL_Log("[MediaEngine] Suppressed exception during destruction");
        }
    }

    // Multi-source audio support
    struct AudioSource {
        std::unique_ptr<AudioCapture> capture;               // For system/mic audio
        std::unique_ptr<ProcessLoopbackCapture> appCapture;  // Disposable worker for per-app audio
        std::unique_ptr<AudioEncoder> encoder;               // Owned encoder (if first source for this track)
        AudioEncoder* sharedEncoderPtr = nullptr;            // Always points to the encoder to use
        std::unique_ptr<AudioRingBuffer> ringBuffer;         // Pull Model Buffer (Writer=Capture, Reader=Encoder)
        size_t fullRingBufferCapacityFloats = 0;  // Target capacity; app sources start small and grow on first capture
        std::unique_ptr<AudioResampler> resampler;  // Resampler for this source (to standard format)
        int mixChannels = 2;
        uint32_t mixChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

        // Per-source drift compensation (Phase 2 of AV Sync overhaul)
        std::unique_ptr<AudioResampler> syncResampler;  // 48kHz->48kHz resampler with drift compensation
        std::deque<float> postResampleBuffer;           // Buffer after sync resampling for exact framing
        int64_t syncSamplesOutput = 0;                  // Total samples output by syncResampler (for drift calculation)
        int dropFadeSamplesRemaining = 0;               // Remaining samples for post-drop transition smoothing
        float dropFadeStartL = 0.0f;                    // Left sample anchor for drop transition
        float dropFadeStartR = 0.0f;                    // Right sample anchor for drop transition
        std::vector<float> dropFadeStart;               // Last full frame before a drop, one value per channel
        int underrunFadeSamplesRemaining = 0;           // Remaining samples for post-underrun fade-in
        int packetBoundaryFadeInSamplesRemaining = 0;   // Fade-in after silence/overlap packet timeline correction
        uint64_t overflowDropSamples = 0;               // Newest samples dropped before entering the ring buffer
        uint64_t retainedNewestTrimSamples = 0;         // Oldest samples discarded by ring headroom retention
        uint64_t latencyTrimSamples = 0;                // Oldest samples trimmed to keep latency bounded
        uint64_t tier2TrimSamples = 0;                  // Latency trims specifically caused by tier2 drift correction
        uint64_t bootstrapTrimSamples = 0;              // Oldest startup samples trimmed before the track goes live
        uint64_t postResampleTrimSamples = 0;           // Samples discarded from post-resample backlog cap
        uint64_t underrunPadSamples = 0;                // Silence-padded samples due to source underrun
        uint64_t coverageLossTrimSamples = 0;           // Samples trimmed to track unrecoverable WGC video loss
        uint64_t startupSyntheticRingSamples = 0;       // Startup silence still queued in ringBuffer (48k samples/ch)
        uint64_t startupSyntheticResamplerSamples =
            0;                                     // Startup silence already read from ringBuffer into syncResampler
        uint64_t startupSyntheticPostSamples = 0;  // Startup silence staged in postResampleBuffer
        uint64_t startupGapProtectionSamples = 0;  // Initial post-start silence that must be emitted before trims
        uint64_t qpcAlignedWrittenSamples = 0;     // Timeline samples represented in the ring from packet QPC stitching
        uint64_t packetTimelineGapSamples = 0;     // Silence inserted to preserve packet-QPC continuity
        uint64_t packetTimelineOverlapSamples = 0;  // Packet-leading samples trimmed to avoid time overlap
        uint64_t startupRebasedGapSamples = 0;      // Persistent startup packet-QPC offset suppressed after sync reset
        uint64_t lateAppJoinSuppressedGapSamples = 0;  // First app packet gap suppressed to join live timeline
        uint64_t lateAppJoinPreservedGapSamples = 0;   // Small live-join cushion retained for click-free fade-in
        int64_t alignedStartMs = -1;                   // First source packet offset relative to recording start
        int64_t observedLateStartMs = 0;               // Latest observed startup delay used for startup pull slack
        bool hasAlignedStart = false;                  // True after first packet aligned to recording start
        bool sawCaptureEpoch = false;                  // Activation succeeded even if WASAPI delivered no data
        bool timelineValid = false;  // Epoch-local: source can contribute silence/real audio on the current timeline
        bool isPrimed = false;       // Epoch-local: source has buffered the post-activation safety cushion
        bool bootstrapComplete = false;  // Recording-sticky after startup settles; capture epochs must preserve it
        bool pendingUnderrunRecoveryFade = false;  // Arm fade-in when real audio resumes after padded silence
        bool sawSyncPendingPackets = false;        // App audio arrived before its post-anchor timeline opened
        bool startupRealAudioSeen = false;         // Real audio has been emitted for this source since sync reset
        bool pendingStartupJoinFade = false;       // Fade in real audio when a late source joins after startup silence
        uint64_t pendingRetainedTrimSamples = 0;   // Aggregated ring-headroom trims since the last periodic log
        uint32_t pendingRetainedTrimEvents = 0;    // Aggregated ring-headroom trim events since the last periodic log
        uint64_t pendingLatencyTrimSamples = 0;    // Aggregated trim samples since the last periodic log
        uint32_t pendingLatencyTrimEvents = 0;     // Aggregated trim events since the last periodic log
        uint64_t pendingTier2TrimSamples = 0;      // Aggregated tier2 drift trims since the last periodic log
        uint32_t pendingTier2TrimEvents = 0;       // Aggregated tier2 drift trim events since the last periodic log
        uint64_t pendingCoverageLossTrimSamples = 0;  // Aggregated coverage-loss trims since the last periodic log
        uint32_t pendingCoverageLossTrimEvents = 0;   // Aggregated coverage-loss trim events since the last log
        uint64_t lastRetainedTrimWarnTick = 0;        // Rate-limit explicit retained-audio warnings
        uint64_t lastExtremeDriftWarnTick = 0;        // Rate-limit chronic large-drift diagnostics
        uint64_t lastPacketTimelineAdjustWarnTick = 0;
        // Ingest starvation attribution. A packet whose whole timeline range falls behind the
        // already-exported cursor is real audio destroyed by consumer overrun, not a source gap.
        uint64_t lastRealPacketIngestTick = 0;       // Last tick a real packet was placed on the timeline
        uint64_t timelineStarvationDropSamples = 0;  // Real samples destroyed because the consumer ran ahead
        uint64_t timelineStarvationBeganTick = 0;    // Start of the current fully-starved episode (0 = healthy)
        uint64_t lastTimelineStarvationWarnTick = 0;
        int64_t timelineResyncOffsetSamples = 0;       // Last-resort placement re-anchor after unrecoverable starvation
        uint64_t timelineResyncSuppressedSamples = 0;  // Content skipped by that re-anchor
        uint32_t timelineResyncEvents = 0;
        uint64_t lastAppPlaceDiagTick = 0;    // Throttle app-source placement-divergence diagnostics
        uint64_t lastAppConsumeDiagTick = 0;  // Throttle app-source consume/drain diagnostics
        uint64_t lastCaptureGroupDivergenceWarnTick = 0;
        // App-audio latency observability: the ring backlog at consume time IS the audio-behind-video
        // delay. Sampled every pull so the recording-wide distribution (and any elevated/variable
        // latency) is obvious in the logs instead of needing manual reconstruction.
        uint32_t appLatencyBuckets[5] = {0, 0, 0, 0, 0};  // <50, 50-150, 150-300, 300-600, >600 ms
        uint64_t appLatencySampleCount = 0;
        uint64_t appLatencySumMs = 0;
        uint32_t appLatencyMaxMs = 0;
        uint64_t appLatencyTargetSumMs = 0;
        uint64_t appLatencyExcessSumMs = 0;
        uint32_t appLatencyExcessMaxMs = 0;
        uint32_t appLatencyDrainingSamples = 0;  // observations while backlog drain was active
        uint64_t appLatencyStopDrainSampleCount = 0;  // stop-time forced-drain observations (context only)
        uint64_t appLatencyStopDrainSumMs = 0;
        uint32_t appLatencyStopDrainMaxMs = 0;
        uint64_t appLatencyDrainTransitions = 0;
        uint32_t appLatencyMaxAbsCompDelta = 0;
        uint64_t lastAppLatencyWarnTick = 0;  // throttle the elevated-latency warning
        bool appLatencyWarnActive = false;
        bool appAudioBacklogDrainInitialized = false;
        bool appAudioBacklogDrainActive = false;
        uint32_t appAudioBacklogDrainReason =
            static_cast<uint32_t>(ce::audio::CfrAppAudioBacklogDrainReason::SourceBootstrapPending);
        int64_t appAudioBacklogTargetSamples = 0;
        int64_t appAudioBacklogExcessSamples = 0;
        int32_t appAudioBacklogCompensationDelta = 0;
        uint64_t catastrophicResyncSamples = 0;  // Legacy log-schema sentinel; CFR destructive resync is prohibited
        uint32_t catastrophicResyncEvents = 0;   // Legacy log-schema sentinel; must remain zero
        double wgcCoverageLossTrimAccumulator = 0.0;  // Fractional carry for paced overload micro-trims
        uint64_t timelineResetGeneration = 0;         // Last atomic startup reset acknowledged by this route
        // Epoch transitions are a two-owner hand-off. AudioLoop owns format conversion and the ring writer;
        // PullAndEncodeAudio owns syncResampler/postResampleBuffer. Shared atomics let the writer stop at the
        // epoch boundary until the pull side has encoded the old tail and reset only its own state.
        std::shared_ptr<std::atomic<uint64_t>> epochResetRequested = std::make_shared<std::atomic<uint64_t>>(0);
        std::shared_ptr<std::atomic<uint64_t>> epochResetAcknowledged = std::make_shared<std::atomic<uint64_t>>(0);
        uint64_t epochSyncTailFlushedGeneration = 0;  // Pull-thread-owned generation
        uint64_t lastEpochTransitionWaitLogTick = 0;

        // Rate-based drift correction state
        int64_t prevLeadSamples = 0;       // Previous lead measurement for rate calculation
        int64_t prevLeadSnapshotMs = 0;    // Timestamp of the snapshot for rate window
        int64_t lastRateUpdateMs = 0;      // Timestamp of last rate correction update
        int32_t currentRateDelta = 0;      // Current rate correction in samples/10s
        int32_t targetRateDelta = 0;       // Requested rate correction before slew limiting
        bool rateCompActive = false;       // Whether rate compensation is currently active
        bool targetRateSaturated = false;  // Requested correction exceeded configured positive budget

        AudioConfig config;
        int track = 0;  // Target track number
        // One configured source may target several tracks. Those routes must consume
        // the same physical WASAPI packet stream; starting one endpoint/process
        // capture per route creates independent startup queues and clock phases.
        size_t configuredSourceIndex = std::numeric_limits<size_t>::max();
        size_t captureFanoutOwnerIndex = std::numeric_limits<size_t>::max();
        std::shared_ptr<std::atomic<bool>> appCaptureRouteEnded = std::make_shared<std::atomic<bool>>(false);

        size_t ringBufferPeakSamples = 0;
        uint32_t ringBufferUnderrunCount = 0;
        AudioConfig::SourceType sourceType = AudioConfig::SystemAudio;
    };

    struct TrackAudioFormat {
        int channels = 2;
        uint32_t channelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        int sampleRate = 48000;
        int priority = 10;
    };

    static float ComputeRaisedCosineFade(size_t index, size_t totalSamples) {
        if (totalSamples == 0) {
            return 1.0f;
        }

        const float t = static_cast<float>(index + 1) / static_cast<float>(totalSamples);
        return 0.5f * (1.0f - std::cos(3.14159265358979323846f * t));
    }

    static void ApplyPacketBoundaryFadeIn(float* interleavedSamples, size_t sampleCount, size_t channels,
                                          size_t fadeSamples) {
        if (!interleavedSamples || channels == 0 || sampleCount == 0 || fadeSamples == 0) {
            return;
        }

        const size_t blendSamples = std::min(sampleCount, fadeSamples);
        for (size_t sampleIdx = 0; sampleIdx < blendSamples; ++sampleIdx) {
            const float fade = ComputeRaisedCosineFade(sampleIdx, fadeSamples);
            const size_t base = sampleIdx * channels;
            for (size_t ch = 0; ch < channels; ++ch) {
                interleavedSamples[base + ch] *= fade;
            }
        }
    }

    static uint32_t DefaultChannelMaskForChannels(int channels) {
        switch (channels) {
            case 1:
                return SPEAKER_FRONT_CENTER;
            case 2:
                return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
            case 3:
                return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER;
            case 4:
                return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
            case 5:
                return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_BACK_LEFT |
                       SPEAKER_BACK_RIGHT;
            case 6:
                return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                       SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
            case 7:
                return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                       SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT | SPEAKER_BACK_CENTER;
            case 8:
                return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                       SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
            default:
                return 0;
        }
    }

    static int ParseAudioSampleRate(const AudioConfig& audioConfig) {
        return ce::audio::ParseSampleRateOr(audioConfig.sampleRate, 48000);
    }

    static int AudioSourceLayoutPriority(AudioConfig::SourceType sourceType) {
        return sourceType == AudioConfig::Microphone ? 2 : 1;
    }

    static TrackAudioFormat ProbeSourceTrackFormat(const AudioConfig& audioConfig) {
        TrackAudioFormat format;
        format.sampleRate = ParseAudioSampleRate(audioConfig);
        if (audioConfig.downmix) {
            format.channels = 2;
            format.channelMask = DefaultChannelMaskForChannels(2);
            format.priority = 0;
            return format;
        }

        AudioPacket probed{};
        bool probedOk = false;
        if (audioConfig.sourceType == AudioConfig::AppAudio) {
            // Process loopback accepts a requested format; preserve the default
            // render endpoint layout for app-only tracks instead of hard-coding stereo.
            probedOk = AudioCapture::ProbeMixFormat("", true, &probed);
        } else {
            probedOk = AudioCapture::ProbeMixFormat(audioConfig.device,
                                                    audioConfig.sourceType == AudioConfig::SystemAudio, &probed);
        }
        if (probedOk && probed.channels > 0) {
            format.channels = std::clamp(probed.channels, 1, 8);
            format.channelMask =
                probed.channelMask != 0 ? probed.channelMask : DefaultChannelMaskForChannels(format.channels);
            format.sampleRate = 48000;
        }
        format.priority = AudioSourceLayoutPriority(audioConfig.sourceType);
        return format;
    }

    std::map<int, TrackAudioFormat> ResolveTrackAudioFormats(const AppConfig& appConfig) {
        std::map<int, TrackAudioFormat> resolved;
        for (size_t i = 0; i < appConfig.audioSources.size(); ++i) {
            const AudioConfig& audioConfig = appConfig.audioSources[i];
            if (!audioConfig.enabled) {
                continue;
            }

            std::vector<int> targetTracks = audioConfig.tracks;
            if (targetTracks.empty()) {
                targetTracks.push_back(static_cast<int>(i + 1));
            }

            TrackAudioFormat candidate = ProbeSourceTrackFormat(audioConfig);
            for (int track : targetTracks) {
                auto it = resolved.find(track);
                if (it == resolved.end() || candidate.priority < it->second.priority) {
                    resolved[track] = candidate;
                    DLL_Log("[AudioFormat] Track %d resolved format: %dch mask=0x%x rate=%d priority=%d", track,
                            candidate.channels, candidate.channelMask, candidate.sampleRate, candidate.priority);
                }
            }
        }
        return resolved;
    }

    TrackAudioFormat GetTrackAudioFormat(int track) const {
        auto it = trackAudioFormats.find(track);
        if (it != trackAudioFormats.end()) {
            return it->second;
        }
        return {};
    }

    static void CaptureDropFadeAnchor(AudioSource& src, int channels) {
        channels = std::clamp(channels, 1, 8);
        src.dropFadeStart.assign(static_cast<size_t>(channels), 0.0f);
        if (src.postResampleBuffer.size() < static_cast<size_t>(channels)) {
            src.dropFadeStartL = 0.0f;
            src.dropFadeStartR = 0.0f;
            return;
        }
        const size_t base = src.postResampleBuffer.size() - static_cast<size_t>(channels);
        for (int ch = 0; ch < channels; ++ch) {
            src.dropFadeStart[static_cast<size_t>(ch)] = src.postResampleBuffer[base + static_cast<size_t>(ch)];
        }
        src.dropFadeStartL = src.dropFadeStart[0];
        src.dropFadeStartR = channels > 1 ? src.dropFadeStart[1] : src.dropFadeStart[0];
    }

    static float GetDropFadeAnchor(const AudioSource& src, int channel) {
        if (channel >= 0 && channel < static_cast<int>(src.dropFadeStart.size())) {
            return src.dropFadeStart[static_cast<size_t>(channel)];
        }
        return 0.0f;
    }

    // Per-track encoder with optional mixing (when multiple sources target same
    // track)

    // Member variables
    std::unique_ptr<VideoEncoder> videoEnc;
    std::vector<AudioSource> audioSources;
    SharedMemoryLayout* sharedMemLayout = nullptr;

    // Audio-only recording mode (no video capture/encoding)
    bool audioOnly = false;
    AVFormatContext* audioOnlyFmtCtx = nullptr;
    std::string audioOnlyFilename;
    ce::capture_output::ReservedCaptureOutput audioOnlyOutputReservation;
    bool audioOnlyTrailerSucceeded = false;
    std::vector<AudioEncoder*> trackEncoders;  // All unique encoders for audio-only padding

    AppConfig config;
    std::recursive_mutex muxMutex;  // Must be recursive - WritePacket callback from EncodeFrame
    bool recording;
    bool processLoopbackIntegrityFailureSignaled = false;
    bool timingModeFrozenForSession = false;
    bool sessionUseVfr = false;
    bool activeScreenGrab = false;

    // Audio thread
    std::thread audioThread;
    std::atomic<bool> audioRunning;
    std::atomic<bool> audioSyncPending{false};  // Pause AudioLoop writes during buffer clear
    std::atomic<uint64_t> audioResetRequestedGeneration{0};
    std::atomic<uint64_t> audioResetAcknowledgedGeneration{0};
    std::atomic<uint64_t> audioResetCommittedGeneration{0};
    std::atomic<bool> audioResetPreservePackets{false};
    std::mutex audioDrainMutex;
    std::mutex audioSourceStopMutex;
    std::condition_variable audioDrainCv;
    std::atomic<bool> audioStopDrainRequested{false};
    std::atomic<bool> audioStopDrainComplete{false};
    std::atomic<bool> audioFinalizingCfrStop{false};
    std::chrono::steady_clock::time_point recordingStartTime;  // CaptureEngine clock start time
    int64_t firstVideoFrameMs;                                 // Timestamp of first video frame for A/V sync
    bool firstVideoFrameCommitted;
    int64_t lastVideoFrameMs;                                  // Timestamp of last video frame for audio trimming
    std::atomic<int64_t> videoElapsedMs;                       // Elapsed video time in ms for audio clock sync
    std::atomic<int64_t> recordingStartSystemQPCMs{0};         // Start time in System QPC MS (for Audio Alignment)
    std::atomic<int64_t> recordingStartSystemQpc100ns{0};      // Start time in 100-ns QPC units for packet stitching
    std::atomic<int64_t> wgcStartupExtraDelayQpc{0};           // WGC smoothness delay used for startup preservation
    std::atomic<bool> preservePendingStartupAudioPackets{false};
    SourceTimelineState injectTimelineState;  // Source-frame QPC for inject-relative timing
    SourceTimelineState d3d11TimelineState;   // Source-frame QPC for WGC-relative timing

    // Get current video elapsed time for audio clock compensation
    int64_t GetVideoElapsedMs() const {
        return videoElapsedMs.load();
    }

    bool SessionUsesVfr() const {
        return timingModeFrozenForSession ? sessionUseVfr : config.video.useVFR;
    }

    bool SessionUsesScreenGrab() const {
        return activeScreenGrab;
    }

    int64_t GetCommittedVideoElapsedUs(int64_t fallbackElapsedUs) const {
        if (SessionUsesVfr() || !videoEnc) {
            return fallbackElapsedUs;
        }

        const int64_t encodedDurationUs = videoEnc->GetEncodedDurationUs();
        return encodedDurationUs > 0 ? encodedDurationUs : fallbackElapsedUs;
    }

    void CommitVideoElapsedUs(SourceTimelineState& timelineState, int64_t elapsedUs) {
        if (elapsedUs < 0) {
            return;
        }

        timelineState.lastElapsedUs = std::max(timelineState.lastElapsedUs, elapsedUs);
        this->videoElapsedMs.store(elapsedUs / 1000);
        lastVideoFrameMs = elapsedUs / 1000;
    }

    size_t GetBufferedTimelineSamples(const AudioSource& src) const {
        const size_t kChannels = static_cast<size_t>(std::clamp(src.mixChannels, 1, 8));

        size_t bufferedSamples = src.postResampleBuffer.size() / kChannels;
        if (src.ringBuffer) {
            bufferedSamples += src.ringBuffer->GetAvailable() / kChannels;
        }
        return bufferedSamples;
    }

    // AudioLoop -> PullAndEncodeAudio hand-off for the adaptive ingestion reservoir.
    // Publishes the worst (minimum) headroom seen since the pull side last consumed it,
    // normalized to the shared 48 kHz mixing rate. `kNoAudioIngestHeadroom` doubles as the
    // "nothing observed this window" sentinel so one atomic carries both facts and no
    // observation can be lost between the reader's two accesses.
    static constexpr int64_t kNoAudioIngestHeadroom = std::numeric_limits<int64_t>::max();

    void PublishAudioIngestHeadroom(int64_t headroomSamples, int sampleRate) {
        if (sampleRate <= 0) {
            return;
        }
        const int64_t normalizedSamples =
            sampleRate == 48000 ? headroomSamples : (headroomSamples * 48000) / sampleRate;
        int64_t observedWorst = audioIngestWorstHeadroomSamples.load(std::memory_order_relaxed);
        while (normalizedSamples < observedWorst && !audioIngestWorstHeadroomSamples.compare_exchange_weak(
                                                        observedWorst, normalizedSamples, std::memory_order_relaxed)) {
        }
    }

    // Tracks fully-destroyed packets for one source and applies the bounded last-resort
    // re-anchor when the adaptive reservoir has already saturated. Re-anchoring costs a
    // one-time content skip on this source only; the alternative is permanent silence.
    void ServiceSourceIngestStarvation(AudioSource& src, size_t srcIdx, int64_t packetStartSamples,
                                       int64_t overlapSamples, size_t retainedWriteSamples, int resampledSamples,
                                       int sampleRate, uint64_t nowTick) {
        const bool packetFullyDestroyed = resampledSamples > 0 && retainedWriteSamples == 0 && overlapSamples > 0;
        if (!packetFullyDestroyed) {
            src.timelineStarvationBeganTick = 0;
            return;
        }

        src.timelineStarvationDropSamples += static_cast<uint64_t>(resampledSamples);
        if (src.timelineStarvationBeganTick == 0) {
            src.timelineStarvationBeganTick = nowTick;
        }

        const int64_t boundedRate = std::max<int64_t>(1, sampleRate);
        const int64_t starvedElapsedMs = static_cast<int64_t>(nowTick - src.timelineStarvationBeganTick);
        const int64_t deficitSamples =
            std::max<int64_t>(0, static_cast<int64_t>(src.qpcAlignedWrittenSamples) - packetStartSamples);
        const bool reservoirAtCap = audioIngestReservoirExtraMs >= ce::audio::kAudioIngestMaxExtraReservoirMs;

        if (nowTick - src.lastTimelineStarvationWarnTick >= 1000) {
            DLL_Log(
                "[AudioLoop] WARNING: source ingest starvation src=%zu track=%d process=%s destroyed=%llu samples "
                "(%.1fms total) deficit=%lld samples (%lldms) starvedFor=%lldms reservoirExtra=%lldms atCap=%d. "
                "The exported cursor ran past the live capture edge; every packet for this range is real audio "
                "being discarded as timeline overlap.",
                srcIdx, src.track, src.config.processName.empty() ? "<none>" : src.config.processName.c_str(),
                (unsigned long long)src.timelineStarvationDropSamples,
                static_cast<double>(src.timelineStarvationDropSamples) * 1000.0 / static_cast<double>(boundedRate),
                (long long)deficitSamples, (long long)(deficitSamples * 1000 / boundedRate),
                (long long)starvedElapsedMs, (long long)audioIngestReservoirExtraMs, reservoirAtCap ? 1 : 0);
            src.lastTimelineStarvationWarnTick = nowTick;
        }

        if (!ce::audio::ShouldResyncStarvedLiveAudioSource(IsCfrRecording(), false, reservoirAtCap, true,
                                                           starvedElapsedMs, deficitSamples,
                                                           /*minStarvedMs=*/1500,
                                                           /*minDeficitSamples=*/boundedRate / 100)) {
            return;
        }

        src.timelineResyncOffsetSamples += deficitSamples;
        src.timelineResyncSuppressedSamples += static_cast<uint64_t>(deficitSamples);
        src.timelineResyncEvents++;
        src.timelineStarvationBeganTick = 0;
        src.packetBoundaryFadeInSamplesRemaining = static_cast<int>(std::max<int64_t>(1, boundedRate / 750));
        DLL_Log(
            "[AudioLoop] WARNING: unrecoverable ingest starvation - re-anchoring src=%zu track=%d process=%s by "
            "%lld samples (%lldms) after %lldms at the reservoir cap. This source skips that much content once "
            "so live audio resumes; track lengths, PTS, and every other source are unchanged.",
            srcIdx, src.track, src.config.processName.empty() ? "<none>" : src.config.processName.c_str(),
            (long long)deficitSamples, (long long)(deficitSamples * 1000 / boundedRate), (long long)starvedElapsedMs);
    }

    size_t DropOldestBufferedSamples(AudioSource& src, size_t samplesToDrop) {
        const size_t kChannels = static_cast<size_t>(std::clamp(src.mixChannels, 1, 8));

        if (samplesToDrop == 0) {
            return 0;
        }

        size_t droppedSamples = 0;

        size_t postSamples = src.postResampleBuffer.size() / kChannels;
        size_t dropFromPost = std::min(postSamples, samplesToDrop);
        if (dropFromPost > 0) {
            size_t dropFloats = dropFromPost * kChannels;
            ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticPostSamples, dropFromPost);
            ce::audio::ConsumeSyntheticBufferedSamples(src.startupGapProtectionSamples, dropFromPost);
            src.postResampleBuffer.erase(src.postResampleBuffer.begin(),
                                         src.postResampleBuffer.begin() + static_cast<std::ptrdiff_t>(dropFloats));
            droppedSamples += dropFromPost;
            samplesToDrop -= dropFromPost;
        }

        if (samplesToDrop > 0 && src.ringBuffer) {
            size_t droppedFloats = src.ringBuffer->Skip(samplesToDrop * kChannels);
            size_t droppedFromRing = droppedFloats / kChannels;
            ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticRingSamples, droppedFromRing);
            ce::audio::ConsumeSyntheticBufferedSamples(src.startupGapProtectionSamples, droppedFromRing);
            droppedSamples += droppedFromRing;
        }

        if (droppedSamples > 0) {
            src.latencyTrimSamples += droppedSamples;
            src.bootstrapTrimSamples += droppedSamples;
        }

        return droppedSamples;
    }

    void DiscardPendingAudioPackets() {
        for (auto& src : audioSources) {
            if (src.capture) {
                src.capture->DiscardPendingPackets();
            }
            if (src.appCapture) {
                src.appCapture->DiscardPendingPackets();
            }
        }
    }

    size_t StopAudioCaptureSources(bool discardPendingPackets) {
        std::lock_guard<std::mutex> stopLock(audioSourceStopMutex);
        size_t pendingAfterStop = 0;
        for (auto& src : audioSources) {
            if (src.capture) {
                src.capture->Stop(discardPendingPackets);
                if (!discardPendingPackets) {
                    pendingAfterStop += src.capture->PendingPacketCount();
                }
            }
            if (src.appCapture) {
                src.appCapture->Stop(discardPendingPackets);
                if (!discardPendingPackets) {
                    pendingAfterStop += src.appCapture->PendingPacketCount();
                }
            }
        }
        return pendingAfterStop;
    }

    struct FinalSourceCatchupStatus {
        bool ready = true;
        size_t sourceIndex = 0;
        int track = 0;
        int64_t requestedSamples = 0;
        size_t bufferedSamples = 0;
        int64_t missingSamples = 0;
    };

    FinalSourceCatchupStatus GetFinalCfrSourceCatchupStatus(int64_t targetUs) const {
        FinalSourceCatchupStatus status;
        if (!IsCfrRecording() || targetUs <= 0) {
            return status;
        }

        constexpr int kStopSampleRate = 48000;
        const int64_t targetSamples = ce::audio::ComputeDurationUsToSamples(targetUs, kStopSampleRate);
        for (const auto& kv : cachedTrackToSources) {
            const int track = kv.first;
            const auto trackIt = trackTimelineSamples.find(track);
            const int64_t trackCursorSamples = trackIt != trackTimelineSamples.end() ? trackIt->second : 0;
            const int64_t requestedSamples = targetSamples - trackCursorSamples;
            if (requestedSamples <= 0) {
                continue;
            }

            for (size_t srcIdx : kv.second) {
                if (srcIdx >= audioSources.size()) {
                    continue;
                }
                const auto& src = audioSources[srcIdx];
                if (!src.sharedEncoderPtr || !src.ringBuffer) {
                    continue;
                }

                const bool isAppAudioSource = (src.sourceType == AudioConfig::AppAudio);
                const bool optionalUnstarted = ce::audio::IsOptionalUnstartedAppAudioSource(
                    isAppAudioSource, src.timelineValid, src.sawSyncPendingPackets);
                const bool appCaptureRouteEnded =
                    src.appCaptureRouteEnded && src.appCaptureRouteEnded->load(std::memory_order_acquire);
                const bool inactiveStartedAppSourceMaySilence =
                    ce::audio::ShouldTreatInactiveStartedAppCaptureAsSilence(
                        true, isAppAudioSource, src.timelineValid || src.sawSyncPendingPackets, !appCaptureRouteEnded);
                const bool sparseStartedSourceCanSilence = ce::audio::ShouldTreatSparseStartedSourceAsSilence(
                    true, src.timelineValid, src.bootstrapComplete, optionalUnstarted, true);
                const bool strictSource = src.sourceType != AudioConfig::Microphone;
                const size_t bufferedTimelineSamples = GetBufferedTimelineSamples(src);
                const bool sparseStartedSourceMaySilence =
                    ce::audio::ShouldTreatStartedTimelineSourceShortfallAsSilence(sparseStartedSourceCanSilence,
                                                                                  bufferedTimelineSamples) ||
                    inactiveStartedAppSourceMaySilence;
                if (ce::audio::ShouldWaitForFinalCfrSourceCatchup(true, strictSource, optionalUnstarted,
                                                                  sparseStartedSourceMaySilence, requestedSamples,
                                                                  bufferedTimelineSamples)) {
                    status.ready = false;
                    status.sourceIndex = srcIdx;
                    status.track = track;
                    status.requestedSamples = requestedSamples;
                    status.bufferedSamples = bufferedTimelineSamples;
                    status.missingSamples =
                        requestedSamples -
                        static_cast<int64_t>(std::min<size_t>(
                            bufferedTimelineSamples, static_cast<size_t>(std::max<int64_t>(requestedSamples, 0))));
                    return status;
