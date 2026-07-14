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
          lastVideoFrameMs(0),
          videoElapsedMs(0) {}
    ~MediaEngine() {
        StopRecording();
    }

    // Multi-source audio support
    struct AudioSource {
        std::unique_ptr<AudioCapture> capture;        // For system/mic audio
        std::unique_ptr<ProcessLoopbackCapture> appCapture;  // Disposable worker for per-app audio
        std::unique_ptr<AudioEncoder> encoder;        // Owned encoder (if first source for this track)
        AudioEncoder* sharedEncoderPtr = nullptr;     // Always points to the encoder to use
        std::unique_ptr<AudioRingBuffer> ringBuffer;  // Pull Model Buffer (Writer=Capture, Reader=Encoder)
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
        bool timelineValid = false;                // True once the source can contribute silence/real audio on timeline
        bool isPrimed = false;                     // True after source has buffered a startup safety cushion
        bool bootstrapComplete = false;            // True after startup backlog is settled and live sync may engage
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
        uint64_t appLatencyDrainTransitions = 0;
        uint32_t appLatencyMaxAbsCompDelta = 0;
        uint64_t lastAppLatencyWarnTick = 0;  // throttle the elevated-latency warning
        bool appLatencyWarnActive = false;
        bool appAudioBacklogDrainInitialized = false;
        bool appAudioBacklogDrainActive = false;
        uint32_t appAudioBacklogDrainReason =
            static_cast<uint32_t>(ce::audio::CfrAppAudioBacklogDrainReason::WithinSlack);
        int64_t appAudioBacklogTargetSamples = 0;
        int64_t appAudioBacklogExcessSamples = 0;
        int32_t appAudioBacklogCompensationDelta = 0;
        uint64_t catastrophicResyncSamples = 0;       // Stale samples dropped resyncing to live after a read-stall
        uint32_t catastrophicResyncEvents = 0;        // Number of catastrophic backlog resyncs (alt-tab/DPC/overload)
        uint64_t lastCatastrophicResyncTick = 0;      // Throttle catastrophic resync logging
        double wgcCoverageLossTrimAccumulator = 0.0;  // Fractional carry for paced overload micro-trims
        uint64_t timelineResetGeneration = 0;         // Last atomic startup reset acknowledged by this route
        // Epoch transitions are a two-owner hand-off. AudioLoop owns format conversion and the ring writer;
        // PullAndEncodeAudio owns syncResampler/postResampleBuffer. Shared atomics let the writer stop at the
        // epoch boundary until the pull side has encoded the old tail and reset only its own state.
        std::shared_ptr<std::atomic<uint64_t>> epochResetRequested =
            std::make_shared<std::atomic<uint64_t>>(0);
        std::shared_ptr<std::atomic<uint64_t>> epochResetAcknowledged =
            std::make_shared<std::atomic<uint64_t>>(0);
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
                        true, isAppAudioSource, src.timelineValid || src.sawSyncPendingPackets,
                        !appCaptureRouteEnded);
                const bool sparseStartedSourceCanSilence = ce::audio::ShouldTreatSparseStartedSourceAsSilence(
                    true, src.timelineValid, src.bootstrapComplete, optionalUnstarted, true);
                const bool strictSource = src.sourceType != AudioConfig::Microphone;
                const size_t bufferedTimelineSamples = GetBufferedTimelineSamples(src);
                const bool sparseStartedSourceMaySilence = ce::audio::ShouldTreatStartedAppSourceShortfallAsSilence(
                    sparseStartedSourceCanSilence, bufferedTimelineSamples) ||
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
                }
            }
        }

        return status;
    }

    bool WaitForFinalCfrAudioSourceCatchup(int64_t targetUs) {
        if (!ce::audio::ShouldDrainStoppedCaptureQueuesBeforeFinalAudioPull(audioRunning.load(), audioOnly, targetUs) ||
            !IsCfrRecording()) {
            return true;
        }

        FinalSourceCatchupStatus status = GetFinalCfrSourceCatchupStatus(targetUs);
        if (status.ready) {
            return true;
        }

        constexpr auto kFinalCatchupMaxWait = std::chrono::milliseconds(500);
        const auto waitStart = std::chrono::steady_clock::now();
        const auto deadline = waitStart + kFinalCatchupMaxWait;
        DLL_Log(
            "[StopAudio] Waiting for final source catch-up: targetUs=%lld track=%d src=%zu requested=%lld "
            "buffered=%zu missing=%lld",
            targetUs, status.track, status.sourceIndex, status.requestedSamples, status.bufferedSamples,
            status.missingSamples);

        std::unique_lock<std::mutex> lock(audioDrainMutex);
        audioDrainCv.wait_until(lock, deadline, [this, targetUs, &status]() {
            if (!audioRunning.load(std::memory_order_acquire)) {
                return true;
            }
            status = GetFinalCfrSourceCatchupStatus(targetUs);
            return status.ready;
        });
        lock.unlock();

        status = GetFinalCfrSourceCatchupStatus(targetUs);
        const auto waitedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count();
        if (!status.ready) {
            DLL_Log(
                "[StopAudio] WARNING: Final source catch-up timed out after %lldms: targetUs=%lld track=%d src=%zu "
                "requested=%lld buffered=%zu missing=%lld. Final force drain may need silence padding.",
                static_cast<long long>(waitedMs), targetUs, status.track, status.sourceIndex, status.requestedSamples,
                status.bufferedSamples, status.missingSamples);
            return false;
        }

        DLL_Log("[StopAudio] Final source catch-up ready after %lldms for targetUs=%lld",
                static_cast<long long>(waitedMs), targetUs);
        return true;
    }

    size_t StopCaptureSourcesAndDrainAudioLoop() {
        const size_t pendingPackets = StopAudioCaptureSources(false);

        audioStopDrainComplete.store(false, std::memory_order_release);
        audioStopDrainRequested.store(true, std::memory_order_release);
        audioDrainCv.notify_all();

        {
            std::unique_lock<std::mutex> lock(audioDrainMutex);
            audioDrainCv.wait(lock, [this]() {
                return audioStopDrainComplete.load(std::memory_order_acquire) || !audioRunning.load();
            });
        }

        audioStopDrainRequested.store(false, std::memory_order_release);
        DLL_Log("[StopAudio] Capture stop-drain completed: preservedPackets=%zu audioOnly=%d", pendingPackets,
                audioOnly ? 1 : 0);
        return pendingPackets;
    }

    void AudioThreadEntry() noexcept {
        bool failed = false;
        try {
            AudioLoop();
        } catch (const std::exception& e) {
            failed = true;
            DLL_Log("[AudioLoop] ERROR: Unhandled exception escaped audio worker: %s", e.what());
        } catch (...) {
            failed = true;
            DLL_Log("[AudioLoop] ERROR: Unknown exception escaped audio worker");
        }

        if (failed) {
            // No consumer remains after an unexpected worker exit. Stop the
            // producers and discard their queues instead of allowing them to
            // grow indefinitely for the rest of the recording.
            StopAudioCaptureSources(true);
        }
        audioRunning.store(false, std::memory_order_release);
        audioStopDrainComplete.store(true, std::memory_order_release);
        audioDrainCv.notify_all();
    }

    bool StartAudioThread() {
        audioStopDrainComplete.store(false, std::memory_order_release);
        audioRunning.store(true, std::memory_order_release);
        try {
            audioThread = std::thread(&MediaEngine::AudioThreadEntry, this);
            return true;
        } catch (const std::exception& e) {
            DLL_Log("[AudioLoop] ERROR: Failed to create audio worker: %s", e.what());
        } catch (...) {
            DLL_Log("[AudioLoop] ERROR: Failed to create audio worker with unknown exception");
        }

        audioRunning.store(false, std::memory_order_release);
        audioStopDrainComplete.store(true, std::memory_order_release);
        audioDrainCv.notify_all();
        StopAudioCaptureSources(true);
        return false;
    }

    void DrainStoppedCaptureQueuesBeforeFinalPull(int64_t targetUs) {
        if (!ce::audio::ShouldDrainStoppedCaptureQueuesBeforeFinalAudioPull(audioRunning.load(), audioOnly, targetUs)) {
            return;
        }

        StopCaptureSourcesAndDrainAudioLoop();
    }

    void ApplyAudioTimelineReset(uint64_t generation, int64_t startQpcMs, bool preservePendingPackets) {
        // Discard anything captured before the first video frame for the normal
        // low-latency start. WGC CFR smoothness startup is the exception: the
        // selected video frame is deliberately older, so queued post-anchor audio
        // is the content that matches that frame and must be preserved.
        if (!preservePendingPackets) {
            DiscardPendingAudioPackets();
        }
        std::set<AudioEncoder*> resetEncoders;
        for (auto& src : audioSources) {
            if (src.sharedEncoderPtr && resetEncoders.insert(src.sharedEncoderPtr).second) {
                src.sharedEncoderPtr->ResetForRecordingStart(0, generation);
            }
            if (src.ringBuffer) {
                src.ringBuffer->Clear();
            }
            if (src.syncResampler) {
                src.syncResampler->Reset();
            }
            // AudioLoop may have already preprocessed packets while waiting for the first
            // video frame. Drop that conversion state too so the track starts from a clean
            // audio timeline anchor.
            src.resampler.reset();
            src.postResampleBuffer.clear();
            src.syncSamplesOutput = 0;
            src.dropFadeSamplesRemaining = 0;
            src.dropFadeStartL = 0.0f;
            src.dropFadeStartR = 0.0f;
            src.dropFadeStart.clear();
            src.underrunFadeSamplesRemaining = 0;
            src.packetBoundaryFadeInSamplesRemaining = 0;
            src.overflowDropSamples = 0;
            src.retainedNewestTrimSamples = 0;
            src.latencyTrimSamples = 0;
            src.tier2TrimSamples = 0;
            src.bootstrapTrimSamples = 0;
            src.postResampleTrimSamples = 0;
            src.underrunPadSamples = 0;
            src.coverageLossTrimSamples = 0;
            src.startupSyntheticRingSamples = 0;
            src.startupSyntheticResamplerSamples = 0;
            src.startupSyntheticPostSamples = 0;
            src.startupGapProtectionSamples = 0;
            src.qpcAlignedWrittenSamples = 0;
            src.packetTimelineGapSamples = 0;
            src.packetTimelineOverlapSamples = 0;
            src.startupRebasedGapSamples = 0;
            src.lateAppJoinSuppressedGapSamples = 0;
            src.lateAppJoinPreservedGapSamples = 0;
            src.alignedStartMs = -1;
            src.observedLateStartMs = 0;
            src.hasAlignedStart = false;
            src.sawCaptureEpoch = false;
            const bool appCaptureRouteEnded =
                src.appCaptureRouteEnded && src.appCaptureRouteEnded->load(std::memory_order_acquire);
            src.timelineValid = src.sourceType != AudioConfig::AppAudio || appCaptureRouteEnded;
            src.isPrimed = false;
            src.bootstrapComplete = false;
            src.pendingUnderrunRecoveryFade = false;
            src.startupRealAudioSeen = false;
            src.pendingStartupJoinFade = false;
            src.pendingRetainedTrimSamples = 0;
            src.pendingRetainedTrimEvents = 0;
            src.pendingLatencyTrimSamples = 0;
            src.pendingLatencyTrimEvents = 0;
            src.pendingTier2TrimSamples = 0;
            src.pendingTier2TrimEvents = 0;
            src.pendingCoverageLossTrimSamples = 0;
            src.pendingCoverageLossTrimEvents = 0;
            src.lastAppPlaceDiagTick = 0;
            src.lastAppConsumeDiagTick = 0;
            src.appLatencyBuckets[0] = 0;
            src.appLatencyBuckets[1] = 0;
            src.appLatencyBuckets[2] = 0;
            src.appLatencyBuckets[3] = 0;
            src.appLatencyBuckets[4] = 0;
            src.appLatencySampleCount = 0;
            src.appLatencySumMs = 0;
            src.appLatencyMaxMs = 0;
            src.appLatencyTargetSumMs = 0;
            src.appLatencyExcessSumMs = 0;
            src.appLatencyExcessMaxMs = 0;
            src.appLatencyDrainingSamples = 0;
            src.appLatencyDrainTransitions = 0;
            src.appLatencyMaxAbsCompDelta = 0;
            src.lastAppLatencyWarnTick = 0;
            src.appLatencyWarnActive = false;
            src.appAudioBacklogDrainInitialized = false;
            src.appAudioBacklogDrainActive = false;
            src.appAudioBacklogDrainReason =
                static_cast<uint32_t>(ce::audio::CfrAppAudioBacklogDrainReason::WithinSlack);
            src.appAudioBacklogTargetSamples = 0;
            src.appAudioBacklogExcessSamples = 0;
            src.appAudioBacklogCompensationDelta = 0;
            src.catastrophicResyncSamples = 0;
            src.catastrophicResyncEvents = 0;
            src.lastCatastrophicResyncTick = 0;
            src.lastRetainedTrimWarnTick = 0;
            src.lastPacketTimelineAdjustWarnTick = 0;
            src.wgcCoverageLossTrimAccumulator = 0.0;
            src.prevLeadSamples = 0;
            src.prevLeadSnapshotMs = 0;
            src.lastRateUpdateMs = 0;
            src.currentRateDelta = 0;
            src.targetRateDelta = 0;
            src.rateCompActive = false;
            src.targetRateSaturated = false;
            src.timelineResetGeneration = generation;
            src.epochResetRequested->store(0, std::memory_order_release);
            src.epochResetAcknowledged->store(0, std::memory_order_release);
            src.epochSyncTailFlushedGeneration = 0;
            src.lastEpochTransitionWaitLogTick = 0;
            DLL_Log(
                "[A/V START] Audio route acknowledged reset: generation=%llu src=%zu track=%d type=%d "
                "startMs=%lld timelineValid=%d captureLatencyMs=%.3f",
                static_cast<unsigned long long>(generation),
                static_cast<size_t>(&src - audioSources.data()), src.track, static_cast<int>(src.sourceType),
                startQpcMs, src.timelineValid ? 1 : 0, static_cast<double>(src.config.captureLatencyMs));
        }
        DLL_Log(
            "[A/V START] Reset generation=%llu applied atomically: routes=%zu encoders=%zu preservePending=%d",
            static_cast<unsigned long long>(generation), audioSources.size(), resetEncoders.size(),
            preservePendingPackets ? 1 : 0);
    }

    void SyncAudioToFirstVideoFrame(int64_t startQpcMs, int64_t startQpc100ns, bool preservePendingPackets = false) {
        audioSyncPending.store(true, std::memory_order_release);
        audioResetPreservePackets.store(preservePendingPackets, std::memory_order_release);
        recordingStartSystemQPCMs.store(startQpcMs, std::memory_order_release);
        recordingStartSystemQpc100ns.store(startQpc100ns, std::memory_order_release);
        const uint64_t generation = audioResetRequestedGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        DLL_Log(
            "[A/V START] Shared startup reset issued: generation=%llu startMs=%lld startQpc100ns=%lld "
            "sources=%zu preservePending=%d worker=%d",
            static_cast<unsigned long long>(generation), startQpcMs, startQpc100ns, audioSources.size(),
            preservePendingPackets ? 1 : 0, audioRunning.load(std::memory_order_acquire) ? 1 : 0);
        audioDrainCv.notify_all();

        bool resetAppliedByWorker = false;
        if (audioRunning.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(audioDrainMutex);
            audioDrainCv.wait(lock, [this, generation]() {
                return audioResetAcknowledgedGeneration.load(std::memory_order_acquire) >= generation ||
                       !audioRunning.load(std::memory_order_acquire);
            });
            resetAppliedByWorker =
                audioResetAcknowledgedGeneration.load(std::memory_order_acquire) >= generation;
        }
        if (!resetAppliedByWorker) {
            ApplyAudioTimelineReset(generation, startQpcMs, preservePendingPackets);
            audioResetAcknowledgedGeneration.store(generation, std::memory_order_release);
        }

        ResetAudioPullStateForRecording();
        audioResetCommittedGeneration.store(generation, std::memory_order_release);
        audioSyncPending.store(false, std::memory_order_release);
        preservePendingStartupAudioPackets.store(false, std::memory_order_release);
        audioDrainCv.notify_all();
        DLL_Log(
            "[A/V START] Shared startup reset committed: generation=%llu routeAcks=%zu encoderAndPullState=ready",
            static_cast<unsigned long long>(generation), audioSources.size());
    }

    // Pull Model: source counters are diagnostic/source-local; each exported
    // track advances from trackTimelineSamples so source order cannot change
    // the muxed timeline.
    std::vector<int64_t> encodedSamplesPerSource;
    std::map<int, int64_t> trackTimelineSamples;
    std::map<int, uint64_t> trackRealMixedSamples;
    std::map<int, uint64_t> trackFullSilenceSamples;
    std::map<int, uint64_t> trackPartialSilenceSamples;

    std::map<int, bool> trackWasSilent;
    std::map<int, uint64_t> trackSilentSamples;
    std::map<int, uint64_t> trackSilentChunks;
    std::map<int, uint64_t> trackSilenceTransitions;
    std::map<int, uint64_t> trackLastSilenceLogTick;
    std::map<int, bool> trackBootstrapComplete;
    std::map<int, bool> trackFirstPullAfterBootstrap;
    std::map<int, int> trackBootstrapWaitLogCounters;

    // Cached track→source index map, built once in StartRecording to avoid
    // per-frame reconstruction (~120 rebuilds/sec at 120fps).
    std::map<int, std::vector<size_t>> cachedTrackToSources;

    // PullAndEncodeAudio counters — reset per recording to avoid stale state
    int warpCount = 0;
    int dropLogCounter = 0;
    int driftLogCounter = 0;
    std::map<int, int> trackSyncCheckCounters;
    std::map<int, TrackAudioFormat> trackAudioFormats;

    // Per-recording frame log counters (avoid statics that leak across recordings)
    int injectFrameLogCount = 0;
    int screengrabFrameLogCount = 0;
    int silenceLogCounter = 0;
    int mixLogCounter = 0;

    int64_t GetLastVideoEncodeTimeUs() const {
        if (videoEnc)
            return videoEnc->GetLastFrameEncodeTimeUs();
        return 0;
    }

    int64_t GetLastFrameFenceWaitUs() const {
        if (videoEnc)
            return videoEnc->GetLastFrameFenceWaitUs();
        return 0;
    }

    bool WasLastFrameDeferred() const {
        if (videoEnc)
            return videoEnc->WasLastFrameDeferred();
        return false;
    }

    bool CanRepeatLastFrame() {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        return videoEnc && recording && firstVideoFrameMs != 0 && videoEnc->CanRepeatLastFrame();
    }

    void ResetRepeatFrameCache() {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (videoEnc) {
            videoEnc->ResetRepeatFrameCache();
        }
    }

    void ReleaseEncoderTextures() {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (videoEnc) {
            videoEnc->ReleasePreservedEncoderTextures();
        }
    }

    void UpdateVideoEncoderSharedMem(void* sharedMem, void* shmemBuffer) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        sharedMemLayout = (SharedMemoryLayout*)sharedMem;
        if (videoEnc) {
            videoEnc->SetSharedMem((SharedMemoryLayout*)sharedMem, (ShmemBuffer*)shmemBuffer);
        }
    }

    void SetSourcePrefers10BitHint(bool prefer10Bit) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (videoEnc) {
            DLL_Log("[VideoEncoder] SetSourcePrefers10Bit(%s)", prefer10Bit ? "true" : "false");
            videoEnc->SetSourcePrefers10Bit(prefer10Bit);
        }
    }

    void SetCursorCompositionSuppressedHint(bool suppressed) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (videoEnc) {
            DLL_Log("[VideoEncoder] Cursor composition %s (capture frames %s the cursor)",
                    suppressed ? "suppressed" : "active", suppressed ? "already contain" : "do not contain");
            videoEnc->SetCursorCompositionSuppressed(suppressed);
        }
    }

    void SetActiveScreenGrab(bool enabled) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        activeScreenGrab = enabled;
    }

    void SetAudioOnly(bool enabled) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        audioOnly = enabled;
    }

    void InitAudioOnlyMuxer(const AppConfig* config) {
        const std::filesystem::path exeDir = ce::capture_output::GetExecutableDirectory();
        const std::filesystem::path outDir =
            ce::capture_output::ResolveCaptureDirectory(config->video.outputDir, exeDir);
        audioOnlyOutputReservation =
            ce::capture_output::ReservedCaptureOutput::Reserve(outDir, L"capture_audio", L"mka");
        if (!audioOnlyOutputReservation) {
            DLL_Log("MediaEngine: Failed to reserve collision-safe audio-only output in %s",
                    outDir.string().c_str());
            audioOnlyFmtCtx = nullptr;
            return;
        }
        audioOnlyFilename = audioOnlyOutputReservation.Utf8Path();
        audioOnlyTrailerSucceeded = false;
        if (avformat_alloc_output_context2(&audioOnlyFmtCtx, nullptr, "matroska", audioOnlyFilename.c_str()) < 0) {
            DLL_Log("MediaEngine: Failed to create audio-only muxer");
            audioOnlyFmtCtx = nullptr;
            audioOnlyOutputReservation.CleanupOwnedFile();
            audioOnlyFilename.clear();
        }
    }

    void CleanupAudioOnlyMuxer() {
        int closeResult = 0;
        if (audioOnlyFmtCtx) {
            if (audioOnlyFmtCtx->pb) {
                closeResult = avio_closep(&audioOnlyFmtCtx->pb);
                if (closeResult < 0) {
                    DLL_Log("MediaEngine: Failed to close audio-only output: %d", closeResult);
                }
            }
            avformat_free_context(audioOnlyFmtCtx);
            audioOnlyFmtCtx = nullptr;
        }
        if (audioOnlyTrailerSucceeded && closeResult >= 0) {
            audioOnlyOutputReservation.Publish();
        } else {
            audioOnlyOutputReservation.CleanupOwnedFile();
        }
        audioOnlyTrailerSucceeded = false;
        audioOnlyFilename.clear();
    }

    // Trusted System QPC Frequency
    int64_t qpcFreq = 0;

    bool Init(const AppConfig* config) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        DLL_Log("MediaEngine::Init starting");

        // Initialize QPC Frequency
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq = f.QuadPart;
        DLL_Log("MediaEngine: Trusted QPC Frequency: %lld", qpcFreq);

        this->config = *config;
        trackAudioFormats = ResolveTrackAudioFormats(*config);
        DLL_Log("[AVSyncAuto] engine_config: resolvedRenderLatencyMs=%.3f confidence=%s reason=%s usedAudioProbe=%d",
                static_cast<double>(this->config.avSyncResolvedRenderLatencyMs), this->config.avSyncConfidence.c_str(),
                this->config.avSyncReason.c_str(), this->config.avSyncUsedAudioProbe ? 1 : 0);

        // Setup Video (Alloc Only) - skip for audio-only
        if (audioOnly) {
            DLL_Log("MediaEngine::Init audio-only mode - skipping VideoEncoder");
            videoEnc = nullptr;
            InitAudioOnlyMuxer(config);
        } else {
            DLL_Log("MediaEngine::Init creating VideoEncoder");
            videoEnc = std::make_unique<VideoEncoder>();
            DLL_Log("MediaEngine::Init calling VideoEncoder::Init");
            bool vRes = videoEnc->Init(config->video, 0, 0, config->video.fps,
                                       [this](AVPacket* pkt) { this->WritePacket(pkt); });
            if (!vRes) {
                DLL_Log("MediaEngine::Init VideoEncoder init failed");
                return false;
            }
            DLL_Log("MediaEngine::Init VideoEncoder initialized OK");
        }

        // Setup Audio Sources (supports multiple: system audio, microphone, etc.)
        DLL_Log("MediaEngine::Init audio sources count=%d", (int)config->audioSources.size());

        // Maps track number to encoder for that track
        std::map<int, AudioEncoder*> trackToEncoder;
        // Guards against summing two identical app-audio captures into one track.
        std::set<std::string> seenAppAudioTrackKeys;

        for (size_t i = 0; i < config->audioSources.size(); i++) {
            const AudioConfig& audioConfig = config->audioSources[i];
            if (!audioConfig.enabled) {
                DLL_Log("MediaEngine::Init audio source %zu disabled", i);
                continue;
            }

            DLL_Log(
                "MediaEngine::Init setting up audio source %zu (type=%d device=%s "
                "process=%s)",
                i, (int)audioConfig.sourceType, audioConfig.device.empty() ? "default" : audioConfig.device.c_str(),
                audioConfig.processName.empty() ? "N/A" : audioConfig.processName.c_str());

            // Get the list of tracks this source should output to
            std::vector<int> targetTracks = audioConfig.tracks;
            if (targetTracks.empty()) {
                // Default: use track (i+1)
                targetTracks.push_back((int)(i + 1));
            }

            DLL_Log("MediaEngine::Init Audio source %zu targets %zu tracks", i, targetTracks.size());

            // For each target track, create or reuse an encoder
            for (int track : targetTracks) {
                // Defense in depth: never create a second app-audio capture for the
                // same process on the same track. Summing identical captures combs.
                if (audioConfig.sourceType == AudioConfig::AppAudio) {
                    const std::string appKey = AppAudioTrackKey(audioConfig, track);
                    if (!seenAppAudioTrackKeys.insert(appKey).second) {
                        DLL_Log(
                            "MediaEngine::Init WARNING: duplicate app-audio source (process='%s' processId=%lu) "
                            "already targets track %d - skipping duplicate capture to avoid comb-filter artifacts",
                            audioConfig.processName.empty() ? "<pid>" : audioConfig.processName.c_str(),
                            (unsigned long)audioConfig.processId, track);
                        continue;
                    }
                }
                TrackAudioFormat trackFormat = GetTrackAudioFormat(track);
                AudioConfig resolvedAudioConfig = audioConfig;
                resolvedAudioConfig.outputChannels = audioConfig.downmix ? 2 : trackFormat.channels;
                resolvedAudioConfig.outputChannelMask =
                    audioConfig.downmix ? DefaultChannelMaskForChannels(2) : trackFormat.channelMask;
                // Check if we already have an encoder for this track
                AudioEncoder* encoderForTrack = nullptr;
                auto it = trackToEncoder.find(track);
                if (it != trackToEncoder.end()) {
                    encoderForTrack = it->second;
                    DLL_Log("MediaEngine::Init Audio source %zu reusing encoder for track %d", i, track);
                } else {
                    // Create new encoder for this track
                    auto newEncoder = std::make_unique<AudioEncoder>();
                    bool aRes =
                        newEncoder->Init(resolvedAudioConfig, [this](AVPacket* pkt) { this->WritePacket(pkt); });

                    if (!aRes) {
                        DLL_Log("MediaEngine::Init Audio encoder for track %d failed", track);
                        continue;
                    }

                    // Register with VideoEncoder for stream creation (skip in audio-only)
                    if (!audioOnly) {
                        videoEnc->AddAudioContext(resolvedAudioConfig, newEncoder->GetCodecContext(), track);
                    }

                    encoderForTrack = newEncoder.get();
                    trackToEncoder[track] = encoderForTrack;

                    // Create AudioSource to own this encoder
                    AudioSource source;
                    source.config = resolvedAudioConfig;
                    source.track = track;
                    source.configuredSourceIndex = i;
                    source.sourceType = audioConfig.sourceType;
                    source.mixChannels =
                        resolvedAudioConfig.outputChannels > 0 ? resolvedAudioConfig.outputChannels : 2;
                    source.mixChannelMask = resolvedAudioConfig.outputChannelMask != 0
                                                ? resolvedAudioConfig.outputChannelMask
                                                : DefaultChannelMaskForChannels(source.mixChannels);
                    source.encoder = std::move(newEncoder);
                    source.sharedEncoderPtr = source.encoder.get();

                    // Set video time getter for clock drift compensation
                    source.encoder->SetVideoTimeGetter([this]() { return GetVideoElapsedMs(); });

                    // Create appropriate capture type
                    if (audioConfig.sourceType == AudioConfig::AppAudio) {
                        source.appCapture = std::make_unique<ProcessLoopbackCapture>();
                        source.appCapture->SetRequestedFormat(48000, source.mixChannels, source.mixChannelMask);
                    } else {
                        source.capture = std::make_unique<AudioCapture>();
                    }

                    // INIT RING BUFFER AND SYNC RESAMPLER (Per-source drift compensation)
                    InitAudioSourceBuffers(source, audioConfig, i);

                    DLL_Log(
                        "MediaEngine::Init Created new encoder for track %d (source "
                        "%zu, type=%d)",
                        track, i, (int)audioConfig.sourceType);
                    audioSources.push_back(std::move(source));
                }

                // If this source doesn't own the encoder, create a source entry that
                // shares it
                if (it != trackToEncoder.end()) {
                    AudioSource source;
                    source.config = audioConfig;
                    source.config = resolvedAudioConfig;
                    source.track = track;
                    source.configuredSourceIndex = i;
                    source.sourceType = audioConfig.sourceType;
                    source.mixChannels =
                        resolvedAudioConfig.outputChannels > 0 ? resolvedAudioConfig.outputChannels : 2;
                    source.mixChannelMask = resolvedAudioConfig.outputChannelMask != 0
                                                ? resolvedAudioConfig.outputChannelMask
                                                : DefaultChannelMaskForChannels(source.mixChannels);
                    source.encoder = nullptr;  // Shared, not owned
                    source.sharedEncoderPtr = encoderForTrack;

                    // Create appropriate capture type
                    if (audioConfig.sourceType == AudioConfig::AppAudio) {
                        source.appCapture = std::make_unique<ProcessLoopbackCapture>();
                        source.appCapture->SetRequestedFormat(48000, source.mixChannels, source.mixChannelMask);
                    } else {
                        source.capture = std::make_unique<AudioCapture>();
                    }

                    // INIT RING BUFFER AND SYNC RESAMPLER (Per-source drift compensation)
                    InitAudioSourceBuffers(source, audioConfig, i);

                    DLL_Log(
                        "MediaEngine::Init Audio source %zu shares encoder for track "
                        "%d (type=%d)",
                        i, track, (int)audioConfig.sourceType);
                    audioSources.push_back(std::move(source));
                }
            }
        }

        CoalesceCaptureRoutes();

        DLL_Log("MediaEngine::Init complete. Audio sources: %zu, unique tracks: %zu", audioSources.size(),
                trackToEncoder.size());

        // Audio-only: create muxer streams for each audio track
        if (audioOnly && audioOnlyFmtCtx) {
            for (auto& kv : trackToEncoder) {
                int track = kv.first;
                AudioEncoder* enc = kv.second;
                AVStream* stream = avformat_new_stream(audioOnlyFmtCtx, enc->GetCodecContext()->codec);
                if (stream) {
                    stream->id = track;
                    stream->time_base = enc->GetCodecContext()->time_base;
                    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
                    avcodec_parameters_from_context(stream->codecpar, enc->GetCodecContext());
                    DLL_Log("MediaEngine: Created audio-only muxer stream for track %d (stream idx %d)", track,
                            stream->index);
                }
            }
        }

        return true;
    }

    int64_t GetLastVideoFenceWaitUs() const {
        if (videoEnc)
            return videoEnc->GetLastFrameFenceWaitUs();
        return 0;
    }

    void ResetAudioPullStateForRecording() {
        encodedSamplesPerSource.assign(audioSources.size(), 0);
        trackTimelineSamples.clear();
        trackRealMixedSamples.clear();
        trackFullSilenceSamples.clear();
        trackPartialSilenceSamples.clear();
        trackWasSilent.clear();
        trackSilentSamples.clear();
        trackSilentChunks.clear();
        trackSilenceTransitions.clear();
        trackLastSilenceLogTick.clear();
        trackBootstrapComplete.clear();
        trackFirstPullAfterBootstrap.clear();
        trackBootstrapWaitLogCounters.clear();
        cachedTrackToSources.clear();
        for (size_t i = 0; i < audioSources.size(); ++i) {
            auto& src = audioSources[i];
            if (!src.ringBuffer || !src.sharedEncoderPtr) {
                continue;
            }
            cachedTrackToSources[src.track].push_back(i);
            trackTimelineSamples[src.track] = 0;
            trackRealMixedSamples[src.track] = 0;
            trackFullSilenceSamples[src.track] = 0;
            trackPartialSilenceSamples[src.track] = 0;
        }

        warpCount = 0;
        dropLogCounter = 0;
        driftLogCounter = 0;
        trackSyncCheckCounters.clear();
        injectFrameLogCount = 0;
        screengrabFrameLogCount = 0;
        silenceLogCounter = 0;
        mixLogCounter = 0;
    }

    bool StartRecording() {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (recording)
            return true;
        processLoopbackIntegrityFailureSignaled = false;
        if (sharedMemLayout) {
            sharedMemLayout->runtimeState.recordingFailureCode.store(
                static_cast<uint32_t>(RecordingFailureCode::None), std::memory_order_release);
        }

        // Audio-only: open muxer, write header, skip video pipeline entirely
        if (audioOnly) {
            if (!audioOnlyFmtCtx) {
                DLL_Log("MediaEngine: Audio-only muxer not initialized");
                return false;
            }
            if (!audioOnlyOutputReservation.ReleaseToWriter()) {
                DLL_Log("MediaEngine: Reserved audio-only output identity changed before mux open: %s",
                        audioOnlyFilename.c_str());
                return false;
            }
            if (avio_open(&audioOnlyFmtCtx->pb, audioOnlyFilename.c_str(), AVIO_FLAG_WRITE) < 0) {
                DLL_Log("MediaEngine: Failed to open audio-only output file: %s", audioOnlyFilename.c_str());
                return false;
            }
            if (!ce::media::RequireMicrosecondMatroskaTimestampPrecision(audioOnlyFmtCtx)) {
                DLL_Log("MediaEngine: Matroska timestamp_precision=1000 is required for audio-only output");
                const int closeResult = avio_closep(&audioOnlyFmtCtx->pb);
                if (closeResult < 0)
                    DLL_Log("MediaEngine: Failed to close rejected audio-only output: %d", closeResult);
                audioOnlyOutputReservation.CleanupOwnedFile();
                return false;
            }
            AVDictionary* opts = nullptr;
            const int headerResult = avformat_write_header(audioOnlyFmtCtx, &opts);
            av_dict_free(&opts);
            if (headerResult < 0) {
                DLL_Log("MediaEngine: Failed to write audio-only header: %d", headerResult);
                const int closeResult = avio_closep(&audioOnlyFmtCtx->pb);
                if (closeResult < 0)
                    DLL_Log("MediaEngine: Failed to close audio-only output after header failure: %d", closeResult);
                audioOnlyOutputReservation.CleanupOwnedFile();
                return false;
            }
            DLL_Log("MediaEngine: Audio-only recording writing to %s", audioOnlyFilename.c_str());

            // Set stream indices from muxer - only on OWNER encoders (unique)
            {
                int ownerCount = 0;
                for (unsigned int si = 0; si < audioOnlyFmtCtx->nb_streams; si++) {
                    int targetTrack = audioOnlyFmtCtx->streams[si]->id;
                    for (auto& src : audioSources) {
                        if (src.encoder && src.track == targetTrack) {
                            src.encoder->SetStreamIndex((int)si);
                            DLL_Log("[AudioSetup] Stream %d -> track %d encoder %p", si, targetTrack,
                                    (void*)src.encoder.get());
                            ownerCount++;
                            break;
                        }
                    }
                }
                DLL_Log("[AudioSetup] %d owner encoders assigned, %d streams", ownerCount,
                        (int)audioOnlyFmtCtx->nb_streams);
            }

            audioSyncPending.store(false);
            audioFinalizingCfrStop.store(false, std::memory_order_release);
            audioStopDrainRequested.store(false, std::memory_order_release);
            audioStopDrainComplete.store(false, std::memory_order_release);
            preservePendingStartupAudioPackets.store(false, std::memory_order_release);
            ResetAudioPullStateForRecording();

            LARGE_INTEGER audioStartQpc{};
            QueryPerformanceCounter(&audioStartQpc);
            const int64_t audioStartQpcMs =
                qpcFreq > 0 ? (audioStartQpc.QuadPart * 1000) / qpcFreq : static_cast<int64_t>(GetTickCount64());
            const int64_t audioStartQpc100ns =
                qpcFreq > 0 ? static_cast<int64_t>(ce::audio::RawQpcToHundredNanoseconds(
                                  static_cast<uint64_t>(audioStartQpc.QuadPart), static_cast<uint64_t>(qpcFreq)))
                            : 0;
            SyncAudioToFirstVideoFrame(audioStartQpcMs, audioStartQpc100ns);
            recordingStartTime = std::chrono::steady_clock::now();
            recording = true;

            int startedCount = 0;
            for (size_t srcIdx = 0; srcIdx < audioSources.size(); ++srcIdx) {
                auto& src = audioSources[srcIdx];
                if (src.captureFanoutOwnerIndex != srcIdx) {
                    DLL_Log("[AudioRoute] Audio-only route src=%zu track=%d uses capture owner=%zu", srcIdx, src.track,
                            src.captureFanoutOwnerIndex);
                    continue;
                }
                bool started = false;
                if (src.sourceType == AudioConfig::AppAudio && src.appCapture) {
                    if (!src.config.processName.empty()) {
                        started = src.appCapture->StartByName(src.config.processName);
                    } else if (src.config.processId != 0) {
                        started = src.appCapture->StartByPID(src.config.processId);
                    }
                } else if (src.capture) {
                    bool isLoopback = (src.sourceType == AudioConfig::SystemAudio);
                    started = src.capture->Start(src.config.device, isLoopback);
                }
                if (started)
                    startedCount++;
            }
            // Collect unique encoder pointers for track padding
            trackEncoders.clear();
            int encCount = 0, shmCount = 0;
            for (auto& src : audioSources) {
                if (src.encoder) {
                    encCount++;
                    bool dup = false;
                    for (auto* e : trackEncoders) {
                        if (e == src.encoder.get()) {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup)
                        trackEncoders.push_back(src.encoder.get());
                }
                if (src.sharedEncoderPtr)
                    shmCount++;
            }
            DLL_Log("[StartAudio] trackEncoders: %zu unique from %d owners (%d shared), %zu sources",
                    trackEncoders.size(), encCount, shmCount, audioSources.size());
            if (startedCount > 0) {
                if (!StartAudioThread()) {
                    recording = false;
                    for (auto* encoder : trackEncoders) {
                        if (encoder) {
                            encoder->Stop();
                        }
                    }
                    if (audioOnlyFmtCtx) {
                        audioOnlyTrailerSucceeded = av_write_trailer(audioOnlyFmtCtx) >= 0;
                        CleanupAudioOnlyMuxer();
                    }
                    DLL_Log("MediaEngine: Audio-only recording start failed because the audio worker could not start");
                    return false;
                }
            }

            DLL_Log("MediaEngine: Audio-only recording started (%d audio source(s))", startedCount);
            return true;
        }

        if (!videoEnc)
            return false;
        for (auto& src : audioSources) {
            if (src.encoder) {
                // If encoder failed to reinit during Stop(), reinit now
                if (!src.encoder->IsReady()) {
                    DLL_Log("MediaEngine: Audio encoder not ready, attempting reinit");
                    if (!src.encoder->Reinit()) {
                        DLL_Log("MediaEngine: Audio encoder reinit failed, skipping");
                        continue;
                    }
                    DLL_Log("MediaEngine: Audio encoder reinit successful");
                }
                videoEnc->AddAudioContext(src.config, src.encoder->GetCodecContext(), src.track);
                src.sharedEncoderPtr = src.encoder.get();
            }
        }
        // Shared sources must re-acquire the pointer from their reference
        // (Actually simpler: just re-iterate and update sharedEncoderPtr?
        // No, sharedEncoderPtr points to local AudioEncoder instance in another
        // source. That instance is stable, but its internal codecCtx execution
        // pointer changed. VideoEncoder needs the new CodecCtx. audioSources
        // already have the right encoder object.)

        // Start Video (Write Header / Open File)
        if (!videoEnc->Start())
            return false;

        // Audio stream is now added in EnsureDevice after video stream
        // We don't add it here anymore - just set the index when it becomes
        // available The stream index will be set after first frame in ProcessFrame
        timingModeFrozenForSession = true;
        sessionUseVfr = config.video.useVFR;
        const bool preserveWgcStartupQueues = IsWgcCfrRecording();

        // Start Audio Capture and Processing Thread
        if (!audioSources.empty()) {
            // NOTE: Don't set recording start time here!
            // We defer this to the first video frame in ProcessFrameD3D11 for perfect
            // A/V sync. Audio data captured before first video frame will be
            // discarded. IMPORTANT: Prevent ringbuffer from filling/overflowing
            // before first video frame. We intentionally discard audio until the
            // first video frame establishes the timeline.
            audioSyncPending.store(true);
            audioFinalizingCfrStop.store(false, std::memory_order_release);
            audioStopDrainRequested.store(false, std::memory_order_release);
            audioStopDrainComplete.store(false, std::memory_order_release);
            preservePendingStartupAudioPackets.store(preserveWgcStartupQueues, std::memory_order_release);
            if (preserveWgcStartupQueues) {
                DLL_Log(
                    "[A/V START] WGC CFR startup queue preservation armed until first video anchor "
                    "(bounded by capture queue depth; pre-anchor packets are filtered by QPC)");
            }

            // CRITICAL: Reset video clock for new recording to prevent stale
            // timestamps
            firstVideoFrameMs = 0;               // Reset for new recording
            videoElapsedMs.store(0);             // CRITICAL: Reset video clock for new recording
                                                 // to prevent stale timestamps
            recordingStartSystemQPCMs.store(0);  // CRITICAL: Reset QPC start time for new recording
            recordingStartSystemQpc100ns.store(0);
            wgcStartupExtraDelayQpc.store(0, std::memory_order_release);
            injectTimelineState.Reset();
            d3d11TimelineState.Reset();

            // PULL MODEL: Reset audio encoding state for new recording.
            ResetAudioPullStateForRecording();

            // PULL MODEL: CRITICAL - Clear ring buffers to start fresh
            for (auto& src : audioSources) {
                if (src.ringBuffer) {
                    size_t prevAvail = src.ringBuffer->GetAvailable();
                    src.ringBuffer->Clear();
                    if (prevAvail > 0) {
                        DLL_Log("MediaEngine: Cleared stale ringBuffer with %zu samples", prevAvail);
                    }
                }

                // DRIFT COMPENSATION: Reset sync state for new recording
                if (src.syncResampler) {
                    src.syncResampler->Reset();
                }
                // Reset format-conversion resampler so no SWR state from the
                // previous recording bleeds into the new one.
                src.resampler.reset();
                src.postResampleBuffer.clear();
                src.syncSamplesOutput = 0;
                src.dropFadeSamplesRemaining = 0;
                src.dropFadeStartL = 0.0f;
                src.dropFadeStartR = 0.0f;
                src.dropFadeStart.clear();
                src.underrunFadeSamplesRemaining = 0;
                src.packetBoundaryFadeInSamplesRemaining = 0;
                src.overflowDropSamples = 0;
                src.retainedNewestTrimSamples = 0;
                src.latencyTrimSamples = 0;
                src.tier2TrimSamples = 0;
                src.bootstrapTrimSamples = 0;
                src.postResampleTrimSamples = 0;
                src.underrunPadSamples = 0;
                src.coverageLossTrimSamples = 0;
                src.startupSyntheticRingSamples = 0;
                src.startupSyntheticResamplerSamples = 0;
                src.startupSyntheticPostSamples = 0;
                src.startupGapProtectionSamples = 0;
                src.qpcAlignedWrittenSamples = 0;
                src.packetTimelineGapSamples = 0;
                src.packetTimelineOverlapSamples = 0;
                src.startupRebasedGapSamples = 0;
                src.lateAppJoinSuppressedGapSamples = 0;
                src.lateAppJoinPreservedGapSamples = 0;
                src.alignedStartMs = -1;
                src.observedLateStartMs = 0;
                src.hasAlignedStart = false;
                src.sawCaptureEpoch = false;
                src.timelineValid = false;
                src.isPrimed = false;
                src.bootstrapComplete = false;
                src.pendingUnderrunRecoveryFade = false;
                src.sawSyncPendingPackets = false;
                src.startupRealAudioSeen = false;
                src.pendingStartupJoinFade = false;
                src.pendingRetainedTrimSamples = 0;
                src.pendingRetainedTrimEvents = 0;
                src.pendingLatencyTrimSamples = 0;
                src.pendingLatencyTrimEvents = 0;
                src.pendingTier2TrimSamples = 0;
                src.pendingTier2TrimEvents = 0;
                src.pendingCoverageLossTrimSamples = 0;
                src.pendingCoverageLossTrimEvents = 0;
                src.lastAppPlaceDiagTick = 0;
                src.lastAppConsumeDiagTick = 0;
                src.appLatencyBuckets[0] = 0;
                src.appLatencyBuckets[1] = 0;
                src.appLatencyBuckets[2] = 0;
                src.appLatencyBuckets[3] = 0;
                src.appLatencyBuckets[4] = 0;
                src.appLatencySampleCount = 0;
                src.appLatencySumMs = 0;
                src.appLatencyMaxMs = 0;
                src.appLatencyTargetSumMs = 0;
                src.appLatencyExcessSumMs = 0;
                src.appLatencyExcessMaxMs = 0;
                src.appLatencyDrainingSamples = 0;
                src.appLatencyDrainTransitions = 0;
                src.appLatencyMaxAbsCompDelta = 0;
                src.lastAppLatencyWarnTick = 0;
                src.appLatencyWarnActive = false;
                src.appAudioBacklogDrainInitialized = false;
                src.appAudioBacklogDrainActive = false;
                src.appAudioBacklogDrainReason =
                    static_cast<uint32_t>(ce::audio::CfrAppAudioBacklogDrainReason::WithinSlack);
                src.appAudioBacklogTargetSamples = 0;
                src.appAudioBacklogExcessSamples = 0;
                src.appAudioBacklogCompensationDelta = 0;
                src.catastrophicResyncSamples = 0;
                src.catastrophicResyncEvents = 0;
                src.lastCatastrophicResyncTick = 0;
                src.lastRetainedTrimWarnTick = 0;
                src.lastPacketTimelineAdjustWarnTick = 0;
                src.wgcCoverageLossTrimAccumulator = 0.0;
                src.prevLeadSamples = 0;
                src.prevLeadSnapshotMs = 0;
                src.lastRateUpdateMs = 0;
                src.currentRateDelta = 0;
                src.targetRateDelta = 0;
                src.rateCompActive = false;
                src.targetRateSaturated = false;
                src.epochResetRequested->store(0, std::memory_order_release);
                src.epochResetAcknowledged->store(0, std::memory_order_release);
                src.epochSyncTailFlushedGeneration = 0;
                src.lastEpochTransitionWaitLogTick = 0;
            }

            // Start all audio sources
            int startedCount = 0;
            for (size_t srcIdx = 0; srcIdx < audioSources.size(); ++srcIdx) {
                auto& src = audioSources[srcIdx];
                // Recording start will be set when first video frame arrives

                if (src.captureFanoutOwnerIndex != srcIdx) {
                    DLL_Log("[AudioRoute] Route src=%zu track=%d uses capture owner=%zu", srcIdx, src.track,
                            src.captureFanoutOwnerIndex);
                    continue;
                }

                bool started = false;

                if (src.sourceType == AudioConfig::AppAudio && src.appCapture) {
                    // Start per-app audio capture
                    if (!src.config.processName.empty()) {
                        started = src.appCapture->StartByName(src.config.processName);
                        DLL_Log(
                            "MediaEngine: App audio source starting for process '%s' "
                            "(track=%d)",
                            src.config.processName.c_str(), src.track);
                    } else if (src.config.processId != 0) {
                        started = src.appCapture->StartByPID(src.config.processId);
                        DLL_Log("MediaEngine: App audio source starting for PID %lu (track=%d)", src.config.processId,
                                src.track);
                    }
                } else if (src.capture) {
                    // Start regular capture: loopback for system audio, device for
                    // microphone
                    bool isLoopback = (src.sourceType == AudioConfig::SystemAudio);
                    started = src.capture->Start(src.config.device, isLoopback);
                    DLL_Log("MediaEngine: Audio source started (track=%d, type=%d)", src.track, (int)src.sourceType);
                }

                if (started) {
                    startedCount++;
                } else {
                    DLL_Log("MediaEngine: Audio source failed to start (track=%d, type=%d)", src.track,
                            (int)src.sourceType);
                }
            }

            if (startedCount > 0) {
                DLL_Log(
                    "MediaEngine: %d audio source(s) started (sync pending first "
                    "video frame)",
                    startedCount);
                if (!StartAudioThread()) {
                    DLL_Log("MediaEngine: Audio worker unavailable; continuing video recording without live audio");
                }
            }
        }

        recording = true;
        return true;
    }

    void StopRecording() {
        // Audio-only: stop audio thread, write trailer, clean up
        if (audioOnly) {
            {
                std::lock_guard<std::recursive_mutex> lock(muxMutex);
                if (!recording)
                    return;
            }
            // Stop WASAPI/process-loopback first while AudioLoop is still
            // alive. Stop(false) performs each source's one-shot final drain;
            // the CV handshake then proves those committed packets were
            // consumed before the loop and muxer are torn down.
            const size_t preservedStopPackets = audioRunning.load(std::memory_order_acquire)
                                                    ? StopCaptureSourcesAndDrainAudioLoop()
                                                    : StopAudioCaptureSources(false);
            audioRunning = false;
            audioDrainCv.notify_all();
            if (audioThread.joinable())
                audioThread.join();
            {
                std::lock_guard<std::recursive_mutex> lock(muxMutex);
                recording = false;
            }
            DLL_Log("[StopAudio] Audio-only source tail preserved before loop shutdown: packets=%zu",
                    preservedStopPackets);

            // Pad unique stream encoders to equal length
            std::map<int, AudioEncoder*> streamEnc2;
            for (auto& src2 : audioSources) {
                if (src2.encoder) {
                    int si = src2.encoder->GetStreamIndex();
                    if (si >= 0)
                        streamEnc2[si] = src2.encoder.get();
                }
            }
            if (streamEnc2.size() > 1) {
                constexpr int kChunkSamples = 4096;
                int64_t target = 0;
                for (auto& [si, enc] : streamEnc2) {
                    int64_t s = enc->GetSamplesCount();
                    if (s > target)
                        target = s;
                }
                target = ((target + kChunkSamples - 1) / kChunkSamples) * kChunkSamples;
                for (auto& [si, enc] : streamEnc2) {
                    AVCodecContext* ctx = enc->GetCodecContext();
                    const int channels =
                        std::clamp(ctx && ctx->ch_layout.nb_channels > 0 ? ctx->ch_layout.nb_channels : 2, 1, 8);
                    std::vector<float> silence(kChunkSamples * channels, 0.0f);
                    int64_t cur = enc->GetSamplesCount();
                    int64_t pad = target - cur;
                    if (pad > 0) {
                        int64_t remaining = pad;
                        while (remaining > 0) {
                            int chunk = (int)(std::min)(remaining, (int64_t)kChunkSamples);
                            enc->EncodeSamples((const uint8_t*)silence.data(), chunk * channels * (int)sizeof(float),
                                               channels, 48000, 32, 32, channels * 4, true, GetTickCount64());
                            remaining -= chunk;
                        }
                    }
                }
                // Converge until all match
                for (int iter = 0; iter < 10; iter++) {
                    target = 0;
                    for (auto& [si, enc] : streamEnc2) {
                        int64_t s = enc->GetSamplesCount();
                        if (s > target)
                            target = s;
                    }
                    bool allMatch = true;
                    for (auto& [si, enc] : streamEnc2) {
                        int64_t cur = enc->GetSamplesCount();
                        int64_t pad = target - cur;
                        if (pad > 0) {
                            allMatch = false;
                            AVCodecContext* ctx = enc->GetCodecContext();
                            const int channels = std::clamp(
                                ctx && ctx->ch_layout.nb_channels > 0 ? ctx->ch_layout.nb_channels : 2, 1, 8);
                            std::vector<float> silence(kChunkSamples * channels, 0.0f);
                            int64_t remaining = pad;
                            while (remaining > 0) {
                                int chunk = (int)(std::min)(remaining, (int64_t)kChunkSamples);
                                enc->EncodeSamples((const uint8_t*)silence.data(),
                                                   chunk * channels * (int)sizeof(float), channels, 48000, 32, 32,
                                                   channels * 4, true, GetTickCount64());
                                remaining -= chunk;
                            }
                            DLL_Log("[StopAudio] Convergence iter %d: stream %d +%lld to %lld", iter, si,
                                    (long long)pad, (long long)target);
                        }
                    }
                    if (allMatch)
                        break;
                }
                for (auto& [si, enc] : streamEnc2) {
                    DLL_Log("[StopAudio] Final stream %d: %lld samples", si, (long long)enc->GetSamplesCount());
                }
            }
            // Stop each unique encoder exactly once
            for (auto& [si, enc] : streamEnc2) {
                for (const auto& src : audioSources) {
                    if (src.encoder.get() == enc) {
                        enc->SetExpectedSourceSilenceSamples(
                            static_cast<int64_t>(trackFullSilenceSamples[src.track]));
                        break;
                    }
                }
                enc->Stop();
            }
            for (auto& src : audioSources) {
                if (src.ringBuffer)
                    src.ringBuffer->Clear();
                if (src.syncResampler)
                    src.syncResampler->Reset();
            }
            if (audioOnlyFmtCtx) {
                audioOnlyTrailerSucceeded = av_write_trailer(audioOnlyFmtCtx) >= 0;
                if (!audioOnlyTrailerSucceeded) {
                    DLL_Log("[StopAudio] ERROR: Audio-only trailer write failed");
                }
                DLL_Log("[StopAudio] Audio-only recording finalized: %s", audioOnlyFilename.c_str());
                CleanupAudioOnlyMuxer();
            }
            firstVideoFrameMs = 0;
            lastVideoFrameMs = 0;
            recordingStartSystemQPCMs.store(0);
            recordingStartSystemQpc100ns.store(0);
            injectTimelineState.Reset();
            d3d11TimelineState.Reset();
            audioOnly = false;
            DLL_Log(
                "[AVSyncAuto] stop_summary: audioOnly=1 resolvedRenderLatencyMs=%.3f confidence=%s reason=%s "
                "usedAudioProbe=%d",
                static_cast<double>(config.avSyncResolvedRenderLatencyMs), config.avSyncConfidence.c_str(),
                config.avSyncReason.c_str(), config.avSyncUsedAudioProbe ? 1 : 0);
            DLL_Log("[STOP SUMMARY] Audio-only recording finalized");
            return;
        }

        ExtendCfrToCommonAudioLattice();

        int64_t endUs = 0;
        constexpr int kStopSampleRate = 48000;
        const int64_t expectedVideoUsForStop = videoEnc ? videoEnc->GetExpectedFinalDurationUs() : 0;
        const bool finalCfrStopPending = ce::audio::ShouldDrainStoppedCaptureQueuesBeforeFinalAudioPull(
                                             audioRunning.load(), audioOnly, expectedVideoUsForStop) &&
                                         IsCfrRecording();
        if (finalCfrStopPending) {
            audioFinalizingCfrStop.store(true, std::memory_order_release);
            DLL_Log("[StopAudio] Final CFR stop pending: targetUs=%lld", expectedVideoUsForStop);
        }
        WaitForFinalCfrAudioSourceCatchup(expectedVideoUsForStop);
        DrainStoppedCaptureQueuesBeforeFinalPull(expectedVideoUsForStop);
        {
            std::lock_guard<std::recursive_mutex> lock(muxMutex);
            if (!recording) {
                audioFinalizingCfrStop.store(false, std::memory_order_release);
                return;
            }

            // Set recording end timestamp on all audio encoders BEFORE stopping
            // the audio thread so the final drain and flush both target the exact
            // encoded video length.
            if (videoEnc) {
                int64_t expectedDurationUs = videoEnc->GetExpectedFinalDurationUs();
                int64_t encodedDurationUs = videoEnc->GetEncodedDurationUs();
                int64_t durationUs = expectedDurationUs > 0 ? expectedDurationUs : encodedDurationUs;
                if (IsCfrRecording()) {
                    const int64_t wallDurationUs = std::max<int64_t>(
                        IsWgcCfrRecording() ? d3d11TimelineState.lastElapsedUs : injectTimelineState.lastElapsedUs,
                        videoElapsedMs.load() * 1000);

                    DLL_Log(
                        "MediaEngine: CFR stop durations. Expected: %lld us, Encoded: %lld us, Wall: %lld us, "
                        "Selected: %lld us",
                        expectedDurationUs, encodedDurationUs, wallDurationUs, durationUs);
                }
                endUs = durationUs;

                DLL_Log(
                    "MediaEngine: Setting audio end time. Start: %lld us, Dur: %lld "
                    "us, End: %lld us",
                    0LL, durationUs, endUs);

                for (auto& src : audioSources) {
                    if (src.sharedEncoderPtr) {
                        src.sharedEncoderPtr->SetRecordingEndUs(endUs);
                    }
                }

                if (endUs > 0) {
                    // CFR: use PTS-based target so audio matches video exactly.
                    // The encoder thread's drain already covers most audio; this final
                    // pull fills any remaining gap without racing (drain is done by now).
                    // PullAndEncodeAudio intentionally limits very large gaps to bounded
                    // chunks, so force-drain loops until every exported track reaches the
                    // selected video endpoint or forward progress genuinely stops.
                    const int64_t cfrAudioTargetUs = IsCfrRecording() ? videoEnc->GetExpectedFinalDurationUs() : endUs;
                    const int64_t cfrAudioTargetSamples =
                        ce::audio::ComputeDurationUsToSamples(cfrAudioTargetUs, kStopSampleRate);
                    auto minTrackCursorSamples = [this]() -> int64_t {
                        if (cachedTrackToSources.empty()) {
                            return 0;
                        }
                        int64_t minSamples = std::numeric_limits<int64_t>::max();
                        for (const auto& kv : cachedTrackToSources) {
                            const auto trackIt = trackTimelineSamples.find(kv.first);
                            const int64_t trackSamples = trackIt != trackTimelineSamples.end() ? trackIt->second : 0;
                            minSamples = std::min(minSamples, trackSamples);
                        }
                        return minSamples == std::numeric_limits<int64_t>::max() ? 0 : minSamples;
                    };
                    const int64_t minEncodedBefore = minTrackCursorSamples();
                    int64_t minEncodedAfter = minEncodedBefore;
                    const int64_t missingBefore = std::max<int64_t>(0, cfrAudioTargetSamples - minEncodedBefore);
                    const int64_t maxForceDrainIterations =
                        std::max<int64_t>(1, (missingBefore + (kStopSampleRate / 2) - 1) / (kStopSampleRate / 2) + 4);
                    int64_t forceDrainIterations = 0;
                    for (int64_t attempt = 0; attempt < maxForceDrainIterations; ++attempt) {
                        const int64_t beforeIteration = minTrackCursorSamples();
                        if (beforeIteration >= cfrAudioTargetSamples) {
                            minEncodedAfter = beforeIteration;
                            break;
                        }
                        PullAndEncodeAudio(cfrAudioTargetUs, true);
                        ++forceDrainIterations;
                        const int64_t afterIteration = minTrackCursorSamples();
                        minEncodedAfter = afterIteration;
                        if (afterIteration >= cfrAudioTargetSamples) {
                            break;
                        }
                        if (afterIteration <= beforeIteration) {
                            DLL_Log(
                                "[StopAudio] WARNING: forceDrain made no progress: targetUs=%lld targetSamples=%lld "
                                "minTrack=%lld iteration=%lld/%lld",
                                cfrAudioTargetUs, cfrAudioTargetSamples, afterIteration, forceDrainIterations,
                                maxForceDrainIterations);
                            break;
                        }
                    }
                    if (minEncodedAfter < cfrAudioTargetSamples) {
                        DLL_Log(
                            "[StopAudio] WARNING: forceDrain incomplete: targetUs=%lld targetSamples=%lld "
                            "minTrackAfter=%lld missing=%lld iterations=%lld/%lld",
                            cfrAudioTargetUs, cfrAudioTargetSamples, minEncodedAfter,
                            cfrAudioTargetSamples - minEncodedAfter, forceDrainIterations, maxForceDrainIterations);
                    }
                    DLL_Log(
                        "[StopAudio] forceDrain: targetUs=%lld minTrackBefore=%lld minTrackAfter=%lld "
                        "pulled=%lld samples (%.1f ms) iterations=%lld",
                        cfrAudioTargetUs, minEncodedBefore, minEncodedAfter, minEncodedAfter - minEncodedBefore,
                        (double)(minEncodedAfter - minEncodedBefore) / 48.0, forceDrainIterations);
                }
            }

            recording = false;
        }

        // Stop audio thread first
        audioRunning = false;
        audioDrainCv.notify_all();
        if (audioThread.joinable()) {
            audioThread.join();
        }
        if (finalCfrStopPending) {
            audioFinalizingCfrStop.store(false, std::memory_order_release);
            DLL_Log("[StopAudio] Final CFR stop pending cleared");
        }

        // Final A/V sync diagnostic before stopping sources
        {
            const int64_t expectedVideoMs = expectedVideoUsForStop / 1000;
            const int64_t expectedVideoSamples =
                ce::audio::ComputeDurationUsToSamples(expectedVideoUsForStop, kStopSampleRate);
            for (size_t i = 0; i < audioSources.size() && i < encodedSamplesPerSource.size(); i++) {
                const int64_t encSamples = encodedSamplesPerSource[i];
                const int64_t diffSamples = encSamples - expectedVideoSamples;
                const double diffMs = (double)diffSamples * 1000.0 / kStopSampleRate;
                DLL_Log(
                    "[StopAudio] Source %zu: encodedSamples=%lld expectedVideoSamples=%lld diff=%+lld (%+.1f ms) "
                    "track=%d",
                    i, encSamples, expectedVideoSamples, diffSamples, diffMs, audioSources[i].track);
            }
            // App-audio latency distribution over the whole recording (audio-behind-video delay). This
            // is the headline observability fix: it makes elevated/variable latency obvious at a glance
            // instead of needing manual reconstruction. A healthy source sits almost entirely in <50ms;
            // significant time in 300-600ms means the drain is not keeping latency near live.
            for (size_t i = 0; i < audioSources.size(); i++) {
                const AudioSource& s = audioSources[i];
                if (s.sourceType != AudioConfig::AppAudio || s.appLatencySampleCount == 0) {
                    continue;
                }
                const uint64_t n = s.appLatencySampleCount;
                const double avgMs = static_cast<double>(s.appLatencySumMs) / static_cast<double>(n);
                const double targetAvgMs = static_cast<double>(s.appLatencyTargetSumMs) / static_cast<double>(n);
                const double excessAvgMs = static_cast<double>(s.appLatencyExcessSumMs) / static_cast<double>(n);
                const double maxCompPercent = static_cast<double>(s.appLatencyMaxAbsCompDelta) * 100.0 /
                                              (static_cast<double>(kStopSampleRate) * 10.0);
                const double elevatedPct =
                    100.0 *
                    static_cast<double>(s.appLatencyBuckets[2] + s.appLatencyBuckets[3] + s.appLatencyBuckets[4]) /
                    static_cast<double>(n);
                ProcessLoopbackCapture* routedCapture = GetAppCaptureForRoute(i);
                const uint64_t queueOverrunPackets = routedCapture ? routedCapture->GetQueueOverrunPacketCount() : 0;
                const uint64_t queueOverrunFrames = routedCapture ? routedCapture->GetQueueOverrunFrameCount() : 0;
                DLL_Log(
                    "[STOP AUDIO LATENCY] Source %zu track=%d appAudioDelay avg=%.0fms max=%ums "
                    "targetAvg=%.0fms excessAvg=%.0fms excessMax=%ums "
                    "buckets(<50/50-150/150-300/300-600/>600ms)=%.0f%%/%.0f%%/%.0f%%/%.0f%%/%.0f%% "
                    ">=150ms=%.0f%% drainObservations=%u/%llu transitions=%llu maxComp=%.4f%% "
                    "queueOverrun=%llu/%llu underruns=%u trims(lat=%llu normal=%llu cat=%u/%llu). "
                    "Lower/more-uniform excess is better; high excess means audio content ran behind video.",
                    i, s.track, avgMs, s.appLatencyMaxMs, targetAvgMs, excessAvgMs, s.appLatencyExcessMaxMs,
                    100.0 * s.appLatencyBuckets[0] / n, 100.0 * s.appLatencyBuckets[1] / n,
                    100.0 * s.appLatencyBuckets[2] / n, 100.0 * s.appLatencyBuckets[3] / n,
                    100.0 * s.appLatencyBuckets[4] / n, elevatedPct, s.appLatencyDrainingSamples,
                    static_cast<unsigned long long>(n), static_cast<unsigned long long>(s.appLatencyDrainTransitions),
                    maxCompPercent, static_cast<unsigned long long>(queueOverrunPackets),
                    static_cast<unsigned long long>(queueOverrunFrames), s.ringBufferUnderrunCount,
                    static_cast<unsigned long long>(s.latencyTrimSamples),
                    static_cast<unsigned long long>(s.latencyTrimSamples >= s.catastrophicResyncSamples
                                                        ? s.latencyTrimSamples - s.catastrophicResyncSamples
                                                        : 0),
                    s.catastrophicResyncEvents, static_cast<unsigned long long>(s.catastrophicResyncSamples));
            }
            DLL_Log("[StopAudio] Video: expectedDuration=%lld ms (%lld samples)", expectedVideoMs,
                    expectedVideoSamples);
        }

        {
            std::lock_guard<std::recursive_mutex> lock(muxMutex);
            timingModeFrozenForSession = false;
            activeScreenGrab = false;
        }

        DLL_Log("[STOP SUMMARY] Recording finalized");
        DLL_Log("[AVSyncAuto] stop_summary: resolvedRenderLatencyMs=%.3f confidence=%s reason=%s usedAudioProbe=%d",
                static_cast<double>(config.avSyncResolvedRenderLatencyMs), config.avSyncConfidence.c_str(),
                config.avSyncReason.c_str(), config.avSyncUsedAudioProbe ? 1 : 0);
        if (videoEnc) {
            const int64_t finalVideoMs = videoEnc->GetExpectedFinalDurationUs() / 1000;
            const int64_t wallMs = videoElapsedMs.load();
            const int64_t encodedMs = videoEnc->GetEncodedDurationUs() / 1000;
            DLL_Log("[STOP SUMMARY] Video: duration=%lldms wall=%lldms encoded=%lldms pipelineLag=%lldms", finalVideoMs,
                    wallMs, encodedMs, wallMs - encodedMs);
        }
        {
            const int64_t expectedVideoSamples =
                ce::audio::ComputeDurationUsToSamples(expectedVideoUsForStop, kStopSampleRate);
            for (const auto& kv : cachedTrackToSources) {
                const int track = kv.first;
                const int64_t trackSamples = trackTimelineSamples.count(track) ? trackTimelineSamples[track] : 0;
                const int64_t diffSamples = trackSamples - expectedVideoSamples;
                const double diffMs = static_cast<double>(diffSamples) * 1000.0 / kStopSampleRate;
                std::string sourceList;
                for (size_t srcIdx : kv.second) {
                    if (!sourceList.empty()) {
                        sourceList += ",";
                    }
                    sourceList += std::to_string(srcIdx);
                }
                DLL_Log(
                    "[STOP AUDIO TRACK] Track %d: encoded=%lld expected=%lld diff=%+lld (%+.3f ms) realMixed=%llu "
                    "fullSilence=%llu partialSilence=%llu sources=[%s]",
                    track, trackSamples, expectedVideoSamples, diffSamples, diffMs,
                    (unsigned long long)trackRealMixedSamples[track],
                    (unsigned long long)trackFullSilenceSamples[track],
                    (unsigned long long)trackPartialSilenceSamples[track], sourceList.c_str());
            }
        }
        for (size_t i = 0; i < audioSources.size() && i < encodedSamplesPerSource.size(); i++) {
            auto& src = audioSources[i];
            const bool isApp = (src.sourceType == AudioConfig::AppAudio);
            const bool noAppData = isApp && !src.hasAlignedStart;
            if (noAppData && src.sawCaptureEpoch) {
                DLL_Log(
                    "[STOP AUDIO] Source %zu (app-active-no-data): track=%d process=%s; activation epoch was "
                    "ordered but no WASAPI data packet arrived, so the route contains expected timeline silence",
                    i, src.track, src.config.processName.empty() ? "<pid-mode>" : src.config.processName.c_str());
            } else if (noAppData) {
                DLL_Log("[STOP AUDIO] Source %zu (app-never-started): track=%d", i, src.track);
            } else {
                const double latencyTrimPerMinute =
                    ce::audio::ComputeSamplesPerMinute(src.latencyTrimSamples, expectedVideoUsForStop);
                const double bootstrapTrimPerMinute =
                    ce::audio::ComputeSamplesPerMinute(src.bootstrapTrimSamples, expectedVideoUsForStop);
                const double coverageTrimPerMinute =
                    ce::audio::ComputeSamplesPerMinute(src.coverageLossTrimSamples, expectedVideoUsForStop);
                const double tier2TrimPerMinute =
                    ce::audio::ComputeSamplesPerMinute(src.tier2TrimSamples, expectedVideoUsForStop);
                const double retainedTrimPerMinute =
                    ce::audio::ComputeSamplesPerMinute(src.retainedNewestTrimSamples, expectedVideoUsForStop);
                const double ratePpm = ce::audio::ComputeClockMismatchPpm(src.currentRateDelta, kStopSampleRate);
                const uint64_t categorizedLatencyTrim =
                    std::min(src.latencyTrimSamples, src.bootstrapTrimSamples + src.retainedNewestTrimSamples +
                                                         src.coverageLossTrimSamples + src.tier2TrimSamples +
                                                         src.catastrophicResyncSamples);
                const uint64_t uncategorizedLatencyTrim = src.latencyTrimSamples - categorizedLatencyTrim;
                const uint64_t normalLatencyTrim = src.latencyTrimSamples >= src.catastrophicResyncSamples
                                                       ? src.latencyTrimSamples - src.catastrophicResyncSamples
                                                       : 0;
                DLL_Log(
                    "[STOP AUDIO] Source %zu: track=%d encoded=%llu trim=cov:%llu latTotal:%llu liveUncat:%llu "
                    "cat:%llu normal:%llu pad:%llu qgap:%llu qjoin:%llu qjoinKeep:%llu "
                    "ringPeak=%zu ringUnderruns=%u process=%s",
                    i, src.track, (unsigned long long)encodedSamplesPerSource[i],
                    (unsigned long long)src.coverageLossTrimSamples, (unsigned long long)src.latencyTrimSamples,
                    (unsigned long long)uncategorizedLatencyTrim, (unsigned long long)src.catastrophicResyncSamples,
                    (unsigned long long)normalLatencyTrim, (unsigned long long)src.underrunPadSamples,
                    (unsigned long long)src.packetTimelineGapSamples,
                    (unsigned long long)src.lateAppJoinSuppressedGapSamples,
                    (unsigned long long)src.lateAppJoinPreservedGapSamples, src.ringBufferPeakSamples,
                    src.ringBufferUnderrunCount, src.config.processName.empty() ? "-" : src.config.processName.c_str());
                DLL_Log(
                    "[STOP AUDIO DETAIL] Source %zu: ratePpm=%+.2f compDelta=%d sat=%d trimRate(latTotal=%.1f/min "
                    "boot=%.1f/min cov=%.1f/min tier2=%.1f/min retain=%.1f/min) totals(boot=%llu tier2=%llu "
                    "retain=%llu cat=%llu catEvents=%u liveUncat=%llu post=%llu overlap=%llu ovf=%llu)",
                    i, ratePpm, src.currentRateDelta, src.targetRateSaturated ? 1 : 0, latencyTrimPerMinute,
                    bootstrapTrimPerMinute, coverageTrimPerMinute, tier2TrimPerMinute, retainedTrimPerMinute,
                    (unsigned long long)src.bootstrapTrimSamples, (unsigned long long)src.tier2TrimSamples,
                    (unsigned long long)src.retainedNewestTrimSamples,
                    (unsigned long long)src.catastrophicResyncSamples, src.catastrophicResyncEvents,
                    (unsigned long long)uncategorizedLatencyTrim, (unsigned long long)src.postResampleTrimSamples,
                    (unsigned long long)src.packetTimelineOverlapSamples, (unsigned long long)src.overflowDropSamples);
                DLL_Log(
                    "[AVSyncAuto] stop_audio_source: src=%zu track=%d codec=%s encodedSamples=%llu "
                    "captureLatencyMs=%.3f encoderReady=%d streamIndex=%d confidence=%s reason=%s",
                    i, src.track, src.config.codec.c_str(), (unsigned long long)encodedSamplesPerSource[i],
                    static_cast<double>(src.config.captureLatencyMs), (src.encoder && src.encoder->IsReady()) ? 1 : 0,
                    src.encoder ? src.encoder->GetStreamIndex() : -1, config.avSyncConfidence.c_str(),
                    config.avSyncReason.c_str());
            }
        }

        // Stop all audio sources
        for (auto& src : audioSources) {
            if (src.capture) {
                src.capture->Stop();
            }
            if (src.appCapture) {
                src.appCapture->Stop();
            }
            if (src.encoder) {
                src.encoder->SetExpectedSourceSilenceSamples(
                    static_cast<int64_t>(trackFullSilenceSamples[src.track]));
                src.encoder->Stop();
            }

            // PULL MODEL: Clear ring buffers to prevent stale audio in next recording
            if (src.ringBuffer) {
                src.ringBuffer->Clear();
            }

            // DRIFT COMPENSATION: Clear sync buffers and reset counters
            if (src.syncResampler) {
                src.syncResampler->Reset();
            }
            src.postResampleBuffer.clear();
            src.syncSamplesOutput = 0;
            src.dropFadeSamplesRemaining = 0;
            src.dropFadeStartL = 0.0f;
            src.dropFadeStartR = 0.0f;
            src.dropFadeStart.clear();
            src.underrunFadeSamplesRemaining = 0;
            src.packetBoundaryFadeInSamplesRemaining = 0;
            src.overflowDropSamples = 0;
            src.retainedNewestTrimSamples = 0;
            src.latencyTrimSamples = 0;
            src.tier2TrimSamples = 0;
            src.bootstrapTrimSamples = 0;
            src.postResampleTrimSamples = 0;
            src.underrunPadSamples = 0;
            src.coverageLossTrimSamples = 0;
            src.startupSyntheticRingSamples = 0;
            src.startupSyntheticResamplerSamples = 0;
            src.startupSyntheticPostSamples = 0;
            src.qpcAlignedWrittenSamples = 0;
            src.packetTimelineGapSamples = 0;
            src.packetTimelineOverlapSamples = 0;
            src.startupRebasedGapSamples = 0;
            src.lateAppJoinSuppressedGapSamples = 0;
            src.lateAppJoinPreservedGapSamples = 0;
            src.alignedStartMs = -1;
            src.observedLateStartMs = 0;
            src.hasAlignedStart = false;
            src.sawCaptureEpoch = false;
            src.timelineValid = false;
            src.isPrimed = false;
            src.bootstrapComplete = false;
            src.pendingUnderrunRecoveryFade = false;
            src.sawSyncPendingPackets = false;
            src.startupRealAudioSeen = false;
            src.pendingStartupJoinFade = false;
            src.pendingRetainedTrimSamples = 0;
            src.pendingRetainedTrimEvents = 0;
            src.pendingLatencyTrimSamples = 0;
            src.pendingLatencyTrimEvents = 0;
            src.pendingTier2TrimSamples = 0;
            src.pendingTier2TrimEvents = 0;
            src.pendingCoverageLossTrimSamples = 0;
            src.pendingCoverageLossTrimEvents = 0;
            src.lastAppPlaceDiagTick = 0;
            src.lastAppConsumeDiagTick = 0;
            src.appLatencyBuckets[0] = 0;
            src.appLatencyBuckets[1] = 0;
            src.appLatencyBuckets[2] = 0;
            src.appLatencyBuckets[3] = 0;
            src.appLatencyBuckets[4] = 0;
            src.appLatencySampleCount = 0;
            src.appLatencySumMs = 0;
            src.appLatencyMaxMs = 0;
            src.appLatencyTargetSumMs = 0;
            src.appLatencyExcessSumMs = 0;
            src.appLatencyExcessMaxMs = 0;
            src.appLatencyDrainingSamples = 0;
            src.appLatencyDrainTransitions = 0;
            src.appLatencyMaxAbsCompDelta = 0;
            src.lastAppLatencyWarnTick = 0;
            src.appLatencyWarnActive = false;
            src.appAudioBacklogDrainInitialized = false;
            src.appAudioBacklogDrainActive = false;
            src.appAudioBacklogDrainReason =
                static_cast<uint32_t>(ce::audio::CfrAppAudioBacklogDrainReason::WithinSlack);
            src.appAudioBacklogTargetSamples = 0;
            src.appAudioBacklogExcessSamples = 0;
            src.appAudioBacklogCompensationDelta = 0;
            src.catastrophicResyncSamples = 0;
            src.catastrophicResyncEvents = 0;
            src.lastCatastrophicResyncTick = 0;
            src.lastRetainedTrimWarnTick = 0;
            src.lastPacketTimelineAdjustWarnTick = 0;
            src.wgcCoverageLossTrimAccumulator = 0.0;
            src.prevLeadSamples = 0;
            src.prevLeadSnapshotMs = 0;
            src.lastRateUpdateMs = 0;
            src.currentRateDelta = 0;
            src.targetRateDelta = 0;
            src.rateCompActive = false;
            src.targetRateSaturated = false;
            src.epochResetRequested->store(0, std::memory_order_release);
            src.epochResetAcknowledged->store(0, std::memory_order_release);
            src.epochSyncTailFlushedGeneration = 0;
            src.lastEpochTransitionWaitLogTick = 0;
        }

        // Reset video frame tracking for next recording
        firstVideoFrameMs = 0;
        lastVideoFrameMs = 0;
        recordingStartSystemQPCMs.store(0);
        recordingStartSystemQpc100ns.store(0);
        injectTimelineState.Reset();
        d3d11TimelineState.Reset();

        // Note: We don't need to update VideoEncoder audio context here anymore
        // since we're using AddAudioContext and the contexts are stored per-source

        // Stop video encoder (writes trailer)
        if (videoEnc) {
            videoEnc->Stop();
        }
    }

    bool ProcessFrame(uint64_t handle, uint64_t fenceHandle, uint64_t fenceVal, int64_t timestampQPC, int32_t luidLow,
                      int32_t luidHigh, uint32_t sourcePid, uint32_t width, uint32_t height, uint32_t format,
                      bool isHDR, bool isShmem = false, int shmemSlot = 0,
                      const ce::cursor::CaptureState* cursorState = nullptr) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (!videoEnc || !recording)
            return false;

        // Use CaptureEngine's steady_clock for duration to avoid Game QPC /
        // Frequency mismatch issues
        auto now = std::chrono::steady_clock::now();

        // Calculate QPC based timestamp for debugging/Legacy Start Time
        int64_t debugTimestamp = (qpcFreq > 0) ? (timestampQPC * 1000) / qpcFreq : timestampQPC;

        const bool commitsFirstVideoFrame = this->firstVideoFrameMs == 0;

        const int64_t steadyElapsedUs =
            commitsFirstVideoFrame
                ? 0
                : std::chrono::duration_cast<std::chrono::microseconds>(now - this->recordingStartTime).count();
        int64_t realElapsedUs = steadyElapsedUs;
        if (SessionUsesVfr()) {
            realElapsedUs = ComputeSourceDrivenElapsedUs(qpcFreq, timestampQPC, steadyElapsedUs, injectTimelineState);
        } else {
            realElapsedUs = ResolveCfrTimelineElapsedUs(steadyElapsedUs, -1, injectTimelineState.lastElapsedUs);
        }

        // Maybe we want preview later? For now, recording only.
        videoEnc->SetAdapterLUID(luidLow, luidHigh);
        if (cursorState) {
            videoEnc->SetCursorCaptureState(*cursorState);
        }
        bool res = videoEnc->EncodeFrame((HANDLE)handle, (HANDLE)fenceHandle, fenceVal, realElapsedUs, sourcePid, width,
                                         height, format, isHDR, isShmem, shmemSlot);

        if (!res && videoEnc->WasLastFrameDeferred()) {
            return false;
        }
        if (!res) {
            return false;
        }

        if (commitsFirstVideoFrame) {
            // Commit the media/audio anchor only after the encoder accepted the
            // first frame. A failed/deferred candidate must not discard audio
            // or establish a PTS origin for pixels that were never emitted.
            this->firstVideoFrameMs = debugTimestamp;
            this->recordingStartTime = now;
            const int64_t startQpc100ns =
                (qpcFreq > 0 && timestampQPC > 0)
                    ? static_cast<int64_t>(ce::audio::RawQpcToHundredNanoseconds(static_cast<uint64_t>(timestampQPC),
                                                                                 static_cast<uint64_t>(qpcFreq)))
                    : 0;
            DLL_Log(
                "MediaEngine: First successfully encoded inject frame at %lld ms (QPC: %lld) - syncing audio "
                "(StartQPC: %lld)",
                debugTimestamp, timestampQPC, debugTimestamp);
            SyncAudioToFirstVideoFrame(debugTimestamp, startQpc100ns);
        }

        const bool cfrRecording = IsCfrRecording();
        const int64_t committedElapsedUs = cfrRecording && videoEnc ? videoEnc->GetExpectedFinalDurationUs()
                                                                    : GetCommittedVideoElapsedUs(realElapsedUs);
        CommitVideoElapsedUs(injectTimelineState, committedElapsedUs);

        // Update audio stream index for all sources
        for (size_t i = 0; i < audioSources.size(); i++) {
            auto& src = audioSources[i];
            // Get stream index from VideoEncoder for this track
            int idx = videoEnc->GetAudioStreamIndex(src.track);
            if (idx >= 0 && src.encoder) {
                src.encoder->SetStreamIndex(idx);
            }
        }

        if (injectFrameLogCount++ % 60 == 0) {
            DLL_Log("MediaEngine: Sending Frame ts=%lld", debugTimestamp);
        }

        // PULL MODEL: CFR audio follows the authoritative output timeline.
        PullAndEncodeAudio(committedElapsedUs);
        return res;
    }

    bool RepeatLastFrame(int64_t timestampQPC, const ce::cursor::CaptureState* cursorState = nullptr) {
        return RepeatLastFrame(timestampQPC, -1, cursorState);
    }

    bool RepeatLastFrame(int64_t timestampQPC, int64_t timelineElapsedUs,
                         const ce::cursor::CaptureState* cursorState = nullptr) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        const bool cfrRecording = IsCfrRecording();
        const bool wgcCfrRecording = IsWgcCfrRecording();
        // CFR drain: allow scheduled repeats to close already accrued CFR debt
        // before MediaEngine_StopRecording finalizes the encoders.
        if (!videoEnc || firstVideoFrameMs == 0)
            return false;
        if (!recording && !cfrRecording)
            return false;

        auto now = std::chrono::steady_clock::now();
        const int64_t steadyElapsedUs =
            std::chrono::duration_cast<std::chrono::microseconds>(now - this->recordingStartTime).count();

        int64_t realElapsedUs = steadyElapsedUs;
        if (SessionUsesVfr()) {
            realElapsedUs = ComputeSourceDrivenElapsedUs(qpcFreq, timestampQPC, steadyElapsedUs, injectTimelineState);
        } else if (wgcCfrRecording) {
            realElapsedUs = ResolveAuthoritativeCfrTimelineElapsedUs(steadyElapsedUs, timelineElapsedUs,
                                                                     d3d11TimelineState.lastElapsedUs);
        } else {
            realElapsedUs =
                ResolveCfrTimelineElapsedUs(steadyElapsedUs, timelineElapsedUs, injectTimelineState.lastElapsedUs);
        }

        const bool useExplicitWgcCfrTimeline = wgcCfrRecording && timelineElapsedUs >= 0;
        if (cursorState) {
            videoEnc->SetCursorCaptureState(*cursorState);
        }
        bool res = videoEnc->RepeatLastFrame(realElapsedUs, useExplicitWgcCfrTimeline);
        if (!res) {
            return false;
        }

        const int64_t committedElapsedUs =
            cfrRecording ? videoEnc->GetExpectedFinalDurationUs() : GetCommittedVideoElapsedUs(realElapsedUs);
        CommitVideoElapsedUs(wgcCfrRecording ? d3d11TimelineState : injectTimelineState,
                             wgcCfrRecording ? realElapsedUs : committedElapsedUs);
        for (size_t i = 0; i < audioSources.size(); i++) {
            auto& src = audioSources[i];
            int idx = videoEnc->GetAudioStreamIndex(src.track);
            if (idx >= 0 && src.encoder) {
                src.encoder->SetStreamIndex(idx);
            }
        }

        const int64_t audioTimelineUs = cfrRecording ? videoEnc->GetExpectedFinalDurationUs() : committedElapsedUs;
        PullAndEncodeAudio(audioTimelineUs);
        return true;
    }

    void ExtendCfrToCommonAudioLattice() {
        if (!IsCfrRecording() || !videoEnc || !videoEnc->CanRepeatLastFrame()) {
            return;
        }
        const int fps = videoEnc->GetConfiguredFps();
        const int64_t frameCount = videoEnc->GetAssignedCfrFrameCount();
        std::vector<int> sampleRates;
        std::set<AudioEncoder*> seenEncoders;
        for (const auto& source : audioSources) {
            AudioEncoder* encoder = source.sharedEncoderPtr;
            if (!encoder || !seenEncoders.insert(encoder).second) {
                continue;
            }
            const AVCodecContext* codecContext = encoder->GetCodecContext();
            if (codecContext && codecContext->sample_rate > 0) {
                sampleRates.push_back(codecContext->sample_rate);
            }
        }
        const int64_t frameQuantum = ce::mux::ComputeCfrAudioLatticeFrameQuantum(fps, sampleRates);
        const int64_t extensionFrames =
            ce::mux::ComputeCfrAudioLatticeExtensionFrames(frameCount, frameQuantum);
        DLL_Log(
            "[FinalizationLattice] CFR endpoint contract fps=%d frames=%lld quantum=%lld extension=%lld "
            "audioRates=%zu expectedDurationUs=%lld",
            fps, static_cast<long long>(frameCount), static_cast<long long>(frameQuantum),
            static_cast<long long>(extensionFrames), sampleRates.size(), videoEnc->GetExpectedFinalDurationUs());
        for (int64_t index = 0; index < extensionFrames; ++index) {
            const int64_t nextTimelineUs = videoEnc->GetExpectedFinalDurationUs();
            if (!RepeatLastFrame(0, nextTimelineUs)) {
                DLL_Log(
                    "[FinalizationLattice] ERROR: failed to append repeat %lld/%lld at timelineUs=%lld; "
                    "decoded endpoint verification must reject any resulting mismatch",
                    static_cast<long long>(index + 1), static_cast<long long>(extensionFrames),
                    static_cast<long long>(nextTimelineUs));
                break;
            }
        }
        DLL_Log("[FinalizationLattice] CFR endpoint committed frames=%lld durationUs=%lld",
                static_cast<long long>(videoEnc->GetAssignedCfrFrameCount()),
                static_cast<long long>(videoEnc->GetExpectedFinalDurationUs()));
    }

    bool IsWgcCfrRecording() const {
        return SessionUsesScreenGrab() && !SessionUsesVfr();
    }

    bool IsCfrRecording() const {
        return ce::audio::ShouldUseCfrAudioContinuityPolicy(SessionUsesVfr());
    }

    double GetMaxAudioCaptureLatencyMs() const {
        double maxLatencyMs = 0.0;
        for (const auto& src : audioSources) {
            maxLatencyMs = std::max(maxLatencyMs, static_cast<double>(src.config.captureLatencyMs));
        }
        return maxLatencyMs;
    }

    int64_t GetMaxAudioCaptureLatencyQpc() const {
        const double maxLatencyMs = GetMaxAudioCaptureLatencyMs();
        if (qpcFreq <= 0 || maxLatencyMs <= 0.0) {
            return 0;
        }
        return static_cast<int64_t>(std::llround((maxLatencyMs / 1000.0) * static_cast<double>(qpcFreq)));
    }

    void SetWgcStartupExtraDelayQpc(int64_t delayQpc) {
        const int64_t clampedDelayQpc = std::max<int64_t>(0, delayQpc);
        wgcStartupExtraDelayQpc.store(clampedDelayQpc, std::memory_order_release);
        if (clampedDelayQpc > 0 && IsWgcCfrRecording()) {
            preservePendingStartupAudioPackets.store(true, std::memory_order_release);
        }
        if (qpcFreq > 0 || clampedDelayQpc > 0) {
            const double delayMs =
                qpcFreq > 0 ? (static_cast<double>(clampedDelayQpc) * 1000.0) / static_cast<double>(qpcFreq) : 0.0;
            DLL_Log("[AVSyncApply] wgc_start_extra_delay: smoothExtraDelayMs=%.3f smoothExtraDelayQpc=%lld", delayMs,
                    clampedDelayQpc);
        }
    }

    bool PrepareFrameD3D11(void* texture, uint32_t width, uint32_t height, bool isHDR) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (!videoEnc || !recording || !texture || width == 0 || height == 0) {
            DLL_Log(
                "[VideoPrewarm] D3D11 prepare rejected: encoder=%d recording=%d texture=%d dimensions=%ux%u",
                videoEnc ? 1 : 0, recording ? 1 : 0, texture ? 1 : 0, width, height);
            return false;
        }

        const auto start = std::chrono::steady_clock::now();
        const bool prepared = videoEnc->PrepareFrameD3D11(static_cast<ID3D11Texture2D*>(texture), width, height, isHDR);
        const int64_t elapsedUs =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        DLL_Log("[VideoPrewarm] D3D11 deferred encoder/mux prepare %s: dimensions=%ux%u hdr=%d elapsed=%lldus "
                "firstFrameCommitted=%d",
                prepared ? "complete" : "FAILED", width, height, isHDR ? 1 : 0,
                static_cast<long long>(elapsedUs), firstVideoFrameMs != 0 ? 1 : 0);
        return prepared;
    }

    // Direct D3D11 texture processing for screengrab mode (zero-copy)
    bool ProcessFrameD3D11(void* texture, int64_t timestampQPC, uint32_t width, uint32_t height, bool isHDR,
                           int32_t captureLeft, int32_t captureTop, int64_t timelineElapsedUs,
                           const ce::cursor::CaptureState* cursorState) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (!videoEnc || !recording)
            return false;

        auto now = std::chrono::steady_clock::now();
        int64_t debugTimestamp = (qpcFreq > 0) ? (timestampQPC * 1000) / qpcFreq : timestampQPC;

        const bool commitsFirstVideoFrame = this->firstVideoFrameMs == 0;
        int64_t firstAnchorQpc = timestampQPC;
        int64_t firstAnchorMs = debugTimestamp;
        int64_t firstStartQpc100ns = 0;
        bool firstPreservePendingPackets = false;
        if (commitsFirstVideoFrame) {
            if (IsWgcCfrRecording()) {
                const double renderDelayMs = GetMaxAudioCaptureLatencyMs();
                const int64_t renderDelayQpc = GetMaxAudioCaptureLatencyQpc();
                const int64_t smoothExtraDelayQpc =
                    std::max<int64_t>(0, wgcStartupExtraDelayQpc.load(std::memory_order_acquire));
                const bool delayWouldOverflow =
                    renderDelayQpc > 0 && smoothExtraDelayQpc > 0 &&
                    renderDelayQpc > (std::numeric_limits<int64_t>::max() - smoothExtraDelayQpc);
                const int64_t startupDelayQpc =
                    delayWouldOverflow ? renderDelayQpc : (renderDelayQpc + smoothExtraDelayQpc);
                const double smoothExtraDelayMs =
                    qpcFreq > 0 ? (static_cast<double>(smoothExtraDelayQpc) * 1000.0) / static_cast<double>(qpcFreq)
                                : 0.0;
                const double startupDelayMs =
                    qpcFreq > 0 ? (static_cast<double>(startupDelayQpc) * 1000.0) / static_cast<double>(qpcFreq)
                                : renderDelayMs;
                firstAnchorQpc = ce::capture_policy::GetWgcStartupAudioAnchorQpc(timestampQPC, renderDelayQpc);
                LARGE_INTEGER qpcNow;
                QueryPerformanceCounter(&qpcNow);
                const int64_t nowQPC = qpcNow.QuadPart;
                const int64_t frameAgeUs =
                    (qpcFreq > 0 && nowQPC > timestampQPC) ? ((nowQPC - timestampQPC) * 1000000) / qpcFreq : 0;
                const int64_t audioAnchorDelayUs = (qpcFreq > 0 && firstAnchorQpc > timestampQPC)
                                                       ? ((firstAnchorQpc - timestampQPC) * 1000000) / qpcFreq
                                                       : 0;
                const int64_t startupContentDelayUs =
                    qpcFreq > 0 ? (startupDelayQpc * 1000000) / qpcFreq : audioAnchorDelayUs;
                DLL_Log(
                    "MediaEngine: WGC CFR startup anchor candidate from first frame plus render delay "
                    "(videoQPC=%lld anchorQPC=%lld nowQPC=%lld frameAge=%lldus startupContentDelay=%lldus "
                    "audioAnchorDelay=%lldus contentDelayMs=%.3f renderDelayMs=%.3f smoothExtraDelayMs=%.3f "
                    "confidence=%s reason=%s)",
                    timestampQPC, firstAnchorQpc, nowQPC, frameAgeUs, startupContentDelayUs, audioAnchorDelayUs,
                    startupDelayMs, renderDelayMs, smoothExtraDelayMs, config.avSyncConfidence.c_str(),
                    config.avSyncReason.c_str());
                DLL_Log(
                    "[AVSyncApply] wgc_start_anchor_candidate: videoQpc=%lld audioAnchorQpc=%lld "
                    "audioAnchorDelayUs=%lld "
                    "startupContentDelayUs=%lld contentDelayMs=%.3f renderDelayMs=%.3f smoothExtraDelayMs=%.3f "
                    "confidence=%s reason=%s",
                    timestampQPC, firstAnchorQpc, audioAnchorDelayUs, startupContentDelayUs, startupDelayMs,
                    renderDelayMs, smoothExtraDelayMs, config.avSyncConfidence.c_str(), config.avSyncReason.c_str());
            }

            firstAnchorMs = (qpcFreq > 0 && firstAnchorQpc > 0) ? (firstAnchorQpc * 1000) / qpcFreq : debugTimestamp;
            firstStartQpc100ns = (qpcFreq > 0 && firstAnchorQpc > 0)
                                     ? static_cast<int64_t>(ce::audio::RawQpcToHundredNanoseconds(
                                           static_cast<uint64_t>(firstAnchorQpc), static_cast<uint64_t>(qpcFreq)))
                                     : 0;
            firstPreservePendingPackets = ce::audio::ShouldPreservePendingAudioPacketsForStartupSync(
                IsWgcCfrRecording(), wgcStartupExtraDelayQpc.load(std::memory_order_acquire));
        }

        int64_t realElapsedUs = 0;
        const int64_t steadyElapsedUs =
            commitsFirstVideoFrame
                ? 0
                : std::chrono::duration_cast<std::chrono::microseconds>(now - this->recordingStartTime).count();
        if (SessionUsesVfr()) {
            realElapsedUs = ComputeSourceDrivenElapsedUs(qpcFreq, timestampQPC, steadyElapsedUs, d3d11TimelineState);
        } else {
            // CFR output cadence is already driven by the encoder thread's fixed-rate
            // sample loop. Feeding the video encoder WGC source timestamps here causes
            // avoidable skip/dup churn when callback cadence jitters around that output
            // grid, so prefer the sample-clock timeline instead.  Catch-up paths can
            // provide an explicit CFR slot time so buffered fresh frames land on the
            // intended output grid instead of collapsing onto the current wall-clock.
            realElapsedUs = ResolveAuthoritativeCfrTimelineElapsedUs(steadyElapsedUs, timelineElapsedUs,
                                                                     d3d11TimelineState.lastElapsedUs);
        }
        const bool useExplicitWgcCfrTimeline = IsWgcCfrRecording() && timelineElapsedUs >= 0;
        if (cursorState) {
            videoEnc->SetCursorCaptureState(*cursorState);
        }
        if (!videoEnc->EncodeFrameD3D11((ID3D11Texture2D*)texture, realElapsedUs, width, height, isHDR, captureLeft,
                                        captureTop, useExplicitWgcCfrTimeline)) {
            DLL_Log("MediaEngine: D3D11 frame encode failed at ts=%lld", debugTimestamp);
            return false;
        }
        if (commitsFirstVideoFrame) {
            this->firstVideoFrameMs = debugTimestamp;
            this->recordingStartTime = now;
            videoElapsedMs.store(0);
            DLL_Log("MediaEngine: First successfully encoded D3D11 frame at %lld ms (QPC: %lld) (StartQPC: %lld)",
                    debugTimestamp, timestampQPC, firstAnchorMs);
            SyncAudioToFirstVideoFrame(firstAnchorMs, firstStartQpc100ns, firstPreservePendingPackets);
        }
        // Keep the WGC live timeline anchored to the scheduled CFR wall clock even
        // when the encoder has fallen behind. Audio diagnostics and buffering logic
        // need to see that shortfall rather than the shortened encoded duration.
        CommitVideoElapsedUs(d3d11TimelineState, realElapsedUs);

        // Update audio stream index for all sources
        for (size_t i = 0; i < audioSources.size(); i++) {
            auto& src = audioSources[i];
            int idx = videoEnc->GetAudioStreamIndex(src.track);
            if (idx >= 0 && src.encoder) {
                src.encoder->SetStreamIndex(idx);
            }
        }

        if (screengrabFrameLogCount++ % 600 == 0) {
            DLL_Log("MediaEngine: ScreenGrab frame ts=%lld %dx%d", debugTimestamp, width, height);
        }

        // PULL MODEL: WGC CFR audio follows the PTS-based scheduled timeline
        // (GetExpectedFinalDurationUs) rather than the encoder grid time, which has
        // a 1-tick offset that would leave audio 8ms short by the end of recording.
        const int64_t audioTargetUs = IsWgcCfrRecording() ? videoEnc->GetExpectedFinalDurationUs() : realElapsedUs;
        PullAndEncodeAudio(audioTargetUs);
        return true;
    }

    void AppendSyncResamplerOutput(AudioSource& src, size_t srcIdx, int channels, uint8_t** resampledData,
                                   int outSamples) {
        if (outSamples <= 0) {
            return;
        }

        const uint64_t syntheticPostSamples = ce::audio::ConsumeSyntheticBufferedSamples(
            src.startupSyntheticResamplerSamples, static_cast<uint64_t>(outSamples));
        src.startupSyntheticPostSamples += syntheticPostSamples;
        if (!resampledData || !resampledData[0]) {
            DLL_Log("[PullAudio] ERROR: sync resampler returned %d samples without data for src=%zu", outSamples,
                    srcIdx);
            return;
        }

        constexpr int kMixerSampleRate = 48000;
        float* outFloats = reinterpret_cast<float*>(resampledData[0]);
        if (src.dropFadeSamplesRemaining > 0) {
            const int kDropFadeSamples = kMixerSampleRate / 40;
            const int blendSamples = std::min(src.dropFadeSamplesRemaining, outSamples);
            const int blendStart = kDropFadeSamples - src.dropFadeSamplesRemaining;
            for (int sample = 0; sample < blendSamples; ++sample) {
                const float alpha = static_cast<float>(blendStart + sample + 1) / kDropFadeSamples;
                for (int channel = 0; channel < channels; ++channel) {
                    const size_t index = static_cast<size_t>(sample) * channels + channel;
                    const float anchor = GetDropFadeAnchor(src, channel);
                    outFloats[index] = anchor + (outFloats[index] - anchor) * alpha;
                }
            }
            if (blendSamples > 0) {
                src.dropFadeStart.assign(static_cast<size_t>(channels), 0.0f);
                const size_t base = static_cast<size_t>(blendSamples - 1) * channels;
                for (int channel = 0; channel < channels; ++channel) {
                    src.dropFadeStart[static_cast<size_t>(channel)] = outFloats[base + channel];
                }
                src.dropFadeStartL = src.dropFadeStart[0];
                src.dropFadeStartR = channels > 1 ? src.dropFadeStart[1] : src.dropFadeStart[0];
            }
            src.dropFadeSamplesRemaining -= blendSamples;
        }

        if (src.packetBoundaryFadeInSamplesRemaining > 0) {
            const int blendSamples = std::min(src.packetBoundaryFadeInSamplesRemaining, outSamples);
            for (int sample = 0; sample < blendSamples; ++sample) {
                const float alpha =
                    ComputeRaisedCosineFade(static_cast<size_t>(sample),
                                            static_cast<size_t>(std::max(src.packetBoundaryFadeInSamplesRemaining, 1)));
                const size_t base = static_cast<size_t>(sample) * channels;
                for (int channel = 0; channel < channels; ++channel) {
                    outFloats[base + channel] *= alpha;
                }
            }
            src.packetBoundaryFadeInSamplesRemaining -= blendSamples;
        }

        if (src.pendingUnderrunRecoveryFade) {
            src.underrunFadeSamplesRemaining = kMixerSampleRate / 40;
            src.pendingUnderrunRecoveryFade = false;
        }
        if (src.underrunFadeSamplesRemaining > 0) {
            const int kUnderrunFadeSamples = kMixerSampleRate / 40;
            const int blendSamples = std::min(src.underrunFadeSamplesRemaining, outSamples);
            const int blendStart = kUnderrunFadeSamples - src.underrunFadeSamplesRemaining;
            for (int sample = 0; sample < blendSamples; ++sample) {
                const float alpha = static_cast<float>(blendStart + sample + 1) / kUnderrunFadeSamples;
                const size_t base = static_cast<size_t>(sample) * channels;
                for (int channel = 0; channel < channels; ++channel) {
                    outFloats[base + channel] *= alpha;
                }
            }
            src.underrunFadeSamplesRemaining -= blendSamples;
        }

        const int numFloats = outSamples * channels;
        src.postResampleBuffer.insert(src.postResampleBuffer.end(), outFloats, outFloats + numFloats);
        src.syncSamplesOutput += outSamples;

        if (dropLogCounter++ % 500 == 0 &&
            (src.overflowDropSamples > 0 || src.latencyTrimSamples > 0 || src.postResampleTrimSamples > 0)) {
            const uint64_t categorizedLatencyTrim =
                std::min(src.latencyTrimSamples, src.bootstrapTrimSamples + src.retainedNewestTrimSamples +
                                                     src.coverageLossTrimSamples + src.tier2TrimSamples);
            const uint64_t uncategorizedLatencyTrim = src.latencyTrimSamples - categorizedLatencyTrim;
            DLL_Log(
                "[PullAudio] Sample trim stats src=%zu: overflowDropped=%llu latencyTrimTotal=%llu "
                "bootstrapTrim=%llu retainedTrim=%llu coverageTrim=%llu tier2Trim=%llu "
                "uncategorizedLiveTrim=%llu postResampleTrim=%llu",
                srcIdx, static_cast<unsigned long long>(src.overflowDropSamples),
                static_cast<unsigned long long>(src.latencyTrimSamples),
                static_cast<unsigned long long>(src.bootstrapTrimSamples),
                static_cast<unsigned long long>(src.retainedNewestTrimSamples),
                static_cast<unsigned long long>(src.coverageLossTrimSamples),
                static_cast<unsigned long long>(src.tier2TrimSamples),
                static_cast<unsigned long long>(uncategorizedLatencyTrim),
                static_cast<unsigned long long>(src.postResampleTrimSamples));
        }
    }

    bool PumpSourceRingThroughSyncResampler(AudioSource& src, size_t srcIdx, int channels, size_t maxFloats) {
        if (!src.ringBuffer || !src.syncResampler || !src.syncResampler->IsReady() || channels <= 0) {
            return false;
        }

        size_t chunkFloats = std::min(src.ringBuffer->GetAvailable(), maxFloats);
        chunkFloats -= chunkFloats % static_cast<size_t>(channels);
        if (chunkFloats == 0) {
            return false;
        }

        const size_t bufferedSamples = src.ringBuffer->GetAvailable() / static_cast<size_t>(channels);
        src.ringBufferPeakSamples = std::max(src.ringBufferPeakSamples, static_cast<uint64_t>(bufferedSamples));
        std::vector<float> ringData(chunkFloats);
        const size_t actualRead = src.ringBuffer->Read(ringData.data(), chunkFloats);
        if (actualRead == 0) {
            return false;
        }

        const size_t actualReadSamples = actualRead / static_cast<size_t>(channels);
        const uint64_t syntheticReadSamples =
            ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticRingSamples, actualReadSamples);
        src.startupSyntheticResamplerSamples += syntheticReadSamples;

        uint8_t** resampledData = nullptr;
        int outSamples = 0;
        const bool processed =
            src.syncResampler->Process(reinterpret_cast<uint8_t*>(ringData.data()),
                                       static_cast<int>(actualRead * sizeof(float)), &resampledData, &outSamples);
        if (processed) {
            AppendSyncResamplerOutput(src, srcIdx, channels, resampledData, outSamples);
        } else {
            DLL_Log("[PullAudio] ERROR: sync resampler rejected %zu samples for src=%zu", actualReadSamples, srcIdx);
        }
        AudioResampler::FreeOutputBuffer(resampledData);
        return true;
    }

    bool FlushCaptureResamplerForEpoch(AudioSource& src, size_t srcIdx, uint64_t oldEpoch, uint64_t newEpoch) {
        if (!src.resampler || !src.resampler->IsReady() || !src.ringBuffer) {
            return true;
        }

        const size_t channels = static_cast<size_t>(std::clamp(src.mixChannels, 1, 8));
        const int64_t delayedSamples = std::max<int64_t>(0, src.resampler->GetDelay());
        if (delayedSamples > static_cast<int64_t>(std::numeric_limits<size_t>::max() / channels)) {
            DLL_Log(
                "[AudioEpoch] ERROR: Capture-resampler tail size overflow src=%zu track=%d epoch=%llu->%llu "
                "delay=%lld channels=%zu",
                srcIdx, src.track, static_cast<unsigned long long>(oldEpoch),
                static_cast<unsigned long long>(newEpoch), static_cast<long long>(delayedSamples), channels);
            return false;
        }

        const size_t requiredFloats = static_cast<size_t>(delayedSamples) * channels;
        const size_t freeFloats = src.ringBuffer->GetFree();
        if (requiredFloats > freeFloats) {
            const uint64_t nowTick = GetTickCount64();
            if (nowTick - src.lastEpochTransitionWaitLogTick >= 1000) {
                DLL_Log(
                    "[AudioEpoch] Waiting for capture-tail ring capacity src=%zu track=%d epoch=%llu->%llu "
                    "requiredFloats=%zu freeFloats=%zu bufferedSamples=%zu",
                    srcIdx, src.track, static_cast<unsigned long long>(oldEpoch),
                    static_cast<unsigned long long>(newEpoch), requiredFloats, freeFloats,
                    src.ringBuffer->GetAvailable() / channels);
                src.lastEpochTransitionWaitLogTick = nowTick;
            }
            return false;
        }

        size_t flushedSamples = 0;
        constexpr size_t kMaxFlushPasses = 8;
        bool flushComplete = false;
        for (size_t pass = 0; pass < kMaxFlushPasses; ++pass) {
            uint8_t** tailData = nullptr;
            int tailSamples = 0;
            const AudioResampler::FlushResult flushResult = src.resampler->Flush(&tailData, &tailSamples);
            if (flushResult == AudioResampler::FlushResult::Error) {
                AudioResampler::FreeOutputBuffer(tailData);
                DLL_Log(
                    "[AudioEpoch] ERROR: Capture-resampler flush failed src=%zu track=%d epoch=%llu->%llu "
                    "flushed=%zu",
                    srcIdx, src.track, static_cast<unsigned long long>(oldEpoch),
                    static_cast<unsigned long long>(newEpoch), flushedSamples);
                return false;
            }
            if (flushResult == AudioResampler::FlushResult::Complete) {
                flushComplete = true;
                break;
            }
            if (tailSamples > 0 && tailData && tailData[0]) {
                const size_t tailFloats = static_cast<size_t>(tailSamples) * channels;
                const size_t writtenFloats =
                    src.ringBuffer->Write(reinterpret_cast<const float*>(tailData[0]), tailFloats);
                if (writtenFloats != tailFloats) {
                    DLL_Log(
                        "[AudioEpoch] ERROR: Capture tail could not be preserved src=%zu track=%d "
                        "epoch=%llu->%llu requestedFloats=%zu writtenFloats=%zu",
                        srcIdx, src.track, static_cast<unsigned long long>(oldEpoch),
                        static_cast<unsigned long long>(newEpoch), tailFloats, writtenFloats);
                    AudioResampler::FreeOutputBuffer(tailData);
                    return false;
                }
                const size_t writtenSamples = writtenFloats / channels;
                src.qpcAlignedWrittenSamples += writtenSamples;
                flushedSamples += writtenSamples;
            }
            AudioResampler::FreeOutputBuffer(tailData);
        }

        if (!flushComplete) {
            DLL_Log(
                "[AudioEpoch] Capture-resampler flush pass bound reached src=%zu track=%d epoch=%llu->%llu "
                "flushed=%zu reportedDelay=%lld; continuing on the next owner pass",
                srcIdx, src.track, static_cast<unsigned long long>(oldEpoch), static_cast<unsigned long long>(newEpoch),
                flushedSamples, static_cast<long long>(std::max<int64_t>(0, src.resampler->GetDelay())));
            return false;
        }

        DLL_Log(
            "[AudioEpoch] Capture owner published epoch reset src=%zu track=%d epoch=%llu->%llu "
            "captureTail=%zu ringBuffered=%zu",
            srcIdx, src.track, static_cast<unsigned long long>(oldEpoch), static_cast<unsigned long long>(newEpoch),
            flushedSamples, src.ringBuffer->GetAvailable() / channels);
        return true;
    }

    bool ServiceAudioEpochResetOnPull(AudioSource& src, size_t srcIdx) {
        if (!src.epochResetRequested || !src.epochResetAcknowledged) {
            return false;
        }
        const uint64_t requested = src.epochResetRequested->load(std::memory_order_acquire);
        const uint64_t acknowledged = src.epochResetAcknowledged->load(std::memory_order_acquire);
        if (requested == 0 || requested == acknowledged) {
            return false;
        }

        const int channels = std::clamp(src.mixChannels, 1, 8);
        if (src.ringBuffer && src.ringBuffer->GetAvailable() >= static_cast<size_t>(channels)) {
            return false;
        }

        if (src.epochSyncTailFlushedGeneration != requested) {
            size_t syncTailSamples = 0;
            constexpr size_t kMaxFlushPasses = 8;
            if (src.syncResampler && src.syncResampler->IsReady()) {
                bool flushComplete = false;
                for (size_t pass = 0; pass < kMaxFlushPasses; ++pass) {
                    uint8_t** tailData = nullptr;
                    int tailSamples = 0;
                    const AudioResampler::FlushResult flushResult = src.syncResampler->Flush(&tailData, &tailSamples);
                    if (flushResult == AudioResampler::FlushResult::Error) {
                        AudioResampler::FreeOutputBuffer(tailData);
                        DLL_Log(
                            "[AudioEpoch] ERROR: Sync-resampler flush failed src=%zu track=%d generation=%llu "
                            "flushed=%zu",
                            srcIdx, src.track, static_cast<unsigned long long>(requested), syncTailSamples);
                        return false;
                    }
                    if (flushResult == AudioResampler::FlushResult::Complete) {
                        flushComplete = true;
                        break;
                    }
                    AppendSyncResamplerOutput(src, srcIdx, channels, tailData, tailSamples);
                    syncTailSamples += static_cast<size_t>(std::max(tailSamples, 0));
                    AudioResampler::FreeOutputBuffer(tailData);
                }
                if (!flushComplete) {
                    DLL_Log(
                        "[AudioEpoch] Sync-resampler flush pass bound reached src=%zu track=%d generation=%llu "
                        "flushed=%zu reportedDelay=%lld; continuing on the next pull pass",
                        srcIdx, src.track, static_cast<unsigned long long>(requested), syncTailSamples,
                        static_cast<long long>(std::max<int64_t>(0, src.syncResampler->GetDelay())));
                    return false;
                }
            }
            src.epochSyncTailFlushedGeneration = requested;
            DLL_Log(
                "[AudioEpoch] Pull owner drained sync tail src=%zu track=%d generation=%llu syncTail=%zu "
                "postBuffered=%zu",
                srcIdx, src.track, static_cast<unsigned long long>(requested), syncTailSamples,
                src.postResampleBuffer.size() / static_cast<size_t>(channels));
        }

        if (!src.postResampleBuffer.empty()) {
            return false;
        }
        if (src.syncResampler && src.syncResampler->IsReady() && !src.syncResampler->Reset()) {
            DLL_Log("[AudioEpoch] ERROR: Pull-owner sync reset failed src=%zu track=%d generation=%llu", srcIdx,
                    src.track, static_cast<unsigned long long>(requested));
            return false;
        }

        src.startupSyntheticResamplerSamples = 0;
        src.startupSyntheticPostSamples = 0;
        src.dropFadeSamplesRemaining = 0;
        src.dropFadeStartL = 0.0f;
        src.dropFadeStartR = 0.0f;
        src.dropFadeStart.clear();
        src.underrunFadeSamplesRemaining = 0;
        src.pendingUnderrunRecoveryFade = true;
        src.appAudioBacklogDrainInitialized = false;
        src.appAudioBacklogDrainActive = false;
        src.appAudioBacklogDrainReason =
            static_cast<uint32_t>(ce::audio::CfrAppAudioBacklogDrainReason::WithinSlack);
        src.appAudioBacklogTargetSamples = 0;
        src.appAudioBacklogExcessSamples = 0;
        src.appAudioBacklogCompensationDelta = 0;
        src.prevLeadSamples = 0;
        src.prevLeadSnapshotMs = 0;
        src.lastRateUpdateMs = 0;
        src.currentRateDelta = 0;
        src.targetRateDelta = 0;
        src.rateCompActive = false;
        src.targetRateSaturated = false;
        src.epochResetAcknowledged->store(requested, std::memory_order_release);
        audioDrainCv.notify_all();
        DLL_Log(
            "[AudioEpoch] Pull owner acknowledged reset src=%zu track=%d generation=%llu ring=0 post=0 "
            "trackCursor=%lld sourceEncoded=%lld",
            srcIdx, src.track, static_cast<unsigned long long>(requested),
            static_cast<long long>(trackTimelineSamples[src.track]),
            srcIdx < encodedSamplesPerSource.size() ? static_cast<long long>(encodedSamplesPerSource[srcIdx]) : -1ll);
        return true;
    }

    void FlushAudioOnlyResamplerTails() {
        constexpr size_t kMaxFlushPasses = 8;
        constexpr size_t kSyncPumpSamples = 4800;
        for (size_t srcIdx = 0; srcIdx < audioSources.size(); ++srcIdx) {
            auto& src = audioSources[srcIdx];
            const TrackAudioFormat trackFormat = GetTrackAudioFormat(src.track);
            const int channels = std::clamp(trackFormat.channels, 1, 8);

            size_t captureTailSamples = 0;
            if (src.resampler && src.resampler->IsReady() && src.ringBuffer) {
                for (size_t pass = 0; pass < kMaxFlushPasses; ++pass) {
                    uint8_t** tailData = nullptr;
                    int tailSamples = 0;
                    const AudioResampler::FlushResult flushResult = src.resampler->Flush(&tailData, &tailSamples);
                    if (flushResult != AudioResampler::FlushResult::Output) {
                        AudioResampler::FreeOutputBuffer(tailData);
                        if (flushResult == AudioResampler::FlushResult::Error) {
                            DLL_Log("[StopAudio] ERROR: Capture-resampler tail flush failed src=%zu track=%d", srcIdx,
                                    src.track);
                        }
                        break;
                    }
                    if (tailSamples > 0 && tailData && tailData[0]) {
                        float* tailFloats = reinterpret_cast<float*>(tailData[0]);
                        if (src.packetBoundaryFadeInSamplesRemaining > 0) {
                            const size_t fadeSamples = static_cast<size_t>(src.packetBoundaryFadeInSamplesRemaining);
                            ApplyPacketBoundaryFadeIn(tailFloats, static_cast<size_t>(tailSamples),
                                                      static_cast<size_t>(channels), fadeSamples);
                            src.packetBoundaryFadeInSamplesRemaining =
                                fadeSamples > static_cast<size_t>(tailSamples)
                                    ? static_cast<int>(fadeSamples - static_cast<size_t>(tailSamples))
                                    : 0;
                        }
                        const size_t writtenFloats = src.ringBuffer->WriteRetainNew(
                            tailFloats, static_cast<size_t>(tailSamples) * static_cast<size_t>(channels));
                        const size_t writtenSamples = writtenFloats / static_cast<size_t>(channels);
                        src.qpcAlignedWrittenSamples += writtenSamples;
                        captureTailSamples += writtenSamples;
                    }
                    AudioResampler::FreeOutputBuffer(tailData);
                }
            }

            size_t syncInputSamples = 0;
            while (src.ringBuffer && src.ringBuffer->GetAvailable() >= static_cast<size_t>(channels)) {
                const size_t before = src.ringBuffer->GetAvailable();
                if (!PumpSourceRingThroughSyncResampler(src, srcIdx, channels,
                                                        kSyncPumpSamples * static_cast<size_t>(channels))) {
                    break;
                }
                syncInputSamples += (before - src.ringBuffer->GetAvailable()) / static_cast<size_t>(channels);
            }

            size_t syncTailSamples = 0;
            if (src.syncResampler && src.syncResampler->IsReady()) {
                for (size_t pass = 0; pass < kMaxFlushPasses; ++pass) {
                    uint8_t** tailData = nullptr;
                    int tailSamples = 0;
                    const AudioResampler::FlushResult flushResult = src.syncResampler->Flush(&tailData, &tailSamples);
                    if (flushResult != AudioResampler::FlushResult::Output) {
                        AudioResampler::FreeOutputBuffer(tailData);
                        if (flushResult == AudioResampler::FlushResult::Error) {
                            DLL_Log("[StopAudio] ERROR: Sync-resampler tail flush failed src=%zu track=%d", srcIdx,
                                    src.track);
                        }
                        break;
                    }
                    AppendSyncResamplerOutput(src, srcIdx, channels, tailData, tailSamples);
                    syncTailSamples += static_cast<size_t>(std::max(tailSamples, 0));
                    AudioResampler::FreeOutputBuffer(tailData);
                }
            }

            if (captureTailSamples > 0 || syncTailSamples > 0) {
                DLL_Log(
                    "[StopAudio] Flushed audio-only resampler tails: src=%zu track=%d captureTail=%zu "
                    "syncInput=%zu syncTail=%zu postBuffered=%zu",
                    srcIdx, src.track, captureTailSamples, syncInputSamples, syncTailSamples,
                    src.postResampleBuffer.size() / static_cast<size_t>(channels));
            }
        }
    }

    // PULL MODEL: Pull audio from RingBuffers and encode against the relative
    // recording timeline that also drives CFR video emission.
    void PullAndEncodeAudio(int64_t videoTimelineUs, bool forceDrain = false) {
        if (!recording || audioSources.empty())
            return;

        constexpr int SAMPLE_RATE = 48000;
        constexpr int64_t kSteadyAudioPullLatencyMs = ce::audio::kDefaultSteadyAudioPullLatencyMs;
        constexpr int64_t kPrimedSourceCushionSamples = SAMPLE_RATE / 40;  // 25ms
        constexpr int64_t kBaseTargetLatencySamples = (kSteadyAudioPullLatencyMs * SAMPLE_RATE) / 1000;
        constexpr int64_t kMaxPipelineLagContributionMs = 10000;
        constexpr int64_t kWgcCoverageLossMaxBufferedLagMs = 300;
        constexpr int64_t kRuntimeMaxLeadSamples = SAMPLE_RATE / 10;            // 100ms above target
        constexpr int64_t kRuntimeMaxDropPerCall = SAMPLE_RATE / 200;           // 5ms
        constexpr int64_t kRuntimeDropFadeSamples = SAMPLE_RATE / 100;          // 10ms
        constexpr int64_t kOverloadAudioPullQuantumSamples = SAMPLE_RATE / 50;  // 20ms
        constexpr int64_t kLatencyTrimHysteresisSamples = ce::audio::kDefaultAudioPullQuantumSamples;
        constexpr int64_t kMinCompensationBufferSamples = kBaseTargetLatencySamples / 4;
        constexpr int64_t kWgcCfrLeadWarningSamples = SAMPLE_RATE / 10;  // 100ms
        constexpr bool kWgcPreferVideoRepeatsOverAudioCuts = true;
        constexpr double kDefaultMaxCompensationPercent = 1.0;
        constexpr double kTier1MaxPitchPercent = 0.05;  // Keep WGC source-clock correction below audible pitch shift.
        // CFR app-audio latency drain. Process-loopback sources can sit well above the video-derived
        // live target during WGC/source-limited runs even when packet durations end exactly aligned.
        // Drain that content backlog through resampler compensation only: no trims/drops for normal
        // sub-second excess, and no changes to system/mic or WGC video scheduling.
        constexpr double kAppAudioDrainMaxPitchPercent = 0.5;
        constexpr int64_t kAppAudioDrainSlackSamples = SAMPLE_RATE / 50;         // 20ms above target
        constexpr int64_t kAppAudioDrainDeadbandSamples = SAMPLE_RATE / 100;     // 10ms compensation deadband
        constexpr int64_t kAppAudioLatencyWarnExcessSamples = SAMPLE_RATE / 20;  // 50ms above target
        constexpr int64_t kTier2DriftThresholdMs = 20;
        constexpr int64_t kWgcEncoderShortfallBufferedLagMaxMs = 4000;
        constexpr uint32_t kWgcEncoderHealthyDeliveryMarginFps = 4;
        constexpr int64_t kWgcCoverageLossLeadSlackSamples = SAMPLE_RATE / 25;  // 40ms above target
        constexpr int64_t kWgcCoverageLossMaxDropPerCall =
            ce::audio::kDefaultAudioPullQuantumSamples;  // 5ms paced overload trim quantum
        constexpr int64_t kWgcVisualSyncMaxBufferedLagMs = 4000;

        for (size_t srcIdx = 0; srcIdx < audioSources.size(); ++srcIdx) {
            ServiceAudioEpochResetOnPull(audioSources[srcIdx], srcIdx);
        }

        // Audio-only recording follows its wall-clock timeline. Video CFR
        // continuity policies (encoder-lag reservoirs, CFR source waits, and
        // overload trimming) do not apply when there is no video encoder.
        const bool isCfrRecording = !audioOnly && IsCfrRecording();
        const bool isWgcCfrRecording = !audioOnly && IsWgcCfrRecording();
        const int64_t wallVideoMs = this->videoElapsedMs.load();
        int64_t encodedVideoMs = 0;
        int64_t audioTargetUs = videoTimelineUs;
        if (videoEnc && !isCfrRecording) {
            int64_t encodedVideoUs = videoEnc->GetEncodedDurationUs();
            if (encodedVideoUs > 0) {
                encodedVideoMs = encodedVideoUs / 1000;
                audioTargetUs = encodedVideoUs;
            }
        }
        if (isCfrRecording && videoEnc) {
            int64_t encodedVideoUs = videoEnc->GetEncodedDurationUs();
            if (encodedVideoUs > 0) {
                encodedVideoMs = encodedVideoUs / 1000;
            }
            if (audioTargetUs <= 0) {
                audioTargetUs = videoEnc->GetExpectedFinalDurationUs();
            }
        }
        if (audioTargetUs <= 0) {
            audioTargetUs = wallVideoMs * 1000;
        }
        if (audioTargetUs <= 0) {
            return;
        }
        int64_t audioTargetMs = audioTargetUs / 1000;

        auto now = std::chrono::steady_clock::now();
        int64_t steadyElapsedUs = 0;
        if (this->recordingStartTime.time_since_epoch().count() > 0) {
            steadyElapsedUs =
                std::chrono::duration_cast<std::chrono::microseconds>(now - this->recordingStartTime).count();
        }
        int64_t timelineShortfallMs = std::max<int64_t>(0, steadyElapsedUs - audioTargetUs) / 1000;

        const int64_t videoPipelineLagMs = ce::audio::ComputeVideoPipelineLagMs(wallVideoMs, encodedVideoMs);
        const uint32_t configuredWgcOutputFps =
            (isWgcCfrRecording && config.video.fps > 0) ? static_cast<uint32_t>(config.video.fps) : 0u;
        uint32_t wgcTargetFps = isWgcCfrRecording ? (configuredWgcOutputFps > 0 ? configuredWgcOutputFps : 1u) : 0u;
        uint32_t wgcDeliveredFps = 0u;
        uint32_t wgcDeliveredMin250Fps = 0u;
        uint32_t wgcDeliveredMin500Fps = 0u;
        int64_t wgcBufferedVideoContentLagMs = 0;
        bool wgcCoverageLossActive = false;
        uint32_t wgcOverloadFlags = 0u;
        bool wgcEncoderBottlenecked = false;
        uint32_t wgcQueueEmptyTickPermille = 0u;
        uint32_t wgcBufferedAtTickMin = 0u;
        uint32_t wgcSingleFrameTickCount = 0u;
        int64_t wgcSelectionBiasUs = 0;
        if (isWgcCfrRecording && sharedMemLayout) {
            const auto& runtimeState = sharedMemLayout->runtimeState;
            wgcOverloadFlags = runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
            wgcEncoderBottlenecked = runtimeState.encoderBottlenecked.load(std::memory_order_relaxed) != 0;
            const uint32_t telemetryTargetFps = runtimeState.wgcTargetFps.load(std::memory_order_relaxed);
            wgcTargetFps = telemetryTargetFps > 0 ? telemetryTargetFps : wgcTargetFps;
            wgcDeliveredFps = runtimeState.wgcDeliveredFramesPerSec.load(std::memory_order_relaxed);
            wgcDeliveredMin250Fps = runtimeState.wgcDeliveredMin250Fps.load(std::memory_order_relaxed);
            wgcDeliveredMin500Fps = runtimeState.wgcDeliveredMin500Fps.load(std::memory_order_relaxed);
            wgcBufferedVideoContentLagMs = ce::audio::ComputeWgcBufferedVideoContentLagMs(
                runtimeState.oldestBufferedFrameAgeUs.load(std::memory_order_relaxed));
            wgcQueueEmptyTickPermille = runtimeState.wgcQueueEmptyTickPermille.load(std::memory_order_relaxed);
            wgcBufferedAtTickMin = runtimeState.wgcBufferedAtTickMin.load(std::memory_order_relaxed);
            wgcSingleFrameTickCount = runtimeState.wgcSingleFrameTickCount.load(std::memory_order_relaxed);
            wgcSelectionBiasUs = runtimeState.wgcSelectionErrorSignedAvgUs.load(std::memory_order_relaxed);
        }
        const uint32_t wgcRecordingCadenceFps =
            ce::audio::GetWgcRecordingCadenceFps(configuredWgcOutputFps, wgcTargetFps);
        if (isWgcCfrRecording) {
            wgcCoverageLossActive = ce::audio::HasWgcUnrecoverableCoverageLoss(
                wgcRecordingCadenceFps, videoPipelineLagMs, wgcBufferedVideoContentLagMs, wgcEncoderBottlenecked,
                wgcDeliveredFps);
        }
        const auto wgcAudioLagTargets = ce::audio::ComputeWgcAudioLagTargets(
            videoPipelineLagMs, wgcBufferedVideoContentLagMs, isWgcCfrRecording && wgcCoverageLossActive,
            kWgcCoverageLossMaxBufferedLagMs);
        const int64_t wgcSteadyStateBufferedAudioLagMs =
            (isWgcCfrRecording && !wgcCoverageLossActive)
                ? ce::audio::ComputeWgcSteadyStateBufferedAudioLagMs(
                      wgcTargetFps, wgcDeliveredFps, wgcDeliveredMin250Fps, wgcDeliveredMin500Fps,
                      wgcEncoderBottlenecked, wgcQueueEmptyTickPermille, wgcBufferedAtTickMin, wgcSingleFrameTickCount)
                : 0;
        const uint32_t effectiveDeliveredFpsForAudioContinuity =
            ce::audio::ComputeEffectiveDeliveredFpsForAudioContinuity(wgcDeliveredFps, wgcDeliveredMin250Fps,
                                                                      wgcDeliveredMin500Fps);
        const bool wgcEncoderOnlyOverload =
            isWgcCfrRecording && ce::audio::ShouldProtectWgcAudioContinuityDuringEncoderOverload(
                                     wgcEncoderBottlenecked, wgcCoverageLossActive, wgcRecordingCadenceFps,
                                     effectiveDeliveredFpsForAudioContinuity, kWgcEncoderHealthyDeliveryMarginFps);
        int64_t wgcEncoderShortfallBufferedLagMs = 0;
        if (isWgcCfrRecording && wgcEncoderOnlyOverload && !kWgcPreferVideoRepeatsOverAudioCuts) {
            wgcEncoderShortfallBufferedLagMs =
                std::clamp<int64_t>(timelineShortfallMs, 0, kWgcEncoderShortfallBufferedLagMaxMs);
        }

        // In WGC CFR mode the scheduled audio timeline is authoritative. When repeated
        // video frames are the preferred recovery mechanism, keep audio targets tied to
        // actual buffered-video lag rather than raw wall-clock encoder lag.
        const int64_t effectiveWgcDriftLagMs =
            isWgcCfrRecording
                ? ce::audio::ComputeWgcCfrDriftLagMs(wgcAudioLagTargets, kWgcPreferVideoRepeatsOverAudioCuts,
                                                     wgcEncoderShortfallBufferedLagMs)
                : videoPipelineLagMs + timelineShortfallMs;
        const int64_t wgcSelectedContentLeadMs =
            isWgcCfrRecording ? ce::audio::ComputeWgcSelectedContentLeadMs(wgcSelectionBiasUs) : 0;
        const int64_t wgcSelectedContentLagMs =
            isWgcCfrRecording ? ce::audio::ComputeWgcSelectedContentLagMs(wgcSelectionBiasUs) : 0;
        const int64_t wgcVisualContentLagMs =
            isWgcCfrRecording
                ? ce::audio::ComputeWgcVisualContentLagMs(timelineShortfallMs, wgcSelectedContentLeadMs,
                                                          wgcSelectedContentLagMs, kWgcVisualSyncMaxBufferedLagMs)
                : 0;
        const int64_t baseEffectiveWgcTargetBufferLagMs =
            isWgcCfrRecording ? ce::audio::ComputeWgcCfrTargetBufferLagMs(
                                    wgcAudioLagTargets, wgcSteadyStateBufferedAudioLagMs,
                                    kWgcPreferVideoRepeatsOverAudioCuts, wgcEncoderShortfallBufferedLagMs)
                              : videoPipelineLagMs + timelineShortfallMs;
        const int64_t effectiveWgcTargetBufferLagMs = baseEffectiveWgcTargetBufferLagMs;
        const int64_t targetBufferedLagCapMs =
            isWgcCfrRecording ? std::max<int64_t>(kWgcCoverageLossMaxBufferedLagMs, effectiveWgcTargetBufferLagMs)
                              : kMaxPipelineLagContributionMs;
        uint32_t maxWgcAudioLeadExcessSamples = 0;

        // Wall-clock audio anchor: when the video timeline has fallen behind real time
        // (shortfall >500ms), use wall-clock elapsed time as the audio pull target instead
        // of the stalled video PTS. Only for non-CFR modes. In CFR mode, video PTS advances
        // at exactly the target framerate regardless of encoder wall-clock speed, so the
        // "shortfall" between wall clock and PTS is expected and harmless. Activating the
        // anchor in CFR mode would pull audio to wall clock, making it advance faster than
        // video PTS and causing unbounded desync.
        const int64_t wallVideoLagMs = timelineShortfallMs;
        if (ce::audio::ShouldAllowWallClockAudioAnchor(isCfrRecording, forceDrain, wallVideoLagMs)) {
            const int64_t steadyElapsedMs = steadyElapsedUs / 1000;
            if (steadyElapsedMs > audioTargetMs && steadyElapsedMs - audioTargetMs > 200) {
                audioTargetUs = steadyElapsedUs;
                audioTargetMs = steadyElapsedMs;
                timelineShortfallMs = 0;
                if (dropLogCounter++ % 500 == 0) {
                    DLL_Log(
                        "[PullAudio] Using wall-clock audio anchor: videoPts=%lldms, wallClock=%lldms, "
                        "pipelineLag=%lldms, shortfall=%lldms",
                        videoTimelineUs / 1000, steadyElapsedMs, videoPipelineLagMs, wallVideoLagMs);
                }
            }
        }

        if (encodedSamplesPerSource.size() != audioSources.size()) {
            encodedSamplesPerSource.resize(audioSources.size(), 0);
        }

        const auto& trackToSources = cachedTrackToSources;
        for (const auto& kv : trackToSources) {
            int track = kv.first;
            const auto& srcIndices = kv.second;
            if (srcIndices.empty())
                continue;
            const TrackAudioFormat trackFormat = GetTrackAudioFormat(track);
            const int CHANNELS = std::clamp(trackFormat.channels, 1, 8);
            const uint32_t CHANNEL_MASK =
                trackFormat.channelMask != 0 ? trackFormat.channelMask : DefaultChannelMaskForChannels(CHANNELS);

            bool trackAllPrimed = true;
            int64_t trackMaxObservedLateStartMs = 0;
            for (size_t srcIdx : srcIndices) {
                auto& src = audioSources[srcIdx];
                const bool isAppAudioSource = (src.sourceType == AudioConfig::AppAudio);

                size_t primedSampleCount = ce::audio::ComputeBufferedRealAudioSamples(
                    src.postResampleBuffer.size() / CHANNELS, src.startupSyntheticPostSamples);
                if (src.ringBuffer) {
                    primedSampleCount += ce::audio::ComputeBufferedRealAudioSamples(
                        src.ringBuffer->GetAvailable() / CHANNELS, src.startupSyntheticRingSamples);
                }
                if (src.hasAlignedStart && !src.isPrimed &&
                    primedSampleCount >= static_cast<size_t>(kPrimedSourceCushionSamples)) {
                    src.isPrimed = true;
                    DLL_Log(
                        "[PullAudio] Source primed - src=%d realBuffered=%zu samples synthetic(ring=%llu inflight=%llu "
                        "post=%llu) lateStart=%lldms",
                        (int)srcIdx, primedSampleCount, (unsigned long long)src.startupSyntheticRingSamples,
                        (unsigned long long)src.startupSyntheticResamplerSamples,
                        (unsigned long long)src.startupSyntheticPostSamples, src.observedLateStartMs);
                }

                trackAllPrimed =
                    trackAllPrimed && ce::audio::IsSourceStartupPrimed(src.isPrimed, src.timelineValid,
                                                                       isAppAudioSource, src.sawSyncPendingPackets);
                trackMaxObservedLateStartMs = std::max(trackMaxObservedLateStartMs, src.observedLateStartMs);
            }

            const bool trackStartupSettled =
                ce::audio::IsTrackAudioStartupSettled(trackBootstrapComplete[track], trackAllPrimed);
            int64_t trackAudioPullLatencyMs =
                forceDrain ? 0
                           : ce::audio::ComputeAudioPullLatencyMs(kSteadyAudioPullLatencyMs, trackStartupSettled,
                                                                  trackMaxObservedLateStartMs);
            if (isCfrRecording) {
                trackAudioPullLatencyMs = ce::audio::ComputeSettledCfrAudioPullLatencyMs(
                    trackAudioPullLatencyMs, trackStartupSettled, trackAllPrimed);
            }
            const int64_t trackAudioTargetUs = audioTargetUs - (std::max<int64_t>(trackAudioPullLatencyMs, 0) * 1000);
            const int64_t trackAudioTargetMs = trackAudioTargetUs > 0 ? (trackAudioTargetUs / 1000) : 0;
            if (trackAudioTargetUs <= 0) {
                continue;
            }

            const int64_t targetSamples = ce::audio::ComputeDurationUsToSamples(trackAudioTargetUs, SAMPLE_RATE);
            const int64_t targetBufferedSamples = ce::audio::ComputeBufferedAudioTargetSamples(
                SAMPLE_RATE, kBaseTargetLatencySamples,
                isWgcCfrRecording ? effectiveWgcTargetBufferLagMs : videoPipelineLagMs, targetBufferedLagCapMs);

            bool trackReadyForBootstrap = true;
            constexpr size_t kMinBootstrapRealSamples = static_cast<size_t>(SAMPLE_RATE / 40);  // 25ms
            const size_t requiredBootstrapSamples =
                ce::audio::ComputeRequiredBootstrapRealSamples(targetSamples, kMinBootstrapRealSamples);
            for (size_t srcIdx : srcIndices) {
                auto& src = audioSources[srcIdx];
                const bool isAppAudioSource = (src.sourceType == AudioConfig::AppAudio);
                size_t bufferedRealSamples = ce::audio::ComputeBufferedRealAudioSamples(
                    src.postResampleBuffer.size() / CHANNELS, src.startupSyntheticPostSamples);
                if (src.ringBuffer) {
                    bufferedRealSamples += ce::audio::ComputeBufferedRealAudioSamples(
                        src.ringBuffer->GetAvailable() / CHANNELS, src.startupSyntheticRingSamples);
                }
                const bool srcReady = ce::audio::IsSourceBootstrapReady(
                    src.bootstrapComplete, src.timelineValid, src.isPrimed, isAppAudioSource, bufferedRealSamples,
                    requiredBootstrapSamples, src.sawSyncPendingPackets);
                const bool packetlessSilenceReady = ce::audio::ShouldBootstrapPacketlessSourceAsSilence(
                    isCfrRecording, src.timelineValid, bufferedRealSamples, targetSamples, requiredBootstrapSamples);
                trackReadyForBootstrap = trackReadyForBootstrap && (srcReady || packetlessSilenceReady);
            }

            if (!trackBootstrapComplete[track]) {
                const bool forcedBootstrap = forceDrain;
                if (!trackReadyForBootstrap && !forcedBootstrap) {
                    int& waitLogCounter = trackBootstrapWaitLogCounters[track];
                    if (waitLogCounter++ % 120 == 0) {
                        DLL_Log("[PullAudio] Track %d bootstrap pending - target=%lldms samples=%lld", track,
                                trackAudioTargetMs, targetSamples);
                    }
                    continue;
                }

                uint64_t bootstrapTrimmed = 0;
                uint64_t bootstrapProtected = 0;
                for (size_t srcIdx : srcIndices) {
                    auto& src = audioSources[srcIdx];
                    const int64_t remainingStartupProtectionSamples = std::max<int64_t>(
                        0, static_cast<int64_t>(src.startupGapProtectionSamples) - encodedSamplesPerSource[srcIdx]);
                    bootstrapProtected += static_cast<uint64_t>(remainingStartupProtectionSamples);
                    src.bootstrapComplete = true;
                }

                trackBootstrapComplete[track] = true;
                trackFirstPullAfterBootstrap[track] = true;
                trackBootstrapWaitLogCounters[track] = 0;
                DLL_Log(
                    "[PullAudio] Track %d bootstrap complete - target=%lldms samples=%lld forced=%d trimmed=%llu "
                    "protected=%llu",
                    track, trackAudioTargetMs, targetSamples, forcedBootstrap ? 1 : 0,
                    (unsigned long long)bootstrapTrimmed, (unsigned long long)bootstrapProtected);
            }

            size_t firstSrcIdx = srcIndices[0];
            int64_t& trackCursorSamples = trackTimelineSamples[track];
            int64_t pendingSamples = targetSamples - trackCursorSamples;
            if (pendingSamples <= 0)
                continue;

            const bool initialTrackCatchup =
                trackFirstPullAfterBootstrap.count(track) && trackFirstPullAfterBootstrap[track];
            const bool overloadPullQuantum = (isWgcCfrRecording && (wgcOverloadFlags & 0x1u) != 0) ||
                                             (isCfrRecording && !isWgcCfrRecording && timelineShortfallMs > 100);
            int64_t samplesToEncode = ce::audio::ComputeAudioSamplesToEncode(
                pendingSamples, isCfrRecording, trackStartupSettled, forceDrain, initialTrackCatchup,
                overloadPullQuantum, ce::audio::kDefaultAudioPullQuantumSamples, kOverloadAudioPullQuantumSamples);
            if (samplesToEncode <= 0) {
                continue;
            }
            if (initialTrackCatchup) {
                trackFirstPullAfterBootstrap[track] = false;
            }

            const int64_t MAX_GAP_SAMPLES = (SAMPLE_RATE * 2);
            const int64_t MAX_SILENCE_CHUNK = SAMPLE_RATE / 2;
            // A track this far behind the live target is catching up after a read-stall (alt-tab, DPC
            // spike, encoder overload). It MUST make forward progress every pull: deferring to wait for
            // an under-buffered source would freeze it forever, because a co-mixed source sitting at the
            // live edge (e.g. a second app keeping up at ~100ms) NEVER accrues the full catch-up chunk.
            // That freeze is exactly how a multi-app track went permanently silent after one app's
            // backlog stalled it - the track deferred every iteration, its cursor froze, and the stalled
            // app's ring saturated. Only fires when >2s behind (a real stall), so normal pulls keep the
            // defer/buffer-wait protection untouched.
            const bool trackLargeBacklogDrain =
                ce::audio::ShouldSuppressBufferDeferForCatchup(samplesToEncode, MAX_GAP_SAMPLES, initialTrackCatchup);
            if (samplesToEncode > MAX_GAP_SAMPLES && !initialTrackCatchup && !isCfrRecording) {
                warpCount++;
                DLL_Log(
                    "[PullAudio] Large A/V gap (%.2f sec) on track %d - inserting silence (warp #%d). target=%lld, "
                    "encoded=%lld",
                    (double)samplesToEncode / SAMPLE_RATE, track, warpCount, targetSamples, trackCursorSamples);

                for (size_t srcIdx : srcIndices) {
                    if (audioSources[srcIdx].ringBuffer) {
                        size_t skippedFloats =
                            audioSources[srcIdx].ringBuffer->Skip(audioSources[srcIdx].ringBuffer->GetAvailable());
                        ce::audio::ConsumeSyntheticBufferedSamples(audioSources[srcIdx].startupSyntheticRingSamples,
                                                                   skippedFloats / CHANNELS);
                        ce::audio::ConsumeSyntheticBufferedSamples(audioSources[srcIdx].startupGapProtectionSamples,
                                                                   skippedFloats / CHANNELS);
                    }
                }

                samplesToEncode = std::min(samplesToEncode, MAX_SILENCE_CHUNK);
            } else if (samplesToEncode > MAX_GAP_SAMPLES && !initialTrackCatchup) {
                warpCount++;
                samplesToEncode = std::min(samplesToEncode, MAX_SILENCE_CHUNK);
                if (dropLogCounter++ % 20 == 0) {
                    DLL_Log(
                        "[PullAudio] Large CFR audio backlog (%.2f sec) on track %d - draining in bounded chunks "
                        "without dropping buffered audio (chunk=%lld samples, target=%lld, encoded=%lld)",
                        (double)pendingSamples / SAMPLE_RATE, track, samplesToEncode, targetSamples,
                        trackCursorSamples);
                }
            }

            bool deferForSourceBuffer = false;
            const bool finalStopDrain = audioStopDrainRequested.load(std::memory_order_acquire) ||
                                        audioFinalizingCfrStop.load(std::memory_order_acquire);
            for (size_t srcIdx : srcIndices) {
                auto& src = audioSources[srcIdx];
                const bool isAppAudioSource = (src.sourceType == AudioConfig::AppAudio);
                const bool optionalUnstarted = ce::audio::IsOptionalUnstartedAppAudioSource(
                    isAppAudioSource, src.timelineValid, src.sawSyncPendingPackets);
                const bool appCaptureRouteEnded =
                    src.appCaptureRouteEnded && src.appCaptureRouteEnded->load(std::memory_order_acquire);
                const bool inactiveStartedAppSourceMaySilence =
                    ce::audio::ShouldTreatInactiveStartedAppCaptureAsSilence(
                        isCfrRecording, isAppAudioSource, src.timelineValid || src.sawSyncPendingPackets,
                        !appCaptureRouteEnded);
                const bool sparseStartedSourceCanSilence = ce::audio::ShouldTreatSparseStartedSourceAsSilence(
                    isCfrRecording, src.timelineValid, src.bootstrapComplete, optionalUnstarted, finalStopDrain);
                const size_t bufferedTimelineSamples = GetBufferedTimelineSamples(src);
                constexpr int64_t kSparseStartedPartialSilenceThresholdSamples =
                    ce::audio::kDefaultAudioPullQuantumSamples * 4;
                const bool sparseStartedSourceMaySilence = ce::audio::ShouldTreatStartedAppSourceShortfallAsSilence(
                    sparseStartedSourceCanSilence, bufferedTimelineSamples, samplesToEncode,
                    kSparseStartedPartialSilenceThresholdSamples) ||
                    inactiveStartedAppSourceMaySilence;
                if (!trackLargeBacklogDrain &&
                    ce::audio::ShouldDeferCfrAudioPullForSourceBuffer(isCfrRecording, forceDrain, optionalUnstarted,
                                                                      sparseStartedSourceMaySilence, samplesToEncode,
                                                                      bufferedTimelineSamples)) {
                    deferForSourceBuffer = true;
                    if (dropLogCounter++ % 500 == 0) {
                        DLL_Log(
                            "[PullAudio] CFR source wait: track=%d src=%zu buffered=%zu requested=%lld "
                            "target=%lldms encoded=%lld. Deferring audio pull to preserve real source samples.",
                            track, srcIdx, bufferedTimelineSamples, samplesToEncode, trackAudioTargetMs,
                            trackCursorSamples);
                    }
                    break;
                }
                if (sparseStartedSourceMaySilence &&
                    bufferedTimelineSamples < static_cast<size_t>(std::max<int64_t>(samplesToEncode, 0)) &&
                    dropLogCounter++ % 500 == 0) {
                    DLL_Log(
                        "[PullAudio] Timeline source gap silence: track=%d src=%zu buffered=%zu requested=%lld "
                        "target=%lldms encoded=%lld inactiveApp=%d. Source contributes available samples plus silence "
                        "for missing range.",
                        track, srcIdx, bufferedTimelineSamples, samplesToEncode, trackAudioTargetMs,
                        trackCursorSamples, inactiveStartedAppSourceMaySilence ? 1 : 0);
                }
            }
            if (deferForSourceBuffer) {
                continue;
            }

            size_t totalFloats = samplesToEncode * CHANNELS;
            std::vector<float> mixBuffer(totalFloats, 0.0f);
            int activeSources = 0;
            int eligibleSources = 0;

            for (size_t srcIdx : srcIndices) {
                auto& src = audioSources[srcIdx];

                const bool isAppAudioSource = (src.sourceType == AudioConfig::AppAudio);
                const bool expectedTimelineSilence =
                    isAppAudioSource && src.appCaptureRouteEnded &&
                    src.appCaptureRouteEnded->load(std::memory_order_acquire);
                if (ce::audio::IsOptionalUnstartedAppAudioSource(isAppAudioSource, src.timelineValid,
                                                                 src.sawSyncPendingPackets)) {
                    continue;
                }
                ++eligibleSources;

                size_t droppedFloats = src.ringBuffer->GetAndClearDroppedSamples();
                if (droppedFloats > 0) {
                    size_t droppedSamples = droppedFloats / CHANNELS;
                    src.overflowDropSamples += droppedSamples;
                    DLL_Log("[PullAudio] WARNING: Ring buffer overflow - %zu samples dropped for src=%d",
                            droppedSamples, (int)srcIdx);
                    src.syncSamplesOutput += (int64_t)droppedSamples;
                }

                size_t retainedFloats = src.ringBuffer->GetAndClearRetainedSamples();
                if (retainedFloats > 0) {
                    size_t retainedSamples = retainedFloats / CHANNELS;
                    ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticRingSamples, retainedSamples);
                    ce::audio::ConsumeSyntheticBufferedSamples(src.startupGapProtectionSamples, retainedSamples);
                    src.retainedNewestTrimSamples += retainedSamples;
                    src.pendingRetainedTrimSamples += retainedSamples;
                    src.pendingRetainedTrimEvents++;
                    src.latencyTrimSamples += retainedSamples;
                    src.pendingLatencyTrimSamples += retainedSamples;
                    src.pendingLatencyTrimEvents++;
                    if (isCfrRecording) {
                        const uint64_t nowTick = GetTickCount64();
                        if (nowTick - src.lastRetainedTrimWarnTick >= 1000) {
                            const size_t rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                            const size_t rbCapacity = src.ringBuffer->GetCapacity() / CHANNELS;
                            DLL_Log(
                                "[PullAudio] WARNING: CFR audio headroom exhausted - trimmed %zu oldest samples "
                                "for src=%d to retain newest audio (buffered=%zu target=%lld cap=%zu "
                                "pipelineLag=%lldms). This may cause audible discontinuities; encoder/capture "
                                "throughput is behind real time.",
                                retainedSamples, (int)srcIdx, rbAvailable, targetBufferedSamples, rbCapacity,
                                videoPipelineLagMs);
                            src.lastRetainedTrimWarnTick = nowTick;
                        }
                    }
                }

                const int64_t remainingStartupProtectionSamples = std::max<int64_t>(
                    0, static_cast<int64_t>(src.startupGapProtectionSamples) - encodedSamplesPerSource[srcIdx]);
                const bool startupTimelineProtected = remainingStartupProtectionSamples > 0;

                const int64_t targetLatencySamples = targetBufferedSamples;
                if (src.bootstrapComplete && src.syncResampler && src.syncResampler->IsReady()) {
                    size_t rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                    const int64_t expectedLeadSamplesForCorrection =
                        std::max<int64_t>(targetLatencySamples,
                                          kBaseTargetLatencySamples + (effectiveWgcDriftLagMs * SAMPLE_RATE / 1000));
                    const int64_t appDrainBudgetSamples = static_cast<int64_t>(rbAvailable);
                    const auto appAudioDrainBudgetDecision = ce::audio::ComputeCfrAppAudioBacklogDrainDecision(
                        isCfrRecording, src.sourceType == AudioConfig::AppAudio, forceDrain, trackStartupSettled,
                        startupTimelineProtected, appDrainBudgetSamples, expectedLeadSamplesForCorrection,
                        kMinCompensationBufferSamples, static_cast<int64_t>(SAMPLE_RATE) * 10,
                        kAppAudioDrainMaxPitchPercent, kAppAudioDrainSlackSamples, kAppAudioDrainDeadbandSamples);
                    const double maxCompensationPercent =
                        appAudioDrainBudgetDecision.active
                            ? kAppAudioDrainMaxPitchPercent
                            : (isCfrRecording ? kTier1MaxPitchPercent : kDefaultMaxCompensationPercent);
                    src.syncResampler->SetMaxCompensationPercent(maxCompensationPercent);
                    const bool allowWgcCoverageLossTrim =
                        isWgcCfrRecording && wgcCoverageLossActive && !kWgcPreferVideoRepeatsOverAudioCuts &&
                        static_cast<int64_t>(rbAvailable) > targetLatencySamples + kWgcCoverageLossLeadSlackSamples;
                    if (!allowWgcCoverageLossTrim) {
                        src.wgcCoverageLossTrimAccumulator = 0.0;
                    }
                    if (!forceDrain && allowWgcCoverageLossTrim && !startupTimelineProtected) {
                        const int64_t dropSamplesTotal = static_cast<int64_t>(rbAvailable) -
                                                         (targetLatencySamples + kWgcCoverageLossLeadSlackSamples);
                        int64_t dropSamples = ce::audio::ComputeWgcCoverageLossTrimSamples(
                            samplesToEncode,
                            ce::audio::ComputeWgcCoverageLossRatio(videoPipelineLagMs, wgcBufferedVideoContentLagMs),
                            src.wgcCoverageLossTrimAccumulator, kWgcCoverageLossMaxDropPerCall);
                        dropSamples = std::min(dropSamples, dropSamplesTotal);
                        if (dropSamples > 0 && src.ringBuffer) {
                            CaptureDropFadeAnchor(src, CHANNELS);
                            src.dropFadeSamplesRemaining = (int)kRuntimeDropFadeSamples;

                            size_t trimmedFloats = src.ringBuffer->Skip((size_t)dropSamples * CHANNELS);
                            size_t trimmedSamples = trimmedFloats / CHANNELS;
                            ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticRingSamples, trimmedSamples);
                            ce::audio::ConsumeSyntheticBufferedSamples(src.startupGapProtectionSamples, trimmedSamples);
                            src.coverageLossTrimSamples += trimmedSamples;
                            src.pendingCoverageLossTrimSamples += trimmedSamples;
                            src.pendingCoverageLossTrimEvents++;
                            src.latencyTrimSamples += trimmedSamples;
                            src.pendingLatencyTrimSamples += trimmedSamples;
                            src.pendingLatencyTrimEvents++;
                            if (dropLogCounter++ % 500 == 0) {
                                DLL_Log(
                                    "[PullAudio] WGC overload sync trim: src %d ahead by %lld samples - trimming %zu "
                                    "(target=%lld, slack=%lld, pipelineLag=%lldms, contentLag=%lldms, delivered=%u/%u "
                                    "fps, ratio=%.3f%%)",
                                    (int)srcIdx, static_cast<int64_t>(rbAvailable) - targetLatencySamples,
                                    trimmedSamples, targetLatencySamples, kWgcCoverageLossLeadSlackSamples,
                                    videoPipelineLagMs, wgcBufferedVideoContentLagMs, wgcDeliveredFps, wgcTargetFps,
                                    ce::audio::ComputeWgcCoverageLossRatio(videoPipelineLagMs,
                                                                           wgcBufferedVideoContentLagMs) *
                                        100.0);
                            }
                            rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                        }
                    } else if (!forceDrain && isWgcCfrRecording && wgcCoverageLossActive &&
                               kWgcPreferVideoRepeatsOverAudioCuts &&
                               static_cast<int64_t>(rbAvailable) >
                                   targetLatencySamples + kWgcCoverageLossLeadSlackSamples &&
                               dropLogCounter++ % 500 == 0) {
                        DLL_Log(
                            "[PullAudio] WGC source-limited CFR repeats active: preserving continuous audio and "
                            "expecting CFR video "
                            "repeats to absorb mismatch (src=%d ahead=%lld target=%lld "
                            "slack=%lld pipelineLag=%lldms contentLag=%lldms wgcFrameLead=%lldms "
                            "wgcFrameLag=%lldms wgcSelBias=%lldus delivered=%u/%u fps ratio=%.3f%%)",
                            (int)srcIdx, static_cast<int64_t>(rbAvailable) - targetLatencySamples, targetLatencySamples,
                            kWgcCoverageLossLeadSlackSamples, videoPipelineLagMs, wgcBufferedVideoContentLagMs,
                            wgcSelectedContentLeadMs, wgcVisualContentLagMs, wgcSelectionBiasUs, wgcDeliveredFps,
                            wgcTargetFps,
                            ce::audio::ComputeWgcCoverageLossRatio(videoPipelineLagMs, wgcBufferedVideoContentLagMs) *
                                100.0);
                    } else if (!forceDrain && !startupTimelineProtected && !isCfrRecording) {
                        int64_t dropSamplesTotal = ce::audio::ComputeLeadTrimExcessSamples(
                            static_cast<int64_t>(rbAvailable), targetLatencySamples, kRuntimeMaxLeadSamples,
                            kLatencyTrimHysteresisSamples);
                        int64_t dropSamples = std::min(dropSamplesTotal, kRuntimeMaxDropPerCall);
                        if (dropSamples > 0 && src.ringBuffer) {
                            CaptureDropFadeAnchor(src, CHANNELS);
                            src.dropFadeSamplesRemaining = (int)kRuntimeDropFadeSamples;

                            size_t trimmedFloats = src.ringBuffer->Skip((size_t)dropSamples * CHANNELS);
                            size_t trimmedSamples = trimmedFloats / CHANNELS;
                            ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticRingSamples, trimmedSamples);
                            ce::audio::ConsumeSyntheticBufferedSamples(src.startupGapProtectionSamples, trimmedSamples);
                            src.latencyTrimSamples += trimmedSamples;
                            src.pendingLatencyTrimSamples += trimmedSamples;
                            src.pendingLatencyTrimEvents++;
                            if (dropLogCounter++ % 500 == 0) {
                                DLL_Log(
                                    "[PullAudio] Audio latency cap: src %d ahead by %lld samples - trimming %lld "
                                    "(capped from %lld, target=%lld, pipelineLag=%lldms)",
                                    (int)srcIdx, (int64_t)rbAvailable - targetLatencySamples, trimmedSamples,
                                    dropSamplesTotal, targetLatencySamples, videoPipelineLagMs);
                            }
                            rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                        }
                    } else if (isWgcCfrRecording && !wgcEncoderOnlyOverload && !kWgcPreferVideoRepeatsOverAudioCuts) {
                        const int64_t expectedLeadSamplesForCap = expectedLeadSamplesForCorrection;
                        if (static_cast<int64_t>(rbAvailable) > expectedLeadSamplesForCap + kWgcCfrLeadWarningSamples) {
                            // WGC CFR lead is large.  Log diagnostics and do paced trimming
                            // to prevent unbounded lead growth when the PI controller can't
                            // keep up with source-clock drift.
                            constexpr int64_t kWgcLeadHardCapSamples = SAMPLE_RATE / 2;  // 500ms hard cap
                            const int64_t leadExcess = static_cast<int64_t>(rbAvailable) - expectedLeadSamplesForCap;

                            if (dropLogCounter++ % 500 == 0) {
                                const bool targetCompensationSaturated = src.targetRateSaturated;
                                const int32_t currentCompensationDelta = src.currentRateDelta;
                                const int32_t targetCompensationDelta = src.targetRateDelta;
                                const int32_t maxCompensationDelta = src.syncResampler->GetMaxCompensationDelta();
                                const double currentCompensationPercent = (double)currentCompensationDelta * 100.0 /
                                                                          (static_cast<double>(SAMPLE_RATE) * 10.0);
                                DLL_Log(
                                    "[PullAudio] WGC CFR lead warning: src %d ahead by %lld samples (target=%lld, "
                                    "pipelineLag=%lldms, encBottleneck=%d). Tier1 drift correction active "
                                    "(corr=%d/%d per 10s target=%d, %.4f%%, sat=%d, maxBudget=%.2f%%)%s",
                                    (int)srcIdx, leadExcess, expectedLeadSamplesForCap, videoPipelineLagMs,
                                    wgcEncoderBottlenecked ? 1 : 0, currentCompensationDelta, maxCompensationDelta,
                                    targetCompensationDelta, currentCompensationPercent,
                                    targetCompensationSaturated ? 1 : 0, maxCompensationPercent,
                                    targetCompensationSaturated ? " - source clock mismatch exceeds tier1 budget" : "");
                            }

                            // Paced lead trimming when lead exceeds hard cap (500ms).
                            // Prevents unbounded growth while keeping fades smooth.
                            if (!forceDrain && leadExcess > kWgcLeadHardCapSamples && src.ringBuffer) {
                                const int64_t excessAboveCap = leadExcess - kWgcLeadHardCapSamples;
                                const int64_t maxTrimThisCall =
                                    std::min(excessAboveCap, kWgcCoverageLossMaxDropPerCall);
                                if (maxTrimThisCall > 0) {
                                    CaptureDropFadeAnchor(src, CHANNELS);
                                    src.dropFadeSamplesRemaining = (int)kRuntimeDropFadeSamples;

                                    size_t trimmedFloats = src.ringBuffer->Skip((size_t)maxTrimThisCall * CHANNELS);
                                    size_t trimmedSamples = trimmedFloats / CHANNELS;
                                    ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticRingSamples,
                                                                               trimmedSamples);
                                    ce::audio::ConsumeSyntheticBufferedSamples(src.startupGapProtectionSamples,
                                                                               trimmedSamples);
                                    src.coverageLossTrimSamples += trimmedSamples;
                                    src.pendingCoverageLossTrimSamples += trimmedSamples;
                                    src.pendingCoverageLossTrimEvents++;
                                    src.latencyTrimSamples += trimmedSamples;
                                    src.pendingLatencyTrimSamples += trimmedSamples;
                                    src.pendingLatencyTrimEvents++;
                                    if (dropLogCounter++ % 100 == 0) {
                                        DLL_Log(
                                            "[PullAudio] WGC CFR lead cap trim: src %d lead=%lld (cap=%lld) - "
                                            "trimmed %lld samples",
                                            (int)srcIdx, leadExcess, kWgcLeadHardCapSamples, (long long)trimmedSamples);
                                    }
                                    rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                                }
                            }
                        }
                    }
                    if (isWgcCfrRecording) {
                        const int64_t expectedLeadForMax = expectedLeadSamplesForCorrection;
                        const int64_t audioLeadExcessSamples =
                            std::max<int64_t>(0, static_cast<int64_t>(rbAvailable) - expectedLeadForMax);
                        maxWgcAudioLeadExcessSamples = std::max<uint32_t>(
                            maxWgcAudioLeadExcessSamples,
                            static_cast<uint32_t>(std::min<int64_t>(audioLeadExcessSamples, INT32_MAX)));
                    }

                    // Non-CFR modes may still use legacy drift correction. CFR
                    // allows only tiny source-clock correction after startup. CFR app-audio backlog drain is
                    // the explicit exception: it pulls process-loopback content back toward the live target.
                    {
                        const int64_t rbLevel = static_cast<int64_t>(rbAvailable);
                        const int64_t compensationBufferedSamples = rbLevel;
                        if (src.sourceType == AudioConfig::AppAudio && src.captureFanoutOwnerIndex == srcIdx) {
                            const auto [groupMinSamples, groupMaxSamples] = GetCaptureGroupBufferedSampleRange(srcIdx);
                            constexpr int64_t kCaptureGroupDivergenceWarnSamples = SAMPLE_RATE / 20;
                            const int64_t groupSpreadSamples = groupMaxSamples - groupMinSamples;
                            const uint64_t nowGroupTick = GetTickCount64();
                            if (groupSpreadSamples >= kCaptureGroupDivergenceWarnSamples &&
                                nowGroupTick - src.lastCaptureGroupDivergenceWarnTick >= 5000) {
                                ProcessLoopbackCapture* routedCapture = GetAppCaptureForRoute(srcIdx);
                                DLL_Log(
                                    "[AudioRoute] WARNING: shared app capture route backlog diverged owner=%zu "
                                    "process=%s min=%lld max=%lld spread=%lld (%.1fms) captureActive=%d. "
                                    "Applying route-local compensation so a delayed sibling cannot starve this "
                                    "route.",
                                    srcIdx, src.config.processName.empty() ? "<none>" : src.config.processName.c_str(),
                                    static_cast<long long>(groupMinSamples), static_cast<long long>(groupMaxSamples),
                                    static_cast<long long>(groupSpreadSamples),
                                    static_cast<double>(groupSpreadSamples) * 1000.0 / SAMPLE_RATE,
                                    routedCapture && routedCapture->IsCapturing() ? 1 : 0);
                                src.lastCaptureGroupDivergenceWarnTick = nowGroupTick;
                            }
                        }
                        if (compensationBufferedSamples >= kMinCompensationBufferSamples) {
                            const int64_t expectedLead = expectedLeadSamplesForCorrection;
                            const int64_t trueDrift = compensationBufferedSamples - expectedLead;
                            const auto appAudioDrainDecision = ce::audio::ComputeCfrAppAudioBacklogDrainDecision(
                                isCfrRecording, src.sourceType == AudioConfig::AppAudio, forceDrain,
                                trackStartupSettled, startupTimelineProtected, compensationBufferedSamples,
                                expectedLead, kMinCompensationBufferSamples, static_cast<int64_t>(SAMPLE_RATE) * 10,
                                kAppAudioDrainMaxPitchPercent, kAppAudioDrainSlackSamples,
                                kAppAudioDrainDeadbandSamples);
                            const double activeMaxCompensationPercent =
                                appAudioDrainDecision.active
                                    ? kAppAudioDrainMaxPitchPercent
                                    : (isCfrRecording ? kTier1MaxPitchPercent : kDefaultMaxCompensationPercent);
                            src.syncResampler->SetMaxCompensationPercent(activeMaxCompensationPercent);

                            if (src.sourceType == AudioConfig::AppAudio) {
                                const uint32_t newReason = static_cast<uint32_t>(appAudioDrainDecision.reason);
                                const bool stateChanged =
                                    !src.appAudioBacklogDrainInitialized ||
                                    src.appAudioBacklogDrainActive != appAudioDrainDecision.active ||
                                    src.appAudioBacklogDrainReason != newReason;
                                src.appAudioBacklogTargetSamples = appAudioDrainDecision.targetLeadSamples;
                                src.appAudioBacklogExcessSamples = appAudioDrainDecision.excessSamples;
                                src.appAudioBacklogCompensationDelta = appAudioDrainDecision.compensationDelta;
                                if (stateChanged) {
                                    if (src.appAudioBacklogDrainInitialized || appAudioDrainDecision.active) {
                                        const double compPct =
                                            static_cast<double>(appAudioDrainDecision.compensationDelta) * 100.0 /
                                            (static_cast<double>(SAMPLE_RATE) * 10.0);
                                        DLL_Log(
                                            "[AppDrain] state src=%zu track=%d active=%d reason=%s delayMs=%lld "
                                            "targetMs=%lld excessMs=%lld rb=%lld target=%lld delta=%d comp=%.4f%% "
                                            "forceDrain=%d startupSettled=%d startupProtected=%d",
                                            srcIdx, src.track, appAudioDrainDecision.active ? 1 : 0,
                                            ce::audio::CfrAppAudioBacklogDrainReasonName(appAudioDrainDecision.reason),
                                            (long long)(appAudioDrainDecision.backlogSamples * 1000 / SAMPLE_RATE),
                                            (long long)(appAudioDrainDecision.targetLeadSamples * 1000 / SAMPLE_RATE),
                                            (long long)(appAudioDrainDecision.excessSamples * 1000 / SAMPLE_RATE),
                                            (long long)appAudioDrainDecision.backlogSamples,
                                            (long long)appAudioDrainDecision.targetLeadSamples,
                                            appAudioDrainDecision.compensationDelta, compPct, forceDrain ? 1 : 0,
                                            trackStartupSettled ? 1 : 0, startupTimelineProtected ? 1 : 0);
                                    }
                                    if (src.appAudioBacklogDrainInitialized) {
                                        src.appLatencyDrainTransitions++;
                                    }
                                }
                                src.appAudioBacklogDrainInitialized = true;
                                src.appAudioBacklogDrainActive = appAudioDrainDecision.active;
                                src.appAudioBacklogDrainReason = newReason;
                            }

                            // Drift sanity check: detect extreme drift that indicates measurement error
                            if (std::abs(trueDrift) > SAMPLE_RATE * 2) {  // >2 seconds
                                const uint64_t nowWarnTick = GetTickCount64();
                                if (nowWarnTick - src.lastExtremeDriftWarnTick >= 1000) {
                                    DLL_Log(
                                        "[PullAudio] WARNING: Extreme drift detected (%lld samples src=%d) - may "
                                        "indicate sync issue",
                                        trueDrift, (int)srcIdx);
                                    src.lastExtremeDriftWarnTick = nowWarnTick;
                                }
                            }

                            const int64_t nowVideoMs = (encodedSamplesPerSource[srcIdx] * 1000) / SAMPLE_RATE;

                            // Debug logging for drift calculation (periodic)
                            if (driftLogCounter++ % 2000 == 0) {
                                DLL_Log(
                                    "[PullAudio] Drift debug src=%d: syncOutput=%lld encoded=%lld nowVideo=%lld "
                                    "wallVideo=%lld trueDrift=%lld rbLevel=%lld",
                                    (int)srcIdx, src.syncSamplesOutput, encodedSamplesPerSource[srcIdx], nowVideoMs,
                                    wallVideoMs, trueDrift, rbLevel);
                            }

                            if (src.lastRateUpdateMs <= 0) {
                                src.lastRateUpdateMs = nowVideoMs;
                            } else {
                                const int64_t updateElapsed = nowVideoMs - src.lastRateUpdateMs;

                                if (updateElapsed >= 500) {
                                    // --- Tier 1: bounded source-clock correction ---
                                    // Keep a tiny correction lane for real device clock drift. It must not spend
                                    // buffered lead that only exists because the CFR video timeline is behind wall
                                    // clock or because WGC/source coverage is missing.
                                    int32_t tier1Delta = 0;

                                    if (isCfrRecording) {
                                        if (appAudioDrainDecision.active) {
                                            tier1Delta = appAudioDrainDecision.compensationDelta;
                                            const int32_t maxCompensationDelta =
                                                src.syncResampler->GetMaxCompensationDelta();
                                            src.targetRateSaturated =
                                                tier1Delta != 0 && std::abs(tier1Delta) >= maxCompensationDelta &&
                                                appAudioDrainDecision.excessSamples > maxCompensationDelta;
                                        } else {
                                            const bool allowCfrSourceClockCorrection =
                                                ce::audio::ShouldAllowCfrSourceClockDriftCompensation(
                                                    isCfrRecording, forceDrain, trackStartupSettled,
                                                    startupTimelineProtected, wgcEncoderBottlenecked,
                                                    timelineShortfallMs, wgcCoverageLossActive);
                                            if (allowCfrSourceClockCorrection) {
                                                tier1Delta = ce::audio::ComputeTier1CompensationDeltaWithDeadband(
                                                    trueDrift, static_cast<int64_t>(SAMPLE_RATE) * 10,
                                                    kTier1MaxPitchPercent, SAMPLE_RATE / 12);
                                                const int32_t maxCompensationDelta =
                                                    src.syncResampler->GetMaxCompensationDelta();
                                                src.targetRateSaturated =
                                                    tier1Delta != 0 && std::abs(tier1Delta) >= maxCompensationDelta &&
                                                    std::abs(trueDrift) > maxCompensationDelta;
                                                if (isWgcCfrRecording && tier1Delta > 0) {
                                                    const int64_t positiveCompensationHysteresisSamples =
                                                        ce::audio::ComputeWgcPositiveCompensationHysteresisSamples(
                                                            targetLatencySamples, kWgcCfrLeadWarningSamples);
                                                    const bool allowSteadyStatePositiveCompensation =
                                                        ce::audio::ShouldAllowWgcSteadyStateDriftCompensation(
                                                            trackStartupSettled, videoPipelineLagMs, rbLevel,
                                                            targetLatencySamples, kWgcCfrLeadWarningSamples);
                                                    if (ce::audio::ShouldClearWgcPositiveDriftCompensation(
                                                            allowSteadyStatePositiveCompensation, rbLevel, expectedLead,
                                                            positiveCompensationHysteresisSamples)) {
                                                        tier1Delta = 0;
                                                    } else {
                                                        tier1Delta = static_cast<int32_t>(
                                                            ce::audio::ClampWgcPositiveDriftCorrection(
                                                                tier1Delta, positiveCompensationHysteresisSamples));
                                                    }
                                                }
                                            } else {
                                                src.targetRateSaturated = false;
                                            }
                                        }
                                    } else {
                                        const int32_t maxDelta = src.syncResampler->GetMaxCompensationDelta();
                                        tier1Delta =
                                            static_cast<int32_t>(std::clamp(trueDrift, static_cast<int64_t>(-maxDelta),
                                                                            static_cast<int64_t>(maxDelta)));
                                        src.targetRateSaturated = false;
                                    }
                                    src.targetRateDelta = tier1Delta;

                                    constexpr int32_t kMaxRateChange = 1500;
                                    int32_t newDelta = static_cast<int32_t>(
                                        std::clamp(static_cast<int64_t>(tier1Delta),
                                                   static_cast<int64_t>(src.currentRateDelta) - kMaxRateChange,
                                                   static_cast<int64_t>(src.currentRateDelta) + kMaxRateChange));

                                    src.currentRateDelta = newDelta;
                                    if (src.sourceType == AudioConfig::AppAudio) {
                                        src.appLatencyMaxAbsCompDelta =
                                            std::max<uint32_t>(src.appLatencyMaxAbsCompDelta,
                                                               static_cast<uint32_t>(std::abs(src.currentRateDelta)));
                                        src.appAudioBacklogCompensationDelta = newDelta;
                                    }

                                    if (newDelta != 0 || src.rateCompActive) {
                                        int ret = swr_set_compensation(src.syncResampler->GetSwrContext(), -newDelta,
                                                                       SAMPLE_RATE * 10);
                                        if (ret >= 0) {
                                            src.rateCompActive = (newDelta != 0);
                                        }
                                    }

                                    // --- Tier 2: Ring buffer trim with crossfade ---
                                    // Activates when drift exceeds what Tier 1 can handle alone.
                                    // Normally suppressed in WGC CFR mode (prefer video repeats over
                                    // audio cuts), and enabled for non-CFR modes when the wall-clock
                                    // audio anchor is active (encoder severely stalled).
                                    const bool wallClockAnchorActive = ce::audio::ShouldAllowWallClockAudioAnchor(
                                        isCfrRecording, forceDrain, wallVideoLagMs);
                                    if (isWgcCfrRecording && !wgcEncoderOnlyOverload && !startupTimelineProtected &&
                                        (!kWgcPreferVideoRepeatsOverAudioCuts || wallClockAnchorActive) &&
                                        ce::audio::ShouldActivateTier2Trim(trueDrift, SAMPLE_RATE,
                                                                           kTier2DriftThresholdMs) &&
                                        src.ringBuffer) {
                                        const int64_t tier2TrimBudget = ce::audio::ComputeTier2TrimBudget(
                                            trueDrift, SAMPLE_RATE, kRuntimeMaxDropPerCall);
                                        const int64_t tier2MaxTrim = std::min(tier2TrimBudget, std::abs(trueDrift));
                                        if (tier2MaxTrim > 0 && static_cast<int64_t>(rbAvailable) >
                                                                    targetLatencySamples + kRuntimeDropFadeSamples) {
                                            CaptureDropFadeAnchor(src, CHANNELS);
                                            src.dropFadeSamplesRemaining = (int)kRuntimeDropFadeSamples;

                                            size_t trimmedFloats =
                                                src.ringBuffer->Skip((size_t)tier2MaxTrim * CHANNELS);
                                            size_t trimmedSamples = trimmedFloats / CHANNELS;
                                            ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticRingSamples,
                                                                                       trimmedSamples);
                                            ce::audio::ConsumeSyntheticBufferedSamples(src.startupGapProtectionSamples,
                                                                                       trimmedSamples);
                                            src.latencyTrimSamples += trimmedSamples;
                                            src.tier2TrimSamples += trimmedSamples;
                                            src.pendingLatencyTrimSamples += trimmedSamples;
                                            src.pendingLatencyTrimEvents++;
                                            src.pendingTier2TrimSamples += trimmedSamples;
                                            src.pendingTier2TrimEvents++;
                                            rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                                        }
                                    }

                                    if (driftLogCounter++ % 500 == 0) {
                                        const double compensationPercent = (double)src.currentRateDelta * 100.0 /
                                                                           (static_cast<double>(SAMPLE_RATE) * 10.0);
                                        const bool tier2WouldActivate = ce::audio::ShouldActivateTier2Trim(
                                            trueDrift, SAMPLE_RATE, kTier2DriftThresholdMs);
                                        // Whether the tier2 trim path is actually permitted to run this pull. In WGC
                                        // CFR it is intentionally suppressed (prefer video repeats over audio cuts),
                                        // so the common "tier2=1 tier2Applied=0" pair means the drift threshold is
                                        // exceeded but the standing buffer is deliberately PRESERVED, not trimmed -
                                        // benign extra latency that stays out of the encoded timeline (content is
                                        // placed by packet QPC), NOT an audio cut. Logged distinctly so the standing
                                        // startup backlog is not misread as an active trim/convergence failure.
                                        const bool tier2TrimEnabled =
                                            isWgcCfrRecording && !wgcEncoderOnlyOverload && !startupTimelineProtected &&
                                            (!kWgcPreferVideoRepeatsOverAudioCuts || wallClockAnchorActive);
                                        DLL_Log(
                                            "[PullAudio] Src %zu drift: "
                                            "trueDrift=%lld "
                                            "(rb=%lld expected=%lld "
                                            "pipelineLag=%lldms) "
                                            "tier1=%d (%.4f%%) "
                                            "tier2=%d tier2Applied=%d encBottleneck=%d",
                                            srcIdx, trueDrift, rbLevel, expectedLead, effectiveWgcDriftLagMs, newDelta,
                                            compensationPercent, tier2WouldActivate ? 1 : 0,
                                            (tier2WouldActivate && tier2TrimEnabled) ? 1 : 0,
                                            wgcEncoderBottlenecked ? 1 : 0);
                                    }

                                    src.lastRateUpdateMs = nowVideoMs;
                                }
                            }
                        }
                    }

                    // Overflow protection. Non-CFR modes keep the historical short cap. CFR logs pressure instead
                    // of proactively trimming; if the ring actually overflows, validation must fail the recording.
                    if (!forceDrain && src.ringBuffer && !startupTimelineProtected) {
                        constexpr int64_t kMaxOverflowSamples = SAMPLE_RATE / 2;            // 500ms max overflow
                        constexpr int64_t kWgcCfrEmergencyRingMarginSamples = SAMPLE_RATE;  // 1s before full
                        // A catastrophic backlog (a sustained read-stall from alt-tab, a DPC latency spike,
                        // or encoder overload, while live process-loopback capture keeps writing) is the one
                        // case where the CFR "never trim audio" policy must yield: otherwise the ring saturates
                        // and the source goes permanently silent. 2s is far above any legitimate jitter, so the
                        // healthy steady state (~100-200ms backlog) is never touched. Generic, not device tuning.
                        constexpr int64_t kCatastrophicAppBacklogSamples = SAMPLE_RATE * 2;  // 2s = stall, not jitter
                        rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                        const int64_t rbCapacitySamples =
                            static_cast<int64_t>(src.ringBuffer->GetCapacity() / CHANNELS);

                        // Resync a catastrophically backlogged CFR app source to the live edge so a read-stall
                        // cannot end the track in permanent silence. Drops stale backlog, keeps the newest audio,
                        // and fades the seam (CaptureDropFadeAnchor). One discontinuity after a real stall, never
                        // a dead track. Below the threshold the no-trim policy is preserved untouched.
                        const int64_t catastrophicResyncTrim = ce::audio::ComputeCatastrophicBacklogResyncTrim(
                            isCfrRecording, src.sourceType == AudioConfig::AppAudio, static_cast<int64_t>(rbAvailable),
                            targetLatencySamples, kCatastrophicAppBacklogSamples,
                            SAMPLE_RATE / 10 /* keep >= 100ms live cushion */);
                        if (catastrophicResyncTrim > 0) {
                            const int64_t backlogBefore = static_cast<int64_t>(rbAvailable);
                            CaptureDropFadeAnchor(src, CHANNELS);
                            src.dropFadeSamplesRemaining = (int)kRuntimeDropFadeSamples;
                            size_t trimmedFloats =
                                src.ringBuffer->Skip(static_cast<size_t>(catastrophicResyncTrim) * CHANNELS);
                            size_t trimmedSamples = trimmedFloats / CHANNELS;
                            ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticRingSamples, trimmedSamples);
                            ce::audio::ConsumeSyntheticBufferedSamples(src.startupGapProtectionSamples, trimmedSamples);
                            src.latencyTrimSamples += trimmedSamples;
                            src.catastrophicResyncSamples += trimmedSamples;
                            src.catastrophicResyncEvents++;
                            rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                            const uint64_t nowTick = GetTickCount64();
                            if (nowTick - src.lastCatastrophicResyncTick >= 1000) {
                                DLL_Log(
                                    "[PullAudio] App source catastrophic backlog resync - src %d dropped %lld stale "
                                    "samples (%.2fs) to live edge after read-stall (backlogWas=%lld now=%lld "
                                    "target=%lld cap=%lld event#%u). Recovery from alt-tab/DPC/encoder stall; the "
                                    "track stays live instead of going permanently silent.",
                                    (int)srcIdx, (long long)trimmedSamples,
                                    static_cast<double>(trimmedSamples) / SAMPLE_RATE, (long long)backlogBefore,
                                    (long long)rbAvailable, (long long)targetLatencySamples,
                                    (long long)rbCapacitySamples, src.catastrophicResyncEvents);
                                src.lastCatastrophicResyncTick = nowTick;
                            }
                        }

                        const int64_t overflowCapSamples = ce::audio::ComputeRuntimeOverflowCapSamples(
                            isCfrRecording, targetLatencySamples, rbCapacitySamples, kMaxOverflowSamples,
                            kWgcCfrEmergencyRingMarginSamples);
                        const int64_t overflowExcess =
                            static_cast<int64_t>(rbAvailable) - targetLatencySamples - overflowCapSamples;
                        if (overflowExcess > kRuntimeDropFadeSamples && !isCfrRecording) {
                            CaptureDropFadeAnchor(src, CHANNELS);
                            src.dropFadeSamplesRemaining = (int)kRuntimeDropFadeSamples;

                            size_t trimmedFloats = src.ringBuffer->Skip((size_t)overflowExcess * CHANNELS);
                            size_t trimmedSamples = trimmedFloats / CHANNELS;
                            ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticRingSamples, trimmedSamples);
                            ce::audio::ConsumeSyntheticBufferedSamples(src.startupGapProtectionSamples, trimmedSamples);
                            src.latencyTrimSamples += trimmedSamples;
                            src.pendingLatencyTrimSamples += trimmedSamples;
                            src.pendingLatencyTrimEvents++;
                            rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                            if (dropLogCounter++ % 500 == 0) {
                                DLL_Log(
                                    "[PullAudio] Ring buffer overflow protection%s: src %d trimmed %lld samples "
                                    "(rb was %lld samples, target %lld, overflow cap %lld)",
                                    isCfrRecording ? " (CFR emergency capacity guard)" : "", (int)srcIdx,
                                    (long long)trimmedSamples, (long long)(rbAvailable + trimmedSamples),
                                    (long long)targetLatencySamples, (long long)overflowCapSamples);
                            }
                        } else if (overflowExcess > kRuntimeDropFadeSamples && dropLogCounter++ % 500 == 0) {
                            DLL_Log(
                                "[PullAudio] WARNING: CFR audio ring near capacity - src %d excess=%lld "
                                "rb=%lld target=%lld cap=%lld capacity=%lld. Preserving audio; any overflow/drop is "
                                "a validation failure.",
                                (int)srcIdx, (long long)overflowExcess, (long long)rbAvailable,
                                (long long)targetLatencySamples, (long long)overflowCapSamples,
                                (long long)rbCapacitySamples);
                        }
                    }
                }

                if (src.syncResampler && src.syncResampler->IsReady()) {
                    const size_t MAX_CHUNK_FLOATS = (size_t)(SAMPLE_RATE * CHANNELS / 10);
                    while (src.postResampleBuffer.size() < totalFloats) {
                        size_t rbFloats = src.ringBuffer->GetAvailable();
                        if (rbFloats == 0) {
                            src.ringBufferUnderrunCount++;
                            break;
                        }
                        size_t rbSamples = rbFloats / CHANNELS;
                        if (rbSamples > src.ringBufferPeakSamples) {
                            src.ringBufferPeakSamples = rbSamples;
                        }

                        size_t needFloats = totalFloats - src.postResampleBuffer.size();
                        size_t chunkFloats = std::min(rbFloats, needFloats);
                        chunkFloats = std::min(chunkFloats, MAX_CHUNK_FLOATS);
                        chunkFloats -= (chunkFloats % CHANNELS);
                        if (chunkFloats == 0) {
                            break;
                        }

                        std::vector<float> rbData(chunkFloats);
                        size_t actualRead = src.ringBuffer->Read(rbData.data(), chunkFloats);
                        if (actualRead == 0) {
                            break;
                        }

                        size_t actualReadSamples = actualRead / CHANNELS;
                        uint64_t syntheticReadSamples = ce::audio::ConsumeSyntheticBufferedSamples(
                            src.startupSyntheticRingSamples, actualReadSamples);
                        src.startupSyntheticResamplerSamples += syntheticReadSamples;

                        uint8_t** resampledData = nullptr;
                        int outSamples = 0;
                        if (src.syncResampler->Process((uint8_t*)rbData.data(), (int)(actualRead * sizeof(float)),
                                                       &resampledData, &outSamples)) {
                            uint64_t syntheticPostSamples = ce::audio::ConsumeSyntheticBufferedSamples(
                                src.startupSyntheticResamplerSamples, (uint64_t)std::max(outSamples, 0));
                            src.startupSyntheticPostSamples += syntheticPostSamples;
                            if (outSamples > 0 && resampledData && resampledData[0]) {
                                float* outFloats = (float*)resampledData[0];
                                if (src.dropFadeSamplesRemaining > 0) {
                                    const int kDropFadeSamples = SAMPLE_RATE / 40;  // 25ms - smoother transitions
                                    int blendSamples = std::min(src.dropFadeSamplesRemaining, outSamples);
                                    int blendStart = kDropFadeSamples - src.dropFadeSamplesRemaining;
                                    for (int s = 0; s < blendSamples; s++) {
                                        float alpha = (float)(blendStart + s + 1) / kDropFadeSamples;
                                        for (int ch = 0; ch < CHANNELS; ++ch) {
                                            const size_t idx = static_cast<size_t>(s) * CHANNELS + ch;
                                            const float anchor = GetDropFadeAnchor(src, ch);
                                            outFloats[idx] = anchor + (outFloats[idx] - anchor) * alpha;
                                        }
                                    }
                                    if (blendSamples > 0) {
                                        src.dropFadeStart.assign(static_cast<size_t>(CHANNELS), 0.0f);
                                        const size_t base = static_cast<size_t>(blendSamples - 1) * CHANNELS;
                                        for (int ch = 0; ch < CHANNELS; ++ch) {
                                            src.dropFadeStart[static_cast<size_t>(ch)] = outFloats[base + ch];
                                        }
                                        src.dropFadeStartL = src.dropFadeStart[0];
                                        src.dropFadeStartR = CHANNELS > 1 ? src.dropFadeStart[1] : src.dropFadeStart[0];
                                    }
                                    src.dropFadeSamplesRemaining -= blendSamples;
                                }
                                if (src.packetBoundaryFadeInSamplesRemaining > 0) {
                                    const int blendSamples =
                                        std::min(src.packetBoundaryFadeInSamplesRemaining, outSamples);
                                    for (int s = 0; s < blendSamples; ++s) {
                                        const float alpha = ComputeRaisedCosineFade(
                                            static_cast<size_t>(s),
                                            static_cast<size_t>(std::max(src.packetBoundaryFadeInSamplesRemaining, 1)));
                                        const size_t base = static_cast<size_t>(s) * CHANNELS;
                                        for (int ch = 0; ch < CHANNELS; ++ch) {
                                            outFloats[base + ch] *= alpha;
                                        }
                                    }
                                    src.packetBoundaryFadeInSamplesRemaining -= blendSamples;
                                }
                                if (src.pendingUnderrunRecoveryFade) {
                                    src.underrunFadeSamplesRemaining = SAMPLE_RATE / 40;  // 25ms - smoother transitions
                                    src.pendingUnderrunRecoveryFade = false;
                                }
                                if (src.underrunFadeSamplesRemaining > 0) {
                                    const int kUnderrunFadeSamples = SAMPLE_RATE / 40;  // 25ms - smoother transitions
                                    int blendSamples = std::min(src.underrunFadeSamplesRemaining, outSamples);
                                    int blendStart = kUnderrunFadeSamples - src.underrunFadeSamplesRemaining;
                                    for (int s = 0; s < blendSamples; s++) {
                                        float alpha = (float)(blendStart + s + 1) / kUnderrunFadeSamples;
                                        const size_t base = static_cast<size_t>(s) * CHANNELS;
                                        for (int ch = 0; ch < CHANNELS; ++ch) {
                                            outFloats[base + ch] *= alpha;
                                        }
                                    }
                                    src.underrunFadeSamplesRemaining -= blendSamples;
                                }
                                int numFloats = outSamples * CHANNELS;
                                src.postResampleBuffer.insert(src.postResampleBuffer.end(), outFloats,
                                                              outFloats + numFloats);
                                src.syncSamplesOutput += outSamples;

                                // Log sample trim stats periodically
                                if (dropLogCounter++ % 500 == 0 &&
                                    (src.overflowDropSamples > 0 || src.latencyTrimSamples > 0 ||
                                     src.postResampleTrimSamples > 0)) {
                                    const uint64_t categorizedLatencyTrim =
                                        std::min(src.latencyTrimSamples,
                                                 src.bootstrapTrimSamples + src.retainedNewestTrimSamples +
                                                     src.coverageLossTrimSamples + src.tier2TrimSamples);
                                    const uint64_t uncategorizedLatencyTrim =
                                        src.latencyTrimSamples - categorizedLatencyTrim;
                                    DLL_Log(
                                        "[PullAudio] Sample trim stats src=%d: overflowDropped=%llu "
                                        "latencyTrimTotal=%llu bootstrapTrim=%llu retainedTrim=%llu "
                                        "coverageTrim=%llu tier2Trim=%llu uncategorizedLiveTrim=%llu "
                                        "postResampleTrim=%llu",
                                        (int)srcIdx, (unsigned long long)src.overflowDropSamples,
                                        (unsigned long long)src.latencyTrimSamples,
                                        (unsigned long long)src.bootstrapTrimSamples,
                                        (unsigned long long)src.retainedNewestTrimSamples,
                                        (unsigned long long)src.coverageLossTrimSamples,
                                        (unsigned long long)src.tier2TrimSamples,
                                        (unsigned long long)uncategorizedLatencyTrim,
                                        (unsigned long long)src.postResampleTrimSamples);
                                }
                            }
                        }
                        AudioResampler::FreeOutputBuffer(resampledData);
                    }
                }

                const size_t MIN_POST_RESAMPLE_FLOATS = (size_t)(SAMPLE_RATE * CHANNELS / 20);
                const size_t startupProtectedFloats =
                    static_cast<size_t>(std::max<int64_t>(0, remainingStartupProtectionSamples)) * CHANNELS;
                const size_t MAX_POST_RESAMPLE_FLOATS =
                    std::max(totalFloats * 4, MIN_POST_RESAMPLE_FLOATS + startupProtectedFloats);
                if (src.postResampleBuffer.size() > MAX_POST_RESAMPLE_FLOATS && !isCfrRecording) {
                    size_t excess = src.postResampleBuffer.size() - MAX_POST_RESAMPLE_FLOATS;
                    ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticPostSamples, excess / CHANNELS);
                    ce::audio::ConsumeSyntheticBufferedSamples(src.startupGapProtectionSamples, excess / CHANNELS);
                    src.postResampleTrimSamples += excess / CHANNELS;

                    CaptureDropFadeAnchor(src, CHANNELS);
                    src.dropFadeSamplesRemaining = (int)kRuntimeDropFadeSamples;

                    if (dropLogCounter++ % 100 == 0) {
                        DLL_Log(
                            "[PullAudio] WARNING: Post-resample buffer trim - src %d dropping %zu samples (buffer=%zu "
                            "cap=%zu)",
                            (int)srcIdx, excess / CHANNELS, src.postResampleBuffer.size() / CHANNELS,
                            MAX_POST_RESAMPLE_FLOATS / CHANNELS);
                    }
                    src.postResampleBuffer.erase(src.postResampleBuffer.begin(),
                                                 src.postResampleBuffer.begin() + (std::ptrdiff_t)excess);
                } else if (src.postResampleBuffer.size() > MAX_POST_RESAMPLE_FLOATS && dropLogCounter++ % 500 == 0) {
                    DLL_Log(
                        "[PullAudio] WARNING: CFR post-resample backlog exceeded guard - src %d backlog=%zu cap=%zu. "
                        "Preserving audio; underrun/overflow validation will report any real failure.",
                        (int)srcIdx, src.postResampleBuffer.size() / CHANNELS, MAX_POST_RESAMPLE_FLOATS / CHANNELS);
                }

                std::vector<float> srcData(totalFloats, 0.0f);
                size_t available = src.postResampleBuffer.size();
                size_t toCopy = std::min(available, totalFloats);
                const size_t syntheticCopiedSamples =
                    std::min<uint64_t>(src.startupSyntheticPostSamples, toCopy / CHANNELS);
                const size_t realCopiedSamples = (toCopy / CHANNELS) - syntheticCopiedSamples;

                if (toCopy > 0) {
                    std::copy(src.postResampleBuffer.begin(), src.postResampleBuffer.begin() + toCopy, srcData.begin());
                    src.postResampleBuffer.erase(src.postResampleBuffer.begin(),
                                                 src.postResampleBuffer.begin() + toCopy);
                    ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticPostSamples, toCopy / CHANNELS);
                    if (realCopiedSamples > 0 && src.pendingStartupJoinFade) {
                        ApplyPacketBoundaryFadeIn(srcData.data() + syntheticCopiedSamples * CHANNELS, realCopiedSamples,
                                                  CHANNELS,
                                                  static_cast<size_t>(std::max<int64_t>(1, SAMPLE_RATE / 200)));
                        src.pendingStartupJoinFade = false;
                    }
                    if (realCopiedSamples > 0) {
                        activeSources++;
                        src.startupRealAudioSeen = true;
                    }
                }
                // Consume/drain diagnostics (throttled 1/s, app sources). If an app source has
                // real audio sitting in its ring (rbAvail large) but contributes 0 real samples
                // to this pull (postResampleBuffer empty -> fullSilence), the drain stalled even
                // though data exists. syncReady=0 or a stuck swr compensation are the suspects.
                if (src.sourceType == AudioConfig::AppAudio) {
                    const uint64_t nowConsumeTick = GetTickCount64();
                    const size_t rbAvailSamples = src.ringBuffer ? src.ringBuffer->GetAvailable() / CHANNELS : 0;
                    const bool starvedWithRingData = (realCopiedSamples == 0 && rbAvailSamples > 0);
                    const int64_t appTargetSamples =
                        std::max<int64_t>(targetLatencySamples,
                                          kBaseTargetLatencySamples + (effectiveWgcDriftLagMs * SAMPLE_RATE / 1000));
                    const int64_t appExcessSamples =
                        std::max<int64_t>(0, static_cast<int64_t>(rbAvailSamples) - appTargetSamples);
                    const uint32_t appTargetMs =
                        static_cast<uint32_t>(std::max<int64_t>(0, appTargetSamples) * 1000 / SAMPLE_RATE);
                    const uint32_t appExcessMs = static_cast<uint32_t>(appExcessSamples * 1000 / SAMPLE_RATE);
                    const double appCompPct =
                        static_cast<double>(src.currentRateDelta) * 100.0 / (static_cast<double>(SAMPLE_RATE) * 10.0);
                    const auto appDrainReason =
                        static_cast<ce::audio::CfrAppAudioBacklogDrainReason>(src.appAudioBacklogDrainReason);
                    src.appAudioBacklogTargetSamples = appTargetSamples;
                    src.appAudioBacklogExcessSamples = appExcessSamples;
                    src.appLatencyMaxAbsCompDelta = std::max<uint32_t>(
                        src.appLatencyMaxAbsCompDelta, static_cast<uint32_t>(std::abs(src.currentRateDelta)));

                    // Latency observability: the ring backlog at consume time IS the audio-behind-video
                    // delay (buffered audio waiting to be emitted). Sample it EVERY pull so the
                    // recording-wide distribution and any elevated/variable latency are obvious in the
                    // logs rather than needing manual reconstruction from raw cursor values.
                    const uint32_t appDelayMs =
                        static_cast<uint32_t>(static_cast<uint64_t>(rbAvailSamples) * 1000ull / SAMPLE_RATE);
                    const int latBucket = appDelayMs < 50    ? 0
                                          : appDelayMs < 150 ? 1
                                          : appDelayMs < 300 ? 2
                                          : appDelayMs < 600 ? 3
                                                             : 4;
                    src.appLatencyBuckets[latBucket]++;
                    src.appLatencySampleCount++;
                    src.appLatencySumMs += appDelayMs;
                    src.appLatencyTargetSumMs += appTargetMs;
                    src.appLatencyExcessSumMs += appExcessMs;
                    if (appDelayMs > src.appLatencyMaxMs) {
                        src.appLatencyMaxMs = appDelayMs;
                    }
                    if (appExcessMs > src.appLatencyExcessMaxMs) {
                        src.appLatencyExcessMaxMs = appExcessMs;
                    }
                    if (src.appAudioBacklogDrainActive) {
                        src.appLatencyDrainingSamples++;
                    }

                    // Flag clearly-elevated latency loudly WHILE it happens (the signal that was missing).
                    // Post-fix the drain should keep this rare; frequent firing means latency is not draining.
                    constexpr uint32_t kAppLatencyWarnMs = 250;
                    const bool appLatencyElevated =
                        appExcessSamples >= kAppAudioLatencyWarnExcessSamples || appDelayMs >= kAppLatencyWarnMs;
                    const bool appLatencyWarnChanged = appLatencyElevated != src.appLatencyWarnActive;
                    if (appLatencyWarnChanged) {
                        ProcessLoopbackCapture* routedCapture = GetAppCaptureForRoute(srcIdx);
                        const size_t pendingPackets = routedCapture ? routedCapture->PendingPacketCount() : 0;
                        const uint64_t queueOverrunPackets =
                            routedCapture ? routedCapture->GetQueueOverrunPacketCount() : 0;
                        const uint64_t queueOverrunFrames =
                            routedCapture ? routedCapture->GetQueueOverrunFrameCount() : 0;
                        DLL_Log(
                            "[AppLatency] state src=%zu track=%d elevated=%d delayMs=%u targetMs=%u excessMs=%u "
                            "drain=%d reason=%s compDelta=%d comp=%.4f%% rbAvail=%zu queuePending=%zu "
                            "queueOverrun=%llu/%llu underruns=%u",
                            srcIdx, track, appLatencyElevated ? 1 : 0, appDelayMs, appTargetMs, appExcessMs,
                            src.appAudioBacklogDrainActive ? 1 : 0,
                            ce::audio::CfrAppAudioBacklogDrainReasonName(appDrainReason), src.currentRateDelta,
                            appCompPct, rbAvailSamples, pendingPackets,
                            static_cast<unsigned long long>(queueOverrunPackets),
                            static_cast<unsigned long long>(queueOverrunFrames), src.ringBufferUnderrunCount);
                        src.appLatencyWarnActive = appLatencyElevated;
                    }
                    if (appLatencyElevated && nowConsumeTick - src.lastAppLatencyWarnTick >= 5000) {
                        ProcessLoopbackCapture* routedCapture = GetAppCaptureForRoute(srcIdx);
                        const size_t pendingPackets = routedCapture ? routedCapture->PendingPacketCount() : 0;
                        const uint64_t queueOverrunPackets =
                            routedCapture ? routedCapture->GetQueueOverrunPacketCount() : 0;
                        const uint64_t queueOverrunFrames =
                            routedCapture ? routedCapture->GetQueueOverrunFrameCount() : 0;
                        DLL_Log(
                            "[AppLatency] WARNING: app audio src=%zu track=%d delayMs=%u targetMs=%u excessMs=%u "
                            "rbAvail=%zu drain=%d reason=%s compDelta=%d comp=%.4f%% rateCompActive=%d "
                            "underruns=%u queuePending=%zu queueOverrun=%llu/%llu. Content backlog should drain "
                            "toward the video target without trims.",
                            srcIdx, track, appDelayMs, appTargetMs, appExcessMs, rbAvailSamples,
                            src.appAudioBacklogDrainActive ? 1 : 0,
                            ce::audio::CfrAppAudioBacklogDrainReasonName(appDrainReason), src.currentRateDelta,
                            appCompPct, src.rateCompActive ? 1 : 0, src.ringBufferUnderrunCount, pendingPackets,
                            static_cast<unsigned long long>(queueOverrunPackets),
                            static_cast<unsigned long long>(queueOverrunFrames));
                        src.lastAppLatencyWarnTick = nowConsumeTick;
                    }

                    if (nowConsumeTick - src.lastAppConsumeDiagTick >= 1000) {
                        ProcessLoopbackCapture* routedCapture = GetAppCaptureForRoute(srcIdx);
                        const size_t pendingPackets = routedCapture ? routedCapture->PendingPacketCount() : 0;
                        const uint64_t queueOverrunPackets =
                            routedCapture ? routedCapture->GetQueueOverrunPacketCount() : 0;
                        const uint64_t queueOverrunFrames =
                            routedCapture ? routedCapture->GetQueueOverrunFrameCount() : 0;
                        DLL_Log(
                            "[AppDiag] consume src=%zu track=%d delayMs=%u targetMs=%u excessMs=%u "
                            "realCopied=%zu postResampleBuf=%zu rbAvail=%zu syncReady=%d drain=%d reason=%s "
                            "compDelta=%d comp=%.4f%% rateCompActive=%d underruns=%u queuePending=%zu "
                            "queueOverrun=%llu/%llu trims(lat=%llu cat=%u/%llu)%s",
                            srcIdx, track, appDelayMs, appTargetMs, appExcessMs, realCopiedSamples,
                            src.postResampleBuffer.size() / CHANNELS, rbAvailSamples,
                            (src.syncResampler && src.syncResampler->IsReady()) ? 1 : 0,
                            src.appAudioBacklogDrainActive ? 1 : 0,
                            ce::audio::CfrAppAudioBacklogDrainReasonName(appDrainReason), src.currentRateDelta,
                            appCompPct, src.rateCompActive ? 1 : 0, src.ringBufferUnderrunCount, pendingPackets,
                            static_cast<unsigned long long>(queueOverrunPackets),
                            static_cast<unsigned long long>(queueOverrunFrames),
                            static_cast<unsigned long long>(src.latencyTrimSamples), src.catastrophicResyncEvents,
                            static_cast<unsigned long long>(src.catastrophicResyncSamples),
                            starvedWithRingData ? " STARVED_WITH_RING_DATA" : "");
                        src.lastAppConsumeDiagTick = nowConsumeTick;
                    }
                }
                if (toCopy < totalFloats) {
                    // Underrun: apply a short fade-out on the last real samples
                    // before the silence boundary to prevent audible clicks.
                    if (toCopy > 0) {
                        constexpr int kFadeSamples = 8;  // ~0.17ms at 48kHz
                        int realSamples = static_cast<int>(toCopy / CHANNELS);
                        int fadeStart = std::max(0, realSamples - kFadeSamples);
                        for (int s = fadeStart; s < realSamples; ++s) {
                            float alpha = static_cast<float>(realSamples - s) / static_cast<float>(kFadeSamples + 1);
                            const size_t base = static_cast<size_t>(s) * CHANNELS;
                            for (int ch = 0; ch < CHANNELS; ++ch) {
                                srcData[base + ch] *= alpha;
                            }
                        }
                    }
                    size_t padSamples = (totalFloats - toCopy) / CHANNELS;
                    const bool startupPadding = !src.bootstrapComplete;
                    if (!startupPadding && !expectedTimelineSilence) {
                        src.underrunPadSamples += padSamples;
                        if (src.syncResampler && src.syncResampler->IsReady() && src.rateCompActive) {
                            if (swr_set_compensation(src.syncResampler->GetSwrContext(), 0, SAMPLE_RATE * 10) >= 0) {
                                src.prevLeadSamples = 0;
                                src.prevLeadSnapshotMs = 0;
                                src.lastRateUpdateMs = 0;
                                src.currentRateDelta = 0;
                                src.targetRateDelta = 0;
                                src.rateCompActive = false;
                                src.targetRateSaturated = false;
                                src.ringBufferPeakSamples = 0;
                                src.ringBufferUnderrunCount = 0;
                            }
                        }
                        constexpr size_t kMaxSinglePadSamples = SAMPLE_RATE / 50;  // 20ms
                        if (padSamples > kMaxSinglePadSamples && dropLogCounter++ % 20 == 0) {
                            DLL_Log(
                                "[PullAudio] WARNING: Large single padding event - src %d padding %zu samples "
                                "(cap=%zu) - indicates timing problem, not transient underrun",
                                (int)srcIdx, padSamples, kMaxSinglePadSamples);
                        }
                        constexpr size_t kTotalPadWarningSamples = SAMPLE_RATE / 10;  // 100ms
                        if (src.underrunPadSamples > kTotalPadWarningSamples &&
                            (src.underrunPadSamples - padSamples) <= kTotalPadWarningSamples) {
                            DLL_Log(
                                "[PullAudio] WARNING: Total silence padding exceeded %zu samples (%.1fms) for src %d - "
                                "ring buffer is being drained too fast",
                                kTotalPadWarningSamples, (double)src.underrunPadSamples * 1000.0 / SAMPLE_RATE,
                                (int)srcIdx);
                        }
                    }
                    src.pendingUnderrunRecoveryFade = !startupPadding && !expectedTimelineSilence;
                    if (startupPadding && realCopiedSamples == 0) {
                        src.pendingStartupJoinFade = true;
                    }
                    if (!startupPadding && !expectedTimelineSilence && src.alignedStartMs >= 0 &&
                        padSamples >= (size_t)(SAMPLE_RATE / 200) &&
                        dropLogCounter++ % 100 == 0) {
                        DLL_Log(
                            "[PullAudio] WARNING: Source underrun - src %d padding %zu samples with silence "
                            "(available=%zu needed=%zu forceDrain=%d)",
                            (int)srcIdx, padSamples, available / CHANNELS, totalFloats / CHANNELS, forceDrain ? 1 : 0);
                    } else if (startupPadding && padSamples >= (size_t)(SAMPLE_RATE / 50) &&
                               dropLogCounter++ % 200 == 0) {
                        DLL_Log(
                            "[PullAudio] Startup padding - src %d aligned=%d primed=%d boot=%d pad=%zu available=%zu "
                            "need=%zu",
                            (int)srcIdx, (int)src.hasAlignedStart, (int)src.isPrimed, (int)src.bootstrapComplete,
                            padSamples, available / CHANNELS, totalFloats / CHANNELS);
                    }
                }

                for (size_t i = 0; i < totalFloats; i++) {
                    mixBuffer[i] += srcData[i];
                }
            }

            // If ALL sources for this track are silent (game pause), we MUST generate
            // silence. Otherwise, the Audio Stream timestamps stop advancing, and
            // av_interleaved_write_frame will BUFFER VIDEO PACKETS INDEFINITELY
            // waiting for audio to catch up. This causes the 32GB RAM leak.

            bool applyTransitionFade = false;

            if (activeSources == 0) {
                const bool wasSilent = trackWasSilent[track];
                trackWasSilent[track] = true;
                trackSilentSamples[track] += static_cast<uint64_t>(samplesToEncode);
                trackSilentChunks[track]++;
                const uint64_t nowTick = GetTickCount64();
                const uint64_t lastLogTick = trackLastSilenceLogTick[track];
                if (!wasSilent) {
                    trackSilenceTransitions[track]++;
                    DLL_Log(
                        "[PullAudio] Track %d entered silence: generated=%lld samples active=%d/%d "
                        "transitions=%llu target=%lldms encoded=%lld",
                        track, samplesToEncode, activeSources, eligibleSources,
                        static_cast<unsigned long long>(trackSilenceTransitions[track]), trackAudioTargetMs,
                        trackCursorSamples);
                    trackLastSilenceLogTick[track] = nowTick;
                } else if (nowTick - lastLogTick >= 1000) {
                    DLL_Log(
                        "[PullAudio] Track %d still silent: total=%llu samples (%.1fms) chunks=%llu active=%d/%d "
                        "target=%lldms encoded=%lld",
                        track, static_cast<unsigned long long>(trackSilentSamples[track]),
                        static_cast<double>(trackSilentSamples[track]) * 1000.0 / SAMPLE_RATE,
                        static_cast<unsigned long long>(trackSilentChunks[track]), activeSources, eligibleSources,
                        trackAudioTargetMs, trackCursorSamples);
                    trackLastSilenceLogTick[track] = nowTick;
                }
                if (silenceLogCounter++ % 500 == 0) {
                    DLL_Log(
                        "[PullAudio] Track %d silent - generating %lld samples of "
                        "silence to maintain sync",
                        track, samplesToEncode);
                }

                // Zero-fill the mix buffer (which is already zeroed by constructor, but
                // explicit is good)
                std::fill(mixBuffer.begin(), mixBuffer.end(), 0.0f);

                // We still 'processed' the mix (it's just silence)
                // Encode below...
            } else {
                auto it = trackWasSilent.find(track);
                if (it != trackWasSilent.end() && it->second) {
                    applyTransitionFade = true;
                    DLL_Log(
                        "[PullAudio] Track %d resumed after silence: silentTotal=%llu samples (%.1fms) chunks=%llu "
                        "active=%d/%d target=%lldms encoded=%lld",
                        track, static_cast<unsigned long long>(trackSilentSamples[track]),
                        static_cast<double>(trackSilentSamples[track]) * 1000.0 / SAMPLE_RATE,
                        static_cast<unsigned long long>(trackSilentChunks[track]), activeSources, eligibleSources,
                        trackAudioTargetMs, trackCursorSamples);
                    trackSilentSamples[track] = 0;
                    trackSilentChunks[track] = 0;
                }
                trackWasSilent[track] = false;
                if (activeSources < eligibleSources) {
                    const uint64_t nowTick = GetTickCount64();
                    const uint64_t lastLogTick = trackLastSilenceLogTick[track];
                    if (nowTick - lastLogTick >= 1000) {
                        DLL_Log(
                            "[PullAudio] Track %d partial/intermittent silence: active=%d/%d target=%lldms "
                            "encoded=%lld padTotal=%llu qpcGap=%llu qpcOverlap=%llu",
                            track, activeSources, eligibleSources, trackAudioTargetMs, trackCursorSamples,
                            static_cast<unsigned long long>(audioSources[firstSrcIdx].underrunPadSamples),
                            static_cast<unsigned long long>(audioSources[firstSrcIdx].packetTimelineGapSamples),
                            static_cast<unsigned long long>(audioSources[firstSrcIdx].packetTimelineOverlapSamples));
                        trackLastSilenceLogTick[track] = nowTick;
                    }
                }
                // Perform mixing for active sources
                // (Existing logic moved here or just fall through since mixBuffer is
                // already correct?) mixBuffer is already zeroed. We can just skip the
                // "if (activeSources == 0) continue" check and let the flow continue.
                // But we need to handle the "padding" logic inside the source loop.
                // The source loop summed into mixBuffer.
                // If activeSources==0, mixBuffer is [0,0,0...].
                // So we just proceed to encoding!
            }

            // Removed the 'continue' constraint.
            // Soft clipping: Clamp values to [-1, 1] to prevent distortion (only
            // needed if sources > 1)

            // Soft clipping: Clamp values to [-1, 1] to prevent distortion
            {
                int64_t trackPos = trackCursorSamples;
                const bool applyStartupTrackFade =
                    !applyTransitionFade && trackPos == 0 &&
                    audioSources[firstSrcIdx].packetBoundaryFadeInSamplesRemaining <= 0 &&
                    audioSources[firstSrcIdx].underrunFadeSamplesRemaining <= 0 &&
                    !audioSources[firstSrcIdx].pendingUnderrunRecoveryFade;
                const int64_t fadeSamples = applyTransitionFade ? SAMPLE_RATE / 20 : SAMPLE_RATE / 40;
                int64_t fadeStart = applyTransitionFade ? 0 : trackPos;
                if ((applyStartupTrackFade || applyTransitionFade) && fadeSamples > 0) {
                    for (int64_t s = 0; s < samplesToEncode; s++) {
                        int64_t global = fadeStart + s;
                        float gain = (global >= fadeSamples) ? 1.0f : (float)global / (float)fadeSamples;
                        size_t base = (size_t)s * CHANNELS;
                        for (int ch = 0; ch < CHANNELS; ++ch) {
                            mixBuffer[base + ch] *= gain;
                        }
                    }
                }
            }

            // Always use a smooth soft-knee limiter so any residual discontinuity from
            // packet stitching, drift correction, or track transitions cannot turn into
            // a hard clipped click at the encoder input. The limiter is transparent for
            // |sample| <= knee (bit-exact passthrough) and only shapes peaks above it, so
            // in-range audio is untouched (no global tanh waveshaper / no thinning).
            ce::audio::ApplySoftKneeLimiter(mixBuffer.data(), mixBuffer.size());

            // Encode the mixed samples using first source's encoder
            AudioEncoder* encoder = audioSources[firstSrcIdx].sharedEncoderPtr;
            if (encoder) {
                std::vector<uint8_t> encodeData(totalFloats * sizeof(float));
                memcpy(encodeData.data(), mixBuffer.data(), encodeData.size());

                // Calculate timestamp for this audio chunk
                // CRITICAL: Use RELATIVE time from sample count, not absolute QPC!
                // This must match what the encoder expects - time relative to recording
                // start
                int64_t audioChunkTimestampMs = (trackCursorSamples * 1000) / SAMPLE_RATE;

                const AudioEncoder::EncodeResult encodeResult = encoder->EncodeSamples(
                    encodeData.data(), (int)encodeData.size(), CHANNELS, SAMPLE_RATE, 32, 32, CHANNELS * 4,
                    true,  // float32
                    CHANNEL_MASK, audioChunkTimestampMs);
                if (encodeResult.failed || encodeResult.acceptedSamples != samplesToEncode) {
                    DLL_Log(
                        "[PullAudio] ERROR: Track %d encoder acceptance mismatch: requested=%lld accepted=%lld "
                        "submitted=%lld failed=%d cursor=%lld target=%lld",
                        track, static_cast<long long>(samplesToEncode),
                        static_cast<long long>(encodeResult.acceptedSamples),
                        static_cast<long long>(encodeResult.submittedSamples), encodeResult.failed ? 1 : 0,
                        static_cast<long long>(trackCursorSamples), static_cast<long long>(targetSamples));
                }
                samplesToEncode =
                    encodeResult.failed ? 0 : std::min<int64_t>(samplesToEncode, encodeResult.acceptedSamples);

                if (srcIndices.size() > 1 && mixLogCounter++ % 5000 == 0) {
                    DLL_Log("[PullAudio] Mixed %d sources for track %d (%lld samples)", activeSources, track,
                            samplesToEncode);
                }
            }

            if (activeSources == 0) {
                trackFullSilenceSamples[track] += static_cast<uint64_t>(samplesToEncode);
            } else {
                trackRealMixedSamples[track] += static_cast<uint64_t>(samplesToEncode);
                if (activeSources < eligibleSources) {
                    trackPartialSilenceSamples[track] += static_cast<uint64_t>(samplesToEncode);
                }
            }
            trackCursorSamples += samplesToEncode;

            // Keep source counters aligned for source-local diagnostics, but the
            // exported stream timeline is trackTimelineSamples[track].
            for (size_t srcIdx : srcIndices) {
                encodedSamplesPerSource[srcIdx] += samplesToEncode;
            }

            // A/V SYNC MONITORING: Periodic check for drift detection and audio health.
            // Track counters per audio track so multi-track sessions surface every
            // track's health instead of only whichever track hits the shared modulo.
            int& trackSyncCheckCounter = trackSyncCheckCounters[track];
            if (trackSyncCheckCounter++ % 1200 == 0 && firstSrcIdx < encodedSamplesPerSource.size()) {
                int64_t wallVideoMs = videoElapsedMs.load();
                int64_t videoMs = wallVideoMs;
                int64_t encodedVideoMs = wallVideoMs;
                int64_t encodedVideoUsForSummary = wallVideoMs * 1000;
                int64_t scheduledVideoUsForSummary = trackAudioTargetUs;
                if (videoEnc) {
                    int64_t encodedVideoUs = videoEnc->GetEncodedDurationUs();
                    if (encodedVideoUs > 0) {
                        encodedVideoUsForSummary = encodedVideoUs;
                        encodedVideoMs = encodedVideoUs / 1000;
                        if (!isCfrRecording) {
                            videoMs = encodedVideoMs;
                        }
                    }
                    if (isCfrRecording) {
                        const int64_t expectedVideoUs = videoEnc->GetExpectedFinalDurationUs();
                        if (expectedVideoUs > 0) {
                            scheduledVideoUsForSummary = expectedVideoUs;
                        }
                    }
                }
                int64_t audioSamples = trackTimelineSamples[track];
                int64_t audioUs = ce::audio::ComputeSamplesToDurationUs(audioSamples, SAMPLE_RATE);
                int64_t audioMs = audioUs / 1000;
                int64_t avDrift = audioMs - videoMs;
                int64_t latencyAdjustedAvDrift =
                    ce::audio::ComputeLatencyAdjustedAvDriftMs(avDrift, trackAudioPullLatencyMs);
                int64_t pipelineLagMs = ce::audio::ComputeVideoPipelineLagMs(wallVideoMs, encodedVideoMs);
                const int64_t residualSamples = audioSamples - targetSamples;
                const int64_t residualUs = ce::audio::ComputeSamplesToDurationUs(residualSamples, SAMPLE_RATE);
                const int64_t audioVsEncodedUs = audioUs - encodedVideoUsForSummary;
                const int64_t audioVsTargetUs = audioUs - trackAudioTargetUs;
                const int64_t audioVsScheduledUs = audioUs - scheduledVideoUsForSummary;

                // Summarize all sources contributing to this track so issues on a
                // secondary app/system source are visible in the periodic sync log.
                uint64_t overflowDropped = 0;
                uint64_t retainedTrimmed = 0;
                uint64_t latencyTrimmed = 0;
                uint64_t tier2Trimmed = 0;
                uint64_t bootstrapTrimmed = 0;
                uint64_t postTrimmed = 0;
                uint64_t underrunPadded = 0;
                uint64_t coverageLossTrimmed = 0;
                uint64_t packetGapAdjusted = 0;
                uint64_t packetOverlapTrimmed = 0;
                std::string sourceSummary;
                for (size_t srcIdx : srcIndices) {
                    size_t rbLevel = 0;
                    int64_t syncOutput = 0;
                    int64_t alignedStartMs = -1;
                    uint64_t srcOverflowDropped = 0;
                    uint64_t srcRetainedTrimmed = 0;
                    uint64_t srcLatencyTrimmed = 0;
                    uint64_t srcTier2Trimmed = 0;
                    uint64_t srcBootstrapTrimmed = 0;
                    uint64_t srcPostTrimmed = 0;
                    uint64_t srcUnderrunPadded = 0;
                    uint64_t srcCoverageLossTrimmed = 0;
                    uint64_t srcSyntheticRing = 0;
                    uint64_t srcSyntheticInflight = 0;
                    uint64_t srcSyntheticPost = 0;
                    uint64_t srcPacketGapAdjusted = 0;
                    uint64_t srcPacketOverlapTrimmed = 0;
                    bool srcPrimed = false;
                    bool srcBootstrapped = false;
                    bool srcTimelineValid = false;
                    if (srcIdx < audioSources.size()) {
                        auto& src = audioSources[srcIdx];
                        if (src.ringBuffer) {
                            rbLevel = src.ringBuffer->GetAvailable() / CHANNELS;
                        }
                        syncOutput = src.syncSamplesOutput;
                        alignedStartMs = src.alignedStartMs;
                        srcOverflowDropped = src.overflowDropSamples;
                        srcRetainedTrimmed = src.retainedNewestTrimSamples;
                        srcLatencyTrimmed = src.latencyTrimSamples;
                        srcTier2Trimmed = src.tier2TrimSamples;
                        srcBootstrapTrimmed = src.bootstrapTrimSamples;
                        srcPostTrimmed = src.postResampleTrimSamples;
                        srcUnderrunPadded = src.underrunPadSamples;
                        srcCoverageLossTrimmed = src.coverageLossTrimSamples;
                        srcSyntheticRing = src.startupSyntheticRingSamples;
                        srcSyntheticInflight = src.startupSyntheticResamplerSamples;
                        srcSyntheticPost = src.startupSyntheticPostSamples;
                        srcPacketGapAdjusted = src.packetTimelineGapSamples;
                        srcPacketOverlapTrimmed = src.packetTimelineOverlapSamples;
                        srcPrimed = src.isPrimed;
                        srcBootstrapped = src.bootstrapComplete;
                        srcTimelineValid = src.timelineValid;
                    }

                    overflowDropped += srcOverflowDropped;
                    retainedTrimmed += srcRetainedTrimmed;
                    latencyTrimmed += srcLatencyTrimmed;
                    tier2Trimmed += srcTier2Trimmed;
                    bootstrapTrimmed += srcBootstrapTrimmed;
                    postTrimmed += srcPostTrimmed;
                    underrunPadded += srcUnderrunPadded;
                    coverageLossTrimmed += srcCoverageLossTrimmed;
                    packetGapAdjusted += srcPacketGapAdjusted;
                    packetOverlapTrimmed += srcPacketOverlapTrimmed;

                    char sourceState[448];
                    std::snprintf(
                        sourceState, sizeof(sourceState),
                        "src%zu(rb=%zu sync=%lld start=%lld tl=%d primed=%d boot=%d synth=%llu/%llu/%llu ovf=%llu "
                        "rtrim=%llu lat=%llu t2=%llu cov=%llu boottrim=%llu post=%llu pad=%llu qgap=%llu qov=%llu)",
                        srcIdx, rbLevel, syncOutput, alignedStartMs, (int)srcTimelineValid, (int)srcPrimed,
                        (int)srcBootstrapped, (unsigned long long)srcSyntheticRing,
                        (unsigned long long)srcSyntheticInflight, (unsigned long long)srcSyntheticPost,
                        (unsigned long long)srcOverflowDropped, (unsigned long long)srcRetainedTrimmed,
                        (unsigned long long)srcLatencyTrimmed, (unsigned long long)srcTier2Trimmed,
                        (unsigned long long)srcCoverageLossTrimmed, (unsigned long long)srcBootstrapTrimmed,
                        (unsigned long long)srcPostTrimmed, (unsigned long long)srcUnderrunPadded,
                        (unsigned long long)srcPacketGapAdjusted, (unsigned long long)srcPacketOverlapTrimmed);
                    if (!sourceSummary.empty()) {
                        sourceSummary += "; ";
                    }
                    sourceSummary += sourceState;
                }
                if (sourceSummary.empty()) {
                    sourceSummary = "none";
                }
                const uint64_t categorizedLatencyTrim =
                    std::min(latencyTrimmed, bootstrapTrimmed + retainedTrimmed + coverageLossTrimmed + tier2Trimmed);
                const uint64_t uncategorizedLatencyTrim = latencyTrimmed - categorizedLatencyTrim;

                for (size_t srcIdx : srcIndices) {
                    auto& src = audioSources[srcIdx];
                    if (src.pendingRetainedTrimEvents > 0) {
                        DLL_Log(
                            "[PullAudio] Retained-audio trim summary - src=%zu events=%u samples=%llu total=%llu "
                            "target=%lld pipelineLag=%lldms",
                            srcIdx, src.pendingRetainedTrimEvents,
                            static_cast<unsigned long long>(src.pendingRetainedTrimSamples),
                            static_cast<unsigned long long>(src.retainedNewestTrimSamples), targetBufferedSamples,
                            pipelineLagMs);
                        src.pendingRetainedTrimEvents = 0;
                        src.pendingRetainedTrimSamples = 0;
                    }
                    if (src.pendingCoverageLossTrimEvents > 0) {
                        DLL_Log(
                            "[PullAudio] WGC coverage-loss trim summary - src=%zu events=%u samples=%llu total=%llu "
                            "target=%lld pipelineLag=%lldms contentLag=%lldms delivered=%u/%u fps",
                            srcIdx, src.pendingCoverageLossTrimEvents,
                            static_cast<unsigned long long>(src.pendingCoverageLossTrimSamples),
                            static_cast<unsigned long long>(src.coverageLossTrimSamples), targetBufferedSamples,
                            pipelineLagMs, wgcBufferedVideoContentLagMs, wgcDeliveredFps, wgcTargetFps);
                        src.pendingCoverageLossTrimEvents = 0;
                        src.pendingCoverageLossTrimSamples = 0;
                    }
                    if (src.pendingLatencyTrimEvents > 0) {
                        const double trimRatePerMinute =
                            ce::audio::ComputeSamplesPerMinute(src.pendingLatencyTrimSamples, 10000000ll) * 6.0;
                        const uint64_t categorizedLatencyTrim =
                            std::min(src.latencyTrimSamples, src.bootstrapTrimSamples + src.retainedNewestTrimSamples +
                                                                 src.coverageLossTrimSamples + src.tier2TrimSamples +
                                                                 src.catastrophicResyncSamples);
                        const uint64_t uncategorizedLatencyTrim = src.latencyTrimSamples - categorizedLatencyTrim;
                        DLL_Log(
                            "[PullAudio] Latency trim aggregate summary - src=%zu events=%u samples=%llu "
                            "total=%llu bootstrap=%llu retained=%llu coverage=%llu tier2=%llu cat=%llu "
                            "uncategorizedLive=%llu rate=%.1f/min target=%lld pipelineLag=%lldms",
                            srcIdx, src.pendingLatencyTrimEvents,
                            static_cast<unsigned long long>(src.pendingLatencyTrimSamples),
                            static_cast<unsigned long long>(src.latencyTrimSamples),
                            static_cast<unsigned long long>(src.bootstrapTrimSamples),
                            static_cast<unsigned long long>(src.retainedNewestTrimSamples),
                            static_cast<unsigned long long>(src.coverageLossTrimSamples),
                            static_cast<unsigned long long>(src.tier2TrimSamples),
                            static_cast<unsigned long long>(src.catastrophicResyncSamples),
                            static_cast<unsigned long long>(uncategorizedLatencyTrim), trimRatePerMinute,
                            targetBufferedSamples, pipelineLagMs);
                        src.pendingLatencyTrimEvents = 0;
                        src.pendingLatencyTrimSamples = 0;
                    }
                    if (src.pendingTier2TrimEvents > 0) {
                        DLL_Log(
                            "[PullAudio] Tier2 drift trim summary - src=%zu events=%u samples=%llu total=%llu "
                            "overallLatencyTrim=%llu target=%lld pipelineLag=%lldms",
                            srcIdx, src.pendingTier2TrimEvents,
                            static_cast<unsigned long long>(src.pendingTier2TrimSamples),
                            static_cast<unsigned long long>(src.tier2TrimSamples),
                            static_cast<unsigned long long>(src.latencyTrimSamples), targetBufferedSamples,
                            pipelineLagMs);
                        src.pendingTier2TrimEvents = 0;
                        src.pendingTier2TrimSamples = 0;
                    }
                }

                DLL_Log(
                    "[A/V SYNC CHECK] Track %d: Video=%lld ms, Audio=%lld ms, "
                    "Drift=%lld ms, DriftAdj=%lld ms, Pull=%lld ms, VideoWall=%lld ms, VideoEnc=%lld ms, "
                    "audio_vs_encoded_us=%+lld audio_vs_target_us=%+lld audio_vs_scheduled_us=%+lld "
                    "residual_samples=%+lld residual_us=%+lld target_samples=%lld cursor_samples=%lld "
                    "target_us=%lld cursor_us=%lld encoded_video_us=%lld scheduled_video_us=%lld "
                    "PipelineLag=%lld ms, "
                    "ContentLag=%lld ms, CovMode=%d, EncBottleneck=%d, Delivered=%u/%u, Over=0x%x, "
                    "TargetBuf=%lld ms, WgcFrameLead=%lld ms, WgcFrameLag=%lld ms, WgcSelBias=%lld us, Overflow=%llu, "
                    "RetainTrim=%llu, CoverageTrim=%llu, Tier2Trim=%llu, BootstrapTrim=%llu, "
                    "LatencyTrimTotal=%llu, UncategorizedLiveTrim=%llu, PostTrim=%llu, Pad=%llu, QpcGap=%llu, "
                    "QpcOverlap=%llu, Sources=%s",
                    track, videoMs, audioMs, avDrift, latencyAdjustedAvDrift, trackAudioPullLatencyMs, wallVideoMs,
                    encodedVideoMs, audioVsEncodedUs, audioVsTargetUs, audioVsScheduledUs, residualSamples, residualUs,
                    targetSamples, audioSamples, trackAudioTargetUs, audioUs, encodedVideoUsForSummary,
                    scheduledVideoUsForSummary, pipelineLagMs,
                    isWgcCfrRecording ? wgcBufferedVideoContentLagMs : pipelineLagMs, wgcCoverageLossActive ? 1 : 0,
                    wgcEncoderBottlenecked ? 1 : 0, wgcDeliveredFps, wgcTargetFps, wgcOverloadFlags,
                    (targetBufferedSamples * 1000) / SAMPLE_RATE, wgcSelectedContentLeadMs, wgcVisualContentLagMs,
                    wgcSelectionBiasUs, (unsigned long long)overflowDropped, (unsigned long long)retainedTrimmed,
                    (unsigned long long)coverageLossTrimmed, (unsigned long long)tier2Trimmed,
                    (unsigned long long)bootstrapTrimmed, (unsigned long long)latencyTrimmed,
                    (unsigned long long)uncategorizedLatencyTrim, (unsigned long long)postTrimmed,
                    (unsigned long long)underrunPadded, (unsigned long long)packetGapAdjusted,
                    (unsigned long long)packetOverlapTrimmed, sourceSummary.c_str());

                DLL_Log(
                    "[A/V SYNC SUMMARY] Track %d: Wall=%lldms EncV=%lldms Audio=%lldms Drift=%+lldms "
                    "audio_vs_encoded_us=%+lld audio_vs_target_us=%+lld audio_vs_scheduled_us=%+lld "
                    "residual_samples=%+lld residual_us=%+lld target_samples=%lld cursor_samples=%lld "
                    "PipelineLag=%lldms PullLatency=%lldms WgcFrameLead=%lldms WgcFrameLag=%lldms EncBot=%d "
                    "Delivered=%u/%u",
                    track, wallVideoMs, encodedVideoMs, audioMs, avDrift, audioVsEncodedUs, audioVsTargetUs,
                    audioVsScheduledUs, residualSamples, residualUs, targetSamples, audioSamples, pipelineLagMs,
                    trackAudioPullLatencyMs, wgcSelectedContentLeadMs, wgcVisualContentLagMs,
                    wgcEncoderBottlenecked ? 1 : 0, wgcDeliveredFps, wgcTargetFps);

                if (isCfrRecording && trackStartupSettled && residualSamples != 0) {
                    DLL_Log(
                        "[A/V ZERO DRIFT WARNING] Track %d residual_samples=%+lld residual_us=%+lld "
                        "target_samples=%lld cursor_samples=%lld target_us=%lld cursor_us=%lld "
                        "audio_vs_encoded_us=%+lld audio_vs_target_us=%+lld",
                        track, residualSamples, residualUs, targetSamples, audioSamples, trackAudioTargetUs, audioUs,
                        audioVsEncodedUs, audioVsTargetUs);
                }

                if (std::abs(latencyAdjustedAvDrift) > 100) {
                    DLL_Log(
                        "[A/V SYNC WARNING] Track %d adjusted drift exceeds 100ms! raw=%lldms adjusted=%lldms "
                        "pull=%lldms",
                        track, avDrift, latencyAdjustedAvDrift, trackAudioPullLatencyMs);
                }
            }
        }
        // A pull may have consumed the last post-resampler samples of an old epoch.
        // Acknowledge only after that content has actually entered the common track cursor.
        for (size_t srcIdx = 0; srcIdx < audioSources.size(); ++srcIdx) {
            ServiceAudioEpochResetOnPull(audioSources[srcIdx], srcIdx);
        }
        if (sharedMemLayout) {
            sharedMemLayout->runtimeState.wgcAudioLeadExcessSamples.store(
                isWgcCfrRecording ? maxWgcAudioLeadExcessSamples : 0u, std::memory_order_relaxed);
        }
    }

    // Create shared D3D11 textures for Vulkan games to import
    bool CreateSharedCaptureTextures(uint32_t width, uint32_t height, uint32_t format, SharedMemoryLayout* sharedMem) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (!videoEnc) {
            DLL_Log("MediaEngine: CreateSharedCaptureTextures - no encoder");
            return false;
        }
        if (!sharedMem) {
            DLL_Log("MediaEngine: CreateSharedCaptureTextures - sharedMem is null");
            return false;
        }

        // IMPORTANT: Set encoder dimensions and LUID from the parameters before
        // EnsureDevice Otherwise EnsureDevice fails because width/height are still
        // 0 or uses wrong GPU
        videoEnc->SetDimensions(width, height);
        videoEnc->SetAdapterLUID(sharedMem->GetLuidLowPart(), sharedMem->GetLuidHighPart());

        if (!videoEnc->EnsureDevice()) {
            DLL_Log(
                "MediaEngine: CreateSharedCaptureTextures - device init failed "
                "for LUID %08x:%08x",
                sharedMem->GetLuidLowPart(), sharedMem->GetLuidHighPart());
            return false;
        }
        return videoEnc->CreateSharedCaptureTextures(width, height, format, sharedMem);
    }

    void WritePacket(AVPacket* pkt) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (audioOnly && audioOnlyFmtCtx) {
            if (pkt->stream_index >= 0 && (unsigned int)pkt->stream_index < audioOnlyFmtCtx->nb_streams) {
                AVStream* st = audioOnlyFmtCtx->streams[pkt->stream_index];
                AVRational codec_tb = {1, st->codecpar->sample_rate};
                if (codec_tb.den > 0)
                    av_packet_rescale_ts(pkt, codec_tb, st->time_base);
            }
            av_interleaved_write_frame(audioOnlyFmtCtx, pkt);
        } else if (videoEnc) {
            videoEnc->WriteFrame(pkt);
        }
    }

    void ReloadConfig(const AppConfig* newConfig) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        DLL_Log("MediaEngine::ReloadConfig called");

        // Update config
        this->config = *newConfig;
        trackAudioFormats = ResolveTrackAudioFormats(*newConfig);
        DLL_Log("[AVSyncAuto] engine_reload: resolvedRenderLatencyMs=%.3f confidence=%s reason=%s usedAudioProbe=%d",
                static_cast<double>(this->config.avSyncResolvedRenderLatencyMs), this->config.avSyncConfidence.c_str(),
                this->config.avSyncReason.c_str(), this->config.avSyncUsedAudioProbe ? 1 : 0);

        // If recording, we can't fully re-init, but we can log a warning.
        if (recording) {
            DLL_Log(
                "MediaEngine: Config updated, but recording is active. Changes "
                "will apply on next recording.");
            return;
        }

        DLL_Log("MediaEngine: Re-initializing encoders with new config...");

        // Clear audio sources (and their encoders)
        audioSources.clear();
        DLL_Log("MediaEngine: Cleared existing audio sources");

        // Re-create VideoEncoder to apply all new settings
        videoEnc.reset();
        videoEnc = std::make_unique<VideoEncoder>();

        bool vRes =
            videoEnc->Init(config.video, 0, 0, config.video.fps, [this](AVPacket* pkt) { this->WritePacket(pkt); });

        if (!vRes) {
            DLL_Log("MediaEngine: Failed to re-init VideoEncoder!");
            return;
        }
        DLL_Log("MediaEngine: VideoEncoder re-initialized successfully.");

        // Re-create audio sources with new config (including new codec)
        // Maps track number to encoder for that track
        std::map<int, AudioEncoder*> trackToEncoder;
        // Guards against summing two identical app-audio captures into one track.
        std::set<std::string> seenAppAudioTrackKeys;

        for (size_t i = 0; i < config.audioSources.size(); i++) {
            const AudioConfig& audioConfig = config.audioSources[i];
            if (!audioConfig.enabled) {
                DLL_Log("MediaEngine::ReloadConfig audio source %zu disabled", i);
                continue;
            }

            DLL_Log("MediaEngine::ReloadConfig setting up audio source %zu (codec=%s)", i, audioConfig.codec.c_str());

            // Get the list of tracks this source should output to
            std::vector<int> targetTracks = audioConfig.tracks;
            if (targetTracks.empty()) {
                targetTracks.push_back((int)(i + 1));
            }

            DLL_Log("MediaEngine::ReloadConfig Audio source %zu targets %zu tracks", i, targetTracks.size());

            // For each target track, create or reuse an encoder
            for (int track : targetTracks) {
                // Defense in depth: never create a second app-audio capture for the
                // same process on the same track. Summing identical captures combs.
                if (audioConfig.sourceType == AudioConfig::AppAudio) {
                    const std::string appKey = AppAudioTrackKey(audioConfig, track);
                    if (!seenAppAudioTrackKeys.insert(appKey).second) {
                        DLL_Log(
                            "MediaEngine::ReloadConfig WARNING: duplicate app-audio source (process='%s' "
                            "processId=%lu) already targets track %d - skipping duplicate capture to avoid "
                            "comb-filter artifacts",
                            audioConfig.processName.empty() ? "<pid>" : audioConfig.processName.c_str(),
                            (unsigned long)audioConfig.processId, track);
                        continue;
                    }
                }
                TrackAudioFormat trackFormat = GetTrackAudioFormat(track);
                AudioConfig resolvedAudioConfig = audioConfig;
                resolvedAudioConfig.outputChannels = audioConfig.downmix ? 2 : trackFormat.channels;
                resolvedAudioConfig.outputChannelMask =
                    audioConfig.downmix ? DefaultChannelMaskForChannels(2) : trackFormat.channelMask;
                AudioEncoder* encoderForTrack = nullptr;
                auto it = trackToEncoder.find(track);
                if (it != trackToEncoder.end()) {
                    encoderForTrack = it->second;
                    DLL_Log(
                        "MediaEngine::ReloadConfig Audio source %zu reusing encoder "
                        "for track %d",
                        i, track);
                } else {
                    // Create new encoder for this track
                    auto newEncoder = std::make_unique<AudioEncoder>();
                    bool aRes =
                        newEncoder->Init(resolvedAudioConfig, [this](AVPacket* pkt) { this->WritePacket(pkt); });

                    if (!aRes) {
                        DLL_Log("MediaEngine::ReloadConfig Audio encoder for track %d failed", track);
                        continue;
                    }

                    videoEnc->AddAudioContext(resolvedAudioConfig, newEncoder->GetCodecContext(), track);

                    encoderForTrack = newEncoder.get();
                    trackToEncoder[track] = encoderForTrack;

                    AudioSource source;
                    source.config = resolvedAudioConfig;
                    source.track = track;
                    source.configuredSourceIndex = i;
                    source.sourceType = audioConfig.sourceType;
                    source.mixChannels =
                        resolvedAudioConfig.outputChannels > 0 ? resolvedAudioConfig.outputChannels : 2;
                    source.mixChannelMask = resolvedAudioConfig.outputChannelMask != 0
                                                ? resolvedAudioConfig.outputChannelMask
                                                : DefaultChannelMaskForChannels(source.mixChannels);
                    source.encoder = std::move(newEncoder);
                    source.sharedEncoderPtr = source.encoder.get();

                    // Create appropriate capture type
                    if (audioConfig.sourceType == AudioConfig::AppAudio) {
                        source.appCapture = std::make_unique<ProcessLoopbackCapture>();
                        source.appCapture->SetRequestedFormat(48000, source.mixChannels, source.mixChannelMask);
                    } else {
                        source.capture = std::make_unique<AudioCapture>();
                    }

                    // INIT RING BUFFER AND SYNC RESAMPLER (Per-source drift compensation)
                    InitAudioSourceBuffers(source, audioConfig, i);

                    DLL_Log(
                        "MediaEngine::ReloadConfig Created new encoder for track %d "
                        "(source %zu, type=%d)",
                        track, i, (int)audioConfig.sourceType);
                    audioSources.push_back(std::move(source));
                }

                if (it != trackToEncoder.end()) {
                    AudioSource source;
                    source.config = resolvedAudioConfig;
                    source.track = track;
                    source.configuredSourceIndex = i;
                    source.sourceType = audioConfig.sourceType;
                    source.mixChannels =
                        resolvedAudioConfig.outputChannels > 0 ? resolvedAudioConfig.outputChannels : 2;
                    source.mixChannelMask = resolvedAudioConfig.outputChannelMask != 0
                                                ? resolvedAudioConfig.outputChannelMask
                                                : DefaultChannelMaskForChannels(source.mixChannels);
                    source.encoder = nullptr;
                    source.sharedEncoderPtr = encoderForTrack;

                    // Create appropriate capture type
                    if (audioConfig.sourceType == AudioConfig::AppAudio) {
                        source.appCapture = std::make_unique<ProcessLoopbackCapture>();
                        source.appCapture->SetRequestedFormat(48000, source.mixChannels, source.mixChannelMask);
                    } else {
                        source.capture = std::make_unique<AudioCapture>();
                    }

                    // INIT RING BUFFER AND SYNC RESAMPLER (Per-source drift compensation)
                    InitAudioSourceBuffers(source, audioConfig, i);

                    DLL_Log(
                        "MediaEngine::ReloadConfig Audio source %zu shares encoder "
                        "for track %d (type=%d)",
                        i, track, (int)audioConfig.sourceType);
                    audioSources.push_back(std::move(source));
                }
            }
        }

        CoalesceCaptureRoutes();

        DLL_Log(
            "MediaEngine: ReloadConfig complete. Audio sources: %zu, unique "
            "tracks: %zu",
            audioSources.size(), trackToEncoder.size());
    }

