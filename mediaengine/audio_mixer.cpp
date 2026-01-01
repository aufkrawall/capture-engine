#include "audio_mixer.h"
#include "mediaengine.h" // For DLL_Log
#include <algorithm>
#include <cmath>
#include <cstring>

AudioMixer::AudioMixer() { sources.resize(MAX_SOURCES); }

AudioMixer::~AudioMixer() { Reset(); }

bool AudioMixer::Init(const OutputFormat &outFmt) {
  outputFormat = outFmt;

  // Allocate initial mix buffer (1 second worth)
  int bufferSize = outputFormat.sampleRate * outputFormat.channels;
  mixBuffer.resize(bufferSize);
  mixBufferSamples = 0;

  initialized = true;
  DLL_Log("[AudioMixer] Initialized: %dHz %dch", outputFormat.sampleRate,
          outputFormat.channels);
  return true;
}

void AudioMixer::Reset() {
  for (auto &src : sources) {
    src.resampler.reset();
    src.active = false;
  }
  mixBuffer.clear();
  mixBufferSamples = 0;
  initialized = false;
}

AudioMixer::SourceState &AudioMixer::GetSourceState(int sourceId) {
  if (sourceId < 0)
    sourceId = 0;
  if (sourceId >= MAX_SOURCES)
    sourceId = MAX_SOURCES - 1;
  return sources[sourceId];
}

bool AudioMixer::AddSamples(int sourceId, const uint8_t *data, int sizeBytes,
                            int channels, int sampleRate, int bitsPerSample,
                            int validBitsPerSample, int blockAlign,
                            bool isFloat) {
  if (!initialized || !data || sizeBytes <= 0) {
    return false;
  }

  SourceState &src = GetSourceState(sourceId);

  // Build input format
  AudioResampler::InputFormat inputFmt;
  inputFmt.channels = channels;
  inputFmt.sampleRate = sampleRate;
  inputFmt.bitsPerSample = bitsPerSample;
  inputFmt.validBitsPerSample = validBitsPerSample;
  inputFmt.blockAlign = blockAlign;
  inputFmt.isFloat = isFloat;

  // Check if we need to create/reinit resampler
  bool needsInit = !src.resampler || !src.resampler->IsReady();
  if (!needsInit && src.active) {
    needsInit = (src.lastInputFormat.channels != inputFmt.channels ||
                 src.lastInputFormat.sampleRate != inputFmt.sampleRate ||
                 src.lastInputFormat.bitsPerSample != inputFmt.bitsPerSample ||
                 src.lastInputFormat.validBitsPerSample !=
                     inputFmt.validBitsPerSample ||
                 src.lastInputFormat.isFloat != inputFmt.isFloat);
  }

  if (needsInit) {
    if (!src.resampler) {
      src.resampler = std::make_unique<AudioResampler>();
    }

    AudioResampler::OutputFormat outFmt;
    outFmt.channels = outputFormat.channels;
    outFmt.sampleRate = outputFormat.sampleRate;
    outFmt.sampleFmt = AV_SAMPLE_FMT_FLT; // Mix in float32 for quality

    if (!src.resampler->Init(inputFmt, outFmt)) {
      DLL_Log("[AudioMixer] Failed to init resampler for source %d", sourceId);
      return false;
    }

    src.lastInputFormat = inputFmt;
    src.active = true;
    DLL_Log("[AudioMixer] Source %d initialized: %dHz %dch -> %dHz %dch",
            sourceId, sampleRate, channels, outputFormat.sampleRate,
            outputFormat.channels);
  }

  // Resample to output format
  uint8_t **resampledData = nullptr;
  int convertedSamples = 0;
  if (!src.resampler->Process(data, sizeBytes, &resampledData,
                              &convertedSamples)) {
    DLL_Log("[AudioMixer] Resample failed for source %d", sourceId);
    return false;
  }

  if (convertedSamples <= 0) {
    AudioResampler::FreeOutputBuffer(resampledData);
    return true; // Not an error, just no output yet
  }

  // Add to mix buffer
  const float *resampledFloat =
      reinterpret_cast<const float *>(resampledData[0]);
  int totalFrames = convertedSamples;
  int totalSamples = totalFrames * outputFormat.channels;

  // Ensure mix buffer is large enough
  int requiredSize = (mixBufferSamples + totalFrames) * outputFormat.channels;
  if ((int)mixBuffer.size() < requiredSize) {
    mixBuffer.resize(requiredSize * 2); // Double for headroom
  }

  // Mix (sum) samples into buffer
  // Optimized single-pass loops
  float *mixPtr = mixBuffer.data() + (mixBufferSamples * outputFormat.channels);
  
  // 1. Overlap region (add to existing)
  int overlapSamples = std::min(totalSamples, (int)mixBuffer.size() - (mixBufferSamples * outputFormat.channels));
  if (overlapSamples > 0) {
      for (int i = 0; i < overlapSamples; i++) {
        mixPtr[i] += resampledFloat[i];
      }
  }
  
  // 2. Extension region (copy new samples)
  int extensionSamples = totalSamples - overlapSamples;
  if (extensionSamples > 0) {
      if ((int)mixBuffer.size() < (mixBufferSamples * outputFormat.channels) + totalSamples) {
          // Should have been resized already, but safety check
          // Just clamp if something went wrong
          extensionSamples = (int)mixBuffer.size() - (mixBufferSamples * outputFormat.channels) - overlapSamples;
      }
      if (extensionSamples > 0) {
          float* destPtr = mixPtr + overlapSamples;
          const float* srcPtr = resampledFloat + overlapSamples;
          std::memcpy(destPtr, srcPtr, extensionSamples * sizeof(float));
      }
  }

  // Update buffer position - extend by the number of new frames added
  // Note: This simple approach works when sources are time-synchronized.
  // For true multi-source mixing with overlapping timestamps, we'd need
  // timestamp-based alignment, but that's out of scope for current use.
  mixBufferSamples += totalFrames;

  AudioResampler::FreeOutputBuffer(resampledData);
  return true;
}

int AudioMixer::GetMixedSamples(float *output, int maxSamples) {
  if (!initialized || mixBufferSamples == 0 || !output) {
    return 0;
  }

  int samplesToReturn = std::min(maxSamples, mixBufferSamples);
  int totalSamples = samplesToReturn * outputFormat.channels;

  // Apply soft clipping / normalization to prevent clipping
  // Simple approach: find peak and normalize if > 1.0
  float peak = 0.0f;
  for (int i = 0; i < totalSamples; i++) {
    float absVal = std::fabs(mixBuffer[i]);
    if (absVal > peak)
      peak = absVal;
  }

  float gain = 1.0f;
  if (peak > 1.0f) {
    gain = 1.0f / peak;
    DLL_Log("[AudioMixer] Normalizing: peak=%.2f, gain=%.2f", peak, gain);
  }

  // Copy with gain
  for (int i = 0; i < totalSamples; i++) {
    output[i] = mixBuffer[i] * gain;
  }

  // Shift remaining samples
  int remainingSamples =
      (mixBufferSamples - samplesToReturn) * outputFormat.channels;
  if (remainingSamples > 0) {
    std::memmove(mixBuffer.data(), mixBuffer.data() + totalSamples,
                 remainingSamples * sizeof(float));
  }
  mixBufferSamples -= samplesToReturn;

  return samplesToReturn;
}

int AudioMixer::GetAvailableSamples() const { return mixBufferSamples; }
