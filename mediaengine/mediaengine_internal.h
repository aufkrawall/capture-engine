#pragma once

class MediaEngine;

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

struct AudioPullState;
struct AudioLoopState;

class MediaEngine {
public:
    MediaEngine()
        : recording(false),
          audioRunning(false),
          recordingStartTime(),
          firstVideoFrameMs(0),
          firstVideoFrameCommitted(false),
          lastVideoFrameMs(0),
          videoElapsedMs(0) {}~MediaEngine();

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
    };float ComputeRaisedCosineFade(size_t index, size_t totalSamples);void ApplyPacketBoundaryFadeIn(float* interleavedSamples, size_t sampleCount, size_t channels,
                                          size_t fadeSamples);uint32_t DefaultChannelMaskForChannels(int channels);int ParseAudioSampleRate(const AudioConfig& audioConfig);int AudioSourceLayoutPriority(AudioConfig::SourceType sourceType);TrackAudioFormat ProbeSourceTrackFormat(const AudioConfig& audioConfig);std::map<int, TrackAudioFormat> ResolveTrackAudioFormats(const AppConfig& appConfig);TrackAudioFormat GetTrackAudioFormat(int track) const;void CaptureDropFadeAnchor(AudioSource& src, int channels);float GetDropFadeAnchor(const AudioSource& src, int channel);

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
int64_t GetVideoElapsedMs() const;bool SessionUsesVfr() const;bool SessionUsesScreenGrab() const;int64_t GetCommittedVideoElapsedUs(int64_t fallbackElapsedUs) const;void CommitVideoElapsedUs(SourceTimelineState& timelineState, int64_t elapsedUs);size_t GetBufferedTimelineSamples(const AudioSource& src) const;

    // AudioLoop -> PullAndEncodeAudio hand-off for the adaptive ingestion reservoir.
    // Publishes the worst (minimum) headroom seen since the pull side last consumed it,
    // normalized to the shared 48 kHz mixing rate. `kNoAudioIngestHeadroom` doubles as the
    // "nothing observed this window" sentinel so one atomic carries both facts and no
    // observation can be lost between the reader's two accesses.
    static constexpr int64_t kNoAudioIngestHeadroom = std::numeric_limits<int64_t>::max();void PublishAudioIngestHeadroom(int64_t headroomSamples, int sampleRate);

    // Tracks fully-destroyed packets for one source and applies the bounded last-resort
    // re-anchor when the adaptive reservoir has already saturated. Re-anchoring costs a
    // one-time content skip on this source only; the alternative is permanent silence.