private:
    // Identity of an app-audio capture targeting a specific track. Two app-audio
    // sources that resolve to the same process AND feed the same track would
    // capture the same audio twice; summing those near-identical (independently
    // buffered, phase-offset) streams into one track produces comb-filter
    // "metallic" artifacts. We use this key to detect and skip such duplicates.
    static std::string AppAudioTrackKey(const AudioConfig& cfg, int track) {
        return ce::audio::AppAudioTrackIdentity(cfg.processName, static_cast<unsigned long>(cfg.processId), track);
    }

    // Collapse the route objects created for a multi-track source onto one physical
    // capture object. Route-local resamplers/rings remain independent because each
    // track owns a timeline and mix format, but their input packets are fanned out
    // by AudioLoop from this single owner. Process loopback requests the widest
    // routed layout once, then each route's resampler converts that common packet
    // stream to its track layout. Endpoint capture remains native.
    void CoalesceCaptureRoutes() {
        using CaptureRouteKey = std::tuple<int, std::string>;
        std::map<CaptureRouteKey, size_t> owners;
        std::map<CaptureRouteKey, std::pair<int, uint32_t>> appCaptureFormats;
        size_t physicalCaptureCount = 0;
        size_t sharedRouteCount = 0;

        auto captureKey = [](const AudioSource& src) -> CaptureRouteKey {
            std::string physicalIdentity;
            if (src.sourceType == AudioConfig::AppAudio) {
                physicalIdentity = ce::audio::AppAudioTrackIdentity(
                    src.config.processName, static_cast<unsigned long>(src.config.processId), 0);
            } else {
                physicalIdentity = src.config.device.empty() ? "<default>" : src.config.device;
                std::transform(physicalIdentity.begin(), physicalIdentity.end(), physicalIdentity.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            }
            return {static_cast<int>(src.sourceType), std::move(physicalIdentity)};
        };

        for (const auto& src : audioSources) {
            if (src.sourceType != AudioConfig::AppAudio) {
                continue;
            }
            const CaptureRouteKey key = captureKey(src);
            auto& format = appCaptureFormats[key];
            if (src.mixChannels > format.first) {
                format = {src.mixChannels, src.mixChannelMask};
            }
        }

        for (size_t idx = 0; idx < audioSources.size(); ++idx) {
            auto& src = audioSources[idx];
            const CaptureRouteKey key = captureKey(src);
            const auto [it, inserted] = owners.emplace(key, idx);
            if (inserted) {
                src.captureFanoutOwnerIndex = idx;
                if (src.appCapture) {
                    const auto format = appCaptureFormats[key];
                    src.appCapture->SetRequestedFormat(48000, format.first, format.second);
                    DLL_Log("[AudioRoute] App capture owner=%zu configuredSource=%zu requests shared format=%dch/0x%x",
                            idx, src.configuredSourceIndex, format.first, format.second);
                }
                ++physicalCaptureCount;
                continue;
            }

            const size_t ownerIdx = it->second;
            src.captureFanoutOwnerIndex = ownerIdx;
            src.capture.reset();
            src.appCapture.reset();
            ++sharedRouteCount;
            DLL_Log(
                "[AudioRoute] Source route src=%zu track=%d shares physical capture owner=%zu track=%d "
                "configuredSource=%zu type=%d format=%dch/0x%x",
                idx, src.track, ownerIdx, audioSources[ownerIdx].track, src.configuredSourceIndex,
                static_cast<int>(src.sourceType), src.mixChannels, src.mixChannelMask);
        }

        DLL_Log("[AudioRoute] Capture topology: physical=%zu routedFollowers=%zu totalRoutes=%zu", physicalCaptureCount,
                sharedRouteCount, audioSources.size());
    }

    ProcessLoopbackCapture* GetAppCaptureForRoute(size_t srcIdx) {
        if (srcIdx >= audioSources.size()) {
            return nullptr;
        }
        const size_t ownerIdx = audioSources[srcIdx].captureFanoutOwnerIndex;
        if (ownerIdx >= audioSources.size()) {
            return nullptr;
        }
        return audioSources[ownerIdx].appCapture.get();
    }

    std::pair<int64_t, int64_t> GetCaptureGroupBufferedSampleRange(size_t srcIdx) const {
        if (srcIdx >= audioSources.size()) {
            return {0, 0};
        }
        const size_t ownerIdx = audioSources[srcIdx].captureFanoutOwnerIndex;
        int64_t minimum = std::numeric_limits<int64_t>::max();
        int64_t maximum = 0;
        for (const auto& route : audioSources) {
            if (route.captureFanoutOwnerIndex != ownerIdx || !route.ringBuffer) {
                continue;
            }
            const size_t channels = static_cast<size_t>(std::max(1, route.mixChannels));
            const int64_t bufferedSamples = static_cast<int64_t>(route.ringBuffer->GetAvailable() / channels);
            minimum = std::min(minimum, bufferedSamples);
            maximum = std::max(maximum, bufferedSamples);
        }
        return {minimum == std::numeric_limits<int64_t>::max() ? 0 : minimum, maximum};
    }

    // Shared initialization for ring buffer and sync resampler on an AudioSource.
    // Parses sample rate from config (defaults to 48000) and sets up both.
    void InitAudioSourceBuffers(AudioSource& source, const AudioConfig& audioConfig, size_t sourceIdx) {
        constexpr int kMixerSampleRate = 48000;
        constexpr size_t kDefaultAudioRingBufferSeconds = 8;
        // Heavy CFR overload runs can fall tens of seconds behind real time even
        // while audio/video file durations still stay mathematically equal. Give CFR
        // enough retention headroom to avoid destructive oldest-audio trims in those
        // runs so we preserve pitch and avoid crackle while diagnostics report the
        // underlying encoder shortfall honestly.
        constexpr size_t kCfrAudioRingBufferSeconds = 30;
        const bool isCfrPath = ce::audio::ShouldUseCfrAudioContinuityPolicy(config.video.useVFR);
        const size_t ringBufferSeconds = isCfrPath ? kCfrAudioRingBufferSeconds : kDefaultAudioRingBufferSeconds;
        const int channels = std::clamp(source.mixChannels, 1, 8);
        const size_t capacity = static_cast<size_t>(kMixerSampleRate) * ringBufferSeconds * channels;
        source.fullRingBufferCapacityFloats = capacity;

        // App-audio sources commonly target candidate processes that may not be
        // running (a "capture whichever game is running" profile). Allocating the
        // full multi-second CFR retention buffer (~11.5 MB at 30s/48k/stereo) for
        // every such source wastes memory on sources that never capture. Start app
        // sources with a small buffer and grow in-place to full capacity on first
        // captured audio (see AudioLoop). System/mic sources are always active, so
        // they keep full capacity immediately.
        constexpr size_t kAppAudioInitialRingBufferSeconds = 1;
        const size_t initialCapacity = (source.appCapture != nullptr)
                                           ? std::min(capacity, static_cast<size_t>(kMixerSampleRate) *
                                                                    kAppAudioInitialRingBufferSeconds * channels)
                                           : capacity;
        source.ringBuffer = std::make_unique<AudioRingBuffer>(initialCapacity);
        DLL_Log(
            "MediaEngine::Init RingBuffer created for source %zu. Cap=%zu floats (initial, full=%zu floats/%zus), "
            "rate=%d channels=%d mask=0x%x deferred=%d",
            sourceIdx, initialCapacity, capacity, ringBufferSeconds, kMixerSampleRate, channels, source.mixChannelMask,
            initialCapacity < capacity ? 1 : 0);

        source.syncResampler = std::make_unique<AudioResampler>();
        AudioResampler::InputFormat syncInFmt;
        syncInFmt.sampleRate = kMixerSampleRate;
        syncInFmt.channels = channels;
        syncInFmt.bitsPerSample = 32;
        syncInFmt.validBitsPerSample = 32;
        syncInFmt.isFloat = true;
        syncInFmt.blockAlign = channels * 4;
        syncInFmt.channelMask = source.mixChannelMask;
        AudioResampler::OutputFormat syncOutFmt;
        syncOutFmt.sampleRate = kMixerSampleRate;
        syncOutFmt.channels = channels;
        syncOutFmt.sampleFmt = AV_SAMPLE_FMT_FLT;
        syncOutFmt.channelMask = source.mixChannelMask;
        source.syncResampler->Init(syncInFmt, syncOutFmt);
        DLL_Log("MediaEngine::Init SyncResampler created for source %zu (rate=%d channels=%d)", sourceIdx,
                kMixerSampleRate, channels);
    }

    void AudioLoop() {
        DLL_Log("MediaEngine: Audio thread started (sources=%d)", (int)audioSources.size());
        int packetCount = 0;
        int mixCount = 0;

        // Check if any track has multiple sources (requires mixing)
        std::map<int, int> trackSourceCount;
        std::map<int, std::vector<size_t>> trackSourceIndices;  // track -> source indices
        for (size_t i = 0; i < audioSources.size(); i++) {
            auto& src = audioSources[i];
            trackSourceCount[src.track]++;
            trackSourceIndices[src.track].push_back(i);
        }

        // Per-track source summary (process names) and a runtime guard that surfaces
        // any duplicate app-audio capture feeding one track (identical streams comb).
        for (auto& kv : trackSourceIndices) {
            std::string summary;
            std::set<std::string> appIdentities;
            for (size_t idx : kv.second) {
                auto& s = audioSources[idx];
                std::string label;
                if (s.sourceType == AudioConfig::AppAudio) {
                    const char* pn = s.config.processName.empty() ? "<pid>" : s.config.processName.c_str();
                    label = std::string("app:") + pn;
                    if (!appIdentities.insert(AppAudioTrackKey(s.config, kv.first)).second) {
                        DLL_Log(
                            "AudioLoop: WARNING - track %d has duplicate app-audio capture for '%s' - identical "
                            "streams will comb-filter when mixed",
                            kv.first, pn);
                    }
                } else if (s.sourceType == AudioConfig::Microphone) {
                    label = "mic";
                } else {
                    label = "system";
                }
                if (!summary.empty())
                    summary += ", ";
                summary += label;
            }
            DLL_Log("AudioLoop: Track %d sources: [%s]", kv.first, summary.c_str());
        }

        for (auto& kv : trackSourceCount) {
            if (kv.second > 1) {
                DLL_Log("AudioLoop: Track %d has %d sources - REAL mixing enabled", kv.first, kv.second);
            }
        }
        constexpr int64_t kStartupFirstPacketGapCapSamples = 480;
        constexpr int64_t kStartupFirstPacketRebaseThresholdSamples = 2400;

        std::vector<int64_t> sourceTimestamps(audioSources.size(), 0);
        std::vector<bool> sourceLoggedPreStartDrop(audioSources.size(), false);
        std::map<int, int64_t> trackNextTimestamp;  // Track continuous timestamps for mixing
        std::vector<AudioPacket> sourceLastPackets(audioSources.size());
        std::vector<std::chrono::steady_clock::time_point> lastPacketTime(audioSources.size(),
                                                                          std::chrono::steady_clock::now());
        std::vector<AudioPacket> deferredFirstTimelinePackets(audioSources.size());
        std::vector<bool> deferredFirstTimelinePacketValid(audioSources.size(), false);
        std::vector<int64_t> deferredFirstTimelinePacketStartSamples(audioSources.size(), 0);
        std::vector<uint64_t> sourceCaptureEpochs(audioSources.size(), 0);
        std::vector<std::deque<AudioPacket>> pendingEpochPackets(audioSources.size());
        std::vector<std::deque<AudioPacket>> captureFanoutQueues(audioSources.size());
        std::vector<uint64_t> captureFanoutPacketCounts(audioSources.size(), 0);
        std::vector<uint64_t> batchedPreStartDiscardCounts(audioSources.size(), 0);
        int64_t sharedStartupRebaseOffsetSamples = -1;
        uint64_t appliedAudioResetGeneration =
            audioResetAcknowledgedGeneration.load(std::memory_order_acquire);
        bool audioOnlyStopTailFinalized = false;

        // Per-source A/V equalization: delay each audio source so every source sits at the same
        // (max) capture latency, matching the video content delay. Faster sources (e.g. a
        // near-zero-latency microphone vs a high-latency loopback endpoint) get leading silence
        // so they align with the delayed video AND with each other on mixed tracks. The slowest
        // source(s) get delay 0, so the common all-equal-latency case is unchanged (no regression).
        float maxAudioCaptureLatencyMs = 0.0f;
        for (const auto& eqSrc : audioSources) {
            if (eqSrc.config.captureLatencyMs > maxAudioCaptureLatencyMs) {
                maxAudioCaptureLatencyMs = eqSrc.config.captureLatencyMs;
            }
        }
        DLL_Log("[AVSyncAuto] audio_equalization: sources=%zu maxCaptureLatencyMs=%.3f confidence=%s reason=%s",
                audioSources.size(), static_cast<double>(maxAudioCaptureLatencyMs), config.avSyncConfidence.c_str(),
                config.avSyncReason.c_str());
        std::vector<int64_t> audioEqualizationDelaySamples(audioSources.size(), 0);
        for (size_t i = 0; i < audioSources.size(); ++i) {
            const double deltaMs = static_cast<double>(maxAudioCaptureLatencyMs) -
                                   static_cast<double>(audioSources[i].config.captureLatencyMs);
            audioEqualizationDelaySamples[i] =
                deltaMs > 0.0 ? static_cast<int64_t>(std::llround(deltaMs / 1000.0 * 48000.0)) : 0;
            if (audioEqualizationDelaySamples[i] > 0) {
                DLL_Log(
                    "[AudioLoop] A/V equalization: src=%zu captureLatencyMs=%.3f delaySamples=%lld (%.1f ms) to "
                    "match maxLatencyMs=%.3f",
                    i, static_cast<double>(audioSources[i].config.captureLatencyMs),
                    (long long)audioEqualizationDelaySamples[i],
                    static_cast<double>(audioEqualizationDelaySamples[i]) * 1000.0 / 48000.0,
                    static_cast<double>(maxAudioCaptureLatencyMs));
            }
        }

        auto sourceParticipatesInSharedStartupRebase = [this](size_t srcIdx) -> bool {
            if (srcIdx >= audioSources.size()) {
                return false;
            }
            const auto& src = audioSources[srcIdx];
            const bool hasPhysicalOrRoutedCapture =
                src.capture || src.appCapture || src.captureFanoutOwnerIndex < audioSources.size();
            if (!src.sawSyncPendingPackets || !src.sharedEncoderPtr || !hasPhysicalOrRoutedCapture) {
                return false;
            }
            return src.sourceType != AudioConfig::Microphone;
        };

        auto trySelectSharedStartupRebase = [&](bool finalStopDrain = false) -> bool {
            if (sharedStartupRebaseOffsetSamples >= 0) {
                return true;
            }

            size_t participants = 0;
            size_t readyParticipants = 0;
            int64_t earliestPacketStartSamples = std::numeric_limits<int64_t>::max();
            for (size_t i = 0; i < audioSources.size(); ++i) {
                if (!sourceParticipatesInSharedStartupRebase(i)) {
                    continue;
                }
                ++participants;
                if (!deferredFirstTimelinePacketValid[i] && sourceTimestamps[i] == 0) {
                    continue;
                }
                ++readyParticipants;
                if (deferredFirstTimelinePacketValid[i]) {
                    earliestPacketStartSamples =
                        std::min<int64_t>(earliestPacketStartSamples, deferredFirstTimelinePacketStartSamples[i]);
                }
            }

            if (participants == 0) {
                sharedStartupRebaseOffsetSamples = 0;
                return true;
            }
            if (readyParticipants < participants && !finalStopDrain) {
                return false;
            }
            if (readyParticipants < participants) {
                DLL_Log("[AudioLoop] Final stop bypassing absent shared-startup participants: ready=%zu total=%zu",
                        readyParticipants, participants);
            }
            if (earliestPacketStartSamples == std::numeric_limits<int64_t>::max()) {
                sharedStartupRebaseOffsetSamples = 0;
                DLL_Log("[AudioLoop] Shared startup rebase disabled: participants=%zu had no deferred QPC baseline",
                        participants);
                return true;
            }

            sharedStartupRebaseOffsetSamples = ce::audio::ComputeSharedStartupFirstPacketRebaseOffset(
                earliestPacketStartSamples, kStartupFirstPacketGapCapSamples,
                kStartupFirstPacketRebaseThresholdSamples);
            DLL_Log(
                "[AudioLoop] Shared startup rebase selected offset=%lld samples earliest=%lld cap=%lld "
                "participants=%zu",
                (long long)sharedStartupRebaseOffsetSamples, (long long)earliestPacketStartSamples,
                (long long)kStartupFirstPacketGapCapSamples, participants);
            return true;
        };

        while (audioRunning) {
            const uint64_t requestedResetGeneration =
                audioResetRequestedGeneration.load(std::memory_order_acquire);
            if (requestedResetGeneration > appliedAudioResetGeneration) {
                const bool preserveResetPackets = audioResetPreservePackets.load(std::memory_order_acquire);
                ApplyAudioTimelineReset(requestedResetGeneration,
                                        recordingStartSystemQPCMs.load(std::memory_order_acquire),
                                        preserveResetPackets);
                std::fill(sourceTimestamps.begin(), sourceTimestamps.end(), 0);
                std::fill(sourceLoggedPreStartDrop.begin(), sourceLoggedPreStartDrop.end(), false);
                std::fill(deferredFirstTimelinePacketValid.begin(), deferredFirstTimelinePacketValid.end(), false);
                std::fill(deferredFirstTimelinePacketStartSamples.begin(),
                          deferredFirstTimelinePacketStartSamples.end(), 0);
                std::fill(sourceCaptureEpochs.begin(), sourceCaptureEpochs.end(), 0);
                std::fill(sourceLastPackets.begin(), sourceLastPackets.end(), AudioPacket{});
                std::fill(lastPacketTime.begin(), lastPacketTime.end(), std::chrono::steady_clock::now());
                for (auto& queue : pendingEpochPackets) {
                    queue.clear();
                }
                if (!preserveResetPackets) {
                    for (auto& queue : captureFanoutQueues) {
                        queue.clear();
                    }
                }
                trackNextTimestamp.clear();
                sharedStartupRebaseOffsetSamples = -1;
                audioOnlyStopTailFinalized = false;
                appliedAudioResetGeneration = requestedResetGeneration;
                audioResetAcknowledgedGeneration.store(requestedResetGeneration, std::memory_order_release);
                audioDrainCv.notify_all();
            }

            if (requestedResetGeneration >
                    audioResetCommittedGeneration.load(std::memory_order_acquire) &&
                !audioStopDrainRequested.load(std::memory_order_acquire)) {
                std::unique_lock<std::mutex> lock(audioDrainMutex);
                audioDrainCv.wait(lock, [this, requestedResetGeneration]() {
                    return !audioRunning.load(std::memory_order_acquire) ||
                           audioResetCommittedGeneration.load(std::memory_order_acquire) >=
                               requestedResetGeneration ||
                           audioStopDrainRequested.load(std::memory_order_acquire);
                });
                continue;
            }

            if (audioSyncPending.load(std::memory_order_acquire) &&
                preservePendingStartupAudioPackets.load(std::memory_order_acquire) &&
                !audioStopDrainRequested.load(std::memory_order_acquire)) {
                std::unique_lock<std::mutex> lock(audioDrainMutex);
                audioDrainCv.wait_for(lock, std::chrono::milliseconds(5), [this]() {
                    return !audioRunning.load(std::memory_order_acquire) ||
                           !audioSyncPending.load(std::memory_order_acquire) ||
                           audioStopDrainRequested.load(std::memory_order_acquire);
                });
                continue;
            }

            bool gotAnyPacket = false;
            auto now = std::chrono::steady_clock::now();

            // Step 1: Poll all sources and accumulate samples into buffers
            for (size_t srcIdx = 0; srcIdx < audioSources.size(); srcIdx++) {
                auto& src = audioSources[srcIdx];
                if (!src.sharedEncoderPtr)
                    continue;

                // Skip if neither a physical capture nor a routed packet is available.
                if (!src.capture && !src.appCapture && captureFanoutQueues[srcIdx].empty())
                    continue;

                AudioPacket packet;
                bool gotPacket = false;
                bool gotDeferredFirstTimelinePacket = false;

                const uint64_t requestedEpochReset =
                    src.epochResetRequested->load(std::memory_order_acquire);
                const uint64_t acknowledgedEpochReset =
                    src.epochResetAcknowledged->load(std::memory_order_acquire);
                if (requestedEpochReset != 0 && requestedEpochReset != acknowledgedEpochReset) {
                    // The pull owner is still encoding the previous epoch's ring/sync/post tail.
                    // Do not poll new capture data into route state until it acknowledges the reset.
                    continue;
                }

                if (!pendingEpochPackets[srcIdx].empty()) {
                    packet = std::move(pendingEpochPackets[srcIdx].front());
                    pendingEpochPackets[srcIdx].pop_front();
                    gotPacket = true;
                    DLL_Log(
                        "[AudioEpoch] Consuming deferred epoch record after pull acknowledgement src=%zu track=%d "
                        "epoch=%llu remaining=%zu",
                        srcIdx, src.track, static_cast<unsigned long long>(packet.captureEpoch),
                        pendingEpochPackets[srcIdx].size());
                } else if (deferredFirstTimelinePacketValid[srcIdx]) {
                    const bool finalStopDrain = audioStopDrainRequested.load(std::memory_order_acquire);
                    if (!trySelectSharedStartupRebase(finalStopDrain)) {
                        gotAnyPacket = true;
                        continue;
                    }
                    packet = std::move(deferredFirstTimelinePackets[srcIdx]);
                    deferredFirstTimelinePacketValid[srcIdx] = false;
                    gotPacket = true;
                    gotDeferredFirstTimelinePacket = true;
                } else {
                    bool packetCameFromPhysicalCapture = false;
                    if (!captureFanoutQueues[srcIdx].empty()) {
                        packet = std::move(captureFanoutQueues[srcIdx].front());
                        captureFanoutQueues[srcIdx].pop_front();
                        gotPacket = true;
                    } else {
                        // Poll the one physical capture that owns this route group.
                        if (src.appCapture) {
                            gotPacket = src.appCapture->GetNextPacket(packet);
                            if (!gotPacket && src.appCapture->HasIntegrityFailure() &&
                                !processLoopbackIntegrityFailureSignaled) {
                                processLoopbackIntegrityFailureSignaled = true;
                                const uint32_t status = src.appCapture->GetTransportStatus();
                                DLL_Log(
                                    "[AudioLoop] FATAL: app-audio transport integrity failure src=%zu track=%d "
                                    "status=%u; requesting a failed recording stop",
                                    srcIdx, src.track, status);
                                if (sharedMemLayout) {
                                    sharedMemLayout->runtimeState.recordingFailureCode.store(
                                        static_cast<uint32_t>(
                                            RecordingFailureCode::ProcessLoopbackTransportIntegrity),
                                        std::memory_order_release);
                                    sharedMemLayout->runtimeState.cmdStopRecording.store(true,
                                                                                       std::memory_order_release);
                                }
                            }
                        } else if (src.capture) {
                            gotPacket = src.capture->GetNextPacket(packet);
                        }
                        packetCameFromPhysicalCapture = gotPacket;
                    }

                    // During WGC startup, process-loopback queues can contain over a
                    // second of packets preceding the shared video anchor. Removing
                    // only one stale packet per outer pass lets live audio remain
                    // hundreds of milliseconds behind. Drain a bounded batch before
                    // doing any resampling, while preserving evidence needed by the
                    // startup-rebase policy.
                    constexpr size_t kMaxPreStartDiscardBatchPackets = 256;
                    size_t batchedDiscards = 0;
                    int64_t startQPC = recordingStartSystemQPCMs.load(std::memory_order_acquire);
                    while (packetCameFromPhysicalCapture && gotPacket && !packet.data.empty() &&
                           sourceTimestamps[srcIdx] == 0 && startQPC != 0 && packet.timestamp < (startQPC - 5) &&
                           batchedDiscards < kMaxPreStartDiscardBatchPackets) {
                        const bool rememberPreStartPacket = ce::audio::ShouldRememberPreStartPacketForAppBootstrap(
                            src.sourceType == AudioConfig::AppAudio, true, packet.timestamp, startQPC);
                        for (size_t routeIdx = 0; routeIdx < audioSources.size(); ++routeIdx) {
                            if (audioSources[routeIdx].captureFanoutOwnerIndex == srcIdx && rememberPreStartPacket) {
                                audioSources[routeIdx].sawSyncPendingPackets = true;
                            }
                        }
                        ++batchedDiscards;
                        gotAnyPacket = true;
                        packet = {};
                        gotPacket = src.appCapture ? src.appCapture->GetNextPacket(packet)
                                                   : (src.capture ? src.capture->GetNextPacket(packet) : false);
                    }
                    if (batchedDiscards > 0) {
                        batchedPreStartDiscardCounts[srcIdx] += batchedDiscards;
                        if (!sourceLoggedPreStartDrop[srcIdx] || batchedDiscards == kMaxPreStartDiscardBatchPackets) {
                            DLL_Log(
                                "[AudioLoop] Batched pre-start discard owner=%zu packets=%zu total=%llu "
                                "start=%lld nextPacket=%lld fanoutRoutes=%zu%s",
                                srcIdx, batchedDiscards,
                                static_cast<unsigned long long>(batchedPreStartDiscardCounts[srcIdx]), startQPC,
                                gotPacket ? packet.timestamp : 0,
                                static_cast<size_t>(std::count_if(audioSources.begin(), audioSources.end(),
                                                                  [srcIdx](const AudioSource& route) {
                                                                      return route.captureFanoutOwnerIndex == srcIdx;
                                                                  })),
                                batchedDiscards == kMaxPreStartDiscardBatchPackets ? " batch_limit" : "");
                            sourceLoggedPreStartDrop[srcIdx] = true;
                        }
                    }

                    const bool packetStillPreStart = gotPacket && !packet.data.empty() &&
                                                     sourceTimestamps[srcIdx] == 0 && startQPC != 0 &&
                                                     packet.timestamp < (startQPC - 5);
                    if (packetCameFromPhysicalCapture && gotPacket && !packetStillPreStart &&
                        src.captureFanoutOwnerIndex == srcIdx) {
                        size_t followerCount = 0;
                        for (size_t routeIdx = srcIdx + 1; routeIdx < audioSources.size(); ++routeIdx) {
                            if (audioSources[routeIdx].captureFanoutOwnerIndex != srcIdx) {
                                continue;
                            }
                            captureFanoutQueues[routeIdx].push_back(packet);
                            ++followerCount;
                        }
                        if (followerCount > 0) {
                            ++captureFanoutPacketCounts[srcIdx];
                            if (captureFanoutPacketCounts[srcIdx] == 1 ||
                                captureFanoutPacketCounts[srcIdx] % 1000 == 0) {
                                DLL_Log(
                                    "[AudioRoute] Fanned packet owner=%zu routes=%zu packet=%llu "
                                    "pendingFollowerMax=%zu qpc=%llu",
                                    srcIdx, followerCount + 1,
                                    static_cast<unsigned long long>(captureFanoutPacketCounts[srcIdx]),
                                    [&]() {
                                        size_t maximum = 0;
                                        for (size_t routeIdx = 0; routeIdx < captureFanoutQueues.size(); ++routeIdx) {
                                            if (audioSources[routeIdx].captureFanoutOwnerIndex == srcIdx) {
                                                maximum = std::max(maximum, captureFanoutQueues[routeIdx].size());
                                            }
                                        }
                                        return maximum;
                                    }(),
                                    static_cast<unsigned long long>(packet.qpcPosition));
                            }
                        }
                    }
                }

                if (gotPacket && packet.recordType == AudioPacketRecordType::EpochStart) {
                    src.sawCaptureEpoch = true;
                    const uint64_t previousCaptureEpoch = sourceCaptureEpochs[srcIdx];
                    if (packet.captureEpoch != 0 && previousCaptureEpoch == 0) {
                        sourceCaptureEpochs[srcIdx] = packet.captureEpoch;
                    }
                    if (ce::audio::IsAudioCaptureEpochTransition(previousCaptureEpoch, packet.captureEpoch)) {
                        if (!FlushCaptureResamplerForEpoch(src, srcIdx, previousCaptureEpoch, packet.captureEpoch)) {
                            pendingEpochPackets[srcIdx].push_front(std::move(packet));
                            continue;
                        }
                        src.epochResetRequested->store(packet.captureEpoch, std::memory_order_release);
                        audioDrainCv.notify_all();
                        gotAnyPacket = true;
                        DLL_Log(
                            "[AudioEpoch] Ordered epoch start requested pull reset src=%zu track=%d type=%d "
                            "epoch=%llu->%llu ringBuffered=%zu",
                            srcIdx, src.track, static_cast<int>(src.sourceType),
                            static_cast<unsigned long long>(previousCaptureEpoch),
                            static_cast<unsigned long long>(packet.captureEpoch),
                            src.ringBuffer ? src.ringBuffer->GetAvailable() /
                                                 static_cast<size_t>(std::clamp(src.mixChannels, 1, 8))
                                           : 0);
                        continue;
                    }
                    gotAnyPacket = true;
                    DLL_Log(
                        "[AudioRoute] Ordered epoch start reached src=%zu track=%d type=%d epoch=%llu previous=%llu "
                        "ringBuffered=%zu",
                        srcIdx, src.track, static_cast<int>(src.sourceType),
                        static_cast<unsigned long long>(packet.captureEpoch),
                        static_cast<unsigned long long>(previousCaptureEpoch),
                        src.ringBuffer ? src.ringBuffer->GetAvailable() /
                                             static_cast<size_t>(std::clamp(src.mixChannels, 1, 8))
                                       : 0);
                    continue;
                }

                if (gotPacket &&
                    (packet.recordType == AudioPacketRecordType::EndOfStream || packet.endOfStream)) {
                    if (src.appCaptureRouteEnded) {
                        src.appCaptureRouteEnded->store(true, std::memory_order_release);
                    }
                    src.timelineValid = true;
                    gotAnyPacket = true;
                    DLL_Log(
                        "[AudioRoute] Ordered app capture end reached src=%zu track=%d process=%s epoch=%llu "
                        "buffered=%zu; future source gaps are timeline silence until a new epoch live-joins",
                        srcIdx, src.track,
                        src.config.processName.empty() ? "<none>" : src.config.processName.c_str(),
                        static_cast<unsigned long long>(packet.captureEpoch),
                        src.ringBuffer ? src.ringBuffer->GetAvailable() /
                                             static_cast<size_t>(std::clamp(src.mixChannels, 1, 8))
                                       : 0);
                    continue;
                }

                if (gotPacket && !packet.data.empty()) {
                    const bool resumedAfterEnd =
                        src.appCaptureRouteEnded && src.appCaptureRouteEnded->load(std::memory_order_acquire);
                    const uint64_t previousCaptureEpoch = sourceCaptureEpochs[srcIdx];
                    const bool captureEpochTransition =
                        ce::audio::IsAudioCaptureEpochTransition(previousCaptureEpoch, packet.captureEpoch);
                    if (captureEpochTransition) {
                        const uint64_t requested = src.epochResetRequested->load(std::memory_order_acquire);
                        const uint64_t acknowledged = src.epochResetAcknowledged->load(std::memory_order_acquire);
                        if (requested != packet.captureEpoch) {
                            if (!FlushCaptureResamplerForEpoch(src, srcIdx, previousCaptureEpoch,
                                                               packet.captureEpoch)) {
                                pendingEpochPackets[srcIdx].push_front(std::move(packet));
                                continue;
                            }
                            src.epochResetRequested->store(packet.captureEpoch, std::memory_order_release);
                            audioDrainCv.notify_all();
                            pendingEpochPackets[srcIdx].push_back(std::move(packet));
                            gotAnyPacket = true;
                            DLL_Log(
                                "[AudioEpoch] Data arrived without a preceding transition marker; requested "
                                "pull reset and deferred data src=%zu track=%d type=%d epoch=%llu->%llu",
                                srcIdx, src.track, static_cast<int>(src.sourceType),
                                static_cast<unsigned long long>(previousCaptureEpoch),
                                static_cast<unsigned long long>(packet.captureEpoch));
                            continue;
                        }
                        if (acknowledged != packet.captureEpoch) {
                            pendingEpochPackets[srcIdx].push_back(std::move(packet));
                            gotAnyPacket = true;
                            continue;
                        }
                        // A name-monitored app may exit and return under a new PID many minutes later;
                        // fatal/silent-stall recovery also creates a fresh process-loopback stream. The
                        // new stream's first QPC is valid in the recording domain, but it is not contiguous
                        // with this route's old source-local write cursor. Treat it as a fresh late source
                        // so the existing live-join policy overlaps already-encoded silence instead of
                        // materializing the absence as a multi-minute ring-buffer gap.
                        src.resampler.reset();
                        sourceTimestamps[srcIdx] = 0;
                        sourceLastPackets[srcIdx] = {};
                        deferredFirstTimelinePacketValid[srcIdx] = false;
                        src.timelineValid = false;
                        src.isPrimed = false;
                        src.bootstrapComplete = false;
                        src.sawSyncPendingPackets = false;
                        src.startupRealAudioSeen = false;
                        src.startupSyntheticRingSamples = 0;
                        src.startupGapProtectionSamples = 0;
                        src.packetBoundaryFadeInSamplesRemaining = 0;
                        DLL_Log(
                            "[AudioEpoch] Capture owner accepted acknowledged transition src=%zu track=%d type=%d "
                            "process=%s epoch=%llu->%llu requested=%llu acknowledged=%llu trackCursor=%lld "
                            "sourceCursor=%llu; capture state reset for live rejoin",
                            srcIdx, src.track, static_cast<int>(src.sourceType),
                            src.config.processName.empty() ? "<none>" : src.config.processName.c_str(),
                            static_cast<unsigned long long>(previousCaptureEpoch),
                            static_cast<unsigned long long>(packet.captureEpoch),
                            static_cast<unsigned long long>(requested),
                            static_cast<unsigned long long>(acknowledged),
                            static_cast<long long>(trackTimelineSamples[src.track]),
                            static_cast<unsigned long long>(src.qpcAlignedWrittenSamples));
                    }
                    if (packet.captureEpoch != 0) {
                        sourceCaptureEpochs[srcIdx] = packet.captureEpoch;
                    }
                    if (resumedAfterEnd) {
                        src.appCaptureRouteEnded->store(false, std::memory_order_release);
                        DLL_Log(
                            "[AudioRoute] App capture resumed after ordered end src=%zu track=%d process=%s "
                            "epoch=%llu; old tail preserved before live rejoin",
                            srcIdx, src.track,
                            src.config.processName.empty() ? "<none>" : src.config.processName.c_str(),
                            static_cast<unsigned long long>(packet.captureEpoch));
                    }
                    // First captured audio for this source: grow its ring buffer to
                    // full CFR capacity (deferred for app-audio sources whose target
                    // process may never run). The buffer is still empty at this point,
                    // so the in-place grow is lossless, and AudioLoop is the only
                    // thread touching this buffer, so it is race-free.
                    if (src.ringBuffer && src.ringBuffer->GetCapacity() < src.fullRingBufferCapacityFloats) {
                        if (src.ringBuffer->EnsureCapacity(src.fullRingBufferCapacityFloats)) {
                            DLL_Log(
                                "[AudioLoop] Grew ring buffer for src %d to full capacity %zu floats on first capture",
                                (int)srcIdx, src.fullRingBufferCapacityFloats);
                        }
                    }
                    int64_t startQPC = recordingStartSystemQPCMs.load(std::memory_order_acquire);
                    if (startQPC != 0 && sourceTimestamps[srcIdx] == 0 && packet.timestamp < (startQPC - 5)) {
                        const bool rememberPreStartPacket = ce::audio::ShouldRememberPreStartPacketForAppBootstrap(
                            src.sourceType == AudioConfig::AppAudio, sourceTimestamps[srcIdx] == 0, packet.timestamp,
                            startQPC);
                        if (rememberPreStartPacket) {
                            src.sawSyncPendingPackets = true;
                        }
                        if (!sourceLoggedPreStartDrop[srcIdx]) {
                            DLL_Log(
                                "[AudioLoop] Discarding pre-start packet src=%d packet=%lld start=%lld "
                                "appStartupEvidence=%d",
                                (int)srcIdx, packet.timestamp, startQPC, rememberPreStartPacket ? 1 : 0);
                            sourceLoggedPreStartDrop[srcIdx] = true;
                        }
                        continue;
                    }

                    const int64_t startQpc100nsForFirstPacket =
                        recordingStartSystemQpc100ns.load(std::memory_order_acquire);
                    if (!gotDeferredFirstTimelinePacket && sourceTimestamps[srcIdx] == 0 &&
                        sourceParticipatesInSharedStartupRebase(srcIdx) && sharedStartupRebaseOffsetSamples < 0 &&
                        startQpc100nsForFirstPacket > 0 &&
                        packet.qpcPosition >= static_cast<uint64_t>(startQpc100nsForFirstPacket)) {
                        const uint64_t packetStartDelta100ns =
                            packet.qpcPosition - static_cast<uint64_t>(startQpc100nsForFirstPacket);
                        deferredFirstTimelinePacketStartSamples[srcIdx] =
                            static_cast<int64_t>(ce::audio::HundredNanosecondsToSamples(packetStartDelta100ns, 48000)) +
                            audioEqualizationDelaySamples[srcIdx];
                        deferredFirstTimelinePackets[srcIdx] = std::move(packet);
                        deferredFirstTimelinePacketValid[srcIdx] = true;
                        gotAnyPacket = true;
                        trySelectSharedStartupRebase(audioStopDrainRequested.load(std::memory_order_acquire));
                        continue;
                    }

                    gotAnyPacket = true;
                    packetCount++;
                    sourceLastPackets[srcIdx] = packet;
                    lastPacketTime[srcIdx] = now;

                    if (packetCount <= 3 || packetCount % 1000 == 0) {
                        DLL_Log("AudioLoop: Packet #%d src=%d track=%d, size=%d", packetCount, (int)srcIdx, src.track,
                                (int)packet.data.size());
                    }

                    // Standardize each source to the resolved track layout at 48kHz
                    // float. This preserves multichannel tracks while still giving
                    // the mixer a single format per track.
                    if (!src.resampler) {
                        src.resampler = std::make_unique<AudioResampler>();
                    }

                    // Define target format for mixing (48kHz, Stereo, Float)
                    AudioResampler::OutputFormat targetFmt;
                    targetFmt.sampleRate = 48000;
                    targetFmt.channels = std::clamp(src.mixChannels, 1, 8);
                    targetFmt.sampleFmt = AV_SAMPLE_FMT_FLT;  // Packed float (interleaved)
                    targetFmt.channelMask = src.mixChannelMask;

                    // Define input format from packet
                    AudioResampler::InputFormat inputFmt;
                    inputFmt.sampleRate = packet.sampleRate;
                    inputFmt.channels = packet.channels;
                    inputFmt.bitsPerSample = packet.bitsPerSample;
                    inputFmt.validBitsPerSample =
                        packet.validBitsPerSample > 0 ? packet.validBitsPerSample : packet.bitsPerSample;
                    inputFmt.isFloat = packet.isFloat;
                    inputFmt.blockAlign = (packet.channels * packet.bitsPerSample) / 8;
                    inputFmt.channelMask = packet.channelMask;

                    // Initialize/Reinitialize resampler if needed (check all input format fields)
                    bool needReinit = !src.resampler->IsReady();
                    if (!needReinit) {
                        const auto& cur = src.resampler->GetInputFormat();
                        needReinit = (cur.sampleRate != inputFmt.sampleRate || cur.channels != inputFmt.channels ||
                                      cur.bitsPerSample != inputFmt.bitsPerSample || cur.isFloat != inputFmt.isFloat ||
                                      cur.channelMask != inputFmt.channelMask);
                    }
                    if (needReinit) {
                        src.resampler->Init(inputFmt, targetFmt);
                    }

                    uint8_t** resampledData = nullptr;
                    int outSamples = 0;

                    if (src.resampler->Process(packet.data.data(), (int)packet.data.size(), &resampledData,
                                               &outSamples)) {
                        if (outSamples > 0 && resampledData && resampledData[0]) {
                            // OBSOLETE: sourceBuffers accumulation removed to prevent memory
                            // leak size_t oldSize = sourceBuffers[srcIdx].size();
                            // sourceBuffers[srcIdx].resize(oldSize + numFloats);

                            // PULL MODEL (Phase 2): Write to Ring Buffer only
                            // SYNC FIX: Skip write if video thread is clearing buffers
                            if (src.ringBuffer && !audioSyncPending.load()) {
                                // Check for startup delay (Audio arriving late relative to
                                // Video Start)
                                int64_t startQPC = recordingStartSystemQPCMs.load();
                                if (startQPC != 0 && sourceTimestamps[srcIdx] == 0) {
                                    // First packet alignment using System QPC
                                    int64_t pTime = packet.timestamp;  // Packet comes with Absolute QPC MS
                                    int64_t diff = pTime - startQPC;
                                    src.alignedStartMs = diff;
                                    src.observedLateStartMs = std::max<int64_t>(diff, 0);
                                    src.hasAlignedStart = true;
                                    src.timelineValid = true;
                                    if (diff < -5) {
                                        DLL_Log("[AudioLoop] First packet leads recording start by %lld ms for src=%d",
                                                diff, (int)srcIdx);
                                    }
                                }

                                const bool firstTimelinePacket = (sourceTimestamps[srcIdx] == 0);
                                const bool firstPacketSawSyncPending = src.sawSyncPendingPackets;
                                sourceTimestamps[srcIdx] = packet.timestamp;
                                float* writeFloats = (float*)resampledData[0];
                                size_t writeSamples = static_cast<size_t>(outSamples);
                                const int64_t startQpc100ns =
                                    recordingStartSystemQpc100ns.load(std::memory_order_acquire);
                                if (startQpc100ns > 0 && packet.qpcPosition >= static_cast<uint64_t>(startQpc100ns)) {
                                    const uint64_t packetStartDelta100ns =
                                        packet.qpcPosition - static_cast<uint64_t>(startQpc100ns);
                                    int64_t packetStartSamples =
                                        static_cast<int64_t>(ce::audio::HundredNanosecondsToSamples(
                                            packetStartDelta100ns, targetFmt.sampleRate));
                                    // A/V equalization: delay this source to the common max latency
                                    // (leading silence inserted by the gap path below). 0 for the slowest.
                                    packetStartSamples += audioEqualizationDelaySamples[srcIdx];
                                    packetStartSamples = ce::audio::ApplyStartupPacketTimelineRebaseOffset(
                                        packetStartSamples, static_cast<int64_t>(src.startupRebasedGapSamples));
                                    if (srcIdx < encodedSamplesPerSource.size() &&
                                        ce::audio::ShouldAdvancePacketTimelineToEncodedCursor(src.sourceType ==
                                                                                              AudioConfig::AppAudio)) {
                                        const int64_t encodedCursorSamples =
                                            ce::audio::ResolveSourceTimelineWriteCursor(
                                                src.qpcAlignedWrittenSamples, encodedSamplesPerSource[srcIdx]);
                                        if (encodedCursorSamples > static_cast<int64_t>(src.qpcAlignedWrittenSamples)) {
                                            const int64_t cursorAdvance =
                                                encodedCursorSamples -
                                                static_cast<int64_t>(src.qpcAlignedWrittenSamples);
                                            src.qpcAlignedWrittenSamples = static_cast<uint64_t>(encodedCursorSamples);
                                            if (cursorAdvance >= targetFmt.sampleRate / 200) {
                                                const uint64_t nowTick = GetTickCount64();
                                                if (nowTick - src.lastPacketTimelineAdjustWarnTick >= 1000) {
                                                    DLL_Log(
                                                        "[AudioLoop] Late source cursor advance src=%d advanced=%lld "
                                                        "samples encodedCursor=%lld before packet stitching",
                                                        (int)srcIdx, (long long)cursorAdvance,
                                                        (long long)encodedCursorSamples);
                                                    src.lastPacketTimelineAdjustWarnTick = nowTick;
                                                }
                                            }
                                        }
                                    }
                                    if (firstTimelinePacket) {
                                        // Keep a small preserved startup gap so the first live chunk does not
                                        // begin mid-waveform. A smaller 5ms cap caused large real-audio
                                        // startup backlogs and aggressive steady-state trim/correction.
                                        const bool usesSharedStartupRebase =
                                            sourceParticipatesInSharedStartupRebase(srcIdx) &&
                                            sharedStartupRebaseOffsetSamples >= 0;
                                        const int64_t requestedRebaseOffset =
                                            usesSharedStartupRebase ? sharedStartupRebaseOffsetSamples
                                                                    : ce::audio::ComputeStartupFirstPacketRebaseOffset(
                                                                          packetStartSamples, firstPacketSawSyncPending,
                                                                          kStartupFirstPacketGapCapSamples +
                                                                              audioEqualizationDelaySamples[srcIdx],
                                                                          kStartupFirstPacketRebaseThresholdSamples);
                                        const int64_t rebaseOffset =
                                            std::clamp<int64_t>(requestedRebaseOffset, 0, packetStartSamples);
                                        if (rebaseOffset > 0) {
                                            packetStartSamples -= rebaseOffset;
                                            src.startupRebasedGapSamples += static_cast<uint64_t>(rebaseOffset);
                                            DLL_Log(
                                                "[AudioLoop] Startup rebase src=%d suppressed %lld samples of "
                                                "first-packet gap (packetStart=%lld cap=%lld shared=%d)",
                                                (int)srcIdx, (long long)rebaseOffset,
                                                (long long)(packetStartSamples + rebaseOffset),
                                                (long long)kStartupFirstPacketGapCapSamples,
                                                usesSharedStartupRebase ? 1 : 0);
                                        }
                                        src.sawSyncPendingPackets = false;
                                    }
                                    const auto lateJoin = ce::audio::ComputeLateAppSourceJoin(
                                        src.sourceType == AudioConfig::AppAudio, firstTimelinePacket,
                                        firstPacketSawSyncPending, packetStartSamples, trackTimelineSamples[src.track],
                                        targetFmt.sampleRate / 2, targetFmt.sampleRate / 100);
                                    if (lateJoin.joinLive) {
                                        if (lateJoin.joinCursorSamples >
                                            static_cast<int64_t>(src.qpcAlignedWrittenSamples)) {
                                            src.qpcAlignedWrittenSamples =
                                                static_cast<uint64_t>(lateJoin.joinCursorSamples);
                                        }
                                        src.lateAppJoinSuppressedGapSamples +=
                                            static_cast<uint64_t>(lateJoin.suppressedGapSamples);
                                        src.lateAppJoinPreservedGapSamples +=
                                            static_cast<uint64_t>(lateJoin.preservedGapSamples);
                                        src.pendingStartupJoinFade = true;
                                        src.packetBoundaryFadeInSamplesRemaining =
                                            static_cast<int>(std::max<int64_t>(1, targetFmt.sampleRate / 750));
                                        DLL_Log(
                                            "[AudioLoop] Late app source live join src=%d track=%d process=%s "
                                            "packetStart=%lld trackCursor=%lld joinCursor=%lld "
                                            "suppressedGap=%lld preservedGap=%lld qpcStart=%llu",
                                            (int)srcIdx, src.track,
                                            src.config.processName.empty() ? "<none>" : src.config.processName.c_str(),
                                            (long long)packetStartSamples, (long long)trackTimelineSamples[src.track],
                                            (long long)lateJoin.joinCursorSamples,
                                            (long long)lateJoin.suppressedGapSamples,
                                            (long long)lateJoin.preservedGapSamples,
                                            (unsigned long long)packet.qpcPosition);
                                    }
                                    const auto timelineAdjustment =
                                        ce::audio::ComputeStartupAwarePacketTimelineAdjustment(
                                            packetStartSamples, static_cast<int64_t>(src.qpcAlignedWrittenSamples),
                                            targetFmt.sampleRate / 1000, (targetFmt.sampleRate * 150) / 1000,
                                            targetFmt.sampleRate / 250, targetFmt.sampleRate / 200);
                                    const size_t packetTimelineFadeSamples =
                                        static_cast<size_t>(std::max<int64_t>(1, targetFmt.sampleRate / 750));
                                    if (timelineAdjustment.gapSamples > 0) {
                                        // Defense-in-depth: bound the leading-silence gap to what the ring
                                        // buffer can actually retain. WriteRetainNew drops the oldest samples
                                        // to make room, so any excess is discarded anyway; clamping here makes
                                        // it impossible for a corrupt/out-of-domain packet timestamp to size a
                                        // pathological allocation (previously a bogus 192kHz-loopback QPC
                                        // produced a multi-TB std::vector<float> -> bad_alloc -> terminate).
                                        const int64_t ringCapacitySamples =
                                            static_cast<int64_t>(src.ringBuffer->GetCapacity()) /
                                            std::max<int64_t>(1, targetFmt.channels);
                                        const int64_t boundedGapSamples = ce::audio::ClampTimelineGapSamplesToCapacity(
                                            timelineAdjustment.gapSamples, ringCapacitySamples);
                                        if (boundedGapSamples < timelineAdjustment.gapSamples) {
                                            DLL_Log(
                                                "[AudioLoop] WARNING: clamped oversized timeline gap src=%d "
                                                "gap=%lld -> %lld samples (ringCapacity=%lld); qpcStart=%llu likely "
                                                "out-of-domain timestamp",
                                                (int)srcIdx, (long long)timelineAdjustment.gapSamples,
                                                (long long)boundedGapSamples, (long long)ringCapacitySamples,
                                                (unsigned long long)packet.qpcPosition);
                                        }
                                        const size_t gapSamples = static_cast<size_t>(boundedGapSamples);
                                        std::vector<float> silence(gapSamples * targetFmt.channels, 0.0f);
                                        const size_t writtenGapFloats =
                                            src.ringBuffer->WriteRetainNew(silence.data(), silence.size());
                                        const size_t writtenGapSamples = writtenGapFloats / targetFmt.channels;
                                        if (!src.bootstrapComplete) {
                                            src.startupSyntheticRingSamples += writtenGapSamples;
                                            if (firstTimelinePacket) {
                                                src.startupGapProtectionSamples += writtenGapSamples;
                                            }
                                        }
                                        src.qpcAlignedWrittenSamples += writtenGapSamples;
                                        src.packetTimelineGapSamples += writtenGapSamples;
                                        if (writtenGapSamples > 0 && writeSamples > 0) {
                                            src.packetBoundaryFadeInSamplesRemaining =
                                                static_cast<int>(packetTimelineFadeSamples);
                                        }
                                    } else if (timelineAdjustment.overlapSamples > 0) {
                                        const size_t overlapSamples = static_cast<size_t>(std::min<int64_t>(
                                            timelineAdjustment.overlapSamples, static_cast<int64_t>(writeSamples)));
                                        writeFloats += overlapSamples * targetFmt.channels;
                                        writeSamples -= overlapSamples;
                                        src.packetTimelineOverlapSamples += overlapSamples;
                                        if (overlapSamples > 0 && writeSamples > 0) {
                                            src.packetBoundaryFadeInSamplesRemaining =
                                                static_cast<int>(packetTimelineFadeSamples);
                                        }
                                    }

                                    if ((timelineAdjustment.gapSamples >= (targetFmt.sampleRate / 200) ||
                                         timelineAdjustment.overlapSamples >= (targetFmt.sampleRate / 200))) {
                                        const uint64_t nowTick = GetTickCount64();
                                        if (nowTick - src.lastPacketTimelineAdjustWarnTick >= 1000) {
                                            DLL_Log(
                                                "[AudioLoop] Packet timeline adjust src=%d gap=%lld overlap=%lld "
                                                "written=%llu qpcStart=%llu",
                                                (int)srcIdx, (long long)timelineAdjustment.gapSamples,
                                                (long long)timelineAdjustment.overlapSamples,
                                                (unsigned long long)src.qpcAlignedWrittenSamples,
                                                (unsigned long long)packet.qpcPosition);
                                            src.lastPacketTimelineAdjustWarnTick = nowTick;
                                        }
                                    }

                                    // Placement-divergence diagnostics (throttled 1/s, app sources). The smoking
                                    // gun for "app track goes silent while capture stays live" is the qpc-placed
                                    // write position racing ahead of the read/encoded cursor: that fills the ring
                                    // (forcing retain-trim of the very audio the reader needs) while the reader
                                    // underruns at its own cursor. writeMinusEncoded growing unbounded + ringAvail
                                    // pinned near capacity is the failure; a stable small writeMinusEncoded is healthy.
                                    if (src.sourceType == AudioConfig::AppAudio) {
                                        const uint64_t nowDiagTick = GetTickCount64();
                                        if (nowDiagTick - src.lastAppPlaceDiagTick >= 1000) {
                                            const int64_t encodedCursor = srcIdx < encodedSamplesPerSource.size()
                                                                              ? encodedSamplesPerSource[srcIdx]
                                                                              : 0;
                                            const int64_t writeMinusEncoded =
                                                static_cast<int64_t>(src.qpcAlignedWrittenSamples) - encodedCursor;
                                            const size_t ringAvailSamples =
                                                src.ringBuffer
                                                    ? src.ringBuffer->GetAvailable() / std::max(1, targetFmt.channels)
                                                    : 0;
                                            const size_t ringCapSamples =
                                                src.ringBuffer
                                                    ? src.ringBuffer->GetCapacity() / std::max(1, targetFmt.channels)
                                                    : 0;
                                            DLL_Log(
                                                "[AppDiag] place src=%d track=%d placed=%lld writeCursor=%llu "
                                                "encodedCursor=%lld writeMinusEncoded=%lld pendingWriteSamples=%zu "
                                                "ringAvail=%zu/%zu gapTotal=%llu overlapTotal=%llu lastGap=%lld "
                                                "lastOverlap=%lld",
                                                (int)srcIdx, src.track, (long long)packetStartSamples,
                                                (unsigned long long)src.qpcAlignedWrittenSamples,
                                                (long long)encodedCursor, (long long)writeMinusEncoded, writeSamples,
                                                ringAvailSamples, ringCapSamples,
                                                (unsigned long long)src.packetTimelineGapSamples,
                                                (unsigned long long)src.packetTimelineOverlapSamples,
                                                (long long)timelineAdjustment.gapSamples,
                                                (long long)timelineAdjustment.overlapSamples);
                                            src.lastAppPlaceDiagTick = nowDiagTick;
                                        }
                                    }
                                }

                                if (writeSamples > 0) {
                                    if (src.packetBoundaryFadeInSamplesRemaining > 0) {
                                        const size_t fadeSamples =
                                            static_cast<size_t>(src.packetBoundaryFadeInSamplesRemaining);
                                        ApplyPacketBoundaryFadeIn(writeFloats, writeSamples,
                                                                  static_cast<size_t>(targetFmt.channels), fadeSamples);
                                        src.packetBoundaryFadeInSamplesRemaining =
                                            fadeSamples > writeSamples ? static_cast<int>(fadeSamples - writeSamples)
                                                                       : 0;
                                    }
                                    // WriteRetainNew: atomically drops oldest audio to make room,
                                    // then writes new audio. No race between GetFree/Skip/Write.
                                    const size_t writtenFloats =
                                        src.ringBuffer->WriteRetainNew(writeFloats, writeSamples * targetFmt.channels);
                                    src.qpcAlignedWrittenSamples += writtenFloats / targetFmt.channels;
                                }
                            } else if (src.ringBuffer && audioSyncPending.load()) {
                                // The sync gate is still closed, so this packet is
                                // intentionally discarded and must not establish the
                                // source timeline yet.
                                src.sawSyncPendingPackets = true;
                            } else if (!src.ringBuffer) {
                                DLL_Log("[AudioLoop] ERROR: No RingBuffer for source %d", (int)srcIdx);
                            }
                        }

                        // CRITICAL FIX: Always free the buffer allocated by resampler, even
                        // if outSamples == 0 AudioResampler::Process allocates memory via
                        // av_samples_alloc... which must be freed.
                        AudioResampler::FreeOutputBuffer(resampledData);
                    }
                }
            }

            // ===================================================================
            // PULL MODEL (Phase 2): Legacy audio mixing logic removed
            // ===================================================================

            // Audio-only recording has no video thread to drive the pull model.
            // Use the same per-track mixer as video recording so every source
            // sharing a track contributes exactly once and every exported track
            // advances from one coherent sample cursor.
            if (audioOnly && recording && !audioStopDrainRequested.load(std::memory_order_acquire)) {
                const int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - recordingStartTime)
                                              .count();
                if (elapsedUs > 0) {
                    PullAndEncodeAudio(elapsedUs);
                    ++mixCount;
                }
            }

            if (gotAnyPacket) {
                audioDrainCv.notify_all();
            }

            if (!gotAnyPacket) {
                if (audioStopDrainRequested.load(std::memory_order_acquire)) {
                    if (audioOnly && recording && !audioOnlyStopTailFinalized) {
                        // All capture queues are now empty. Flush both resampler
                        // layers on their owning audio worker before selecting the
                        // final shared-track endpoint, then force every track to
                        // that same sample count.
                        FlushAudioOnlyResamplerTails();

                        constexpr int64_t kMixerSampleRate = 48000;
                        const int64_t wallElapsedUs =
                            std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(
                                                     std::chrono::steady_clock::now() - recordingStartTime)
                                                     .count());
                        int64_t finalTargetSamples =
                            ce::audio::ComputeDurationUsToSamples(wallElapsedUs, kMixerSampleRate);
                        for (const auto& src : audioSources) {
                            finalTargetSamples = std::max<int64_t>(finalTargetSamples,
                                                                   static_cast<int64_t>(src.qpcAlignedWrittenSamples));
                            finalTargetSamples = std::max(finalTargetSamples, src.syncSamplesOutput);
                        }
                        for (const auto& track : trackTimelineSamples) {
                            finalTargetSamples = std::max(finalTargetSamples, track.second);
                        }
                        const int64_t finalTargetUs =
                            (finalTargetSamples * 1000000ll + kMixerSampleRate - 1) / kMixerSampleRate;

                        auto minimumTrackCursor = [this]() -> int64_t {
                            if (cachedTrackToSources.empty()) {
                                return 0;
                            }
                            int64_t minimum = std::numeric_limits<int64_t>::max();
                            for (const auto& track : cachedTrackToSources) {
                                const auto cursor = trackTimelineSamples.find(track.first);
                                minimum = std::min(minimum, cursor != trackTimelineSamples.end() ? cursor->second : 0);
                            }
                            return minimum == std::numeric_limits<int64_t>::max() ? 0 : minimum;
                        };

                        const int64_t beforeDrain = minimumTrackCursor();
                        const int64_t missingSamples = std::max<int64_t>(0, finalTargetSamples - beforeDrain);
                        const int64_t maxDrainIterations = std::max<int64_t>(
                            1, (missingSamples + (kMixerSampleRate / 2) - 1) / (kMixerSampleRate / 2) + 8);
                        int64_t drainIterations = 0;
                        for (; drainIterations < maxDrainIterations; ++drainIterations) {
                            const int64_t before = minimumTrackCursor();
                            if (before >= finalTargetSamples || cachedTrackToSources.empty()) {
                                break;
                            }
                            PullAndEncodeAudio(finalTargetUs, true);
                            const int64_t after = minimumTrackCursor();
                            if (after <= before) {
                                DLL_Log(
                                    "[StopAudio] WARNING: audio-only final pull made no progress: target=%lld "
                                    "minimum=%lld iteration=%lld/%lld",
                                    finalTargetSamples, after, drainIterations + 1, maxDrainIterations);
                                break;
                            }
                        }
                        const int64_t afterDrain = minimumTrackCursor();
                        audioOnlyStopTailFinalized = true;
                        DLL_Log(
                            "[StopAudio] Audio-only mixed tail finalized: target=%lld samples wall=%lldus "
                            "minimumBefore=%lld minimumAfter=%lld iterations=%lld tracks=%zu",
                            finalTargetSamples, wallElapsedUs, beforeDrain, afterDrain, drainIterations,
                            cachedTrackToSources.size());
                    }
                    audioStopDrainComplete.store(true, std::memory_order_release);
                    audioDrainCv.notify_all();
                }

                std::unique_lock<std::mutex> lock(audioDrainMutex);
                audioDrainCv.wait_for(lock, std::chrono::milliseconds(5), [this]() {
                    return !audioRunning.load(std::memory_order_acquire) ||
                           audioStopDrainRequested.load(std::memory_order_acquire);
                });
            }
        }

        for (size_t srcIdx = 0; srcIdx < audioSources.size(); ++srcIdx) {
            if (audioSources[srcIdx].captureFanoutOwnerIndex != srcIdx ||
                (captureFanoutPacketCounts[srcIdx] == 0 && batchedPreStartDiscardCounts[srcIdx] == 0)) {
                continue;
            }
            size_t pendingFollowers = 0;
            for (size_t routeIdx = 0; routeIdx < audioSources.size(); ++routeIdx) {
                if (audioSources[routeIdx].captureFanoutOwnerIndex == srcIdx) {
                    pendingFollowers += captureFanoutQueues[routeIdx].size();
                }
            }
            DLL_Log(
                "[AudioRoute] Stop owner=%zu fannedPackets=%llu batchedPreStartDiscards=%llu "
                "pendingFollowerPackets=%zu",
                srcIdx, static_cast<unsigned long long>(captureFanoutPacketCounts[srcIdx]),
                static_cast<unsigned long long>(batchedPreStartDiscardCounts[srcIdx]), pendingFollowers);
        }

        DLL_Log(
            "MediaEngine: Audio thread stopped, processed %d packets, %d mixed "
            "chunks",
            packetCount, mixCount);
    }
};

