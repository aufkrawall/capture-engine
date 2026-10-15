#include "inject_source_read.h"

#include <atomic>

#include "captureengine/app/mediaengine_loader.h"
#include "common/logging/logging.h"

InjectSourceReadReleaseQueue media_main_g_InjectSourceReadReleases;

namespace {

int64_t NowQpc() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

uint32_t QpcToUs(int64_t ticks, int64_t frequency) {
    if (ticks <= 0 || frequency <= 0)
        return 0;
    const int64_t us = ticks * 1000000 / frequency;
    return us > static_cast<int64_t>(UINT32_MAX) ? UINT32_MAX : static_cast<uint32_t>(us);
}

}  // namespace

void DeferInjectLeaseUntilSourceRead(QueuedFrame& frame, const ce::media::FrameSubmissionResultV1& submission,
                                     const char* site) {
    if (!frame.isInjectMode || !MediaEngine_TakeInjectSourceReadToken)
        return;
    uint64_t value = 0;
    // Always take the token so a later submission never inherits it.
    if (!MediaEngine_TakeInjectSourceReadToken(&value))
        return;
    InjectSourceReadToken token(value, MediaEngine_IsInjectSourceReadComplete,
                                MediaEngine_SetInjectSourceReadWakeEvent);
    if (!frame.injectRingLease)
        return;  // already released (e.g. the cached last frame)

    if (submission.source == ce::media::SourceDisposition::RetainForRetry) {
        // Both owners must finish: a retry may be cancelled or dropped before
        // this read completes, or this read may finish before the next retry.
        media_main_g_InjectSourceReadReleases.Defer(frame.injectRingLease.Share(), std::move(token), NowQpc());
        static std::atomic<uint32_t> s_retainedWithRead{0};
        const uint32_t count = s_retainedWithRead.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 4) {
            LogInfo("[Inject SourceRead] %s retained frame=%u ring=%u tex=%d with a pending read value=%llu "
                    "(lease stays on the queued frame, count=%u)",
                    site ? site : "?", frame.frameIndex, frame.ringIndex, frame.textureIndex,
                    static_cast<unsigned long long>(value), count);
        }
        return;
    }

    static std::atomic<bool> s_firstDeferLogged{false};
    if (!s_firstDeferLogged.exchange(true, std::memory_order_relaxed)) {
        LogInfo("[Inject SourceRead] Ring lease release now follows the encoder's GPU read completion "
                "(site=%s frame=%u ring=%u tex=%d value=%llu)",
                site ? site : "?", frame.frameIndex, frame.ringIndex, frame.textureIndex,
                static_cast<unsigned long long>(token.Value()));
    }
    media_main_g_InjectSourceReadReleases.Defer(std::move(frame.injectRingLease), std::move(token), NowQpc());
}

void ReleaseCompletedInjectSourceReads() {
    if (media_main_g_InjectSourceReadReleases.Pending() == 0)
        return;
    media_main_g_InjectSourceReadReleases.ReleaseCompleted(NowQpc());
}

void LogInjectSourceReadWindow(int64_t qpcFrequency) {
    const ce::SourceReadReleaseWindow window = media_main_g_InjectSourceReadReleases.TakeWindow();
    if (window.deferred == 0 && window.immediate == 0 && window.released == 0 && window.armFailures == 0)
        return;
    LogInfo("[Inject SourceRead] deferred=%u immediate=%u released=%u pending=%zu pendingMax=%u holdMaxUs=%u "
            "armFail=%u",
            window.deferred, window.immediate, window.released, media_main_g_InjectSourceReadReleases.Pending(),
            window.maxPending, QpcToUs(window.maxHoldQpc, qpcFrequency), window.armFailures);
}

void LogInjectSourceReadSummary(int64_t qpcFrequency) {
    const ce::SourceReadReleaseWindow total = media_main_g_InjectSourceReadReleases.Totals();
    if (total.deferred == 0 && total.immediate == 0)
        return;
    LogInfo("[Inject SourceRead SUMMARY] deferred=%u immediate=%u released=%u pending=%zu pendingMax=%u "
            "holdMaxUs=%u armFail=%u",
            total.deferred, total.immediate, total.released, media_main_g_InjectSourceReadReleases.Pending(),
            total.maxPending, QpcToUs(total.maxHoldQpc, qpcFrequency), total.armFailures);
}