void ServiceSourceIngestStarvation(AudioSource& src, size_t srcIdx, int64_t packetStartSamples,
                                       int64_t overlapSamples, size_t retainedWriteSamples, int resampledSamples,
                                       int sampleRate, uint64_t nowTick);size_t DropOldestBufferedSamples(AudioSource& src, size_t samplesToDrop);void DiscardPendingAudioPackets();size_t StopAudioCaptureSources(bool discardPendingPackets);

    struct FinalSourceCatchupStatus {
        bool ready = true;
        size_t sourceIndex = 0;
        int track = 0;
        int64_t requestedSamples = 0;
        size_t bufferedSamples = 0;
        int64_t missingSamples = 0;
    };FinalSourceCatchupStatus GetFinalCfrSourceCatchupStatus(int64_t targetUs) const;bool WaitForFinalCfrAudioSourceCatchup(int64_t targetUs);size_t StopCaptureSourcesAndDrainAudioLoop();void AudioThreadEntry() noexcept;bool StartAudioThread();void DrainStoppedCaptureQueuesBeforeFinalPull(int64_t targetUs);void ApplyAudioTimelineReset(uint64_t generation, int64_t startQpcMs, bool preservePendingPackets);void SyncAudioToFirstVideoFrame(int64_t startQpcMs, int64_t startQpc100ns, bool preservePendingPackets = false);

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

    // Adaptive CFR audio ingestion reservoir. The AudioLoop thread publishes the worst
    // observed distance between a freshly placed packet and the already-exported cursor;
    // PullAndEncodeAudio consumes it and deepens the pull lookahead so the producer can
    // overtake the consumer again. Deepening is sync-neutral: it moves only the pull
    // target, never a sample's timeline position. See audio_sync_utils.h for the policy.
    std::atomic<int64_t> audioIngestWorstHeadroomSamples{std::numeric_limits<int64_t>::max()};
    ce::audio::AudioIngestReservoirState audioIngestReservoir;
    int64_t audioIngestReservoirExtraMs = 0;
    uint64_t audioIngestReservoirEvalTick = 0;
    uint64_t audioIngestReservoirLogTick = 0;
    int64_t audioIngestReservoirLoggedMs = 0;
    int64_t audioIngestReservoirPeakMs = 0;

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
    int mixLogCounter = 0;int64_t GetLastVideoEncodeTimeUs() const;int64_t GetLastFrameFenceWaitUs() const;bool WasLastFrameDeferred() const;bool CanRepeatLastFrame();void ResetRepeatFrameCache();void ReleaseEncoderTextures();void UpdateVideoEncoderSharedMem(void* sharedMem, void* shmemBuffer);void SetSourcePrefers10BitHint(bool prefer10Bit);void SetCursorCompositionSuppressedHint(bool suppressed);void SetActiveScreenGrab(bool enabled);void SetAudioOnly(bool enabled);void InitAudioOnlyMuxer(const AppConfig* config);bool CleanupAudioOnlyMuxer();

    // Trusted System QPC Frequency
    int64_t qpcFreq = 0;bool Init(const AppConfig* config);int64_t GetLastVideoFenceWaitUs() const;void ResetAudioPullStateForRecording();bool StartRecording();void CancelUncommittedVideoRecording();bool StopRecording(bool cancelUncommittedVideo = false);bool ProcessFrame(uint64_t handle, uint64_t fenceHandle, uint64_t fenceVal, int64_t timestampQPC, int32_t luidLow,
                      int32_t luidHigh, uint32_t sourcePid, uint32_t width, uint32_t height, uint32_t format,
                      bool isHDR, bool isShmem = false, int shmemSlot = 0,
                      const ce::cursor::CaptureState* cursorState = nullptr);bool RepeatLastFrame(int64_t timestampQPC, const ce::cursor::CaptureState* cursorState = nullptr);bool RepeatLastFrame(int64_t timestampQPC, int64_t timelineElapsedUs,
                         const ce::cursor::CaptureState* cursorState = nullptr);void ExtendCfrToCommonAudioLattice();bool IsWgcCfrRecording() const;bool IsCfrRecording() const;double GetMaxAudioCaptureLatencyMs() const;int64_t GetMaxAudioCaptureLatencyQpc() const;void SetWgcStartupExtraDelayQpc(int64_t delayQpc);bool PrepareFrameD3D11(void* texture, uint32_t width, uint32_t height, bool isHDR);

    // Direct D3D11 texture processing for screengrab mode (zero-copy)
bool ProcessFrameD3D11(void* texture, int64_t timestampQPC, uint32_t width, uint32_t height, bool isHDR,
                           int32_t captureLeft, int32_t captureTop, int64_t timelineElapsedUs,
                           const ce::cursor::CaptureState* cursorState);void AppendSyncResamplerOutput(AudioSource& src, size_t srcIdx, int channels, uint8_t** resampledData,
                                   int outSamples);bool PumpSourceRingThroughSyncResampler(AudioSource& src, size_t srcIdx, int channels, size_t maxFloats);bool FlushCaptureResamplerForEpoch(AudioSource& src, size_t srcIdx, uint64_t oldEpoch, uint64_t newEpoch);bool ServiceAudioEpochResetOnPull(AudioSource& src, size_t srcIdx);void FlushAudioOnlyResamplerTails();

    // PULL MODEL: Pull audio from RingBuffers and encode against the relative
    // recording timeline that also drives CFR video emission.
void PullAndEncodeAudio(int64_t videoTimelineUs, bool forceDrain = false);

    // Create shared D3D11 textures for Vulkan games to import
bool CreateSharedCaptureTextures(uint32_t width, uint32_t height, uint32_t format, SharedMemoryLayout* sharedMem);void WritePacket(AVPacket* pkt);void ReloadConfig(const AppConfig* newConfig);

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
void CoalesceCaptureRoutes();ProcessLoopbackCapture* GetAppCaptureForRoute(size_t srcIdx);std::pair<int64_t, int64_t> GetCaptureGroupBufferedSampleRange(size_t srcIdx) const;

    // Shared initialization for ring buffer and sync resampler on an AudioSource.
    // Parses sample rate from config (defaults to 48000) and sets up both.
