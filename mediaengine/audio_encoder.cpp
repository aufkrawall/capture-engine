#include "audio_encoder.h"
#include "mediaengine.h" // For DLL_Log

extern "C" {
#include <libavutil/audio_fifo.h>
}

AudioEncoder::AudioEncoder()
    : codecCtx(nullptr), resampler(nullptr), frame(nullptr), samplesCount(0),
      streamIndex(-1), initDone(false), audioFifo(nullptr),
      savedCodecId(AV_CODEC_ID_NONE), firstTimestamp(-1), recordingStartUs(0),
      recordingEndUs(0) {
  currentInputFormat = {};
}

AudioEncoder::~AudioEncoder() {
  // Stop flushes but doesn't free resources (for multi-recording support)
  Stop();

  // Now free everything - AudioResampler cleans itself up via unique_ptr
  resampler.reset();
  if (audioFifo) {
    av_audio_fifo_free(audioFifo);
    audioFifo = nullptr;
  }
  if (codecCtx) {
    avcodec_free_context(&codecCtx);
    codecCtx = nullptr;
  }
  if (frame) {
    av_frame_free(&frame);
    frame = nullptr;
  }
}

bool AudioEncoder::Init(const AudioConfig &config,
                        std::function<void(AVPacket *)> packetCallback) {
  onPacket = packetCallback;

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

  std::string codecName = config.codec.empty() ? "aac" : config.codec;
  const AVCodec *codec = avcodec_find_encoder_by_name(codecName.c_str());
  if (!codec) {
    DLL_Log("[AudioEncoder] Codec not found: %s", codecName.c_str());
    return false;
  }

  DLL_Log("[AudioEncoder] Using codec: %s (id=%d)", codecName.c_str(),
          codec->id);

  codecCtx = avcodec_alloc_context3(codec);
  if (!codecCtx) {
    DLL_Log("[AudioEncoder] Failed to allocate codec context");
    return false;
  }

  // ALAC requires specific sample format
  // ALAC requires specific sample format
  // For others (Opus, AAC), prefer Float > S16 > others
  if (codec->id == AV_CODEC_ID_ALAC) {
    codecCtx->sample_fmt = AV_SAMPLE_FMT_S32P;
  } else if (codec->sample_fmts) {
    // iterating to find best supported format
    const enum AVSampleFormat *p = codec->sample_fmts;
    enum AVSampleFormat best = AV_SAMPLE_FMT_NONE;

    // Preference list: Float Planar > Float > S16 Planar > S16 > S32 Planar
    enum AVSampleFormat preferences[] = {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT,
                                         AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_S16,
                                         AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_S32};

    for (auto pref : preferences) {
      p = codec->sample_fmts;
      while (*p != AV_SAMPLE_FMT_NONE) {
        if (*p == pref) {
          best = pref;
          break;
        }
        p++;
      }
      if (best != AV_SAMPLE_FMT_NONE)
        break;
    }

    if (best == AV_SAMPLE_FMT_NONE) {
      // Fallback to first
      best = codec->sample_fmts[0];
    }

    codecCtx->sample_fmt = best;
    DLL_Log("[AudioEncoder] Selected sample format: %d (%s)",
            codecCtx->sample_fmt, av_get_sample_fmt_name(codecCtx->sample_fmt));
  } else {
    // No sample formats listed - likely PCM or permissive encoder
    // Default to S16 for compatibility, or FLT if modern
    codecCtx->sample_fmt = AV_SAMPLE_FMT_S16;
    DLL_Log("[AudioEncoder] No sample formats listed, defaulting to S16");
  }

  codecCtx->bit_rate = config.bitrate * 1000;

  // Parse sample rate - "default" means use 48000, otherwise parse as int
  int sampleRate = 48000; // Default fallback
  if (!config.sampleRate.empty() && config.sampleRate != "default") {
    sampleRate = std::stoi(config.sampleRate);
  }
  codecCtx->sample_rate = sampleRate;

  // Check if sample rate is supported
  if (codec->supported_samplerates) {
    int best_rate = 0;
    for (int i = 0; codec->supported_samplerates[i]; i++) {
      if (codec->supported_samplerates[i] == codecCtx->sample_rate) {
        best_rate = codecCtx->sample_rate;
        break;
      }
      if (!best_rate ||
          abs(codec->supported_samplerates[i] - codecCtx->sample_rate) <
              abs(best_rate - codecCtx->sample_rate)) {
        best_rate = codec->supported_samplerates[i];
      }
    }
    if (best_rate != codecCtx->sample_rate) {
      DLL_Log("[AudioEncoder] Adjusting sample rate from %d to %d",
              codecCtx->sample_rate, best_rate);
      codecCtx->sample_rate = best_rate;
    }
  }

  // Channel Layout - Stereo (use av_channel_layout_copy for proper FFmpeg
  // handling)
  AVChannelLayout chLayout = AV_CHANNEL_LAYOUT_STEREO;
  av_channel_layout_copy(&codecCtx->ch_layout, &chLayout);

  DLL_Log("[AudioEncoder] Opening codec: %s sample_rate=%d sample_fmt=%d",
          codecName.c_str(), codecCtx->sample_rate, codecCtx->sample_fmt);

  // Allow experimental codecs (like Opus in some builds)
  codecCtx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

  int ret = avcodec_open2(codecCtx, codec, nullptr);
  if (ret < 0) {
    char errbuf[256];
    av_strerror(ret, errbuf, sizeof(errbuf));
    DLL_Log("[AudioEncoder] Failed to open codec: %d (%s)", ret, errbuf);
    avcodec_free_context(&codecCtx);
    return false;
  }

  // Save codec ID for recreation in Stop()
  savedCodecId = codec->id;

  DLL_Log("[AudioEncoder] Codec opened. frame_size=%d", codecCtx->frame_size);

  // Create audio FIFO buffer - size based on 2 seconds worth of audio
  // This scales with sample rate (e.g., 96kHz gets larger buffer than 48kHz)
  fifoCapacity = codecCtx->sample_rate * 2;  // 2 seconds buffer
  audioFifo = av_audio_fifo_alloc(codecCtx->sample_fmt,
                                  codecCtx->ch_layout.nb_channels, fifoCapacity);
  if (!audioFifo) {
    DLL_Log("[AudioEncoder] Failed to allocate audio FIFO");
    avcodec_free_context(&codecCtx);
    return false;
  }
  DLL_Log("[AudioEncoder] FIFO allocated: %d samples (%.2f sec at %dHz)",
          fifoCapacity, (float)fifoCapacity / codecCtx->sample_rate, 
          codecCtx->sample_rate);

  // Alloc frame for encoding
  frame = av_frame_alloc();
  if (!frame) {
    DLL_Log("[AudioEncoder] Failed to allocate frame");
    av_audio_fifo_free(audioFifo);
    avcodec_free_context(&codecCtx);
    return false;
  }

  // Set frame parameters
  // For ALAC and variable frame size codecs, frame_size might be 0
  int frame_size = codecCtx->frame_size;
  if (frame_size == 0) {
    frame_size = 4096; // Default frame size for variable codecs
  }

  frame->nb_samples = frame_size;
  frame->format = codecCtx->sample_fmt;
  av_channel_layout_copy(&frame->ch_layout, &codecCtx->ch_layout);
  frame->sample_rate = codecCtx->sample_rate;

  ret = av_frame_get_buffer(frame, 0);
  if (ret < 0) {
    char errbuf[256];
    av_strerror(ret, errbuf, sizeof(errbuf));
    DLL_Log("[AudioEncoder] Failed to allocate frame buffer: %s", errbuf);
    av_frame_free(&frame);
    av_audio_fifo_free(audioFifo);
    avcodec_free_context(&codecCtx);
    return false;
  }

  initDone = true;
  savedConfig = config; // Save for potential reinit
  lastPacketTimestampMs = 0; // Initialize timestamp tracker
  DLL_Log("[AudioEncoder] Initialization complete");
  return true;
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
}

