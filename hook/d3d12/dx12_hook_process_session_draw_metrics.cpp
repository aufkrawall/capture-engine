#include "dx12_hook_internal.h"
#include "hook/runtime/hook_clock.h"
#include "dx12_hook_process_session.h"

void FrameProcessSession::CompleteDrawSubmissionMetrics() {
    ce::hook_clock::QueryCounter(&perfEnd);
    if (diagnostics && perfFreq.QuadPart > 0) {
        const auto toUs = [&](LONGLONG ticks) {
            return (ticks * 1000000) / perfFreq.QuadPart;
        };
        diagnostics->overlayAcquireUs =
            toUs(perfGetBuf.QuadPart - perfQI.QuadPart);
        diagnostics->overlayRecordUs =
            toUs(perfRecord.QuadPart - perfGetBuf.QuadPart);
        diagnostics->overlaySubmitUs =
            toUs(perfSubmit.QuadPart - perfRecord.QuadPart);
        diagnostics->overlayPostSubmitUs =
            toUs(perfEnd.QuadPart - perfSubmit.QuadPart);
        diagnostics->overlayBreakdownValid = true;
    }
    // Periodic perf dump every 300 frames
    static int s_perfDumpCounter = 0;
    if (++s_perfDumpCounter % 300 == 0) {
        double toUs = 1000000.0 / (double)perfFreq.QuadPart;
        double qiUs = (double)(perfGetBuf.QuadPart - perfQI.QuadPart) * toUs;
        double getBufUs = (double)(perfRecord.QuadPart - perfGetBuf.QuadPart) * toUs;
        double submitUs = (double)(perfSubmit.QuadPart - perfRecord.QuadPart) * toUs;
        double totalUs = (double)(perfEnd.QuadPart - perfQI.QuadPart) * toUs;
        HookLogImportant(
            "DX12: Overlay perf: QI+idx=%.0fus getBuf+record=%.0fus submit=%.0fus "
            "total=%.0fus",
            qiUs, getBufUs, submitUs, totalUs);
    }

    if (cmdRecordOk) {
        static int s_firstOverlaySubmitLogged = 0;
        if (s_firstOverlaySubmitLogged == 0) {
            s_firstOverlaySubmitLogged = 1;
            HookLogImportant(
                "DX12: ProcessFrame - first overlay render command list submitted "
                "successfully");
        }

        if (!dx12_hook_s_startupOverlayCompatSettled.exchange(true, std::memory_order_acq_rel)) {
            if (shouldRunStartupOverlayDrawProbe &&
                dx12_hook_s_startupOverlayFirstDrawProbeStage ==
                    StartupOverlayFirstDrawProbeStage::kActualRender) {
                HookLogImportant(
                    "DX12: Startup overlay compat settled - future sync reinit will "
                    "keep the full allocator pool");
            } else {
                HookLogImportant(
                    "DX12: Stable DX12 overlay rendering observed - later startup "
                    "overlay popups will stay on the normal coexistence path");
            }
        }

        // Clear probe state if we were in a probe sequence
        if (shouldRunStartupOverlayDrawProbe &&
            dx12_hook_s_startupOverlayFirstDrawProbeStage ==
                StartupOverlayFirstDrawProbeStage::kActualRender) {
            HookLogImportant("DX12: Startup overlay probe complete - rendering stably");
            dx12_hook_s_startupOverlayFirstDrawProbeStage =
                StartupOverlayFirstDrawProbeStage::kComplete;
            dx12_hook_s_startupOverlayFirstDrawProbeMs = 0;
        }
    }
}