void InitAudioSourceBuffers(AudioSource& source, const AudioConfig& audioConfig, size_t sourceIdx);void AudioLoop();
    // PullAndEncodeAudio phase helpers (keep the function a semantic unit).
    bool ComputeAudioPullTargets(AudioPullState& s, int64_t videoTimelineUs, bool forceDrain);
    bool PullTrackBootstrap(AudioPullState& s, int track, const std::vector<size_t>& srcIndices);
    bool PullTrackGapAndBuffer(AudioPullState& s, int track, const std::vector<size_t>& srcIndices);
    bool PullTrackEncodeSourcesA(AudioPullState& s, int track, const std::vector<size_t>& srcIndices);
    bool PullTrackEncodeSourcesB(AudioPullState& s, int track, size_t srcIdx);
    bool PullTrackEncodeSourcesC1(AudioPullState& s, int track, size_t srcIdx);
    bool PullTrackEncodeSourcesC2(AudioPullState& s, int track, size_t srcIdx);
    bool PullTrackSyncMonitoring(AudioPullState& s, int track, const std::vector<size_t>& srcIndices);
    // AudioLoop phase helpers (keep the function a semantic unit).
    bool AudioLoopInit(AudioLoopState& s);
    bool AudioLoopIteration(AudioLoopState& s);
    bool AudioLoopPollSource(AudioLoopState& s, size_t srcIdx);
    bool AudioLoopCommitSource(AudioLoopState& s, size_t srcIdx);
    bool AudioLoopTail(AudioLoopState& s);
    bool SourceParticipatesInSharedStartupRebase(size_t srcIdx) const;
    bool TrySelectSharedStartupRebase(AudioLoopState& s, bool finalStopDrain);
};

inline std::unique_ptr<MediaEngine> mediaengine_g_Engine;

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
inline std::recursive_mutex mediaengine_g_EngineApiMutex;

inline bool mediaengine_g_PendingAudioOnly = false;

// Per-call state for PullAndEncodeAudio phases (locals moved out of the
// 1934-line function so every unit stays <= 800 lines).
struct AudioPullState {
    static constexpr int SAMPLE_RATE = 48000;
    static constexpr int64_t kSteadyAudioPullLatencyMs = ce::audio::kDefaultSteadyAudioPullLatencyMs;
    static constexpr int64_t kPrimedSourceCushionSamples = SAMPLE_RATE / 40;  // 25ms;
    static constexpr int64_t kBaseTargetLatencySamples = (kSteadyAudioPullLatencyMs * SAMPLE_RATE) / 1000;
    static constexpr int64_t kMaxPipelineLagContributionMs = 10000;
    static constexpr int64_t kWgcCoverageLossMaxBufferedLagMs = 300;
    static constexpr int64_t kRuntimeMaxLeadSamples = SAMPLE_RATE / 10;            // 100ms above target;
    static constexpr int64_t kRuntimeMaxDropPerCall = SAMPLE_RATE / 200;           // 5ms;
    static constexpr int64_t kRuntimeDropFadeSamples = SAMPLE_RATE / 100;          // 10ms;
    static constexpr int64_t kOverloadAudioPullQuantumSamples = SAMPLE_RATE / 50;  // 20ms;
    static constexpr int64_t kLatencyTrimHysteresisSamples = ce::audio::kDefaultAudioPullQuantumSamples;
    static constexpr int64_t kMinCompensationBufferSamples = kBaseTargetLatencySamples / 4;
    static constexpr int64_t kWgcCfrLeadWarningSamples = SAMPLE_RATE / 10;  // 100ms;
    static constexpr bool kWgcPreferVideoRepeatsOverAudioCuts = true;
    static constexpr double kDefaultMaxCompensationPercent = 1.0;
    static constexpr double kTier1MaxPitchPercent = 0.05;  // Keep WGC source-clock correction below audible pitch shift.;
    static constexpr double kAppAudioDrainMaxPitchPercent = 0.5;
    static constexpr int64_t kAppAudioDrainSlackSamples = SAMPLE_RATE / 50;         // 20ms above target;
    static constexpr int64_t kAppAudioDrainDeadbandSamples = SAMPLE_RATE / 100;     // 10ms compensation deadband;
    static constexpr int64_t kAppAudioLatencyWarnExcessSamples = SAMPLE_RATE / 20;  // 50ms above target;
    static constexpr int64_t kTier2DriftThresholdMs = 20;
    static constexpr int64_t kWgcEncoderShortfallBufferedLagMaxMs = 4000;
    static constexpr uint32_t kWgcEncoderHealthyDeliveryMarginFps = 4;
    static constexpr int64_t kWgcCoverageLossLeadSlackSamples = SAMPLE_RATE / 25;  // 40ms above target;
    static constexpr int64_t kWgcCoverageLossMaxDropPerCall = ce::audio::kDefaultAudioPullQuantumSamples;  // 5ms paced overload trim quantum;
    static constexpr int64_t kWgcVisualSyncMaxBufferedLagMs = 4000;
    static constexpr size_t kMinBootstrapRealSamples = static_cast<size_t>(SAMPLE_RATE / 40);  // 25ms;
    static constexpr int64_t kSparseStartedPartialSilenceThresholdSamples = ce::audio::kDefaultAudioPullQuantumSamples * 4;

