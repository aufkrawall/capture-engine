#pragma once

// Submit ledger and policy for AV1 show-existing CFR repeats (av1_nvenc).
//
// A repeat slot that would re-encode the cached frame is instead committed as a
// header-only temporal unit that re-shows the frame the decoder already holds
// (av1_obu.h). The decision is made at submit time, but the reference slot is
// only known once NVENC returns the previous frame's packet, `pipeline` frames
// later. The ledger therefore records every encoder submit and every committed
// show-existing slot in send order (= NVENC output order with b_frames=0) and
// emits the synthetic units right after their frame's packet.
//
// Smoothness contract: this changes what a repeat costs, never which slots
// repeat or what they show. A show-existing unit displays exactly the frame
// sent to the encoder last, which is the cached frame a re-encode would copy;
// every condition where that is not guaranteed re-encodes instead.
// Design: llm-wiki/nvenc-encoding.md "AV1 show-existing repeats".

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "av1_obu.h"

namespace ce::media {

enum class Av1RepeatReencodeReason : uint8_t {
    Disabled,       // session not eligible (codec/config)
    NotArmed,       // header contract not verified yet
    Disarmed,       // contract violated earlier this session
    Overlay,        // dynamic overlays need recomposed pixels
    KeyframeDue,    // the keyframe cap lands on this repeat
    AfterKeyframe,  // a shown KEY_FRAME is not showable
    NoPriorFrame,   // nothing known to show (start, dropped submit)
    LedgerBound,    // NVENC holds too many frames; re-encode pushes one out
    Count,
};

const char* Av1RepeatReencodeReasonName(Av1RepeatReencodeReason reason);

struct Av1ShowExistingConfig {
    bool enabled = false;
    const char* disabledReason = "not_configured";
    // Shown inter frames with refresh_frame_flags != 0 that must be verified
    // after the first keyframe before any repeat is committed.
    uint32_t armAfterInterFrames = 0;
    // Committed show-existing slots waiting behind unreturned encoder packets.
    uint32_t maxPendingShowExisting = 0;
};

struct Av1SyntheticPacket {
    int64_t pts = 0;
    uint8_t size = 0;
    std::array<uint8_t, av1::kMaxShowExistingUnitBytes> bytes = {};
};

// What to write for one encoder packet (or one commit / the flush tail).
struct Av1PacketPlan {
    // Emergency only: slots appended to the encoder packet's duration because
    // the show-existing units committed behind it cannot be written.
    int64_t holdSlots = 0;
    std::vector<Av1SyntheticPacket> after;
    const char* violation = nullptr;  // non-null once, when the session disarmed
};

// The encoder packet's duration in 1/fps slots (repeats need CFR) once it holds
// `holdSlots`: its own slot plus the held ones. The finalize flush passes
// NVENC's duration as is, which may be 0. The writers' duration ceiling admits
// the hold (ce::mux::ClampCfrVideoPacketDuration).
inline int64_t Av1HeldPacketDuration(int64_t encoderDuration, int64_t holdSlots) {
    return holdSlots > 0 ? (encoderDuration > 1 ? encoderDuration : 1) + holdSlots : encoderDuration;
}

struct Av1ShowExistingStats {
    uint64_t written = 0;
    uint64_t reencoded[static_cast<size_t>(Av1RepeatReencodeReason::Count)] = {};
    uint64_t holdSlots = 0;
    uint64_t violations = 0;
    uint64_t packetsVerified = 0;
    uint32_t pendingPeak = 0;
};

class Av1ShowExistingRepeats {
public:
    void Reset(const Av1ShowExistingConfig& config);
    bool Enabled() const {
        return config_.enabled;
    }
    bool Armed() const {
        return armed_;
    }
    const char* DisabledReason() const {
        return disabledReason_;
    }

    // Codec extradata (sequence header OBU) at open; a failure disables.
    bool ObserveExtradata(const uint8_t* data, size_t size);

    // Submit-time decision for a repeat slot. True commits the slot: nothing
    // may be sent to the encoder for `pts`, and `plan` receives units that can
    // be written immediately (no encoder packet outstanding).
    bool TryCommitShowExisting(int64_t pts, bool pixelsDiffer, bool keyframeDue, Av1PacketPlan* plan,
                               Av1RepeatReencodeReason* reason);

    // Every frame handed to the encoder, in send order; `key` when it will be
    // a keyframe (forced, or the encoder's own GOP end).
    void OnFrameSubmitted(int64_t pts, bool key);
    // Stops committing for the rest of the session (e.g. keyframe prediction
    // lost); committed slots are still written.
    void Disarm(const char* reason) {
        Disarm(reason, nullptr);
    }
    // A handed-off frame that never reached the encoder (gate drop, failure).
    void OnFrameDropped(int64_t pts);

    // Every encoder packet in output order, before it is written.
    Av1PacketPlan OnEncoderPacket(int64_t pts, const uint8_t* data, size_t size);

    uint32_t PendingShowExisting() const {
        return pendingShowExisting_;
    }
    const Av1ShowExistingStats& Stats() const {
        return stats_;
    }
    const av1::SequenceHeader& Sequence() const {
        return sequence_;
    }

private:
    struct Entry {
        int64_t pts = 0;
        bool showExisting = false;
        bool key = false;
    };

    void Disarm(const char* reason, Av1PacketPlan* plan);
    void Disable(const char* reason);
    bool WriteUnit(int64_t pts, Av1PacketPlan* plan);
    void HoldSlot(const char* reason, Av1PacketPlan* plan);
    // Writes or holds the show-existing entries now at the ledger head.
    void ReleaseFollowingEntries(Av1PacketPlan* plan);

    Av1ShowExistingConfig config_;
    const char* disabledReason_ = "not_configured";
    av1::SequenceHeader sequence_;
    std::deque<Entry> ledger_;
    uint32_t pendingShowExisting_ = 0;
    bool armed_ = false;
    bool disarmed_ = false;
    bool firstSubmitSeen_ = false;
    bool keySeen_ = false;
    uint32_t verifiedInterFrames_ = 0;
    // Last frame sent to the encoder (known at submit time).
    bool lastSubmittedKnown_ = false;
    bool lastSubmittedKey_ = false;
    // Last encoder packet written: which slot shows it, and whether it may be.
    bool lastWrittenShowable_ = false;
    uint8_t lastWrittenSlot_ = 0;
    std::array<uint32_t, av1::kNumRefFrames> refFrameId_ = {};
    Av1ShowExistingStats stats_;
};

}  // namespace ce::media
