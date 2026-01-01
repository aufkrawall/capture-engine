#include "mediaengine.h"
#include "../common/logging.h"
#include "app_audio_capture.h"
#include "audio_capture.h"
#include "audio_encoder.h"

#include "audio_resampler.h"
#include "audio_ring_buffer.h" // Pull Model Buffer
#include "video_encoder.h"
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

// Global or Singleton state preferred for DLL functions
// Or we map config to instance.
// For simplicity, SINGLETON pattern (one active engine).

class MediaEngine {
public:
  MediaEngine() : recording(false), audioRunning(false), firstVideoFrameMs(0), lastVideoFrameMs(0), videoElapsedMs(0) {}
  ~MediaEngine() { StopRecording(); }

  // Multi-source audio support
  struct AudioSource {
    std::unique_ptr<AudioCapture> capture;       // For system/mic audio
    std::unique_ptr<AppAudioCapture> appCapture; // For per-app audio
    std::unique_ptr<AudioEncoder>
        encoder; // Owned encoder (if first source for this track)
    AudioEncoder *sharedEncoderPtr =
        nullptr; // Always points to the encoder to use
    std::unique_ptr<AudioRingBuffer> ringBuffer; // Pull Model Buffer (Writer=Capture, Reader=Encoder)
    AudioConfig config;
    int track = 0; // Target track number
    AudioConfig::SourceType sourceType = AudioConfig::SystemAudio;
  };

  // Per-track encoder with optional mixing (when multiple sources target same track)


  // Member variables
  std::unique_ptr<VideoEncoder> videoEnc;
  std::vector<AudioSource> audioSources;


  AppConfig config;
  std::recursive_mutex muxMutex; // Must be recursive - WritePacket callback from EncodeFrame
  bool recording;

  // Audio thread
  std::thread audioThread;
  std::atomic<bool> audioRunning;
  int64_t firstVideoFrameMs;  // Timestamp of first video frame for A/V sync
  int64_t lastVideoFrameMs;   // Timestamp of last video frame for audio trimming
  std::atomic<int64_t> videoElapsedMs;  // Elapsed video time in ms for audio clock sync
  
  // Get current video elapsed time for audio clock compensation
  int64_t GetVideoElapsedMs() const { return videoElapsedMs.load(); }

  // Pull Model: Track encoded samples per source for progressive encoding
  std::vector<int64_t> encodedSamplesPerSource;

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

