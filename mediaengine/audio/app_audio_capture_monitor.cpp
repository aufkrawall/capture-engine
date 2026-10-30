#include "app_audio_capture_internal.h"
#include "common/platform/thread_label.h"


void AppAudioCapture::ProcessMonitorLoop() {
    ce::platform::NameCurrentThread("CE app audio monitor");
    DLL_Log("[AppAudioCapture] Monitor loop started for '%s'", targetProcessName.c_str());

    while (isMonitoring.load() && !shouldStop.load()) {
        FinalizePendingAsyncStart();

        if (asyncStartInProgress.load(std::memory_order_acquire)) {
            for (int i = 0; i < 5 && isMonitoring.load() && !shouldStop.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        // Check if we're already capturing
        if (!isCapturing.load()) {
            // CaptureLoop owns the three-consecutive-miss process-exit policy.
            // Reap and clean its completed session before activating a replacement;
            // a second one-shot monitor probe used to bypass that tolerance and
            // permanently stop healthy capture on a transient OpenProcess failure.
            if (captureThread.joinable()) {
                captureThread.join();
                CleanupCapture();
                targetPID.store(0, std::memory_order_release);
            }

            // Not capturing - try to find the target process
            const auto selection = FindProcessByName(targetProcessName, processExclusions_);
            const DWORD pid = selection.selectedProcessId;
            if (pid != 0) {
                selectionDeferredLog_.Reset();
                DLL_Log("[AppAudioCapture] Selected process-tree root '%s' with PID %lu", targetProcessName.c_str(),
                        pid);
                targetPID.store(pid);
                if (!BeginAsyncStartForPID(pid)) {
                    targetPID.store(0, std::memory_order_release);
                }
            } else if (selection.excludedProcessCount != 0 || selection.excludedTreeOverlapCount != 0 ||
                       selection.unresolvedPathProcessCount != 0) {
                const uint64_t key = ce::log_meter::FieldKey(
                    selection.excludedProcessCount, selection.excludedTreeOverlapCount,
                    selection.unresolvedPathProcessCount, selection.firstUnresolvedPathProcessId);
                if (const auto verdict = selectionDeferredLog_.Observe(key, GetTickCount64())) {
                    const ce::log_meter::SuppressedNote note(verdict.suppressed);
                    DLL_Log(
                        "[AppAudioCapture] Deferred process-name selection '%s': excluded=%zu treeOverlap=%zu "
                        "unresolvedPaths=%zu firstUnresolvedPID=%lu%s",
                        targetProcessName.c_str(), selection.excludedProcessCount,
                        selection.excludedTreeOverlapCount, selection.unresolvedPathProcessCount,
                        static_cast<unsigned long>(selection.firstUnresolvedPathProcessId), note.c_str());
                }
            }
        }

        // Check every second, but use small intervals for responsive shutdown
        for (int i = 0; i < 10 && isMonitoring.load() && !shouldStop.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    DLL_Log("[AppAudioCapture] Monitor loop exited");
}
