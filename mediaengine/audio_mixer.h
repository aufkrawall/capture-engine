#pragma once

#include "audio_resampler.h"
#include <cstdint>
#include <memory>
#include <vector>

// AudioMixer: Mixes multiple audio sources into a single output stream
// All sources are resampled to a common format, then summed with normalization
class AudioMixer {
public:
  AudioMixer();
  ~AudioMixer();

  // Output format for mixed audio
  struct OutputFormat {
    int channels = 2;
    int sampleRate = 48000;
    int bitsPerSample = 32; // Internal processing in float32
    bool isFloat = true;
  };

  // Initialize mixer with output format
  bool Init(const OutputFormat &outFmt);

  // Add samples from a source - they will be resampled to output format
  // sourceId: unique identifier for this source (for tracking)
  // Returns true if samples were accepted
  bool AddSamples(int sourceId, const uint8_t *data, int sizeBytes,
                  int channels, int sampleRate, int bitsPerSample,
                  int validBitsPerSample, int blockAlign, bool isFloat);

  // Get mixed output samples (consumes internal buffer)
  // Returns number of samples (per channel) written to output
  // output must be pre-allocated with at least maxSamples * channels *
  // sizeof(float)
  int GetMixedSamples(float *output, int maxSamples);

  // Get number of mixed samples available
  int GetAvailableSamples() const;

  // Reset mixer state
  void Reset();

  // Get output format
  const OutputFormat &GetOutputFormat() const { return outputFormat; }

private:
  OutputFormat outputFormat;
  bool initialized = false;

  // Per-source resampler (lazily created on first sample from source)
  struct SourceState {
    std::unique_ptr<AudioResampler> resampler;
    AudioResampler::InputFormat lastInputFormat;
    bool active = false;
  };
  std::vector<SourceState> sources;
  static const int MAX_SOURCES = 8;

  // Internal mixing buffer (float32 interleaved)
  std::vector<float> mixBuffer;
  int mixBufferSamples = 0; // Number of samples per channel in buffer

  // Ensure source state exists
  SourceState &GetSourceState(int sourceId);
};
