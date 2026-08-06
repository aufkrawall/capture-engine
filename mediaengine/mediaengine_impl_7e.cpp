#include "mediaengine_internal.h"

bool MediaEngine::PullTrackEncodeSourcesB(AudioPullState& s, int track, size_t srcIdx) {
    auto& CHANNELS = s.CHANNELS;
    auto& totalFloats = s.totalFloats;
    auto& activeSources = s.activeSources;
    auto& srcData = s.srcData;
    auto& available = s.available;
    auto& toCopy = s.toCopy;
    auto& syntheticCopiedSamples = s.syntheticCopiedSamples;
    auto& realCopiedSamples = s.realCopiedSamples;
    constexpr int SAMPLE_RATE = AudioPullState::SAMPLE_RATE;
        auto& src = audioSources[srcIdx];
        auto& trackCursorSamples = trackTimelineSamples[track];
                srcData =std::vector<float> (totalFloats, 0.0f);
                available = src.postResampleBuffer.size();
                toCopy = std::min(available, totalFloats);
                syntheticCopiedSamples =
                    std::min<uint64_t>(src.startupSyntheticPostSamples, toCopy / CHANNELS);
                realCopiedSamples = (toCopy / CHANNELS) - syntheticCopiedSamples;

                if (toCopy > 0) {
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    std::copy(src.postResampleBuffer.begin(), src.postResampleBuffer.begin() + toCopy, srcData.begin());
                    src.postResampleBuffer.erase(src.postResampleBuffer.begin(),
                                                 // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
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
    return true;
}
