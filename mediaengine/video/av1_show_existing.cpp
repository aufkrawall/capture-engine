#include "av1_show_existing.h"

#include <algorithm>

namespace ce::media {

const char* Av1RepeatReencodeReasonName(Av1RepeatReencodeReason reason) {
    switch (reason) {
        case Av1RepeatReencodeReason::Disabled:
            return "disabled";
        case Av1RepeatReencodeReason::NotArmed:
            return "not_armed";
        case Av1RepeatReencodeReason::Disarmed:
            return "disarmed";
        case Av1RepeatReencodeReason::Overlay:
            return "overlay";
        case Av1RepeatReencodeReason::KeyframeDue:
            return "keyframe_due";
        case Av1RepeatReencodeReason::AfterKeyframe:
            return "after_keyframe";
        case Av1RepeatReencodeReason::NoPriorFrame:
            return "no_prior_frame";
        case Av1RepeatReencodeReason::LedgerBound:
            return "ledger_bound";
        case Av1RepeatReencodeReason::Count:
            break;
    }
    return "unknown";
}

void Av1ShowExistingRepeats::Reset(const Av1ShowExistingConfig& config) {
    *this = Av1ShowExistingRepeats{};
    config_ = config;
    disabledReason_ = config_.enabled ? "" : config_.disabledReason;
}

void Av1ShowExistingRepeats::Disable(const char* reason) {
    config_.enabled = false;
    armed_ = false;
    disabledReason_ = reason;
}

bool Av1ShowExistingRepeats::ObserveExtradata(const uint8_t* data, size_t size) {
    if (!config_.enabled || !data || size == 0)
        return config_.enabled;
    // av1C (ISO-BMFF/Matroska CodecPrivate) carries the config OBUs after a
    // 4-byte header whose first byte is marker=1, version=1.
    if (size >= 4 && data[0] == 0x81) {
        data += 4;
        size -= 4;
    }
    av1::TemporalUnitInfo info;
    const av1::ParseResult parsed = av1::ParseTemporalUnit(data, size, &sequence_, &info);
    if (parsed.status != av1::ParseStatus::Ok) {
        Disable(parsed.reason);
        return false;
    }
    const av1::ParseResult writable = av1::CanWriteShowExisting(sequence_);
    if (info.sequenceHeaderSeen && writable.status != av1::ParseStatus::Ok) {
        Disable(writable.reason);
        return false;
    }
    return true;
}

void Av1ShowExistingRepeats::Disarm(const char* reason, Av1PacketPlan* plan) {
    armed_ = false;
    if (disarmed_)
        return;
    disarmed_ = true;
    disabledReason_ = reason;
    ++stats_.violations;
    if (plan)
        plan->violation = reason;
}

bool Av1ShowExistingRepeats::WriteUnit(int64_t pts, Av1PacketPlan* plan) {
    Av1SyntheticPacket packet;
    packet.pts = pts;
    const size_t size = av1::WriteShowExistingTemporalUnit(sequence_, lastWrittenSlot_, refFrameId_[lastWrittenSlot_],
                                                           packet.bytes.data(), packet.bytes.size());
    if (size == 0)
        return false;
    packet.size = static_cast<uint8_t>(size);
    plan->after.push_back(packet);
    ++stats_.written;
    return true;
}

void Av1ShowExistingRepeats::HoldSlot(const char* reason, Av1PacketPlan* plan) {
    ++plan->holdSlots;
    ++stats_.holdSlots;
    Disarm(reason, plan);
}

bool Av1ShowExistingRepeats::TryCommitShowExisting(int64_t pts, bool pixelsDiffer, bool keyframeDue,
                                                   Av1PacketPlan* plan, Av1RepeatReencodeReason* reason) {
    const auto reencode = [&](Av1RepeatReencodeReason why) {
        ++stats_.reencoded[static_cast<size_t>(why)];
        if (reason)
            *reason = why;
        return false;
    };
    if (!config_.enabled)
        return reencode(Av1RepeatReencodeReason::Disabled);
    if (disarmed_)
        return reencode(Av1RepeatReencodeReason::Disarmed);
    if (!armed_)
        return reencode(Av1RepeatReencodeReason::NotArmed);
    if (pixelsDiffer)
        return reencode(Av1RepeatReencodeReason::Overlay);
    if (keyframeDue)
        return reencode(Av1RepeatReencodeReason::KeyframeDue);
    if (!lastSubmittedKnown_)
        return reencode(Av1RepeatReencodeReason::NoPriorFrame);
    if (lastSubmittedKey_)
        return reencode(Av1RepeatReencodeReason::AfterKeyframe);
    if (ledger_.empty()) {
        // Every encoder packet is written: show the last one right away.
        if (!lastWrittenShowable_)
            return reencode(Av1RepeatReencodeReason::AfterKeyframe);
        if (WriteUnit(pts, plan))
            return true;
        Disarm("show_existing_unwritable", plan);
        return reencode(Av1RepeatReencodeReason::Disarmed);
    }
    if (pendingShowExisting_ >= config_.maxPendingShowExisting)
        return reencode(Av1RepeatReencodeReason::LedgerBound);
    ledger_.push_back(Entry{pts, true, false});
    ++pendingShowExisting_;
    stats_.pendingPeak = std::max(stats_.pendingPeak, pendingShowExisting_);
    return true;
}

void Av1ShowExistingRepeats::OnFrameSubmitted(int64_t pts, bool key) {
    if (!config_.enabled && ledger_.empty())
        return;
    // NVENC opens every stream with a keyframe whatever the caller predicted.
    key = key || !firstSubmitSeen_;
    firstSubmitSeen_ = true;
    ledger_.push_back(Entry{pts, false, key});
    lastSubmittedKnown_ = true;
    lastSubmittedKey_ = key;
}

void Av1ShowExistingRepeats::OnFrameDropped(int64_t pts) {
    const auto it = std::find_if(ledger_.rbegin(), ledger_.rend(),
                                 [&](const Entry& entry) { return !entry.showExisting && entry.pts == pts; });
    if (it == ledger_.rend())
        return;
    ledger_.erase(std::next(it).base());
    // The decoder keeps showing whatever preceded the dropped frame; the next
    // repeat re-encodes rather than guess whether that frame is showable.
    lastSubmittedKnown_ = false;
}

void Av1ShowExistingRepeats::ReleaseFollowingEntries(Av1PacketPlan* plan) {
    while (!ledger_.empty() && ledger_.front().showExisting) {
        const Entry entry = ledger_.front();
        ledger_.pop_front();
        --pendingShowExisting_;
        // Committed, but if the frame before it cannot be re-shown, no encode
        // can be inserted any more: that frame stays on screen through the slot.
        if (!lastWrittenShowable_)
            HoldSlot("committed_repeat_unshowable", plan);
        else if (!WriteUnit(entry.pts, plan))
            HoldSlot("show_existing_unwritable", plan);
    }
}

Av1PacketPlan Av1ShowExistingRepeats::OnEncoderPacket(int64_t pts, const uint8_t* data, size_t size) {
    Av1PacketPlan plan;
    if (ledger_.empty() && !config_.enabled)
        return plan;
    const auto match = std::find_if(ledger_.begin(), ledger_.end(),
                                    [&](const Entry& entry) { return !entry.showExisting && entry.pts == pts; });
    if (match == ledger_.end()) {
        Disarm("packet_without_submit", &plan);
        return plan;
    }
    if (match != ledger_.begin()) {
        // Frames ahead of this packet never came back. Committed slots between
        // them are left empty; the container keeps the previous frame on screen.
        for (auto it = ledger_.begin(); it != match; ++it) {
            if (it->showExisting) {
                --pendingShowExisting_;
                ++stats_.holdSlots;
            }
        }
        ledger_.erase(ledger_.begin(), match);
        Disarm("packet_order_mismatch", &plan);
    }
    const Entry entry = ledger_.front();
    ledger_.pop_front();

    av1::TemporalUnitInfo info;
    const av1::ParseResult parsed = av1::ParseTemporalUnit(data, size, &sequence_, &info);
    bool showable = false;
    if (parsed.status != av1::ParseStatus::Ok) {
        Disarm(parsed.reason, &plan);
    } else if (info.extensionSeen) {
        Disarm("obu_extension", &plan);
    } else if (info.frameHeaders != 1) {
        Disarm(info.frameHeaders == 0 ? "no_frame_header" : "multiple_frames_per_unit", &plan);
    } else if (av1::CanWriteShowExisting(sequence_).status != av1::ParseStatus::Ok) {
        Disarm(av1::CanWriteShowExisting(sequence_).reason, &plan);
    } else if (info.frame.showExistingFrame) {
        Disarm("encoder_show_existing", &plan);
    } else {
        const av1::FrameHeader& frame = info.frame;
        const bool shownKey = frame.frameType == av1::FrameType::Key && frame.showFrame;
        ++stats_.packetsVerified;
        for (int slot = 0; slot < av1::kNumRefFrames; ++slot) {
            if (frame.refreshFrameFlags & (1u << slot))
                refFrameId_[slot] = frame.currentFrameId;
        }
        if (frame.refreshFrameFlags != 0) {
            for (uint8_t slot = 0; slot < av1::kNumRefFrames; ++slot) {
                if (frame.refreshFrameFlags & (1u << slot)) {
                    lastWrittenSlot_ = slot;
                    break;
                }
            }
        }
        showable = frame.showFrame && frame.showableFrame && frame.refreshFrameFlags != 0;
        if (shownKey) {
            keySeen_ = true;
            if (!entry.key)
                Disarm("unexpected_keyframe", &plan);
        } else if (!frame.showFrame) {
            Disarm("hidden_frame", &plan);
        } else if (frame.refreshFrameFlags == 0) {
            Disarm("non_reference_frame", &plan);
        } else if (!armed_ && !disarmed_ && keySeen_ && ++verifiedInterFrames_ >= config_.armAfterInterFrames) {
            armed_ = config_.enabled;
        }
    }
    lastWrittenShowable_ = showable;
    ReleaseFollowingEntries(&plan);
    return plan;
}

}  // namespace ce::media