// gptreport.md Section 5.1: Buffer audio packets until streamIndex is available
void AudioEncoder::SetStreamIndex(int index) {
  if (streamIndex == index)
    return;

  // Always log to trace the issue
  DLL_Log("[AudioEnc] SetStreamIndex called: current=%d new=%d pending=%zu",
          streamIndex, index, pendingPackets.size());

  int oldIndex = streamIndex;
  streamIndex = index;

  // Flush any buffered packets now that we have a valid stream
  if (oldIndex < 0 && streamIndex >= 0 && !pendingPackets.empty()) {
    DLL_Log("[AudioEnc] Flushing %zu buffered packets to stream %d",
            pendingPackets.size(), streamIndex);
    for (AVPacket *pkt : pendingPackets) {
      pkt->stream_index = streamIndex;
      if (onPacket) {
        onPacket(pkt);
      }
      av_packet_free(&pkt);
    }
    pendingPackets.clear();
  }
}

void AudioEncoder::EncodeSamples(const uint8_t *data, int sizeBytes,
                                 int channels, int sampleRate,
                                 int bitsPerSample, int validBitsPerSample,
                                 int blockAlign, bool isFloat,
                                 int64_t timestamp) {
  // If encoder was invalidated (reopen failed in Stop), try to reinit
  if (!initDone && !savedConfig.codec.empty()) {
    DLL_Log("[AudioEnc] Attempting reinit after previous failure");
    if (!Init(savedConfig, onPacket)) {
      DLL_Log("[AudioEnc] Reinit failed, cannot encode");
      return;
    }
  }

  if (!initDone || !codecCtx || !data || sizeBytes <= 0)
    return;

  // DEFERRED RESET: SetRecordingStart was called from video thread - apply reset now with logging
  if (needsReset) {
    int fifoSize = audioFifo ? av_audio_fifo_size(audioFifo) : 0;
    DLL_Log("[AudioEnc] RECORDING START RESET: startUs=%lld, OLD samplesCount=%lld, resampledTotal=%lld, FIFO=%d",
            (long long)pendingStartUs, (long long)samplesCount, (long long)resampledSamplesTotal, fifoSize);
    
    recordingStartUs = pendingStartUs;
    recordingEndUs = 0;
    firstTimestamp = -1;
    samplesCount = 0;  // CRITICAL: Reset to 0 for fresh start
    resampledSamplesTotal = 0;  // Reset resampler output counter
    lastPacketTimestampMs = 0;
    lastInputTimestamp = -1;  // CRITICAL: Reset to prevent false discontinuity detection
    if (audioFifo) av_audio_fifo_reset(audioFifo);
    if (resampler) resampler->Reset(); // FULL RESET (Clears buffers + drift)
    
    pendingStartUs = 0;
    needsReset = false;
    DLL_Log("[AudioEnc] Reset complete - audio will start from PTS=0 with fade-in");
  }

  // CRITICAL: Discard audio samples that arrive before first video frame
  // This ensures 0ms A/V sync - audio should not be encoded until video starts
  if (recordingStartUs == 0) {
    // Recording start not set yet (waiting for first video frame)
    // Silently discard this audio data
    return;
  }

  // CRITICAL: Discard audio samples that arrive after last video frame
  // This ensures audio track ends exactly when video ends
  // Check against recordingEndUs (using microsecond precision)
  int64_t timestampUs = timestamp * 1000;
  if (recordingEndUs > 0 && timestampUs > recordingEndUs) {
    // Recording has ended, discard this audio data
    return;
  }

  // Build input format descriptor
  AudioResampler::InputFormat inputFmt;
  inputFmt.channels = channels;
  inputFmt.sampleRate = sampleRate;
  inputFmt.bitsPerSample = bitsPerSample;
  inputFmt.validBitsPerSample = validBitsPerSample;
  inputFmt.isFloat = isFloat;
  inputFmt.blockAlign = blockAlign;

  // Initialize or reinitialize resampler if format changed
  bool needsInit = !resampler || !resampler->IsReady();
  if (!needsInit) {
    // Check if format changed
    needsInit =
        (currentInputFormat.channels != inputFmt.channels ||
         currentInputFormat.sampleRate != inputFmt.sampleRate ||
         currentInputFormat.bitsPerSample != inputFmt.bitsPerSample ||
         currentInputFormat.validBitsPerSample != inputFmt.validBitsPerSample ||
         currentInputFormat.isFloat != inputFmt.isFloat);
  }

  if (needsInit) {
    if (!resampler) {
      resampler = std::make_unique<AudioResampler>();
    }

    AudioResampler::OutputFormat outputFmt;
    outputFmt.channels = codecCtx->ch_layout.nb_channels;
    outputFmt.sampleRate = codecCtx->sample_rate;
    outputFmt.sampleFmt = codecCtx->sample_fmt;

    if (!resampler->Init(inputFmt, outputFmt)) {
      DLL_Log("[AudioEnc] Failed to init resampler");
      return;
    }

    currentInputFormat = inputFmt;
    DLL_Log(
        "[AudioEnc] Resampler initialized: %dHz %dch %s%d -> %dHz %dch fmt=%d",
        sampleRate, channels, isFloat ? "float" : "int", bitsPerSample,
        codecCtx->sample_rate, outputFmt.channels, (int)outputFmt.sampleFmt);
  }

  // Diagnostic: check if audio data is non-silent (reduced frequency to avoid log spam)
  static int diagCounter = 0;
  if (samplesCount == 0) {
    diagCounter = 0;
  }

  if (diagCounter++ % 5000 == 0 && isFloat && sizeBytes > 0) {
    const float *samples = reinterpret_cast<const float *>(data);
    float maxLevel = 0.0f;
    int checkCount = std::min(sizeBytes / 4, 100);
    for (int i = 0; i < checkCount; i++) {
      float absVal = samples[i] > 0 ? samples[i] : -samples[i];
      if (absVal > maxLevel)
        maxLevel = absVal;
    }
    DLL_Log("[AudioEnc] Audio level check: maxLevel=%.6f (first %d samples)",
            maxLevel, checkCount);
  }

  // Resample using AudioResampler
  uint8_t **resampledData = nullptr;
  int convertedSamples = 0;
  
  // NOTE: Clock drift compensation is now handled upstream in MediaEngine::PullAndEncodeAudio
  // using per-source syncResamplers. This block is disabled to prevent conflicts.
  // The videoTimeGetter callback is still available for monitoring if needed.
  #if 0
  // Apply clock drift compensation BEFORE resampling so it affects this batch
  // This adjusts the resampler ratio to produce fewer/more output samples
  // CRITICAL: Use resampledSamplesTotal (FIFO input) not samplesCount (encoder output)
  // because the FIFO decouples resampler from encoder and we need accurate feedback
  
  if (videoTimeGetter && resampler) {
    int64_t videoElapsedMs = videoTimeGetter();
    if (videoElapsedMs > 0) {
      // THRESHOLD DEFINITIONS (harmonized across all drift detection):
      // - BACKLOG_THRESHOLD: 100ms - If audio is more than 100ms behind video,
      //   we're processing a backlog (queue lag) and should NOT pitch-shift.
      // - CONTINUITY_TOLERANCE: 100ms - If input timestamp jumps by >100ms,
      //   it's a true gap (data missing), not just processing delay.
      const int64_t BACKLOG_THRESHOLD_SAMPLES = av_rescale(100, codecCtx->sample_rate, 1000);
      
      int64_t expectedSamples = av_rescale(videoElapsedMs, codecCtx->sample_rate, 1000);
      int64_t gapSamples = expectedSamples - resampledSamplesTotal;

      // If Gap is HUGE (>100ms), we are likely processing backlog (catching up).
      // We should NOT pitch shift in this case, because we are naturally catching up 
      // by processing samples faster than real-time. Pitch shifting here causes "Doppler" artifacts.
      bool isSteadyState = (abs(gapSamples) < BACKLOG_THRESHOLD_SAMPLES);
      
      if (isSteadyState) {
          // Use resampledSamplesTotal for accurate drift measurement
          resampler->AdjustForClockDrift(videoElapsedMs, resampledSamplesTotal);
      }
    }
  }
  #endif
  
  if (!resampler->Process(data, sizeBytes, &resampledData, &convertedSamples)) {
    DLL_Log("[AudioEnc] Resample failed");
    return;
  }

  if (convertedSamples <= 0) {
    AudioResampler::FreeOutputBuffer(resampledData);
    return;
  }

  // Track cumulative resampler output for drift calculation
  resampledSamplesTotal += convertedSamples;

  // Write resampled data to FIFO
  int ret =
      av_audio_fifo_write(audioFifo, (void **)resampledData, convertedSamples);
  
  // Apply 50ms fade-in to the new samples if at the very start of recording
  // 50ms @ 48kHz = 2400 samples
  const int FADE_SAMPLES = codecCtx->sample_rate / 20;
  if (samplesCount < FADE_SAMPLES) {
      int samplesToFade = convertedSamples;
      int channels = codecCtx->ch_layout.nb_channels;
      
      // If planar, we must handle all planes
      int numPlanes = av_sample_fmt_is_planar(codecCtx->sample_fmt) ? channels : 1;
      int planeSamples = av_sample_fmt_is_planar(codecCtx->sample_fmt) ? 1 : channels;

      for (int p = 0; p < numPlanes; p++) {
          if (codecCtx->sample_fmt == AV_SAMPLE_FMT_FLT || codecCtx->sample_fmt == AV_SAMPLE_FMT_FLTP) {
              float* fData = (float*)resampledData[p];
              for (int i = 0; i < samplesToFade; i++) {
                  int64_t currentSmp = samplesCount + i;
                  float gain = (float)currentSmp / FADE_SAMPLES;
                  if (gain > 1.0f) gain = 1.0f;
                  if (gain < 0.0f) gain = 0.0f;
                  
                  if (numPlanes == 1) { // Interleaved
                      for (int c = 0; c < channels; c++) fData[i * channels + c] *= gain;
                  } else { // Planar
                      fData[i] *= gain;
                  }
              }
          } else if (codecCtx->sample_fmt == AV_SAMPLE_FMT_S16 || codecCtx->sample_fmt == AV_SAMPLE_FMT_S16P) {
              int16_t* sData = (int16_t*)resampledData[p];
              for (int i = 0; i < samplesToFade; i++) {
                  int64_t currentSmp = samplesCount + i;
                  float gain = (float)currentSmp / FADE_SAMPLES;
                  if (gain > 1.0f) gain = 1.0f;
                  
                  if (numPlanes == 1) {
                      for (int c = 0; c < channels; c++) sData[i * channels + c] = (int16_t)(sData[i * channels + c] * gain);
                  } else {
                      sData[i] = (int16_t)(sData[i] * gain);
                  }
              }
          } else if (codecCtx->sample_fmt == AV_SAMPLE_FMT_S32 || codecCtx->sample_fmt == AV_SAMPLE_FMT_S32P) {
              int32_t* sData = (int32_t*)resampledData[p];
              for (int i = 0; i < samplesToFade; i++) {
                  int64_t currentSmp = samplesCount + i;
                  float gain = (float)currentSmp / FADE_SAMPLES;
                  if (gain > 1.0f) gain = 1.0f;
                  
                  if (numPlanes == 1) {
                      for (int c = 0; c < channels; c++) sData[i * channels + c] = (int32_t)(sData[i * channels + c] * gain);
                  } else {
                      sData[i] = (int32_t)(sData[i] * gain);
                  }
              }
          }
      }
  }

  if (ret < convertedSamples) {
    DLL_Log("[AudioEnc] Failed to write to audio FIFO");
  }

  AudioResampler::FreeOutputBuffer(resampledData);
  
  // SAFETY: Check FIFO size to prevent unbounded growth (Memory Leak Protection)
  // If FIFO grows > 5 seconds, something is broken (encoder stalled or input too fast).
  // 5 seconds @ 48kHz = 240,000 samples. 
  const int MAX_FIFO_SAMPLES = codecCtx->sample_rate * 5; 
  int currentFifoSize = av_audio_fifo_size(audioFifo);
  
  if (currentFifoSize > MAX_FIFO_SAMPLES) {
      DLL_Log("[AudioEnc] CRITICAL: FIFO Overflow (%d samples). Resetting to prevent memory leak.", currentFifoSize);
      av_audio_fifo_reset(audioFifo);
      // We must also reset timestamps/counters? No, just drop the buffered audio.
      // Sync logic below will handle the gaps.
  }

  // NOTE: Gap detection REMOVED.
  // The MediaEngine pull model handles all timing by pulling exact sample counts
  // based on video timeline. This encoder just encodes what it receives.
  // No HARD RESYNC, no warping - just simple PTS = samplesCount.
  
  if (firstTimestamp < 0 && recordingStartUs > 0) {
    firstTimestamp = timestamp;
    
    // Simple reset: counters should already be 0 from deferred reset.
    // Just log and FIFO clear as safety.
    if (samplesCount != 0 || resampledSamplesTotal != 0) {
      DLL_Log("[AudioEnc] First packet safety reset: samplesCount=%lld -> 0, resampledTotal=%lld -> 0", 
              (long long)samplesCount, (long long)resampledSamplesTotal);
      samplesCount = 0;
      resampledSamplesTotal = 0;
      if (resampler) resampler->ResetClockTracking();
      if (audioFifo) {
          av_audio_fifo_reset(audioFifo);
      }
    }
    
    DLL_Log("[AudioEnc] First audio packet processing (PTS=0)");
  }

  // Track latest packet timestamp for PTS calculation
  // This ensures audio PTS uses the same clock source as video (QPC)
  lastPacketTimestampMs = timestamp;

  // Encode while we have enough samples
  int frame_size = codecCtx->frame_size;
  if (frame_size == 0) {
    frame_size = 4096; // Default for variable codecs
  }

  // Periodic FIFO status (reduced frequency to avoid log spam)
  static int logCounter = 0;
  if (logCounter++ % 5000 == 0) {
    int currentSize = av_audio_fifo_size(audioFifo);
    
    // Warn if over 50% capacity (approx 1-2 seconds depending on sample rate)
    if (currentSize > fifoCapacity / 2) {
        DLL_Log("[AudioEnc] WARN: Audio FIFO high: %d/%d samples", currentSize, fifoCapacity);
    } else {
        DLL_Log("[AudioEnc] FIFO size=%d, frame_size=%d", currentSize, frame_size);
    }
  }

  while (av_audio_fifo_size(audioFifo) >= frame_size) {
    // Make frame writable
    ret = av_frame_make_writable(frame);
    if (ret < 0) {
      DLL_Log("[AudioEnc] Failed to make frame writable: %d", ret);
      return;
    }

    frame->nb_samples = frame_size;

    // Read from FIFO into frame
    ret = av_audio_fifo_read(audioFifo, (void **)frame->data, frame_size);
    if (ret < frame_size) {
      DLL_Log("[AudioEnc] Failed to read from FIFO: got %d, expected %d", ret,
              frame_size);
      return;
    }

    // Set PTS using simple sample counting from 0 (CFR audio)
    // This matches how video PTS is calculated - simple frame counting
    // Video: pts = frame_number (0, 1, 2, 3...)
    // Audio: pts = samples_encoded (0, 4096, 8192...)
    // Both are constant-rate clocks that stay perfectly synchronized
    frame->pts = samplesCount;
    
    // Debug: Log first 10 frames for each encoder to track PTS
    static int frameLogCounter = 0;
    if (frameLogCounter++ < 10) {
      DLL_Log("[AudioEnc] FRAME PTS DEBUG: pts=%lld (%.3f sec) samplesCount=%lld streamIdx=%d",
              (long long)frame->pts, 
              (double)frame->pts / codecCtx->sample_rate,
              (long long)samplesCount,
              streamIndex);
    }
    
    samplesCount += frame_size;

    // Encode frame
    ret = avcodec_send_frame(codecCtx, frame);
    if (ret < 0) {
      char errbuf[256];
      av_strerror(ret, errbuf, sizeof(errbuf));
      DLL_Log("[AudioEnc] avcodec_send_frame failed: %s (code=%d)", errbuf,
              ret);
      continue;
    }

    // Receive packets
    int pktCount = 0;
    while (true) {
      AVPacket *pkt = av_packet_alloc();
      ret = avcodec_receive_packet(codecCtx, pkt);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        av_packet_free(&pkt);
        break;
      }
      if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        DLL_Log("[AudioEnc] avcodec_receive_packet failed: %s", errbuf);
        av_packet_free(&pkt);
        break;
      }

      pktCount++;
      // Buffer packets until stream index is set (gptreport.md Section 5.1)
      // Otherwise we'd write audio packets with wrong stream index
      if (streamIndex < 0) {
        static bool warnedOnce = false;
        if (!warnedOnce) {
          DLL_Log(
              "[AudioEnc] Buffering audio packets - stream not yet assigned");
          warnedOnce = true;
        }
        // Clone packet and add to pending buffer instead of dropping
        // Limit pending buffer size to prevent unbounded growth
        static const size_t MAX_PENDING_PACKETS = 1000;
        if (pendingPackets.size() >= MAX_PENDING_PACKETS) {
          static bool warnedMax = false;
          if (!warnedMax) {
            DLL_Log("[AudioEnc] WARNING: Pending packet buffer full (%zu), "
                    "dropping oldest packet", MAX_PENDING_PACKETS);
            warnedMax = true;
          }
          AVPacket* oldest = pendingPackets.front();
          av_packet_free(&oldest);
          pendingPackets.erase(pendingPackets.begin());
        }
        AVPacket *cloned = av_packet_clone(pkt);
        if (cloned) {
          pendingPackets.push_back(cloned);
        }
        av_packet_free(&pkt);
        continue;
      }

      // Success - call callback with correct stream index
      pkt->stream_index = streamIndex;
      if (onPacket) {
        onPacket(pkt);
      } else {
        DLL_Log("[AudioEnc] WARNING: onPacket callback is NULL!");
      }
      av_packet_free(&pkt);
    }

    // Log if we didn't get any packets after sending a frame
    static int noPacketCount = 0;
    if (pktCount == 0) {
      noPacketCount++;
      if (noPacketCount % 100 == 1) {
        DLL_Log("[AudioEnc] No packets received after send_frame (count=%d)",
                noPacketCount);
      }
    }
  }
}

