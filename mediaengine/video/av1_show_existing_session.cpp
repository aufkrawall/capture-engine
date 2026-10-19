#include "av1_show_existing_session.h"

#include <windows.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <iterator>
#include <string>

#include "mediaengine/engine/mediaengine.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>
}

namespace ce::media {
namespace {

constexpr uint64_t kWindowLogIntervalMs = 10000;
// Committed show-existing slots may wait this long behind unreturned NVENC
// packets in a file recording. FFmpeg returns a packet only once `delay`
// newer frames were sent, so with sparse fresh frames (static scenes, slow
// sources) a pipeline-sized bound re-encoded most repeats (measured: one in
// ten unique, ~60% still encoded). The audio pull subtracts pending slots via
// GetEncoderInFlightFrames. A live stream keeps the pipeline-sized bound: its
// packets never wait longer than one more encoder pipeline.
constexpr int kFilePendingLatencyBudgetMs = 1000;

// User custom options that cannot change references, keyframes or frame
// types. Anything else (nonref_p, intra refresh, lookahead, GOP, B-frames, ...)
// keeps the session on re-encoded repeats.
bool IsReferenceNeutralCustomOption(const std::string& key) {
    static const char* const kAllowed[] = {
        "preset",        "tune",       "level",       "tier",
        "profile",       "rc",         "b",           "maxrate",
        "bufsize",       "cq",         "qp",          "multipass",
        "spatial-aq",    "spatial_aq", "aq-strength", "split_encode_mode",
        "surfaces",      "delay",      "gpu",         "highbitdepth",
        "weighted_pred", "s12m_tc",    "qmin",        "qmax",
        "init_qpP",      "init_qpB",   "init_qpI",
    };
    return std::any_of(std::begin(kAllowed), std::end(kAllowed), [&](const char* allowed) { return key == allowed; });
}

int64_t ReadNvencInt(AVCodecContext* context, const char* name, int64_t fallback) {
    int64_t value = fallback;
    if (!context || !context->priv_data || av_opt_get_int(context->priv_data, name, 0, &value) < 0)
        return fallback;
    return value;
}

std::string ReencodeBreakdown(const Av1ShowExistingStats& now, const Av1ShowExistingStats& start) {
    std::string out;
    for (size_t reason = 0; reason < static_cast<size_t>(Av1RepeatReencodeReason::Count); ++reason) {
        const uint64_t count = now.reencoded[reason] - start.reencoded[reason];
        if (count == 0)
            continue;
        if (!out.empty())
            out += ',';
        out += Av1RepeatReencodeReasonName(static_cast<Av1RepeatReencodeReason>(reason));
        out += '=';
        out += std::to_string(count);
    }
    return out.empty() ? "none" : out;
}

}  // namespace

const char* Av1ShowExistingIneligibleReason(const Av1ShowExistingInputs& inputs, const AVCodec* codec) {
    if (inputs.encoder != "av1_nvenc")
        return "codec";
    if (inputs.vfr)
        return "vfr";
    if (inputs.fps <= 0 || inputs.keyframeIntervalSeconds <= 0)
        return "keyframe_interval_default";
    if (inputs.maxBFrames > 0)
        return "b_frames";
    if (inputs.lookaheadActive)
        return "lookahead_scene_cut_keyframes";
    for (const auto& key : inputs.customOptionKeys) {
        if (!IsReferenceNeutralCustomOption(key))
            return "custom_option";
    }
    if (!codec || !codec->priv_class ||
        !av_opt_find(static_cast<void*>(const_cast<AVClass**>(&codec->priv_class)), "idr_free_bitrate_change", nullptr,
                     0, AV_OPT_SEARCH_FAKE_OBJ))
        return "ffmpeg_without_idr_free_bitrate_change";
    return nullptr;
}

Av1ShowExistingSession::~Av1ShowExistingSession() {
    av_packet_free(&unitPacket_);
}

void Av1ShowExistingSession::Configure(const Av1ShowExistingInputs& inputs, const AVCodec* codec,
                                       AVDictionary** options) {
    Stop();
    fps_ = inputs.fps;
    live_ = inputs.liveOutput;
    ineligibleReason_ = Av1ShowExistingIneligibleReason(inputs, codec);
    eligible_ = ineligibleReason_ == nullptr;
    if (!eligible_)
        return;
    av_dict_set(options, "forced-idr", "1", 0);
    av_dict_set(options, "idr_free_bitrate_change", "1", 0);
}

void Av1ShowExistingSession::Start(AVCodecContext* context, uint32_t pipelineDepth) {
    pipelineDepth_ = std::max<uint32_t>(1, pipelineDepth);
    Av1ShowExistingConfig config;
    config.armAfterInterFrames = pipelineDepth_;
    config.maxPendingShowExisting =
        live_ ? pipelineDepth_
              : std::max<uint32_t>(pipelineDepth_,
                                   static_cast<uint32_t>(std::max(0, fps_) * kFilePendingLatencyBudgetMs / 1000));
    config.enabled = eligible_;
    config.disabledReason = ineligibleReason_ ? ineligibleReason_ : "";
    gop_ = context->gop_size;
    if (config.enabled && (gop_ < 2 || gop_ > INT_MAX / 4)) {
        config.enabled = false;
        config.disabledReason = "gop_unknown";
    }
    capSlots_ = 2 * gop_;
    encodedAfterKey_ = 0;
    slotsAfterKey_ = 0;
    anySubmit_ = false;
    const bool needsCompensation = context->bit_rate > 0 || context->rc_max_rate > 0;
    const int64_t dynamicBitrate = ReadNvencInt(context, "dyn_bitrate_supported", 0);
    if (config.enabled && needsCompensation && dynamicBitrate != 1) {
        config.enabled = false;
        config.disabledReason = "no_dynamic_bitrate";
    }
    ledger_.Reset(config);
    if (config.enabled && context->extradata && context->extradata_size > 0)
        ledger_.ObserveExtradata(context->extradata, static_cast<size_t>(context->extradata_size));
    rates_.Reset(ledger_.Enabled() ? fps_ : 0, context->bit_rate, context->rc_max_rate, live_);
    reconfigureFailuresSeen_ = ReadNvencInt(context, "bitrate_reconfig_failures", 0);
    awaitingReconfigure_ = false;
    if (!unitPacket_)
        unitPacket_ = av_packet_alloc();
    started_ = true;
    armedLogged_ = false;
    windowStart_ = ledger_.Stats();
    lastWindowLogMs_ = GetTickCount64();
    forcedKeyframes_ = 0;
    if (ledger_.Enabled()) {
        DLL_Log(
            "[AV1 ShowExisting] enabled (%s): repeat slots re-show the previous frame without an encode; "
            "NVENC GOP %lld encoded frames, CE forces a key after %lld slots; arms after %u verified frames; "
            "ledger bound %u; bitrate follows encoded frames%s (base %lld/%lld bps, %s)",
            live_ ? "live" : "file", static_cast<long long>(gop_), static_cast<long long>(capSlots_), pipelineDepth_,
            config.maxPendingShowExisting, live_ ? ", at most x1.10 of the target in any second" : "",
            static_cast<long long>(context->bit_rate), static_cast<long long>(context->rc_max_rate),
            rates_.Active() ? "IDR-free reconfigure" : "constant QP, nothing to scale");
    } else if (std::strcmp(ledger_.DisabledReason(), "codec") != 0) {
        DLL_Log("[AV1 ShowExisting] disabled: reason=%s; repeats re-encode the cached frame", ledger_.DisabledReason());
    }
    Publish();
}

void Av1ShowExistingSession::Stop() {
    if (started_)
        LogSummary();
    started_ = false;
    pendingPublished_.store(0, std::memory_order_release);
}

void Av1ShowExistingSession::Publish() {
    pendingPublished_.store(ledger_.PendingShowExisting(), std::memory_order_release);
}

void Av1ShowExistingSession::ApplyRates(AVCodecContext* context) {
    if (!rates_.TakeChange())
        return;
    const auto rates = rates_.Current();
    if (rates.bitRate > 0)
        context->bit_rate = rates.bitRate;
    if (rates.maxRate > 0)
        context->rc_max_rate = rates.maxRate;
    awaitingReconfigure_ = true;
}

void Av1ShowExistingSession::CheckReconfigureOutcome(AVCodecContext* context) {
    if (!awaitingReconfigure_)
        return;
    const int64_t failures = ReadNvencInt(context, "bitrate_reconfig_failures", reconfigureFailuresSeen_);
    if (failures <= reconfigureFailuresSeen_)
        return;
    reconfigureFailuresSeen_ = failures;
    const double refused = rates_.Current().factor;
    rates_.OnRejected();
    DLL_Log("[AV1 ShowExisting] NVENC rejected bitrate x%.2f; capped at x%.2f for this session", refused,
            rates_.Current().factor);
    ApplyRates(context);
}

void Av1ShowExistingSession::PrepareSubmit(AVCodecContext* context, AVFrame* frame) {
    if (!started_ || !frame)
        return;
    CheckReconfigureOutcome(context);
    // NVENC keys the first frame and every gop-th encoded frame after a key,
    // forced keys included (measured: forced key at 100, own key at 340).
    bool key = !anySubmit_ || encodedAfterKey_ + 1 >= gop_;
    if (eligible_) {
        const bool forced = anySubmit_ && slotsAfterKey_ + 1 >= capSlots_;
        frame->pict_type = forced ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        forcedKeyframes_ += forced ? 1 : 0;
        key = key || forced;
    }
    anySubmit_ = true;
    encodedAfterKey_ = key ? 0 : encodedAfterKey_ + 1;
    slotsAfterKey_ = key ? 0 : slotsAfterKey_ + 1;
    ledger_.OnFrameSubmitted(frame->pts, key);
    cacheHoldsLastSubmit_ = false;
    rates_.ObserveSlot(true);
    ApplyRates(context);
    Publish();
    LogWindow(GetTickCount64(), false);
}

void Av1ShowExistingSession::OnSubmitDropped(int64_t pts) {
    if (!started_)
        return;
    ledger_.OnFrameDropped(pts);
    // NVENC never counted it, so the keyframe prediction is off from here on.
    if (ledger_.Enabled()) {
        if (ledger_.Armed()) {
            DLL_Log(
                "[AV1 ShowExisting] disarmed: the submit at pts=%lld never reached the encoder; repeats "
                "re-encode from now on",
                static_cast<long long>(pts));
        }
        ledger_.Disarm("submit_dropped");
    }
    Publish();
}

bool Av1ShowExistingSession::TryRepeat(AVCodecContext* context, int64_t pts, bool pixelsDiffer,
                                       const PacketSink& sink) {
    if (!started_)
        return false;
    Av1PacketPlan plan;
    Av1RepeatReencodeReason reason{};
    const bool keyframeDue = slotsAfterKey_ + 1 >= capSlots_;
    if (!ledger_.TryCommitShowExisting(pts, pixelsDiffer || !cacheHoldsLastSubmit_, keyframeDue, &plan, &reason)) {
        if (plan.violation)
            DLL_Log("[AV1 ShowExisting] disarmed: reason=%s; repeats re-encode from now on", plan.violation);
        return false;
    }
    ++slotsAfterKey_;
    rates_.ObserveSlot(false);
    ApplyRates(context);
    EmitUnits(plan, sink);
    Publish();
    LogWindow(GetTickCount64(), false);
    return true;
}

void Av1ShowExistingSession::EmitUnits(const Av1PacketPlan& plan, const PacketSink& sink) {
    for (const auto& unit : plan.after) {
        if (!unitPacket_ || av_new_packet(unitPacket_, unit.size) < 0) {
            DLL_Log("[AV1 ShowExisting] could not allocate a %u-byte unit for pts=%lld", unit.size,
                    static_cast<long long>(unit.pts));
            continue;
        }
        std::memcpy(unitPacket_->data, unit.bytes.data(), unit.size);
        unitPacket_->pts = unit.pts;
        unitPacket_->dts = unit.pts;
        unitPacket_->duration = 1;
        unitPacket_->flags = 0;
        sink(unitPacket_);
        av_packet_unref(unitPacket_);
    }
}

void Av1ShowExistingSession::WritePacket(AVPacket* packet, const PacketSink& sink) {
    if (!started_) {
        sink(packet);
        return;
    }
    const Av1PacketPlan plan = ledger_.OnEncoderPacket(packet->pts, packet->data, static_cast<size_t>(packet->size));
    if (plan.violation) {
        DLL_Log("[AV1 ShowExisting] disarmed at pts=%lld: reason=%s holdSlots=%lld; repeats re-encode from now on",
                static_cast<long long>(packet->pts), plan.violation, static_cast<long long>(plan.holdSlots));
    }
    packet->duration = Av1HeldPacketDuration(packet->duration, plan.holdSlots);
    heldSlotsInSink_ = std::max<int64_t>(0, plan.holdSlots);
    sink(packet);
    heldSlotsInSink_ = 0;
    EmitUnits(plan, sink);
    if (!armedLogged_ && ledger_.Armed()) {
        armedLogged_ = true;
        const auto& sequence = ledger_.Sequence();
        DLL_Log(
            "[AV1 ShowExisting] armed after %llu verified packets (orderHintBits=%u frameIds=%d "
            "decoderModel=%d)",
            static_cast<unsigned long long>(ledger_.Stats().packetsVerified), sequence.orderHintBits,
            sequence.frameIdNumbersPresent, sequence.decoderModelInfoPresent);
    }
    Publish();
}

void Av1ShowExistingSession::LogWindow(uint64_t nowMs, bool force) {
    if (!ledger_.Enabled() && !force)
        return;
    if (!force && nowMs - lastWindowLogMs_ < kWindowLogIntervalMs)
        return;
    const auto& stats = ledger_.Stats();
    DLL_Log(
        "[AV1 ShowExisting] %s: shown=%llu reencoded=%s pendingPeak=%u holds=%llu armed=%d "
        "bitrate=x%.2f (max x%.2f, changes=%llu, peakCapped=%llu, rejected=%llu) forcedKeys=%llu",
        force ? "summary" : "window", static_cast<unsigned long long>(stats.written - windowStart_.written),
        ReencodeBreakdown(stats, windowStart_).c_str(), stats.pendingPeak,
        static_cast<unsigned long long>(stats.holdSlots), ledger_.Armed(), rates_.Current().factor,
        rates_.MaxAppliedFactor(), static_cast<unsigned long long>(rates_.Changes()),
        static_cast<unsigned long long>(rates_.PeakCapped()), static_cast<unsigned long long>(rates_.Rejections()),
        static_cast<unsigned long long>(forcedKeyframes_));
    lastWindowLogMs_ = nowMs;
    if (!force)
        windowStart_ = stats;
}

void Av1ShowExistingSession::LogSummary() {
    if (!ledger_.Enabled() && ledger_.Stats().written == 0)
        return;
    windowStart_ = Av1ShowExistingStats{};
    LogWindow(GetTickCount64(), true);
}

}  // namespace ce::media
