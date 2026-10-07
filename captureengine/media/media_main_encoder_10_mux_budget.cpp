#include "media_main_internal.h"
#include "media_main_encoder_session.h"

// Mux writer byte budget (common/capture/capture_policy/cfr_mux_byte_budget.h): when the
// output target drains slower than the encoder fills the mux queue, cap the share of CFR
// slots that carry fresh pixels so the queue never reaches the limit at which WriteFrame
// blocks the encoder thread and every blocked slot becomes timeline debt.

namespace {
constexpr uint64_t kCfrMuxByteBudgetProgressLogMs = 5000;
constexpr double kMiB = 1024.0 * 1024.0;
}  // namespace

double MediaEncoderSession::cfrMuxFreshFractionCap() const {
    return cfrMuxByteBudget.active ? cfrMuxByteBudget.freshFractionCap : 1.0;
}

void MediaEncoderSession::updateCfrMuxByteBudget() {
    const bool liveCfr = recordingOutputLive && !config.video.useVFR &&
                         media_main_g_Recording.load(std::memory_order_acquire);
    const uint64_t nowUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (liveCfr && !ce::capture_policy::ShouldSampleCfrMuxFlow(cfrMuxByteBudget, nowUs)) {
        return;
    }
    ce::capture_policy::CfrMuxFlowSample sample{};
    sample.nowUs = nowUs;
    if (liveCfr && MediaEngine_GetMuxFlowSnapshotV1) {
        ce::media::MuxFlowSnapshotV1 snapshot;
        if (MediaEngine_GetMuxFlowSnapshotV1(&snapshot) && snapshot.valid) {
            sample.enqueuedBytes = snapshot.enqueuedBytes;
            sample.writtenBytes = snapshot.writtenBytes;
            sample.writerBusyUs = snapshot.writerBusyUs;
            sample.queuedBytes = snapshot.queuedBytes;
            sample.queueLimitBytes = snapshot.queueLimitBytes;
        }
    }
    const auto outputs = ce::media::submission::GetAcceptedOutputCounts();
    sample.freshOutputs = outputs.fresh;
    sample.totalOutputs = outputs.total;

    const auto decision = ce::capture_policy::UpdateCfrMuxByteBudget(cfrMuxByteBudget, sample, liveCfr);
    if (!decision.sampled && !decision.exited) {
        return;
    }
    const auto& window = cfrMuxByteBudget.last;
    const uint64_t nowMs = GetTickCount64();
    if (decision.entered) {
        cfrMuxByteBudgetActiveSinceMs = nowMs;
        cfrMuxByteBudgetLastProgressLogMs = nowMs;
        LogWarn(
            "[CFR MUX BUDGET] entered: episode=%llu fill=%u.%u%% writer=%.2fMB/s capacity=%.2fMB/s "
            "enqueue=%.2fMB/s busy=%u.%u%% freshShare=%.3f -> freshCap=%.3f shortfall=%u "
            "(output target slower than the encoder; repeats replace fresh slots evenly instead of the "
            "encoder blocking on a full mux queue. CFR PTS, audio timeline and encoder settings unchanged)",
            static_cast<unsigned long long>(cfrMuxByteBudget.episodes), window.fillPermille / 10u,
            window.fillPermille % 10u, window.writerBytesPerSec / kMiB, window.capacityBytesPerSec / kMiB,
            window.enqueueBytesPerSec / kMiB, window.busyPermille / 10u, window.busyPermille % 10u,
            window.observedFreshShare, decision.freshFractionCap, outputShortfallTicks);
    } else if (decision.exited) {
        LogInfo(
            "[CFR MUX BUDGET] exited: reason=%s duration=%llums minFreshCap=%.3f fill=%u.%u%% "
            "capacity=%.2fMB/s enqueue=%.2fMB/s shortfall=%u",
            decision.reason,
            static_cast<unsigned long long>(cfrMuxByteBudgetActiveSinceMs > 0 && nowMs >= cfrMuxByteBudgetActiveSinceMs
                                                ? nowMs - cfrMuxByteBudgetActiveSinceMs
                                                : 0),
            cfrMuxByteBudget.minimumFreshFractionCap, window.fillPermille / 10u, window.fillPermille % 10u,
            window.capacityBytesPerSec / kMiB, window.enqueueBytesPerSec / kMiB, outputShortfallTicks);
        cfrMuxByteBudgetActiveSinceMs = 0;
    } else if (decision.active && nowMs - cfrMuxByteBudgetLastProgressLogMs >= kCfrMuxByteBudgetProgressLogMs) {
        cfrMuxByteBudgetLastProgressLogMs = nowMs;
        LogInfo(
            "[CFR MUX BUDGET] pacing: reason=%s fill=%u.%u%% writer=%.2fMB/s capacity=%.2fMB/s enqueue=%.2fMB/s "
            "busy=%u.%u%% freshShare=%.3f freshCap=%.3f minFreshCap=%.3f shortfall=%u",
            decision.reason, window.fillPermille / 10u, window.fillPermille % 10u, window.writerBytesPerSec / kMiB,
            window.capacityBytesPerSec / kMiB, window.enqueueBytesPerSec / kMiB, window.busyPermille / 10u,
            window.busyPermille % 10u, window.observedFreshShare, decision.freshFractionCap,
            cfrMuxByteBudget.minimumFreshFractionCap, outputShortfallTicks);
    }
}

void MediaEncoderSession::logCfrMuxByteBudgetSummary() {
    LogInfo(
        "[CFR MUX BUDGET SUMMARY] episodes=%llu activeWindows=%llu (~%llums) minFreshCap=%.3f activeAtStop=%d "
        "PtsGrid=immutable AudioTimeline=unchanged EncoderSettings=unchanged",
        static_cast<unsigned long long>(cfrMuxByteBudget.episodes),
        static_cast<unsigned long long>(cfrMuxByteBudget.activeWindows),
        static_cast<unsigned long long>(cfrMuxByteBudget.activeWindows *
                                        (ce::capture_policy::kCfrMuxByteBudgetWindowUs / 1000u)),
        cfrMuxByteBudget.minimumFreshFractionCap, cfrMuxByteBudget.active ? 1 : 0);
}
