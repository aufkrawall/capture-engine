// Output attribution exports, included by flow_hook_entry.cpp.

namespace {

// The game frame of each frame generation runtime output against the frame CE attributed it to: one constant
// offset per presenter lifetime; allocation addresses alone do not identify a lifetime.
struct OutputFrameCheck {
    std::mutex mutex;
    ce::flow::RuntimeOutputFrameTracker tracker;
    ce::log_meter::ChangeGate mismatchLog;
    uint64_t checks = 0;
    uint64_t mismatches = 0;
    uint64_t ownerViolations = 0;
} g_OutputFrames;

// The output a runtime announced on this thread (CEFlow_NoteRuntimeOutputFrame), judged once its Present returned.
struct PendingRuntimeOutput {
    const void* presenter = nullptr;
    uint64_t lifetime = 0;
    uint64_t frame = 0;
    bool composingFrameOwnedByTopmost = false;
    bool valid = false;
};
thread_local PendingRuntimeOutput t_PendingRuntimeOutput;

}  // namespace

// Before the runtime presents the output: the game frame it shows, and whether AMD composes a frame only the final-batch
// route draws. CE's account of the output is complete only when the Present returns (a nested presenter's batch inside
// it carries CE's draw), so the judgment waits for CEFlow_NoteRuntimeOutputPresented.
extern "C" __declspec(dllexport) void CEFlow_NoteRuntimeOutputFrame(const void* presenter, uint64_t lifetime,
                                                                       uint64_t frame) {
    DX12FFXOutputAttribution stale;
    DX12_TakeFFXOutputAttributionForValidation(&stale);
    t_PendingRuntimeOutput = {presenter, lifetime, frame, DX12_IsFFXComposingFrameOwnedByTopmost(), true};
}

extern "C" __declspec(dllexport) uint64_t CEFlow_PendingRuntimeOutputFrame() {
    return t_PendingRuntimeOutput.valid ? t_PendingRuntimeOutput.frame : 0;
}

extern "C" __declspec(dllexport) void CEFlow_NoteRuntimeOutputPresented() {
    const PendingRuntimeOutput pending = t_PendingRuntimeOutput;
    t_PendingRuntimeOutput.valid = false;
    if (!pending.valid)
        return;
    const void* const presenter = pending.presenter;
    const uint64_t lifetime = pending.lifetime;
    const uint64_t frame = pending.frame;
    DX12FFXOutputAttribution output;
    if (!DX12_TakeFFXOutputAttributionForValidation(&output)) {
        // Composed without passing CE's no-callback route at all - while AMD composes a frame only that route
        // draws (its UI baseline is retired), so the output shows no overlay.
        if (pending.composingFrameOwnedByTopmost) {
            std::lock_guard<std::mutex> lock(g_OutputFrames.mutex);
            ++g_OutputFrames.checks;
            ++g_OutputFrames.ownerViolations;
            HookLogImportant("CEFlow: runtime output of game frame %llu composed outside the final-batch route while "
                             "its frame is topmost-owned",
                             static_cast<unsigned long long>(frame));
        }
        return;
    }
    std::lock_guard<std::mutex> lock(g_OutputFrames.mutex);
    const int64_t offset = static_cast<int64_t>(output.frame) - static_cast<int64_t>(frame);
    ++g_OutputFrames.checks;
    if (output.record.owner != ce::dx12_overlay_policy::FFXFrameOverlayOwner::kUnknown) {
        const auto verdict = ce::dx12_overlay_policy::JudgeFrameExactFFXOutput(output.record.owner, output.topmostDrawn);
        if (!verdict.covered || verdict.doubleDrawn) {
            ++g_OutputFrames.ownerViolations;
            HookLogImportant("CEFlow: runtime output of game frame %llu (CE frame %llu, owner=%s) %s",
                             static_cast<unsigned long long>(frame), static_cast<unsigned long long>(output.frame),
                             ce::dx12_overlay_policy::FFXFrameOverlayOwnerName(output.record.owner),
                             verdict.doubleDrawn ? "drawn by the final-batch route over its UI baseline"
                                                 : "without its owner's draw");
        }
    }
    const auto observation = g_OutputFrames.tracker.Observe(presenter, lifetime, offset);
    const auto logVerdict = g_OutputFrames.mismatchLog.Observe(
        ce::log_meter::FieldKey(lifetime, offset, g_OutputFrames.tracker.Offset()));
    if (observation == ce::flow::RuntimeOutputFrameTracker::Observation::kFirstInLifetime) {
        HookLogImportant("CEFlow: runtime presenter lifetime=%llu presenter=%p initialOffset=%lld",
                         static_cast<unsigned long long>(lifetime), presenter, static_cast<long long>(offset));
    } else if (observation == ce::flow::RuntimeOutputFrameTracker::Observation::kMismatch) {
        ++g_OutputFrames.mismatches;
        if (logVerdict.log) {
            HookLogImportant("CEFlow: runtime output of game frame %llu attributed to CE frame %llu (offset %lld, "
                             "earlier outputs %lld; owner=%s topmostDrawn=%d lifetime=%llu mismatches=%llu)%s",
                             static_cast<unsigned long long>(frame), static_cast<unsigned long long>(output.frame),
                             static_cast<long long>(offset), static_cast<long long>(g_OutputFrames.tracker.Offset()),
                             ce::dx12_overlay_policy::FFXFrameOverlayOwnerName(output.record.owner),
                             output.topmostDrawn ? 1 : 0, static_cast<unsigned long long>(lifetime),
                             static_cast<unsigned long long>(g_OutputFrames.mismatches),
                             ce::log_meter::SuppressedNote(logVerdict.suppressed).c_str());
        }
    }
}

extern "C" __declspec(dllexport) void CEFlow_GetOverlayCoverage(CEFlowOverlayCoverage* out) {
    const DX12OverlayCoverageSnapshot snapshot = GetOverlayCoverageSnapshot();
    out->presents = snapshot.totalPresents;
    out->uncovered = snapshot.uncoveredPresents;
    out->currentUncoveredStreak = snapshot.currentStreak;
    out->longestUncoveredStreak = snapshot.longestStreak;
    out->doubleDraws = snapshot.doubleDraws;
    out->draws = dx12_hook_g_OverlayCoverageDrawCount.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lock(g_OutputFrames.mutex);
    out->outputFrameChecks = g_OutputFrames.checks;
    out->outputFrameMismatches = g_OutputFrames.mismatches;
    out->outputOwnerViolations = g_OutputFrames.ownerViolations;
}