static std::unique_ptr<MediaEngine> g_Engine;
static std::recursive_mutex g_EngineApiMutex;
static bool g_PendingAudioOnly = false;

extern "C" {

// Global Logger
static std::atomic<LogCallback> g_LogCallback{nullptr};
static void ReleaseSharedD3D11DeviceGlobals();

MEDIAENGINE_API void DLL_Log(const char* fmt, ...) {
    LogCallback callback = g_LogCallback.load(std::memory_order_acquire);
    if (!callback)
        return;
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    callback(buffer);
}

MEDIAENGINE_API void MediaEngine_SetLogCallback(LogCallback callback) {
    g_LogCallback.store(callback, std::memory_order_release);
    DLL_Log("MediaEngine Logging Initialized");
}

MEDIAENGINE_API bool MediaEngine_Init(const AppConfig* config) {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    DLL_Log("[Media] MediaEngine_Init Called. Version: %s (Built: %s)", CAPTURE_VERSION, BUILD_TIMESTAMP);
    if (!g_Engine) {
        g_Engine = std::make_unique<MediaEngine>();
    }
    if (g_PendingAudioOnly) {
        g_Engine->SetAudioOnly(true);
        g_PendingAudioOnly = false;
        DLL_Log("[Media] MediaEngine_Init: audio-only mode enabled");
    }
    // config is a pointer, pass it directly
    return g_Engine->Init(config);
}

MEDIAENGINE_API bool MediaEngine_StartRecording() {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine)
        return g_Engine->StartRecording();
    return false;
}

