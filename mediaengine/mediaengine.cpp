#include "mediaengine.h"
#include "../common/logging.h"
#include "app_audio_capture.h"
#include "audio_capture.h"
#include "audio_encoder.h"

#include <chrono>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include "audio_resampler.h"
#include "audio_ring_buffer.h"  // Pull Model Buffer
#include "video_encoder.h"

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

        AudioConfig config;
        int track = 0;  // Target track number
        AudioConfig::SourceType sourceType = AudioConfig::SystemAudio;
    };

    // Per-track encoder with optional mixing (when multiple sources target same
    // track)

    // Member variables
    std::unique_ptr<VideoEncoder> videoEnc;
    std::vector<AudioSource> audioSources;

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

    // Get current video elapsed time for audio clock compensation
    int64_t GetVideoElapsedMs() const {
        return videoElapsedMs.load();
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

    void SyncAudioToFirstVideoFrame(int64_t startQpcMs) {
        recordingStartSystemQPCMs.store(startQpcMs, std::memory_order_release);

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
        }
        audioSyncPending.store(false);
    }

    // Pull Model: Track encoded samples per source for progressive encoding
    std::vector<int64_t> encodedSamplesPerSource;

    std::map<int, bool> trackWasSilent;

    // Cached track→source index map, built once in StartRecording to avoid
    // per-frame reconstruction (~120 rebuilds/sec at 120fps).
    std::map<int, std::vector<size_t>> cachedTrackToSources;

    // PullAndEncodeAudio counters — reset per recording to avoid stale state
    int warpCount = 0;
    int dropLogCounter = 0;
    int driftLogCounter = 0;
    int syncCheckCounter = 0;

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

            // PULL MODEL: Reset audio encoding state for new recording
            encodedSamplesPerSource.clear();
            encodedSamplesPerSource.resize(audioSources.size(), 0);

            trackWasSilent.clear();

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
            syncCheckCounter = 0;
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
        {
            std::lock_guard<std::recursive_mutex> lock(muxMutex);
            if (!recording)
                return;
            recording = false;
        }

        // Set recording end timestamp on all audio encoders BEFORE stopping audio
        // thread This ensures audio is trimmed to match the last video frame
        // exactly NEW: Use exact encoded video duration for sample-perfect sync
        if (videoEnc) {
            int64_t durationUs = videoEnc->GetExpectedFinalDurationUs();
            if (durationUs <= 0)
                durationUs = videoEnc->GetEncodedDurationUs();
            int64_t endUs = durationUs;

            DLL_Log(
                "MediaEngine: Setting audio end time. Start: %lld us, Dur: %lld "
                "us, End: %lld us",
                0LL, durationUs, endUs);

            for (auto& src : audioSources) {
                if (src.sharedEncoderPtr) {
                    src.sharedEncoderPtr->SetRecordingEndUs(endUs);
                }
            }
        }

        // Stop audio thread first
        audioRunning = false;
        if (audioThread.joinable()) {
            audioThread.join();
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
        }

        // Reset video frame tracking for next recording
        firstVideoFrameMs = 0;
        lastVideoFrameMs = 0;

        // Note: We don't need to update VideoEncoder audio context here anymore
        // since we're using AddAudioContext and the contexts are stored per-source

        // Stop video encoder (writes trailer)
        if (videoEnc) {
            videoEnc->Stop();
        }
    }

    void ProcessFrame(uint64_t handle, uint64_t fenceHandle, uint64_t fenceVal, int64_t timestampQPC, int32_t luidLow,
                      int32_t luidHigh, uint32_t sourcePid, uint32_t width, uint32_t height, uint32_t format,
                      bool isHDR, bool isShmem = false, int shmemSlot = 0) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (!videoEnc || !recording)
            return;

        // Use CaptureEngine's steady_clock for duration to avoid Game QPC /
        // Frequency mismatch issues
        auto now = std::chrono::steady_clock::now();

        // Calculate QPC based timestamp for debugging/Legacy Start Time
        int64_t debugTimestamp = (qpcFreq > 0) ? (timestampQPC * 1000) / qpcFreq : timestampQPC;

        if (this->firstVideoFrameMs == 0) {
            this->firstVideoFrameMs = debugTimestamp;  // Store QPC-based start for logs
            this->recordingStartTime = now;

            DLL_Log(
                "MediaEngine: First inject frame at %lld ms (QPC: %lld) - "
                "syncing audio (StartQPC: %lld)",
                debugTimestamp, timestampQPC, debugTimestamp);

            // Use the source frame's QPC as the audio sync anchor rather than when
            // the media process happened to receive the frame.
            SyncAudioToFirstVideoFrame(debugTimestamp);
        }

        // Calculate Real Elapsed Time (microseconds for precise PTS)
        int64_t realElapsedUs =
            std::chrono::duration_cast<std::chrono::microseconds>(now - this->recordingStartTime).count();

        // Update Atomic Member (for Audio Thread / Pull) — keep ms for audio sync
        this->videoElapsedMs.store(realElapsedUs / 1000);

        // Maybe we want preview later? For now, recording only.
        videoEnc->SetAdapterLUID(luidLow, luidHigh);
        bool res = videoEnc->EncodeFrame((HANDLE)handle, (HANDLE)fenceHandle, fenceVal, realElapsedUs, sourcePid, width,
                                         height, format, isHDR, isShmem, shmemSlot);
        (void)res;  // Avoid unused warn if not logging success here

        // Track last video frame timestamp for audio trimming
        lastVideoFrameMs = realElapsedUs / 1000;

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
        PullAndEncodeAudio(debugTimestamp);
    }

    // Direct D3D11 texture processing for screengrab mode (zero-copy)
    // Direct D3D11 texture processing for screengrab mode (zero-copy)
    void ProcessFrameD3D11(void* texture, int64_t timestampQPC, uint32_t width, uint32_t height) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (!videoEnc || !recording)
            return;

        // Use CaptureEngine's steady_clock for duration to avoid Game QPC /
        // Frequency mismatch issues
        auto now = std::chrono::steady_clock::now();
        int64_t debugTimestamp = (qpcFreq > 0) ? (timestampQPC * 1000) / qpcFreq : timestampQPC;

        if (this->firstVideoFrameMs == 0) {
            this->firstVideoFrameMs = debugTimestamp;
            this->recordingStartTime = now;

            // Start of recording logic
            DLL_Log(
                "MediaEngine: First D3D11 frame at %lld ms (QPC: %lld) "
                "(StartQPC: %lld)",
                debugTimestamp, timestampQPC, debugTimestamp);

            // Reset elapsed clock for audio sync
            videoElapsedMs.store(0);

            SyncAudioToFirstVideoFrame(debugTimestamp);
        }

        // Calculate Real Elapsed Time (microseconds for precise PTS)
        int64_t realElapsedUs =
            std::chrono::duration_cast<std::chrono::microseconds>(now - this->recordingStartTime).count();

        // Update Atomic Member (for Audio Thread / Pull) — keep ms for audio sync
        this->videoElapsedMs.store(realElapsedUs / 1000);

        videoEnc->EncodeFrameD3D11((ID3D11Texture2D*)texture, realElapsedUs, width, height);

        // Track last video frame timestamp for audio trimming
        lastVideoFrameMs = realElapsedUs / 1000;

        // Update audio stream index for all sources
        for (size_t i = 0; i < audioSources.size(); i++) {
            auto& src = audioSources[i];
            int idx = videoEnc->GetAudioStreamIndex(src.track);
            if (idx >= 0 && src.encoder) {
                src.encoder->SetStreamIndex(idx);
            }
        }

        if (screengrabFrameLogCount++ % 60 == 0) {
            DLL_Log("MediaEngine: ScreenGrab frame ts=%lld %dx%d", debugTimestamp, width, height);
        }

        // PULL MODEL: Encode audio for this video frame
        PullAndEncodeAudio(debugTimestamp);
    }

    // PULL MODEL: Pull audio from RingBuffers and encode based on Video PTS
    // This version properly groups sources by track and mixes before encoding
    void PullAndEncodeAudio(int64_t videoTimestampMs) {
        if (!recording || audioSources.empty())
            return;

        // Drive audio from authoritative encoded video timeline once available.
        // This prevents long-run drift when effective delivered frame cadence
        // differs from nominal FPS (e.g. 112fps delivered at 120fps target).
        int64_t audioTargetMs = videoTimestampMs;
        if (videoEnc) {
            int64_t encodedVideoUs = videoEnc->GetEncodedDurationUs();
            if (encodedVideoUs > 0) {
                audioTargetMs = encodedVideoUs / 1000;
            }
        }
        if (audioTargetMs <= 0) {
            audioTargetMs = this->videoElapsedMs.load();
        }

        // Apply a latency offset to allow audio capture to buffer.
        // WASAPI loopback has ~20-50ms latency. If we pull exactly up to the video time,
        // the audio hasn't arrived yet, causing silence padding and subsequent dropping.
        const int64_t AUDIO_PULL_LATENCY_MS = 50;
        audioTargetMs -= AUDIO_PULL_LATENCY_MS;

        // Safety: if video elapsed time is somehow negative or 0, skip
        if (audioTargetMs <= 0)
            return;

        // 48kHz stereo, calculate total samples needed up to this point
        const int SAMPLE_RATE = 48000;
        const int CHANNELS = 2;
        int64_t targetSamples = (audioTargetMs * SAMPLE_RATE) / 1000;

        // Ensure tracking vector is sized correctly
        if (encodedSamplesPerSource.size() != audioSources.size()) {
            encodedSamplesPerSource.resize(audioSources.size(), 0);
        }

        // Use the pre-built track→source map (built once in StartRecording)
        const auto& trackToSources = cachedTrackToSources;

        // For each track, pull audio from all sources, mix, then encode
        for (const auto& kv : trackToSources) {
            int track = kv.first;
            const auto& srcIndices = kv.second;

            if (srcIndices.empty())
                continue;

            // Calculate samples to encode based on first source's progress
            size_t firstSrcIdx = srcIndices[0];
            int64_t samplesToEncode = targetSamples - encodedSamplesPerSource[firstSrcIdx];

            if (samplesToEncode <= 0)
                continue;  // Already caught up

            // SAFETY CAP: If gap is too large (> 2 seconds), insert silence to
            // maintain timeline integrity. Previously we warped counters forward
            // without encoding, which created permanent A/V offset. Now we encode
            // actual silence for the gap, preserving 1:1 relationship between
            // encodedSamplesPerSource and actual encoded audio data.
            const int64_t MAX_GAP_SAMPLES = (SAMPLE_RATE * 2);  // 2 seconds
            const int64_t MAX_SILENCE_CHUNK = SAMPLE_RATE / 2;  // Encode at most 500ms of silence per call

            if (samplesToEncode > MAX_GAP_SAMPLES) {
                warpCount++;
                DLL_Log(
                    "[PullAudio] Large A/V gap (%.2f sec) on track %d - "
                    "inserting silence (warp #%d). "
                    "target=%lld, encoded=%lld",
                    (double)samplesToEncode / SAMPLE_RATE, track, warpCount, targetSamples,
                    encodedSamplesPerSource[firstSrcIdx]);

                // Flush ring buffers to prevent overflow while we catch up
                for (size_t srcIdx : srcIndices) {
                    if (audioSources[srcIdx].ringBuffer) {
                        audioSources[srcIdx].ringBuffer->Skip(audioSources[srcIdx].ringBuffer->GetAvailable());
                    }
                }

                // Cap how much silence we encode per call to avoid blocking
                // We'll catch up over multiple calls
                samplesToEncode = std::min(samplesToEncode, MAX_SILENCE_CHUNK);
            }

            size_t totalFloats = samplesToEncode * CHANNELS;

            // Prepare mix buffer (initialized to zero for summing)
            std::vector<float> mixBuffer(totalFloats, 0.0f);
            int activeSources = 0;

            // Pull from each source with drift compensation and sum into mix buffer
            for (size_t srcIdx : srcIndices) {
                auto& src = audioSources[srcIdx];

                // Check for dropped samples (ring buffer overflow)
                size_t droppedFloats = src.ringBuffer->GetAndClearDroppedSamples();
                if (droppedFloats > 0) {
                    size_t droppedSamples = droppedFloats / CHANNELS;
                    DLL_Log(
                        "[PullAudio] WARNING: Ring buffer overflow - %zu samples "
                        "dropped for src=%d",
                        droppedSamples, (int)srcIdx);
                    // Compensate: advance syncSamplesOutput so the PI controller
                    // doesn't interpret lost samples as drift requiring correction.
                    // Without this, the controller sees fewer output samples than
                    // expected and overcorrects the resampling ratio.
                    src.syncSamplesOutput += (int64_t)droppedSamples;
                }

                // =====================================================
                // DRIFT COMPENSATION via syncResampler
                // =====================================================
                // Target latency: ~20ms (960 samples @ 48kHz)
                const int64_t TARGET_LATENCY_SAMPLES = 960;

                if (src.syncResampler && src.syncResampler->IsReady()) {
                    size_t rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;  // Samples per channel

                    // HARD LATENCY CAP: This is an emergency guard, not a normal control
                    // loop. WASAPI and process loopback can legitimately arrive tens of
                    // milliseconds late/early relative to video, so trimming at ~60ms total
                    // lead creates audible crackle long before it prevents a real runaway.
                    // Only trim when the source is far ahead, and do it gently.
                    const int64_t MAX_LEAD_SAMPLES = SAMPLE_RATE / 10;                           // 100ms
                    const int64_t MAX_DROP_PER_CALL = SAMPLE_RATE / 200;                         // 5ms
                    const int64_t DROP_FADE_SAMPLES = SAMPLE_RATE / 200;                         // 5ms
                    const int64_t MIN_COMPENSATION_BUFFER_SAMPLES = TARGET_LATENCY_SAMPLES / 4;  // 5ms
                    if ((int64_t)rbAvailable > TARGET_LATENCY_SAMPLES + MAX_LEAD_SAMPLES) {
                        int64_t dropSamplesTotal = (int64_t)rbAvailable - (TARGET_LATENCY_SAMPLES + MAX_LEAD_SAMPLES);
                        int64_t dropSamples = std::min(dropSamplesTotal, MAX_DROP_PER_CALL);
                        if (dropSamples > 0 && src.ringBuffer) {
                            if (src.postResampleBuffer.size() >= CHANNELS) {
                                size_t base = src.postResampleBuffer.size() - CHANNELS;
                                src.dropFadeStartL = src.postResampleBuffer[base];
                                src.dropFadeStartR = src.postResampleBuffer[base + 1];
                            } else {
                                src.dropFadeStartL = 0.0f;
                                src.dropFadeStartR = 0.0f;
                            }
                            src.dropFadeSamplesRemaining = (int)DROP_FADE_SAMPLES;

                            src.ringBuffer->Skip((size_t)dropSamples * CHANNELS);
                            if (dropLogCounter++ % 500 == 0) {
                                DLL_Log(
                                    "[PullAudio] Audio latency cap: src %d ahead by %lld samples - "
                                    "trimming %lld (capped from %lld)",
                                    (int)srcIdx, (int64_t)rbAvailable - TARGET_LATENCY_SAMPLES, dropSamples,
                                    dropSamplesTotal);
                            }
                            rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                        }
                    }

                    if ((int64_t)rbAvailable >= MIN_COMPENSATION_BUFFER_SAMPLES) {
                        int64_t rbError = (int64_t)rbAvailable - TARGET_LATENCY_SAMPLES;

                        // Connect ring buffer level error to the drift compensator.
                        // swr_set_compensation(delta): delta > 0 adds output (slower drain),
                        //                              delta < 0 removes output (faster drain).
                        // AdjustForClockDrift negates the PI output before passing to swr, so
                        // a positive PI correction (driftSamples > 0) correctly produces a
                        // negative swr delta (removes samples = faster drain).
                        //
                        // We map rbError directly to driftSamples:
                        //   driftSamples = syncSamplesOutput - expectedSamples
                        //   => expectedSamples = syncSamplesOutput - rbError
                        //   => fakeExpectedSamples = syncSamplesOutput - rbError

                        int64_t fakeExpectedSamples = src.syncSamplesOutput - rbError;

                        // Convert back to "Video Time" for the API signature
                        // expected = (time * rate) / 1000
                        // time = (expected * 1000) / rate
                        int64_t correctedVideoTimeMs = (fakeExpectedSamples * 1000) / SAMPLE_RATE;

                        // Ensure we don't pass negative time if buffer is empty at start
                        if (correctedVideoTimeMs < 0)
                            correctedVideoTimeMs = 0;

                        src.syncResampler->AdjustForClockDrift(correctedVideoTimeMs, src.syncSamplesOutput);

                        // Debug log occasionally
                        if (driftLogCounter++ % 1000 == 0 && abs(rbError) > 100) {
                            // Show calculated drift (which is output - expected = output -
                            // (output - rbError) = rbError)
                            DLL_Log(
                                "[PullAudio] Src %d Buffer Level: %zu (Err: %lld) -> Comp "
                                "Drift: %lld",
                                (int)srcIdx, rbAvailable, rbError, rbError);
                        }
                    } else if (driftLogCounter++ % 2000 == 0) {
                        DLL_Log("[PullAudio] Src %d Buffer low (%zu) - holding drift compensation", (int)srcIdx,
                                rbAvailable);
                    }
                }

                // Pull available audio from ring buffer and resample into
                // postResampleBuffer. IMPORTANT: We must be able to drain more than
                // ~10ms per call. At recording start, video PTS can jump (e.g. first
                // frames arrive late), while audio capture threads may have already
                // buffered/synthesized that interval. If we only drain 10ms, the ring
                // buffer overflows and we drop early audio, causing the exact "silence
                // / brief sound / silence / delayed" symptom.
                if (src.syncResampler && src.syncResampler->IsReady()) {
                    const size_t MAX_CHUNK_FLOATS = (size_t)(SAMPLE_RATE * CHANNELS / 10);  // 100ms
                    while (src.postResampleBuffer.size() < totalFloats) {
                        size_t rbFloats = src.ringBuffer->GetAvailable();
                        if (rbFloats == 0) {
                            break;
                        }

                        size_t needFloats = totalFloats - src.postResampleBuffer.size();
                        size_t chunkFloats = std::min(rbFloats, needFloats);
                        chunkFloats = std::min(chunkFloats, MAX_CHUNK_FLOATS);

                        // Keep channel alignment (interleaved stereo)
                        chunkFloats -= (chunkFloats % CHANNELS);
                        if (chunkFloats == 0) {
                            break;
                        }

                        std::vector<float> rbData(chunkFloats);
                        size_t actualRead = src.ringBuffer->Read(rbData.data(), chunkFloats);
                        if (actualRead == 0) {
                            break;
                        }

                        // Resample through syncResampler
                        uint8_t** resampledData = nullptr;
                        int outSamples = 0;
                        if (src.syncResampler->Process((uint8_t*)rbData.data(), (int)(actualRead * sizeof(float)),
                                                       &resampledData, &outSamples)) {
                            if (outSamples > 0 && resampledData && resampledData[0]) {
                                float* outFloats = (float*)resampledData[0];
                                if (src.dropFadeSamplesRemaining > 0) {
                                    const int kDropFadeSamples = SAMPLE_RATE / 200;
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
                                int numFloats = outSamples * CHANNELS;
                                src.postResampleBuffer.insert(src.postResampleBuffer.end(), outFloats,
                                                              outFloats + numFloats);
                                src.syncSamplesOutput += outSamples;
                            }
                            AudioResampler::FreeOutputBuffer(resampledData);
                        }
                    }
                }

                // Safety cap: bound postResampleBuffer to prevent unbounded growth
                // if syncResampler consistently expands output (e.g. during clock stretch).
                // Minimum 50ms to absorb short encoder stalls without truncating audio.
                const size_t MIN_POST_RESAMPLE_FLOATS = (size_t)(SAMPLE_RATE * CHANNELS / 20);  // 50ms
                const size_t MAX_POST_RESAMPLE_FLOATS = std::max(totalFloats * 4, MIN_POST_RESAMPLE_FLOATS);
                if (src.postResampleBuffer.size() > MAX_POST_RESAMPLE_FLOATS) {
                    size_t excess = src.postResampleBuffer.size() - MAX_POST_RESAMPLE_FLOATS;
                    src.postResampleBuffer.erase(src.postResampleBuffer.begin(),
                                                 src.postResampleBuffer.begin() + (std::ptrdiff_t)excess);
                }

                // Pop exactly totalFloats from postResampleBuffer for mixing
                std::vector<float> srcData(totalFloats, 0.0f);
                size_t available = src.postResampleBuffer.size();
                size_t toCopy = std::min(available, totalFloats);

                if (toCopy > 0) {
                    std::copy(src.postResampleBuffer.begin(), src.postResampleBuffer.begin() + toCopy, srcData.begin());
                    src.postResampleBuffer.erase(src.postResampleBuffer.begin(),
                                                 src.postResampleBuffer.begin() + toCopy);
                    activeSources++;
                }
                // If toCopy < totalFloats, the rest is already zero (silence padding)

                // Sum into mix buffer
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
                const int64_t FADE_SAMPLES = SAMPLE_RATE / 20;  // 50ms
                int64_t trackPos = encodedSamplesPerSource[firstSrcIdx];
                int64_t fadeStart = applyTransitionFade ? 0 : trackPos;
                if (trackPos < FADE_SAMPLES || applyTransitionFade) {
                    for (int64_t s = 0; s < samplesToEncode; s++) {
                        int64_t global = fadeStart + s;
                        float gain = (global >= FADE_SAMPLES) ? 1.0f : (float)global / (float)FADE_SAMPLES;
                        size_t base = (size_t)s * CHANNELS;
                        mixBuffer[base + 0] *= gain;
                        mixBuffer[base + 1] *= gain;
                    }
                }
            }

            if (activeSources > 1) {
                // Soft-knee limiter: linear below 0.9, hyperbolic above.
                // Avoids the discontinuous gain jump of hard clipping which
                // causes audible clicks when mixed sources briefly exceed 1.0.
                // Maximum output is 1.0 (asymptotic), knee is smooth (C¹).
                constexpr float kKnee = 0.9f;
                constexpr float kRange = 1.0f - kKnee;   // 0.1
                constexpr float kScale = 1.0f / kRange;  // 10.0
                for (auto& s : mixBuffer) {
                    if (s > kKnee) {
                        float excess = (s - kKnee) * kScale;  // 0..∞ mapped from kKnee..∞
                        s = kKnee + kRange * (excess / (1.0f + excess));
                    } else if (s < -kKnee) {
                        float excess = (-s - kKnee) * kScale;
                        s = -(kKnee + kRange * (excess / (1.0f + excess)));
                    }
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

                if (srcIndices.size() > 1 && mixLogCounter++ % 500 == 0) {
                    DLL_Log("[PullAudio] Mixed %d sources for track %d (%lld samples)", activeSources, track,
                            samplesToEncode);
                }
            }

            // Update source counters ONLY when we actually encoded real audio
            for (size_t srcIdx : srcIndices) {
                encodedSamplesPerSource[srcIdx] += samplesToEncode;
            }

            // A/V SYNC MONITORING: Periodic check for drift detection
            // Log every ~60 seconds (6000 frames @ 100fps, 7200 @ 120fps)
            if (syncCheckCounter++ % 6000 == 0 && firstSrcIdx < encodedSamplesPerSource.size()) {
                int64_t wallVideoMs = videoElapsedMs.load();
                int64_t videoMs = wallVideoMs;
                if (videoEnc) {
                    int64_t encodedVideoUs = videoEnc->GetEncodedDurationUs();
                    if (encodedVideoUs > 0) {
                        videoMs = encodedVideoUs / 1000;
                    }
                }
                int64_t audioSamples = encodedSamplesPerSource[firstSrcIdx];
                int64_t audioMs = (audioSamples * 1000) / SAMPLE_RATE;
                int64_t avDrift = audioMs - videoMs;

                // Gather ring buffer level and drift compensation data
                size_t rbLevel = 0;
                int64_t syncOutput = 0;
                size_t droppedTotal = 0;
                if (firstSrcIdx < audioSources.size()) {
                    auto& src = audioSources[firstSrcIdx];
                    if (src.ringBuffer)
                        rbLevel = src.ringBuffer->GetAvailable() / CHANNELS;
                    syncOutput = src.syncSamplesOutput;
                }

                DLL_Log(
                    "[A/V SYNC CHECK] Track %d: Video=%lld ms, Audio=%lld ms, "
                    "Drift=%lld ms, VideoWall=%lld ms, RBLevel=%zu samples, "
                    "SyncOutput=%lld, Dropped=%zu",
                    track, videoMs, audioMs, avDrift, wallVideoMs, rbLevel, syncOutput, droppedTotal);

                if (std::abs(avDrift) > 100) {
                    DLL_Log(
                        "[A/V SYNC WARNING] Track %d drift exceeds 100ms! "
                        "Investigate potential sync issues.",
                        track);
                }
            }
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
        int iSampleRate = 48000;
        if (audioConfig.sampleRate != "default" && !audioConfig.sampleRate.empty()) {
            try {
                iSampleRate = std::stoi(audioConfig.sampleRate);
            } catch (...) {}
        }
        size_t capacity = static_cast<size_t>(iSampleRate) * 2 * 2;
        source.ringBuffer = std::make_unique<AudioRingBuffer>(capacity);
        DLL_Log("MediaEngine::Init RingBuffer created for source %zu. Cap=%zu samples, rate=%d", sourceIdx, capacity,
                iSampleRate);

        source.syncResampler = std::make_unique<AudioResampler>();
        AudioResampler::InputFormat syncInFmt;
        syncInFmt.sampleRate = iSampleRate;
        syncInFmt.channels = 2;
        syncInFmt.bitsPerSample = 32;
        syncInFmt.validBitsPerSample = 32;
        syncInFmt.isFloat = true;
        syncInFmt.blockAlign = 8;
        AudioResampler::OutputFormat syncOutFmt;
        syncOutFmt.sampleRate = iSampleRate;
        syncOutFmt.channels = 2;
        syncOutFmt.sampleFmt = AV_SAMPLE_FMT_FLT;
        source.syncResampler->Init(syncInFmt, syncOutFmt);
        DLL_Log("MediaEngine::Init SyncResampler created for source %zu (rate=%d)", sourceIdx, iSampleRate);
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
                            DLL_Log("[AudioLoop] Discarding pre-start packet src=%d packet=%lld start=%lld", (int)srcIdx,
                                    packet.timestamp, startQPC);
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
                            int numFloats = outSamples * targetFmt.channels;
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

                                    if (diff > 5 && src.ringBuffer) {
                                        // Audio arrived late relative to recording start; pad with
                                        // silence to preserve accurate timeline alignment.
                                        int64_t silenceSamples = (diff * 48000) / 1000;
                                        size_t silenceFloats = (size_t)silenceSamples * 2;  // stereo
                                        // Cap to ring buffer free space to avoid overflow
                                        size_t freeSpace = src.ringBuffer->GetFree();
                                        silenceFloats = std::min(silenceFloats, freeSpace);
                                        // Align to stereo channel boundary
                                        silenceFloats -= silenceFloats % 2;
                                        if (silenceFloats > 0) {
                                            std::vector<float> silence(silenceFloats, 0.0f);
                                            src.ringBuffer->Write(silence.data(), silenceFloats);
                                            DLL_Log(
                                                "[AudioLoop] Inserted %lld ms silence for "
                                                "late-start src=%d",
                                                diff, (int)srcIdx);
                                        }
                                    }
                                }

                                sourceTimestamps[srcIdx] = packet.timestamp;
                                // WriteRetainNew: atomically drops oldest audio to make room,
                                // then writes new audio. No race between GetFree/Skip/Write.
                                src.ringBuffer->WriteRetainNew((float*)resampledData[0], numFloats);
                            } else if (src.ringBuffer && audioSyncPending.load()) {
                                // The sync gate is still closed, so this packet is
                                // intentionally discarded and must not establish the
                                // source timeline yet.
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

extern "C" {

// Global Logger
static LogCallback g_LogCallback = nullptr;

MEDIAENGINE_API void DLL_Log(const char* fmt, ...) {
    if (!g_LogCallback)
        return;
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    g_LogCallback(buffer);
}

MEDIAENGINE_API void MediaEngine_SetLogCallback(LogCallback callback) {
    g_LogCallback = callback;
    DLL_Log("MediaEngine Logging Initialized");
}

MEDIAENGINE_API bool MediaEngine_Init(const AppConfig* config) {
    DLL_Log("[Media] MediaEngine_Init Called. Version: %s (Built: %s)", CAPTURE_VERSION, BUILD_TIMESTAMP);
    if (!g_Engine)
        g_Engine = std::make_unique<MediaEngine>();
    // config is a pointer, pass it directly
    return g_Engine->Init(config);
}

MEDIAENGINE_API bool MediaEngine_StartRecording() {
    if (g_Engine)
        return g_Engine->StartRecording();
    return false;
}

MEDIAENGINE_API void MediaEngine_ReloadConfig(const AppConfig* config) {
    if (g_Engine)
        g_Engine->ReloadConfig(config);
}

MEDIAENGINE_API void MediaEngine_StopRecording() {
    if (g_Engine)
        g_Engine->StopRecording();
}

MEDIAENGINE_API void MediaEngine_ReleaseEncoderTextures() {
    if (g_Engine && g_Engine->videoEnc)
        g_Engine->videoEnc->ReleasePreservedEncoderTextures();
}

MEDIAENGINE_API void MediaEngine_Shutdown() {
    if (g_Engine)
        g_Engine->StopRecording();
    g_Engine.reset();
}

MEDIAENGINE_API void MediaEngine_ProcessFrame(uint64_t textureHandle, uint64_t fenceHandle, uint64_t fenceValue,
                                              int64_t timestamp, int32_t luidLow, int32_t luidHigh, uint32_t sourcePid,
                                              uint32_t width, uint32_t height, uint32_t format, bool isHDR,
                                              bool isShmem, int shmemSlot) {
    if (g_Engine) {
        g_Engine->ProcessFrame(textureHandle, fenceHandle, fenceValue, timestamp, luidLow, luidHigh, sourcePid, width,
                               height, format, isHDR, isShmem, shmemSlot);
    }
}
MEDIAENGINE_API void MediaEngine_ProcessFrameD3D11(void* texture, int64_t timestamp, uint32_t width, uint32_t height) {
    if (g_Engine)
        g_Engine->ProcessFrameD3D11(texture, timestamp, width, height);
}

MEDIAENGINE_API bool MediaEngine_CreateSharedCaptureTextures(uint32_t width, uint32_t height, uint32_t format,
                                                             SharedMemoryLayout* sharedMem) {
    if (!g_Engine) {
        DLL_Log("[MediaEngine] CreateSharedCaptureTextures: Engine not ready");
        return false;
    }
    return g_Engine->CreateSharedCaptureTextures(width, height, format, sharedMem);
}

MEDIAENGINE_API int64_t MediaEngine_GetLastFrameEncodeTimeUs() {
    if (g_Engine) {
        return g_Engine->GetLastVideoEncodeTimeUs();
    }
    return 0;
}

MEDIAENGINE_API void MediaEngine_SetSharedMem(void* pSharedMem, void* pShmem) {
    if (g_Engine && g_Engine->videoEnc) {
        g_Engine->videoEnc->SetSharedMem((SharedMemoryLayout*)pSharedMem, (ShmemBuffer*)pShmem);
    }
}

MEDIAENGINE_API int64_t MediaEngine_GetLastFrameFenceWaitUs() {
    if (g_Engine) {
        return g_Engine->GetLastFrameFenceWaitUs();
    }
    return 0;
}

// Shared D3D11 device for screengrab mode - ensures ScreenCapture and
// VideoEncoder use same device
ID3D11Device* g_SharedD3D11Device = nullptr;
ID3D11DeviceContext* g_SharedD3D11Context = nullptr;

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

}  // extern "C"