  bool Init(const AppConfig *config) {
    std::lock_guard<std::recursive_mutex> lock(muxMutex);
    DLL_Log("MediaEngine::Init starting");
    DLL_Log("MediaEngine::Init config ptr=%p", (void *)config);
    this->config = *config;

    // Setup Video (Alloc Only)
    DLL_Log("MediaEngine::Init creating VideoEncoder");
    videoEnc = std::make_unique<VideoEncoder>();
    DLL_Log("MediaEngine::Init calling VideoEncoder::Init");
    bool vRes =
        videoEnc->Init(config->video, 0, 0,
                       config->video.fps, // Resolution deferred to first frame
                       [this](AVPacket *pkt) { this->WritePacket(pkt); });

    if (!vRes) {
      DLL_Log("MediaEngine::Init VideoEncoder init failed");
      return false;
    }
    DLL_Log("MediaEngine::Init VideoEncoder initialized OK");

    // Setup Audio Sources (supports multiple: system audio, microphone, etc.)
    DLL_Log("MediaEngine::Init audio sources count=%d",
            (int)config->audioSources.size());

    // Maps track number to encoder for that track
    std::map<int, AudioEncoder *> trackToEncoder;

    for (size_t i = 0; i < config->audioSources.size(); i++) {
      const AudioConfig &audioConfig = config->audioSources[i];
      if (!audioConfig.enabled) {
        DLL_Log("MediaEngine::Init audio source %zu disabled", i);
        continue;
      }

      DLL_Log("MediaEngine::Init setting up audio source %zu (type=%d device=%s process=%s)", i,
              (int)audioConfig.sourceType,
              audioConfig.device.empty() ? "default" : audioConfig.device.c_str(),
              audioConfig.processName.empty() ? "N/A" : audioConfig.processName.c_str());

      // Get the list of tracks this source should output to
      std::vector<int> targetTracks = audioConfig.tracks;
      if (targetTracks.empty()) {
        // Default: use track (i+1)
        targetTracks.push_back((int)(i + 1));
      }

      DLL_Log("MediaEngine::Init Audio source %zu targets %zu tracks", i,
              targetTracks.size());

      // For each target track, create or reuse an encoder
      for (int track : targetTracks) {
        // Check if we already have an encoder for this track
        AudioEncoder *encoderForTrack = nullptr;
        auto it = trackToEncoder.find(track);
        if (it != trackToEncoder.end()) {
          encoderForTrack = it->second;
          DLL_Log(
              "MediaEngine::Init Audio source %zu reusing encoder for track %d",
              i, track);
        } else {
          // Create new encoder for this track
          auto newEncoder = std::make_unique<AudioEncoder>();
          bool aRes = newEncoder->Init(
              audioConfig, [this](AVPacket *pkt) { this->WritePacket(pkt); });

          if (!aRes) {
            DLL_Log("MediaEngine::Init Audio encoder for track %d failed",
                    track);
            continue;
          }

          // Register with VideoEncoder for stream creation
          videoEnc->AddAudioContext(audioConfig, newEncoder->GetCodecContext(),
                                    track);

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

          // INIT RING BUFFER (Pull Model)
          int iSampleRate = 48000;
          if (audioConfig.sampleRate != "default" && !audioConfig.sampleRate.empty()) {
             try { iSampleRate = std::stoi(audioConfig.sampleRate); } catch(...) {}
          }
          size_t capacity = iSampleRate * 2 * 2; 
          source.ringBuffer = std::make_unique<AudioRingBuffer>(capacity);
          DLL_Log("MediaEngine::Init RingBuffer created. Cap=%zu samples", capacity);

          DLL_Log("MediaEngine::Init Created new encoder for track %d (source "
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
          source.encoder = nullptr; // Shared, not owned
          source.sharedEncoderPtr = encoderForTrack;

          // Create appropriate capture type
          if (audioConfig.sourceType == AudioConfig::AppAudio) {
            source.appCapture = std::make_unique<AppAudioCapture>();
          } else {
            source.capture = std::make_unique<AudioCapture>();
          }
          
          // INIT RING BUFFER (Pull Model)
          int iSampleRate = 48000;
          if (audioConfig.sampleRate != "default" && !audioConfig.sampleRate.empty()) {
             try { iSampleRate = std::stoi(audioConfig.sampleRate); } catch(...) {}
          }
          size_t capacity = iSampleRate * 2 * 2;
          source.ringBuffer = std::make_unique<AudioRingBuffer>(capacity);
          DLL_Log("MediaEngine::Init RingBuffer created (shared). Cap=%zu samples", capacity);

          DLL_Log("MediaEngine::Init Audio source %zu shares encoder for track "
                  "%d (type=%d)",
                  i, track, (int)audioConfig.sourceType);
          audioSources.push_back(std::move(source));
        }
      }
    }

    DLL_Log(
        "MediaEngine::Init complete. Audio sources: %zu, unique tracks: %zu",
        audioSources.size(), trackToEncoder.size());
    return true;
  }



  int64_t GetLastVideoFenceWaitUs() const {
      if (videoEnc) return videoEnc->GetLastFrameFenceWaitUs();
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
    for (auto &src : audioSources) {
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
        videoEnc->AddAudioContext(src.config, src.encoder->GetCodecContext(),
                                  src.track);
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
      // We defer this to the first video frame in ProcessFrameD3D11 for perfect A/V sync.
      // Audio data captured before first video frame will be discarded.
      // CRITICAL: Reset video clock for new recording to prevent stale timestamps
      firstVideoFrameMs = 0;  // Reset for new recording
      videoElapsedMs.store(0); // CRITICAL: Reset video clock for new recording to prevent stale timestamps

      // PULL MODEL: Reset audio encoding state for new recording
      encodedSamplesPerSource.clear();
      encodedSamplesPerSource.resize(audioSources.size(), 0);
      
      // PULL MODEL: CRITICAL - Clear ring buffers to start fresh
      for (auto &src : audioSources) {
        if (src.ringBuffer) {
          size_t prevAvail = src.ringBuffer->GetAvailable();
          src.ringBuffer->Clear();
          if (prevAvail > 0) {
            DLL_Log("MediaEngine: Cleared stale ringBuffer with %zu samples", prevAvail);
          }
        }
      }

      // Start all audio sources
      int startedCount = 0;
      for (auto &src : audioSources) {
        // Recording start will be set when first video frame arrives

        bool started = false;
        
        if (src.sourceType == AudioConfig::AppAudio && src.appCapture) {
          // Start per-app audio capture
          if (!src.config.processName.empty()) {
            started = src.appCapture->StartByName(src.config.processName);
            DLL_Log("MediaEngine: App audio source starting for process '%s' (track=%d)",
                    src.config.processName.c_str(), src.track);
          } else if (src.config.processId != 0) {
            started = src.appCapture->StartByPID(src.config.processId);
            DLL_Log("MediaEngine: App audio source starting for PID %lu (track=%d)",
                    src.config.processId, src.track);
          }
        } else if (src.capture) {
          // Start regular capture: loopback for system audio, device for microphone
          bool isLoopback = (src.sourceType == AudioConfig::SystemAudio);
          started = src.capture->Start(src.config.device, isLoopback);
          DLL_Log("MediaEngine: Audio source started (track=%d, type=%d)",
                  src.track, (int)src.sourceType);
        }
        
        if (started) {
          startedCount++;
        } else {
          DLL_Log("MediaEngine: Audio source failed to start (track=%d, type=%d)",
                  src.track, (int)src.sourceType);
        }
      }

      if (startedCount > 0) {
        DLL_Log("MediaEngine: %d audio source(s) started (sync pending first video frame)",
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

    // Set recording end timestamp on all audio encoders BEFORE stopping audio thread
    // This ensures audio is trimmed to match the last video frame exactly
    // NEW: Use exact encoded video duration for sample-perfect sync
    if (videoEnc) {
      int64_t durationUs = videoEnc->GetEncodedDurationUs();
      int64_t startUs = firstVideoFrameMs * 1000;
      int64_t endUs = startUs + durationUs;
      
      DLL_Log("MediaEngine: Setting audio end time. Start: %lld us, Dur: %lld us, End: %lld us", 
              startUs, durationUs, endUs);

      for (auto &src : audioSources) {
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
    for (auto &src : audioSources) {
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

  void ProcessFrame(uint64_t handle, uint64_t fenceHandle, uint64_t fenceVal,
                    int64_t timestamp, int32_t luidLow, int32_t luidHigh,
                    uint32_t sourcePid, uint32_t width, uint32_t height,
                    uint32_t format, bool isHDR, bool isShmem = false, int shmemSlot = 0) {
    std::lock_guard<std::recursive_mutex> lock(muxMutex);
    if (!videoEnc || !recording)
      return;

    // On first video frame, sync audio to this timestamp for 0ms A/V desync
    if (firstVideoFrameMs == 0) {
      firstVideoFrameMs = timestamp;
      DLL_Log("MediaEngine: First inject frame at %lld ms - syncing audio", timestamp);
      
      // Set recording start for all audio encoders to match video start (Microseconds)
      for (auto &src : audioSources) {
        if (src.sharedEncoderPtr) {
          src.sharedEncoderPtr->SetRecordingStart(timestamp * 1000);
        }
      }
    }

    // Maybe we want preview later? For now, recording only.
    videoEnc->SetAdapterLUID(luidLow, luidHigh);
    bool res = videoEnc->EncodeFrame((HANDLE)handle, (HANDLE)fenceHandle,
                                     fenceVal, timestamp, sourcePid, width,
                                     height, format, isHDR, isShmem, shmemSlot);
    (void)res; // Avoid unused warn if not logging success here

    // Track last video frame timestamp for audio trimming
    lastVideoFrameMs = timestamp;
    
    // Update video elapsed time for audio clock sync
    videoElapsedMs.store(timestamp - firstVideoFrameMs);

    // Update audio stream index for all sources
    for (size_t i = 0; i < audioSources.size(); i++) {
      auto &src = audioSources[i];
      // Get stream index from VideoEncoder for this track
      int idx = videoEnc->GetAudioStreamIndex(src.track);
      if (idx >= 0 && src.encoder) {
        src.encoder->SetStreamIndex(idx);
      }
    }

    static int logCount = 0;
    if (logCount++ % 60 == 0) {
      DLL_Log("MediaEngine: Sending Frame ts=%lld", timestamp);
    }

    // PULL MODEL: Encode audio for this video frame (same as screengrab mode)
    PullAndEncodeAudio(timestamp);
  }

  // Direct D3D11 texture processing for screengrab mode (zero-copy)
  void ProcessFrameD3D11(void *texture, int64_t timestamp, uint32_t width,
                         uint32_t height) {
    std::lock_guard<std::recursive_mutex> lock(muxMutex);
    if (!videoEnc || !recording)
      return;

    // On first video frame, sync audio to this timestamp for 0ms A/V desync
    if (firstVideoFrameMs == 0) {
      firstVideoFrameMs = timestamp;
      DLL_Log("MediaEngine: First video frame at %lld ms - syncing audio", timestamp);
      
      // Set recording start for all audio encoders to match video start (Microseconds)
      for (auto &src : audioSources) {
        if (src.sharedEncoderPtr) {
          src.sharedEncoderPtr->SetRecordingStart(timestamp * 1000);
        }
      }
    }

    videoEnc->EncodeFrameD3D11((ID3D11Texture2D *)texture, timestamp, width,
                               height);

    // Track last video frame timestamp for audio trimming
    lastVideoFrameMs = timestamp;

    // Update audio stream index for all sources
    for (size_t i = 0; i < audioSources.size(); i++) {
      auto &src = audioSources[i];
      int idx = videoEnc->GetAudioStreamIndex(src.track);
      if (idx >= 0 && src.encoder) {
        src.encoder->SetStreamIndex(idx);
      }
    }

    static int logCount = 0;
    if (logCount++ % 60 == 0) {
      DLL_Log("MediaEngine: ScreenGrab frame ts=%lld %dx%d", timestamp, width,
              height);
    }

    // PULL MODEL: Encode audio for this video frame
    PullAndEncodeAudio(timestamp);
  }

  // PULL MODEL: Pull audio from RingBuffers and encode based on Video PTS
  // This version properly groups sources by track and mixes before encoding
  void PullAndEncodeAudio(int64_t videoTimestampMs) {
    if (!recording || audioSources.empty())
      return;

    // Use the pre-computed relative video elapsed time
    // videoElapsedMs is updated in ProcessFrame/ProcessFrameD3D11 and is already relative
    int64_t audioTargetMs = videoElapsedMs.load();
    
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

    // Group sources by track for mixing
    std::map<int, std::vector<size_t>> trackToSources;
    for (size_t srcIdx = 0; srcIdx < audioSources.size(); srcIdx++) {
      auto &src = audioSources[srcIdx];
      if (src.ringBuffer && src.sharedEncoderPtr) {
        trackToSources[src.track].push_back(srcIdx);
      }
    }

    // For each track, pull audio from all sources, mix, then encode
    for (auto &kv : trackToSources) {
      int track = kv.first;
      auto &srcIndices = kv.second;
      
      if (srcIndices.empty()) continue;
      
      // Calculate samples to encode based on first source's progress
      size_t firstSrcIdx = srcIndices[0];
      int64_t samplesToEncode = targetSamples - encodedSamplesPerSource[firstSrcIdx];
      
      if (samplesToEncode <= 0)
        continue; // Already caught up
      
      // SAFETY CAP: If gap is too large (> 500ms), warp the counters close to target
      // but leave room for one chunk of audio so encoding still happens.
      // This prevents 32GB leaks while still producing audio output.
      const int64_t MAX_SILENCE_SAMPLES = (SAMPLE_RATE / 2); // 500ms
      const int64_t ONE_FRAME_SAMPLES = SAMPLE_RATE / 120;   // ~400 samples at 120fps
      
      if (samplesToEncode > MAX_SILENCE_SAMPLES) {
          // How much to warp: jump to (target - one frame) so we still encode something
          int64_t warpAmount = samplesToEncode - ONE_FRAME_SAMPLES;
          if (warpAmount > 0) {
              static int warpLogCounter = 0;
              if (warpLogCounter++ % 60 == 0) {  // Log every ~0.5 sec at 120fps
                  DLL_Log("[PullAudio] Large A/V gap (%.2f sec) - warping %lld samples, encoding %lld",
                          (double)samplesToEncode / SAMPLE_RATE, warpAmount, ONE_FRAME_SAMPLES);
              }
              
              // Warp all source counters
              for (size_t srcIdx : srcIndices) {
                  encodedSamplesPerSource[srcIdx] += warpAmount;
              }
              
              // Update samplesToEncode to only encode the remaining small chunk
              samplesToEncode = ONE_FRAME_SAMPLES;
          }
      }

      size_t totalFloats = samplesToEncode * CHANNELS;
      
      // Prepare mix buffer (initialized to zero for summing)
      std::vector<float> mixBuffer(totalFloats, 0.0f);
      int activeSources = 0;
      
      // Pull from each source and sum into mix buffer
      for (size_t srcIdx : srcIndices) {
        auto &src = audioSources[srcIdx];
        
        // Check for dropped samples
        size_t droppedFloats = src.ringBuffer->GetAndClearDroppedSamples();
        if (droppedFloats > 0) {
          DLL_Log("[PullAudio] WARNING: Ring buffer overflow - %zu samples dropped for src=%d",
                  droppedFloats, (int)srcIdx);
        }
        
        // Pull from ring buffer
        std::vector<float> srcData(totalFloats);
        size_t samplesRead = src.ringBuffer->Read(srcData.data(), totalFloats);
        
        if (samplesRead == 0) {
          // Buffer underflow - this source has no data (game paused/silence)
          // Continue to next source, counter will be updated below for ALL sources
          continue;
        }
        
        // Pad with silence if partial read
        if (samplesRead < totalFloats) {
          memset(srcData.data() + samplesRead, 0, (totalFloats - samplesRead) * sizeof(float));
        }
        
        // Sum into mix buffer
        for (size_t i = 0; i < totalFloats; i++) {
          mixBuffer[i] += srcData[i];
        }
        activeSources++;
      }
      
      // If ALL sources for this track are silent (game pause), SKIP encoding entirely.
      // Do NOT encode silence here - the AudioEncoder has its own gap handling.
      // Do NOT advance counters - this allows us to "catch up" when real audio arrives.
      // This fixes the audio delay issue after game pauses.
      // If ALL sources for this track are silent (game pause), we MUST generate silence.
      // Otherwise, the Audio Stream timestamps stop advancing, and av_interleaved_write_frame
      // will BUFFER VIDEO PACKETS INDEFINITELY waiting for audio to catch up. 
      // This causes the 32GB RAM leak.
      
      if (activeSources == 0) {
        /* 
           Logic:
           1. Create a zeroed buffer matching totalFloats.
           2. Encode it.
           3. Advance counters.
        */
        static int silenceLogCounter = 0;
        if (silenceLogCounter++ % 500 == 0) {
          DLL_Log("[PullAudio] Track %d silent - generating %lld samples of silence to maintain sync", 
                  track, samplesToEncode);
        }
        
        // Zero-fill the mix buffer (which is already zeroed by constructor, but explicit is good)
        std::fill(mixBuffer.begin(), mixBuffer.end(), 0.0f);
        
        // We still 'processed' the mix (it's just silence)
        // Encode below...
      } else {
         // Perform mixing for active sources
         // (Existing logic moved here or just fall through since mixBuffer is already correct?)
         // mixBuffer is already zeroed.
         // We can just skip the "if (activeSources == 0) continue" check and let the flow continue.
         // But we need to handle the "padding" logic inside the source loop.
         // The source loop summed into mixBuffer.
         // If activeSources==0, mixBuffer is [0,0,0...].
         // So we just proceed to encoding!
      }
      
      // Removed the 'continue' constraint.
      // Soft clipping: Clamp values to [-1, 1] to prevent distortion (only needed if sources > 1)

      
      // Soft clipping: Clamp values to [-1, 1] to prevent distortion
      if (activeSources > 1) {
        for (auto &s : mixBuffer) {
          if (s > 1.0f) s = 1.0f;
          else if (s < -1.0f) s = -1.0f;
        }
      }
      
      // Encode the mixed samples using first source's encoder
      AudioEncoder *encoder = audioSources[firstSrcIdx].sharedEncoderPtr;
      if (encoder) {
        std::vector<uint8_t> encodeData(totalFloats * sizeof(float));
        memcpy(encodeData.data(), mixBuffer.data(), encodeData.size());
        
        // Calculate timestamp for this audio chunk
        // CRITICAL: Use RELATIVE time from sample count, not absolute QPC!
        // This must match what the encoder expects - time relative to recording start
        int64_t audioChunkTimestampMs = (encodedSamplesPerSource[firstSrcIdx] * 1000) / SAMPLE_RATE;
        
        encoder->EncodeSamples(
            encodeData.data(), (int)encodeData.size(), 
            CHANNELS, SAMPLE_RATE, 
            32, 32, CHANNELS * 4, true,  // float32
            audioChunkTimestampMs);
            
        static int mixLogCounter = 0;
        if (srcIndices.size() > 1 && mixLogCounter++ % 500 == 0) {
          DLL_Log("[PullAudio] Mixed %d sources for track %d (%lld samples)", 
                  activeSources, track, samplesToEncode);
        }
      }
      
      // Update source counters ONLY when we actually encoded real audio
      for (size_t srcIdx : srcIndices) {
        encodedSamplesPerSource[srcIdx] += samplesToEncode;
      }
    }
  }

  // Create shared D3D11 textures for Vulkan games to import
  bool CreateSharedCaptureTextures(uint32_t width, uint32_t height,
                                   uint32_t format,
                                   SharedMemoryLayout *sharedMem) {
    std::lock_guard<std::recursive_mutex> lock(muxMutex);
    if (!videoEnc) {
      DLL_Log("MediaEngine: CreateSharedCaptureTextures - no encoder");
      return false;
    }

    // IMPORTANT: Set encoder dimensions and LUID from the parameters before EnsureDevice
    // Otherwise EnsureDevice fails because width/height are still 0 or uses wrong GPU
    videoEnc->SetDimensions(width, height);
    videoEnc->SetAdapterLUID(sharedMem->luidLowPart, sharedMem->luidHighPart);

    if (!videoEnc->EnsureDevice()) {
      DLL_Log("MediaEngine: CreateSharedCaptureTextures - device init failed for LUID %08x:%08x", 
              sharedMem->luidLowPart, sharedMem->luidHighPart);
      return false;
    }
    return videoEnc->CreateSharedCaptureTextures(width, height, format,
                                                 sharedMem);
  }

  void WritePacket(AVPacket *pkt) {
    std::lock_guard<std::recursive_mutex> lock(muxMutex);
    if (videoEnc) {
      videoEnc->WriteFrame(pkt);
    }
  }

  void ReloadConfig(const AppConfig *newConfig) {
    std::lock_guard<std::recursive_mutex> lock(muxMutex);
    DLL_Log("MediaEngine::ReloadConfig called");

    // Update config
    this->config = *newConfig;

    // If recording, we can't fully re-init, but we can log a warning.
    if (recording) {
      DLL_Log("MediaEngine: Config updated, but recording is active. Changes "
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
        videoEnc->Init(config.video, 0, 0, config.video.fps,
                       [this](AVPacket *pkt) { this->WritePacket(pkt); });

    if (!vRes) {
      DLL_Log("MediaEngine: Failed to re-init VideoEncoder!");
      return;
    }
    DLL_Log("MediaEngine: VideoEncoder re-initialized successfully.");

    // Re-create audio sources with new config (including new codec)
    // Maps track number to encoder for that track
    std::map<int, AudioEncoder *> trackToEncoder;

    for (size_t i = 0; i < config.audioSources.size(); i++) {
      const AudioConfig &audioConfig = config.audioSources[i];
      if (!audioConfig.enabled) {
        DLL_Log("MediaEngine::ReloadConfig audio source %zu disabled", i);
        continue;
      }

      DLL_Log(
          "MediaEngine::ReloadConfig setting up audio source %zu (codec=%s)", i,
          audioConfig.codec.c_str());

      // Get the list of tracks this source should output to
      std::vector<int> targetTracks = audioConfig.tracks;
      if (targetTracks.empty()) {
        targetTracks.push_back((int)(i + 1));
      }

      DLL_Log("MediaEngine::ReloadConfig Audio source %zu targets %zu tracks",
              i, targetTracks.size());

      // For each target track, create or reuse an encoder
      for (int track : targetTracks) {
        AudioEncoder *encoderForTrack = nullptr;
        auto it = trackToEncoder.find(track);
        if (it != trackToEncoder.end()) {
          encoderForTrack = it->second;
          DLL_Log("MediaEngine::ReloadConfig Audio source %zu reusing encoder "
                  "for track %d",
                  i, track);
        } else {
          // Create new encoder for this track
          auto newEncoder = std::make_unique<AudioEncoder>();
          bool aRes = newEncoder->Init(
              audioConfig, [this](AVPacket *pkt) { this->WritePacket(pkt); });

          if (!aRes) {
            DLL_Log(
                "MediaEngine::ReloadConfig Audio encoder for track %d failed",
                track);
            continue;
          }

          videoEnc->AddAudioContext(audioConfig, newEncoder->GetCodecContext(),
                                    track);

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

          DLL_Log("MediaEngine::ReloadConfig Created new encoder for track %d "
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

          DLL_Log("MediaEngine::ReloadConfig Audio source %zu shares encoder "
                  "for track %d (type=%d)",
                  i, track, (int)audioConfig.sourceType);
          audioSources.push_back(std::move(source));
        }
      }
    }

    DLL_Log("MediaEngine: ReloadConfig complete. Audio sources: %zu, unique "
            "tracks: %zu",
            audioSources.size(), trackToEncoder.size());
  }

private:
  void AudioLoop() {
    DLL_Log("MediaEngine: Audio thread started (sources=%d)",
            (int)audioSources.size());
    int packetCount = 0;
    int mixCount = 0;

    // Check if any track has multiple sources (requires mixing)
    std::map<int, int> trackSourceCount;
    std::map<int, std::vector<size_t>>
        trackSourceIndices; // track -> source indices
    for (size_t i = 0; i < audioSources.size(); i++) {
      auto &src = audioSources[i];
      trackSourceCount[src.track]++;
      trackSourceIndices[src.track].push_back(i);
    }

    bool needsMixing = false;
    for (auto &kv : trackSourceCount) {
      if (kv.second > 1) {
        needsMixing = true;
        DLL_Log("AudioLoop: Track %d has %d sources - REAL mixing enabled",
                kv.first, kv.second);
      }
    }

    // Per-source sample buffers for mixing (float32 samples, interleaved
    // stereo)
    const int MIX_CHUNK_SAMPLES = 480; // 10ms at 48kHz
    const int CHANNELS = 2;
    const int CHUNK_SIZE = MIX_CHUNK_SAMPLES * CHANNELS;

    std::vector<std::vector<float>> sourceBuffers(audioSources.size());
    std::vector<std::unique_ptr<AudioResampler>> sourceResamplers(audioSources.size());
    std::vector<int64_t> sourceTimestamps(audioSources.size(), 0);
    std::map<int, int64_t> trackNextTimestamp; // Track continuous timestamps for mixing
    std::vector<AudioPacket> sourceLastPackets(audioSources.size());
    std::vector<std::chrono::steady_clock::time_point> lastPacketTime(
        audioSources.size(), std::chrono::steady_clock::now());

    while (audioRunning) {
      bool gotAnyPacket = false;
      auto now = std::chrono::steady_clock::now();

      // Step 1: Poll all sources and accumulate samples into buffers
      for (size_t srcIdx = 0; srcIdx < audioSources.size(); srcIdx++) {
        auto &src = audioSources[srcIdx];
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
          gotAnyPacket = true;
          packetCount++;
          sourceLastPackets[srcIdx] = packet;
          lastPacketTime[srcIdx] = now;
          if (sourceTimestamps[srcIdx] == 0) {
            sourceTimestamps[srcIdx] = packet.timestamp;
          }

          if (packetCount <= 3 || packetCount % 1000 == 0) {
            DLL_Log("AudioLoop: Packet #%d src=%d track=%d, size=%d",
                    packetCount, (int)srcIdx, src.track,
                    (int)packet.data.size());
          }

          // Use AudioResampler to standardize all sources to 48kHz Float Stereo
          // This creates a unified timeline for the mixer
          if (!sourceResamplers[srcIdx]) {
            sourceResamplers[srcIdx] = std::make_unique<AudioResampler>();
          }

          // Define target format for mixing (48kHz, Stereo, Float)
          AudioResampler::OutputFormat targetFmt;
          targetFmt.sampleRate = 48000;
          targetFmt.channels = 2;
          targetFmt.sampleFmt = AV_SAMPLE_FMT_FLT; // Packed float (interleaved)

          // Define input format from packet
          AudioResampler::InputFormat inputFmt;
          inputFmt.sampleRate = packet.sampleRate;
          inputFmt.channels = packet.channels;
          inputFmt.bitsPerSample = packet.bitsPerSample;
          inputFmt.validBitsPerSample = packet.validBitsPerSample > 0 ? packet.validBitsPerSample : packet.bitsPerSample;
          inputFmt.isFloat = packet.isFloat;
          inputFmt.blockAlign = (packet.channels * packet.bitsPerSample) / 8;

          // Initialize/Reinitialize resampler if needed
          if (!sourceResamplers[srcIdx]->IsReady() || 
               sourceResamplers[srcIdx]->GetOutputFormat().sampleRate != targetFmt.sampleRate) {
             sourceResamplers[srcIdx]->Init(inputFmt, targetFmt);
          }

          uint8_t **resampledData = nullptr;
          int outSamples = 0;
          
          if (sourceResamplers[srcIdx]->Process(packet.data.data(), (int)packet.data.size(), 
                                                &resampledData, &outSamples)) {
             if (outSamples > 0 && resampledData && resampledData[0]) {
                 int numFloats = outSamples * targetFmt.channels;
                 // OBSOLETE: sourceBuffers accumulation removed to prevent memory leak
                 // size_t oldSize = sourceBuffers[srcIdx].size();
                 // sourceBuffers[srcIdx].resize(oldSize + numFloats);
                 
                 // PULL MODEL (Phase 2): Write to Ring Buffer only
                 if (src.ringBuffer) {
                     size_t written = src.ringBuffer->Write((float*)resampledData[0], numFloats);
                     if (written < numFloats) {
                         // Only log overflow occasionally to avoid log spam
                         static int logCounter = 0;
                         if (logCounter++ % 100 == 0) {
                             DLL_Log("[AudioLoop] RingBuffer overflow: src=%d, requested=%d, written=%zu",
                                     (int)srcIdx, numFloats, written);
                         }
                     }
                 } else {
                     DLL_Log("[AudioLoop] ERROR: No RingBuffer for source %d", (int)srcIdx);
                 }
             }
             
             // CRITICAL FIX: Always free the buffer allocated by resampler, even if outSamples == 0
             // AudioResampler::Process allocates memory via av_samples_alloc... which must be freed.
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

    DLL_Log("MediaEngine: Audio thread stopped, processed %d packets, %d mixed "
            "chunks",
            packetCount, mixCount);
  }
};

static std::unique_ptr<MediaEngine> g_Engine;

extern "C" {

// Global Logger
static LogCallback g_LogCallback = nullptr;

MEDIAENGINE_API void DLL_Log(const char *fmt, ...) {
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

MEDIAENGINE_API bool MediaEngine_Init(const AppConfig *config) {
  DLL_Log("[Media] MediaEngine_Init Called. Version: %s (Built: %s)",
          CAPTURE_VERSION, BUILD_TIMESTAMP);
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

MEDIAENGINE_API void MediaEngine_ReloadConfig(const AppConfig *config) {
  if (g_Engine)
    g_Engine->ReloadConfig(config);
}

MEDIAENGINE_API void MediaEngine_StopRecording() {
  if (g_Engine)
    g_Engine->StopRecording();
}

MEDIAENGINE_API void MediaEngine_Shutdown() {
  if (g_Engine)
    g_Engine->StopRecording();
  g_Engine.reset();
}

MEDIAENGINE_API void MediaEngine_ProcessFrame(
    uint64_t textureHandle, uint64_t fenceHandle, uint64_t fenceValue,
    int64_t timestamp, int32_t luidLow, int32_t luidHigh, uint32_t sourcePid,
    uint32_t width, uint32_t height, uint32_t format, bool isHDR,
    bool isShmem, int shmemSlot) {
  if (g_Engine) {
    g_Engine->ProcessFrame(textureHandle, fenceHandle, fenceValue, timestamp,
                           luidLow, luidHigh, sourcePid, width, height, format,
                           isHDR, isShmem, shmemSlot);
  }
}
MEDIAENGINE_API void MediaEngine_ProcessFrameD3D11(void *texture,
                                                   int64_t timestamp,
                                                   uint32_t width,
                                                   uint32_t height) {
  if (g_Engine)
    g_Engine->ProcessFrameD3D11(texture, timestamp, width, height);
}

MEDIAENGINE_API bool
MediaEngine_CreateSharedCaptureTextures(uint32_t width, uint32_t height,
                                        uint32_t format,
                                        SharedMemoryLayout *sharedMem) {
  if (!g_Engine) {
    DLL_Log("[MediaEngine] CreateSharedCaptureTextures: Engine not ready");
    return false;
  }
  return g_Engine->CreateSharedCaptureTextures(width, height, format,
                                               sharedMem);
}

MEDIAENGINE_API int64_t MediaEngine_GetLastFrameEncodeTimeUs() {
  if (g_Engine) {
    return g_Engine->GetLastVideoEncodeTimeUs();
  }
  return 0;
}

MEDIAENGINE_API void MediaEngine_SetSharedMem(void *pSharedMem, void *pShmem) {
  if (g_Engine && g_Engine->videoEnc) {
    g_Engine->videoEnc->SetSharedMem((SharedMemoryLayout *)pSharedMem,
                                    (ShmemBuffer *)pShmem);
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
ID3D11Device *g_SharedD3D11Device = nullptr;
ID3D11DeviceContext *g_SharedD3D11Context = nullptr;

MEDIAENGINE_API ID3D11Device *MediaEngine_GetD3D11Device() {
  if (g_SharedD3D11Device)
    return g_SharedD3D11Device;

  // Create D3D11 device with video support
  D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_1,
                                       D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL featureLevel;

  UINT createFlags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
  createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 createFlags, featureLevels, 2,
                                 D3D11_SDK_VERSION, &g_SharedD3D11Device,
                                 &featureLevel, &g_SharedD3D11Context);

  if (FAILED(hr)) {
    DLL_Log("[MediaEngine] Failed to create shared D3D11 device: HR=0x%x", hr);
    return nullptr;
  }

  DLL_Log("[MediaEngine] Created shared D3D11 device (Feature Level: 0x%x)",
          featureLevel);
  return g_SharedD3D11Device;
}

// D3D11 Immediate Context Mutex
// Protects access to the immediate context shared between WGC thread and
// Encoder thread
std::recursive_mutex g_D3D11Mutex;

MEDIAENGINE_API void MediaEngine_LockD3D11() { g_D3D11Mutex.lock(); }

MEDIAENGINE_API void MediaEngine_UnlockD3D11() { g_D3D11Mutex.unlock(); }



} // extern "C"