MEDIAENGINE_API void MediaEngine_ReloadConfig(const AppConfig* config) {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine)
        g_Engine->ReloadConfig(config);
}

MEDIAENGINE_API void MediaEngine_SetActiveScreenGrab(bool activeScreenGrab) {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine)
        g_Engine->SetActiveScreenGrab(activeScreenGrab);
}

MEDIAENGINE_API void MediaEngine_SetWgcStartupExtraDelayQpc(int64_t delayQpc) {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine)
        g_Engine->SetWgcStartupExtraDelayQpc(delayQpc);
}

MEDIAENGINE_API void MediaEngine_SetAudioOnly(bool audioOnly) {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    g_PendingAudioOnly = audioOnly;
    if (g_Engine)
        g_Engine->SetAudioOnly(audioOnly);
}

MEDIAENGINE_API void MediaEngine_StopRecording() {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine)
        g_Engine->StopRecording();
}

MEDIAENGINE_API void MediaEngine_ReleaseEncoderTextures() {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine)
        g_Engine->ReleaseEncoderTextures();
}

MEDIAENGINE_API bool MediaEngine_MeasureRenderEndpointLatency(const char* cacheDir, bool forceRemeasure,
                                                              double* outLatencyMs) {
    // Standalone WASAPI probe; intentionally NOT guarded by the engine instance (it can run before
    // MediaEngine_Init). The probe itself is fail-safe and logs all components.
    const std::string dir = cacheDir ? cacheDir : "";
    const ce::audio::RenderLatencyProbeResult r = ce::audio::MeasureRenderEndpointLatency(dir, forceRemeasure);
    if (!r.ok) {
        return false;
    }
    if (outLatencyMs) {
        *outLatencyMs = r.latencyMs;
    }
    return true;
}

