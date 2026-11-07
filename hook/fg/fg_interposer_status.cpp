// NVIDIA Smooth Motion's status and rates, decided from a present interposer's streams
// (hook/present/present_interposer_evidence.h has the evidence and why it is what it is).
#include "fg_detection.h"

#include "common/logging/log_meter.h"
#include "hook/runtime/hook_common.h"

namespace pi = ce::present_interposer;

pi::OutputPresentClass FGCompatibility::NoteInterposerOutputPresent(uint32_t threadId, int64_t nowUs) {
    if (!IsNvPresentLoaded()) {
        return {};
    }
    pi::OutputPresentClass present;
    pi::WindowReport report;
    bool windowClosed = false;
    {
        std::lock_guard<std::mutex> lock(interposerEvidenceLock);
        windowClosed = interposerEvidence.NoteOutput(threadId, nowUs, &present, &report);
    }
    if (windowClosed) {
        NotePresentInterposerCadence(report);
    }

    static ce::log_meter::ChangeGate s_dedicatedPresenter;
    if (const auto verdict = s_dedicatedPresenter.Observe(ce::log_meter::FieldKey(present.measured))) {
        if (!present.measured) {
            HookLogImportant("FG: The present interposer's output stream is on one thread (tid=0x%04X) - its runs say "
                             "nothing; submissionEvidence=%d%s",
                             threadId, IsApplicationSubmissionCountingEnabled() ? 1 : 0,
                             ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }
    if (present.measured) {
        applicationSubmissions.store(0, std::memory_order_relaxed);
    } else if (IsApplicationSubmissionCountingEnabled()) {
        const uint64_t submissions = applicationSubmissions.exchange(0, std::memory_order_acq_rel);
        if (submissions > 0) {
            present.measured = true;
            present.applicationFrame = ce::fg_runtime::IsApplicationSourcedInterposerPresent(submissions);
            bool hasDirectAppPresents = false;
            {
                std::lock_guard<std::mutex> lock(interposerEvidenceLock);
                hasDirectAppPresents = interposerEvidence.HasApplicationStream();
            }
            if (present.applicationFrame && !hasDirectAppPresents) {
                NoteInterposerApplicationPresent(nowUs);
            }
        }
    }
    return present;
}

void FGCompatibility::NoteInterposerApplicationPresent(int64_t nowUs) {
    pi::WindowReport report;
    bool windowClosed = false;
    {
        std::lock_guard<std::mutex> lock(interposerEvidenceLock);
        windowClosed = interposerEvidence.NoteApplication(nowUs, &report);
    }
    if (windowClosed) {
        NotePresentInterposerCadence(report);
    }
}

void FGCompatibility::ResetInterposerEvidence() {
    std::lock_guard<std::mutex> lock(interposerEvidenceLock);
    interposerEvidence.Reset();
    interposerLatch.Reset();
    interposerCadenceDecided.store(false, std::memory_order_release);
    nvidiaSMMeasuredOutputFps.store(0.0f, std::memory_order_release);
    nvidiaSMMeasuredApplicationFps.store(0.0f, std::memory_order_release);
    nvidiaSMMeasuredMultiplier.store(0, std::memory_order_release);
}

void FGCompatibility::RecordPresentForNvidiaSmoothMotion(pi::OutputPresentClass source) {
    if (!IsNvPresentLoaded()) {
        return;
    }
    // Unmeasured keeps the neutral sample: every present counts, and only the paired-gap heuristics can say more.
    explicitSourceClassification.store(source.measured, std::memory_order_release);
    RecordFrame(!source.measured || source.applicationFrame ? 1 : 0);
}

void FGCompatibility::NotePresentInterposerCadence(const pi::WindowReport& report) {
    const pi::CadenceVerdict& verdict = report.verdict;
    static std::atomic<uint32_t> s_windows{0};
    const uint32_t window = s_windows.fetch_add(1, std::memory_order_relaxed) + 1;
    static ce::log_meter::ChangeGate s_verdicts;
    const auto changedVerdict = s_verdicts.Observe(ce::log_meter::FieldKey(
        verdict.decided, verdict.generating, verdict.multiplier, static_cast<int>(report.stream), report.modalRun));
    if (changedVerdict || ce::log_meter::ShouldLogCadence(window, 5, 60)) {
        HookLogImportant("FG: Present interposer output cadence window #%u - application=%.1f fps output=%.1f fps "
                         "generating=%d multiplier=%d decided=%d stream=%s runs=%d modalRun=%d",
                         window, verdict.applicationFps, verdict.outputFps, verdict.generating ? 1 : 0,
                         verdict.multiplier, verdict.decided ? 1 : 0, pi::EvidenceStreamName(report.stream),
                         report.runs, report.modalRun);
    }

    // Too few frames say nothing. This used to clear the status: every window of a game below 20 fps did.
    if (!verdict.decided) {
        return;
    }
    interposerCadenceDecided.store(true, std::memory_order_release);
    bool active = false;
    {
        std::lock_guard<std::mutex> lock(interposerEvidenceLock);
        active = interposerLatch.Observe(verdict);
    }
    if (active) {
        if (verdict.generating) {
            nvidiaSMMeasuredOutputFps.store(verdict.outputFps, std::memory_order_release);
            nvidiaSMMeasuredApplicationFps.store(verdict.applicationFps, std::memory_order_release);
            nvidiaSMMeasuredMultiplier.store(verdict.multiplier, std::memory_order_release);
        }
    } else {
        nvidiaSMMeasuredOutputFps.store(0.0f, std::memory_order_release);
        nvidiaSMMeasuredApplicationFps.store(0.0f, std::memory_order_release);
        nvidiaSMMeasuredMultiplier.store(0, std::memory_order_release);
    }

    // The cadence is the authority for the interposer, but never over a real in-process FG
    // runtime: DLSS-G and FFX own the swapchain themselves and their presents are not an
    // interposer's output chain.
    if (!ce::fg_runtime::CanEvaluateNvidiaSmoothMotionPattern(CaptureDetectionSnapshot())) {
        return;
    }

    const bool wasDetected = nvidiaSmoothMotionDetected.exchange(active, std::memory_order_acq_rel);
    if (wasDetected != active) {
        HookLogImportant("FG: NVIDIA Smooth Motion %s from the present interposer's output cadence "
                         "(multiplier=%d output=%.1f fps application=%.1f fps stream=%s modalRun=%d)",
                         active ? "ACTIVE" : "inactive", verdict.multiplier, verdict.outputFps, verdict.applicationFps,
                         pi::EvidenceStreamName(report.stream), report.modalRun);
    }
}
