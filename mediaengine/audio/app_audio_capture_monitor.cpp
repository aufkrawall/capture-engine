#include "app_audio_capture_internal.h"


void AppAudioCapture::ProcessMonitorLoop() {
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
            const auto selection = FindProcessByName(targetProcessName);
            const DWORD pid = selection.selectedProcessId;
            if (pid != 0) {
                DLL_Log("[AppAudioCapture] Selected process-tree root '%s' with PID %lu", targetProcessName.c_str(),
                        pid);
                targetPID.store(pid);
                if (!BeginAsyncStartForPID(pid)) {
                    targetPID.store(0, std::memory_order_release);
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