MEDIAENGINE_API void MediaEngine_Shutdown() {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine)
        g_Engine->StopRecording();
    g_Engine.reset();
    ReleaseSharedD3D11DeviceGlobals();
}

MEDIAENGINE_API bool MediaEngine_ProcessFrame(uint64_t textureHandle, uint64_t fenceHandle, uint64_t fenceValue,
                                              int64_t timestamp, int32_t luidLow, int32_t luidHigh, uint32_t sourcePid,
                                              uint32_t width, uint32_t height, uint32_t format, bool isHDR,
                                              bool isShmem, int shmemSlot,
                                              const ce::cursor::CaptureState* cursorState) {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine) {
        return g_Engine->ProcessFrame(textureHandle, fenceHandle, fenceValue, timestamp, luidLow, luidHigh, sourcePid,
                                      width, height, format, isHDR, isShmem, shmemSlot, cursorState);
    }
    return false;
}

MEDIAENGINE_API bool MediaEngine_RepeatLastFrame(int64_t timestamp, const ce::cursor::CaptureState* cursorState) {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine) {
        return g_Engine->RepeatLastFrame(timestamp, cursorState);
    }
    return false;
}

MEDIAENGINE_API bool MediaEngine_RepeatLastFrameWithTimeline(int64_t timestamp, int64_t timelineElapsedUs,
                                                             const ce::cursor::CaptureState* cursorState) {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine) {
        return g_Engine->RepeatLastFrame(timestamp, timelineElapsedUs, cursorState);
    }
    return false;
}

