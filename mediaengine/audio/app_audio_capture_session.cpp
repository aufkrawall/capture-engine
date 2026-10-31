#include "app_audio_capture_internal.h"
#include "process_loopback_session_policy.h"

bool AppAudioCapture::HandleSessionActivity(bool activationQualified) {
    std::array<ce::process_loopback::AudioSessionCreation, 64> creations{};
    const size_t count = audioSessionMonitor_.TakeSessionCreations(creations.data(), creations.size());
    if (count == 0)
        return false;
    auto processes = SnapshotProcessTree();
    if (!targetProcessName.empty() && !processExclusions_.empty())
        PopulateProcessTreePaths(processes, targetProcessName, processExclusions_);
    const ce::process_loopback::CaptureSessionBinding binding{
        targetPID.load(std::memory_order_acquire), activationAudioSessionGeneration_.load(std::memory_order_acquire),
        activationQualified, activationHadObservedTargetSession_.load(std::memory_order_acquire)};
    const auto decision = ce::process_loopback::ResolveSessionActivity(
        processes, std::span(creations.data(), count), targetProcessName, processExclusions_, binding);
    if (decision.action == ce::process_loopback::SessionActivityAction::Continue)
        return false;

    if (decision.action == ce::process_loopback::SessionActivityAction::ReselectProcess) {
        DLL_Log("[AppAudioCapture] Active process tree no longer owns its audio: targetPID=%lu nextRootPID=%lu "
                "excluded=%zu treeOverlap=%zu unresolvedPaths=%zu; recycling before draining new session audio",
                static_cast<unsigned long>(binding.processId),
                static_cast<unsigned long>(decision.selection.selectedProcessId), decision.selection.excludedProcessCount,
                decision.selection.excludedTreeOverlapCount, decision.selection.unresolvedPathProcessCount);
    } else {
        DLL_Log("[AppAudioCapture] Target render session requires a fresh process-loopback binding: targetPID=%lu "
                "sessionPID=%lu activationGeneration=%llu activationQualified=%d targetSessionObservedAtActivation=%d "
                "epoch=%llu; recycling the disposable worker", static_cast<unsigned long>(binding.processId),
                static_cast<unsigned long>(decision.sessionProcessId),
                static_cast<unsigned long long>(binding.generation), binding.qualified ? 1 : 0,
                binding.observedTargetSession ? 1 : 0,
                static_cast<unsigned long long>(captureEpoch.load(std::memory_order_relaxed)));
    }
    workerRecycleRequested.store(true, std::memory_order_release);
    if (packetReadyEvent_)
        SetEvent(packetReadyEvent_);
    return true;
}
