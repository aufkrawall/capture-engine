#include "mediaengine.h"
#include "../common/logging.h"
#include "../common/shared_defs.h"
#include "app_audio_capture.h"
#include "audio_capture.h"
#include "audio_encoder.h"

#include <dxgi1_5.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
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
          firstVideoFrameMs(0),
          lastVideoFrameMs(0),
          videoElapsedMs(0),
          recordingStartTime() {}
    ~MediaEngine() {
        StopRecording();
    }

    // Multi-source audio support
    struct AudioSource {
        std::unique_ptr<AudioCapture> capture;        // For system/mic audio
        std::unique_ptr<AppAudioCapture> appCapture;  // For per-app audio
        std::unique_ptr<AudioEncoder> encoder;        // Owned encoder (if first source for this track)
        AudioEncoder* sharedEncoderPtr = nullptr;     // Always points to the encoder to use
        std::unique_ptr<AudioRingBuffer> ringBuffer;  // Pull Model Buffer (Writer=Capture, Reader=Encoder)
        std::unique_ptr<AudioResampler> resampler;    // Resampler for this source (to standard format)

        // Per-source drift compensation (Phase 2 of AV Sync overhaul)
        std::unique_ptr<AudioResampler> syncResampler;  // 48kHz->48kHz resampler with drift compensation
        std::deque<float> postResampleBuffer;           // Buffer after sync resampling for exact framing
        int64_t syncSamplesOutput = 0;                  // Total samples output by syncResampler (for drift calculation)
        int dropFadeSamplesRemaining = 0;               // Remaining samples for post-drop transition smoothing
        float dropFadeStartL = 0.0f;                    // Left sample anchor for drop transition
        float dropFadeStartR = 0.0f;                    // Right sample anchor for drop transition
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
        int64_t alignedStartMs = -1;                // First source packet offset relative to recording start
        int64_t observedLateStartMs = 0;            // Latest observed startup delay used for startup pull slack
        bool hasAlignedStart = false;               // True after first packet aligned to recording start
        bool isPrimed = false;                      // True after source has buffered a startup safety cushion
        bool bootstrapComplete = false;             // True after startup backlog is settled and live sync may engage
        bool pendingUnderrunRecoveryFade = false;   // Arm fade-in when real audio resumes after padded silence
        bool sawSyncPendingPackets = false;         // Audio arrived while sync gate was closed before first video frame
        bool startupRealAudioSeen = false;          // Real audio has been emitted for this source since sync reset
        bool pendingStartupJoinFade = false;        // Fade in real audio when a late source joins after startup silence
        uint64_t pendingRetainedTrimSamples = 0;    // Aggregated ring-headroom trims since the last periodic log
        uint32_t pendingRetainedTrimEvents = 0;     // Aggregated ring-headroom trim events since the last periodic log
        uint64_t pendingLatencyTrimSamples = 0;     // Aggregated trim samples since the last periodic log
        uint32_t pendingLatencyTrimEvents = 0;      // Aggregated trim events since the last periodic log
        uint64_t pendingTier2TrimSamples = 0;       // Aggregated tier2 drift trims since the last periodic log
        uint32_t pendingTier2TrimEvents = 0;        // Aggregated tier2 drift trim events since the last periodic log
        uint64_t pendingCoverageLossTrimSamples = 0;  // Aggregated coverage-loss trims since the last periodic log
        uint32_t pendingCoverageLossTrimEvents = 0;   // Aggregated coverage-loss trim events since the last log
        uint64_t lastRetainedTrimWarnTick = 0;        // Rate-limit explicit retained-audio warnings
        uint64_t lastExtremeDriftWarnTick = 0;        // Rate-limit chronic large-drift diagnostics
        uint64_t lastPacketTimelineAdjustWarnTick = 0;
        double wgcCoverageLossTrimAccumulator = 0.0;  // Fractional carry for paced overload micro-trims

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

        size_t ringBufferPeakSamples = 0;
        uint32_t ringBufferUnderrunCount = 0;
        AudioConfig::SourceType sourceType = AudioConfig::SystemAudio;
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

    // Per-track encoder with optional mixing (when multiple sources target same
    // track)

    // Member variables
    std::unique_ptr<VideoEncoder> videoEnc;
    std::vector<AudioSource> audioSources;
    SharedMemoryLayout* sharedMemLayout = nullptr;

    AppConfig config;
    std::recursive_mutex muxMutex;  // Must be recursive - WritePacket callback from EncodeFrame
    bool recording;

    // Audio thread
    std::thread audioThread;
    std::atomic<bool> audioRunning;
    std::atomic<bool> audioSyncPending{false};                 // Pause AudioLoop writes during buffer clear
    std::chrono::steady_clock::time_point recordingStartTime;  // CaptureEngine clock start time
    int64_t firstVideoFrameMs;                                 // Timestamp of first video frame for A/V sync
    int64_t lastVideoFrameMs;                                  // Timestamp of last video frame for audio trimming
    std::atomic<int64_t> videoElapsedMs;                       // Elapsed video time in ms for audio clock sync
    std::atomic<int64_t> recordingStartSystemQPCMs{0};         // Start time in System QPC MS (for Audio Alignment)
    std::atomic<int64_t> recordingStartSystemQpc100ns{0};      // Start time in 100-ns QPC units for packet stitching
    SourceTimelineState injectTimelineState;                   // Source-frame QPC for inject-relative timing
    SourceTimelineState d3d11TimelineState;                    // Source-frame QPC for WGC-relative timing

    // Get current video elapsed time for audio clock compensation
    int64_t GetVideoElapsedMs() const {
        return videoElapsedMs.load();
    }

    int64_t GetCommittedVideoElapsedUs(int64_t fallbackElapsedUs) const {
        if (config.video.useVFR || !videoEnc) {
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
        constexpr size_t kChannels = 2;

        size_t bufferedSamples = src.postResampleBuffer.size() / kChannels;
        if (src.ringBuffer) {
            bufferedSamples += src.ringBuffer->GetAvailable() / kChannels;
        }
        return bufferedSamples;
    }

    size_t DropOldestBufferedSamples(AudioSource& src, size_t samplesToDrop) {
        constexpr size_t kChannels = 2;

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

    void SyncAudioToFirstVideoFrame(int64_t startQpcMs, int64_t startQpc100ns) {
        recordingStartSystemQPCMs.store(startQpcMs, std::memory_order_release);
        recordingStartSystemQpc100ns.store(startQpc100ns, std::memory_order_release);

        // Discard anything captured before the first video frame so audio starts on
        // the same timeline anchor as video, even if packets were still queued in
        // the capture objects.
        audioSyncPending.store(true);
        DiscardPendingAudioPackets();
        for (auto& src : audioSources) {
            if (src.sharedEncoderPtr) {
                src.sharedEncoderPtr->SetRecordingStart(0);
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
            src.alignedStartMs = -1;
            src.observedLateStartMs = 0;
            src.hasAlignedStart = false;
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
        }
        audioSyncPending.store(false);
    }

    // Pull Model: Track encoded samples per source for progressive encoding
    std::vector<int64_t> encodedSamplesPerSource;

    std::map<int, bool> trackWasSilent;
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

        // Setup Video (Alloc Only)
        DLL_Log("MediaEngine::Init creating VideoEncoder");
        videoEnc = std::make_unique<VideoEncoder>();
        DLL_Log("MediaEngine::Init calling VideoEncoder::Init");
        bool vRes = videoEnc->Init(config->video, 0, 0,
                                   config->video.fps,  // Resolution deferred to first frame
                                   [this](AVPacket* pkt) { this->WritePacket(pkt); });

        if (!vRes) {
            DLL_Log("MediaEngine::Init VideoEncoder init failed");
            return false;
        }
        DLL_Log("MediaEngine::Init VideoEncoder initialized OK");

        // Setup Audio Sources (supports multiple: system audio, microphone, etc.)
        DLL_Log("MediaEngine::Init audio sources count=%d", (int)config->audioSources.size());

        // Maps track number to encoder for that track
        std::map<int, AudioEncoder*> trackToEncoder;

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
                // Check if we already have an encoder for this track
                AudioEncoder* encoderForTrack = nullptr;
                auto it = trackToEncoder.find(track);
                if (it != trackToEncoder.end()) {
                    encoderForTrack = it->second;
                    DLL_Log("MediaEngine::Init Audio source %zu reusing encoder for track %d", i, track);
                } else {
                    // Create new encoder for this track
                    auto newEncoder = std::make_unique<AudioEncoder>();
                    bool aRes = newEncoder->Init(audioConfig, [this](AVPacket* pkt) { this->WritePacket(pkt); });

                    if (!aRes) {
                        DLL_Log("MediaEngine::Init Audio encoder for track %d failed", track);
                        continue;
                    }

                    // Register with VideoEncoder for stream creation
                    videoEnc->AddAudioContext(audioConfig, newEncoder->GetCodecContext(), track);

                    encoderForTrack = newEncoder.get();
                    trackToEncoder[track] = encoderForTrack;

                    // Create AudioSource to own this encoder
                    AudioSource source;
                    source.config = audioConfig;
                    source.track = track;
                    source.sourceType = audioConfig.sourceType;
                    source.encoder = std::move(newEncoder);
                    source.sharedEncoderPtr = source.encoder.get();

                    // Set video time getter for clock drift compensation
                    source.encoder->SetVideoTimeGetter([this]() { return GetVideoElapsedMs(); });

                    // Create appropriate capture type
                    if (audioConfig.sourceType == AudioConfig::AppAudio) {
                        source.appCapture = std::make_unique<AppAudioCapture>();
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
                    source.track = track;
                    source.sourceType = audioConfig.sourceType;
                    source.encoder = nullptr;  // Shared, not owned
                    source.sharedEncoderPtr = encoderForTrack;

                    // Create appropriate capture type
                    if (audioConfig.sourceType == AudioConfig::AppAudio) {
                        source.appCapture = std::make_unique<AppAudioCapture>();
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

        DLL_Log("MediaEngine::Init complete. Audio sources: %zu, unique tracks: %zu", audioSources.size(),
                trackToEncoder.size());
        return true;
    }

    int64_t GetLastVideoFenceWaitUs() const {
        if (videoEnc)
            return videoEnc->GetLastFrameFenceWaitUs();
        return 0;
    }

    bool StartRecording() {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (recording)
            return true;

        if (!videoEnc)
            return false;

        // REFRESH AUDIO CONTEXTS IN VIDEO ENCODER
        // AudioEncoder recreates its AVCodecContext on Stop(), invalidating
        // pointers held by VideoEncoder. We must clear and re-add the current valid
        // contexts.
        videoEnc->ClearAudioContexts();
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

        // Start Audio Capture and Processing Thread
        if (!audioSources.empty()) {
            // NOTE: Don't set recording start time here!
            // We defer this to the first video frame in ProcessFrameD3D11 for perfect
            // A/V sync. Audio data captured before first video frame will be
            // discarded. IMPORTANT: Prevent ringbuffer from filling/overflowing
            // before first video frame. We intentionally discard audio until the
            // first video frame establishes the timeline.
            audioSyncPending.store(true);

            // CRITICAL: Reset video clock for new recording to prevent stale
            // timestamps
            firstVideoFrameMs = 0;               // Reset for new recording
            videoElapsedMs.store(0);             // CRITICAL: Reset video clock for new recording
                                                 // to prevent stale timestamps
            recordingStartSystemQPCMs.store(0);  // CRITICAL: Reset QPC start time for new recording
            recordingStartSystemQpc100ns.store(0);
            injectTimelineState.Reset();
            d3d11TimelineState.Reset();

            // PULL MODEL: Reset audio encoding state for new recording
            encodedSamplesPerSource.clear();
            encodedSamplesPerSource.resize(audioSources.size(), 0);

            trackWasSilent.clear();
            trackBootstrapComplete.clear();
            trackFirstPullAfterBootstrap.clear();
            trackBootstrapWaitLogCounters.clear();

            // Build the track→source index map once (used every PullAndEncodeAudio call)
            cachedTrackToSources.clear();
            for (size_t i = 0; i < audioSources.size(); i++) {
                auto& src = audioSources[i];
                if (src.ringBuffer && src.sharedEncoderPtr) {
                    cachedTrackToSources[src.track].push_back(i);
                }
            }

            // Reset PullAndEncodeAudio counters for clean per-recording state
            warpCount = 0;
            dropLogCounter = 0;
            driftLogCounter = 0;
            trackSyncCheckCounters.clear();
            injectFrameLogCount = 0;
            screengrabFrameLogCount = 0;
            silenceLogCounter = 0;
            mixLogCounter = 0;

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
                src.alignedStartMs = -1;
                src.observedLateStartMs = 0;
                src.hasAlignedStart = false;
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
            }

            // Start all audio sources
            int startedCount = 0;
            for (auto& src : audioSources) {
                // Recording start will be set when first video frame arrives

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
                audioRunning = true;
                audioThread = std::thread(&MediaEngine::AudioLoop, this);
            }
        }

        recording = true;
        return true;
    }

    void StopRecording() {
        int64_t endUs = 0;
        constexpr int kStopSampleRate = 48000;
        const int64_t expectedVideoUsForStop = videoEnc ? videoEnc->GetExpectedFinalDurationUs() : 0;
        {
            std::lock_guard<std::recursive_mutex> lock(muxMutex);
            if (!recording)
                return;

            // Set recording end timestamp on all audio encoders BEFORE stopping
            // the audio thread so the final drain and flush both target the exact
            // encoded video length.
            if (videoEnc) {
                int64_t expectedDurationUs = videoEnc->GetExpectedFinalDurationUs();
                int64_t encodedDurationUs = videoEnc->GetEncodedDurationUs();
                int64_t durationUs = expectedDurationUs > 0 ? expectedDurationUs : encodedDurationUs;
                if (IsWgcCfrRecording()) {
                    const int64_t wallDurationUs =
                        std::max<int64_t>(d3d11TimelineState.lastElapsedUs, videoElapsedMs.load() * 1000);

                    DLL_Log(
                        "MediaEngine: WGC stop durations. Expected: %lld us, Encoded: %lld us, Wall: %lld us, "
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
                    // WGC CFR: use PTS-based target so audio matches video exactly.
                    // The encoder thread's drain already covers most audio; this final
                    // pull fills any remaining gap without racing (drain is done by now).
                    const int64_t wgcAudioTargetUs =
                        IsWgcCfrRecording() ? videoEnc->GetExpectedFinalDurationUs() : endUs;
                    const int64_t minEncodedBefore =
                        encodedSamplesPerSource.empty()
                            ? 0
                            : *std::min_element(encodedSamplesPerSource.begin(), encodedSamplesPerSource.end());
                    PullAndEncodeAudio(wgcAudioTargetUs, true);
                    const int64_t minEncodedAfter =
                        encodedSamplesPerSource.empty()
                            ? 0
                            : *std::min_element(encodedSamplesPerSource.begin(), encodedSamplesPerSource.end());
                    DLL_Log(
                        "[StopAudio] forceDrain: targetUs=%lld minEncodedBefore=%lld minEncodedAfter=%lld "
                        "pulled=%lld samples (%.1f ms)",
                        wgcAudioTargetUs, minEncodedBefore, minEncodedAfter, minEncodedAfter - minEncodedBefore,
                        (double)(minEncodedAfter - minEncodedBefore) / 48.0);
                }
            }

            recording = false;
        }

        // Stop audio thread first
        audioRunning = false;
        if (audioThread.joinable()) {
            audioThread.join();
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
            DLL_Log("[StopAudio] Video: expectedDuration=%lld ms (%lld samples)", expectedVideoMs,
                    expectedVideoSamples);
        }

        DLL_Log("[STOP SUMMARY] Recording finalized");
        if (videoEnc) {
            const int64_t finalVideoMs = videoEnc->GetExpectedFinalDurationUs() / 1000;
            const int64_t wallMs = videoElapsedMs.load();
            const int64_t encodedMs = videoEnc->GetEncodedDurationUs() / 1000;
            DLL_Log("[STOP SUMMARY] Video: duration=%lldms wall=%lldms encoded=%lldms pipelineLag=%lldms", finalVideoMs,
                    wallMs, encodedMs, wallMs - encodedMs);
        }
        for (size_t i = 0; i < audioSources.size() && i < encodedSamplesPerSource.size(); i++) {
            auto& src = audioSources[i];
            const bool isApp = (src.sourceType == AudioConfig::AppAudio);
            const bool neverStarted = isApp && !src.hasAlignedStart;
            if (neverStarted) {
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
                DLL_Log(
                    "[STOP AUDIO] Source %zu: track=%d encoded=%llu trim=cov:%llu lat:%llu pad:%llu qgap:%llu "
                    "ringPeak=%zu ringUnderruns=%u",
                    i, src.track, (unsigned long long)encodedSamplesPerSource[i],
                    (unsigned long long)src.coverageLossTrimSamples, (unsigned long long)src.latencyTrimSamples,
                    (unsigned long long)src.underrunPadSamples, (unsigned long long)src.packetTimelineGapSamples,
                    src.ringBufferPeakSamples, src.ringBufferUnderrunCount);
                DLL_Log(
                    "[STOP AUDIO DETAIL] Source %zu: ratePpm=%+.2f compDelta=%d sat=%d trimRate(lat=%.1f/min "
                    "boot=%.1f/min cov=%.1f/min tier2=%.1f/min retain=%.1f/min) totals(boot=%llu tier2=%llu "
                    "retain=%llu post=%llu overlap=%llu ovf=%llu)",
                    i, ratePpm, src.currentRateDelta, src.targetRateSaturated ? 1 : 0, latencyTrimPerMinute,
                    bootstrapTrimPerMinute, coverageTrimPerMinute, tier2TrimPerMinute, retainedTrimPerMinute,
                    (unsigned long long)src.bootstrapTrimSamples, (unsigned long long)src.tier2TrimSamples,
                    (unsigned long long)src.retainedNewestTrimSamples, (unsigned long long)src.postResampleTrimSamples,
                    (unsigned long long)src.packetTimelineOverlapSamples, (unsigned long long)src.overflowDropSamples);
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
            src.alignedStartMs = -1;
            src.observedLateStartMs = 0;
            src.hasAlignedStart = false;
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
                      bool isHDR, bool isShmem = false, int shmemSlot = 0) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (!videoEnc || !recording)
            return false;

        // Use CaptureEngine's steady_clock for duration to avoid Game QPC /
        // Frequency mismatch issues
        auto now = std::chrono::steady_clock::now();

        // Calculate QPC based timestamp for debugging/Legacy Start Time
        int64_t debugTimestamp = (qpcFreq > 0) ? (timestampQPC * 1000) / qpcFreq : timestampQPC;

        if (this->firstVideoFrameMs == 0) {
            this->firstVideoFrameMs = debugTimestamp;  // Store QPC-based start for logs
            this->recordingStartTime = now;
            const int64_t startQpc100ns =
                (qpcFreq > 0 && timestampQPC > 0)
                    ? static_cast<int64_t>(ce::audio::RawQpcToHundredNanoseconds(static_cast<uint64_t>(timestampQPC),
                                                                                 static_cast<uint64_t>(qpcFreq)))
                    : 0;

            DLL_Log(
                "MediaEngine: First inject frame at %lld ms (QPC: %lld) - "
                "syncing audio (StartQPC: %lld)",
                debugTimestamp, timestampQPC, debugTimestamp);

            // Use the source frame's QPC as the audio sync anchor rather than when
            // the media process happened to receive the frame.
            SyncAudioToFirstVideoFrame(debugTimestamp, startQpc100ns);
        }

        const int64_t steadyElapsedUs =
            std::chrono::duration_cast<std::chrono::microseconds>(now - this->recordingStartTime).count();
        int64_t realElapsedUs = steadyElapsedUs;
        if (config.video.useVFR) {
            realElapsedUs = ComputeSourceDrivenElapsedUs(qpcFreq, timestampQPC, steadyElapsedUs, injectTimelineState);
        } else {
            realElapsedUs = ResolveCfrTimelineElapsedUs(steadyElapsedUs, -1, injectTimelineState.lastElapsedUs);
        }

        // Maybe we want preview later? For now, recording only.
        videoEnc->SetAdapterLUID(luidLow, luidHigh);
        bool res = videoEnc->EncodeFrame((HANDLE)handle, (HANDLE)fenceHandle, fenceVal, realElapsedUs, sourcePid, width,
                                         height, format, isHDR, isShmem, shmemSlot);

        if (!res && videoEnc->WasLastFrameDeferred()) {
            return false;
        }
        if (!res) {
            return false;
        }

        const int64_t committedElapsedUs = GetCommittedVideoElapsedUs(realElapsedUs);
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

        // PULL MODEL: Encode audio for this video frame (same as screengrab mode)
        PullAndEncodeAudio(committedElapsedUs);
        return res;
    }

    bool RepeatLastFrame(int64_t timestampQPC) {
        return RepeatLastFrame(timestampQPC, -1);
    }

    bool RepeatLastFrame(int64_t timestampQPC, int64_t timelineElapsedUs) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        const bool wgcCfrRecording = IsWgcCfrRecording();
        // WGC CFR: allow audio pull during drain even when recording==false,
        // so the encoder thread can feed audio per drain frame instead of
        // relying on a single forceDrain pull at the very end.
        if (!videoEnc || firstVideoFrameMs == 0)
            return false;
        if (!recording && !wgcCfrRecording)
            return false;

        auto now = std::chrono::steady_clock::now();
        const int64_t steadyElapsedUs =
            std::chrono::duration_cast<std::chrono::microseconds>(now - this->recordingStartTime).count();

        int64_t realElapsedUs = steadyElapsedUs;
        if (config.video.useVFR) {
            realElapsedUs = ComputeSourceDrivenElapsedUs(qpcFreq, timestampQPC, steadyElapsedUs, injectTimelineState);
        } else if (wgcCfrRecording) {
            realElapsedUs = ResolveAuthoritativeCfrTimelineElapsedUs(steadyElapsedUs, timelineElapsedUs,
                                                                     d3d11TimelineState.lastElapsedUs);
        } else {
            realElapsedUs =
                ResolveCfrTimelineElapsedUs(steadyElapsedUs, timelineElapsedUs, injectTimelineState.lastElapsedUs);
        }

        bool res = videoEnc->RepeatLastFrame(realElapsedUs);
        if (!res) {
            return false;
        }

        const int64_t committedElapsedUs = GetCommittedVideoElapsedUs(realElapsedUs);
        CommitVideoElapsedUs(wgcCfrRecording ? d3d11TimelineState : injectTimelineState,
                             wgcCfrRecording ? realElapsedUs : committedElapsedUs);
        for (size_t i = 0; i < audioSources.size(); i++) {
            auto& src = audioSources[i];
            int idx = videoEnc->GetAudioStreamIndex(src.track);
            if (idx >= 0 && src.encoder) {
                src.encoder->SetStreamIndex(idx);
            }
        }

        const int64_t audioTimelineUs = wgcCfrRecording ? videoEnc->GetExpectedFinalDurationUs() : committedElapsedUs;
        PullAndEncodeAudio(audioTimelineUs);
        return true;
    }

    bool IsWgcCfrRecording() const {
        return config.captureMethod != "inject" && !config.video.useVFR;
    }

    // Direct D3D11 texture processing for screengrab mode (zero-copy)
    // Direct D3D11 texture processing for screengrab mode (zero-copy)
    bool ProcessFrameD3D11(void* texture, int64_t timestampQPC, uint32_t width, uint32_t height, bool isHDR,
                           int32_t captureLeft, int32_t captureTop, int64_t timelineElapsedUs) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (!videoEnc || !recording)
            return false;

        auto now = std::chrono::steady_clock::now();
        int64_t debugTimestamp = (qpcFreq > 0) ? (timestampQPC * 1000) / qpcFreq : timestampQPC;

        if (this->firstVideoFrameMs == 0) {
            this->firstVideoFrameMs = debugTimestamp;

            int64_t anchorQPC = timestampQPC;
            if (IsWgcCfrRecording()) {
                LARGE_INTEGER qpcNow;
                QueryPerformanceCounter(&qpcNow);
                const int64_t nowQPC = qpcNow.QuadPart;
                anchorQPC = ce::audio::ClampStartupAnchorQpc(timestampQPC, nowQPC, qpcFreq,
                                                             static_cast<uint32_t>(std::max(0, config.video.fps)));
                int64_t anchorDeltaQpc = anchorQPC - timestampQPC;
                int64_t anchorDeltaMs = (qpcFreq > 0) ? (anchorDeltaQpc * 1000) / qpcFreq : 0;
                if (anchorDeltaMs > 0) {
                    DLL_Log(
                        "MediaEngine: WGC CFR clamped startup audio anchor from %lld to %lld "
                        "(delta=%lldms)",
                        timestampQPC, anchorQPC, anchorDeltaMs);
                }
            }

            const int64_t anchorMs = (qpcFreq > 0 && anchorQPC > 0) ? (anchorQPC * 1000) / qpcFreq : debugTimestamp;
            this->recordingStartTime = now;
            const int64_t startQpc100ns = (qpcFreq > 0 && anchorQPC > 0)
                                              ? static_cast<int64_t>(ce::audio::RawQpcToHundredNanoseconds(
                                                    static_cast<uint64_t>(anchorQPC), static_cast<uint64_t>(qpcFreq)))
                                              : 0;
            // Start of recording logic
            DLL_Log(
                "MediaEngine: First D3D11 frame at %lld ms (QPC: %lld) "
                "(StartQPC: %lld)",
                debugTimestamp, timestampQPC, anchorMs);

            // Reset elapsed clock for audio sync
            videoElapsedMs.store(0);

            SyncAudioToFirstVideoFrame(anchorMs, startQpc100ns);
        }

        int64_t realElapsedUs = 0;
        const int64_t steadyElapsedUs =
            std::chrono::duration_cast<std::chrono::microseconds>(now - this->recordingStartTime).count();
        if (config.video.useVFR) {
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
        if (!videoEnc->EncodeFrameD3D11((ID3D11Texture2D*)texture, realElapsedUs, width, height, isHDR, captureLeft,
                                        captureTop)) {
            DLL_Log("MediaEngine: D3D11 frame encode failed at ts=%lld", debugTimestamp);
            return false;
        }
        const int64_t committedElapsedUs = GetCommittedVideoElapsedUs(realElapsedUs);
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

    // PULL MODEL: Pull audio from RingBuffers and encode against the relative
    // recording timeline that also drives CFR video emission.
    void PullAndEncodeAudio(int64_t videoTimelineUs, bool forceDrain = false) {
        if (!recording || audioSources.empty())
            return;

        constexpr int SAMPLE_RATE = 48000;
        constexpr int CHANNELS = 2;
        constexpr int64_t kSteadyAudioPullLatencyMs = ce::audio::kDefaultSteadyAudioPullLatencyMs;
        constexpr int64_t kPrimedSourceCushionSamples = SAMPLE_RATE / 50;  // 20ms
        constexpr int64_t kBootstrapHoldLimitMs = 500;
        constexpr int64_t kBaseTargetLatencySamples = (kSteadyAudioPullLatencyMs * SAMPLE_RATE) / 1000;
        constexpr int64_t kMaxPipelineLagContributionMs = 10000;
        constexpr int64_t kWgcCoverageLossMaxBufferedLagMs = 300;
        constexpr int64_t kRuntimeMaxLeadSamples = SAMPLE_RATE / 10;            // 100ms above target
        constexpr int64_t kRuntimeMaxDropPerCall = SAMPLE_RATE / 200;           // 5ms
        constexpr int64_t kRuntimeDropFadeSamples = SAMPLE_RATE / 100;          // 10ms
        constexpr int64_t kOverloadAudioPullQuantumSamples = SAMPLE_RATE / 50;  // 20ms
        constexpr int64_t kPacketTimelineFadeSamples = SAMPLE_RATE / 750;       // ~1.33ms
        constexpr int64_t kStartupPacketTimelineWindowSamples = (SAMPLE_RATE * 150) / 1000;
        constexpr int64_t kStartupPacketTimelineSlopSamples = SAMPLE_RATE / 250;  // 4ms
        constexpr int64_t kStartupPacketOverlapTrimThresholdSamples = SAMPLE_RATE / 200;  // 5ms
        constexpr int64_t kLatencyTrimHysteresisSamples = ce::audio::kDefaultAudioPullQuantumSamples;
        constexpr int64_t kMinCompensationBufferSamples = kBaseTargetLatencySamples / 4;
        constexpr int64_t kWgcCfrLeadWarningSamples = SAMPLE_RATE / 10;  // 100ms
        constexpr bool kWgcPreferVideoRepeatsOverAudioCuts = true;
        constexpr double kDefaultMaxCompensationPercent = 1.0;
        constexpr double kTier1MaxPitchPercent = 1.0;  // 1.0% covers ~4800 samples/10s at 48kHz
        constexpr int64_t kTier2DriftThresholdMs = 20;
        constexpr int64_t kWgcEncoderShortfallBufferedLagMaxMs = 4000;
        constexpr uint32_t kWgcEncoderHealthyDeliveryMarginFps = 4;
        constexpr int64_t kWgcCoverageLossLeadSlackSamples = SAMPLE_RATE / 25;  // 40ms above target
        constexpr int64_t kWgcCoverageLossMaxDropPerCall =
            ce::audio::kDefaultAudioPullQuantumSamples;  // 5ms paced overload trim quantum

        const bool isWgcCfrRecording = IsWgcCfrRecording();
        const int64_t wallVideoMs = this->videoElapsedMs.load();
        int64_t encodedVideoMs = 0;
        int64_t audioTargetUs = videoTimelineUs;
        if (videoEnc && !isWgcCfrRecording) {
            int64_t encodedVideoUs = videoEnc->GetEncodedDurationUs();
            if (encodedVideoUs > 0) {
                encodedVideoMs = encodedVideoUs / 1000;
                audioTargetUs = encodedVideoUs;
            }
        }
        if (isWgcCfrRecording && videoEnc) {
            int64_t encodedVideoUs = videoEnc->GetEncodedDurationUs();
            if (encodedVideoUs > 0) {
                encodedVideoMs = encodedVideoUs / 1000;
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
        uint32_t effectiveDeliveredFpsForAudioContinuity = wgcDeliveredFps;
        if (wgcDeliveredMin250Fps > 0) {
            effectiveDeliveredFpsForAudioContinuity =
                std::min(effectiveDeliveredFpsForAudioContinuity, wgcDeliveredMin250Fps);
        }
        if (wgcDeliveredMin500Fps > 0) {
            effectiveDeliveredFpsForAudioContinuity =
                std::min(effectiveDeliveredFpsForAudioContinuity, wgcDeliveredMin500Fps);
        }
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
        const int64_t effectiveWgcTargetBufferLagMs =
            isWgcCfrRecording ? ce::audio::ComputeWgcCfrTargetBufferLagMs(
                                    wgcAudioLagTargets, wgcSteadyStateBufferedAudioLagMs,
                                    kWgcPreferVideoRepeatsOverAudioCuts, wgcEncoderShortfallBufferedLagMs)
                              : videoPipelineLagMs + timelineShortfallMs;
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
        if (!forceDrain && !isWgcCfrRecording && wallVideoLagMs > 500) {
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

                trackAllPrimed = trackAllPrimed &&
                                 ce::audio::IsSourceStartupPrimed(src.isPrimed, src.hasAlignedStart, isAppAudioSource);
                trackMaxObservedLateStartMs = std::max(trackMaxObservedLateStartMs, src.observedLateStartMs);
            }

            const bool trackStartupSettled =
                ce::audio::IsTrackAudioStartupSettled(trackBootstrapComplete[track], trackAllPrimed);
            int64_t trackAudioPullLatencyMs =
                forceDrain ? 0
                           : ce::audio::ComputeAudioPullLatencyMs(kSteadyAudioPullLatencyMs, trackStartupSettled,
                                                                  trackMaxObservedLateStartMs);
            if (isWgcCfrRecording && trackStartupSettled && trackAllPrimed) {
                trackAudioPullLatencyMs = std::min<int64_t>(trackAudioPullLatencyMs, 30);
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
                    src.bootstrapComplete, src.hasAlignedStart, src.isPrimed, isAppAudioSource, bufferedRealSamples,
                    requiredBootstrapSamples);
                trackReadyForBootstrap = trackReadyForBootstrap && srcReady;
            }

            if (!trackBootstrapComplete[track]) {
                const bool forcedBootstrap = trackAudioTargetMs >= kBootstrapHoldLimitMs;
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
                    if (remainingStartupProtectionSamples <= 0) {
                        const size_t bufferedTimelineSamples = GetBufferedTimelineSamples(src);
                        const int64_t dropBeforeLive = static_cast<int64_t>(bufferedTimelineSamples) -
                                                       (std::max<int64_t>(targetSamples, 0) + targetBufferedSamples);
                        if (dropBeforeLive > 0) {
                            const size_t droppedSamples =
                                DropOldestBufferedSamples(src, static_cast<size_t>(dropBeforeLive));
                            bootstrapTrimmed += droppedSamples;
                            if (droppedSamples > 0) {
                                // Bootstrap trims can otherwise start playback mid-waveform
                                // and create an audible click/distortion in the first live audio.
                                src.pendingUnderrunRecoveryFade = true;
                            }
                        }
                    }
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
            int64_t pendingSamples = targetSamples - encodedSamplesPerSource[firstSrcIdx];
            if (pendingSamples <= 0)
                continue;

            if (ce::audio::ShouldDeferAudioPullUntilQuantum(pendingSamples, trackStartupSettled, forceDrain)) {
                continue;
            }

            int64_t samplesToEncode = pendingSamples;
            const bool initialTrackCatchup =
                trackFirstPullAfterBootstrap.count(track) && trackFirstPullAfterBootstrap[track];
            if (trackStartupSettled && !forceDrain) {
                // After bootstrap, the pull quantum rounding can leave a residual (up to 960
                // samples = 20ms) that accumulates into a constant audio-video offset. Skip
                // quantum rounding on the first pull after bootstrap to achieve exact alignment.
                if (!initialTrackCatchup) {
                    const bool overloadPullQuantum = isWgcCfrRecording && (wgcOverloadFlags & 0x1u) != 0;
                    const int64_t quantumSamples = overloadPullQuantum ? kOverloadAudioPullQuantumSamples
                                                                       : ce::audio::kDefaultAudioPullQuantumSamples;
                    samplesToEncode = (samplesToEncode / quantumSamples) * quantumSamples;
                }
                if (samplesToEncode <= 0) {
                    continue;
                }
            }
            if (initialTrackCatchup) {
                trackFirstPullAfterBootstrap[track] = false;
            }

            const int64_t MAX_GAP_SAMPLES = (SAMPLE_RATE * 2);
            const int64_t MAX_SILENCE_CHUNK = SAMPLE_RATE / 2;
            if (samplesToEncode > MAX_GAP_SAMPLES && !initialTrackCatchup) {
                warpCount++;
                DLL_Log(
                    "[PullAudio] Large A/V gap (%.2f sec) on track %d - inserting silence (warp #%d). target=%lld, "
                    "encoded=%lld",
                    (double)samplesToEncode / SAMPLE_RATE, track, warpCount, targetSamples,
                    encodedSamplesPerSource[firstSrcIdx]);

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
            }

            size_t totalFloats = samplesToEncode * CHANNELS;
            std::vector<float> mixBuffer(totalFloats, 0.0f);
            int activeSources = 0;

            for (size_t srcIdx : srcIndices) {
                auto& src = audioSources[srcIdx];

                const bool isAppAudioSource = (src.sourceType == AudioConfig::AppAudio);
                if (ce::audio::IsOptionalUnstartedAppAudioSource(isAppAudioSource, src.hasAlignedStart)) {
                    continue;
                }

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
                    if (isWgcCfrRecording) {
                        const uint64_t nowTick = GetTickCount64();
                        if (nowTick - src.lastRetainedTrimWarnTick >= 1000) {
                            const size_t rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                            const size_t rbCapacity = src.ringBuffer->GetCapacity() / CHANNELS;
                            DLL_Log(
                                "[PullAudio] WARNING: WGC CFR audio headroom exhausted - trimmed %zu oldest samples "
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
                    const double maxCompensationPercent =
                        isWgcCfrRecording ? kTier1MaxPitchPercent : kDefaultMaxCompensationPercent;
                    src.syncResampler->SetMaxCompensationPercent(maxCompensationPercent);
                    size_t rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                    const int64_t expectedLeadSamplesForCorrection =
                        std::max<int64_t>(targetLatencySamples,
                                          kBaseTargetLatencySamples + (effectiveWgcDriftLagMs * SAMPLE_RATE / 1000));
                    const bool allowWgcCoverageLossTrim =
                        isWgcCfrRecording && wgcCoverageLossActive && !kWgcPreferVideoRepeatsOverAudioCuts &&
                        static_cast<int64_t>(rbAvailable) > targetLatencySamples + kWgcCoverageLossLeadSlackSamples;
                    if (!allowWgcCoverageLossTrim) {
                        src.wgcCoverageLossTrimAccumulator = 0.0;
                    }
                    if (allowWgcCoverageLossTrim && !startupTimelineProtected) {
                        const int64_t dropSamplesTotal = static_cast<int64_t>(rbAvailable) -
                                                         (targetLatencySamples + kWgcCoverageLossLeadSlackSamples);
                        int64_t dropSamples = ce::audio::ComputeWgcCoverageLossTrimSamples(
                            samplesToEncode,
                            ce::audio::ComputeWgcCoverageLossRatio(videoPipelineLagMs, wgcBufferedVideoContentLagMs),
                            src.wgcCoverageLossTrimAccumulator, kWgcCoverageLossMaxDropPerCall);
                        dropSamples = std::min(dropSamples, dropSamplesTotal);
                        if (dropSamples > 0 && src.ringBuffer) {
                            if (src.postResampleBuffer.size() >= CHANNELS) {
                                size_t base = src.postResampleBuffer.size() - CHANNELS;
                                src.dropFadeStartL = src.postResampleBuffer[base];
                                src.dropFadeStartR = src.postResampleBuffer[base + 1];
                            } else {
                                src.dropFadeStartL = 0.0f;
                                src.dropFadeStartR = 0.0f;
                            }
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
                    } else if (isWgcCfrRecording && wgcCoverageLossActive && kWgcPreferVideoRepeatsOverAudioCuts &&
                               static_cast<int64_t>(rbAvailable) >
                                   targetLatencySamples + kWgcCoverageLossLeadSlackSamples &&
                               dropLogCounter++ % 500 == 0) {
                        DLL_Log(
                            "[PullAudio] WGC coverage loss active: preserving audio continuity and expecting host "
                            "video "
                            "repeats to absorb mismatch (src=%d ahead=%lld target=%lld slack=%lld pipelineLag=%lldms "
                            "contentLag=%lldms delivered=%u/%u fps ratio=%.3f%%)",
                            (int)srcIdx, static_cast<int64_t>(rbAvailable) - targetLatencySamples, targetLatencySamples,
                            kWgcCoverageLossLeadSlackSamples, videoPipelineLagMs, wgcBufferedVideoContentLagMs,
                            wgcDeliveredFps, wgcTargetFps,
                            ce::audio::ComputeWgcCoverageLossRatio(videoPipelineLagMs, wgcBufferedVideoContentLagMs) *
                                100.0);
                    } else if (!isWgcCfrRecording && !startupTimelineProtected) {
                        int64_t dropSamplesTotal = ce::audio::ComputeLeadTrimExcessSamples(
                            static_cast<int64_t>(rbAvailable), targetLatencySamples, kRuntimeMaxLeadSamples,
                            kLatencyTrimHysteresisSamples);
                        int64_t dropSamples = std::min(dropSamplesTotal, kRuntimeMaxDropPerCall);
                        if (dropSamples > 0 && src.ringBuffer) {
                            if (src.postResampleBuffer.size() >= CHANNELS) {
                                size_t base = src.postResampleBuffer.size() - CHANNELS;
                                src.dropFadeStartL = src.postResampleBuffer[base];
                                src.dropFadeStartR = src.postResampleBuffer[base + 1];
                            } else {
                                src.dropFadeStartL = 0.0f;
                                src.dropFadeStartR = 0.0f;
                            }
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
                            if (leadExcess > kWgcLeadHardCapSamples && src.ringBuffer) {
                                const int64_t excessAboveCap = leadExcess - kWgcLeadHardCapSamples;
                                const int64_t maxTrimThisCall =
                                    std::min(excessAboveCap, kWgcCoverageLossMaxDropPerCall);
                                if (maxTrimThisCall > 0) {
                                    if (src.postResampleBuffer.size() >= CHANNELS) {
                                        size_t base = src.postResampleBuffer.size() - CHANNELS;
                                        src.dropFadeStartL = src.postResampleBuffer[base];
                                        src.dropFadeStartR = src.postResampleBuffer[base + 1];
                                    } else {
                                        src.dropFadeStartL = 0.0f;
                                        src.dropFadeStartR = 0.0f;
                                    }
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

                    // Two-tier drift correction:
                    // Tier 1 - Inaudible micro-pitch via swr_set_compensation (max 0.05%)
                    // Tier 2 - Ring buffer sample trimming with crossfade (when drift > 20ms)
                    {
                        const int64_t rbLevel = static_cast<int64_t>(rbAvailable);
                        if (rbLevel >= kMinCompensationBufferSamples) {
                            const int64_t expectedLead = expectedLeadSamplesForCorrection;
                            const int64_t trueDrift = rbLevel - expectedLead;

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
                                    // --- Tier 1: Micro-pitch correction ---
                                    // Keep rate correction for real source drift, but do not spend buffered lead that
                                    // only exists because the authoritative CFR timeline is slightly behind wall clock.
                                    int32_t tier1Delta = 0;

                                    if (isWgcCfrRecording) {
                                        if (wgcEncoderOnlyOverload) {
                                            tier1Delta = 0;
                                            src.targetRateSaturated = false;
                                        } else {
                                            const int64_t positiveCompensationHysteresisSamples =
                                                ce::audio::ComputeWgcPositiveCompensationHysteresisSamples(
                                                    targetLatencySamples, kWgcCfrLeadWarningSamples);
                                            const bool allowSteadyStatePositiveCompensation =
                                                ce::audio::ShouldAllowWgcSteadyStateDriftCompensation(
                                                    trackStartupSettled, videoPipelineLagMs, rbLevel,
                                                    targetLatencySamples, kWgcCfrLeadWarningSamples);
                                            tier1Delta = ce::audio::ComputeTier1CompensationDelta(
                                                trueDrift, static_cast<int64_t>(SAMPLE_RATE) * 10,
                                                kTier1MaxPitchPercent);
                                            src.targetRateSaturated =
                                                std::abs(trueDrift) > src.syncResampler->GetMaxCompensationDelta();
                                            if (tier1Delta > 0) {
                                                if (ce::audio::ShouldClearWgcPositiveDriftCompensation(
                                                        allowSteadyStatePositiveCompensation, rbLevel, expectedLead,
                                                        positiveCompensationHysteresisSamples)) {
                                                    tier1Delta = 0;
                                                } else {
                                                    tier1Delta =
                                                        static_cast<int32_t>(ce::audio::ClampWgcPositiveDriftCorrection(
                                                            tier1Delta, positiveCompensationHysteresisSamples));
                                                }
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
                                    const bool wallClockAnchorActive = !isWgcCfrRecording && wallVideoLagMs > 500;
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
                                            if (src.postResampleBuffer.size() >= CHANNELS) {
                                                size_t base = src.postResampleBuffer.size() - CHANNELS;
                                                src.dropFadeStartL = src.postResampleBuffer[base];
                                                src.dropFadeStartR = src.postResampleBuffer[base + 1];
                                            } else {
                                                src.dropFadeStartL = 0.0f;
                                                src.dropFadeStartR = 0.0f;
                                            }
                                            src.dropFadeSamplesRemaining = (int)kRuntimeDropFadeSamples;

                                            size_t trimmedFloats =
                                                src.ringBuffer->Skip((size_t)tier2MaxTrim * CHANNELS);
                                            size_t trimmedSamples = trimmedFloats / CHANNELS;
                                            ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticRingSamples,
                                                                                       trimmedSamples);
                                            ce::audio::ConsumeSyntheticBufferedSamples(
                                                src.startupGapProtectionSamples, trimmedSamples);
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
                                        DLL_Log(
                                            "[PullAudio] Src %zu drift: "
                                            "trueDrift=%lld "
                                            "(rb=%lld expected=%lld "
                                            "pipelineLag=%lldms) "
                                            "tier1=%d (%.4f%%) "
                                            "tier2=%d encBottleneck=%d",
                                            srcIdx, trueDrift, rbLevel, expectedLead, effectiveWgcDriftLagMs, newDelta,
                                            compensationPercent,
                                            ce::audio::ShouldActivateTier2Trim(trueDrift, SAMPLE_RATE,
                                                                               kTier2DriftThresholdMs)
                                                ? 1
                                                : 0,
                                            wgcEncoderBottlenecked ? 1 : 0);
                                    }

                                    src.lastRateUpdateMs = nowVideoMs;
                                }
                            }
                        }
                    }

                    // Overflow protection: unconditionally cap ring buffer depth to prevent
                    // unbounded growth when the encoder is severely stalled.  The existing
                    // lead-excess and Tier2 trims are gated on encoder-bottleneck/coverage-loss
                    // conditions and are capped at 5ms/call, so they cannot keep up when the
                    // ring buffer grows to 10+ seconds.  This overflow cap discards excess
                    // samples with a crossfade whenever the buffer exceeds
                    // targetLatency + kMaxOverflowSamples (500ms).
                    if (src.ringBuffer && !startupTimelineProtected) {
                        constexpr int64_t kMaxOverflowSamples = SAMPLE_RATE / 2;  // 500ms max overflow
                        rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                        const int64_t overflowExcess =
                            static_cast<int64_t>(rbAvailable) - targetLatencySamples - kMaxOverflowSamples;
                        if (overflowExcess > kRuntimeDropFadeSamples) {
                            if (src.postResampleBuffer.size() >= CHANNELS) {
                                size_t base = src.postResampleBuffer.size() - CHANNELS;
                                src.dropFadeStartL = src.postResampleBuffer[base];
                                src.dropFadeStartR = src.postResampleBuffer[base + 1];
                            } else {
                                src.dropFadeStartL = 0.0f;
                                src.dropFadeStartR = 0.0f;
                            }
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
                                    "[PullAudio] Ring buffer overflow protection: src %d trimmed %lld samples "
                                    "(rb was %lld samples, target %lld, overflow cap %lld)",
                                    (int)srcIdx, (long long)trimmedSamples, (long long)(rbAvailable + trimmedSamples),
                                    (long long)targetLatencySamples, (long long)kMaxOverflowSamples);
                            }
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
                                        float inL = outFloats[s * CHANNELS];
                                        float inR = outFloats[s * CHANNELS + 1];
                                        outFloats[s * CHANNELS] =
                                            src.dropFadeStartL + (inL - src.dropFadeStartL) * alpha;
                                        outFloats[s * CHANNELS + 1] =
                                            src.dropFadeStartR + (inR - src.dropFadeStartR) * alpha;
                                        src.dropFadeStartL = outFloats[s * CHANNELS];
                                        src.dropFadeStartR = outFloats[s * CHANNELS + 1];
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
                                        outFloats[s * CHANNELS] *= alpha;
                                        outFloats[s * CHANNELS + 1] *= alpha;
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
                                        outFloats[s * CHANNELS] *= alpha;
                                        outFloats[s * CHANNELS + 1] *= alpha;
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
                                    DLL_Log(
                                        "[PullAudio] Sample trim stats src=%d: overflowDropped=%llu "
                                        "latencyTrimmed=%llu postResampleTrimmed=%llu",
                                        (int)srcIdx, (unsigned long long)src.overflowDropSamples,
                                        (unsigned long long)src.latencyTrimSamples,
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
                if (src.postResampleBuffer.size() > MAX_POST_RESAMPLE_FLOATS) {
                    size_t excess = src.postResampleBuffer.size() - MAX_POST_RESAMPLE_FLOATS;
                    ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticPostSamples, excess / CHANNELS);
                    ce::audio::ConsumeSyntheticBufferedSamples(src.startupGapProtectionSamples, excess / CHANNELS);
                    src.postResampleTrimSamples += excess / CHANNELS;

                    if (src.postResampleBuffer.size() >= CHANNELS) {
                        size_t fadeBase = src.postResampleBuffer.size() - CHANNELS;
                        src.dropFadeStartL = src.postResampleBuffer[fadeBase];
                        src.dropFadeStartR = src.postResampleBuffer[fadeBase + 1];
                    } else {
                        src.dropFadeStartL = 0.0f;
                        src.dropFadeStartR = 0.0f;
                    }
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
                }

                std::vector<float> srcData(totalFloats, 0.0f);
                size_t available = src.postResampleBuffer.size();
                size_t toCopy = std::min(available, totalFloats);
                const size_t syntheticCopiedSamples = std::min<uint64_t>(src.startupSyntheticPostSamples, toCopy / CHANNELS);
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
                if (toCopy < totalFloats) {
                    // Underrun: apply a short fade-out on the last real samples
                    // before the silence boundary to prevent audible clicks.
                    if (toCopy > 0) {
                        constexpr int kFadeSamples = 8;  // ~0.17ms at 48kHz
                        int realSamples = static_cast<int>(toCopy / CHANNELS);
                        int fadeStart = std::max(0, realSamples - kFadeSamples);
                        for (int s = fadeStart; s < realSamples; ++s) {
                            float alpha = static_cast<float>(realSamples - s) / static_cast<float>(kFadeSamples + 1);
                            srcData[s * CHANNELS + 0] *= alpha;
                            srcData[s * CHANNELS + 1] *= alpha;
                        }
                    }
                    size_t padSamples = (totalFloats - toCopy) / CHANNELS;
                    const bool startupPadding = !src.bootstrapComplete;
                    if (!startupPadding) {
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
                    src.pendingUnderrunRecoveryFade = !startupPadding;
                    if (startupPadding && realCopiedSamples == 0) {
                        src.pendingStartupJoinFade = true;
                    }
                    if (!startupPadding && src.alignedStartMs >= 0 && padSamples >= (size_t)(SAMPLE_RATE / 200) &&
                        dropLogCounter++ % 100 == 0) {
                        DLL_Log(
                            "[PullAudio] WARNING: Source underrun - src %d padding %zu samples with silence "
                            "(available=%zu needed=%zu)",
                            (int)srcIdx, padSamples, available / CHANNELS, totalFloats / CHANNELS);
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

            // If ALL sources for this track are silent (game pause), SKIP encoding
            // entirely. Do NOT encode silence here - the AudioEncoder has its own gap
            // handling. Do NOT advance counters - this allows us to "catch up" when
            // real audio arrives. This fixes the audio delay issue after game pauses.
            // If ALL sources for this track are silent (game pause), we MUST generate
            // silence. Otherwise, the Audio Stream timestamps stop advancing, and
            // av_interleaved_write_frame will BUFFER VIDEO PACKETS INDEFINITELY
            // waiting for audio to catch up. This causes the 32GB RAM leak.

            bool applyTransitionFade = false;

            if (activeSources == 0) {
                trackWasSilent[track] = true;
                /*
                   Logic:
                   1. Create a zeroed buffer matching totalFloats.
                   2. Encode it.
                   3. Advance counters.
                */
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
                }
                trackWasSilent[track] = false;
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
                int64_t trackPos = encodedSamplesPerSource[firstSrcIdx];
                const bool applyStartupTrackFade =
                    !applyTransitionFade && trackPos == 0 && audioSources[firstSrcIdx].packetBoundaryFadeInSamplesRemaining <= 0 &&
                    audioSources[firstSrcIdx].underrunFadeSamplesRemaining <= 0 &&
                    !audioSources[firstSrcIdx].pendingUnderrunRecoveryFade;
                const int64_t fadeSamples = applyTransitionFade ? SAMPLE_RATE / 20 : SAMPLE_RATE / 40;
                int64_t fadeStart = applyTransitionFade ? 0 : trackPos;
                if ((applyStartupTrackFade || applyTransitionFade) && fadeSamples > 0) {
                    for (int64_t s = 0; s < samplesToEncode; s++) {
                        int64_t global = fadeStart + s;
                        float gain = (global >= fadeSamples) ? 1.0f : (float)global / (float)fadeSamples;
                        size_t base = (size_t)s * CHANNELS;
                        mixBuffer[base + 0] *= gain;
                        mixBuffer[base + 1] *= gain;
                    }
                }
            }

            {
                // Always use a smooth soft-knee limiter so any residual discontinuity from
                // packet stitching, drift correction, or track transitions cannot turn into
                // a hard clipped click at the encoder input.
                constexpr float kKnee = 0.9f;
                constexpr float kRange = 1.0f - kKnee;
                constexpr float kScale = 1.0f / kRange;
                for (auto& s : mixBuffer) {
                    if (s > kKnee) {
                        float excess = (s - kKnee) * kScale;
                        s = kKnee + kRange * (excess / (1.0f + excess));
                    } else if (s < -kKnee) {
                        float excess = (-s - kKnee) * kScale;
                        s = -(kKnee + kRange * (excess / (1.0f + excess)));
                    }
                    s = std::tanh(s * 1.2f) / 1.2f;
                }
            }

            // Encode the mixed samples using first source's encoder
            AudioEncoder* encoder = audioSources[firstSrcIdx].sharedEncoderPtr;
            if (encoder) {
                std::vector<uint8_t> encodeData(totalFloats * sizeof(float));
                memcpy(encodeData.data(), mixBuffer.data(), encodeData.size());

                // Calculate timestamp for this audio chunk
                // CRITICAL: Use RELATIVE time from sample count, not absolute QPC!
                // This must match what the encoder expects - time relative to recording
                // start
                int64_t audioChunkTimestampMs = (encodedSamplesPerSource[firstSrcIdx] * 1000) / SAMPLE_RATE;

                encoder->EncodeSamples(encodeData.data(), (int)encodeData.size(), CHANNELS, SAMPLE_RATE, 32, 32,
                                       CHANNELS * 4,
                                       true,  // float32
                                       audioChunkTimestampMs);

                if (srcIndices.size() > 1 && mixLogCounter++ % 5000 == 0) {
                    DLL_Log("[PullAudio] Mixed %d sources for track %d (%lld samples)", activeSources, track,
                            samplesToEncode);
                }
            }

            // Update source counters ONLY when we actually encoded real audio
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
                if (videoEnc) {
                    int64_t encodedVideoUs = videoEnc->GetEncodedDurationUs();
                    if (encodedVideoUs > 0) {
                        encodedVideoMs = encodedVideoUs / 1000;
                        if (!isWgcCfrRecording) {
                            videoMs = encodedVideoMs;
                        }
                    }
                }
                int64_t audioSamples = encodedSamplesPerSource[firstSrcIdx];
                int64_t audioMs = (audioSamples * 1000) / SAMPLE_RATE;
                int64_t avDrift = audioMs - videoMs;
                int64_t latencyAdjustedAvDrift =
                    ce::audio::ComputeLatencyAdjustedAvDriftMs(avDrift, trackAudioPullLatencyMs);
                int64_t pipelineLagMs = ce::audio::ComputeVideoPipelineLagMs(wallVideoMs, encodedVideoMs);

                // Summarize all sources contributing to this track so issues on a
                // secondary app/system source are visible in the periodic sync log.
                uint64_t overflowDropped = 0;
                uint64_t retainedTrimmed = 0;
                uint64_t latencyTrimmed = 0;
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
                    }

                    overflowDropped += srcOverflowDropped;
                    retainedTrimmed += srcRetainedTrimmed;
                    latencyTrimmed += srcLatencyTrimmed;
                    postTrimmed += srcPostTrimmed;
                    underrunPadded += srcUnderrunPadded;
                    coverageLossTrimmed += srcCoverageLossTrimmed;
                    packetGapAdjusted += srcPacketGapAdjusted;
                    packetOverlapTrimmed += srcPacketOverlapTrimmed;

                    char sourceState[352];
                    std::snprintf(
                        sourceState, sizeof(sourceState),
                        "src%zu(rb=%zu sync=%lld start=%lld primed=%d boot=%d synth=%llu/%llu/%llu ovf=%llu "
                        "rtrim=%llu lat=%llu t2=%llu cov=%llu boottrim=%llu post=%llu pad=%llu qgap=%llu qov=%llu)",
                        srcIdx, rbLevel, syncOutput, alignedStartMs, (int)srcPrimed, (int)srcBootstrapped,
                        (unsigned long long)srcSyntheticRing, (unsigned long long)srcSyntheticInflight,
                        (unsigned long long)srcSyntheticPost, (unsigned long long)srcOverflowDropped,
                        (unsigned long long)srcRetainedTrimmed, (unsigned long long)srcLatencyTrimmed,
                        (unsigned long long)srcTier2Trimmed, (unsigned long long)srcCoverageLossTrimmed,
                        (unsigned long long)srcBootstrapTrimmed, (unsigned long long)srcPostTrimmed,
                        (unsigned long long)srcUnderrunPadded, (unsigned long long)srcPacketGapAdjusted,
                        (unsigned long long)srcPacketOverlapTrimmed);
                    if (!sourceSummary.empty()) {
                        sourceSummary += "; ";
                    }
                    sourceSummary += sourceState;
                }
                if (sourceSummary.empty()) {
                    sourceSummary = "none";
                }

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
                        DLL_Log(
                            "[PullAudio] Latency trim summary - src=%zu events=%u samples=%llu total=%llu rate=%.1f/min "
                            "target=%lld pipelineLag=%lldms",
                            srcIdx, src.pendingLatencyTrimEvents,
                            static_cast<unsigned long long>(src.pendingLatencyTrimSamples),
                            static_cast<unsigned long long>(src.latencyTrimSamples), trimRatePerMinute,
                            targetBufferedSamples,
                            pipelineLagMs);
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
                    "PipelineLag=%lld ms, "
                    "ContentLag=%lld ms, CovMode=%d, EncBottleneck=%d, Delivered=%u/%u, Over=0x%x, "
                    "TargetBuf=%lld ms, Overflow=%llu, "
                    "RetainTrim=%llu, CoverageTrim=%llu, LatencyTrim=%llu, PostTrim=%llu, Pad=%llu, QpcGap=%llu, "
                    "QpcOverlap=%llu, Sources=%s",
                    track, videoMs, audioMs, avDrift, latencyAdjustedAvDrift, trackAudioPullLatencyMs, wallVideoMs,
                    encodedVideoMs, pipelineLagMs, isWgcCfrRecording ? wgcBufferedVideoContentLagMs : pipelineLagMs,
                    wgcCoverageLossActive ? 1 : 0, wgcEncoderBottlenecked ? 1 : 0, wgcDeliveredFps, wgcTargetFps,
                    wgcOverloadFlags, (targetBufferedSamples * 1000) / SAMPLE_RATE, (unsigned long long)overflowDropped,
                    (unsigned long long)retainedTrimmed, (unsigned long long)coverageLossTrimmed,
                    (unsigned long long)latencyTrimmed, (unsigned long long)postTrimmed,
                    (unsigned long long)underrunPadded, (unsigned long long)packetGapAdjusted,
                    (unsigned long long)packetOverlapTrimmed, sourceSummary.c_str());

                DLL_Log(
                    "[A/V SYNC SUMMARY] Track %d: Wall=%lldms EncV=%lldms Audio=%lldms Drift=%+lldms "
                    "PipelineLag=%lldms PullLatency=%lldms EncBot=%d Delivered=%u/%u",
                    track, wallVideoMs, encodedVideoMs, audioMs, avDrift, pipelineLagMs, trackAudioPullLatencyMs,
                    wgcEncoderBottlenecked ? 1 : 0, wgcDeliveredFps, wgcTargetFps);

                if (std::abs(latencyAdjustedAvDrift) > 100) {
                    DLL_Log(
                        "[A/V SYNC WARNING] Track %d adjusted drift exceeds 100ms! raw=%lldms adjusted=%lldms "
                        "pull=%lldms",
                        track, avDrift, latencyAdjustedAvDrift, trackAudioPullLatencyMs);
                }
            }
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
        if (videoEnc) {
            videoEnc->WriteFrame(pkt);
        }
    }

    void ReloadConfig(const AppConfig* newConfig) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        DLL_Log("MediaEngine::ReloadConfig called");

        // Update config
        this->config = *newConfig;

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
                    bool aRes = newEncoder->Init(audioConfig, [this](AVPacket* pkt) { this->WritePacket(pkt); });

                    if (!aRes) {
                        DLL_Log("MediaEngine::ReloadConfig Audio encoder for track %d failed", track);
                        continue;
                    }

                    videoEnc->AddAudioContext(audioConfig, newEncoder->GetCodecContext(), track);

                    encoderForTrack = newEncoder.get();
                    trackToEncoder[track] = encoderForTrack;

                    AudioSource source;
                    source.config = audioConfig;
                    source.track = track;
                    source.sourceType = audioConfig.sourceType;
                    source.encoder = std::move(newEncoder);
                    source.sharedEncoderPtr = source.encoder.get();

                    // Create appropriate capture type
                    if (audioConfig.sourceType == AudioConfig::AppAudio) {
                        source.appCapture = std::make_unique<AppAudioCapture>();
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
                    source.config = audioConfig;
                    source.track = track;
                    source.sourceType = audioConfig.sourceType;
                    source.encoder = nullptr;
                    source.sharedEncoderPtr = encoderForTrack;

                    // Create appropriate capture type
                    if (audioConfig.sourceType == AudioConfig::AppAudio) {
                        source.appCapture = std::make_unique<AppAudioCapture>();
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

        DLL_Log(
            "MediaEngine: ReloadConfig complete. Audio sources: %zu, unique "
            "tracks: %zu",
            audioSources.size(), trackToEncoder.size());
    }

private:
    // Shared initialization for ring buffer and sync resampler on an AudioSource.
    // Parses sample rate from config (defaults to 48000) and sets up both.
    void InitAudioSourceBuffers(AudioSource& source, const AudioConfig& audioConfig, size_t sourceIdx) {
        constexpr int kMixerSampleRate = 48000;
        constexpr size_t kDefaultAudioRingBufferSeconds = 8;
        // Heavy WGC CFR overload runs can fall tens of seconds behind real time even
        // while audio/video file durations still stay mathematically equal. Give WGC
        // enough retention headroom to avoid destructive oldest-audio trims in those
        // runs so we preserve pitch and avoid crackle while diagnostics report the
        // underlying encoder shortfall honestly.
        constexpr size_t kWgcCfrAudioRingBufferSeconds = 30;
        const bool isWgcCfrPath = config.captureMethod != "inject" && !config.video.useVFR;
        const size_t ringBufferSeconds = isWgcCfrPath ? kWgcCfrAudioRingBufferSeconds : kDefaultAudioRingBufferSeconds;
        size_t capacity = static_cast<size_t>(kMixerSampleRate) * ringBufferSeconds * 2;
        source.ringBuffer = std::make_unique<AudioRingBuffer>(capacity);
        DLL_Log("MediaEngine::Init RingBuffer created for source %zu. Cap=%zu samples (%zus), rate=%d", sourceIdx,
                capacity, ringBufferSeconds, kMixerSampleRate);

        source.syncResampler = std::make_unique<AudioResampler>();
        AudioResampler::InputFormat syncInFmt;
        syncInFmt.sampleRate = kMixerSampleRate;
        syncInFmt.channels = 2;
        syncInFmt.bitsPerSample = 32;
        syncInFmt.validBitsPerSample = 32;
        syncInFmt.isFloat = true;
        syncInFmt.blockAlign = 8;
        AudioResampler::OutputFormat syncOutFmt;
        syncOutFmt.sampleRate = kMixerSampleRate;
        syncOutFmt.channels = 2;
        syncOutFmt.sampleFmt = AV_SAMPLE_FMT_FLT;
        source.syncResampler->Init(syncInFmt, syncOutFmt);
        DLL_Log("MediaEngine::Init SyncResampler created for source %zu (rate=%d)", sourceIdx, kMixerSampleRate);
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

        bool needsMixing = false;
        for (auto& kv : trackSourceCount) {
            if (kv.second > 1) {
                needsMixing = true;
                DLL_Log("AudioLoop: Track %d has %d sources - REAL mixing enabled", kv.first, kv.second);
            }
        }

        // Per-source sample buffers for mixing (float32 samples, interleaved
        // stereo)
        const int MIX_CHUNK_SAMPLES = 480;  // 10ms at 48kHz
        const int CHANNELS = 2;
        const int CHUNK_SIZE = MIX_CHUNK_SAMPLES * CHANNELS;

        std::vector<int64_t> sourceTimestamps(audioSources.size(), 0);
        std::vector<bool> sourceLoggedPreStartDrop(audioSources.size(), false);
        std::map<int, int64_t> trackNextTimestamp;  // Track continuous timestamps for mixing
        std::vector<AudioPacket> sourceLastPackets(audioSources.size());
        std::vector<std::chrono::steady_clock::time_point> lastPacketTime(audioSources.size(),
                                                                          std::chrono::steady_clock::now());
        int64_t lastSeenStartQPC = 0;  // Detect recording session changes

        while (audioRunning) {
            bool gotAnyPacket = false;
            auto now = std::chrono::steady_clock::now();

            // Detect new recording session (StartRecording called again) and
            // reset per-source timestamps so first-packet silence padding fires.
            {
                int64_t currentStartQPC = recordingStartSystemQPCMs.load();
                if (currentStartQPC != lastSeenStartQPC) {
                    lastSeenStartQPC = currentStartQPC;
                    std::fill(sourceTimestamps.begin(), sourceTimestamps.end(), 0);
                    std::fill(sourceLoggedPreStartDrop.begin(), sourceLoggedPreStartDrop.end(), false);
                    for (auto& src : audioSources) {
                        src.alignedStartMs = -1;
                        src.observedLateStartMs = 0;
                        src.hasAlignedStart = false;
                        src.isPrimed = false;
                        src.bootstrapComplete = false;
                        src.pendingUnderrunRecoveryFade = false;
                        src.underrunFadeSamplesRemaining = 0;
                        src.packetBoundaryFadeInSamplesRemaining = 0;
                        src.startupSyntheticRingSamples = 0;
                        src.startupSyntheticResamplerSamples = 0;
                        src.startupSyntheticPostSamples = 0;
                        src.startupGapProtectionSamples = 0;
                        src.qpcAlignedWrittenSamples = 0;
                        src.packetTimelineGapSamples = 0;
                        src.packetTimelineOverlapSamples = 0;
                        src.startupRebasedGapSamples = 0;
                        src.startupRealAudioSeen = false;
                        src.pendingStartupJoinFade = false;
                        src.bootstrapTrimSamples = 0;
                        src.lastPacketTimelineAdjustWarnTick = 0;
                    }
                }
            }

            // Step 1: Poll all sources and accumulate samples into buffers
            for (size_t srcIdx = 0; srcIdx < audioSources.size(); srcIdx++) {
                auto& src = audioSources[srcIdx];
                if (!src.sharedEncoderPtr)
                    continue;

                // Skip if no capture source available
                if (!src.capture && !src.appCapture)
                    continue;

                AudioPacket packet;
                bool gotPacket = false;

                // Poll from appropriate capture type
                if (src.appCapture) {
                    gotPacket = src.appCapture->GetNextPacket(packet);
                } else if (src.capture) {
                    gotPacket = src.capture->GetNextPacket(packet);
                }

                if (gotPacket && !packet.data.empty()) {
                    int64_t startQPC = recordingStartSystemQPCMs.load(std::memory_order_acquire);
                    if (startQPC != 0 && sourceTimestamps[srcIdx] == 0 && packet.timestamp < (startQPC - 5)) {
                        if (!sourceLoggedPreStartDrop[srcIdx]) {
                            DLL_Log("[AudioLoop] Discarding pre-start packet src=%d packet=%lld start=%lld",
                                    (int)srcIdx, packet.timestamp, startQPC);
                            sourceLoggedPreStartDrop[srcIdx] = true;
                        }
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

                    // Use AudioResampler to standardize all sources to 48kHz Float Stereo
                    // This creates a unified timeline for the mixer
                    if (!src.resampler) {
                        src.resampler = std::make_unique<AudioResampler>();
                    }

                    // Define target format for mixing (48kHz, Stereo, Float)
                    AudioResampler::OutputFormat targetFmt;
                    targetFmt.sampleRate = 48000;
                    targetFmt.channels = 2;
                    targetFmt.sampleFmt = AV_SAMPLE_FMT_FLT;  // Packed float (interleaved)

                    // Define input format from packet
                    AudioResampler::InputFormat inputFmt;
                    inputFmt.sampleRate = packet.sampleRate;
                    inputFmt.channels = packet.channels;
                    inputFmt.bitsPerSample = packet.bitsPerSample;
                    inputFmt.validBitsPerSample =
                        packet.validBitsPerSample > 0 ? packet.validBitsPerSample : packet.bitsPerSample;
                    inputFmt.isFloat = packet.isFloat;
                    inputFmt.blockAlign = (packet.channels * packet.bitsPerSample) / 8;

                    // Initialize/Reinitialize resampler if needed (check all input format fields)
                    bool needReinit = !src.resampler->IsReady();
                    if (!needReinit) {
                        const auto& cur = src.resampler->GetInputFormat();
                        needReinit = (cur.sampleRate != inputFmt.sampleRate || cur.channels != inputFmt.channels ||
                                      cur.bitsPerSample != inputFmt.bitsPerSample || cur.isFloat != inputFmt.isFloat);
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
                                    if (diff < -5) {
                                        DLL_Log("[AudioLoop] First packet leads recording start by %lld ms for src=%d",
                                                diff, (int)srcIdx);
                                    }
                                }

                                const bool firstTimelinePacket = (sourceTimestamps[srcIdx] == 0);
                                sourceTimestamps[srcIdx] = packet.timestamp;
                                float* writeFloats = (float*)resampledData[0];
                                size_t writeSamples = static_cast<size_t>(outSamples);
                                const int64_t startQpc100ns =
                                    recordingStartSystemQpc100ns.load(std::memory_order_acquire);
                                if (startQpc100ns > 0 && packet.qpcPosition >= static_cast<uint64_t>(startQpc100ns)) {
                                    const uint64_t packetStartDelta100ns =
                                        packet.qpcPosition - static_cast<uint64_t>(startQpc100ns);
                                    int64_t packetStartSamples = static_cast<int64_t>(
                                        ce::audio::HundredNanosecondsToSamples(packetStartDelta100ns,
                                                                               targetFmt.sampleRate));
                                    packetStartSamples = ce::audio::ApplyStartupPacketTimelineRebaseOffset(
                                        packetStartSamples, static_cast<int64_t>(src.startupRebasedGapSamples));
                                    if (firstTimelinePacket) {
                                        // Keep only one small startup quantum of preserved silence so the
                                        // first bootstrap pull can already include some real audio.
                                        constexpr int64_t kStartupFirstPacketGapCapSamples =
                                            ce::audio::kDefaultAudioPullQuantumSamples;
                                        constexpr int64_t kStartupFirstPacketRebaseThresholdSamples = 2400;
                                        const int64_t rebaseOffset = ce::audio::ComputeStartupFirstPacketRebaseOffset(
                                            packetStartSamples, src.sawSyncPendingPackets, kStartupFirstPacketGapCapSamples,
                                            kStartupFirstPacketRebaseThresholdSamples);
                                        if (rebaseOffset > 0) {
                                            packetStartSamples -= rebaseOffset;
                                            src.startupRebasedGapSamples += static_cast<uint64_t>(rebaseOffset);
                                            DLL_Log(
                                                "[AudioLoop] Startup rebase src=%d suppressed %lld samples of first-packet gap "
                                                "(packetStart=%lld cap=%lld)",
                                                (int)srcIdx, (long long)rebaseOffset,
                                                (long long)(packetStartSamples + rebaseOffset),
                                                (long long)kStartupFirstPacketGapCapSamples);
                                        }
                                        src.sawSyncPendingPackets = false;
                                    }
                                    const auto timelineAdjustment = ce::audio::ComputeStartupAwarePacketTimelineAdjustment(
                                        packetStartSamples, static_cast<int64_t>(src.qpcAlignedWrittenSamples),
                                        targetFmt.sampleRate / 1000, (targetFmt.sampleRate * 150) / 1000,
                                        targetFmt.sampleRate / 250, targetFmt.sampleRate / 200);
                                    const size_t packetTimelineFadeSamples =
                                        static_cast<size_t>(std::max<int64_t>(1, targetFmt.sampleRate / 750));
                                    if (timelineAdjustment.gapSamples > 0) {
                                        const size_t gapSamples = static_cast<size_t>(timelineAdjustment.gapSamples);
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

            if (!gotAnyPacket) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        DLL_Log(
            "MediaEngine: Audio thread stopped, processed %d packets, %d mixed "
            "chunks",
            packetCount, mixCount);
    }
};

static std::unique_ptr<MediaEngine> g_Engine;
static std::recursive_mutex g_EngineApiMutex;

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
    if (!g_Engine)
        g_Engine = std::make_unique<MediaEngine>();
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
                                              bool isShmem, int shmemSlot) {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine) {
        return g_Engine->ProcessFrame(textureHandle, fenceHandle, fenceValue, timestamp, luidLow, luidHigh, sourcePid,
                                      width, height, format, isHDR, isShmem, shmemSlot);
    }
    return false;
}

MEDIAENGINE_API bool MediaEngine_RepeatLastFrame(int64_t timestamp) {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine) {
        return g_Engine->RepeatLastFrame(timestamp);
    }
    return false;
}

MEDIAENGINE_API bool MediaEngine_RepeatLastFrameWithTimeline(int64_t timestamp, int64_t timelineElapsedUs) {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine) {
        return g_Engine->RepeatLastFrame(timestamp, timelineElapsedUs);
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

MEDIAENGINE_API bool MediaEngine_ProcessFrameD3D11(void* texture, int64_t timestamp, uint32_t width, uint32_t height,
                                                   bool isHDR, int32_t captureLeft, int32_t captureTop,
                                                   int64_t timelineElapsedUs) {
    std::lock_guard<std::recursive_mutex> apiLock(g_EngineApiMutex);
    if (g_Engine) {
        return g_Engine->ProcessFrameD3D11(texture, timestamp, width, height, isHDR, captureLeft, captureTop,
                                           timelineElapsedUs);
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

}  // extern "C"