MEDIAENGINE_API bool MediaEngine_CanRepeatLastFrame() {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine) {
        return g_Engine->CanRepeatLastFrame();
    }
    return false;
}

MEDIAENGINE_API void MediaEngine_ResetRepeatFrameCache() {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine) {
        g_Engine->ResetRepeatFrameCache();
    }
}

MEDIAENGINE_API bool MediaEngine_PrepareFrameD3D11(void* texture, uint32_t width, uint32_t height, bool isHDR) {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine) {
        return g_Engine->PrepareFrameD3D11(texture, width, height, isHDR);
    }
    return false;
}

MEDIAENGINE_API bool MediaEngine_ProcessFrameD3D11(void* texture, int64_t timestamp, uint32_t width, uint32_t height,
                                                   bool isHDR, int32_t captureLeft, int32_t captureTop,
                                                   int64_t timelineElapsedUs,
                                                   const ce::cursor::CaptureState* cursorState) {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine) {
        return g_Engine->ProcessFrameD3D11(texture, timestamp, width, height, isHDR, captureLeft, captureTop,
                                           timelineElapsedUs, cursorState);
    }
    return false;
}

MEDIAENGINE_API bool MediaEngine_CreateSharedCaptureTextures(uint32_t width, uint32_t height, uint32_t format,
                                                             SharedMemoryLayout* sharedMem) {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (!g_Engine) {
        DLL_Log("[MediaEngine] CreateSharedCaptureTextures: Engine not ready");
        return false;
    }
    return g_Engine->CreateSharedCaptureTextures(width, height, format, sharedMem);
}

