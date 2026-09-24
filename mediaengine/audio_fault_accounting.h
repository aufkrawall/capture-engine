#pragma once

// Audio encode fault accounting: a pure decision helper plus the cross-thread lock for
// the shared audio timeline cursors.
//
// Under a codec fault the audio CONTENT can be lost or shifted while every length check
// stays green (the recording-end clamp keeps track lengths exactly equal). These keep the
// timeline honest: a consumed-but-lost sample range becomes an explicit recorded hole
// instead of being silently re-filled (which would compress the track and re-place every
// later sample).
//
// A hole is placed according to where the lost range sat in the encoder's sample stream:
// - FIFO FRONT losses (a frame already read, e.g. a terminal avcodec_send_frame failure)
//   are placed by advancing the PTS cursor (samplesCount) past the frame's exact range -
//   the jump lands where the frame had its PTS.
// - FIFO TAIL losses (a consumed intake range that could not enter the FIFO) are placed
//   by appending silence to the FIFO: pending samples must keep their positions, which a
//   PTS jump would destroy. AudioEncoder::AppendSilenceHole owns that placement.

#include <algorithm>
#include <cstdint>
#include <mutex>

#include "audio_sync/timeline_constants.h"

namespace ce::audio {

// The mix chunk is consumed (erased from every source) before it is encoded, so the track
// cursor must advance by the FULL consumed chunk even when the encoder accepted less.
// Shrinking that advance would make the next pull re-request the consumed range and fill
// it with newer samples (content compression). A shortfall is a real unrecoverable hole
// only when the encode FAILED; a shortfall with failed == false is the intentional
// recording-end clamp and must not be counted as loss. acceptedSamples is what really
// entered the encoder pipeline (resampler + FIFO intake): samples merely queued in the
// FIFO still count as accepted because they are placed later, not lost.
inline int64_t ComputeConsumedChunkHoleSamples(int64_t consumedChunkSamples, int64_t acceptedSamples, bool failed) {
    if (!failed || consumedChunkSamples <= 0) {
        return 0;
    }
    const int64_t accepted = std::clamp<int64_t>(acceptedSamples, 0, consumedChunkSamples);
    return consumedChunkSamples - accepted;
}

// Maps a consumed chunk between sample rates through the same duration round trip
// the encode path uses to size hole silence (ComputeSamplesToDurationUs +
// ComputeDurationUsToSamples), so a hole counted here and the silence placed there
// cover exactly the same timeline range. Identity rates map exactly.
inline int64_t ComputeOutputRateChunkSamples(int64_t chunkSamples, int chunkRate, int outputRate) {
    if (chunkRate > 0 && chunkRate == outputRate) {
        return chunkSamples;
    }
    return ComputeDurationUsToSamples(ComputeSamplesToDurationUs(chunkSamples, chunkRate), outputRate);
}

// Leaf lock for the two cross-thread audio timeline cursors (encodedSamplesPerSource /
// trackTimelineSamples): the pull thread advances them while the audio worker reads them
// for write-cursor pinning and gap suppression. Deliberately NOT muxMutex: the audio
// worker must never block on a muxMutex-holder that is itself waiting on the audio worker
// (e.g. SyncAudioToFirstVideoFrame). It is a true leaf - nothing else is acquired or
// awaited while it is held (callers may already hold audioDrainMutex around it, e.g. the
// stop-time catch-up wait, but never the other way around) - so no lock-order cycle can
// form and it cannot deadlock.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
inline std::mutex g_audioCursorSyncMutex;

}  // namespace ce::audio