    bool isCfrRecording{};
    bool isWgcCfrRecording{};
    uint64_t pullTick{};
    int64_t effectiveAudioPullLatencyMs{};
    int64_t baseTargetLatencySamples{};
    int64_t reservoirPassBaseMs{};
    int64_t wallVideoMs{};
    int64_t encodedVideoMs{};
    int64_t audioTargetUs{};
    int64_t audioTargetMs{};
    std::chrono::steady_clock::time_point now{};
    int64_t steadyElapsedUs{};
    int64_t timelineShortfallMs{};
    int64_t videoPipelineLagMs{};
    uint32_t configuredWgcOutputFps{};
    uint32_t wgcTargetFps{};
    uint32_t wgcDeliveredFps{};
    uint32_t wgcDeliveredMin250Fps{};
    uint32_t wgcDeliveredMin500Fps{};
    int64_t wgcBufferedVideoContentLagMs{};
    bool wgcCoverageLossActive{};
    uint32_t wgcOverloadFlags{};
    bool wgcEncoderBottlenecked{};
    uint32_t wgcQueueEmptyTickPermille{};
    uint32_t wgcBufferedAtTickMin{};
    uint32_t wgcSingleFrameTickCount{};
    int64_t wgcSelectionBiasUs{};
    uint32_t wgcRecordingCadenceFps{};
    ce::audio::WgcAudioLagTargets wgcAudioLagTargets{};
    int64_t wgcSteadyStateBufferedAudioLagMs{};
    uint32_t effectiveDeliveredFpsForAudioContinuity{};
    bool wgcEncoderOnlyOverload{};
    int64_t wgcEncoderShortfallBufferedLagMs{};
    int64_t screenGrabDriftLagMs{};
    int64_t effectiveSourceClockDriftLagMs{};
    int64_t wgcSelectedContentLeadMs{};
    int64_t wgcSelectedContentLagMs{};
    int64_t wgcVisualContentLagMs{};
    int64_t screenGrabTargetBufferLagMs{};
    int64_t effectiveAudioTargetBufferLagMs{};
    int64_t targetBufferedLagCapMs{};
    uint32_t maxWgcAudioLeadExcessSamples{};
    int64_t wallVideoLagMs{};
    bool cfrTimelineRecoveryActive{};
    uint64_t reservoirTick{};
    bool reservoirActive{};
    int64_t elapsedSinceEvalMs{};
    int64_t observedHeadroomSamples{};
    bool headroomObserved{};
    int64_t observedHeadroomMs{};
    ce::audio::AudioIngestReservoirDecision reservoirDecision{};
    bool reservoirChanged{};
    int64_t encodedVideoUs{};
    uint32_t telemetryTargetFps{};
    int64_t steadyElapsedMs{};
    MediaEngine::TrackAudioFormat trackFormat{};
    int CHANNELS{};
    uint32_t CHANNEL_MASK{};
    bool trackAllPrimed{};
    int64_t trackMaxObservedLateStartMs{};
    bool trackStartupSettled{};
    int64_t trackAudioPullLatencyMs{};
    int64_t trackAudioTargetUs{};
    int64_t trackAudioTargetMs{};
    int64_t targetSamples{};
    int64_t targetBufferedSamples{};
    bool trackReadyForBootstrap{};
    size_t requiredBootstrapSamples{};
    size_t firstSrcIdx{};
    int64_t pendingSamples{};
    bool initialTrackCatchup{};
    bool overloadPullQuantum{};
    int64_t samplesToEncode{};
    int64_t MAX_GAP_SAMPLES{};
    int64_t MAX_SILENCE_CHUNK{};
    bool trackLargeBacklogDrain{};
    bool deferForSourceBuffer{};
    bool finalStopDrain{};
    size_t totalFloats{};
    std::vector<float> mixBuffer{};
    int activeSources{};
    int eligibleSources{};
    bool applyTransitionFade{};
    bool isAppAudioSource{};
    size_t primedSampleCount{};
    size_t bufferedRealSamples{};
    bool srcReady{};
    bool optionalUnstarted{};
    bool packetlessSilenceReady{};
    size_t bufferedTimelineSamples{};
    bool srcTimelineReady{};
    bool forcedBootstrap{};
    uint64_t bootstrapTrimmed{};
    uint64_t bootstrapProtected{};
    bool appCaptureRouteEnded{};
    bool inactiveStartedAppSourceMaySilence{};
    bool sparseStartedSourceCanSilence{};
    bool sparseStartedSourceMaySilence{};
    int64_t sourceIngestIdleMs{};
    bool sourceRecentlyDeliveredRealPackets{};
    bool expectedTimelineSilence{};
    size_t droppedFloats{};
    size_t retainedFloats{};
    int64_t remainingStartupProtectionSamples{};
    bool startupTimelineProtected{};
    int64_t targetLatencySamples{};
    size_t MIN_POST_RESAMPLE_FLOATS{};
    size_t startupProtectedFloats{};
    size_t MAX_POST_RESAMPLE_FLOATS{};
    std::vector<float> srcData{};
    size_t available{};
    size_t toCopy{};
    size_t syntheticCopiedSamples{};
    size_t realCopiedSamples{};
    bool wasSilent{};
    uint64_t nowTick{};
    uint64_t lastLogTick{};
    std::map<int, bool>::iterator it{};
    int64_t trackPos{};
    bool applyStartupTrackFade{};
    int64_t fadeSamples{};
    int64_t fadeStart{};
    std::vector<uint8_t> encodeData{};
    int64_t audioChunkTimestampMs{};
    int64_t videoMs{};
    int64_t encodedVideoUsForSummary{};
    int64_t scheduledVideoUsForSummary{};
    int64_t audioSamples{};
    int64_t audioUs{};
    int64_t audioMs{};
    int64_t avDrift{};
    int64_t latencyAdjustedAvDrift{};
    int64_t pipelineLagMs{};
    int64_t residualSamples{};
    int64_t residualUs{};
    int64_t audioVsEncodedUs{};
    int64_t audioVsTargetUs{};
    int64_t audioVsScheduledUs{};
    uint64_t overflowDropped{};
    uint64_t retainedTrimmed{};
    uint64_t latencyTrimmed{};
    uint64_t tier2Trimmed{};
    uint64_t postTrimmed{};
    uint64_t underrunPadded{};
    uint64_t coverageLossTrimmed{};
    uint64_t packetGapAdjusted{};
    uint64_t packetOverlapTrimmed{};
    uint64_t categorizedLatencyTrim{};
    uint64_t uncategorizedLatencyTrim{};
    int64_t reservoirLeadAllowanceSamples{};
    AudioEncoder::EncodeResult encodeResult{};
    int64_t videoTimelineUs{};
    bool forceDrain{};
};