MEDIAENGINE_API int64_t MediaEngine_GetLastFrameEncodeTimeUs() {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine) {
        return g_Engine->GetLastVideoEncodeTimeUs();
    }
    return 0;
}

MEDIAENGINE_API void MediaEngine_SetSharedMem(void* pSharedMem, void* pShmem) {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine)
        g_Engine->UpdateVideoEncoderSharedMem(pSharedMem, pShmem);
}

MEDIAENGINE_API int64_t MediaEngine_GetLastFrameFenceWaitUs() {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine) {
        return g_Engine->GetLastFrameFenceWaitUs();
    }
    return 0;
}

MEDIAENGINE_API bool MediaEngine_WasLastFrameDeferred() {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine) {
        return g_Engine->WasLastFrameDeferred();
    }
    return false;
}

// Shared D3D11 device for screengrab mode - ensures ScreenCapture and
// VideoEncoder use same device
ID3D11Device* g_SharedD3D11Device = nullptr;
ID3D11DeviceContext* g_SharedD3D11Context = nullptr;

static void ReleaseSharedD3D11DeviceGlobals() {
    if (g_SharedD3D11Context) {
        g_SharedD3D11Context->ClearState();
        g_SharedD3D11Context->Flush();
    }
    if (g_SharedD3D11Device) {
        IDXGIDevice3* dxgiDevice3 = nullptr;
        if (SUCCEEDED(g_SharedD3D11Device->QueryInterface(IID_PPV_ARGS(&dxgiDevice3))) && dxgiDevice3) {
            dxgiDevice3->Trim();
            dxgiDevice3->Release();
            DLL_Log("[MediaEngine] Trimmed shared D3D11 device residency");
        }
    }
    if (g_SharedD3D11Context) {
        g_SharedD3D11Context->Release();
        g_SharedD3D11Context = nullptr;
    }
    if (g_SharedD3D11Device) {
        g_SharedD3D11Device->Release();
        g_SharedD3D11Device = nullptr;
    }
}

