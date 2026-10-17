#pragma once

// VideoEncoder's AV1 show-existing repeats: eligibility at codec open,
// CE-owned keyframes, the submit ledger, bitrate compensation and the packets
// written behind each NVENC packet. Everything runs on the encoder thread,
// except the finalize flush on the writer thread after that thread stopped.
// Design, measurements and rules: llm-wiki/nvenc-encoding.md "AV1
// show-existing repeats".

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "av1_show_existing.h"
#include "encoder_rate_compensation.h"

struct AVCodec;
struct AVCodecContext;
struct AVDictionary;
struct AVFrame;
struct AVPacket;

namespace ce::media {

struct Av1ShowExistingInputs {
    std::string encoder;
    bool vfr = false;
    bool liveOutput = false;
    int fps = 0;
    int keyframeIntervalSeconds = 0;
    int maxBFrames = 0;
    bool lookaheadActive = false;
    std::vector<std::string> customOptionKeys;
};

// nullptr when the session may use show-existing repeats, else a stable reason.
const char* Av1ShowExistingIneligibleReason(const Av1ShowExistingInputs& inputs, const AVCodec* codec);

class Av1ShowExistingSession {
public:
    using PacketSink = std::function<void(AVPacket*)>;

    ~Av1ShowExistingSession();

    // Before avcodec_open2: when eligible, asks for IDR-free bitrate changes
    // and IDR keyframes when CE forces one. NVENC keeps its own GOP: it
    // counts encoded frames, so CE predicts its keyframes and forces one only
    // when repeats stretched a GOP to twice the keyframe interval (a GOP cut
    // short by a forced key costs NVENC rate control, measured).
    void Configure(const Av1ShowExistingInputs& inputs, const AVCodec* codec, AVDictionary** options);
    // After avcodec_open2 succeeded.
    void Start(AVCodecContext* context, uint32_t pipelineDepth);
    void Stop();

    // Every frame handed to the encoder, before the hand-off: predicts or
    // forces its keyframe and applies a pending bitrate change.
    void PrepareSubmit(AVCodecContext* context, AVFrame* frame);
    void OnSubmitDropped(int64_t pts);
    // The repeat cache now holds exactly the frame sent to the encoder last.
    void NoteRepeatCacheHoldsLastSubmit() {
        cacheHoldsLastSubmit_ = true;
    }
    // A repeat slot: true when it was committed as a show-existing unit and
    // must not be encoded. `pixelsDiffer` when the repeat would not show the
    // frame sent to the encoder last.
    bool TryRepeat(AVCodecContext* context, int64_t pts, bool pixelsDiffer, const PacketSink& sink);
    // Writes an encoder packet and the show-existing units committed behind it.
    void WritePacket(AVPacket* packet, const PacketSink& sink);

    uint32_t PendingSlots() const {
        return pendingPublished_.load(std::memory_order_acquire);
    }
    void LogWindow(uint64_t nowMs, bool force);
    void LogSummary();

private:
    void ApplyRates(AVCodecContext* context);
    void CheckReconfigureOutcome(AVCodecContext* context);
    void EmitUnits(const Av1PacketPlan& plan, const PacketSink& sink);
    void Publish();

    int fps_ = 0;
    const char* ineligibleReason_ = "not_configured";
    bool eligible_ = false;
    bool live_ = false;
    int64_t gop_ = 0;       // NVENC GOP in encoded frames
    int64_t capSlots_ = 0;  // longest GOP in output slots before CE forces a key
    int64_t encodedAfterKey_ = 0;
    int64_t slotsAfterKey_ = 0;
    bool anySubmit_ = false;
    uint32_t pipelineDepth_ = 0;
    bool started_ = false;
    Av1ShowExistingRepeats ledger_;
    SubmittedRateCompensation rates_;
    bool cacheHoldsLastSubmit_ = false;
    bool awaitingReconfigure_ = false;
    int64_t reconfigureFailuresSeen_ = 0;
    AVPacket* unitPacket_ = nullptr;
    std::atomic<uint32_t> pendingPublished_{0};
    bool armedLogged_ = false;
    uint64_t lastWindowLogMs_ = 0;
    Av1ShowExistingStats windowStart_;
    uint64_t forcedKeyframes_ = 0;
};

}  // namespace ce::media