// Per-call state for AudioLoop phases (locals moved out of the
// 1076-line function so every unit stays <= 800 lines).
struct AudioLoopState {
    static constexpr int64_t kStartupFirstPacketGapCapSamples = 480;
    static constexpr int64_t kStartupFirstPacketRebaseThresholdSamples = 2400;
    static constexpr int64_t kAudioWorkerSchedulingGapThresholdUs = 25000;

    int packetCount{};
    int mixCount{};
    std::vector<int64_t> sourceTimestamps{};
    std::vector<bool> sourceLoggedPreStartDrop{};
    std::vector<AudioPacket> sourceLastPackets{};
    std::vector<std::chrono::steady_clock::time_point> lastPacketTime{};
    std::vector<AudioPacket> deferredFirstTimelinePackets{};
    std::vector<bool> deferredFirstTimelinePacketValid{};
    std::vector<int64_t> deferredFirstTimelinePacketStartSamples{};
    std::vector<uint64_t> sourceCaptureEpochs{};
    std::vector<uint64_t> captureFanoutPacketCounts{};
    std::vector<uint64_t> batchedPreStartDiscardCounts{};
    std::chrono::steady_clock::time_point lastAudioWorkerIteration{};
    std::chrono::steady_clock::time_point audioWorkerSchedulingDiagnosticsArmTime{};
    uint64_t audioWorkerSchedulingGapEvents{};
    int64_t audioWorkerSchedulingGapMaxUs{};
    int64_t lastAudioWorkerSchedulingGapLogMs{};
    int64_t sharedStartupRebaseOffsetSamples{};
    uint64_t appliedAudioResetGeneration{};
    bool audioOnlyStopTailFinalized{};
    std::vector<int64_t> audioEqualizationDelaySamples{};
    bool gotAnyPacket{};
    std::chrono::steady_clock::time_point now{};
    std::vector<std::deque<AudioPacket>> pendingEpochPackets{};
    std::vector<std::deque<AudioPacket>> captureFanoutQueues{};
    std::map<int, int64_t> trackNextTimestamp{};
    AudioPacket packet{};
};