MEDIAENGINE_API ID3D11Device* MediaEngine_GetD3D11Device() {
    if (g_SharedD3D11Device)
        return g_SharedD3D11Device;

    // Create D3D11 device with video support
    D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL featureLevel;

    UINT createFlags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags, featureLevels, 2,
                                   D3D11_SDK_VERSION, &g_SharedD3D11Device, &featureLevel, &g_SharedD3D11Context);

    if (FAILED(hr)) {
        DLL_Log("[MediaEngine] Failed to create shared D3D11 device: HR=0x%x", hr);
        return nullptr;
    }

    DLL_Log("[MediaEngine] Created shared D3D11 device (Feature Level: 0x%x)", featureLevel);
    // Enable Multithreaded protection for D3D11 device
    ID3D11Multithread* pMultithread = nullptr;
    if (SUCCEEDED(g_SharedD3D11Device->QueryInterface(__uuidof(ID3D11Multithread), (void**)&pMultithread))) {
        pMultithread->SetMultithreadProtected(TRUE);
        pMultithread->Release();
        DLL_Log("[Media] D3D11 Multithread protection ENABLED");
    }

    return g_SharedD3D11Device;
}

MEDIAENGINE_API void MediaEngine_ReleaseSharedD3D11Device() {
    ReleaseSharedD3D11DeviceGlobals();
}

// D3D11 Immediate Context Mutex
// Protects access to the immediate context shared between WGC thread and
// Encoder thread
std::recursive_mutex g_D3D11Mutex;

MEDIAENGINE_API void MediaEngine_LockD3D11() {
    g_D3D11Mutex.lock();
}

MEDIAENGINE_API void MediaEngine_UnlockD3D11() {
    g_D3D11Mutex.unlock();
}

MEDIAENGINE_API void MediaEngine_SetSourcePrefers10Bit(bool prefer10Bit) {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine)
        g_Engine->SetSourcePrefers10BitHint(prefer10Bit);
}

// Suppress encoder-side cursor composition while the capture source's frames
// already contain the cursor (DXGI duplication reporting a software/composed
// cursor) so the recording does not show a double cursor. Toggled by the
// capture layer on hardware/software cursor-plane transitions.
MEDIAENGINE_API void MediaEngine_SetCursorCompositionSuppressed(bool suppressed) {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine)
        g_Engine->SetCursorCompositionSuppressedHint(suppressed);
}

}  // extern "C"
