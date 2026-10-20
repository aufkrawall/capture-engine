#include "media_main_internal.h"
#include "media_main_encoder_session.h"

#include <cmath>
#include <cstdio>
#include <vector>

// Inject CFR start: frame zero by content target, and the encoder prewarm that keeps the first encode
// from holding it. Evidence and rationale: llm-wiki/cfr-capture-sync.md ("Inject frame zero").

bool MediaEncoderSession::SelectInjectCfrStartFrame() {
    // The go-live iteration's selector still ran the warm-up rule and popped the oldest buffered frame;
    // it competes with the rest here.
    if (popped && frame.isInjectMode) {
        bufferedInjectFrames.push_front(std::move(frame));
        frame = QueuedFrame{};
        popped = false;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (injectStartWaitStartQpc <= 0) {
        injectStartWaitStartQpc = now.QuadPart;
    }
    const size_t requiredReserve =
        ce::capture_policy::GetInjectReserveFrames(false, smoothedInjectFenceMs, frameIntervalMs);
    const size_t protectedTail = ce::capture_policy::GetMinBufferedInjectFrames(requiredReserve, false);
    const size_t eligible =
        bufferedInjectFrames.size() > protectedTail ? bufferedInjectFrames.size() - protectedTail : 0;
    std::vector<int64_t> timestamps;
    timestamps.reserve(eligible);
    for (size_t i = 0; i < eligible; ++i) {
        timestamps.push_back(bufferedInjectFrames[i].timestamp);
    }
    // The first live tick is anchored at the end of frame zero's encode (prewarmed: a few ms), so frame
    // zero's content belongs one content delay before now.
    const int64_t targetQpc = now.QuadPart - std::max<int64_t>(0, avContentDelayQpc);
    const int64_t sourceIntervalQpc =
        injectInputPredictor.IsCalibrated()
            ? std::max<int64_t>(1, static_cast<int64_t>(std::llround(injectInputPredictor.SmoothedIntervalQpc())))
            : targetIntervalTicks;
    const int64_t waitBudgetQpc =
        static_cast<int64_t>(ce::capture_policy::kInjectCfrStartFreshWaitMaxMs) * qpcFreq.QuadPart / 1000;
    const bool waitBudgetSpent = now.QuadPart - injectStartWaitStartQpc >= waitBudgetQpc;
    const auto decision = ce::capture_policy::DecideInjectCfrStartFrame(
        timestamps.data(), timestamps.size(), targetQpc,
        ce::capture_policy::GetInjectCfrSelectionLeadToleranceQpc(targetIntervalTicks),
        std::max<int64_t>(targetIntervalTicks, sourceIntervalQpc), waitBudgetSpent);

    for (size_t i = 0; i < decision.discardBefore && !bufferedInjectFrames.empty(); ++i) {
        QueuedFrame stale = std::move(bufferedInjectFrames.front());
        bufferedInjectFrames.pop_front();
        DiscardQueuedFrame(stale);
        ++injectStartStaleDiscards;
    }
    if (!decision.ready) {
        ++injectStartWaitTicks;
        if (injectStartWaitTicks <= 4 || (injectStartWaitTicks % 30) == 0) {
            LogInfo(
                "[EncoderThread] Inject CFR start waiting for a frame near the content target: "
                "candidateAgeUs=%lld hasCandidate=%d buffered=%zu tail=%zu discardedStale=%llu waitTicks=%u",
                static_cast<long long>(qpcToUs(decision.ageQpc)), decision.hasCandidate ? 1 : 0,
                bufferedInjectFrames.size(), protectedTail,
                static_cast<unsigned long long>(injectStartStaleDiscards), injectStartWaitTicks);
        }
        return false;
    }

    frame = std::move(bufferedInjectFrames.front());
    bufferedInjectFrames.pop_front();
    popped = true;
    injectStartSelectionQpc = now.QuadPart;
    LogInfo(
        "[EncoderThread] Inject CFR start frame: ageUs=%lld fresh=%d budgetSpent=%d discardedStale=%llu "
        "waitedMs=%lld waitTicks=%u buffered=%zu tail=%zu frame=%u ts=%lld targetQpc=%lld",
        static_cast<long long>(qpcToUs(decision.ageQpc)), decision.fresh ? 1 : 0, waitBudgetSpent ? 1 : 0,
        static_cast<unsigned long long>(injectStartStaleDiscards),
        static_cast<long long>(qpcToUs(now.QuadPart - injectStartWaitStartQpc) / 1000), injectStartWaitTicks,
        bufferedInjectFrames.size(), protectedTail, frame.frameIndex, static_cast<long long>(frame.timestamp),
        static_cast<long long>(targetQpc));
    injectStartWaitStartQpc = 0;
    injectStartWaitTicks = 0;
    return true;
}

void MediaEncoderSession::PrewarmInjectEncoder(const QueuedFrame& warmupFrame) {
    if (injectEncoderPrewarmAttempted || useScreenGrab || !warmupFrame.isInjectMode || warmupFrame.width == 0 ||
        warmupFrame.height == 0) {
        return;
    }
    injectEncoderPrewarmAttempted = true;
    if (!MediaEngine_PrepareInjectVideo) {
        LogWarn("[EncoderThread] Inject transactional video prewarm unavailable; the first encode opens the output");
        return;
    }
    // Output, header and conversion shaders cost the first encode ~120 ms at 4K (20261019_173119), and the
    // live timeline starts where that encode ends. Paying it during the hidden warm-up keeps frame zero's
    // neighbour one tick away instead of a whole encode.
    LARGE_INTEGER startQpc;
    LARGE_INTEGER endQpc;
    QueryPerformanceCounter(&startQpc);
    const bool prepared =
        MediaEngine_PrepareInjectVideo(MediaState().engine, warmupFrame.luidLow, warmupFrame.luidHigh,
                                       warmupFrame.width, warmupFrame.height, warmupFrame.format, warmupFrame.isHDR);
    QueryPerformanceCounter(&endQpc);
    char line[320];
    snprintf(line, sizeof(line),
             "[EncoderThread] Inject transactional video prewarm %s: elapsed=%lldus dimensions=%ux%u format=%u "
             "hdr=%d buffered=%zu queued=%zu; timeline remains uncommitted%s",
             prepared ? "complete" : "FAILED", static_cast<long long>(qpcToUs(endQpc.QuadPart - startQpc.QuadPart)),
             warmupFrame.width, warmupFrame.height, warmupFrame.format, warmupFrame.isHDR ? 1 : 0,
             bufferedInjectFrames.size(), MediaState().frameQueue.Size(),
             prepared ? "" : " (the first encode retries and reports through the normal path)");
    if (prepared) {
        LogInfo("%s", line);
    } else {
        LogWarn("%s", line);
    }
}
