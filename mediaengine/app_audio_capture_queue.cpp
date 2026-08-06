#include "app_audio_capture_internal.h"


bool AppAudioCapture::GetNextPacket(AudioPacket& packet) {
    FinalizePendingAsyncStart();
    if (!isCapturing.load(std::memory_order_acquire) && !isMonitoring.load(std::memory_order_acquire) &&
        targetPID.load(std::memory_order_acquire) != 0 && !asyncStartInProgress.load(std::memory_order_acquire)) {
        targetPID.store(0, std::memory_order_release);
    }
    AudioPacket queuedPacket;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (packetQueue.empty())
            return false;
        queuedPacket = std::move(packetQueue.front());
        packetQueue.pop_front();
        if (!packetQueue.empty() && packetReadyEvent_) {
            SetEvent(packetReadyEvent_);
        }
    }
    packet = std::move(queuedPacket);
    return true;
}

void AppAudioCapture::DiscardPendingPackets() {
    std::deque<AudioPacket> discarded;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        discarded.swap(packetQueue);
        if (packetReadyEvent_) {
            ResetEvent(packetReadyEvent_);
        }
    }
    if (!discarded.empty()) {
        DLL_Log("[AppAudioCapture] Discarding %zu queued packets for PID %lu", discarded.size(), targetPID.load());
    }
}

size_t AppAudioCapture::PendingPacketCount() {
    std::lock_guard<std::mutex> lock(queueMutex);
    return packetQueue.size();
}