void AudioEncoder::Stop() {
  if (initDone) {
    Flush();
  }

  // Only drain the audio FIFO to start fresh for next recording
  if (audioFifo) {
    av_audio_fifo_reset(audioFifo);
  }

  // Reset all state for next recording
  DLL_Log(
      "[AudioEnc] Stop: Resetting state (samplesCount=%lld, streamIndex=%d)",
      (long long)samplesCount, streamIndex);

  samplesCount = 0;
  streamIndex = -1; // Critical for next run
  firstTimestamp = -1;
  recordingStartUs = 0;
  recordingEndUs = 0;
  resampledSamplesTotal = 0;  // Reset resampler output counter for next recording
  lastInputTimestamp = -1;    // Reset continuity tracking
  lastPacketTimestampMs = 0;  // Reset PTS tracker

  // Clear any pending packets
  for (auto *pkt : pendingPackets) {
    av_packet_free(&pkt);
  }
  pendingPackets.clear();

  // Flush resampler buffers if they exist
  if (resampler) {
    // Recreating it is safest to clear internal buffers
    resampler.reset();
  }

  // CRITICAL: ALAC and other encoders don't reset properly with
  // avcodec_flush_buffers() We must recreate the codec context entirely for
  // each new recording. This is similar to how VideoEncoder recreates its codec
  // context.
  if (codecCtx && savedCodecId != AV_CODEC_ID_NONE) {
    // Save settings we need to restore
    int sampleRate = codecCtx->sample_rate;
    AVSampleFormat sampleFmt = codecCtx->sample_fmt;
    AVChannelLayout chLayout;
    av_channel_layout_default(&chLayout, 2); // Init to default first

    // Try to copy from existing context if valid
    if (codecCtx->ch_layout.nb_channels > 0) {
      av_channel_layout_uninit(&chLayout); // Clear default
      if (av_channel_layout_copy(&chLayout, &codecCtx->ch_layout) < 0) {
        DLL_Log(
            "[AudioEnc] Failed to copy channel layout, using default stereo");
        av_channel_layout_default(&chLayout, 2);
      }
    } else {
      DLL_Log("[AudioEnc] Existing channel layout invalid (channels=0), using "
              "default stereo");
    }
    int64_t bitRate = codecCtx->bit_rate;

    // Free old context
    avcodec_free_context(&codecCtx);

    // Find codec and recreate
    // Find codec and recreate
    const AVCodec *codec = nullptr;
    if (!savedCodecName.empty()) {
      codec = avcodec_find_encoder_by_name(savedCodecName.c_str());
    }
    if (!codec) {
      codec = avcodec_find_encoder(savedCodecId); // Fallback
    }

    if (codec) {
      codecCtx = avcodec_alloc_context3(codec);
      if (codecCtx) {
        // Restore settings
        codecCtx->sample_rate = sampleRate;
        codecCtx->sample_fmt = sampleFmt;
        // Correctly copy channel layout into new context
        av_channel_layout_copy(&codecCtx->ch_layout, &chLayout);
        codecCtx->bit_rate = bitRate;
        // Don't set time_base manually - let avcodec_open2 handle it or default
        // it, just like Init() codecCtx->time_base = {1, sampleRate};

        // CRITICAL: Allow experimental codecs (Opus) when recreating context
        codecCtx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

        DLL_Log(
            "[AudioEnc] Reopening: rate=%d, fmt=%d, channels=%d, bitrate=%lld",
            sampleRate, sampleFmt, chLayout.nb_channels, (long long)bitRate);

        int ret = avcodec_open2(codecCtx, codec, nullptr);
        if (ret < 0) {
          char errbuf[256];
          av_strerror(ret, errbuf, sizeof(errbuf));
          DLL_Log("[AudioEnc] Failed to reopen codec in Stop: %s", errbuf);
          // Mark as not initialized so we don't try to use broken context
          initDone = false;
          avcodec_free_context(&codecCtx);
          codecCtx = nullptr;
        } else {
          DLL_Log("[AudioEnc] Codec recreated successfully for next recording");
        }
      }
    }
    av_channel_layout_uninit(&chLayout);
  }

  // Note: codecCtx is preserved for VideoEncoder::savedAudioCodecCtx
  // Everything is kept for reuse in subsequent recordings
}

void AudioEncoder::Flush() {
  if (!initDone || !codecCtx)
    return;

  DLL_Log("[AudioEncoder] Flushing encoder...");

  // Calculate maximum samples to encode based on video duration
  // This ensures audio ends exactly when video ends (Microsecond precision)
  int64_t maxSamples = INT64_MAX; // No limit by default
  if (recordingEndUs > 0 && recordingStartUs > 0 && recordingEndUs > recordingStartUs) {
    int64_t durationUs = recordingEndUs - recordingStartUs;
    
    // av_rescale_rnd for precise sample count from microseconds
    // Use AV_ROUND_UP to ensure audio is never shorter than video
    maxSamples = av_rescale_rnd(durationUs, codecCtx->sample_rate, 1000000, AV_ROUND_UP);
    
    DLL_Log("[AudioEncoder] Video duration: %lld us, max audio samples: %lld, current: %lld",
            durationUs, maxSamples, samplesCount);
  }

  const int fixedFrameSize = codecCtx->frame_size;
  if (fixedFrameSize > 0 && maxSamples != INT64_MAX) {
    int64_t rem = maxSamples % fixedFrameSize;
    if (rem != 0) {
      maxSamples += (fixedFrameSize - rem);
    }
  }

  auto drainPackets = [&]() {
    while (true) {
      AVPacket *pkt = av_packet_alloc();
      int dret = avcodec_receive_packet(codecCtx, pkt);
      if (dret == AVERROR(EAGAIN) || dret == AVERROR_EOF) {
        av_packet_free(&pkt);
        break;
      }
      if (dret < 0) {
        av_packet_free(&pkt);
        break;
      }
      pkt->stream_index = streamIndex;
      if (onPacket)
        onPacket(pkt);
      av_packet_free(&pkt);
    }
  };

  // Encode any remaining samples in FIFO (up to maxSamples limit)
  int fifoSize = audioFifo ? av_audio_fifo_size(audioFifo) : 0;
  if (fifoSize > 0 && frame) {
    int frame_size = codecCtx->frame_size ? codecCtx->frame_size : 4096;
    int sampleSize = av_get_bytes_per_sample(codecCtx->sample_fmt);
    int channels = codecCtx->ch_layout.nb_channels;
    bool planar = av_sample_fmt_is_planar(codecCtx->sample_fmt) != 0;
    int numPlanes = planar ? channels : 1;

    while (fifoSize > 0) {
      int64_t remainingAllowed = maxSamples - samplesCount;
      if (remainingAllowed <= 0) {
        DLL_Log("[AudioEncoder] Already at sample limit, discarding %d buffered samples", fifoSize);
        av_audio_fifo_reset(audioFifo);
        break;
      }

      int samplesToRead = (int)std::min<int64_t>((int64_t)fifoSize, std::min<int64_t>((int64_t)frame_size, remainingAllowed));
      int samplesToSend = (codecCtx->frame_size > 0) ? codecCtx->frame_size : samplesToRead;

      if (samplesToSend <= 0)
        break;

      if (frame->nb_samples != samplesToSend) {
        av_frame_unref(frame);
        frame->nb_samples = samplesToSend;
        frame->format = codecCtx->sample_fmt;
        av_channel_layout_copy(&frame->ch_layout, &codecCtx->ch_layout);
        frame->sample_rate = codecCtx->sample_rate;
        int bret = av_frame_get_buffer(frame, 0);
        if (bret < 0) {
          break;
        }
      }

      int ret = av_frame_make_writable(frame);
      if (ret < 0) {
        break;
      }

      int rret = av_audio_fifo_read(audioFifo, (void **)frame->data, samplesToRead);
      if (rret < samplesToRead) {
        break;
      }

      if (samplesToSend > samplesToRead) {
        int framePlaneStride = sampleSize * (planar ? 1 : channels);
        int padBytes = (samplesToSend - samplesToRead) * framePlaneStride;
        for (int p = 0; p < numPlanes && frame->data[p]; p++) {
          uint8_t *dst = (uint8_t *)frame->data[p] + (samplesToRead * framePlaneStride);
          memset(dst, 0, padBytes);
        }
      }

      if (samplesToRead == fifoSize) {
        const int FADE_SAMPLES = codecCtx->sample_rate / 20;
        int fadeStart = std::max(0, samplesToRead - FADE_SAMPLES);

        for (int p = 0; p < numPlanes && frame->data[p]; p++) {
          if (codecCtx->sample_fmt == AV_SAMPLE_FMT_FLT || codecCtx->sample_fmt == AV_SAMPLE_FMT_FLTP) {
            float *fData = (float *)frame->data[p];
            for (int i = fadeStart; i < samplesToRead; i++) {
              float fadePos = (float)(samplesToRead - 1 - i) / FADE_SAMPLES;
              float gain = fadePos < 1.0f ? fadePos : 1.0f;
              if (numPlanes == 1) {
                for (int c = 0; c < channels; c++)
                  fData[i * channels + c] *= gain;
              } else {
                fData[i] *= gain;
              }
            }
          } else if (codecCtx->sample_fmt == AV_SAMPLE_FMT_S16 || codecCtx->sample_fmt == AV_SAMPLE_FMT_S16P) {
            int16_t *sData = (int16_t *)frame->data[p];
            for (int i = fadeStart; i < samplesToRead; i++) {
              float fadePos = (float)(samplesToRead - 1 - i) / FADE_SAMPLES;
              float gain = fadePos < 1.0f ? fadePos : 1.0f;
              if (numPlanes == 1) {
                for (int c = 0; c < channels; c++)
                  sData[i * channels + c] = (int16_t)(sData[i * channels + c] * gain);
              } else {
                sData[i] = (int16_t)(sData[i] * gain);
              }
            }
          } else if (codecCtx->sample_fmt == AV_SAMPLE_FMT_S32 || codecCtx->sample_fmt == AV_SAMPLE_FMT_S32P) {
            int32_t *sData = (int32_t *)frame->data[p];
            for (int i = fadeStart; i < samplesToRead; i++) {
              float fadePos = (float)(samplesToRead - 1 - i) / FADE_SAMPLES;
              float gain = fadePos < 1.0f ? fadePos : 1.0f;
              if (numPlanes == 1) {
                for (int c = 0; c < channels; c++)
                  sData[i * channels + c] = (int32_t)(sData[i * channels + c] * gain);
              } else {
                sData[i] = (int32_t)(sData[i] * gain);
              }
            }
          }
        }
      }

      frame->pts = samplesCount;
      samplesCount += samplesToSend;

      ret = avcodec_send_frame(codecCtx, frame);
      if (ret == AVERROR(EAGAIN)) {
        drainPackets();
        ret = avcodec_send_frame(codecCtx, frame);
      }
      if (ret < 0) {
        break;
      }
      drainPackets();

      fifoSize = av_audio_fifo_size(audioFifo);
    }
  }

  // Padding: If audio is shorter than video (due to dropouts at the end), fill with silence
  // Also calculate exact discard padding for sample-accurate end trimming
  int64_t discardPaddingSamples = 0;
  if (recordingEndUs > 0 && maxSamples != INT64_MAX) {
      int64_t samplesNeeded = maxSamples - samplesCount;
      if (samplesNeeded > 0) {
          DLL_Log("[AudioEncoder] Padding stream end: %lld samples silence needed", samplesNeeded);
          
          int frame_size = codecCtx->frame_size ? codecCtx->frame_size : 4096;
          
          // Allocate a silence buffer (zeroed) - reuse frame since we drained FIFO
          int ret = av_frame_make_writable(frame);
          if (ret >= 0) {
              int sampleSize = av_get_bytes_per_sample(codecCtx->sample_fmt);
              int channels = codecCtx->ch_layout.nb_channels;
              
              while (samplesNeeded > 0) {
                  // For most encoders, we MUST send exactly frame_size samples.
                  // If we need fewer, we still send a full frame to be safe.
                  int samplesToPrepare = (int)std::min((int64_t)frame_size, samplesNeeded);
                  
                  // If fixed frame size, always send full frame
                  int samplesToSend = (codecCtx->frame_size > 0) ? codecCtx->frame_size : samplesToPrepare;
                  
                  frame->nb_samples = samplesToSend;
                  
                  // Zero out the entire frame buffer (padding extra if needed)
                  for (int i = 0; i < AV_NUM_DATA_POINTERS && frame->data[i]; i++) {
                      int planeSize = samplesToSend * sampleSize;
                      if (!av_sample_fmt_is_planar(codecCtx->sample_fmt)) {
                          planeSize *= channels; // Interleaved
                      }
                      memset(frame->data[i], 0, planeSize);
                  }
                  
                  frame->pts = samplesCount;
                  samplesCount += samplesToSend;
                  samplesNeeded -= samplesToSend;
                  
                  ret = avcodec_send_frame(codecCtx, frame);
                  if (ret == AVERROR(EAGAIN)) {
                      // Drain loop inline
                      while (true) {
                          AVPacket *pkt = av_packet_alloc();
                          int dret = avcodec_receive_packet(codecCtx, pkt);
                          if (dret < 0) {
                              av_packet_free(&pkt);
                              break;
                          }
                          pkt->stream_index = streamIndex;
                          if (onPacket) onPacket(pkt);
                          av_packet_free(&pkt);
                      }
                      // Retry sending the frame
                      ret = avcodec_send_frame(codecCtx, frame);
                  }
                  
                  if (ret < 0) {
                      DLL_Log("[AudioEncoder] Error sending silence frame: %d", ret);
                      break;
                  }

                  // Drain packets after each frame to keep buffers moving
                  drainPackets();
              }
          }
      }
      
      // Calculate exact discard padding for sample-accurate trimming
      // We aligned maxSamples to frame_size boundary, so we may have extra samples
      int64_t durationUs = recordingEndUs - recordingStartUs;
      int64_t targetSamples = av_rescale_rnd(durationUs, codecCtx->sample_rate, 1000000, AV_ROUND_DOWN);
      discardPaddingSamples = samplesCount - targetSamples;
      
      if (discardPaddingSamples > 0 && discardPaddingSamples < codecCtx->frame_size) {
          DLL_Log("[AudioEncoder] Setting trailing_padding for sample-accurate end: %lld samples (target=%lld, actual=%lld)",
                  discardPaddingSamples, targetSamples, samplesCount);
          codecCtx->trailing_padding = (int)discardPaddingSamples;
      }
  }

  // NOTE: Do NOT send NULL frame here! That puts encoder in permanent EOF
  // state. For multi-recording support, we just drain remaining packets without
  // sending EOF. The NULL frame should only be sent in destructor when truly
  // closing forever.

  // Final drain
  while (true) {
    AVPacket *pkt = av_packet_alloc();
    int ret = avcodec_receive_packet(codecCtx, pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      av_packet_free(&pkt);
      break;
    }
    if (ret < 0) {
      av_packet_free(&pkt);
      break;
    }
    pkt->stream_index =
        streamIndex; // Ensure correct stream index for flushed packets
    if (onPacket) {
      onPacket(pkt);
    }
    av_packet_free(&pkt);
  }

  DLL_Log("[AudioEncoder] Flush complete");
}
